#ifndef USERSETTINGS_H
#define USERSETTINGS_H

#include <QByteArray>
#include <QJsonObject>
#include <QString>
#include <functional>

// Per-user webmail state kept in <maildir>/<user>/.webmail.json: signature,
// auto-reply, labels, snoozes and contacts. Access is serialized process-wide.
namespace UserSettings {

QJsonObject load(const QString& maildirBase, const QString& user);

// Loads, applies `change`, and atomically writes the result back.
void update(const QString& maildirBase, const QString& user,
            const std::function<void(QJsonObject&)>& change);

// Records an address in the user's contacts (used for autocomplete).
void rememberContact(QJsonObject& settings, const QString& email, const QString& name);

// Builds an out-of-office reply for `message` sent by `sender` to `userEmail`, or
// returns an empty array when no reply is due (disabled, out of window, bulk mail,
// or this sender was answered in the last few days).
QByteArray autoReplyFor(const QString& maildirBase, const QString& user, const QString& userEmail,
                        const QString& sender, const QByteArray& message, const QString& hostname);

} // namespace UserSettings

#endif // USERSETTINGS_H
