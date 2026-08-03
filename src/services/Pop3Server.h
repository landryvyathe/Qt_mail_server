#ifndef POP3SERVER_H
#define POP3SERVER_H

#include <QTcpServer>
#include <QThreadPool>
#include "../ConfigManager.h"

class MaildirStorage;
class AuthManager;
class RateLimiter;

class Pop3Server : public QTcpServer {
    Q_OBJECT
public:
    Pop3Server(const ConfigManager& config, bool isImplicitTls,
               MaildirStorage* storage, AuthManager* auth, QObject* parent = nullptr);
    ~Pop3Server();
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

#endif // POP3SERVER_H
