#include "ImapServer.h"
#include "ImapConnection.h"
#include "../storage/MaildirStorage.h"
#include "../auth/AuthManager.h"
#include "../utils/RateLimiter.h"
#include <QDebug>

ImapServer::ImapServer(const ConfigManager& config, bool isImplicitTls,
                       MaildirStorage* storage, AuthManager* auth, QObject* parent)
    : QTcpServer(parent), m_config(config), m_isImplicitTls(isImplicitTls),
      m_storage(storage), m_auth(auth) {
    m_threadPool = QThreadPool::globalInstance();

    auto rl = config.rateLimits();
    if (rl.enabled) {
        m_rateLimiter = new RateLimiter(rl.maxConnectionsPerIp, rl.windowSeconds);
    }
}

ImapServer::~ImapServer() {
    delete m_rateLimiter;
}

bool ImapServer::start() {
    int port = m_isImplicitTls ? m_config.port("imaps") : m_config.port("imap");
    if (port <= 0) port = m_isImplicitTls ? 993 : 143;

    bool success = false;
    for (const auto& iface : m_config.interfaces()) {
        if (listen(iface, port)) {
            qInfo() << "IMAP Server listening on" << iface.toString() << ":" << port
                    << (m_isImplicitTls ? "(TLS)" : "");
            success = true;
        } else {
            qWarning() << "IMAP Server failed on" << iface.toString() << ":" << port
                       << "-" << errorString();
        }
    }
    return success;
}

void ImapServer::incomingConnection(qintptr socketDescriptor) {
    qInfo() << "Incoming IMAP connection. Dispatching to thread pool.";
    auto* conn = new ImapConnection(socketDescriptor, m_storage, m_auth,
                                    m_rateLimiter, m_config.quotaMaxBytes(),
                                    m_config.quotaSoftPercent(),
                                    m_isImplicitTls, m_config.hostname());
    m_threadPool->start(conn);
}
