#ifndef APISERVER_H
#define APISERVER_H

#include <QObject>
#include <QThread>
#include <memory>

// forward declaration – httplib is a header-only library
namespace httplib {
    class Server;
}

class ConfigManager;
#include "WebmailApi.h"

class ApiServer : public QObject {
    Q_OBJECT
public:
    explicit ApiServer(const ConfigManager& config, const QString& configFilePath, QObject* parent = nullptr);
    ~ApiServer();

    bool start();
    void stop();
    // Enables the user-facing webmail API and static UI; call before start().
    void setWebmailServices(const WebmailServices& services) { m_webmailServices = services; }

private:
    const ConfigManager& m_config;
    QString m_configFilePath;

    // server runs in its own thread and is stopped via m_stopFlag
    std::unique_ptr<httplib::Server> m_server;  // owned here, accessed only from worker thread
    QThread*  m_thread    = nullptr;
    bool      m_stopFlag   = false;
    int       m_port       = 0;
    WebmailServices m_webmailServices;
    std::unique_ptr<WebmailApi> m_webmail;
};

#endif // APISERVER_H
