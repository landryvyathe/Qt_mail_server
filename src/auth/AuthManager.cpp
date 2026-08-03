                                    #include "AuthManager.h"
#include "../utils/RateLimiter.h"
#include <QDebug>

AuthManager::AuthManager(QObject* parent)
    : QObject(parent), m_rateLimiter(nullptr),
      m_maxAuthAttempts(5), m_authWindowSecs(60) {
}

AuthManager::~AuthManager() {
    delete m_rateLimiter;
}

bool AuthManager::init(const QString& host, int port, const QString& dbName,
                        const QString& user, const QString& password) {
    if (!m_repo.init(host, port, dbName, user, password)) {
        return false;
    }

    m_rateLimiter  = new RateLimiter(m_maxAuthAttempts, m_authWindowSecs);
    m_initialized  = true;
    qInfo() << "AuthManager: initialized with rate limiting"
            << m_maxAuthAttempts << "attempts per" << m_authWindowSecs << "s";
    return true;
}

bool AuthManager::canAttemptAuth(const QHostAddress& ip) {
    if (!m_rateLimiter) return true;
    return m_rateLimiter->allowAuthAttempt(ip);
}

bool AuthManager::authenticate(const QString& email, const QString& password,
                                const QHostAddress& clientIp) {
    if (!m_initialized) return false;

    // Rate-limit auth attempts
    if (!canAttemptAuth(clientIp)) {
        qWarning() << "AuthManager: rate-limited auth attempt from" << clientIp.toString()
                   << "for" << email;
        return false;
    }

    bool ok = m_repo.verifyCredentials(email, password);

    if (ok) {
        // Reset rate limit counter on successful auth
        if (m_rateLimiter) m_rateLimiter->reset(clientIp);
        qInfo() << "AuthManager: authenticated" << email << "from" << clientIp.toString();
    } else {
        qWarning() << "AuthManager: auth failed for" << email << "from" << clientIp.toString();
    }

    return ok;
}

bool AuthManager::userExists(const QString& email) {
    if (!m_initialized) return false;
    return m_repo.userExists(email);
}

bool AuthManager::createUser(const QString& email, const QString& password) {
    if (!m_initialized) return false;
    return m_repo.createUser(email, password);
}
