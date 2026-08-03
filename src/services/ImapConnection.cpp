#include "ImapConnection.h"
#include "../storage/MaildirStorage.h"
#include "../auth/AuthManager.h"
#include "../tls/TlsContext.h"
#include "../utils/RateLimiter.h"
#include <QDebug>
#include <QDateTime>
#include <QEventLoop>
#include <QThread>
#include <QTimer>

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

    // Rate limit
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

    // Build capability greeting (don't advertise STARTTLS if already encrypted)
    QString caps;
    if (!m_socket->isEncrypted())
        caps = "OK [CAPABILITY IMAP4rev1 STARTTLS LOGINDISABLED] " + m_hostname + " ready";
    else
        caps = "OK [CAPABILITY IMAP4rev1 AUTH=LOGIN AUTH=PLAIN] " + m_hostname + " ready";

    qInfo() << "IMAP: greeting sent to" << peer.toString();
    sendUntagged(caps);

    while (m_socket->isOpen()) {
        if (m_socket->waitForReadyRead(300000)) {
            while (m_socket->canReadLine()) {
                QByteArray line = m_socket->readLine(65536).trimmed();
                if (line.isEmpty()) continue;
                QString text = QString::fromUtf8(line);
                qInfo() << "IMAP CMD from" << peer.toString() << ":" << text.left(200);
                QString tag, cmd, args;

                int sp1 = text.indexOf(' ');
                int sp2 = text.indexOf(' ', sp1 + 1);

                if (sp1 >= 0) {
                    tag = text.left(sp1);
                    if (sp2 >= 0) {
                        cmd  = text.mid(sp1 + 1, sp2 - sp1 - 1).toUpper();
                        args = text.mid(sp2 + 1);
                    } else {
                        cmd  = text.mid(sp1 + 1).toUpper();
                        args = "";
                    }
                } else {
                    tag  = "*";
                    cmd  = text.toUpper();
                    args = "";
                }

                if (cmd == "LOGOUT") {
                    sendTagged(tag, "OK", "LOGOUT completed");
                    m_socket->disconnectFromHost();
                    break;
                }

                handleCommand(tag, cmd, args);
            }
        } else {
            qInfo() << "IMAP: connection idle timeout or closed for" << peer.toString();
            break;
        }
    }

    qInfo() << "IMAP: connection finished from" << peer.toString();
    m_socket->disconnectFromHost();
    delete m_socket;
}

void ImapConnection::handleCommand(const QString& tag, const QString& cmd, const QString& args) {
    // --- CAPABILITY ---
    if (cmd == "CAPABILITY") {
        QString caps = "CAPABILITY IMAP4rev1";
        if (TlsContext::instance().isConfigured() && !m_socket->isEncrypted())
            caps += " STARTTLS";
        if (m_auth && m_auth->isEnabled()) {
            if (m_socket->isEncrypted())
                caps += " AUTH=LOGIN AUTH=PLAIN";
            else
                caps += " LOGINDISABLED";
        }
        caps += " " + m_hostname;
        sendUntagged(caps);
        sendTagged(tag, "OK", "CAPABILITY completed");

    // --- STARTTLS ---
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

    // --- LOGIN ---
    } else if (cmd == "LOGIN" && m_auth && m_auth->isEnabled()) {
        QStringList parts = args.split(' ');
        if (parts.size() >= 2) {
            QString user = parts[0];
            QString pass = parts[1];
            // strip quotes
            if ((user.startsWith('"') && user.endsWith('"')) ||
                (user.startsWith('\'') && user.endsWith('\'')))
                user = user.mid(1, user.size() - 2);
            if ((pass.startsWith('"') && pass.endsWith('"')) ||
                (pass.startsWith('\'') && pass.endsWith('\'')))
                pass = pass.mid(1, pass.size() - 2);

            if (m_auth->authenticate(user, pass, m_socket->peerAddress())) {
                m_state = AUTHENTICATED;
                m_session.user = user.section('@', 0, 0).toLower().trimmed();
                m_session.mailbox = "INBOX";
                sendTagged(tag, "OK", "LOGIN completed");
            } else {
                sendTagged(tag, "NO", "LOGIN failed");
            }
        } else {
            sendTagged(tag, "BAD", "Syntax: LOGIN user pass");
        }

    // --- AUTHENTICATE PLAIN ---
    } else if (cmd == "AUTHENTICATE" && m_auth && m_auth->isEnabled()) {
        QString method = args.section(' ', 0, 0).toUpper();
        if (method == "PLAIN") {
            // Bare continuation - IMAP spec requires just "+", not "* +"
            m_socket->write("+\r\n");
            m_socket->flush();
            if (m_socket->waitForReadyRead(10000) && m_socket->canReadLine()) {
                QByteArray blob = QByteArray::fromBase64(m_socket->readLine().trimmed());
                int sep1 = blob.indexOf('\0');
                int sep2 = blob.indexOf('\0', sep1 + 1);
                if (sep1 >= 0 && sep2 > sep1) {
                    QString user = QString::fromUtf8(blob.mid(sep1 + 1, sep2 - sep1 - 1));
                    QString pass = QString::fromUtf8(blob.mid(sep2 + 1));
                    if (m_auth->authenticate(user, pass, m_socket->peerAddress())) {
                        m_state = AUTHENTICATED;
                        m_session.user = user.section('@', 0, 0).toLower().trimmed();
                        m_session.mailbox = "INBOX";
                        sendTagged(tag, "OK", "AUTHENTICATE completed");
                    } else {
                        sendTagged(tag, "NO", "AUTHENTICATE failed");
                    }
                }
            }
        } else {
            sendTagged(tag, "BAD", "Unsupported AUTH method");
        }

    // --- Authenticated commands ---
    } else if (m_state != AUTHENTICATED && m_state != SELECTED) {
        sendTagged(tag, "NO", "Authenticate first");

    // --- SELECT / EXAMINE ---
    } else if (cmd == "SELECT" || cmd == "EXAMINE") {
        QString mailbox = args.trimmed();
        // Strip surrounding quotes if present
        if ((mailbox.startsWith('"') && mailbox.endsWith('"')) ||
            (mailbox.startsWith('\'') && mailbox.endsWith('\'')))
            mailbox = mailbox.mid(1, mailbox.size() - 2);

        if (mailbox.toUpper() == "INBOX") {
            m_session.messages = m_storage->listMessages(m_session.user);
            m_session.deleted.clear();
            m_state = SELECTED;
            m_session.mailbox = "INBOX";

            sendUntagged(QString("%1 EXISTS").arg(m_session.messages.size()));
            sendUntagged("0 RECENT");
            sendUntagged("OK [UIDVALIDITY 1] UIDs valid");
            int unseen = 0;
            for (const auto& e : m_session.messages) {
                if (!e.seen) unseen++;
            }
            sendUntagged(QString("OK [UNSEEN %1]").arg(unseen));
            sendTagged(tag, "OK", "SELECT completed");
        } else {
            sendTagged(tag, "NO", "No such mailbox");
        }

    // --- CREATE ---
    } else if (cmd == "CREATE") {
        QString mailbox = args.trimmed();
        if ((mailbox.startsWith('"') && mailbox.endsWith('"')) ||
            (mailbox.startsWith('\'') && mailbox.endsWith('\'')))
            mailbox = mailbox.mid(1, mailbox.size() - 2);

        if (mailbox.toUpper() == "INBOX") {
            sendTagged(tag, "NO", "INBOX already exists");
        } else if (m_storage->createMailbox(m_session.user, mailbox)) {
            sendTagged(tag, "OK", "CREATE completed");
        } else {
            sendTagged(tag, "NO", "CREATE failed");
        }

    // --- LIST ---
    } else if (cmd == "LIST") {
        sendUntagged("LIST () \".\" \"INBOX\"");
        sendTagged(tag, "OK", "LIST completed");

    // --- LSUB ---
    } else if (cmd == "LSUB") {
        sendUntagged("LSUB () \".\" \"INBOX\"");
        sendTagged(tag, "OK", "LSUB completed");

    // --- STATUS ---
    } else if (cmd == "STATUS") {
        QString mailbox = args.section(' ', 0, 0).trimmed();
        if ((mailbox.startsWith('"') && mailbox.endsWith('"')) ||
            (mailbox.startsWith('\'') && mailbox.endsWith('\'')))
            mailbox = mailbox.mid(1, mailbox.size() - 2);
        if (mailbox.toUpper() == "INBOX") {
            auto msgs = m_storage->listMessages(m_session.user);
            sendUntagged("STATUS INBOX (MESSAGES " + QString::number(msgs.size()) + ")");
        }
        sendTagged(tag, "OK", "STATUS completed");

    // --- FETCH ---
    } else if (cmd == "FETCH") {
        int space = args.indexOf(' ');
        if (space < 0) { sendTagged(tag, "BAD", "Syntax"); return; }
        QString seq = args.left(space);
        QString attrs = args.mid(space + 1).toUpper();

        int seqNum = seq.toInt();
        if (seqNum < 1 || seqNum > m_session.messages.size()) {
            sendTagged(tag, "NO", "Invalid sequence number");
            return;
        }

        MailEntry entry = m_session.messages.at(seqNum - 1);
        entry = m_storage->loadMessage(m_session.user, entry.uid);

        if (attrs.contains("BODY[]") || attrs.contains("RFC822")) {
            sendUntagged(QString("%1 FETCH (BODY[] {%2}").arg(seqNum).arg(entry.rawMessage.size()));
            m_socket->write(entry.rawMessage);
            m_socket->write("\r\n)\r\n");
            m_socket->flush();
        } else if (attrs.contains("BODY.PEEK[]")) {
            sendUntagged(QString("%1 FETCH (BODY[] {%2}").arg(seqNum).arg(entry.rawMessage.size()));
            m_socket->write(entry.rawMessage);
            m_socket->write("\r\n)\r\n");
            m_socket->flush();
        } else if (attrs.contains("FLAGS")) {
            QString flags = "()";
            if (entry.seen || entry.flagged)
                flags = QString("(%1%2)").arg(entry.seen ? "\\Seen " : "", entry.flagged ? "\\Flagged " : "").trimmed();
            sendUntagged(QString("%1 FETCH (FLAGS %2)").arg(seqNum).arg(flags));
        } else if (attrs.contains("UID")) {
            sendUntagged(QString("%1 FETCH (UID %2)").arg(seqNum).arg(entry.uid));
        } else {
            sendUntagged(QString("%1 FETCH (UID %2 FLAGS ())").arg(seqNum).arg(entry.uid));
        }
        sendTagged(tag, "OK", "FETCH completed");

    // --- UID FETCH ---
    } else if (cmd == "UID") {
        QString subCmd = args.section(' ', 0, 0).toUpper();
        if (subCmd == "FETCH") {
            QString rest = args.section(' ', 1);
            int space = rest.indexOf(' ');
            int uid = rest.left(space).toInt();
            for (int i = 0; i < m_session.messages.size(); ++i) {
                if (m_session.messages[i].uid == uid) {
                    MailEntry entry = m_storage->loadMessage(m_session.user, uid);
                    sendUntagged(QString("%1 FETCH (UID %2 BODY[] {%3}")
                                     .arg(i + 1).arg(uid).arg(entry.rawMessage.size()));
                    m_socket->write(entry.rawMessage);
                    m_socket->write("\r\n)\r\n");
                    m_socket->flush();
                    break;
                }
            }
        } else if (subCmd == "STORE") {
            int sp2 = args.indexOf(' ', args.indexOf(' ') + 1);
            int uid  = args.mid(args.indexOf(' ') + 1, sp2 - args.indexOf(' ') - 1).toInt();
            QString flags = args.mid(sp2 + 1).trimmed().toUpper();

            for (int i = 0; i < m_session.messages.size(); ++i) {
                if (m_session.messages[i].uid == uid) {
                    if (flags.contains("\\SEEN")) {
                        m_storage->markSeen(m_session.user, uid);
                        m_session.messages[i].seen = true;
                    }
                    if (flags.contains("\\FLAGGED")) {
                        m_storage->markFlagged(m_session.user, uid, true);
                    }
                    break;
                }
            }
        }
        sendTagged(tag, "OK", "UID completed");

    // --- STORE ---
    } else if (cmd == "STORE") {
        int space = args.indexOf(' ');
        int seq = args.left(space).toInt();
        QString flags = args.mid(space + 1).trimmed().toUpper();

        if (seq >= 1 && seq <= m_session.messages.size()) {
            MailEntry& e = m_session.messages[seq - 1];
            if (flags.contains("\\SEEN")) {
                m_storage->markSeen(m_session.user, e.uid);
                e.seen = true;
            }
            if (flags.contains("\\FLAGGED")) {
                m_storage->markFlagged(m_session.user, e.uid, true);
                e.flagged = true;
            }
        }
        sendTagged(tag, "OK", "STORE completed");

    // --- EXPUNGE ---
    } else if (cmd == "EXPUNGE") {
        for (int i = m_session.messages.size() - 1; i >= 0; --i) {
            if (m_session.deleted.value(m_session.messages[i].uid, false)) {
                m_storage->deleteMessage(m_session.user, m_session.messages[i].uid);
                m_session.messages.removeAt(i);
                sendUntagged(QString("%1 EXPUNGE").arg(i + 1));
            }
        }
        m_session.deleted.clear();
        sendTagged(tag, "OK", "EXPUNGE completed");

    // --- NOOP ---
    } else if (cmd == "NOOP") {
        sendTagged(tag, "OK", "NOOP completed");

    // --- CHECK ---
    } else if (cmd == "CHECK") {
        sendTagged(tag, "OK", "CHECK completed");

    } else {
        sendTagged(tag, "BAD", "Unknown command");
    }
}
