#ifndef OUTBOUNDWORKER_H
#define OUTBOUNDWORKER_H

#include <QObject>
#include <QTimer>
#include <QSslSocket>
#include <QQueue>
#include "../ConfigManager.h"

struct OutboundEmail {
    QString filePath;
    QString envelopeFrom;
    QStringList envelopeRcpt;
    QByteArray rawData;
    int retryCount = 0;
    qint64  nextAttemptAt = 0;
};

class OutboundWorker : public QObject {
    Q_OBJECT
public:
    explicit OutboundWorker(const ConfigManager& config, const QString& configFilePath, QObject *parent = nullptr);
    ~OutboundWorker();
    void start();
    void stop();

signals:
    void logMessage(const QString& msg);

private slots:
    void processQueue();

private:
    // -- MX lookup --
    QByteArray resolveMX(const QString& domain);

    // -- Delivery strategies --
    struct SmtpDeliveryResult {
        bool    temporary = false;
        QString errorText;
    };
    SmtpDeliveryResult deliverDirect(OutboundEmail& email);
    SmtpDeliveryResult deliverViaRelay(OutboundEmail& email);

    // -- SMTP phase helpers --
    bool smtpReadResponse(QSslSocket* s, int& code, QString& text, int timeoutMs = 30000);
    bool smtpSendCommand(QSslSocket* s, const QByteArray& cmd, int expectedCode, int& code, QString& text);

    // -- AUTH helpers --
    bool smtpAuthLogin(QSslSocket* s, const QString& user, const QString& pass, int& code, QString& text);
    bool smtpAuthPlain(QSslSocket* s, const QString& user, const QString& pass, int& code, QString& text);

    // -- Queue management --
    void loadPendingFromDisk();
    void markDelivered(const OutboundEmail& email);
    void markDeferred(OutboundEmail& email, const QString& reason);
    void markFailed(const OutboundEmail& email);

    // -- DKIM signing --
    QByteArray signDkim(const QByteArray& rawEmail);

    // Re-reads sender_passwords from disk so an address registered with DirectMail after
    // startup (e.g. by auto-provisioning on account creation) can send without a restart.
    QHash<QString, QString> liveSenderPasswords() const;

    const ConfigManager& m_config;
    QString m_configFilePath;
    QString m_spoolDir;
    QString m_hostname;
    ConfigManager::RelayConfig m_relay;

    QTimer*              m_timer;
    QQueue<OutboundEmail> m_queue;
    bool m_stopping;
    int  m_maxRetries;
};

#endif // OUTBOUNDWORKER_H
