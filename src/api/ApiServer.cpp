#include "ApiServer.h"
#include "../ConfigManager.h"

// httplib is a single-header library
#define CPPHTTPLIB_OPENSSL_SUPPORT
#include <httplib.h>

#include <QDebug>
#include <unistd.h>
#include <crypt.h>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QDateTime>
#include <QMutex>
#include <QMutexLocker>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QSqlError>
#include <QFile>
#include <QRandomGenerator>
#include <QDir>
#include <QTcpSocket>
#include <QProcess>
#include <QProcessEnvironment>
#include <QThread>
#include <QRegularExpression>

// GB to bytes constant
static constexpr qint64 GB = 1073741824LL;

// Session timeout: 24 hours
static constexpr qint64 SESSION_TTL_SECS = 86400;

// -------------------- helpers -----------------------------------------------
static QString hashPassword(const QString& password)
{
    // Use SHA-512 crypt (same as UserRepository / AuthManager)
    static const char chars[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789./";
    static const int nchars = sizeof(chars) - 1;

    QByteArray salt(16, '\0');
    for (int i = 0; i < 16; ++i)
        salt[i] = chars[QRandomGenerator::global()->bounded(nchars)];

    QByteArray setting = "$6$" + salt + "$";
    char* result = crypt(password.toUtf8().constData(), setting.constData());
    if (!result) {
        qCritical() << "ApiServer: crypt() returned NULL for password hash";
        return {};
    }
    return QString::fromLatin1(result);
}

static bool verifyPassword(const QString& plainPassword, const QString& storedHash)
{
    QByteArray setting = storedHash.toLatin1();
    char* result = crypt(plainPassword.toUtf8().constData(), setting.constData());
    if (!result) return false;
    return storedHash == QString::fromLatin1(result);
}

static QString randomToken()
{
    QByteArray r(32, 0);
    for (int i = 0; i < 32; ++i)
        r[i] = static_cast<char>('a' + QRandomGenerator::global()->bounded(26));
    return QString::fromLatin1(r.toHex());
}

static double bytesToGB(qint64 bytes) { return static_cast<double>(bytes) / GB; }
static qint64 gbToBytes(double gb) { return static_cast<qint64>(gb * GB); }

// Probe a TCP port to check if a service is alive
static bool probePort(const QString& host, int port, int timeoutMs = 500)
{
    QTcpSocket sock;
    sock.connectToHost(host, port);
    return sock.waitForConnected(timeoutMs);
}

// Read /proc/self/status for memory info
static QPair<qint64,qint64> readMemoryInfo()
{
    QFile f("/proc/self/status");
    if (!f.open(QIODevice::ReadOnly)) return {0, 0};
    QString text = f.readAll();
    qint64 vmRss = 0, vmSize = 0;
    for (const auto& line : text.split('\n')) {
        if (line.startsWith("VmRSS:")) {
            vmRss = line.split(QRegularExpression("\\s+"), Qt::SkipEmptyParts).value(1).toLongLong();
        } else if (line.startsWith("VmSize:")) {
            vmSize = line.split(QRegularExpression("\\s+"), Qt::SkipEmptyParts).value(1).toLongLong();
        }
    }
    return {vmRss, vmSize};
}

static qint64 uptimeSeconds()
{
    QFile f("/proc/uptime");
    if (!f.open(QIODevice::ReadOnly)) return 0;
    return static_cast<qint64>(f.readAll().split(' ').first().toDouble());
}

// ---------------------------------------------------------------------------
// Constructor
// ---------------------------------------------------------------------------
ApiServer::ApiServer(const ConfigManager& config, const QString& configFilePath, QObject* parent)
    : QObject(parent), m_config(config), m_configFilePath(configFilePath) {

    m_port = config.port("api");
    if (m_port <= 0) m_port = 8080;
}

ApiServer::~ApiServer() {
    stop();
}

// ---------------------------------------------------------------------------
// start
// ---------------------------------------------------------------------------
bool ApiServer::start() {
    if (m_thread) {
        qWarning() << "ApiServer: already started";
        return false;
    }

    m_stopFlag = false;

    auto worker = [this]() {
        // Create the httplib::Server in the worker thread
        m_server = std::make_unique<httplib::Server>();

        // --- Database connection factory (thread-safe: opens per-request) ---
        auto dbCfg = m_config.database();
        auto openDb = [this, dbCfg]() -> QSqlDatabase {
            static QAtomicInt counter(0);
            QString name = QStringLiteral("api_%1_%2")
                .arg(reinterpret_cast<quintptr>(this))
                .arg(counter.fetchAndAddRelaxed(1));
            QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QPSQL"), name);
            db.setHostName(dbCfg.value("host").toString());
            db.setPort(dbCfg.value("port").toInt(5432));
            db.setDatabaseName(dbCfg.value("name").toString());
            db.setUserName(dbCfg.value("user").toString());
            db.setPassword(dbCfg.value("password").toString());
            if (!db.open()) {
                qWarning() << "ApiServer: failed to open database:" << db.lastError().text();
            }
            return db;
        };

        auto closeDb = [](const QString& name) {
            QSqlDatabase::database(name).close();
            // Note: removeDatabase would warn if QSqlQuery objects are still in scope,
            // so we only close the connection. The unique connection name prevents conflicts.
        };

        // --- Rate-limiting state (per-IP counters) ---
        QMap<QString, qint64> requestTimestamps;
        QMutex rateMutex;

        auto checkRateLimit = [&](const std::string& ip) -> bool {
            auto cfg = m_config.apiConfig();
            if (cfg.rateLimitRpm <= 0) return true;

            QMutexLocker lock(&rateMutex);
            QString qip = QString::fromStdString(ip);
            qint64 now = QDateTime::currentSecsSinceEpoch();
            qint64 windowStart = now - 60;

            int count = 0;
            auto it = requestTimestamps.find(qip);
            if (it != requestTimestamps.end() && it.value() > windowStart) {
                count++;
            }
            if (it == requestTimestamps.end()) {
                requestTimestamps[qip] = now;
                count++;
            } else {
                qint64 last = it.value();
                if (last > windowStart) {
                    return false;
                }
                it.value() = now;
            }
            return count <= cfg.rateLimitRpm;
        };

        // --- Session-based authentication ---
        struct Session { QString user; qint64 expires; };
        QMap<QString, Session> sessions;
        QMutex sessionMutex;

        // Load admin password hash from config
        QString adminPassHash = m_config.getRawConfig().value("admin_password_hash").toString();
        bool requireLogin = !adminPassHash.isEmpty();
        if (!requireLogin) {
            qInfo() << "ApiServer: No admin_password_hash in config — API is open (no auth required)";
        }

        // Purge expired sessions
        auto purgeSessions = [&]() {
            QMutexLocker lk(&sessionMutex);
            qint64 now = QDateTime::currentSecsSinceEpoch();
            for (auto it = sessions.begin(); it != sessions.end(); ) {
                if (it.value().expires < now)
                    it = sessions.erase(it);
                else
                    ++it;
            }
        };

        auto checkAuth = [&](const httplib::Request& req, httplib::Response& res) -> bool {
            if (!requireLogin) return true;

            auto auth = req.get_header_value("Authorization");
            if (auth.empty()) {
                res.status = 401;
                res.set_content("{\"error\":\"Missing Authorization header\"}", "application/json");
                return false;
            }
            QString bearer = QString::fromStdString(auth);
            if (!bearer.startsWith("Bearer ")) {
                res.status = 401;
                res.set_content("{\"error\":\"Invalid Authorization format\"}", "application/json");
                return false;
            }
            QString token = bearer.mid(7).trimmed();

            QMutexLocker lk(&sessionMutex);
            auto it = sessions.find(token);
            if (it == sessions.end()) {
                res.status = 401;
                res.set_content("{\"error\":\"Invalid or expired session\"}", "application/json");
                return false;
            }
            if (it.value().expires < QDateTime::currentSecsSinceEpoch()) {
                sessions.erase(it);
                res.status = 401;
                res.set_content("{\"error\":\"Session expired\"}", "application/json");
                return false;
            }
            // Extend session on each authenticated request
            it.value().expires = QDateTime::currentSecsSinceEpoch() + SESSION_TTL_SECS;
            return true;
        };

        // Helper: respond with JSON error
        auto jsonError = [](httplib::Response& res, int code, const QString& msg) {
            res.status = code;
            QJsonObject obj;
            obj["error"] = msg;
            res.set_content(QJsonDocument(obj).toJson(QJsonDocument::Compact).toStdString(),
                            "application/json");
        };

        // Helper: respond with JSON
        auto jsonOk = [](httplib::Response& res, int code, const QJsonObject& obj) {
            res.status = code;
            res.set_content(QJsonDocument(obj).toJson(QJsonDocument::Compact).toStdString(),
                            "application/json");
        };

        // Load Alibaba Cloud credentials from config
        QJsonObject alibabaCfg = m_config.getRawConfig().value("alibaba_cloud").toObject();
        QString aliAccessKey = alibabaCfg.value("access_key_id").toString();
        QString aliSecret = alibabaCfg.value("access_key_secret").toString();
        QString aliRegion = alibabaCfg.value("region").toString("ap-southeast-1");
        bool aliConfigured = !aliAccessKey.isEmpty() && !aliSecret.isEmpty();
        if (aliConfigured) {
            qInfo() << "ApiServer: Alibaba Cloud DirectMail configured (region:" << aliRegion << ")";
        } else {
            qInfo() << "ApiServer: Alibaba Cloud NOT configured — sender auto-creation disabled";
        }

        // Resolve path to the Python script
        QString aliScriptPath;
        QStringList scriptSearchPaths = {
            QDir::currentPath() + "/scripts/alibaba_provision_sender.py",
            QDir::currentPath() + "/../scripts/alibaba_provision_sender.py",
        };
        for (const auto& p : scriptSearchPaths) {
            if (QFile::exists(p)) { aliScriptPath = p; break; }
        }
        if (aliConfigured && aliScriptPath.isEmpty())
            qWarning() << "ApiServer: alibaba_provision_sender.py not found — new mailboxes will not be able to send";

        // Registers the address with DirectMail and gives it its own SMTP password (DirectMail only
        // lets a login send as itself). Runs synchronously: this httplib worker thread has no Qt
        // event loop, so QProcess::finished would never be delivered.
        auto createAliSender = [&](const QString& email) {
            if (!aliConfigured || aliScriptPath.isEmpty()) return;
            QProcess proc;
            proc.start("python3", {aliScriptPath, email, "--config", m_configFilePath, "--quiet"});
            if (!proc.waitForFinished(60000)) {
                proc.kill();
                qWarning() << "DirectMail provisioning timed out for" << email;
                return;
            }
            if (proc.exitCode() != 0)
                qWarning() << "DirectMail provisioning failed for" << email << ":"
                           << QString::fromUtf8(proc.readAllStandardError()).trimmed();
            else
                qInfo() << "DirectMail sender provisioned:" << email;
        };

        // ============================================
        //  AUTH & SESSION ENDPOINTS
        // ============================================

        // POST /api/login
        m_server->Post("/api/login", [&](const httplib::Request& req, httplib::Response& res) {
            purgeSessions();
            QJsonParseError err;
            QJsonDocument doc = QJsonDocument::fromJson(QByteArray::fromStdString(req.body), &err);
            if (err.error != QJsonParseError::NoError) {
                jsonError(res, 400, "Invalid JSON"); return;
            }
            QString password = doc.object().value("password").toString();
            if (password.isEmpty()) {
                jsonError(res, 400, "password is required"); return;
            }
            if (!requireLogin) {
                jsonError(res, 400, "No admin password configured"); return;
            }
            if (!verifyPassword(password, adminPassHash)) {
                jsonError(res, 401, "Invalid password"); return;
            }
            QString token = randomToken();
            qint64 expires = QDateTime::currentSecsSinceEpoch() + SESSION_TTL_SECS;
            {
                QMutexLocker lk(&sessionMutex);
                sessions[token] = {"admin", expires};
            }
            QJsonObject resp;
            resp["token"] = token;
            resp["expires_in"] = SESSION_TTL_SECS;
            resp["user"] = "admin";
            jsonOk(res, 200, resp);
        });

        // POST /api/logout
        m_server->Post("/api/logout", [&](const httplib::Request& req, httplib::Response& res) {
            auto auth = req.get_header_value("Authorization");
            if (!auth.empty()) {
                QString bearer = QString::fromStdString(auth);
                if (bearer.startsWith("Bearer ")) {
                    QMutexLocker lk(&sessionMutex);
                    sessions.remove(bearer.mid(7).trimmed());
                }
            }
            res.set_content("{\"status\":\"logged out\"}", "application/json");
        });

        // GET /api/session — check if logged in
        m_server->Get("/api/session", [&](const httplib::Request& req, httplib::Response& res) {
            if (!requireLogin) {
                res.set_content("{\"authenticated\":true,\"user\":\"admin\",\"auth_required\":false}", "application/json");
                return;
            }
            auto auth = req.get_header_value("Authorization");
            if (auth.empty() || !QString::fromStdString(auth).startsWith("Bearer ")) {
                res.set_content("{\"authenticated\":false,\"auth_required\":true}", "application/json");
                return;
            }
            QString token = QString::fromStdString(auth).mid(7).trimmed();
            QMutexLocker lk(&sessionMutex);
            auto it = sessions.find(token);
            if (it != sessions.end() && it.value().expires > QDateTime::currentSecsSinceEpoch()) {
                QJsonObject r; r["authenticated"] = true; r["user"] = it.value().user;
                jsonOk(res, 200, r);
            } else {
                res.set_content("{\"authenticated\":false,\"auth_required\":true}", "application/json");
            }
        });

        // ============================================
        //  SERVICES ENDPOINTS
        // ============================================

        // GET /api/services — status of all services
        m_server->Get("/api/services", [&](const httplib::Request& req, httplib::Response& res) {
            if (!checkAuth(req, res)) return;
            // Define services with names and ports
            QList<QPair<QString, int>> svcs = {
                {"SMTP",      m_config.port("smtp")},
                {"Submission", m_config.port("submission")},
                {"SMTPS",     m_config.port("smtps")},
                {"IMAP",      m_config.port("imap")},
                {"IMAPS",     m_config.port("imaps")},
                {"POP3",      m_config.port("pop3")},
                {"POP3S",     m_config.port("pop3s")},
                {"API",       m_port},
            };
            QJsonArray arr;
            for (const auto& svc : svcs) {
                QJsonObject o;
                o["name"] = svc.first;
                o["port"] = svc.second;
                if (svc.second > 0) {
                    bool alive = probePort("127.0.0.1", svc.second);
                    o["status"] = alive ? "online" : "offline";
                    o["listening"] = alive;
                } else {
                    o["status"] = "disabled";
                    o["listening"] = false;
                }
                arr.append(o);
            }
            QJsonObject resp;
            resp["services"] = arr;
            resp["total"] = arr.size();
            resp["time"] = QDateTime::currentSecsSinceEpoch();
            jsonOk(res, 200, resp);
        });

        // ============================================
        //  SYSTEM ENDPOINTS
        // ============================================

        // GET /api/system — memory, uptime, etc.
        m_server->Get("/api/system", [&](const httplib::Request& req, httplib::Response& res) {
            if (!checkAuth(req, res)) return;
            QJsonObject sys;
            auto [vmRss, vmSize] = readMemoryInfo();
            sys["memory_rss_kb"] = vmRss;
            sys["memory_rss_mb"] = QString::number(vmRss / 1024.0, 'f', 1);
            sys["memory_size_kb"] = vmSize;
            sys["memory_size_mb"] = QString::number(vmSize / 1024.0, 'f', 1);
            sys["uptime_seconds"] = uptimeSeconds();
            qint64 up = uptimeSeconds();
            int days = up / 86400;
            int hours = (up % 86400) / 3600;
            int mins = (up % 3600) / 60;
            sys["uptime_display"] = QString("%1d %2h %3m").arg(days).arg(hours).arg(mins);
            sys["hostname"] = m_config.hostname();
            sys["api_port"] = m_port;
            sys["time"] = QDateTime::currentSecsSinceEpoch();
            jsonOk(res, 200, sys);
        });

        // ============================================
        //  LOGS ENDPOINT
        // ============================================

        // GET /api/logs?lines=200
        m_server->Get("/api/logs", [&](const httplib::Request& req, httplib::Response& res) {
            if (!checkAuth(req, res)) return;
            int lines = 100;
            if (req.has_param("lines")) {
                lines = QString::fromStdString(req.get_param_value("lines")).toInt();
                if (lines < 1) lines = 1;
                if (lines > 2000) lines = 2000;
            }
            QString logFile = QDir::currentPath() + "/server_live.log";
            QProcess proc;
            proc.start("tail", {"-n", QString::number(lines), logFile});
            proc.waitForFinished(3000);
            QJsonObject resp;
            resp["lines"] = lines;
            resp["content"] = QString::fromUtf8(proc.readAllStandardOutput());
            jsonOk(res, 200, resp);
        });

        // ============================================
        //  CATCH-ALL ENDPOINT
        // ============================================

        // GET /api/catch-all
        m_server->Get("/api/catch-all", [&](const httplib::Request& req, httplib::Response& res) {
            if (!checkAuth(req, res)) return;
            QJsonObject o;
            o["enabled"] = m_config.getRawConfig().value("catch_all_enabled").toBool(true);
            o["domain"] = m_config.hostname();
            o["description"] = "You can toggle it anytime. When off, senders of emails to non-existent users will get proper bounce notifications — just like Google does.";
            jsonOk(res, 200, o);
        });

        // POST /api/catch-all — toggle
        m_server->Post("/api/catch-all", [&](const httplib::Request& req, httplib::Response& res) {
            if (!checkAuth(req, res)) return;
            QJsonParseError err;
            QJsonDocument doc = QJsonDocument::fromJson(QByteArray::fromStdString(req.body), &err);
            if (err.error != QJsonParseError::NoError) {
                jsonError(res, 400, "Invalid JSON"); return;
            }
            bool enabled = doc.object().value("enabled").toBool(true);
            // Write back to config file
            QString configPath = QDir::currentPath() + "/config.json";
            QFile cf(configPath);
            QJsonObject root;
            if (cf.open(QIODevice::ReadOnly)) {
                root = QJsonDocument::fromJson(cf.readAll()).object();
                cf.close();
            }
            root["catch_all_enabled"] = enabled;
            if (cf.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
                cf.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
                cf.close();
            }
            // Update in-memory config immediately — no restart needed
            m_config.setCatchAllEnabled(enabled);
            QJsonObject o;
            o["catch_all_enabled"] = enabled;
            o["status"] = "applied";
            o["message"] = "Config saved and applied immediately. No restart needed.";
            jsonOk(res, 200, o);
        });

        // POST /api/admin/password — change admin password
        m_server->Post("/api/admin/password", [&](const httplib::Request& req, httplib::Response& res) {
            if (!checkAuth(req, res)) return;
            QJsonParseError err;
            QJsonDocument doc = QJsonDocument::fromJson(QByteArray::fromStdString(req.body), &err);
            if (err.error != QJsonParseError::NoError) {
                jsonError(res, 400, "Invalid JSON"); return;
            }
            QString newPassword = doc.object().value("new_password").toString();
            if (newPassword.isEmpty()) {
                jsonError(res, 400, "new_password is required"); return;
            }
            QString newHash = hashPassword(newPassword);

            // Write to config file
            QString configPath = QDir::currentPath() + "/config.json";
            QFile cf(configPath);
            QJsonObject root;
            if (cf.open(QIODevice::ReadOnly)) {
                root = QJsonDocument::fromJson(cf.readAll()).object();
                cf.close();
            }
            root["admin_password_hash"] = newHash;
            if (cf.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
                cf.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
                cf.close();
            }
            // Update in-memory so it takes effect immediately
            adminPassHash = newHash;
            requireLogin = true;
            QJsonObject o;
            o["status"] = "Admin password updated";
            jsonOk(res, 200, o);
        });

        // ============================================
        //  ALIBABA CLOUD SETTINGS ENDPOINTS
        // ============================================

        // GET /api/alibaba/settings
        m_server->Get("/api/alibaba/settings", [&](const httplib::Request& req, httplib::Response& res) {
            if (!checkAuth(req, res)) return;
            QJsonObject o;
            o["configured"] = aliConfigured;
            o["region"] = aliRegion;
            o["access_key_id"] = aliConfigured ? aliAccessKey.left(5) + "..." + aliAccessKey.right(3) : "";
            o["script_found"] = !aliScriptPath.isEmpty();
            o["script_path"] = aliScriptPath;
            jsonOk(res, 200, o);
        });

        // POST /api/alibaba/settings — update credentials
        m_server->Post("/api/alibaba/settings", [&](const httplib::Request& req, httplib::Response& res) {
            if (!checkAuth(req, res)) return;
            QJsonParseError err;
            QJsonDocument doc = QJsonDocument::fromJson(QByteArray::fromStdString(req.body), &err);
            if (err.error != QJsonParseError::NoError) {
                jsonError(res, 400, "Invalid JSON"); return;
            }
            QJsonObject body = doc.object();
            QString newKey = body.value("access_key_id").toString();
            QString newSecret = body.value("access_key_secret").toString();
            QString newRegion = body.value("region").toString("ap-southeast-1");

            // Write to config file
            QString configPath = QDir::currentPath() + "/config.json";
            QFile cf(configPath);
            QJsonObject root;
            if (cf.open(QIODevice::ReadOnly)) {
                root = QJsonDocument::fromJson(cf.readAll()).object();
                cf.close();
            }
            QJsonObject ali = root.value("alibaba_cloud").toObject();
            if (!newKey.isEmpty()) ali["access_key_id"] = newKey;
            if (!newSecret.isEmpty()) ali["access_key_secret"] = newSecret;
            ali["region"] = newRegion;
            root["alibaba_cloud"] = ali;
            if (cf.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
                cf.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
                cf.close();
            }

            // Update runtime vars
            aliAccessKey = ali["access_key_id"].toString();
            aliSecret = ali["access_key_secret"].toString();
            aliRegion = ali["region"].toString("ap-southeast-1");
            aliConfigured = !aliAccessKey.isEmpty() && !aliSecret.isEmpty();

            QJsonObject o;
            o["status"] = "updated";
            o["configured"] = aliConfigured;
            o["region"] = aliRegion;
            jsonOk(res, 200, o);
        });

        // ============================================
        //  USER MANAGEMENT API ENDPOINTS
        // ============================================

        // GET /api/users — list all users
        m_server->Get("/api/users", [&](const httplib::Request& req, httplib::Response& res) {
            if (!checkAuth(req, res)) return;
            QSqlDatabase db = openDb();
            if (!db.isOpen()) { jsonError(res, 500, "Database not connected"); return; }
            QString dbName = db.connectionName();

            QSqlQuery q(db);
            q.exec("SELECT id, email, quota_bytes, active, created_at, maildir FROM users ORDER BY id");
            QJsonArray arr;
            while (q.next()) {
                QJsonObject u;
                u["id"] = q.value(0).toInt();
                u["email"] = q.value(1).toString();
                qint64 qb = q.value(2).toLongLong();
                u["quota_bytes"] = qb;
                u["quota_gb"] = QString::number(bytesToGB(qb), 'f', 1);
                u["active"] = q.value(3).toBool();
                u["created_at"] = q.value(4).toDateTime().toString(Qt::ISODate);
                u["maildir"] = q.value(5).toString();
                arr.append(u);
            }
            QJsonObject resp;
            resp["users"] = arr;
            resp["total"] = arr.size();
            res.set_content(QJsonDocument(resp).toJson(QJsonDocument::Compact).toStdString(),
                            "application/json");
            closeDb(dbName);
        });

        // POST /api/users — create single user
        m_server->Post("/api/users", [&](const httplib::Request& req, httplib::Response& res) {
            if (!checkAuth(req, res)) return;
            QSqlDatabase db = openDb();
            if (!db.isOpen()) { jsonError(res, 500, "Database not connected"); return; }
            QString dbName = db.connectionName();

            QJsonParseError err;
            QJsonDocument doc = QJsonDocument::fromJson(QByteArray::fromStdString(req.body), &err);
            if (err.error != QJsonParseError::NoError) {
                jsonError(res, 400, "Invalid JSON"); closeDb(dbName); return;
            }
            QJsonObject body = doc.object();
            QString email = body.value("email").toString().trimmed();
            QString password = body.value("password").toString();
            qint64 quota = 0;
            if (body.contains("quota_gb")) {
                double gb = body.value("quota_gb").toDouble();
                if (gb > 0) quota = gbToBytes(gb);
            }
            if (quota <= 0 && body.contains("quota_bytes")) {
                quota = body.value("quota_bytes").toVariant().toLongLong();
            }
            if (quota <= 0) quota = GB; // default 1 GB

            if (email.isEmpty() || password.isEmpty()) {
                jsonError(res, 400, "email and password are required"); closeDb(dbName); return;
            }

            QString pwdHash = hashPassword(password);

            QSqlQuery q(db);
            q.prepare("INSERT INTO users (email, password_hash, quota_bytes, active, maildir) "
                      "VALUES (:email, :hash, :quota, true, '') RETURNING id");
            q.bindValue(":email", email);
            q.bindValue(":hash", pwdHash);
            q.bindValue(":quota", quota);

            if (!q.exec()) {
                jsonError(res, 409, "User already exists or DB error: " + q.lastError().text());
                closeDb(dbName); return;
            }
            q.next();
            int newId = q.value(0).toInt();

            // Auto-create Alibaba sender address
            createAliSender(email);

            QJsonObject resp;
            resp["id"] = newId;
            resp["email"] = email;
            resp["status"] = "created";
            res.status = 201;
            res.set_content(QJsonDocument(resp).toJson(QJsonDocument::Compact).toStdString(),
                            "application/json");
            closeDb(dbName);
        });

        // POST /api/users/batch — create many users
        m_server->Post("/api/users/batch", [&](const httplib::Request& req, httplib::Response& res) {
            if (!checkAuth(req, res)) return;
            QSqlDatabase db = openDb();
            if (!db.isOpen()) { jsonError(res, 500, "Database not connected"); return; }
            QString dbName = db.connectionName();

            QJsonParseError err;
            QJsonDocument doc = QJsonDocument::fromJson(QByteArray::fromStdString(req.body), &err);
            if (err.error != QJsonParseError::NoError) {
                jsonError(res, 400, "Invalid JSON"); closeDb(dbName); return;
            }
            QJsonArray users = doc.array();
            if (users.isEmpty()) { jsonError(res, 400, "Array of users required"); closeDb(dbName); return; }

            QJsonArray created;
            QJsonArray errors;
            int okCount = 0;

            for (int i = 0; i < users.size(); i++) {
                QJsonObject u = users[i].toObject();
                QString email = u.value("email").toString().trimmed();
                QString password = u.value("password").toString();
                qint64 quota = 0;
                if (u.contains("quota_gb")) {
                    double gb = u.value("quota_gb").toDouble();
                    if (gb > 0) quota = gbToBytes(gb);
                }
                if (quota <= 0 && u.contains("quota_bytes")) {
                    quota = u.value("quota_bytes").toVariant().toLongLong();
                }
                if (quota <= 0) quota = GB;

                if (email.isEmpty() || password.isEmpty()) {
                    QJsonObject e;
                    e["index"] = i;
                    e["error"] = "email and password required";
                    errors.append(e);
                    continue;
                }

                QSqlQuery q(db);
                q.prepare("INSERT INTO users (email, password_hash, quota_bytes, active, maildir) "
                          "VALUES (:email, :hash, :quota, true, '') RETURNING id");
                q.bindValue(":email", email);
                q.bindValue(":hash", hashPassword(password));
                q.bindValue(":quota", quota);

                if (!q.exec()) {
                    QJsonObject e;
                    e["index"] = i;
                    e["email"] = email;
                    e["error"] = q.lastError().text();
                    errors.append(e);
                    continue;
                }
                q.next();
                QJsonObject r;
                r["index"] = i;
                r["id"] = q.value(0).toInt();
                r["email"] = email;
                created.append(r);
                okCount++;

                // Auto-create Alibaba sender address
                createAliSender(email);
            }

            QJsonObject resp;
            resp["created"] = okCount;
            resp["total"] = users.size();
            resp["users"] = created;
            resp["errors"] = errors;
            res.set_content(QJsonDocument(resp).toJson(QJsonDocument::Compact).toStdString(),
                            "application/json");
            closeDb(dbName);
        });

        // PUT /api/users/:id — update user
        m_server->Put(R"(/api/users/(\d+))", [&](const httplib::Request& req, httplib::Response& res) {
            if (!checkAuth(req, res)) return;
            QSqlDatabase db = openDb();
            if (!db.isOpen()) { jsonError(res, 500, "Database not connected"); return; }
            QString dbName = db.connectionName();

            int userId = QString::fromStdString(req.matches[1]).toInt();

            QJsonParseError err;
            QJsonDocument doc = QJsonDocument::fromJson(QByteArray::fromStdString(req.body), &err);
            if (err.error != QJsonParseError::NoError) {
                jsonError(res, 400, "Invalid JSON"); closeDb(dbName); return;
            }
            QJsonObject body = doc.object();

            QSqlQuery q(db);

            if (body.contains("email")) {
                q.prepare("UPDATE users SET email=:v WHERE id=:id");
                q.bindValue(":v", body.value("email").toString().trimmed());
                q.bindValue(":id", userId);
                if (!q.exec() || q.numRowsAffected() == 0) {
                    jsonError(res, 404, "User not found or email conflict"); closeDb(dbName); return;
                }
            }
            if (body.contains("active")) {
                q.prepare("UPDATE users SET active=:v WHERE id=:id");
                q.bindValue(":v", body.value("active").toBool());
                q.bindValue(":id", userId);
                q.exec();
            }
            if (body.contains("quota_gb")) {
                double gb = body.value("quota_gb").toDouble();
                if (gb > 0) {
                    q.prepare("UPDATE users SET quota_bytes=:v WHERE id=:id");
                    q.bindValue(":v", gbToBytes(gb));
                    q.bindValue(":id", userId);
                    q.exec();
                }
            } else if (body.contains("quota_bytes")) {
                q.prepare("UPDATE users SET quota_bytes=:v WHERE id=:id");
                q.bindValue(":v", body.value("quota_bytes").toVariant().toLongLong());
                q.bindValue(":id", userId);
                q.exec();
            }
            if (body.contains("password")) {
                q.prepare("UPDATE users SET password_hash=:v WHERE id=:id");
                q.bindValue(":v", hashPassword(body.value("password").toString()));
                q.bindValue(":id", userId);
                q.exec();
            }
            if (body.contains("maildir")) {
                q.prepare("UPDATE users SET maildir=:v WHERE id=:id");
                q.bindValue(":v", body.value("maildir").toString());
                q.bindValue(":id", userId);
                q.exec();
            }

            res.set_content("{\"status\":\"updated\"}", "application/json");
            closeDb(dbName);
        });

        // POST /api/users/:id/reset-password
        m_server->Post(R"(/api/users/(\d+)/reset-password)", [&](const httplib::Request& req, httplib::Response& res) {
            if (!checkAuth(req, res)) return;
            QSqlDatabase db = openDb();
            if (!db.isOpen()) { jsonError(res, 500, "Database not connected"); return; }
            QString dbName = db.connectionName();

            int userId = QString::fromStdString(req.matches[1]).toInt();

            QJsonParseError err;
            QJsonDocument doc = QJsonDocument::fromJson(QByteArray::fromStdString(req.body), &err);
            if (err.error != QJsonParseError::NoError) {
                jsonError(res, 400, "Invalid JSON"); closeDb(dbName); return;
            }
            QString newPassword = doc.object().value("password").toString();
            if (newPassword.isEmpty()) {
                jsonError(res, 400, "password is required"); closeDb(dbName); return;
            }

            QSqlQuery q(db);
            q.prepare("UPDATE users SET password_hash=:hash WHERE id=:id");
            q.bindValue(":hash", hashPassword(newPassword));
            q.bindValue(":id", userId);
            if (!q.exec() || q.numRowsAffected() == 0) {
                jsonError(res, 404, "User not found"); closeDb(dbName); return;
            }

            res.set_content("{\"status\":\"password reset\"}", "application/json");
            closeDb(dbName);
        });

        // POST /api/users/:id/password — change password (requires old password)
        m_server->Post(R"(/api/users/(\d+)/password)", [&](const httplib::Request& req, httplib::Response& res) {
            if (!checkAuth(req, res)) return;
            QSqlDatabase db = openDb();
            if (!db.isOpen()) { jsonError(res, 500, "Database not connected"); return; }
            QString dbName = db.connectionName();

            int userId = QString::fromStdString(req.matches[1]).toInt();

            QJsonParseError err;
            QJsonDocument doc = QJsonDocument::fromJson(QByteArray::fromStdString(req.body), &err);
            if (err.error != QJsonParseError::NoError) {
                jsonError(res, 400, "Invalid JSON"); closeDb(dbName); return;
            }
            QString oldPassword = doc.object().value("old_password").toString();
            QString newPassword = doc.object().value("new_password").toString();
            if (oldPassword.isEmpty() || newPassword.isEmpty()) {
                jsonError(res, 400, "old_password and new_password are required"); closeDb(dbName); return;
            }

            // Verify old password
            QSqlQuery q(db);
            q.prepare("SELECT password_hash FROM users WHERE id=:id");
            q.bindValue(":id", userId);
            if (!q.exec() || !q.next()) {
                jsonError(res, 404, "User not found"); closeDb(dbName); return;
            }
            if (!verifyPassword(oldPassword, q.value(0).toString())) {
                jsonError(res, 403, "Incorrect old password"); closeDb(dbName); return;
            }

            // Set new password
            q.prepare("UPDATE users SET password_hash=:hash WHERE id=:id");
            q.bindValue(":hash", hashPassword(newPassword));
            q.bindValue(":id", userId);
            q.exec();

            res.set_content("{\"status\":\"password changed\"}", "application/json");
            closeDb(dbName);
        });

        // DELETE /api/users/:id — delete user
        m_server->Delete(R"(/api/users/(\d+))", [&](const httplib::Request& req, httplib::Response& res) {
            if (!checkAuth(req, res)) return;
            QSqlDatabase db = openDb();
            if (!db.isOpen()) { jsonError(res, 500, "Database not connected"); return; }
            QString dbName = db.connectionName();

            int userId = QString::fromStdString(req.matches[1]).toInt();
            QSqlQuery q(db);
            q.prepare("DELETE FROM users WHERE id=:id");
            q.bindValue(":id", userId);
            if (!q.exec() || q.numRowsAffected() == 0) {
                jsonError(res, 404, "User not found"); closeDb(dbName); return;
            }
            res.set_content("{\"status\":\"deleted\"}", "application/json");
            closeDb(dbName);
        });

        // GET /api/dashboard — summary stats
        m_server->Get("/api/dashboard", [&](const httplib::Request& req, httplib::Response& res) {
            if (!checkAuth(req, res)) return;
            QSqlDatabase db = openDb();
            if (!db.isOpen()) { jsonError(res, 500, "Database not connected"); return; }
            QString dbName = db.connectionName();

            QJsonObject d;
            QSqlQuery q(db);

            q.exec("SELECT COUNT(*) FROM users");
            if (q.next()) d["total_users"] = q.value(0).toInt();

            q.exec("SELECT COUNT(*) FROM users WHERE active=true");
            if (q.next()) d["active_users"] = q.value(0).toInt();

            q.exec("SELECT COUNT(*) FROM users WHERE active=false");
            if (q.next()) d["inactive_users"] = q.value(0).toInt();

            q.exec("SELECT COALESCE(SUM(quota_bytes),0) FROM users");
            qint64 totalQb = 0;
            if (q.next()) { totalQb = q.value(0).toLongLong(); d["total_quota_bytes"] = totalQb; }
            d["total_quota_gb"] = QString::number(bytesToGB(totalQb), 'f', 1);

            q.exec("SELECT COALESCE(SUM(LENGTH(COALESCE(maildir,''))),0) FROM users WHERE active=true");
            qint64 usedBytes = 0;
            if (q.next()) usedBytes = q.value(0).toLongLong();
            d["used_quota_bytes"] = usedBytes;
            d["used_quota_gb"] = QString::number(bytesToGB(usedBytes), 'f', 1);

            d["hostname"] = m_config.hostname();
            d["time"] = QDateTime::currentSecsSinceEpoch();

            res.set_content(QJsonDocument(d).toJson(QJsonDocument::Compact).toStdString(),
                            "application/json");
            closeDb(dbName);
        });

        // --- GET /health ---
        m_server->Get("/health", [&](const httplib::Request& req, httplib::Response& res) {
            Q_UNUSED(req);
            res.set_content("{\"status\":\"ok\"}", "application/json");
        });

        // --- GET /stats ---
        m_server->Get("/stats", [&](const httplib::Request& req, httplib::Response& res) {
            if (!checkAuth(req, res)) return;
            auto rlCfg = m_config.rateLimits();
            QJsonObject stats;
            stats["hostname"] = m_config.hostname();
            stats["outbound_mode"] = QStringLiteral("direct");
            stats["rate_limiting"] = rlCfg.enabled;
            stats["time"] = QDateTime::currentSecsSinceEpoch();
            res.set_content(QJsonDocument(stats).toJson(QJsonDocument::Compact).toStdString(),
                            "application/json");
        });

        // --- GET /config/status ---
        m_server->Get("/config/status", [&](const httplib::Request& req, httplib::Response& res) {
            if (!checkAuth(req, res)) return;
            auto oc = m_config.outbound();
            QJsonObject obj;
            obj["mode"] = QStringLiteral("direct");
            obj["spool_dir"] = oc.spoolDir;
            obj["maildir_base"] = m_config.maildirBasePath();
            obj["quota_bytes"] = m_config.quotaMaxBytes();
            obj["dkim_enabled"] = m_config.dkim().enabled;
            obj["tls_cert"] = !m_config.tlsCertFile().isEmpty();
            res.set_content(QJsonDocument(obj).toJson(QJsonDocument::Compact).toStdString(),
                            "application/json");
        });

        // --- Serve admin panel static file ---
        // Read the admin.html from the same directory as the binary (or from source)
        QString adminFilePath;
        QStringList searchPaths = {
            QDir::currentPath() + "/admin.html",
            QDir::currentPath() + "/../admin.html",
            QDir::currentPath() + "/src/admin.html",
        };
        for (const auto& p : searchPaths) {
            if (QFile::exists(p)) { adminFilePath = p; break; }
        }

        std::string adminHtml;
        if (!adminFilePath.isEmpty()) {
            QFile f(adminFilePath);
            if (f.open(QIODevice::ReadOnly)) {
                adminHtml = f.readAll().toStdString();
                qInfo() << "ApiServer: loaded admin panel from" << adminFilePath;
            }
        }
        if (adminHtml.empty()) {
            adminHtml = "<html><body><h1>Admin panel not found</h1></body></html>";
            qWarning() << "ApiServer: admin.html not found";
        }

        m_server->Get("/admin", [adminHtml](const httplib::Request& req, httplib::Response& res) {
            Q_UNUSED(req);
            res.set_content(adminHtml, "text/html");
        });

        // Root redirects to /admin
        m_server->Get("/", [&](const httplib::Request& req, httplib::Response& res) {
            Q_UNUSED(req);
            res.set_redirect("/admin");
        });

        // --- Webmail: user API + static UI ---
        if (m_webmailServices.isValid()) {
            m_webmail = std::make_unique<WebmailApi>(m_config, m_webmailServices);
            m_webmail->registerRoutes(*m_server);
            // Base64 attachments inflate 25 MB of mail to ~34 MB of JSON.
            m_server->set_payload_max_length(40 * 1024 * 1024);

            m_server->set_file_extension_and_mimetype_mapping("webmanifest", "application/manifest+json");
            const QStringList webmailDirs = {QDir::currentPath() + "/webmail",
                                             QDir::currentPath() + "/../webmail"};
            for (const QString& dir : webmailDirs) {
                if (QDir(dir).exists() && m_server->set_mount_point("/webmail", dir.toStdString())) {
                    qInfo() << "ApiServer: webmail UI served from" << dir;
                    break;
                }
            }
            m_server->Get("/webmail", [](const httplib::Request&, httplib::Response& res) {
                res.set_redirect("/webmail/");
            });
        }

        // Set global error handler (only for truly unmatched routes)
        m_server->set_error_handler([&](const httplib::Request& req, httplib::Response& res) {
            Q_UNUSED(req);
            // Only overwrite if body is empty (route handler didn't set a response)
            if (res.body.empty()) {
                res.status = 404;
                res.set_content("{\"error\":\"not found\"}", "application/json");
            }
        });

        // --- Listen (blocking in this worker thread) ---
        qInfo() << "ApiServer: listening on 0.0.0.0:" << m_port;
        m_server->listen("0.0.0.0", m_port);

        qInfo() << "ApiServer: stopped";
    };

    m_thread = QThread::create(worker);
    m_thread->start();
    return true;
}

// ---------------------------------------------------------------------------
// stop  – FIX CRITICAL BUG #2
// ---------------------------------------------------------------------------
void ApiServer::stop() {
    if (!m_thread) return;

    qInfo() << "ApiServer: stopping...";
    m_stopFlag = true;

    // httplib's stop() must be called from a different thread than the
    // one running listen().  We are in the main thread, the server runs in
    // the worker thread – so calling stop() from here is CORRECT.
    if (m_server) {
        m_server->stop();
    }

    m_thread->quit();
    m_thread->wait(5000);

    m_server.reset();
    m_thread->deleteLater();
    m_thread = nullptr;
    qInfo() << "ApiServer: stopped cleanly";
}
