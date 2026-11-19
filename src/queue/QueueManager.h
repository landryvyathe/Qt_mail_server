#ifndef QUEUEMANAGER_H
#define QUEUEMANAGER_H

#include <QObject>
#include <QString>
#include "OutboundWorker.h"

class MimeParser;
class ConfigManager;

class QueueManager : public QObject {
    Q_OBJECT
public:
    explicit QueueManager(const ConfigManager& config, const QString& configFilePath, QObject* parent = nullptr);
    ~QueueManager();

    bool init();
    bool spoolMessage(const QByteArray& mimeMessage, const QString& envelopeFrom,
                      const QStringList& envelopeRcpt);
    void start();
    void stop();

private:
    const ConfigManager& m_config;
    QString m_configFilePath;
    QString m_spoolDir;

    QThread*          m_workerThread = nullptr;
    OutboundWorker*   m_worker       = nullptr;
};

#endif // QUEUEMANAGER_H
