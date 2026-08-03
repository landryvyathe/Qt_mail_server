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

class ApiServer : public QObject {
    Q_OBJECT
public:
    explicit ApiServer(const ConfigManager& config, QObject* parent = nullptr);
    ~ApiServer();

    bool start();
    void stop();

private:
    const ConfigManager& m_config;

    // server runs in its own thread and is stopped via m_stopFlag
    std::unique_ptr<httplib::Server> m_server;  // owned here, accessed only from worker thread
    QThread*  m_thread    = nullptr;
    bool      m_stopFlag   = false;
    int       m_port       = 0;
};

#endif // APISERVER_H
