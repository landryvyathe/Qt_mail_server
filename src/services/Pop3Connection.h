#ifndef POP3CONNECTION_H
#define POP3CONNECTION_H

#include <QRunnable>
#include <QSslSocket>
#include <QList>

class MaildirStorage;
class AuthManager;
class RateLimiter;
struct MailEntry;

class Pop3Connection : public QRunnable {
public:
    Pop3Connection(qintptr socketDescriptor, MaildirStorage* storage,
                   AuthManager* auth, RateLimiter* rateLimiter,
                   qint64 quotaHardBytes, int quotaSoftPercent,
                   bool implicitTls, const QString& hostname);
    void run() override;

private:
    void sendResponse(const QString& message);
    void handleCommand(const QString& commandLine);

    enum State { AUTHORIZATION, TRANSACTION, UPDATE };
    struct Session {
        QString user;
        QList<MailEntry> messages;
        QHash<int, bool> deleted;
    };

    qintptr       m_socketDescriptor;
    QSslSocket*   m_socket;
    MaildirStorage* m_storage;
    AuthManager*    m_auth;
    RateLimiter*    m_rateLimiter;
    qint64        m_quotaHardBytes;
    int           m_quotaSoftPercent;
    bool          m_implicitTls;
    QString       m_hostname;
    State         m_state = AUTHORIZATION;
    Session       m_session;
    QString       m_authUser;
};

#endif // POP3CONNECTION_H
