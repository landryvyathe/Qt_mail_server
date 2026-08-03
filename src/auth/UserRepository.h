#ifndef USERREPOSITORY_H
#define USERREPOSITORY_H

#include <QString>
#include <QSqlDatabase>
#include <optional>

struct User {
    int id;
    QString email;
    QString passwordHash;   // $6$<salt>$<hash>  (SHA-512 crypt)
    QString maildir;
    bool active;
};

class UserRepository {
public:
    UserRepository();

    bool init(const QString& host, int port, const QString& dbName,
              const QString& user, const QString& password);

    std::optional<User> findByEmail(const QString& email);
    bool createUser(const QString& email, const QString& plainPassword);
    bool setPassword(const QString& email, const QString& newPlainPassword);
    bool verifyCredentials(const QString& email, const QString& plainPassword);
    bool userExists(const QString& email);

private:
    // Hash a plain password using SHA-512 crypt with a random 16-byte salt.
    QString hashPassword(const QString& plainPassword);
    // Verify a plain password against a stored $6$... hash.
    bool verifyPassword(const QString& plainPassword, const QString& storedHash);

    QSqlDatabase m_db;
};

#endif // USERREPOSITORY_H
