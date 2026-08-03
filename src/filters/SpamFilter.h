#ifndef SPAMFILTER_H
#define SPAMFILTER_H

#include <QObject>
#include <QString>

class QNetworkAccessManager;
class QNetworkReply;

/**
 * Async Rspamd spam filter – sends messages to Rspamd via HTTP and
 * reports results asynchronously via callbacks.
 *
 * This replaces the old blocking IO with non-blocking QNetworkAccessManager calls.
 */
class SpamFilter : public QObject {
    Q_OBJECT
public:
    struct Result {
        bool   isSpam  = false;
        double score   = 0.0;
        double threshold = 5.0;
        QString action;
        QStringList symbols;
        QString rawResponse;
    };

    explicit SpamFilter(const QString& rspamdHost = "127.0.0.1",
                        int rspamdPort = 11333,
                        QObject* parent = nullptr);

    /**
     * Check a MIME message against Rspamd asynchronously.
     * The result is delivered via the spamResult signal.
     * Returns a handle (reply pointer) that can be used to track.
     */
    QNetworkReply* check(const QByteArray& mimeMessage, const QString& queueId = QString());

signals:
    void spamResult(const SpamFilter::Result& result);

private:
    QNetworkAccessManager* m_nam;
    QString m_rspamdUrl;
};

#endif // SPAMFILTER_H
