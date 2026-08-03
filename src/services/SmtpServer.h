#ifndef SMTPSERVER_H
#define SMTPSERVER_H

#include <QTcpServer>
#include <QThreadPool>
#include "../ConfigManager.h"

class MaildirStorage;
class AuthManager;
class RateLimiter;
class QueueManager;

class SmtpServer : public QTcpServer {
    Q_OBJECT
public:
    SmtpServer(const ConfigManager& config, bool isImplicitTls,
               MaildirStorage* storage, AuthManager* auth,
               QueueManager* queueMgr, QObject* parent = nullptr);
    ~SmtpServer();
    bool start();

protected:
    void incomingConnection(qintptr socketDescriptor) override;

private:
    const ConfigManager& m_config;
    bool m_isImplicitTls;
    MaildirStorage* m_storage;
    AuthManager*    m_auth;
    QueueManager*   m_queueMgr;
    RateLimiter*    m_rateLimiter;
    QThreadPool*    m_threadPool;
    QString         m_hostname;
    QStringList     m_localDomains;
};

#endif // SMTPSERVER_H
