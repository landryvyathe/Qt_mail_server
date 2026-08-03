#ifndef SMTPCONNECTION_H
#define SMTPCONNECTION_H

#include <QRunnable>
#include <QSslSocket>
#include <QStringList>
#include <QByteArray>
#include <QHostAddress>

class MaildirStorage;
class AuthManager;
class RateLimiter;
class QueueManager;

class SmtpConnection : public QRunnable {
public:
    SmtpConnection(qintptr socketDescriptor,
                   MaildirStorage* storage,
                   AuthManager* auth,
                   RateLimiter* rateLimiter,
                   QueueManager* queueMgr,
                   const QStringList& localDomains,
                   qint64 quotaHardBytes,
                   const QString& hostname,
                   bool implicitTls = false,
                   bool catchAllEnabled = true);
    void run() override;

private:
    void sendResponse(int code, const QString& message);
    void handleCommand(const QString& commandLine);
    bool isLocalDomain(const QString& email) const;

    qintptr       m_socketDescriptor;
    QSslSocket*   m_socket;
    MaildirStorage* m_storage;
    AuthManager*    m_auth;
    RateLimiter*    m_rateLimiter;
    QueueManager*   m_queueMgr;
    QStringList     m_localDomains;
    qint64        m_quotaHardBytes;
    QString       m_hostname;
    bool          m_implicitTls;
    bool          m_catchAllEnabled;

    enum State { INIT, HELO_EHLO, MAIL_FROM, RCPT_TO, AUTH_USER, AUTH_PASS,
                 AUTH_PLAIN_DATA, DATA_HEADERS, DATA_BODY, QUIT };
    State m_state;

    QString     m_sender;
    QStringList m_recipients;
    QByteArray  m_messageData;
    QString     m_authUser;
    bool        m_authenticated = false;
};

#endif // SMTPCONNECTION_H
