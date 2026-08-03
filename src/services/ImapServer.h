#ifndef IMAPSERVER_H
#define IMAPSERVER_H

#include <QTcpServer>
#include <QThreadPool>
#include "../ConfigManager.h"

class MaildirStorage;
class AuthManager;
class RateLimiter;

class ImapServer : public QTcpServer {
    Q_OBJECT
public:
    ImapServer(const ConfigManager& config, bool isImplicitTls,
               MaildirStorage* storage, AuthManager* auth, QObject* parent = nullptr);
    ~ImapServer();
    bool start();
    void setRateLimiter(RateLimiter* rl) { m_rateLimiter = rl; }

protected:
    void incomingConnection(qintptr socketDescriptor) override;

private:
    const ConfigManager& m_config;
    bool m_isImplicitTls;
    MaildirStorage* m_storage;
    AuthManager*    m_auth;
    RateLimiter*    m_rateLimiter = nullptr;
    QThreadPool*    m_threadPool;
};

#endif // IMAPSERVER_H
