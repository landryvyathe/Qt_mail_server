#include "ImapConnection.h"
#include "ImapMime.h"
#include "../storage/MaildirStorage.h"
#include "../auth/AuthManager.h"
#include "../tls/TlsContext.h"
#include "../utils/RateLimiter.h"
#include <QDebug>
#include <QDateTime>
#include <QEventLoop>
#include <QLocale>
#include <QRegularExpression>
#include <QThread>
#include <QTimer>
#include <algorithm>
#include <functional>

namespace {

const int kIdleTimeoutMs = 300000;
const int kLiteralTimeoutMs = 30000;
const qint64 kMaxAppendBytes = 50LL * 1024 * 1024;
const char* kCapabilities = "IMAP4rev1 LITERAL+ ID NAMESPACE UNSELECT MOVE SPECIAL-USE";
const char* const kDefaultFolders[] = {"Sent", "Drafts", "Trash", "Junk", "Archive", "Snoozed"};

QString stripQuotes(const QString& s) { return ImapMime::unquote(s.trimmed()); }

QString imapFlagsToLetters(const QString& list) {
    const QString up = list.toUpper();
    QString letters;
    if (up.contains("\\DRAFT"))    letters += 'D';
    if (up.contains("\\FLAGGED"))  letters += 'F';
    if (up.contains("\\ANSWERED")) letters += 'R';
    if (up.contains("\\SEEN"))     letters += 'S';
    if (up.contains("\\DELETED"))  letters += 'T';
    return letters;
}

QByteArray internalDate(const QDateTime& dt) {
    return '"' + QLocale::c().toString(dt.toUTC(), "dd-MMM-yyyy hh:mm:ss").toLatin1() + " +0000\"";
}

} // namespace

ImapConnection::ImapConnection(qintptr socketDescriptor, MaildirStorage* storage,
                               AuthManager* auth, RateLimiter* rateLimiter,
                               qint64 quotaHardBytes, int quotaSoftPercent,
                               bool implicitTls, const QString& hostname)
    : m_socketDescriptor(socketDescriptor), m_socket(nullptr),
      m_storage(storage), m_auth(auth), m_rateLimiter(rateLimiter),
      m_quotaHardBytes(quotaHardBytes), m_quotaSoftPercent(quotaSoftPercent),
      m_implicitTls(implicitTls), m_hostname(hostname) {
    setAutoDelete(true);
}

void ImapConnection::sendTagged(const QString& tag, const QString& status, const QString& text) {
    m_socket->write(QString("%1 %2 %3\r\n").arg(tag, status, text).toUtf8());
    m_socket->flush();
}

void ImapConnection::sendUntagged(const QString& text) {
    m_socket->write(QString("* %1\r\n").arg(text).toUtf8());
    m_socket->flush();
}

void ImapConnection::run() {
    m_socket = new QSslSocket();
    if (!m_socket->setSocketDescriptor(m_socketDescriptor)) {
        qWarning() << "IMAP: Failed to set socket descriptor";
        delete m_socket;
        return;
    }

    QHostAddress peer = m_socket->peerAddress();
    if (m_rateLimiter && !m_rateLimiter->allow(peer)) {
        qWarning() << "IMAP: Rate-limited from" << peer.toString();
        sendUntagged("BYE Too many connections");
        m_socket->disconnectFromHost();
        delete m_socket;
        return;
    }

    if (m_implicitTls && TlsContext::instance().isConfigured()) {
        qInfo() << "IMAP: Starting implicit TLS for" << peer.toString();
        m_socket->setSslConfiguration(TlsContext::instance().getConfiguration());
        m_socket->startServerEncryption();

        QEventLoop loop;
        QTimer::singleShot(10000, &loop, &QEventLoop::quit);
        QObject::connect(m_socket, &QSslSocket::encrypted, &loop, &QEventLoop::quit);
        loop.exec();

        if (!m_socket->isEncrypted()) {
            qWarning() << "IMAP: Implicit TLS failed for" << peer.toString()
                       << "-" << m_socket->errorString();
            delete m_socket;
            return;
        }
        qInfo() << "IMAP: Implicit TLS established for" << peer.toString();
    }

    const QString caps = m_socket->isEncrypted()
        ? QString("OK [CAPABILITY %1 AUTH=PLAIN] %2 ready").arg(kCapabilities, m_hostname)
        : QString("OK [CAPABILITY %1 STARTTLS LOGINDISABLED] %2 ready").arg(kCapabilities, m_hostname);
    qInfo() << "IMAP: greeting sent to" << peer.toString();
    sendUntagged(caps);

    while (m_socket->isOpen()) {
        if (!m_socket->waitForReadyRead(kIdleTimeoutMs)) {
            qInfo() << "IMAP: connection idle timeout or closed for" << peer.toString();
            break;
        }
        while (m_socket->canReadLine()) {
            const QByteArray line = m_socket->readLine(65536).trimmed();
            if (line.isEmpty()) continue;
            const QString text = QString::fromUtf8(line);

            const int sp1 = text.indexOf(' ');
            const int sp2 = text.indexOf(' ', sp1 + 1);
            QString tag = "*", cmd = text.toUpper(), args;
            if (sp1 >= 0) {
                tag = text.left(sp1);
                cmd = (sp2 >= 0 ? text.mid(sp1 + 1, sp2 - sp1 - 1) : text.mid(sp1 + 1)).toUpper();
                args = sp2 >= 0 ? text.mid(sp2 + 1) : QString();
            }
            // Never log credentials.
            const bool secret = cmd == "LOGIN" || cmd == "AUTHENTICATE";
            qInfo() << "IMAP CMD from" << peer.toString() << ":"
                    << (secret ? tag + " " + cmd + " ***" : text.left(200));

            if (cmd == "LOGOUT") {
                sendUntagged("BYE Logging out");
                sendTagged(tag, "OK", "LOGOUT completed");
                m_socket->disconnectFromHost();
                break;
            }
            handleCommand(tag, cmd, args);
        }
    }

    qInfo() << "IMAP: connection finished from" << peer.toString();
    m_socket->disconnectFromHost();
    delete m_socket;
}

void ImapConnection::handleCommand(const QString& tag, const QString& cmd, const QString& args) {
    if (handleAuthCommand(tag, cmd, args)) return;
    if (m_state == NOT_AUTHENTICATED) {
        sendTagged(tag, "NO", "Authenticate first");
        return;
    }
    handleMailboxCommand(tag, cmd, args);
}

bool ImapConnection::handleAuthCommand(const QString& tag, const QString& cmd, const QString& args) {
    if (cmd == "CAPABILITY") {
        QString caps = QString("CAPABILITY ") + kCapabilities;
        if (TlsContext::instance().isConfigured() && !m_socket->isEncrypted())
            caps += " STARTTLS LOGINDISABLED";
        else
            caps += " AUTH=PLAIN";
        sendUntagged(caps);
        sendTagged(tag, "OK", "CAPABILITY completed");
    } else if (cmd == "NOOP") {
        if (m_state == SELECTED) announceNewMessages();
        sendTagged(tag, "OK", "NOOP completed");
    } else if (cmd == "ID") {
        sendUntagged("ID (\"name\" \"bykiva-mailserver\")");
        sendTagged(tag, "OK", "ID completed");
    } else if (cmd == "STARTTLS") {
        if (TlsContext::instance().isConfigured() && !m_socket->isEncrypted()) {
            sendTagged(tag, "OK", "Begin TLS now");
            m_socket->setSslConfiguration(TlsContext::instance().getConfiguration());
            m_socket->startServerEncryption();
            QEventLoop tlsLoop;
            QTimer::singleShot(5000, &tlsLoop, &QEventLoop::quit);
            QObject::connect(m_socket, &QSslSocket::encrypted, &tlsLoop, &QEventLoop::quit);
            tlsLoop.exec();
        } else {
            sendTagged(tag, "NO", "STARTTLS not available");
        }
    } else if (cmd == "LOGIN") {
        if (!m_auth || !m_auth->isEnabled() || !m_socket->isEncrypted()) {
            sendTagged(tag, "NO", "LOGIN disabled, use TLS");
            return true;
        }
        const QStringList parts = ImapMime::tokenize(args);
        if (parts.size() < 2) { sendTagged(tag, "BAD", "Syntax: LOGIN user pass"); return true; }
        const QString user = stripQuotes(parts[0]);
        if (m_auth->authenticate(user, stripQuotes(parts[1]), m_socket->peerAddress())) {
            loginSucceeded(user);
            sendTagged(tag, "OK", QString("[CAPABILITY %1] LOGIN completed").arg(kCapabilities));
        } else {
            sendTagged(tag, "NO", "[AUTHENTICATIONFAILED] LOGIN failed");
        }
    } else if (cmd == "AUTHENTICATE") {
        if (!m_auth || !m_auth->isEnabled() || !m_socket->isEncrypted()
            || args.section(' ', 0, 0).toUpper() != "PLAIN") {
            sendTagged(tag, "NO", "Unsupported AUTH method");
            return true;
        }
        QByteArray encoded = args.section(' ', 1, 1).toLatin1();
        if (encoded.isEmpty()) {
            m_socket->write("+ \r\n");
            m_socket->flush();
            if (!m_socket->canReadLine()) m_socket->waitForReadyRead(10000);
            encoded = m_socket->readLine(8192).trimmed();
        }
        const QByteArray blob = QByteArray::fromBase64(encoded);
        const int sep1 = blob.indexOf('\0');
        const int sep2 = blob.indexOf('\0', sep1 + 1);
        if (sep1 < 0 || sep2 <= sep1) { sendTagged(tag, "BAD", "Invalid PLAIN data"); return true; }
        const QString user = QString::fromUtf8(blob.mid(sep1 + 1, sep2 - sep1 - 1));
        const QString pass = QString::fromUtf8(blob.mid(sep2 + 1));
        if (m_auth->authenticate(user, pass, m_socket->peerAddress())) {
            loginSucceeded(user);
            sendTagged(tag, "OK", "AUTHENTICATE completed");
        } else {
            sendTagged(tag, "NO", "[AUTHENTICATIONFAILED] AUTHENTICATE failed");
        }
    } else {
        return false;
    }
    return true;
}

void ImapConnection::loginSucceeded(const QString& user) {
    m_state = AUTHENTICATED;
    m_session.user = user.section('@', 0, 0).toLower().trimmed();
    m_session.mailbox.clear();
    for (const char* folder : kDefaultFolders)
        if (!m_storage->mailboxExists(m_session.user, folder))
            m_storage->createMailbox(m_session.user, folder);
}

// Storage path key: INBOX lives in the user's maildir, folders in ".Name" subdirectories.
QString ImapConnection::storeKey(const QString& mailbox) const {
    return mailbox == "INBOX" ? m_session.user : m_session.user + "/." + mailbox;
}

// Returns "INBOX", a valid folder name, or an empty string when the name is unsafe.
QString ImapConnection::normalizeMailbox(const QString& raw) const {
    const QString name = stripQuotes(raw);
    if (name.compare("INBOX", Qt::CaseInsensitive) == 0) return "INBOX";
    static const QRegularExpression bad(R"([/\\*%\x00-\x1f]|\.\.|^\.|\.$)");
    if (name.isEmpty() || name.size() > 200 || bad.match(name).hasMatch()) return {};
    return name;
}

bool ImapConnection::mailboxExists(const QString& mailbox) const {
    return mailbox == "INBOX" || (!mailbox.isEmpty() && m_storage->mailboxExists(m_session.user, mailbox));
}

void ImapConnection::handleMailboxCommand(const QString& tag, const QString& cmd, const QString& args) {
    if (cmd == "SELECT" || cmd == "EXAMINE") {
        selectMailbox(tag, cmd, args);
    } else if (cmd == "NAMESPACE") {
        sendUntagged("NAMESPACE ((\"\" \".\")) NIL NIL");
        sendTagged(tag, "OK", "NAMESPACE completed");
    } else if (cmd == "LIST" || cmd == "LSUB") {
        listMailboxes(tag, cmd, args);
    } else if (cmd == "SUBSCRIBE" || cmd == "UNSUBSCRIBE") {
        sendTagged(tag, "OK", cmd + " completed");
    } else if (cmd == "CREATE") {
        createMailbox(tag, args);
    } else if (cmd == "DELETE") {
        deleteMailbox(tag, args);
    } else if (cmd == "RENAME") {
        renameMailbox(tag, args);
    } else if (cmd == "STATUS") {
        statusMailbox(tag, args);
    } else if (cmd == "APPEND") {
        append(tag, args);
    } else if (m_state != SELECTED) {
        sendTagged(tag, "NO", "Select a mailbox first");
    } else if (cmd == "FETCH") {
        fetch(tag, args, false);
    } else if (cmd == "STORE") {
        store(tag, args, false);
    } else if (cmd == "SEARCH") {
        search(tag, args, false);
    } else if (cmd == "COPY" || cmd == "MOVE") {
        copyMessages(tag, args, false, cmd == "MOVE");
    } else if (cmd == "UID") {
        const QString sub = args.section(' ', 0, 0).toUpper();
        const QString rest = args.section(' ', 1);
        if (sub == "FETCH") fetch(tag, rest, true);
        else if (sub == "STORE") store(tag, rest, true);
        else if (sub == "SEARCH") search(tag, rest, true);
        else if (sub == "COPY" || sub == "MOVE") copyMessages(tag, rest, true, sub == "MOVE");
        else sendTagged(tag, "BAD", "Unsupported UID command");
    } else if (cmd == "EXPUNGE") {
        if (m_session.readOnly) { sendTagged(tag, "NO", "Mailbox is read-only"); return; }
        expunge(true);
        sendTagged(tag, "OK", "EXPUNGE completed");
    } else if (cmd == "CHECK") {
        announceNewMessages();
        sendTagged(tag, "OK", "CHECK completed");
    } else if (cmd == "CLOSE" || cmd == "UNSELECT") {
        if (cmd == "CLOSE" && !m_session.readOnly) expunge(false);
        m_state = AUTHENTICATED;
        m_session.messages.clear();
        sendTagged(tag, "OK", cmd + " completed");
    } else {
        sendTagged(tag, "BAD", "Unknown command");
    }
}

void ImapConnection::listMailboxes(const QString& tag, const QString& cmd, const QString& args) {
    const QStringList parts = ImapMime::tokenize(args);
    const QString reference = parts.size() >= 1 ? stripQuotes(parts[0]) : QString();
    const QString pattern = parts.size() >= 2 ? stripQuotes(parts[1]) : QString("*");
    if (pattern.isEmpty()) {
        sendUntagged(cmd + " (\\Noselect) \".\" \"\"");
        sendTagged(tag, "OK", cmd + " completed");
        return;
    }

    QString rx;
    for (QChar c : reference + pattern) {
        if (c == '*') rx += ".*";
        else if (c == '%') rx += "[^.]*";
        else rx += QRegularExpression::escape(QString(c));
    }
    const QRegularExpression matcher(QRegularExpression::anchoredPattern(rx),
                                     QRegularExpression::CaseInsensitiveOption);

    const QStringList folders = m_storage->listMailboxes(m_session.user);
    QStringList names = {"INBOX"};
    names += folders;
    for (const QString& name : names) {
        if (!matcher.match(name).hasMatch()) continue;
        bool hasChildren = false;
        for (const QString& f : folders) hasChildren |= f.startsWith(name + '.');
        QStringList attrs = {hasChildren ? "\\HasChildren" : "\\HasNoChildren"};
        if (name == "Sent") attrs << "\\Sent";
        else if (name == "Drafts") attrs << "\\Drafts";
        else if (name == "Trash") attrs << "\\Trash";
        else if (name == "Junk") attrs << "\\Junk";
        else if (name == "Archive") attrs << "\\Archive";
        sendUntagged(QString("%1 (%2) \".\" %3").arg(cmd, attrs.join(' '),
                     QString::fromUtf8(ImapMime::nstring(name.toUtf8()))));
    }
    sendTagged(tag, "OK", cmd + " completed");
}

void ImapConnection::createMailbox(const QString& tag, const QString& args) {
    QString name = normalizeMailbox(args);
    if (name.isEmpty() || name == "INBOX") { sendTagged(tag, "NO", "Invalid mailbox name"); return; }
    if (mailboxExists(name)) { sendTagged(tag, "NO", "[ALREADYEXISTS] Mailbox already exists"); return; }
    // Create missing parents so the hierarchy stays browsable.
    const QStringList levels = name.split('.');
    for (int i = 1; i <= levels.size(); ++i) {
        const QString part = levels.mid(0, i).join('.');
        if (!mailboxExists(part) && !m_storage->createMailbox(m_session.user, part)) {
            sendTagged(tag, "NO", "CREATE failed");
            return;
        }
    }
    sendTagged(tag, "OK", "CREATE completed");
}

void ImapConnection::deleteMailbox(const QString& tag, const QString& args) {
    const QString name = normalizeMailbox(args);
    if (name == "INBOX" || name.isEmpty()) { sendTagged(tag, "NO", "Cannot delete this mailbox"); return; }
    if (!mailboxExists(name)) { sendTagged(tag, "NO", "[NONEXISTENT] No such mailbox"); return; }
    if (m_state == SELECTED && m_session.mailbox == name) {
        m_state = AUTHENTICATED;
        m_session.messages.clear();
    }
    const bool ok = m_storage->deleteMailbox(m_session.user, name);
    sendTagged(tag, ok ? "OK" : "NO", ok ? "DELETE completed" : "DELETE failed");
}

void ImapConnection::renameMailbox(const QString& tag, const QString& args) {
    const QStringList parts = ImapMime::tokenize(args);
    if (parts.size() < 2) { sendTagged(tag, "BAD", "Syntax: RENAME from to"); return; }
    const QString from = normalizeMailbox(parts[0]);
    const QString to = normalizeMailbox(parts[1]);
    if (from.isEmpty() || to.isEmpty() || from == "INBOX" || to == "INBOX") {
        sendTagged(tag, "NO", "Invalid mailbox name");
        return;
    }
    if (!mailboxExists(from)) { sendTagged(tag, "NO", "[NONEXISTENT] No such mailbox"); return; }
    if (mailboxExists(to)) { sendTagged(tag, "NO", "[ALREADYEXISTS] Target exists"); return; }
    const bool ok = m_storage->renameMailbox(m_session.user, from, to);
    sendTagged(tag, ok ? "OK" : "NO", ok ? "RENAME completed" : "RENAME failed");
}

void ImapConnection::statusMailbox(const QString& tag, const QString& args) {
    const QStringList parts = ImapMime::tokenize(args);
    const QString name = parts.isEmpty() ? QString() : normalizeMailbox(parts[0]);
    if (!mailboxExists(name)) { sendTagged(tag, "NO", "[NONEXISTENT] No such mailbox"); return; }

    const QList<MailEntry> msgs = m_storage->listMessages(storeKey(name));
    int unseen = 0, top = 0;
    for (const auto& e : msgs) { if (!e.seen) ++unseen; top = std::max(top, e.uid); }
    const QString wanted = parts.size() >= 2 ? parts.mid(1).join(' ').toUpper() : QString("(MESSAGES)");
    QStringList items;
    if (wanted.contains("MESSAGES")) items << QString("MESSAGES %1").arg(msgs.size());
    if (wanted.contains("RECENT")) items << "RECENT 0";
    if (wanted.contains("UIDNEXT")) items << QString("UIDNEXT %1").arg(top + 1);
    if (wanted.contains("UIDVALIDITY")) items << QString("UIDVALIDITY %1").arg(m_storage->uidValidity(storeKey(name)));
    if (wanted.contains("UNSEEN")) items << QString("UNSEEN %1").arg(unseen);
    sendUntagged(QString("STATUS %1 (%2)").arg(QString::fromUtf8(ImapMime::nstring(name.toUtf8())),
                                               items.join(' ')));
    sendTagged(tag, "OK", "STATUS completed");
}

void ImapConnection::copyMessages(const QString& tag, const QString& args, bool byUid, bool move) {
    const QStringList parts = ImapMime::tokenize(args);
    if (parts.size() < 2) { sendTagged(tag, "BAD", "Syntax: COPY set mailbox"); return; }
    const QString dest = normalizeMailbox(parts[1]);
    if (!mailboxExists(dest)) { sendTagged(tag, "NO", "[TRYCREATE] No such mailbox"); return; }
    if (move && m_session.readOnly) { sendTagged(tag, "NO", "Mailbox is read-only"); return; }

    const QList<int> indexes = resolveSet(parts[0], byUid);
    qint64 bytes = 0;
    for (int i : indexes) bytes += m_session.messages[i].size;
    if (!move && m_storage->isOverQuota(m_session.user, bytes)) {
        sendTagged(tag, "NO", "[OVERQUOTA] Quota exceeded");
        return;
    }
    for (int i : indexes) {
        const MailEntry e = m_storage->loadMessage(storeKey(m_session.mailbox), m_session.messages[i].uid);
        const int uid = m_storage->storeMessage(storeKey(dest), e.rawMessage);
        if (uid < 0) { sendTagged(tag, "NO", "COPY failed"); return; }
        QString letters = flagLetters(i);
        letters.remove('T');
        m_storage->setFlags(storeKey(dest), uid, letters);
    }
    if (move) removeIndexes(indexes);
    if (dest == m_session.mailbox) announceNewMessages();
    sendTagged(tag, "OK", QString(byUid ? "UID " : "") + (move ? "MOVE" : "COPY") + " completed");
}

void ImapConnection::selectMailbox(const QString& tag, const QString& cmd, const QString& args) {
    const QString name = normalizeMailbox(args);
    if (!mailboxExists(name)) {
        m_state = AUTHENTICATED;
        m_session.messages.clear();
        sendTagged(tag, "NO", "[NONEXISTENT] No such mailbox");
        return;
    }
    m_session.mailbox = name;
    refreshMessages();
    m_state = SELECTED;
    m_session.readOnly = cmd == "EXAMINE";

    int firstUnseen = 0;
    for (int i = 0; i < m_session.messages.size() && !firstUnseen; ++i)
        if (!flagLetters(i).contains('S')) firstUnseen = i + 1;

    sendUntagged("FLAGS (\\Answered \\Flagged \\Deleted \\Seen \\Draft)");
    sendUntagged("OK [PERMANENTFLAGS (\\Answered \\Flagged \\Deleted \\Seen \\Draft)] Flags permitted");
    sendUntagged(QString("%1 EXISTS").arg(m_session.messages.size()));
    sendUntagged("0 RECENT");
    if (firstUnseen) sendUntagged(QString("OK [UNSEEN %1] First unseen").arg(firstUnseen));
    sendUntagged(QString("OK [UIDVALIDITY %1] UIDs valid").arg(m_storage->uidValidity(storeKey(name))));
    sendUntagged(QString("OK [UIDNEXT %1] Predicted next UID").arg(maxUid() + 1));
    sendTagged(tag, "OK", QString("[%1] %2 completed")
                              .arg(m_session.readOnly ? "READ-ONLY" : "READ-WRITE", cmd));
}

void ImapConnection::refreshMessages() {
    m_session.messages = m_storage->listMessages(storeKey(m_session.mailbox));
    m_session.messages.erase(std::remove_if(m_session.messages.begin(), m_session.messages.end(),
                                            [](const MailEntry& e) { return e.uid < 1; }),
                             m_session.messages.end());
    std::sort(m_session.messages.begin(), m_session.messages.end(),
              [](const MailEntry& a, const MailEntry& b) { return a.uid < b.uid; });
}

void ImapConnection::announceNewMessages() {
    const int top = maxUid();
    QList<MailEntry> fresh = m_storage->listMessages(storeKey(m_session.mailbox));
    std::sort(fresh.begin(), fresh.end(),
              [](const MailEntry& a, const MailEntry& b) { return a.uid < b.uid; });
    int added = 0;
    for (const MailEntry& e : fresh)
        if (e.uid > top) { m_session.messages.append(e); ++added; }
    if (added) sendUntagged(QString("%1 EXISTS").arg(m_session.messages.size()));
}

int ImapConnection::maxUid() const {
    return m_session.messages.isEmpty() ? 0 : m_session.messages.last().uid;
}

// Returns 0-based indexes into m_session.messages.
QList<int> ImapConnection::resolveSet(const QString& set, bool byUid) const {
    QList<int> indexes;
    if (!byUid) {
        for (int seq : ImapMime::parseSequenceSet(set, m_session.messages.size()))
            indexes.append(seq - 1);
        return indexes;
    }
    // "N:*" must include the highest UID even when N is larger (RFC 3501 6.4.8).
    const QList<int> uids = ImapMime::parseSequenceSet(set, std::max(maxUid(), 1));
    for (int i = 0; i < m_session.messages.size(); ++i)
        if (std::binary_search(uids.begin(), uids.end(), m_session.messages[i].uid))
            indexes.append(i);
    return indexes;
}

QString ImapConnection::flagLetters(int index) const {
    const QString& name = m_session.messages[index].fileName;
    const int idx = name.indexOf(":2,");
    return idx >= 0 ? name.mid(idx + 3) : QString();
}

QByteArray ImapConnection::imapFlags(int index) const {
    const QString letters = flagLetters(index);
    QStringList out;
    if (letters.contains('R')) out << "\\Answered";
    if (letters.contains('F')) out << "\\Flagged";
    if (letters.contains('T')) out << "\\Deleted";
    if (letters.contains('S')) out << "\\Seen";
    if (letters.contains('D')) out << "\\Draft";
    return '(' + out.join(' ').toLatin1() + ')';
}

void ImapConnection::applyFlags(int index, const QString& letters) {
    MailEntry& e = m_session.messages[index];
    if (!m_storage->setFlags(storeKey(m_session.mailbox), e.uid, letters)) return;
    QString base = e.fileName;
    const int idx = base.indexOf(":2,");
    if (idx >= 0) base = base.left(idx);
    QString sorted;
    for (QChar c : QStringLiteral("DFRST"))
        if (letters.contains(c)) sorted += c;
    e.fileName = base + ":2," + sorted;
    e.seen = sorted.contains('S');
    e.flagged = sorted.contains('F');
    e.deleted_ = sorted.contains('T');
}

QByteArray ImapConnection::loadCrlf(int index) const {
    return ImapMime::toCrlf(m_storage->loadMessage(storeKey(m_session.mailbox), m_session.messages[index].uid).rawMessage);
}

void ImapConnection::fetch(const QString& tag, const QString& args, bool byUid) {
    const int space = args.indexOf(' ');
    if (space < 0) { sendTagged(tag, "BAD", "Syntax: FETCH set items"); return; }
    QString itemSpec = args.mid(space + 1).trimmed();
    if (itemSpec.startsWith('(') && itemSpec.endsWith(')')) itemSpec = itemSpec.mid(1, itemSpec.size() - 2);

    QStringList items = ImapMime::tokenize(itemSpec);
    QStringList expanded;
    for (const QString& it : items) {
        const QString up = it.toUpper();
        if (up == "ALL") expanded << "FLAGS" << "INTERNALDATE" << "RFC822.SIZE" << "ENVELOPE";
        else if (up == "FAST") expanded << "FLAGS" << "INTERNALDATE" << "RFC822.SIZE";
        else if (up == "FULL") expanded << "FLAGS" << "INTERNALDATE" << "RFC822.SIZE" << "ENVELOPE" << "BODY";
        else expanded << it;
    }
    if (byUid && !expanded.contains("UID", Qt::CaseInsensitive)) expanded.prepend("UID");

    static const QRegularExpression bodyRe("^(BODY(?:\\.PEEK)?)\\[([^\\]]*)\\](?:<(\\d+)(?:\\.(\\d+))?>)?$",
                                           QRegularExpression::CaseInsensitiveOption);

    for (int index : resolveSet(args.left(space), byUid)) {
        QByteArray msg;
        bool loaded = false;
        auto message = [&]() -> const QByteArray& {
            if (!loaded) { msg = loadCrlf(index); loaded = true; }
            return msg;
        };
        bool markSeen = false;
        bool flagsSent = false;
        QList<QByteArray> parts;

        for (const QString& item : expanded) {
            const QString up = item.toUpper();
            const MailEntry& e = m_session.messages[index];
            if (up == "UID") parts << "UID " + QByteArray::number(e.uid);
            else if (up == "FLAGS") { parts << "FLAGS " + imapFlags(index); flagsSent = true; }
            else if (up == "INTERNALDATE") parts << "INTERNALDATE " + internalDate(e.date);
            else if (up == "RFC822.SIZE") parts << "RFC822.SIZE " + QByteArray::number(message().size());
            else if (up == "ENVELOPE") parts << "ENVELOPE " + ImapMime::envelope(message());
            else if (up == "BODYSTRUCTURE" || up == "BODY") parts << up.toLatin1() + ' ' + ImapMime::bodyStructure(message());
            else if (up == "RFC822") { parts << "RFC822 " + ImapMime::nstring(message()); markSeen = true; }
            else if (up == "RFC822.HEADER") parts << "RFC822.HEADER " + ImapMime::nstring(ImapMime::headerBlock(message()));
            else if (up == "RFC822.TEXT") { parts << "RFC822.TEXT " + ImapMime::nstring(ImapMime::bodyBlock(message())); markSeen = true; }
            else {
                const auto m = bodyRe.match(item);
                if (!m.hasMatch()) { sendTagged(tag, "BAD", "Unsupported FETCH item " + item); return; }
                QByteArray data;
                if (!ImapMime::section(message(), m.captured(2), data)) data = "";
                QByteArray name = "BODY[" + m.captured(2).toUpper().toLatin1() + "]";
                if (!m.captured(3).isEmpty()) {
                    const qint64 start = m.captured(3).toLongLong();
                    const qint64 count = m.captured(4).isEmpty() ? -1 : m.captured(4).toLongLong();
                    data = start >= data.size() ? QByteArray("") : data.mid(start, count);
                    name += '<' + m.captured(3).toLatin1() + '>';
                }
                parts << name + ' ' + ImapMime::nstring(data.isNull() ? QByteArray("") : data);
                if (!m.captured(1).contains("PEEK", Qt::CaseInsensitive)) markSeen = true;
            }
        }

        if (markSeen && !m_session.readOnly && !flagLetters(index).contains('S')) {
            applyFlags(index, flagLetters(index) + 'S');
            if (!flagsSent) parts << "FLAGS " + imapFlags(index);
        }
        QByteArray line = "* " + QByteArray::number(index + 1) + " FETCH (";
        for (int i = 0; i < parts.size(); ++i) line += (i ? " " : "") + parts[i];
        m_socket->write(line + ")\r\n");
    }
    m_socket->flush();
    sendTagged(tag, "OK", QString(byUid ? "UID FETCH" : "FETCH") + " completed");
}

void ImapConnection::store(const QString& tag, const QString& args, bool byUid) {
    const QStringList parts = ImapMime::tokenize(args);
    if (parts.size() < 3) { sendTagged(tag, "BAD", "Syntax: STORE set op flags"); return; }
    if (m_session.readOnly) { sendTagged(tag, "NO", "Mailbox is read-only"); return; }

    const QString op = parts[1].toUpper();
    const bool silent = op.endsWith(".SILENT");
    const QString letters = imapFlagsToLetters(parts.mid(2).join(' '));

    for (int index : resolveSet(parts[0], byUid)) {
        QString current = flagLetters(index);
        QString next;
        if (op.startsWith("+FLAGS")) next = current + letters;
        else if (op.startsWith("-FLAGS")) {
            next = current;
            for (QChar c : letters) next.remove(c);
        } else if (op.startsWith("FLAGS")) next = letters;
        else { sendTagged(tag, "BAD", "Unknown STORE operation"); return; }
        applyFlags(index, next);

        if (!silent) {
            QString resp = QString("%1 FETCH (FLAGS %2").arg(index + 1).arg(QString::fromLatin1(imapFlags(index)));
            if (byUid) resp += QString(" UID %1").arg(m_session.messages[index].uid);
            sendUntagged(resp + ")");
        }
    }
    sendTagged(tag, "OK", QString(byUid ? "UID STORE" : "STORE") + " completed");
}

void ImapConnection::search(const QString& tag, const QString& args, bool byUid) {
    QStringList tokens = ImapMime::tokenize(args);
    if (tokens.size() >= 2 && tokens[0].toUpper() == "CHARSET") tokens = tokens.mid(2);

    using Pred = std::function<bool(int)>;
    int pos = 0;
    std::function<Pred()> parseKey;
    auto dateOf = [](const QString& s) {
        return QLocale::c().toDate(stripQuotes(s), "d-MMM-yyyy");
    };
    auto contains = [](int, const QByteArray& part, const QString& needle) {
        return QString::fromUtf8(part).contains(needle, Qt::CaseInsensitive);
    };

    parseKey = [&]() -> Pred {
        if (pos >= tokens.size()) return [](int) { return true; };
        const QString tok = tokens[pos++];
        const QString key = tok.toUpper();
        auto arg = [&]() { return pos < tokens.size() ? stripQuotes(tokens[pos++]) : QString(); };
        auto has = [this](QChar c) { return [this, c](int i) { return flagLetters(i).contains(c); }; };
        auto lacks = [this](QChar c) { return [this, c](int i) { return !flagLetters(i).contains(c); }; };

        if (key.startsWith('(')) {
            const QStringList saved = tokens;
            const int savedPos = pos;
            tokens = ImapMime::tokenize(tok.mid(1, tok.size() - 2));
            pos = 0;
            QList<Pred> preds;
            while (pos < tokens.size()) preds << parseKey();
            tokens = saved;
            pos = savedPos;
            return [preds](int i) { for (const auto& p : preds) if (!p(i)) return false; return true; };
        }
        if (key == "ALL") return [](int) { return true; };
        if (key == "SEEN") return has('S');
        if (key == "UNSEEN" || key == "NEW") return lacks('S');
        if (key == "OLD" || key == "RECENT") return [key](int) { return key == "OLD"; };
        if (key == "FLAGGED") return has('F');
        if (key == "UNFLAGGED") return lacks('F');
        if (key == "DELETED") return has('T');
        if (key == "UNDELETED") return lacks('T');
        if (key == "ANSWERED") return has('R');
        if (key == "UNANSWERED") return lacks('R');
        if (key == "DRAFT") return has('D');
        if (key == "UNDRAFT") return lacks('D');
        if (key == "NOT") { Pred p = parseKey(); return [p](int i) { return !p(i); }; }
        if (key == "OR") { Pred a = parseKey(); Pred b = parseKey(); return [a, b](int i) { return a(i) || b(i); }; }
        if (key == "UID") {
            const QList<int> set = resolveSet(arg(), true);
            return [set](int i) { return set.contains(i); };
        }
        if (key == "LARGER" || key == "SMALLER") {
            const qint64 n = arg().toLongLong();
            const bool larger = key == "LARGER";
            return [this, n, larger](int i) { const qint64 s = loadCrlf(i).size(); return larger ? s > n : s < n; };
        }
        if (key == "SINCE" || key == "BEFORE" || key == "ON" ||
            key == "SENTSINCE" || key == "SENTBEFORE" || key == "SENTON") {
            const QDate d = dateOf(arg());
            const QString k = key.startsWith("SENT") ? key.mid(4) : key;
            return [this, d, k](int i) {
                const QDate md = m_session.messages[i].date.toUTC().date();
                return k == "SINCE" ? md >= d : k == "BEFORE" ? md < d : md == d;
            };
        }
        if (key == "FROM" || key == "TO" || key == "CC" || key == "BCC" || key == "SUBJECT") {
            const QString needle = arg();
            const QByteArray field = key.toLatin1();
            return [this, needle, field, contains](int i) {
                return contains(i, ImapMime::headerValue(ImapMime::headerBlock(loadCrlf(i)), field), needle);
            };
        }
        if (key == "HEADER") {
            const QByteArray field = arg().toLatin1();
            const QString needle = arg();
            return [this, needle, field, contains](int i) {
                return contains(i, ImapMime::headerValue(ImapMime::headerBlock(loadCrlf(i)), field), needle);
            };
        }
        if (key == "BODY" || key == "TEXT") {
            const QString needle = arg();
            const bool bodyOnly = key == "BODY";
            return [this, needle, bodyOnly, contains](int i) {
                const QByteArray m = loadCrlf(i);
                return contains(i, bodyOnly ? ImapMime::bodyBlock(m) : m, needle);
            };
        }
        if (key == "KEYWORD") { arg(); return [](int) { return false; }; }
        if (key == "UNKEYWORD") { arg(); return [](int) { return true; }; }
        // A bare sequence set.
        const QList<int> set = resolveSet(tok, false);
        return [set](int i) { return set.contains(i); };
    };

    QList<Pred> preds;
    while (pos < tokens.size()) preds << parseKey();

    QStringList hits;
    for (int i = 0; i < m_session.messages.size(); ++i) {
        bool ok = true;
        for (const auto& p : preds) if (!p(i)) { ok = false; break; }
        if (ok) hits << QString::number(byUid ? m_session.messages[i].uid : i + 1);
    }
    sendUntagged(hits.isEmpty() ? QString("SEARCH") : "SEARCH " + hits.join(' '));
    sendTagged(tag, "OK", QString(byUid ? "UID SEARCH" : "SEARCH") + " completed");
}

void ImapConnection::expunge(bool sendResponses) {
    for (int i = m_session.messages.size() - 1; i >= 0; --i) {
        if (!flagLetters(i).contains('T')) continue;
        if (!m_storage->deleteMessage(storeKey(m_session.mailbox), m_session.messages[i].uid)) continue;
        m_session.messages.removeAt(i);
        if (sendResponses) sendUntagged(QString("%1 EXPUNGE").arg(i + 1));
    }
}

// Removes specific messages (after MOVE), announcing each as expunged.
void ImapConnection::removeIndexes(const QList<int>& indexes) {
    QList<int> sorted = indexes;
    std::sort(sorted.begin(), sorted.end(), std::greater<int>());
    for (int i : sorted) {
        if (!m_storage->deleteMessage(storeKey(m_session.mailbox), m_session.messages[i].uid)) continue;
        m_session.messages.removeAt(i);
        sendUntagged(QString("%1 EXPUNGE").arg(i + 1));
    }
}

bool ImapConnection::readLiteral(qint64 size, QByteArray& out) {
    out.clear();
    while (out.size() < size) {
        if (m_socket->bytesAvailable() == 0 && !m_socket->waitForReadyRead(kLiteralTimeoutMs))
            return false;
        out += m_socket->read(size - out.size());
    }
    // Consume the rest of the command line (normally just CRLF).
    if (!m_socket->canReadLine()) m_socket->waitForReadyRead(kLiteralTimeoutMs);
    m_socket->readLine(65536);
    return true;
}

void ImapConnection::append(const QString& tag, const QString& args) {
    static const QRegularExpression litRe("\\{(\\d+)(\\+?)\\}$");
    const auto m = litRe.match(args.trimmed());
    if (!m.hasMatch()) { sendTagged(tag, "BAD", "APPEND requires a literal"); return; }

    const qint64 size = m.captured(1).toLongLong();
    const bool nonSync = !m.captured(2).isEmpty();
    const QStringList parts = ImapMime::tokenize(args.left(m.capturedStart()).trimmed());
    const QString dest = parts.isEmpty() ? QString() : normalizeMailbox(parts[0]);
    const bool exists = mailboxExists(dest);
    const bool tooBig = size > kMaxAppendBytes
        || m_storage->isOverQuota(m_session.user, size);

    if (!nonSync) {
        if (!exists) { sendTagged(tag, "NO", "[TRYCREATE] No such mailbox"); return; }
        if (tooBig) { sendTagged(tag, "NO", "[OVERQUOTA] Message too large"); return; }
        m_socket->write("+ Ready for literal data\r\n");
        m_socket->flush();
    }

    QByteArray data;
    if (!readLiteral(size, data)) { sendTagged(tag, "BAD", "Literal not received"); return; }
    if (!exists) { sendTagged(tag, "NO", "[TRYCREATE] No such mailbox"); return; }
    if (tooBig) { sendTagged(tag, "NO", "[OVERQUOTA] Message too large"); return; }

    const int uid = m_storage->storeMessage(storeKey(dest), data);
    if (uid < 0) { sendTagged(tag, "NO", "APPEND failed"); return; }

    QString letters;
    for (const QString& p : parts.mid(1))
        if (p.startsWith('(')) letters = imapFlagsToLetters(p);
    if (!letters.isEmpty()) m_storage->setFlags(storeKey(dest), uid, letters);
    if (m_state == SELECTED && dest == m_session.mailbox) announceNewMessages();
    sendTagged(tag, "OK", "APPEND completed");
}
