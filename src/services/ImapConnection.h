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
    bool handleAuthCommand(const QString& tag, const QString& cmd, const QString& args);
    void handleMailboxCommand(const QString& tag, const QString& cmd, const QString& args);

    void loginSucceeded(const QString& user);
    QString storeKey(const QString& mailbox) const;
    QString normalizeMailbox(const QString& raw) const;
    bool mailboxExists(const QString& mailbox) const;
    void listMailboxes(const QString& tag, const QString& cmd, const QString& args);
    void createMailbox(const QString& tag, const QString& args);
    void deleteMailbox(const QString& tag, const QString& args);
    void renameMailbox(const QString& tag, const QString& args);
    void statusMailbox(const QString& tag, const QString& args);
    void copyMessages(const QString& tag, const QString& args, bool byUid, bool move);
    void removeIndexes(const QList<int>& indexes);
    void selectMailbox(const QString& tag, const QString& cmd, const QString& args);
    void refreshMessages();
    void announceNewMessages();
    int  maxUid() const;
    QList<int> resolveSet(const QString& set, bool byUid) const;

    void fetch(const QString& tag, const QString& args, bool byUid);
    void store(const QString& tag, const QString& args, bool byUid);
    void search(const QString& tag, const QString& args, bool byUid);
    void expunge(bool sendResponses);
    void append(const QString& tag, const QString& args);

    QString flagLetters(int index) const;
    QByteArray imapFlags(int index) const;
    void applyFlags(int index, const QString& letters);
    QByteArray loadCrlf(int index) const;
    bool readLiteral(qint64 size, QByteArray& out);

    enum State { NOT_AUTHENTICATED, AUTHENTICATED, SELECTED };
    struct Session {
        QString user;
        QString mailbox;
        QList<MailEntry> messages;
        bool readOnly = false;
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
