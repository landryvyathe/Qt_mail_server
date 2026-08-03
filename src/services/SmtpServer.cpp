#include "SmtpServer.h"
#include "SmtpConnection.h"
#include "../storage/MaildirStorage.h"
#include "../auth/AuthManager.h"
#include "../utils/RateLimiter.h"
#include <QDebug>

SmtpServer::SmtpServer(const ConfigManager& config, bool isImplicitTls,
                       MaildirStorage* storage, AuthManager* auth,
                       QueueManager* queueMgr, QObject* parent)
    : QTcpServer(parent), m_config(config), m_isImplicitTls(isImplicitTls),
      m_storage(storage), m_auth(auth), m_queueMgr(queueMgr), m_rateLimiter(nullptr) {

    m_threadPool = QThreadPool::globalInstance();
    m_hostname = config.hostname();
    if (m_hostname.isEmpty()) m_hostname = "localhost";
    m_localDomains = config.localDomains();

    auto rl = config.rateLimits();
    if (rl.enabled) {
        m_rateLimiter = new RateLimiter(rl.maxConnectionsPerIp, rl.windowSeconds);
    }
}

SmtpServer::~SmtpServer() {
    delete m_rateLimiter;
}

bool SmtpServer::start() {
    for (const auto& interface : m_config.interfaces()) {
        if (m_isImplicitTls) {
            // Port 465 (SMTPS) — implicit TLS
            int port465 = m_config.port("smtps");
            if (port465 <= 0) port465 = 465;
            if (listen(interface, port465)) {
                qInfo() << "SMTPS Server listening on" << interface.toString() << ":" << port465;
            } else {
                qWarning() << "SMTPS Server failed on" << interface.toString() << ":" << port465
                           << "-" << errorString();
            }
        } else {
            // Port 25 (SMTP)
            int port25 = m_config.port("smtp");
            if (port25 <= 0) port25 = 25;
            if (listen(interface, port25)) {
                qInfo() << "SMTP Server listening on" << interface.toString() << ":" << port25;
            } else {
                qWarning() << "SMTP Server failed on" << interface.toString() << ":" << port25
                           << "-" << errorString();
            }

            // Port 587 (submission)
            int port587 = m_config.port("submission");
            if (port587 > 0) {
                SmtpServer* sub = new SmtpServer(m_config, false, m_storage, m_auth, m_queueMgr, parent());
                if (sub->listen(interface, port587)) {
                    qInfo() << "SMTP Submission listening on" << interface.toString() << ":" << port587;
                }
            }
        }
    }
    return isListening();
}

void SmtpServer::incomingConnection(qintptr socketDescriptor) {
    qInfo() << "Incoming SMTP connection. Dispatching to thread pool.";
    SmtpConnection* connection = new SmtpConnection(
        socketDescriptor, m_storage, m_auth, m_rateLimiter,
        m_queueMgr, m_localDomains,
        m_config.quotaMaxBytes(), m_hostname, m_isImplicitTls,
        m_config.getRawConfig().value("catch_all_enabled").toBool(true));
    m_threadPool->start(connection);
}
