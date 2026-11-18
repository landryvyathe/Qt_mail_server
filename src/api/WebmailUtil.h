#ifndef WEBMAILUTIL_H
#define WEBMAILUTIL_H

// Small helpers shared by the webmail API translation units.
#define CPPHTTPLIB_OPENSSL_SUPPORT
#include <httplib.h>
#include "../services/ImapMime.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QString>

namespace WebmailUtil {

inline void sendJson(httplib::Response& res, int code, const QJsonValue& value) {
    res.status = code;
    const QByteArray body = value.isArray()
        ? QJsonDocument(value.toArray()).toJson(QJsonDocument::Compact)
        : QJsonDocument(value.toObject()).toJson(QJsonDocument::Compact);
    res.set_content(body.toStdString(), "application/json; charset=utf-8");
}

inline void sendError(httplib::Response& res, int code, const QString& message) {
    sendJson(res, code, QJsonObject{{"error", message}});
}

inline bool parseBody(const httplib::Request& req, httplib::Response& res, QJsonObject& out) {
    QJsonParseError err;
    const QJsonDocument doc = QJsonDocument::fromJson(QByteArray::fromStdString(req.body), &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject()) {
        sendError(res, 400, "Invalid JSON body");
        return false;
    }
    out = doc.object();
    return true;
}

inline QString queryParam(const httplib::Request& req, const char* name) {
    return req.has_param(name) ? QString::fromStdString(req.get_param_value(name)) : QString();
}

// "INBOX", a safe folder name, or empty when the name is invalid.
inline QString normalizeFolder(const QString& raw) {
    const QString name = raw.trimmed();
    if (name.compare("INBOX", Qt::CaseInsensitive) == 0) return "INBOX";
    static const QRegularExpression bad(R"([/\\*%"\x00-\x1f]|\.\.|^\.|\.$)");
    if (name.isEmpty() || name.size() > 200 || bad.match(name).hasMatch()) return {};
    return name;
}

inline QString flagLetters(const QString& fileName) {
    const int idx = fileName.indexOf(":2,");
    return idx >= 0 ? fileName.mid(idx + 3) : QString();
}

inline QString headerText(const QByteArray& header, const char* name) {
    return QString::fromUtf8(ImapMime::headerValue(header, name));
}

inline QString addressOf(const QString& header, QString* name = nullptr) {
    static const QRegularExpression angle("<([^>]+)>");
    const auto m = angle.match(header);
    if (name) {
        QString n = m.hasMatch() ? header.left(m.capturedStart()).trimmed() : QString();
        if (n.startsWith('"') && n.endsWith('"') && n.size() >= 2) n = n.mid(1, n.size() - 2);
        *name = n;
    }
    return (m.hasMatch() ? m.captured(1) : header).trimmed().toLower();
}

} // namespace WebmailUtil

#endif // WEBMAILUTIL_H
