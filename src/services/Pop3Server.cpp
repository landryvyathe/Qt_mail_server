#include "Pop3Server.h"
#include "Pop3Connection.h"
#include "../storage/MaildirStorage.h"
#include "../auth/AuthManager.h"
#include "../utils/RateLimiter.h"
#include <QDebug>

Pop3Server::Pop3Server(const ConfigManager& config, bool isImplicitTls,
                       MaildirStorage* storage, AuthManager* auth, QObject* parent)
    : QTcpServer(parent), m_config(config), m_isImplicitTls(isImplicitTls),
      m_storage(storage), m_auth(auth) {
    m_threadPool = QThreadPool::globalInstance();

    auto rl = config.rateLimits();
    if (rl.enabled) {
        m_rateLimiter = new RateLimiter(rl.maxConnectionsPerIp, rl.windowSeconds);
    }
}

Pop3Server::~Pop3Server() {
    delete m_rateLimiter;
}

bool Pop3Server::start() {
    int port = m_isImplicitTls ? m_config.port("pop3s") : m_config.port("pop3");
    if (port <= 0) port = m_isImplicitTls ? 995 : 110;

    bool success = false;
    for (const auto& iface : m_config.interfaces()) {
        if (listen(iface, port)) {
            qInfo() << "POP3 Server listening on" << iface.toString() << ":" << port
                    << (m_isImplicitTls ? "(TLS)" : "");
            success = true;
        } else {
            qWarning() << "POP3 Server failed on" << iface.toString() << ":" << port
                       << "-" << errorString();
        }
    }
    return success;
}

void Pop3Server::incomingConnection(qintptr socketDescriptor) {
    qInfo() << "Incoming POP3 connection. Dispatching to thread pool.";
    auto* conn = new Pop3Connection(socketDescriptor, m_storage, m_auth,
                                    m_rateLimiter, m_config.quotaMaxBytes(),
                                    m_config.quotaSoftPercent(),
                                    m_isImplicitTls, m_config.hostname());
    m_threadPool->start(conn);
}
