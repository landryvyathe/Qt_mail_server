#ifndef TLSCONTEXT_H
#define TLSCONTEXT_H

#include <QSslConfiguration>
#include <QString>
#include "../ConfigManager.h"

class TlsContext {
public:
    static TlsContext& instance();
    void init(const ConfigManager& config);

    QSslConfiguration getConfiguration() const;
    bool isConfigured() const;

private:
    TlsContext() = default;
    Q_DISABLE_COPY_MOVE(TlsContext)

    QSslConfiguration m_sslConfig;
    bool m_configured = false;
};

#endif // TLSCONTEXT_H
