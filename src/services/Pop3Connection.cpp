#include "Pop3Connection.h"
#include "../storage/MaildirStorage.h"
#include "../auth/AuthManager.h"
#include "../tls/TlsContext.h"
#include "../utils/RateLimiter.h"
#include <QDebug>
#include <QEventLoop>
#include <QThread>
#include <QTimer>

Pop3Connection::Pop3Connection(qintptr socketDescriptor, MaildirStorage* storage,
                               AuthManager* auth, RateLimiter* rateLimiter,
                               qint64 quotaHardBytes, int quotaSoftPercent,
                               bool implicitTls, const QString& hostname)
    : m_socketDescriptor(socketDescriptor), m_socket(nullptr),
      m_storage(storage), m_auth(auth), m_rateLimiter(rateLimiter),
      m_quotaHardBytes(quotaHardBytes), m_quotaSoftPercent(quotaSoftPercent),
      m_implicitTls(implicitTls), m_hostname(hostname) {
    setAutoDelete(true);
}

void Pop3Connection::run() {
    m_socket = new QSslSocket();
    if (!m_socket->setSocketDescriptor(m_socketDescriptor)) {
        qWarning() << "POP3: Failed to set socket descriptor";
        delete m_socket;
        return;
    }

    QHostAddress peer = m_socket->peerAddress();
    if (m_rateLimiter && !m_rateLimiter->allow(peer)) {
        qWarning() << "POP3: Rate-limited from" << peer.toString();
        sendResponse("-ERR Too many connections");
        m_socket->disconnectFromHost();
        delete m_socket;
        return;
    }

    if (m_implicitTls && TlsContext::instance().isConfigured()) {
        qInfo() << "POP3: Starting implicit TLS for" << peer.toString();
        m_socket->setSslConfiguration(TlsContext::instance().getConfiguration());
        m_socket->startServerEncryption();

        QEventLoop loop;
        QTimer::singleShot(10000, &loop, &QEventLoop::quit);
        QObject::connect(m_socket, &QSslSocket::encrypted, &loop, &QEventLoop::quit);
        loop.exec();

        if (!m_socket->isEncrypted()) {
            qWarning() << "POP3: Implicit TLS failed for" << peer.toString()
                       << "-" << m_socket->errorString();
            delete m_socket;
            return;
        }
        qInfo() << "POP3: Implicit TLS established for" << peer.toString();
    }

    qInfo() << "POP3: connection from" << peer.toString();
    sendResponse("+OK " + m_hostname + " POP3 server ready");

    while (m_socket->isOpen()) {
        if (m_socket->waitForReadyRead(300000)) {
            while (m_socket->canReadLine()) {
                QByteArray line = m_socket->readLine(65536).trimmed();
                if (line.isEmpty()) continue;
                handleCommand(QString::fromUtf8(line));
            }
        } else {
            break;
        }
    }

    // --- UPDATE state: expunge deleted messages ---
    if (m_state == TRANSACTION) {
        for (auto it = m_session.deleted.begin(); it != m_session.deleted.end(); ++it) {
            if (it.value()) m_storage->deleteMessage(m_session.user, it.key());
        }
    }

    m_socket->disconnectFromHost();
    delete m_socket;
}

void Pop3Connection::sendResponse(const QString& message) {
    m_socket->write((message + "\r\n").toUtf8());
    m_socket->flush();
}

void Pop3Connection::handleCommand(const QString& line) {
    QString cmd  = line.section(' ', 0, 0).toUpper();
    QString args = line.section(' ', 1);

    if (cmd == "QUIT") {
        if (m_state == TRANSACTION)
            m_state = UPDATE;
        sendResponse("+OK Bye");
        m_socket->disconnectFromHost();
        return;
    }

    if (m_state == AUTHORIZATION) {
        if (cmd == "CAPA") {
            QString caps = "+OK Capability list follows\r\n"
                           "USER\r\n"
                           "PASS\r\n"
                           "UIDL\r\n"
                           "TOP\r\n";
            if (TlsContext::instance().isConfigured() && !m_socket->isEncrypted())
                caps += "STLS\r\n";
            caps += "IMPLEMENTATION MailServer/1.0\r\n.\r\n";
            m_socket->write(caps.toUtf8());
            m_socket->flush();
        } else if (cmd == "STLS") {
            if (TlsContext::instance().isConfigured() && !m_socket->isEncrypted()) {
                sendResponse("+OK Begin TLS");
                m_socket->setSslConfiguration(TlsContext::instance().getConfiguration());
                m_socket->startServerEncryption();
                m_socket->waitForEncrypted(5000);
            } else {
                sendResponse("-ERR TLS not available");
            }
        } else if (cmd == "USER") {
            m_authUser = args.trimmed();
            sendResponse("+OK");
        } else if (cmd == "PASS") {
            QString pass = args.trimmed();
            if (m_auth && m_auth->authenticate(m_authUser, pass, m_socket->peerAddress())) {
                m_state = TRANSACTION;
                m_session.user = m_authUser.section('@', 0, 0).toLower().trimmed();
                m_session.messages = m_storage->listMessages(m_session.user);
                sendResponse("+OK Logged in, " + QString::number(m_session.messages.size()) + " messages");
            } else {
                sendResponse("-ERR Authentication failed");
            }
        } else if (cmd == "APOP") {
            sendResponse("-ERR APOP not supported");
        } else {
            sendResponse("-ERR Unknown command in AUTHORIZATION state");
        }

    } else if (m_state == TRANSACTION) {
        if (cmd == "STAT") {
            int count = 0;
            qint64 size = 0;
            for (const auto& e : m_session.messages) {
                if (!m_session.deleted.value(e.uid, false)) {
                    count++;
                    size += e.size;
                }
            }
            sendResponse(QString("+OK %1 %2").arg(count).arg(size));
        } else if (cmd == "LIST") {
            if (args.isEmpty()) {
                int count = 0;
                qint64 total = 0;
                for (int i = 0; i < m_session.messages.size(); ++i) {
                    if (!m_session.deleted.value(m_session.messages[i].uid, false)) {
                        count++;
                        total += m_session.messages[i].size;
                    }
                }
                sendResponse(QString("+OK %1 messages (%2 octets)").arg(count).arg(total));
                for (int i = 0; i < m_session.messages.size(); ++i) {
                    if (!m_session.deleted.value(m_session.messages[i].uid, false)) {
                        sendResponse(QString("%1 %2").arg(i + 1).arg(m_session.messages[i].size));
                    }
                }
                sendResponse(".");
            } else {
                int idx = args.toInt() - 1;
                if (idx >= 0 && idx < m_session.messages.size()) {
                    if (m_session.deleted.value(m_session.messages[idx].uid, false)) {
                        sendResponse("-ERR Message deleted");
                    } else {
                        sendResponse(QString("+OK %1 %2").arg(idx + 1).arg(m_session.messages[idx].size));
                    }
                } else {
                    sendResponse("-ERR No such message");
                }
            }
        } else if (cmd == "RETR") {
            int idx = args.toInt() - 1;
            if (idx >= 0 && idx < m_session.messages.size()) {
                if (m_session.deleted.value(m_session.messages[idx].uid, false)) {
                    sendResponse("-ERR Message deleted");
                } else {
                    MailEntry entry = m_storage->loadMessage(m_session.user, m_session.messages[idx].uid);
                    sendResponse("+OK " + QString::number(entry.rawMessage.size()) + " octets");
                    m_socket->write(entry.rawMessage);
                    if (!entry.rawMessage.endsWith("\r\n"))
                        m_socket->write("\r\n");
                    sendResponse(".");
                }
            } else {
                sendResponse("-ERR No such message");
            }
        } else if (cmd == "DELE") {
            int idx = args.toInt() - 1;
            if (idx >= 0 && idx < m_session.messages.size()) {
                m_session.deleted[m_session.messages[idx].uid] = true;
                sendResponse("+OK Message marked for deletion");
            } else {
                sendResponse("-ERR No such message");
            }
        } else if (cmd == "NOOP") {
            sendResponse("+OK");
        } else if (cmd == "RSET") {
            m_session.deleted.clear();
            sendResponse("+OK Reset");
        } else if (cmd == "TOP") {
            QStringList parts = args.split(' ');
            int idx = parts[0].toInt() - 1;
            int n   = parts.size() >= 2 ? parts[1].toInt() : 0;
            if (idx >= 0 && idx < m_session.messages.size()) {
                MailEntry entry = m_storage->loadMessage(m_session.user, m_session.messages[idx].uid);
                // Return headers + first n lines of body
                QByteArray data = entry.rawMessage;
                int bodySep = data.indexOf("\r\n\r\n");
                if (bodySep < 0) bodySep = data.indexOf("\n\n");
                QByteArray headers = (bodySep >= 0) ? data.left(bodySep) : data;
                sendResponse("+OK Top of message");
                m_socket->write(headers);
                if (n > 0 && bodySep >= 0) {
                    m_socket->write("\r\n\r\n");
                    QByteArray body = data.mid(bodySep + 4);
                    QList<QByteArray> bodyLines = body.split('\n');
                    for (int i = 0; i < qMin(n, bodyLines.size()); ++i) {
                        m_socket->write(bodyLines[i] + "\r\n");
                    }
                }
                sendResponse(".");
            } else {
                sendResponse("-ERR No such message");
            }
        } else if (cmd == "UIDL") {
            if (args.isEmpty()) {
                sendResponse("+OK UID listing");
                for (int i = 0; i < m_session.messages.size(); ++i) {
                    if (!m_session.deleted.value(m_session.messages[i].uid, false))
                        sendResponse(QString("%1 %2").arg(i + 1).arg(m_session.messages[i].uid));
                }
                sendResponse(".");
            } else {
                int idx = args.toInt() - 1;
                if (idx >= 0 && idx < m_session.messages.size()) {
                    if (m_session.deleted.value(m_session.messages[idx].uid, false)) {
                        sendResponse("-ERR Message deleted");
                    } else {
                        sendResponse(QString("+OK %1 %2").arg(idx + 1).arg(m_session.messages[idx].uid));
                    }
                } else {
                    sendResponse("-ERR No such message");
                }
            }
        } else {
            sendResponse("-ERR Unknown command");
        }
    }
}
