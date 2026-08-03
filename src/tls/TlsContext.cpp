#include "TlsContext.h"
#include <QByteArray>
#include <QFile>
#include <QList>
#include <QSslCertificate>
#include <QSslKey>
#include <QDebug>
#include <QJsonObject>

TlsContext& TlsContext::instance() {
    static TlsContext inst;
    return inst;
}

void TlsContext::init(const ConfigManager& config) {
    QJsonObject tlsConfig = config.getRawConfig().value("tls").toObject();
    QString certPath = tlsConfig.value("cert_file").toString();
    QString keyPath = tlsConfig.value("key_file").toString();

    if (certPath.isEmpty() || keyPath.isEmpty()) {
        qWarning() << "TLS Context: No certificate or key provided in config. TLS will be disabled.";
        return;
    }

    QFile certFile(certPath);
    if (!certFile.open(QIODevice::ReadOnly)) {
        qWarning() << "TLS Context: Failed to open certificate file:" << certPath;
        return;
    }
    QByteArray certData = certFile.readAll();
    QList<QSslCertificate> certChain = QSslCertificate::fromData(certData, QSsl::Pem);
    if (certChain.isEmpty()) {
        qWarning() << "TLS Context: Failed to parse certificate in" << certPath;
        return;
    }
    QSslCertificate cert = certChain.first();
    
    QFile keyFile(keyPath);
    if (!keyFile.open(QIODevice::ReadOnly)) {
        qWarning() << "TLS Context: Failed to open key file:" << keyPath;
        return;
    }
    QSslKey key(&keyFile, QSsl::Rsa, QSsl::Pem);
    if (key.isNull()) {
        // Try EC key
        keyFile.seek(0);
        key = QSslKey(&keyFile, QSsl::Ec, QSsl::Pem);
    }
    if (key.isNull()) {
        qWarning() << "TLS Context: Failed to parse private key in" << keyPath;
        return;
    }

    m_sslConfig = QSslConfiguration::defaultConfiguration();
    m_sslConfig.setLocalCertificateChain(certChain);
    m_sslConfig.setPrivateKey(key);
    m_sslConfig.setProtocol(QSsl::TlsV1_2OrLater);
    
    m_configured = true;
    qInfo() << "TLS Context: Successfully loaded certificate and key.";
}

QSslConfiguration TlsContext::getConfiguration() const {
    return m_sslConfig;
}

bool TlsContext::isConfigured() const {
    return m_configured;
}
