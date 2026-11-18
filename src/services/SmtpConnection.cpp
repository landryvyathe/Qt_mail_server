#include "SmtpConnection.h"
#include "../storage/MaildirStorage.h"
#include "../auth/AuthManager.h"
#include "../tls/TlsContext.h"
#include "../utils/RateLimiter.h"
#include "../queue/QueueManager.h"
#include "../storage/UserSettings.h"
#include <QDebug>
#include <QEventLoop>
#include <QThread>
#include <QTimer>

SmtpConnection::SmtpConnection(qintptr socketDescriptor,
                               MaildirStorage* storage,
                               AuthManager* auth,
                               RateLimiter* rateLimiter,
                               QueueManager* queueMgr,
                               const QStringList& localDomains,
                               qint64 quotaHardBytes,
                               const QString& hostname,
                               bool implicitTls,
                               bool catchAllEnabled)
    : m_socketDescriptor(socketDescriptor), m_socket(nullptr),
      m_storage(storage), m_auth(auth), m_rateLimiter(rateLimiter),
      m_queueMgr(queueMgr), m_localDomains(localDomains),
      m_quotaHardBytes(quotaHardBytes), m_hostname(hostname),
      m_state(INIT), m_implicitTls(implicitTls),
      m_catchAllEnabled(catchAllEnabled) {
    setAutoDelete(true);
}

void SmtpConnection::run() {
    m_socket = new QSslSocket();
    if (!m_socket->setSocketDescriptor(m_socketDescriptor)) {
        qWarning() << "SMTP: Failed to set socket descriptor.";
        delete m_socket;
        return;
    }

    // --- Rate limiting check ---
    QHostAddress peer = m_socket->peerAddress();
    if (m_rateLimiter && !m_rateLimiter->allow(peer)) {
        qWarning() << "SMTP: Rate-limited connection from" << peer.toString();
        sendResponse(421, "Service temporarily unavailable – too many connections");
        m_socket->disconnectFromHost();
        delete m_socket;
        return;
    }

    // --- Implicit TLS for SMTPS (port 465) ---
    if (m_implicitTls) {
        if (TlsContext::instance().isConfigured()) {
            qInfo() << "SMTP: Starting implicit TLS for" << peer.toString();
            m_socket->setSslConfiguration(TlsContext::instance().getConfiguration());
            m_socket->startServerEncryption();

            QEventLoop loop;
            QTimer::singleShot(10000, &loop, &QEventLoop::quit);
            QObject::connect(m_socket, &QSslSocket::encrypted, &loop, &QEventLoop::quit);
            loop.exec();

            if (!m_socket->isEncrypted()) {
                qWarning() << "SMTP: Implicit TLS failed for" << peer.toString()
                           << "-" << m_socket->errorString();
                m_socket->disconnectFromHost();
                delete m_socket;
                return;
            }
            qInfo() << "SMTP: implicit TLS established from" << peer.toString();
        } else {
            qWarning() << "SMTP: Implicit TLS requested but TLS not configured";
            sendResponse(421, "TLS not configured");
            m_socket->disconnectFromHost();
            delete m_socket;
            return;
        }
    }

    qInfo() << "SMTP: connection from" << peer.toString();
    sendResponse(220, m_hostname + " ESMTP MailServer/1.0");

    while (m_socket->isOpen() && m_socket->state() == QAbstractSocket::ConnectedState) {
        if (m_socket->waitForReadyRead(300000)) {
            while (m_socket->canReadLine()) {
                QByteArray rawLine = m_socket->readLine(1048576);
                if (rawLine.isEmpty()) continue;
                if (!rawLine.endsWith('\n') && rawLine.size() == 1048576) {
                    sendResponse(500, "5.5.2 Line too long");
                    m_socket->disconnectFromHost();
                    break;
                }
                // Message data must stay byte-exact: leading whitespace marks folded headers.
                QByteArray dataLine = rawLine;
                while (dataLine.endsWith('\n') || dataLine.endsWith('\r')) dataLine.chop(1);
                QString line = m_state == DATA_BODY ? QString::fromLatin1(dataLine)
                                                    : QString::fromUtf8(rawLine).trimmed();
                if (m_state == DATA_BODY) {
                    if (line == ".") {
                        // End of DATA – separate local vs remote recipients
                        QStringList remoteRcpt;
                        int storedCount = 0;

                        for (const QString& rcpt : m_recipients) {
                            QString rcptDomain = rcpt.section('@', -1).toLower().trimmed();
                            bool isLocal = m_localDomains.contains(rcptDomain)
                                           || rcptDomain == "localhost"
                                           || rcptDomain == "127.0.0.1";

                            if (isLocal && m_storage) {
                                QString user = rcpt.section('@', 0, 0).toLower().trimmed();
                                qint64 used = m_storage->usageBytes(user);
                                if (used + m_messageData.size() > m_quotaHardBytes) {
                                    qWarning() << "SMTP: Over quota for" << user;
                                    continue;
                                }
                                QByteArray storedMsg = "Received: from " + m_sender.toLatin1()
                                    + " by " + m_hostname.toLatin1() + " (MailServer/1.0)\r\n"
                                    + m_messageData;
                                int uid = m_storage->storeMessage(user, storedMsg);
                                if (uid > 0) storedCount++;
                                if (uid > 0 && m_queueMgr && !m_sender.isEmpty()) {
                                    const QByteArray reply = UserSettings::autoReplyFor(
                                        m_storage->basePath(), user, rcpt.toLower().trimmed(),
                                        m_sender, m_messageData, m_hostname);
                                    if (!reply.isEmpty())
                                        m_queueMgr->spoolMessage(reply, rcpt.toLower().trimmed(), {m_sender});
                                }
                            } else {
                                // Remote recipient — route through outbound queue
                                remoteRcpt << rcpt;
                            }
                        }

                        // Spool remote recipients via QueueManager
                        if (!remoteRcpt.isEmpty() && m_queueMgr) {
                            m_queueMgr->spoolMessage(m_messageData, m_sender, remoteRcpt);
                            qInfo() << "SMTP: spooled" << remoteRcpt.size()
                                    << "remote recipient(s) for outbound delivery";
                        }

                        sendResponse(250, QString("2.0.0 Ok: stored %1 messages, queued %2 for relay")
                                     .arg(storedCount).arg(remoteRcpt.size()));
                        m_state = HELO_EHLO;
                        m_sender.clear();
                        m_recipients.clear();
                        m_messageData.clear();
                    } else {
                        if (dataLine.startsWith('.')) dataLine.remove(0, 1);
                        m_messageData.append(dataLine + "\r\n");
                    }
                } else {
                    handleCommand(line);
                }
            }
        } else {
            break;
        }
    }

    qInfo() << "SMTP: connection closed from" << peer.toString();
    m_socket->disconnectFromHost();
    if (m_socket->state() != QAbstractSocket::UnconnectedState)
        m_socket->waitForDisconnected();
    delete m_socket;
}

void SmtpConnection::sendResponse(int code, const QString& message) {
    m_socket->write(QString("%1 %2\r\n").arg(code).arg(message).toUtf8());
    m_socket->flush();
}

void SmtpConnection::handleCommand(const QString& commandLine) {
    if (commandLine.isEmpty()) return;

    QString cmd  = commandLine.section(' ', 0, 0).toUpper();
    QString args = commandLine.section(' ', 1);

    // --- EHLO / HELO ---
    if (cmd == "EHLO" || cmd == "HELO") {
        if (cmd == "EHLO") {
            QString resp = "250-" + m_hostname + "\r\n"
                           "250-PIPELINING\r\n"
                           "250-8BITMIME\r\n";
            if (m_auth && m_auth->isEnabled() && !m_authenticated && m_socket->isEncrypted())
                resp += "250-AUTH LOGIN PLAIN\r\n";
            if (TlsContext::instance().isConfigured() && !m_socket->isEncrypted())
                resp += "250-STARTTLS\r\n";
            resp += "250 SIZE\r\n";
            m_socket->write(resp.toUtf8());
            m_socket->flush();
        } else {
            sendResponse(250, m_hostname);
        }
        m_state = HELO_EHLO;

    // --- STARTTLS ---
    } else if (cmd == "STARTTLS") {
        if (TlsContext::instance().isConfigured() && !m_socket->isEncrypted()) {
            sendResponse(220, "2.0.0 Ready to start TLS");
            m_socket->setSslConfiguration(TlsContext::instance().getConfiguration());
            m_socket->startServerEncryption();

            QEventLoop tlsLoop;
            QTimer::singleShot(5000, &tlsLoop, &QEventLoop::quit);
            QObject::connect(m_socket, &QSslSocket::encrypted, &tlsLoop, &QEventLoop::quit);
            tlsLoop.exec();

            if (!m_socket->isEncrypted()) {
                qWarning() << "SMTP: TLS negotiation failed:" << m_socket->errorString();
                m_socket->disconnectFromHost();
            } else {
                qInfo() << "SMTP: TLS established";
                m_state = INIT;
            }
        } else {
            sendResponse(502, "5.5.1 Command not implemented");
        }

    // --- AUTH LOGIN / PLAIN ---
    } else if (cmd == "AUTH" && m_auth && m_auth->isEnabled() && !m_socket->isEncrypted()) {
        sendResponse(538, "5.7.11 Encryption required for requested authentication mechanism");
    } else if (cmd == "AUTH" && m_auth && m_auth->isEnabled()) {
        QString method = args.section(' ', 0, 0).toUpper();
        if (method == "LOGIN") {
            sendResponse(334, "VXNlcm5hbWU6"); // "Username:"
            m_state = AUTH_USER;
        } else if (method == "PLAIN") {
            // AUTH PLAIN <base64("\0username\0password")>
            QString payload = args.section(' ', 1).trimmed();
            if (payload.isEmpty()) {
                // Client may send empty initial response, ask for it
                sendResponse(334, "");
                m_state = AUTH_PLAIN_DATA;
            } else {
                QByteArray decoded = QByteArray::fromBase64(payload.toUtf8());
                QStringList parts = QString::fromUtf8(decoded).split(QChar('\0'));
                if (parts.size() >= 3) {
                    QString authzid = parts[0];
                    QString username = parts[1];
                    QString password = parts[2];
                    Q_UNUSED(authzid);
                    QHostAddress peer = m_socket->peerAddress();
                    if (m_auth->authenticate(username, password, peer)) {
                        sendResponse(235, "2.7.0 Authentication successful");
                        m_authenticated = true;
                        m_authUser = username;
                    } else {
                        sendResponse(535, "5.7.8 Authentication credentials invalid");
                    }
                } else {
                    sendResponse(501, "5.5.4 Invalid AUTH PLAIN parameter");
                }
                m_state = HELO_EHLO;
                return;
            }
        } else {
            sendResponse(504, "5.7.4 Unrecognized authentication type");
        }
    } else if (m_state == AUTH_PLAIN_DATA) {
        QByteArray decoded = QByteArray::fromBase64(commandLine.toUtf8());
        QStringList parts = QString::fromUtf8(decoded).split(QChar('\0'));
        if (parts.size() >= 3) {
            QHostAddress peer = m_socket->peerAddress();
            if (m_auth->authenticate(parts[1], parts[2], peer)) {
                sendResponse(235, "2.7.0 Authentication successful");
                m_authenticated = true;
                m_authUser = parts[1];
            } else {
                sendResponse(535, "5.7.8 Authentication credentials invalid");
            }
        } else {
            sendResponse(501, "5.5.4 Invalid AUTH PLAIN parameter");
        }
        m_state = HELO_EHLO;
    } else if (m_state == AUTH_USER) {
        m_authUser = QString::fromUtf8(QByteArray::fromBase64(commandLine.toUtf8()));
        sendResponse(334, "UGFzc3dvcmQ6"); // "Password:"
        m_state = AUTH_PASS;
    } else if (m_state == AUTH_PASS) {
        QHostAddress peer = m_socket->peerAddress();
        QString pass = QString::fromUtf8(QByteArray::fromBase64(commandLine.toUtf8()));
        if (m_auth->authenticate(m_authUser, pass, peer)) {
            sendResponse(235, "2.7.0 Authentication successful");
            m_authenticated = true;
        } else {
            sendResponse(535, "5.7.8 Authentication credentials invalid");
        }
        m_state = HELO_EHLO;

    // --- MAIL FROM ---
    } else if (cmd == "MAIL") {
        if (args.toUpper().startsWith("FROM:")) {
            m_sender = args.mid(5).trimmed();
            // Extract address from angle brackets, strip ESMTP params (SIZE=, BODY=, etc.)
            if (m_sender.startsWith('<')) {
                int closePos = m_sender.indexOf('>');
                if (closePos > 1) {
                    m_sender = m_sender.mid(1, closePos - 1);
                }
            }
            if (m_authenticated && m_sender.compare(m_authUser.trimmed(), Qt::CaseInsensitive) != 0) {
                sendResponse(553, "5.7.1 Sender address does not match authenticated user");
                return;
            }
            if (!m_authenticated && isLocalDomain(m_sender)) {
                sendResponse(530, "5.7.0 Authentication required to send as a local address");
                return;
            }
            sendResponse(250, "2.1.0 Ok");
            m_state = MAIL_FROM;
        } else {
            sendResponse(501, "5.5.4 Syntax error");
        }

    // --- RCPT TO ---
    } else if (cmd == "RCPT") {
        if (args.toUpper().startsWith("TO:")) {
            QString rcpt = args.mid(3).trimmed();
            if (rcpt.startsWith('<') && rcpt.endsWith('>'))
                rcpt = rcpt.mid(1, rcpt.size() - 2);

            if (!isLocalDomain(rcpt) && !m_authenticated) {
                qWarning() << "SMTP: relay denied to" << rcpt << "from" << m_socket->peerAddress().toString();
                sendResponse(554, "5.7.1 Relay access denied");
                return;
            }

            // Check if recipient is local — if catch-all is off, reject unknown users
            if (isLocalDomain(rcpt) && !m_catchAllEnabled) {
                if (!m_auth || !m_auth->userExists(rcpt)) {
                    sendResponse(550, "5.1.1 No such user");
                    return;
                }
            }

            // Quota check (configurable path)
            const QString mailbox = rcpt.section('@', 0, 0).toLower();
            qint64 used = (m_storage && isLocalDomain(rcpt)) ? m_storage->usageBytes(mailbox) : 0;
            if (used >= m_quotaHardBytes) {
                sendResponse(550, "5.2.2 Mailbox full");
            } else {
                m_recipients.append(rcpt);
                sendResponse(250, "2.1.5 Ok");
                m_state = RCPT_TO;
            }
        } else {
            sendResponse(501, "5.5.4 Syntax error");
        }

    // --- DATA ---
    } else if (cmd == "DATA") {
        if (m_recipients.isEmpty()) {
            sendResponse(554, "5.5.1 No valid recipients");
        } else {
            sendResponse(354, "End data with <CR><LF>.<CR><LF>");
            m_state = DATA_BODY;
        }

    // --- QUIT ---
    } else if (cmd == "QUIT") {
        sendResponse(221, "2.0.0 Bye");
        m_socket->disconnectFromHost();

    // --- RSET ---
    } else if (cmd == "RSET") {
        m_sender.clear();
        m_recipients.clear();
        m_messageData.clear();
        m_state = HELO_EHLO;
        sendResponse(250, "2.0.0 Ok");

    // --- NOOP ---
    } else if (cmd == "NOOP") {
        sendResponse(250, "2.0.0 Ok");

    } else {
        sendResponse(502, "5.5.2 Command not recognized");
    }
}

bool SmtpConnection::isLocalDomain(const QString& email) const {
    int atPos = email.indexOf('@');
    if (atPos < 0) return false;
    QString domain = email.mid(atPos + 1).toLower();
    return m_localDomains.contains(domain);
}
