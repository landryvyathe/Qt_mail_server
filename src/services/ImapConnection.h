#ifndef IMAPCONNECTION_H
#define IMAPCONNECTION_H

#include <QRunnable>
#include <QSslSocket>
#include <QHash>
#include <QList>

class MaildirStorage;
class AuthManager;
class RateLimiter;
struct MailEntry;

class ImapConnection : public QRunnable {
public:
    ImapConnection(qintptr socketDescriptor, MaildirStorage* storage,
                   AuthManager* auth, RateLimiter* rateLimiter,
                   qint64 quotaHardBytes, int quotaSoftPercent,
                   bool implicitTls, const QString& hostname);
    void run() override;

private:
    void sendTagged(const QString& tag, const QString& status, const QString& text);
    void sendUntagged(const QString& text);
    void handleCommand(const QString& tag, const QString& cmd, const QString& args);

    enum State { NOT_AUTHENTICATED, AUTHENTICATED, SELECTED };
    struct Session {
        QString user;
        QString mailbox;
        QList<MailEntry> messages;
        int nextUid = 1;
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
    State         m_state = NOT_AUTHENTICATED;
    Session       m_session;
};

#endif // IMAPCONNECTION_H
