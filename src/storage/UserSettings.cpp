#include "UserSettings.h"
#include "../services/ImapMime.h"

#include <QDateTime>
#include <QFile>
#include <QJsonDocument>
#include <QLocale>
#include <QMutex>
#include <QMutexLocker>
#include <QRandomGenerator>
#include <QSaveFile>

namespace {

QMutex g_mutex;
constexpr qint64 kVacationRepeatSecs = 4 * 24 * 3600;
constexpr int kMaxContacts = 2000;

QString settingsPath(const QString& base, const QString& user) {
    return base + "/" + user + "/.webmail.json";
}

QJsonObject readUnlocked(const QString& base, const QString& user) {
    QFile f(settingsPath(base, user));
    if (!f.open(QIODevice::ReadOnly)) return {};
    return QJsonDocument::fromJson(f.readAll()).object();
}

bool isBulkOrAutomated(const QString& sender, const QByteArray& header) {
    const QString s = sender.toLower();
    if (s.isEmpty() || s.contains("noreply") || s.contains("no-reply") || s.contains("mailer-daemon")
        || s.startsWith("postmaster@") || s.contains("notification"))
        return true;
    const QByteArray autoSubmitted = ImapMime::headerValue(header, "Auto-Submitted").trimmed().toLower();
    if (!autoSubmitted.isEmpty() && autoSubmitted != "no") return true;
    const QByteArray precedence = ImapMime::headerValue(header, "Precedence").trimmed().toLower();
    if (precedence == "bulk" || precedence == "list" || precedence == "junk") return true;
    return !ImapMime::headerValue(header, "List-Id").isEmpty()
        || !ImapMime::headerValue(header, "List-Unsubscribe").isEmpty();
}

QByteArray encodeWord(const QString& text) {
    for (QChar c : text)
        if (c.unicode() > 0x7e) return "=?UTF-8?B?" + text.toUtf8().toBase64() + "?=";
    return text.toLatin1();
}

} // namespace

namespace UserSettings {

QJsonObject load(const QString& maildirBase, const QString& user) {
    QMutexLocker lock(&g_mutex);
    return readUnlocked(maildirBase, user);
}

void update(const QString& maildirBase, const QString& user,
            const std::function<void(QJsonObject&)>& change) {
    QMutexLocker lock(&g_mutex);
    QJsonObject settings = readUnlocked(maildirBase, user);
    change(settings);
    QSaveFile f(settingsPath(maildirBase, user));
    if (!f.open(QIODevice::WriteOnly)) return;
    f.write(QJsonDocument(settings).toJson(QJsonDocument::Compact));
    f.commit();
}

void rememberContact(QJsonObject& settings, const QString& email, const QString& name) {
    const QString addr = email.trimmed().toLower();
    if (addr.isEmpty() || !addr.contains('@') || addr.size() > 254) return;
    QJsonObject contacts = settings.value("contacts").toObject();
    QJsonObject c = contacts.value(addr).toObject();
    if (!name.trimmed().isEmpty()) c["name"] = name.trimmed().left(120);
    c["count"] = c.value("count").toInt() + 1;
    c["last"] = QDateTime::currentSecsSinceEpoch();
    contacts[addr] = c;
    if (contacts.size() > kMaxContacts) {
        QString oldest;
        qint64 oldestTs = LLONG_MAX;
        for (auto it = contacts.begin(); it != contacts.end(); ++it) {
            const qint64 ts = it.value().toObject().value("last").toInteger();
            if (ts < oldestTs) { oldestTs = ts; oldest = it.key(); }
        }
        contacts.remove(oldest);
    }
    settings["contacts"] = contacts;
}

QByteArray autoReplyFor(const QString& maildirBase, const QString& user, const QString& userEmail,
                        const QString& sender, const QByteArray& message, const QString& hostname) {
    const QByteArray header = ImapMime::headerBlock(ImapMime::toCrlf(message.left(65536)));
    if (isBulkOrAutomated(sender, header)) return {};

    QByteArray reply;
    update(maildirBase, user, [&](QJsonObject& s) {
        const QJsonObject vac = s.value("vacation").toObject();
        const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
        if (!vac.value("enabled").toBool()) return;
        if (vac.value("start").toInteger() > 0 && nowMs < vac.value("start").toInteger()) return;
        if (vac.value("end").toInteger() > 0 && nowMs > vac.value("end").toInteger()) return;

        QJsonObject replied = s.value("vacationReplied").toObject();
        const QString key = sender.toLower();
        const qint64 nowSecs = nowMs / 1000;
        if (nowSecs - replied.value(key).toInteger() < kVacationRepeatSecs) return;
        replied[key] = nowSecs;
        s["vacationReplied"] = replied;

        QString subject = vac.value("subject").toString().trimmed();
        if (subject.isEmpty()) subject = "Auto: " + QString::fromUtf8(ImapMime::headerValue(header, "Subject"));
        subject.replace('\r', ' ').replace('\n', ' ');
        const QByteArray msgId = ImapMime::headerValue(header, "Message-ID").trimmed();
        const QByteArray body = vac.value("body").toString().toUtf8();

        reply = "From: " + userEmail.toUtf8() + "\r\nTo: " + sender.toUtf8() + "\r\n"
              + "Subject: " + encodeWord(subject.left(900)) + "\r\n"
              + "Date: " + QLocale::c().toString(QDateTime::currentDateTimeUtc(), "ddd, dd MMM yyyy hh:mm:ss").toLatin1() + " +0000\r\n"
              + "Message-ID: <" + QByteArray::number(QRandomGenerator::system()->generate64(), 16) + "@" + hostname.toLatin1() + ">\r\n"
              + (msgId.isEmpty() ? QByteArray() : "In-Reply-To: " + msgId + "\r\nReferences: " + msgId + "\r\n")
              + "Auto-Submitted: auto-replied\r\nPrecedence: bulk\r\nMIME-Version: 1.0\r\n"
              + "Content-Type: text/plain; charset=utf-8\r\nContent-Transfer-Encoding: base64\r\n\r\n";
        const QByteArray b64 = body.toBase64();
        for (int i = 0; i < b64.size(); i += 76) reply += b64.mid(i, 76) + "\r\n";
    });
    return reply;
}

} // namespace UserSettings
