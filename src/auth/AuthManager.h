#ifndef AUTHMANAGER_H
#define AUTHMANAGER_H

#include <QObject>
#include <QHostAddress>
#include "UserRepository.h"

class RateLimiter;

class AuthManager : public QObject {
    Q_OBJECT
public:
    explicit AuthManager(QObject* parent = nullptr);
    ~AuthManager();

    bool init(const QString& host, int port, const QString& dbName,
              const QString& user, const QString& password);

    // Verify credentials with rate limiting
    bool authenticate(const QString& email, const QString& password,
                      const QHostAddress& clientIp);

    bool userExists(const QString& email);
    bool createUser(const QString& email, const QString& password);

    bool isEnabled() const { return m_initialized; }

    // Check if this IP is allowed to even attempt auth
    bool canAttemptAuth(const QHostAddress& ip);

private:
    UserRepository m_repo;
    RateLimiter*   m_rateLimiter;
    bool m_initialized = false;
    int  m_maxAuthAttempts;
    int  m_authWindowSecs;
};

#endif // AUTHMANAGER_H
