// Gmail-style webmail features: settings, labels, snooze, conversations, contacts.
#include "WebmailApi.h"
#include "WebmailUtil.h"
#include "../storage/MaildirStorage.h"
#include "../storage/UserSettings.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QHash>
#include <QSet>
#include <algorithm>
#include <chrono>

using namespace WebmailUtil;

namespace {

constexpr int kMaxLabels = 100;
constexpr int kMaxThreadMessages = 100;
constexpr int kSnoozeCheckSecs = 60;

struct ItemRef { QString folder; int uid; };

QList<ItemRef> itemRefs(const QJsonObject& body) {
    QList<ItemRef> out;
    for (const QJsonValue& v : body.value("items").toArray()) {
        const QJsonObject o = v.toObject();
        const QString folder = normalizeFolder(o.value("folder").toString());
        const int uid = o.value("uid").toInt();
        if (!folder.isEmpty() && uid > 0) out.append({folder, uid});
    }
    return out;
}

QSet<QString> messageIds(const QString& text) {
    static const QRegularExpression re("<[^<>\\s]+>");
    QSet<QString> ids;
    for (auto it = re.globalMatch(text); it.hasNext();) ids.insert(it.next().captured(0));
    return ids;
}

bool validLabelName(const QString& name) {
    static const QRegularExpression bad(R"([\x00-\x1f"<>\\])");
    return !name.isEmpty() && name.size() <= 40 && !bad.match(name).hasMatch();
}

QJsonArray renameInArray(const QJsonArray& arr, const QString& from, const QString& to) {
    QJsonArray out;
    for (const QJsonValue& v : arr) {
        const QString n = v.toString() == from ? to : v.toString();
        if (!n.isEmpty() && !out.contains(n)) out.append(n);
    }
    return out;
}

} // namespace

// ---- settings ---------------------------------------------------------------------

void WebmailApi::handleSettingsGet(const Session& s, httplib::Response& res) {
    const QJsonObject st = UserSettings::load(m_services.storage->basePath(), s.user);
    sendJson(res, 200, QJsonObject{{"signature", st.value("signature").toString()},
                                   {"vacation", st.value("vacation").toObject()},
                                   {"labels", st.value("labels").toArray()}});
}

void WebmailApi::handleSettingsSave(const Session& s, const QJsonObject& body, httplib::Response& res) {
    UserSettings::update(m_services.storage->basePath(), s.user, [&](QJsonObject& st) {
        if (body.contains("signature")) st["signature"] = body.value("signature").toString().left(20000);
        if (body.contains("vacation")) {
            const QJsonObject v = body.value("vacation").toObject();
            st["vacation"] = QJsonObject{
                {"enabled", v.value("enabled").toBool()},
                {"subject", v.value("subject").toString().left(200)},
                {"body", v.value("body").toString().left(10000)},
                {"start", v.value("start").toInteger()},
                {"end", v.value("end").toInteger()},
            };
            if (!v.value("enabled").toBool()) st.remove("vacationReplied");
        }
    });
    handleSettingsGet(s, res);
}

// ---- labels -----------------------------------------------------------------------

void WebmailApi::handleLabels(const Session& s, const QJsonObject& body, httplib::Response& res) {
    const QString action = body.value("action").toString();
    const QString name = body.value("name").toString().trimmed();
    QString error;

    // Resolve Message-IDs before taking the settings lock (header reads hit the disk).
    QStringList ids;
    if (action == "apply") {
        for (const ItemRef& r : itemRefs(body)) {
            if (!folderExists(s, r.folder)) continue;
            const QString id = headerText(m_services.storage->loadHeader(folderKey(s, r.folder), r.uid), "Message-ID").trimmed();
            if (!id.isEmpty()) ids << id;
        }
    }

    UserSettings::update(m_services.storage->basePath(), s.user, [&](QJsonObject& st) {
        QJsonArray labels = st.value("labels").toArray();
        QJsonObject map = st.value("messageLabels").toObject();
        auto indexOf = [&labels](const QString& n) {
            for (int i = 0; i < labels.size(); ++i)
                if (labels[i].toObject().value("name").toString().compare(n, Qt::CaseInsensitive) == 0) return i;
            return -1;
        };
        static const QRegularExpression colorRe("^#[0-9a-fA-F]{6}$");
        const QString color = colorRe.match(body.value("color").toString()).hasMatch()
            ? body.value("color").toString() : QString("#5f6672");

        if (action == "create") {
            if (!validLabelName(name)) { error = "Invalid label name"; return; }
            if (indexOf(name) >= 0) { error = "Label already exists"; return; }
            if (labels.size() >= kMaxLabels) { error = "Too many labels"; return; }
            labels.append(QJsonObject{{"name", name}, {"color", color}});
        } else if (action == "update") {
            const int i = indexOf(name);
            const QString newName = body.value("newName").toString().trimmed();
            if (i < 0) { error = "No such label"; return; }
            if (!newName.isEmpty() && newName != name) {
                if (!validLabelName(newName) || indexOf(newName) >= 0) { error = "Invalid new name"; return; }
                for (auto it = map.begin(); it != map.end(); ++it) it.value() = renameInArray(it.value().toArray(), name, newName);
            }
            labels[i] = QJsonObject{{"name", newName.isEmpty() ? name : newName}, {"color", color}};
        } else if (action == "delete") {
            const int i = indexOf(name);
            if (i < 0) { error = "No such label"; return; }
            labels.removeAt(i);
            for (auto it = map.begin(); it != map.end();) {
                const QJsonArray arr = renameInArray(it.value().toArray(), name, QString());
                if (arr.isEmpty()) it = map.erase(it); else { it.value() = arr; ++it; }
            }
        } else if (action == "apply") {
            QStringList add, remove;
            for (const QJsonValue& v : body.value("add").toArray()) if (indexOf(v.toString()) >= 0) add << v.toString();
            for (const QJsonValue& v : body.value("remove").toArray()) remove << v.toString();
            for (const QString& id : ids) {
                QJsonArray arr = map.value(id).toArray();
                for (const QString& r : remove) arr = renameInArray(arr, r, QString());
                for (const QString& a : add) if (!arr.contains(a)) arr.append(a);
                if (arr.isEmpty()) map.remove(id); else map[id] = arr;
            }
        } else {
            error = "Unknown label action";
            return;
        }
        st["labels"] = labels;
        st["messageLabels"] = map;
    });
    if (!error.isEmpty()) { sendError(res, 400, error); return; }
    handleSettingsGet(s, res);
}

// ---- snooze -----------------------------------------------------------------------

void WebmailApi::handleSnooze(const Session& s, const QJsonObject& body, httplib::Response& res) {
    const bool wake = body.value("action").toString() == "wake";
    const qint64 until = body.value("until").toInteger();
    if (!wake && until <= QDateTime::currentMSecsSinceEpoch()) { sendError(res, 400, "Pick a time in the future"); return; }

    QJsonArray added;
    QSet<QString> woken;
    for (const ItemRef& r : itemRefs(body)) {
        if (!folderExists(s, r.folder)) continue;
        const QString id = headerText(m_services.storage->loadHeader(folderKey(s, r.folder), r.uid), "Message-ID").trimmed();
        if (wake) {
            if (r.folder != "Snoozed") continue;
            const MailEntry e = m_services.storage->loadMessage(folderKey(s, "Snoozed"), r.uid);
            if (e.rawMessage.isNull() || m_services.storage->storeMessage(s.user, e.rawMessage) < 0) continue;
            m_services.storage->deleteMessage(folderKey(s, "Snoozed"), r.uid);
            woken.insert(id);
            continue;
        }
        if (id.isEmpty() || r.folder == "Snoozed") continue;
        if (!moveMessages(s.user, r.folder, {r.uid}, "Snoozed")) continue;
        added.append(QJsonObject{{"messageId", id}, {"until", until}, {"from", r.folder}});
    }
    UserSettings::update(m_services.storage->basePath(), s.user, [&](QJsonObject& st) {
        QJsonArray snoozes;
        for (const QJsonValue& v : st.value("snoozes").toArray()) {
            const QString id = v.toObject().value("messageId").toString();
            bool replaced = woken.contains(id);
            for (const QJsonValue& a : added) replaced |= a.toObject().value("messageId").toString() == id;
            if (!replaced) snoozes.append(v);
        }
        for (const QJsonValue& a : added) snoozes.append(a);
        st["snoozes"] = snoozes;
    });
    sendJson(res, 200, QJsonObject{{"status", "ok"}, {"snoozed", added.size()}, {"woken", woken.size()}});
}

void WebmailApi::snoozeLoop() {
    while (!m_stopping) {
        {
            std::unique_lock<std::mutex> lock(m_wakeMutex);
            m_wake.wait_for(lock, std::chrono::seconds(kSnoozeCheckSecs), [this] { return m_stopping.load(); });
        }
        if (m_stopping) break;
        const QDir base(m_services.storage->basePath());
        for (const QString& user : base.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
            if (QFile::exists(base.filePath(user + "/.webmail.json"))) wakeDueSnoozes(user);
        }
    }
}

// Returns due snoozed messages to the Inbox as unread.
void WebmailApi::wakeDueSnoozes(const QString& user) {
    const QString basePath = m_services.storage->basePath();
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    QSet<QString> due;
    for (const QJsonValue& v : UserSettings::load(basePath, user).value("snoozes").toArray())
        if (v.toObject().value("until").toInteger() <= now) due.insert(v.toObject().value("messageId").toString());
    if (due.isEmpty()) return;

    const QString snoozedKey = folderKeyFor(user, "Snoozed");
    for (const MailEntry& e : m_services.storage->listMessages(snoozedKey)) {
        const QString id = headerText(m_services.storage->loadHeader(snoozedKey, e.uid), "Message-ID").trimmed();
        if (!due.contains(id)) continue;
        const MailEntry full = m_services.storage->loadMessage(snoozedKey, e.uid);
        if (m_services.storage->storeMessage(user, full.rawMessage) > 0)
            m_services.storage->deleteMessage(snoozedKey, e.uid);
    }
    UserSettings::update(basePath, user, [&](QJsonObject& st) {
        QJsonArray keep;
        for (const QJsonValue& v : st.value("snoozes").toArray())
            if (!due.contains(v.toObject().value("messageId").toString())) keep.append(v);
        st["snoozes"] = keep;
    });
}

// ---- conversations ------------------------------------------------------------------

void WebmailApi::handleThread(const Session& s, const httplib::Request& req, httplib::Response& res) {
    const QString folder = normalizeFolder(queryParam(req, "folder"));
    const int uid = queryParam(req, "uid").toInt();
    if (!folderExists(s, folder) || uid < 1) { sendError(res, 404, "No such message"); return; }

    const QByteArray head = m_services.storage->loadHeader(folderKey(s, folder), uid);
    QSet<QString> ids = messageIds(headerText(head, "Message-ID") + ' ' + headerText(head, "In-Reply-To")
                                   + ' ' + headerText(head, "References"));

    struct Hit { QString folder; int uid; qint64 received; QSet<QString> ids; };
    QList<Hit> candidates;
    QStringList folders = {folder, "INBOX", "Sent", "Archive"};
    folders.removeDuplicates();
    for (const QString& f : folders) {
        if (!folderExists(s, f)) continue;
        for (const MailEntry& e : m_services.storage->listMessages(folderKey(s, f))) {
            const QByteArray h = m_services.storage->loadHeader(folderKey(s, f), e.uid);
            const QSet<QString> own = messageIds(headerText(h, "Message-ID") + ' ' + headerText(h, "In-Reply-To")
                                                 + ' ' + headerText(h, "References"));
            if (!own.isEmpty()) candidates.append({f, e.uid, e.date.toMSecsSinceEpoch(), own});
        }
    }

    QList<Hit> members;
    QSet<QString> seenKeys;
    for (int pass = 0; pass < 2 && !ids.isEmpty(); ++pass) {
        for (const Hit& h : candidates) {
            const QString key = h.folder + ':' + QString::number(h.uid);
            if (seenKeys.contains(key) || !h.ids.intersects(ids)) continue;
            seenKeys.insert(key);
            members.append(h);
            ids.unite(h.ids);
        }
    }
    if (seenKeys.isEmpty()) members.append({folder, uid, 0, {}});
    std::sort(members.begin(), members.end(), [](const Hit& a, const Hit& b) { return a.received < b.received; });
    if (members.size() > kMaxThreadMessages) members = members.mid(members.size() - kMaxThreadMessages);

    QJsonArray out;
    for (const Hit& h : members) out.append(QJsonObject{{"folder", h.folder}, {"uid", h.uid}, {"received", h.received}});
    sendJson(res, 200, QJsonObject{{"messages", out}});
}

// ---- contacts ---------------------------------------------------------------------

void WebmailApi::handleContacts(const Session& s, const httplib::Request& req, httplib::Response& res) {
    const QString q = queryParam(req, "q").trimmed().toLower();
    const int limit = queryParam(req, "all") == "1" ? 2000 : 8;
    const QJsonObject contacts = UserSettings::load(m_services.storage->basePath(), s.user).value("contacts").toObject();

    struct C { QString email, name; int count; qint64 last; };
    QList<C> list;
    for (auto it = contacts.begin(); it != contacts.end(); ++it) {
        const QJsonObject o = it.value().toObject();
        const QString name = o.value("name").toString();
        if (!q.isEmpty() && !it.key().contains(q) && !name.toLower().contains(q)) continue;
        list.append({it.key(), name, o.value("count").toInt(), o.value("last").toInteger()});
    }
    std::sort(list.begin(), list.end(), [](const C& a, const C& b) {
        return a.count != b.count ? a.count > b.count : a.last > b.last;
    });
    QJsonArray out;
    for (int i = 0; i < list.size() && i < limit; ++i)
        out.append(QJsonObject{{"email", list[i].email}, {"name", list[i].name}, {"count", list[i].count}});
    sendJson(res, 200, QJsonObject{{"contacts", out}});
}

void WebmailApi::handleContactsSave(const Session& s, const QJsonObject& body, httplib::Response& res) {
    const QString action = body.value("action").toString();
    const QString email = body.value("email").toString().trimmed().toLower();
    static const QRegularExpression emailRe(R"(^[^@\s<>",;]+@[A-Za-z0-9.-]+\.[A-Za-z]{2,}$)");
    if (!emailRe.match(email).hasMatch()) { sendError(res, 400, "Invalid email address"); return; }
    UserSettings::update(m_services.storage->basePath(), s.user, [&](QJsonObject& st) {
        if (action == "delete") {
            QJsonObject contacts = st.value("contacts").toObject();
            contacts.remove(email);
            st["contacts"] = contacts;
        } else {
            UserSettings::rememberContact(st, email, body.value("name").toString());
        }
    });
    sendJson(res, 200, QJsonObject{{"status", "ok"}});
}
