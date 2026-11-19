#ifndef WEBMAILAPI_H
#define WEBMAILAPI_H

#include <QHash>
#include <QJsonObject>
#include <QMutex>
#include <QString>
#include <QStringList>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>

namespace httplib {
class Server;
struct Request;
struct Response;
}
class ConfigManager;
class MaildirStorage;
class AuthManager;
class QueueManager;

struct WebmailServices {
    MaildirStorage* storage = nullptr;
    AuthManager*    auth    = nullptr;
    QueueManager*   queue   = nullptr;
    bool isValid() const { return storage && auth && queue; }
};

// JSON API used by the browser webmail (/api/mail/*). Users log in with their
// own mailbox credentials; sessions are separate from the admin API's.
class WebmailApi {
public:
    WebmailApi(const ConfigManager& config, const WebmailServices& services);
    ~WebmailApi();
    void registerRoutes(httplib::Server& server);

private:
    struct Session {
        QString user;   // mailbox name (local part)
        QString email;
        qint64  expires = 0;
    };

    bool authorize(const httplib::Request& req, httplib::Response& res, Session& out);
    QString createSession(const QString& email);
    void dropSession(const httplib::Request& req);
    void loadSessions();
    void saveSessionsLocked() const;

    void handleFolders(const Session& s, httplib::Response& res);
    void handleFolderAction(const Session& s, const QJsonObject& body, httplib::Response& res);
    void handleList(const Session& s, const httplib::Request& req, httplib::Response& res);
    void handleMessage(const Session& s, const httplib::Request& req, httplib::Response& res);
    void handleFlags(const Session& s, const QJsonObject& body, httplib::Response& res);
    void handleMove(const Session& s, const QJsonObject& body, httplib::Response& res);
    void handleDelete(const Session& s, const QJsonObject& body, httplib::Response& res);
    void handleSend(const Session& s, const QJsonObject& body, httplib::Response& res, bool draft);

    // Gmail-style features (WebmailFeatures.cpp)
    void handleSettingsGet(const Session& s, httplib::Response& res);
    void handleSettingsSave(const Session& s, const QJsonObject& body, httplib::Response& res);
    void handleLabels(const Session& s, const QJsonObject& body, httplib::Response& res);
    void handleSnooze(const Session& s, const QJsonObject& body, httplib::Response& res);
    void handleThread(const Session& s, const httplib::Request& req, httplib::Response& res);
    void handleContacts(const Session& s, const httplib::Request& req, httplib::Response& res);
    void handleContactsSave(const Session& s, const QJsonObject& body, httplib::Response& res);
    void snoozeLoop();
    void wakeDueSnoozes(const QString& user);

    bool moveMessages(const QString& user, const QString& from, const QList<int>& uids, const QString& to);
    QStringList allMailFolders(const QString& user) const;
    QString folderKey(const Session& s, const QString& folder) const;
    static QString folderKeyFor(const QString& user, const QString& folder);
    bool folderExists(const Session& s, const QString& folder) const;
    bool isLocalAddress(const QString& email) const;

    const ConfigManager& m_config;
    WebmailServices      m_services;
    QStringList          m_localDomains;
    QString              m_hostname;
    QHash<QString, Session> m_sessions;   // keyed by SHA-256 of the bearer token
    QMutex               m_sessionMutex;
    QString              m_sessionFile;
    std::thread          m_snoozeThread;
    std::atomic<bool>    m_stopping{false};
    std::mutex           m_wakeMutex;
    std::condition_variable m_wake;
};

#endif // WEBMAILAPI_H
