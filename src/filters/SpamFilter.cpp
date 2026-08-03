#include "SpamFilter.h"

#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QUrl>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QDebug>

SpamFilter::SpamFilter(const QString& rspamdHost, int rspamdPort, QObject* parent)
    : QObject(parent) {

    m_rspamdUrl = QString("http://%1:%2/checkv2").arg(rspamdHost).arg(rspamdPort);
    m_nam = new QNetworkAccessManager(this);
}

QNetworkReply* SpamFilter::check(const QByteArray& mimeMessage, const QString& queueId) {
    QUrl rspamdUrl(m_rspamdUrl);
    QNetworkRequest req(rspamdUrl);
    req.setHeader(QNetworkRequest::ContentTypeHeader, "application/octet-stream");

    if (!queueId.isEmpty()) {
        req.setRawHeader("Queue-Id", queueId.toUtf8());
    }

    QNetworkReply* reply = m_nam->post(req, mimeMessage);

    // Connect the reply to parse the result and emit spamResult
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        reply->deleteLater();

        Result result;

        if (reply->error() != QNetworkReply::NoError) {
            qWarning() << "SpamFilter: Rspamd request failed:" << reply->errorString();
            // Treat Rspamd errors as pass-through (not spam)
            result.isSpam = false;
            result.rawResponse = reply->errorString();
            emit spamResult(result);
            return;
        }

        QByteArray data = reply->readAll();
        result.rawResponse = QString::fromUtf8(data);

        QJsonDocument doc = QJsonDocument::fromJson(data);
        if (!doc.isObject()) {
            qWarning() << "SpamFilter: Rspamd returned non-JSON";
            emit spamResult(result);
            return;
        }

        QJsonObject obj = doc.object();
        result.score     = obj.value("score").toDouble();
        result.threshold = obj.value("required_score").toDouble(5.0);
        result.action    = obj.value("action").toString("no action");
        result.isSpam    = result.score >= result.threshold;

        if (obj.contains("symbols")) {
            QJsonObject syms = obj.value("symbols").toObject();
            result.symbols = syms.keys();
        }

        qInfo() << "SpamFilter: score=" << result.score
                << "threshold=" << result.threshold
                << "action=" << result.action;
        emit spamResult(result);
    });

    return reply;
}
