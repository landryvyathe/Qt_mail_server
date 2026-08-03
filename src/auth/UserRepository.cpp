#include "UserRepository.h"

#include <QSqlQuery>
#include <QSqlError>
#include <QDebug>
#include <QCryptographicHash>
#include <QRandomGenerator>
#include <unistd.h>
#include <crypt.h>

// ---------------------------------------------------------------------------
// Constructor
// ---------------------------------------------------------------------------
UserRepository::UserRepository() {}

// ---------------------------------------------------------------------------
// init
// ---------------------------------------------------------------------------
bool UserRepository::init(const QString& host, int port, const QString& dbName,
                           const QString& user, const QString& password) {
    m_db = QSqlDatabase::addDatabase("QPSQL", "mailserver_connection");
    m_db.setHostName(host);
    m_db.setPort(port);
    m_db.setDatabaseName(dbName);
    m_db.setUserName(user);
    m_db.setPassword(password);

    if (!m_db.open()) {
        qCritical() << "UserRepository: DB open failed:" << m_db.lastError().text();
        return false;
    }
    qInfo() << "UserRepository: connected to PostgreSQL";
    return true;
}

// ---------------------------------------------------------------------------
// Generate a cryptographically-secure random salt
// ---------------------------------------------------------------------------
static QString generateRandomSalt(int length = 16) {
    const char chars[] =
        "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789./";
    const int nchars = sizeof(chars) - 1;
    QByteArray salt;
    salt.resize(length);

    // Use a proper CSPRNG source
    QRandomGenerator* rng = QRandomGenerator::system();
    for (int i = 0; i < length; ++i) {
        salt[i] = chars[rng->bounded(nchars)];
    }
    return QString::fromLatin1(salt);
}

// ---------------------------------------------------------------------------
// Hash a plain password using SHA-512 crypt ($6$) with a random 16‑byte salt
// ---------------------------------------------------------------------------
QString UserRepository::hashPassword(const QString& plainPassword) {
    QString saltStr = generateRandomSalt(16);
    QByteArray setting = QString("$6$%1").arg(saltStr).toLatin1();
    char* result = crypt(plainPassword.toUtf8().constData(), setting.constData());
    if (!result) {
        qCritical() << "UserRepository: crypt() returned NULL";
        return {};
    }
    return QString::fromLatin1(result);
}

// ---------------------------------------------------------------------------
// Verify a plain password against a stored $6$... hash
// ---------------------------------------------------------------------------
bool UserRepository::verifyPassword(const QString& plainPassword, const QString& storedHash) {
    QByteArray setting = storedHash.toLatin1();
    char* result = crypt(plainPassword.toUtf8().constData(), setting.constData());
    if (!result) return false;
    return storedHash == QString::fromLatin1(result);
}

// ---------------------------------------------------------------------------
// CRUD
// ---------------------------------------------------------------------------
std::optional<User> UserRepository::findByEmail(const QString& email) {
    QSqlQuery query(m_db);
    query.prepare("SELECT id, email, password_hash, maildir, active FROM users WHERE email = :email");
    query.bindValue(":email", email);
    if (!query.exec() || !query.next()) return std::nullopt;

    User u;
    u.id           = query.value("id").toInt();
    u.email        = query.value("email").toString();
    u.passwordHash = query.value("password_hash").toString();
    u.maildir      = query.value("maildir").toString();
    u.active       = query.value("active").toBool();
    return u;
}

bool UserRepository::createUser(const QString& email, const QString& plainPassword) {
    QString hash = hashPassword(plainPassword);
    if (hash.isEmpty()) return false;

    QSqlQuery query(m_db);
    query.prepare("INSERT INTO users (email, password_hash) VALUES (:email, :hash)");
    query.bindValue(":email", email);
    query.bindValue(":hash", hash);
    if (!query.exec()) {
        qWarning() << "UserRepository: createUser failed:" << query.lastError().text();
        return false;
    }
    qInfo() << "UserRepository: created user" << email;
    return true;
}

bool UserRepository::setPassword(const QString& email, const QString& newPlainPassword) {
    QString hash = hashPassword(newPlainPassword);
    if (hash.isEmpty()) return false;

    QSqlQuery query(m_db);
    query.prepare("UPDATE users SET password_hash = :hash WHERE email = :email");
    query.bindValue(":hash", hash);
    query.bindValue(":email", email);
    if (!query.exec()) {
        qWarning() << "UserRepository: setPassword failed:" << query.lastError().text();
        return false;
    }
    return true;
}

bool UserRepository::verifyCredentials(const QString& email, const QString& plainPassword) {
    auto user = findByEmail(email);
    if (!user.has_value() || !user->active) return false;
    return verifyPassword(plainPassword, user->passwordHash);
}

bool UserRepository::userExists(const QString& email) {
    QSqlQuery query(m_db);
    query.prepare("SELECT 1 FROM users WHERE email = :email");
    query.bindValue(":email", email);
    return query.exec() && query.next();
}
