#ifndef CONFIGMANAGER_H
#define CONFIGMANAGER_H

#include <QJsonObject>
#include <QJsonArray>
#include <QJsonDocument>
#include <QString>
#include <QHostAddress>
#include <QStringList>
#include <QList>

class ConfigManager {
public:
    explicit ConfigManager(const QString& configFilePath);

    // Basic
    const QJsonObject& getRawConfig() const { return m_config; }
    QList<QHostAddress> interfaces() const;

    // Ports
    int port(const QString& service) const;

    // Storage
    QString maildirBasePath() const;

    // Quota
    qint64 quotaMaxBytes() const;
    int quotaSoftPercent() const;

    // Outbound
    struct RelayConfig {
        bool enabled = false;
        QString host;
        int port = 25;
        QString authMethod; // "LOGIN" or "PLAIN"
        QString username;
        QString password;
        bool useTls = false;      // implicit TLS on connect (SMTPS, e.g. 465)
        bool useStartTls = false; // opportunistic STARTTLS after EHLO
    };

    struct OutboundConfig {
        QString spoolDir;
        RelayConfig relay;
    };
    OutboundConfig outbound() const;

    // API
    struct ApiConfig {
        QString tokenHash;
        int rateLimitRpm;
    };
    ApiConfig apiConfig() const;

    // Rate limits
    struct RateLimitConfig {
        bool enabled;
        int maxConnectionsPerIp;
        int windowSeconds;
        int smtpRatePerWindow;
        int authAttemptsPerWindow;
    };
    RateLimitConfig rateLimits() const;

    // Rspamd
    struct RspamdConfig {
        QString host;
        int port;
    };
    RspamdConfig rspamdConfig() const;

    // DKIM
    struct DkimConfig {
        bool enabled;
        QString domain;
        QString selector;
        QString privateKeyPath;
        QStringList headersToSign;
    };
    DkimConfig dkim() const;

    // Hostname
    QString hostname() const;

    // Local domains (email domains this server handles)
    QStringList localDomains() const;

    // TLS
    QString tlsCertFile() const;
    QString tlsKeyFile() const;

    // Database
    QJsonObject database() const;

    // Runtime config updates (writes to in-memory config, mutable for const access)
    void setCatchAllEnabled(bool enabled) const;
    void reload(const QString& configFilePath) const;

private:
    mutable QJsonObject m_config;
};

#endif // CONFIGMANAGER_H
