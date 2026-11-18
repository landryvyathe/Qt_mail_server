#include "WebmailApi.h"
#include "../ConfigManager.h"
#include "../auth/AuthManager.h"
#include "../queue/QueueManager.h"
#include "../services/ImapMime.h"
#include "../storage/MaildirStorage.h"

#define CPPHTTPLIB_OPENSSL_SUPPORT
#include <httplib.h>

#include <QDateTime>
#include <QDebug>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLocale>
#include <QMutexLocker>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QUrl>
#include <algorithm>

namespace {

constexpr qint64 kSessionTtlSecs = 8 * 3600;
constexpr int    kMaxPageSize = 100;
constexpr int    kMaxRecipients = 100;
constexpr qint64 kMaxMessageBytes = 25LL * 1024 * 1024;
const QStringList kDefaultFolders = {"Sent", "Drafts", "Trash", "Junk"};

void sendJson(httplib::Response& res, int code, const QJsonValue& value) {
    res.status = code;
    const QByteArray body = value.isArray()
        ? QJsonDocument(value.toArray()).toJson(QJsonDocument::Compact)
        : QJsonDocument(value.toObject()).toJson(QJsonDocument::Compact);
    res.set_content(body.toStdString(), "application/json; charset=utf-8");
}

void sendError(httplib::Response& res, int code, const QString& message) {
    sendJson(res, code, QJsonObject{{"error", message}});
}

bool parseBody(const httplib::Request& req, httplib::Response& res, QJsonObject& out) {
    QJsonParseError err;
    const QJsonDocument doc = QJsonDocument::fromJson(QByteArray::fromStdString(req.body), &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject()) {
        sendError(res, 400, "Invalid JSON body");
        return false;
    }
    out = doc.object();
    return true;
}

QString queryParam(const httplib::Request& req, const char* name) {
    return req.has_param(name) ? QString::fromStdString(req.get_param_value(name)) : QString();
}

// "INBOX", a safe folder name, or empty when the name is invalid.
QString normalizeFolder(const QString& raw) {
    const QString name = raw.trimmed();
    if (name.compare("INBOX", Qt::CaseInsensitive) == 0) return "INBOX";
    static const QRegularExpression bad(R"([/\\*%"\x00-\x1f]|\.\.|^\.|\.$)");
    if (name.isEmpty() || name.size() > 200 || bad.match(name).hasMatch()) return {};
    return name;
}

QString flagLetters(const QString& fileName) {
    const int idx = fileName.indexOf(":2,");
    return idx >= 0 ? fileName.mid(idx + 3) : QString();
}

QString folderRole(const QString& name) {
    if (name == "INBOX") return "inbox";
    if (name == "Sent") return "sent";
    if (name == "Drafts") return "drafts";
    if (name == "Trash") return "trash";
    if (name == "Junk") return "junk";
    return {};
}

QList<int> uidList(const QJsonObject& body) {
    QList<int> uids;
    for (const QJsonValue& v : body.value("uids").toArray())
        if (v.toInt() > 0) uids << v.toInt();
    return uids;
}

QString headerText(const QByteArray& header, const char* name) {
    return QString::fromUtf8(ImapMime::headerValue(header, name));
}

// ---- MIME building --------------------------------------------------------

QString singleLine(const QString& s) {
    QString out = s;
    out.replace('\r', ' ').replace('\n', ' ');
    return out.trimmed();
}

QByteArray encodeWord(const QString& text) {
    const QString clean = singleLine(text);
    bool ascii = true;
    for (QChar c : clean) if (c.unicode() > 0x7e) { ascii = false; break; }
    if (ascii) return clean.toLatin1();
    return "=?UTF-8?B?" + clean.toUtf8().toBase64() + "?=";
}

QByteArray base64Lines(const QByteArray& data) {
    const QByteArray b64 = data.toBase64();
    QByteArray out;
    out.reserve(b64.size() + b64.size() / 76 * 2 + 2);
    for (int i = 0; i < b64.size(); i += 76) out += b64.mid(i, 76) + "\r\n";
    return out;
}

QByteArray randomHex(int bytes) {
    QByteArray raw(bytes, Qt::Uninitialized);
    QRandomGenerator::system()->fillRange(reinterpret_cast<quint32*>(raw.data()), bytes / 4);
    return raw.toHex();
}

bool isValidEmail(const QString& email) {
    static const QRegularExpression re(R"(^[^@\s<>",;:()\[\]\\]+@[A-Za-z0-9.-]+\.[A-Za-z]{2,}$)");
    return email.size() <= 254 && re.match(email).hasMatch();
}

struct Attachment {
    QString filename;
    QByteArray contentType;
    QByteArray data;
};

struct Outgoing {
    QString from;
    QStringList to, cc, bcc;
    QString subject, text, html, inReplyTo, references;
    QList<Attachment> attachments;
};

QByteArray textPart(const QByteArray& type, const QString& content) {
    return "Content-Type: " + type + "; charset=utf-8\r\nContent-Transfer-Encoding: base64\r\n\r\n"
         + base64Lines(content.toUtf8());
}

QByteArray buildMime(const Outgoing& m, const QString& hostname) {
    QByteArray head;
    head += "From: " + m.from.toUtf8() + "\r\n";
    if (!m.to.isEmpty()) head += "To: " + m.to.join(", ").toUtf8() + "\r\n";
    if (!m.cc.isEmpty()) head += "Cc: " + m.cc.join(", ").toUtf8() + "\r\n";
    head += "Subject: " + encodeWord(m.subject) + "\r\n";
    head += "Date: " + QLocale::c().toString(QDateTime::currentDateTimeUtc(),
                                             "ddd, dd MMM yyyy hh:mm:ss").toLatin1() + " +0000\r\n";
    head += "Message-ID: <" + randomHex(16) + "@" + hostname.toLatin1() + ">\r\n";
    if (!m.inReplyTo.isEmpty()) head += "In-Reply-To: " + singleLine(m.inReplyTo).toLatin1() + "\r\n";
    if (!m.references.isEmpty()) head += "References: " + singleLine(m.references).toLatin1() + "\r\n";
    head += "MIME-Version: 1.0\r\nX-Mailer: Bykiva Webmail\r\n";

    QByteArray body;
    if (m.html.isEmpty()) {
        body = textPart("text/plain", m.text);
    } else {
        const QByteArray alt = "alt-" + randomHex(8);
        body = "Content-Type: multipart/alternative; boundary=\"" + alt + "\"\r\n\r\n"
             + "--" + alt + "\r\n" + textPart("text/plain", m.text)
             + "--" + alt + "\r\n" + textPart("text/html", m.html)
             + "--" + alt + "--\r\n";
    }
    if (m.attachments.isEmpty()) return head + body;

    const QByteArray mixed = "mix-" + randomHex(8);
    QByteArray out = head + "Content-Type: multipart/mixed; boundary=\"" + mixed + "\"\r\n\r\n"
                   + "--" + mixed + "\r\n" + body;
    for (const Attachment& a : m.attachments) {
        const QByteArray encodedName = QUrl::toPercentEncoding(a.filename);
        out += "--" + mixed + "\r\nContent-Type: " + a.contentType + "; name=\"" + encodeWord(a.filename) + "\"\r\n"
             + "Content-Disposition: attachment; filename*=UTF-8''" + encodedName + "\r\n"
             + "Content-Transfer-Encoding: base64\r\n\r\n" + base64Lines(a.data);
    }
    return out + "--" + mixed + "--\r\n";
}

} // namespace

WebmailApi::WebmailApi(const ConfigManager& config, const WebmailServices& services)
    : m_config(config), m_services(services),
      m_localDomains(config.localDomains()), m_hostname(config.hostname()) {
    for (QString& d : m_localDomains) d = d.toLower();
}

// ---- sessions ---------------------------------------------------------------

QString WebmailApi::createSession(const QString& email) {
    const QString token = QString::fromLatin1(randomHex(32));
    QMutexLocker lock(&m_sessionMutex);
    const qint64 now = QDateTime::currentSecsSinceEpoch();
    for (auto it = m_sessions.begin(); it != m_sessions.end();)
        it = it->expires < now ? m_sessions.erase(it) : std::next(it);
    m_sessions.insert(token, {email.section('@', 0, 0).toLower(), email.toLower(), now + kSessionTtlSecs});
    return token;
}

static QString bearerToken(const httplib::Request& req) {
    const QString auth = QString::fromStdString(req.get_header_value("Authorization"));
    return auth.startsWith("Bearer ") ? auth.mid(7).trimmed() : QString();
}

bool WebmailApi::authorize(const httplib::Request& req, httplib::Response& res, Session& out) {
    const QString token = bearerToken(req);
    QMutexLocker lock(&m_sessionMutex);
    auto it = m_sessions.find(token);
    const qint64 now = QDateTime::currentSecsSinceEpoch();
    if (token.isEmpty() || it == m_sessions.end() || it->expires < now) {
        if (it != m_sessions.end()) m_sessions.erase(it);
        lock.unlock();
        sendError(res, 401, "Not signed in");
        return false;
    }
    it->expires = now + kSessionTtlSecs;
    out = *it;
    return true;
}

void WebmailApi::dropSession(const httplib::Request& req) {
    QMutexLocker lock(&m_sessionMutex);
    m_sessions.remove(bearerToken(req));
}

// ---- helpers ------------------------------------------------------------------

QString WebmailApi::folderKey(const Session& s, const QString& folder) const {
    return folder == "INBOX" ? s.user : s.user + "/." + folder;
}

bool WebmailApi::folderExists(const Session& s, const QString& folder) const {
    return folder == "INBOX" || (!folder.isEmpty() && m_services.storage->mailboxExists(s.user, folder));
}

bool WebmailApi::isLocalAddress(const QString& email) const {
    return m_localDomains.contains(email.section('@', 1).toLower());
}

bool WebmailApi::moveMessages(const Session& s, const QString& from, const QList<int>& uids, const QString& to) {
    MaildirStorage* st = m_services.storage;
    const QList<MailEntry> entries = st->listMessages(folderKey(s, from));
    for (int uid : uids) {
        auto it = std::find_if(entries.begin(), entries.end(), [uid](const MailEntry& e) { return e.uid == uid; });
        if (it == entries.end()) continue;
        const MailEntry full = st->loadMessage(folderKey(s, from), uid);
        const int newUid = st->storeMessage(folderKey(s, to), full.rawMessage);
        if (newUid < 0) return false;
        QString letters = flagLetters(it->fileName);
        letters.remove('T');
        st->setFlags(folderKey(s, to), newUid, letters);
        st->deleteMessage(folderKey(s, from), uid);
    }
    return true;
}

// ---- routes ---------------------------------------------------------------------

void WebmailApi::registerRoutes(httplib::Server& server) {
    server.Post("/api/mail/login", [this](const httplib::Request& req, httplib::Response& res) {
        QJsonObject body;
        if (!parseBody(req, res, body)) return;
        const QString email = body.value("email").toString().trimmed().toLower();
        const QString password = body.value("password").toString();
        if (!isValidEmail(email) || password.isEmpty() || !isLocalAddress(email)) {
            sendError(res, 400, "Enter your full email address and password");
            return;
        }
        // Behind the local nginx proxy every request comes from loopback; rate-limit the real client.
        QHostAddress client(QString::fromStdString(req.remote_addr));
        const QHostAddress forwarded(QString::fromStdString(req.get_header_value("X-Real-IP")));
        if (client.isLoopback() && !forwarded.isNull()) client = forwarded;
        if (!m_services.auth->authenticate(email, password, client)) {
            sendError(res, 401, "Wrong email or password");
            return;
        }
        const QString user = email.section('@', 0, 0);
        for (const QString& f : kDefaultFolders)
            if (!m_services.storage->mailboxExists(user, f)) m_services.storage->createMailbox(user, f);
        sendJson(res, 200, QJsonObject{{"token", createSession(email)}, {"email", email},
                                       {"expiresIn", kSessionTtlSecs}});
    });

    server.Post("/api/mail/logout", [this](const httplib::Request& req, httplib::Response& res) {
        dropSession(req);
        sendJson(res, 200, QJsonObject{{"status", "signed out"}});
    });

    auto authed = [this](auto handler) {
        return [this, handler](const httplib::Request& req, httplib::Response& res) {
            Session s;
            if (authorize(req, res, s)) handler(s, req, res);
        };
    };
    auto authedJson = [this](auto handler) {
        return [this, handler](const httplib::Request& req, httplib::Response& res) {
            Session s;
            QJsonObject body;
            if (authorize(req, res, s) && parseBody(req, res, body)) handler(s, body, res);
        };
    };

    server.Get("/api/mail/me", authed([](const Session& s, const httplib::Request&, httplib::Response& res) {
        sendJson(res, 200, QJsonObject{{"email", s.email}});
    }));
    server.Get("/api/mail/folders", authed([this](const Session& s, const httplib::Request&, httplib::Response& res) {
        handleFolders(s, res);
    }));
    server.Post("/api/mail/folders", authedJson([this](const Session& s, const QJsonObject& b, httplib::Response& res) {
        handleFolderAction(s, b, res);
    }));
    server.Get("/api/mail/messages", authed([this](const Session& s, const httplib::Request& req, httplib::Response& res) {
        handleList(s, req, res);
    }));
    server.Get("/api/mail/message", authed([this](const Session& s, const httplib::Request& req, httplib::Response& res) {
        handleMessage(s, req, res);
    }));
    server.Post("/api/mail/flags", authedJson([this](const Session& s, const QJsonObject& b, httplib::Response& res) {
        handleFlags(s, b, res);
    }));
    server.Post("/api/mail/move", authedJson([this](const Session& s, const QJsonObject& b, httplib::Response& res) {
        handleMove(s, b, res);
    }));
    server.Post("/api/mail/delete", authedJson([this](const Session& s, const QJsonObject& b, httplib::Response& res) {
        handleDelete(s, b, res);
    }));
    server.Post("/api/mail/send", authedJson([this](const Session& s, const QJsonObject& b, httplib::Response& res) {
        handleSend(s, b, res, false);
    }));
    server.Post("/api/mail/draft", authedJson([this](const Session& s, const QJsonObject& b, httplib::Response& res) {
        handleSend(s, b, res, true);
    }));
}

// ---- handlers -------------------------------------------------------------------

void WebmailApi::handleFolders(const Session& s, httplib::Response& res) {
    QStringList names = {"INBOX"};
    names += m_services.storage->listMailboxes(s.user);
    QJsonArray out;
    for (const QString& name : names) {
        const QList<MailEntry> msgs = m_services.storage->listMessages(folderKey(s, name));
        int unseen = 0;
        for (const MailEntry& e : msgs) if (!e.seen) ++unseen;
        out.append(QJsonObject{{"name", name}, {"role", folderRole(name)},
                               {"total", msgs.size()}, {"unseen", unseen}});
    }
    sendJson(res, 200, QJsonObject{{"folders", out},
                                   {"usedBytes", m_services.storage->usageBytes(s.user)},
                                   {"quotaBytes", m_config.quotaMaxBytes()}});
}

void WebmailApi::handleFolderAction(const Session& s, const QJsonObject& body, httplib::Response& res) {
    const QString action = body.value("action").toString();
    const QString name = normalizeFolder(body.value("name").toString());
    if (name.isEmpty()) { sendError(res, 400, "Invalid folder name"); return; }
    const bool protectedFolder = name == "INBOX" || kDefaultFolders.contains(name);
    MaildirStorage* st = m_services.storage;

    if (action == "create") {
        if (folderExists(s, name)) { sendError(res, 409, "Folder already exists"); return; }
        if (!st->createMailbox(s.user, name)) { sendError(res, 500, "Could not create folder"); return; }
    } else if (action == "rename") {
        const QString target = normalizeFolder(body.value("newName").toString());
        if (protectedFolder || target.isEmpty() || target == "INBOX") { sendError(res, 400, "Cannot rename this folder"); return; }
        if (!folderExists(s, name)) { sendError(res, 404, "No such folder"); return; }
        if (folderExists(s, target)) { sendError(res, 409, "A folder with that name exists"); return; }
        if (!st->renameMailbox(s.user, name, target)) { sendError(res, 500, "Rename failed"); return; }
    } else if (action == "delete") {
        if (protectedFolder) { sendError(res, 400, "This folder cannot be deleted"); return; }
        if (!st->deleteMailbox(s.user, name)) { sendError(res, 404, "No such folder"); return; }
    } else if (action == "empty") {
        if (!folderExists(s, name)) { sendError(res, 404, "No such folder"); return; }
        for (const MailEntry& e : st->listMessages(folderKey(s, name))) st->deleteMessage(folderKey(s, name), e.uid);
    } else {
        sendError(res, 400, "Unknown folder action");
        return;
    }
    handleFolders(s, res);
}

void WebmailApi::handleList(const Session& s, const httplib::Request& req, httplib::Response& res) {
    const QString folder = normalizeFolder(queryParam(req, "folder"));
    if (!folderExists(s, folder)) { sendError(res, 404, "No such folder"); return; }
    const int offset = std::max(0, queryParam(req, "offset").toInt());
    const int limit = std::clamp(queryParam(req, "limit").toInt(), 1, kMaxPageSize);
    const QString query = queryParam(req, "q").trimmed();

    QList<MailEntry> entries = m_services.storage->listMessages(folderKey(s, folder));
    std::sort(entries.begin(), entries.end(), [](const MailEntry& a, const MailEntry& b) { return a.uid > b.uid; });

    QJsonArray items;
    int matched = 0;
    for (const MailEntry& e : entries) {
        if (e.uid < 1) continue;
        const bool needHeader = !query.isEmpty() || (matched >= offset && items.size() < limit);
        if (!needHeader) { ++matched; continue; }
        const QByteArray header = m_services.storage->loadHeader(folderKey(s, folder), e.uid);
        if (!query.isEmpty()) {
            const QString hay = headerText(header, "From") + ' ' + headerText(header, "To") + ' '
                              + headerText(header, "Subject");
            if (!hay.contains(query, Qt::CaseInsensitive)) continue;
        }
        if (matched++ < offset || items.size() >= limit) continue;
        const QString letters = flagLetters(e.fileName);
        const QString ctype = headerText(header, "Content-Type").toLower();
        items.append(QJsonObject{
            {"uid", e.uid},
            {"from", headerText(header, "From")},
            {"to", headerText(header, "To")},
            {"subject", headerText(header, "Subject")},
            {"date", headerText(header, "Date")},
            {"received", e.date.toMSecsSinceEpoch()},
            {"size", e.size},
            {"seen", letters.contains('S')},
            {"flagged", letters.contains('F')},
            {"answered", letters.contains('R')},
            {"attachment", ctype.startsWith("multipart/mixed") || ctype.startsWith("multipart/related")},
        });
    }
    sendJson(res, 200, QJsonObject{{"folder", folder}, {"total", matched}, {"offset", offset}, {"messages", items}});
}

void WebmailApi::handleMessage(const Session& s, const httplib::Request& req, httplib::Response& res) {
    const QString folder = normalizeFolder(queryParam(req, "folder"));
    const int uid = queryParam(req, "uid").toInt();
    if (!folderExists(s, folder) || uid < 1) { sendError(res, 404, "No such message"); return; }

    const QString key = folderKey(s, folder);
    const MailEntry e = m_services.storage->loadMessage(key, uid);
    if (e.rawMessage.isNull()) { sendError(res, 404, "No such message"); return; }
    if (queryParam(req, "peek") != "1" && !flagLetters(e.fileName).contains('S'))
        m_services.storage->setFlags(key, uid, flagLetters(e.fileName) + 'S');

    res.status = 200;
    res.set_header("Cache-Control", "no-store");
    res.set_content(e.rawMessage.toStdString(), "message/rfc822");
}

void WebmailApi::handleFlags(const Session& s, const QJsonObject& body, httplib::Response& res) {
    const QString folder = normalizeFolder(body.value("folder").toString());
    if (!folderExists(s, folder)) { sendError(res, 404, "No such folder"); return; }
    static const QString allowed = "DFRS";
    QString add, remove;
    for (QChar c : body.value("add").toString()) if (allowed.contains(c)) add += c;
    for (QChar c : body.value("remove").toString()) if (allowed.contains(c)) remove += c;

    const QString key = folderKey(s, folder);
    const QList<int> uids = uidList(body);
    for (const MailEntry& e : m_services.storage->listMessages(key)) {
        if (!uids.contains(e.uid)) continue;
        QString letters = flagLetters(e.fileName) + add;
        for (QChar c : remove) letters.remove(c);
        m_services.storage->setFlags(key, e.uid, letters);
    }
    sendJson(res, 200, QJsonObject{{"status", "ok"}});
}

void WebmailApi::handleMove(const Session& s, const QJsonObject& body, httplib::Response& res) {
    const QString from = normalizeFolder(body.value("folder").toString());
    const QString to = normalizeFolder(body.value("to").toString());
    if (!folderExists(s, from) || !folderExists(s, to)) { sendError(res, 404, "No such folder"); return; }
    if (from == to) { sendJson(res, 200, QJsonObject{{"status", "ok"}}); return; }
    if (!moveMessages(s, from, uidList(body), to)) { sendError(res, 500, "Move failed"); return; }
    sendJson(res, 200, QJsonObject{{"status", "ok"}});
}

void WebmailApi::handleDelete(const Session& s, const QJsonObject& body, httplib::Response& res) {
    const QString folder = normalizeFolder(body.value("folder").toString());
    if (!folderExists(s, folder)) { sendError(res, 404, "No such folder"); return; }
    const QList<int> uids = uidList(body);
    if (folder == "Trash" || body.value("permanent").toBool()) {
        for (int uid : uids) m_services.storage->deleteMessage(folderKey(s, folder), uid);
    } else if (!moveMessages(s, folder, uids, "Trash")) {
        sendError(res, 500, "Delete failed");
        return;
    }
    sendJson(res, 200, QJsonObject{{"status", "ok"}});
}

void WebmailApi::handleSend(const Session& s, const QJsonObject& body, httplib::Response& res, bool draft) {
    Outgoing m;
    m.from = s.email;
    m.subject = body.value("subject").toString().left(998);
    m.text = body.value("text").toString();
    m.html = body.value("html").toString();
    m.inReplyTo = body.value("inReplyTo").toString().left(998);
    m.references = body.value("references").toString().left(4000);

    QStringList invalid;
    auto readList = [&](const char* field, QStringList& into) {
        for (const QJsonValue& v : body.value(field).toArray()) {
            const QString addr = v.toString().trimmed().toLower();
            if (addr.isEmpty()) continue;
            if (isValidEmail(addr)) into << addr; else invalid << addr;
        }
    };
    readList("to", m.to);
    readList("cc", m.cc);
    readList("bcc", m.bcc);
    if (!invalid.isEmpty()) { sendError(res, 400, "Invalid address: " + invalid.join(", ")); return; }
    const int count = m.to.size() + m.cc.size() + m.bcc.size();
    if (!draft && count == 0) { sendError(res, 400, "Add at least one recipient"); return; }
    if (count > kMaxRecipients) { sendError(res, 400, "Too many recipients"); return; }

    qint64 total = m.text.size() + m.html.size();
    for (const QJsonValue& v : body.value("attachments").toArray()) {
        const QJsonObject a = v.toObject();
        Attachment att;
        att.filename = singleLine(a.value("filename").toString()).left(200);
        att.contentType = singleLine(a.value("contentType").toString()).toLatin1();
        if (att.contentType.isEmpty() || !att.contentType.contains('/')) att.contentType = "application/octet-stream";
        att.data = QByteArray::fromBase64(a.value("content").toString().toLatin1());
        if (att.filename.isEmpty()) att.filename = "attachment";
        total += att.data.size();
        m.attachments << att;
    }
    if (total > kMaxMessageBytes) { sendError(res, 413, "Message is larger than 25 MB"); return; }

    MaildirStorage* st = m_services.storage;
    const QByteArray mime = buildMime(m, m_hostname);
    if (st->isOverQuota(s.user, mime.size())) { sendError(res, 507, "Your mailbox is full"); return; }

    const int oldDraft = body.value("draftUid").toInt();
    if (oldDraft > 0) st->deleteMessage(folderKey(s, "Drafts"), oldDraft);

    if (draft) {
        const int uid = st->storeMessage(folderKey(s, "Drafts"), mime);
        if (uid < 0) { sendError(res, 500, "Could not save draft"); return; }
        st->setFlags(folderKey(s, "Drafts"), uid, "DS");
        sendJson(res, 200, QJsonObject{{"status", "saved"}, {"uid", uid}});
        return;
    }

    QStringList remote, failed;
    int delivered = 0;
    for (const QString& rcpt : m.to + m.cc + m.bcc) {
        if (!isLocalAddress(rcpt)) { remote << rcpt; continue; }
        const QString box = rcpt.section('@', 0, 0);
        if (!m_services.auth->userExists(rcpt) || st->isOverQuota(box, mime.size())) { failed << rcpt; continue; }
        const QByteArray stored = "Received: from webmail by " + m_hostname.toLatin1() + " (Bykiva Webmail)\r\n" + mime;
        if (st->storeMessage(box, stored) > 0) ++delivered; else failed << rcpt;
    }
    if (!remote.isEmpty() && !m_services.queue->spoolMessage(mime, s.email, remote)) {
        sendError(res, 502, "Could not queue mail for delivery");
        return;
    }

    const int sentUid = st->storeMessage(folderKey(s, "Sent"), mime);
    if (sentUid > 0) st->setFlags(folderKey(s, "Sent"), sentUid, "S");

    const QJsonObject replyTo = body.value("replyTo").toObject();
    const QString replyFolder = normalizeFolder(replyTo.value("folder").toString());
    const int replyUid = replyTo.value("uid").toInt();
    if (replyUid > 0 && folderExists(s, replyFolder)) {
        for (const MailEntry& e : st->listMessages(folderKey(s, replyFolder)))
            if (e.uid == replyUid) st->setFlags(folderKey(s, replyFolder), replyUid, flagLetters(e.fileName) + 'R');
    }

    qInfo() << "Webmail: message from" << s.email << "delivered locally:" << delivered
            << "queued:" << remote.size() << "failed:" << failed.size();
    QJsonArray failedJson;
    for (const QString& f : failed) failedJson.append(f);
    sendJson(res, 200, QJsonObject{{"status", "sent"}, {"delivered", delivered},
                                   {"queued", remote.size()}, {"failed", failedJson}});
}
