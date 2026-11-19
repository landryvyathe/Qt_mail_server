#include "ConfigManager.h"
#include <QJsonDocument>
#include <QJsonArray>
#include <QFile>
#include <QDebug>

ConfigManager::ConfigManager(const QString& configFilePath) {
    QFile file(configFilePath);
    if (!file.open(QIODevice::ReadOnly)) {
        qCritical() << "ConfigManager: Failed to open config file:" << configFilePath;
        return;
    }

    QByteArray data = file.readAll();
    file.close();

    QJsonParseError parseError;
    QJsonDocument doc = QJsonDocument::fromJson(data, &parseError);
    if (parseError.error != QJsonParseError::NoError) {
        qCritical() << "ConfigManager: Failed to parse JSON:" << parseError.errorString();
        return;
    }

    m_config = doc.object();
    qInfo() << "ConfigManager: Configuration loaded successfully.";
}

QList<QHostAddress> ConfigManager::interfaces() const {
    QList<QHostAddress> addrs;
    QJsonArray arr = m_config.value("interfaces").toArray();
    for (const QJsonValue& val : arr) {
        QHostAddress addr(val.toString());
        if (!addr.isNull()) {
            addrs.append(addr);
        }
    }
    if (addrs.isEmpty()) {
        addrs.append(QHostAddress::Any);
    }
    return addrs;
}

int ConfigManager::port(const QString& service) const {
    QJsonObject ports = m_config.value("ports").toObject();
    return ports.value(service).toInt(0);
}

QString ConfigManager::maildirBasePath() const {
    return m_config.value("maildir_base_path").toString("./maildir");
}

qint64 ConfigManager::quotaMaxBytes() const {
    return m_config.value("quota_max_bytes").toVariant().toLongLong();
}

int ConfigManager::quotaSoftPercent() const {
    return m_config.value("quota_soft_percent").toInt(80);
}

ConfigManager::OutboundConfig ConfigManager::outbound() const {
    OutboundConfig cfg;
    QJsonObject obj = m_config.value("outbound").toObject();
    cfg.spoolDir = obj.value("spool_dir").toString("./spool");

    QJsonObject relayObj = obj.value("relay").toObject();
    cfg.relay.enabled     = relayObj.value("enabled").toBool(false);
    cfg.relay.host        = relayObj.value("host").toString();
    cfg.relay.port        = relayObj.value("port").toInt(25);
    cfg.relay.authMethod  = relayObj.value("auth_method").toString("LOGIN").toUpper();
    cfg.relay.username    = relayObj.value("username").toString();
    cfg.relay.password    = relayObj.value("password").toString();
    cfg.relay.useTls      = relayObj.value("use_tls").toBool(false);
    cfg.relay.useStartTls = relayObj.value("use_starttls").toBool(false);
    const QJsonObject senderPw = relayObj.value("sender_passwords").toObject();
    for (auto it = senderPw.begin(); it != senderPw.end(); ++it)
        cfg.relay.senderPasswords.insert(it.key().toLower(), it.value().toString());

    return cfg;
}

ConfigManager::ApiConfig ConfigManager::apiConfig() const {
    ApiConfig cfg;
    QJsonObject obj = m_config.value("api").toObject();
    cfg.tokenHash   = obj.value("token_hash").toString();
    cfg.rateLimitRpm = obj.value("rate_limit_rpm").toInt(30);
    return cfg;
}

ConfigManager::RateLimitConfig ConfigManager::rateLimits() const {
    RateLimitConfig cfg;
    QJsonObject obj = m_config.value("rate_limits").toObject();
    cfg.enabled               = obj.value("enabled").toBool(true);
    cfg.maxConnectionsPerIp   = obj.value("max_connections_per_ip").toInt(10);
    cfg.windowSeconds         = obj.value("window_seconds").toInt(60);
    cfg.smtpRatePerWindow     = obj.value("smtp_rate_per_window").toInt(30);
    cfg.authAttemptsPerWindow = obj.value("auth_attempts_per_window").toInt(5);
    return cfg;
}

ConfigManager::RspamdConfig ConfigManager::rspamdConfig() const {
    RspamdConfig cfg;
    QJsonObject obj = m_config.value("rspamd").toObject();
    cfg.host = obj.value("host").toString("127.0.0.1");
    cfg.port = obj.value("port").toInt(11333);
    return cfg;
}

ConfigManager::DkimConfig ConfigManager::dkim() const {
    DkimConfig cfg;
    QJsonObject obj = m_config.value("dkim").toObject();
    cfg.enabled        = obj.value("enabled").toBool(false);
    cfg.domain         = obj.value("domain").toString();
    cfg.selector       = obj.value("selector").toString("mail");
    cfg.privateKeyPath = obj.value("private_key_path").toString();
    QJsonArray hdrs = obj.value("headers_to_sign").toArray();
    for (const auto& h : hdrs)
        cfg.headersToSign << h.toString();
    return cfg;
}

QString ConfigManager::hostname() const {
    return m_config.value("hostname").toString("localhost");
}

QStringList ConfigManager::localDomains() const {
    QStringList domains;
    QJsonArray arr = m_config.value("local_domains").toArray();
    for (const QJsonValue& v : arr)
        domains << v.toString().toLower().trimmed();
    if (domains.isEmpty()) {
        // fallback: use hostname as the only local domain
        domains << hostname().toLower().trimmed();
    }
    return domains;
}

QString ConfigManager::tlsCertFile() const {
    return m_config.value("tls").toObject().value("cert_file").toString();
}

QString ConfigManager::tlsKeyFile() const {
    return m_config.value("tls").toObject().value("key_file").toString();
}

QJsonObject ConfigManager::database() const {
    return m_config.value("database").toObject();
}

void ConfigManager::setCatchAllEnabled(bool enabled) const {
    m_config["catch_all_enabled"] = enabled;
}

void ConfigManager::reload(const QString& configFilePath) const {
    QFile file(configFilePath);
    if (!file.open(QIODevice::ReadOnly)) {
        qWarning() << "ConfigManager::reload: Failed to open config file:" << configFilePath;
        return;
    }
    QByteArray data = file.readAll();
    file.close();
    QJsonParseError parseError;
    QJsonDocument doc = QJsonDocument::fromJson(data, &parseError);
    if (parseError.error != QJsonParseError::NoError) {
        qWarning() << "ConfigManager::reload: Failed to parse JSON:" << parseError.errorString();
        return;
    }
    m_config = doc.object();
    qInfo() << "ConfigManager: Configuration reloaded.";
}
