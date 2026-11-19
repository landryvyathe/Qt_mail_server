#include "QueueManager.h"
#include "../parser/MimeParser.h"
#include "../ConfigManager.h"

#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QThread>
#include <QDateTime>
#include <QDebug>

QueueManager::QueueManager(const ConfigManager& config, const QString& configFilePath, QObject* parent)
    : QObject(parent), m_config(config), m_configFilePath(configFilePath) {

    auto oc = m_config.outbound();
    m_spoolDir = oc.spoolDir.isEmpty() ? QStringLiteral("./spool") : oc.spoolDir;
}

QueueManager::~QueueManager() {
    stop();
}

bool QueueManager::init() {
    QDir root(m_spoolDir);
    bool ok = root.mkpath("new");
    root.mkpath("deferred");
    root.mkpath("failed");
    root.mkpath("delivered");
    qInfo() << "QueueManager: spool root =" << m_spoolDir;
    return ok;
}

bool QueueManager::spoolMessage(const QByteArray& mimeMessage,
                                 const QString& envelopeFrom,
                                 const QStringList& envelopeRcpt) {

    // --- FIX CRITICAL BUG #3: MIME parse failure MUST reject the message ---
    MimeParser parser;
    if (!parser.parse(mimeMessage)) {
        qCritical() << "QueueManager: MIME parse FAILED – message REJECTED."
                     << "Parser error:" << parser.lastError();
        return false;
    }

    // --- generate unique filename ---
    QString fileName = QString::number(QDateTime::currentMSecsSinceEpoch()) + ".eml";
    QString filePath = m_spoolDir + "/new/" + fileName;

    QFile file(filePath);
    if (!file.open(QIODevice::WriteOnly)) {
        qCritical() << "QueueManager: Cannot write spool file:" << filePath;
        return false;
    }

    // --- Write metadata as JSON envelope header ---
    QJsonObject meta;
    meta["from"] = envelopeFrom;
    QJsonArray arr;
    for (const QString& r : envelopeRcpt) arr.append(r);
    meta["rcpt"]  = arr;
    meta["queued"] = QDateTime::currentMSecsSinceEpoch();

    QByteArray jsonLine = QJsonDocument(meta).toJson(QJsonDocument::Compact);
    file.write(jsonLine);
    file.write("\n");

    // --- Write MIME body with CORRECT \r\n line endings (FIX CRITICAL BUG #4) ---
    QByteArray normalized = mimeMessage;
    // Normalize line endings: ensure all lines end with \r\n
    normalized.replace("\r\n", "\n");     // collapse existing CRLF to LF
    normalized.replace("\r", "\n");       // bare CR → LF
    normalized.replace("\n", "\r\n");     // LF → CRLF

    file.write(normalized);
    file.close();

    qInfo() << "QueueManager: spooled message to" << filePath
            << "from:" << envelopeFrom << "to:" << envelopeRcpt;
    return true;
}

void QueueManager::start() {
    m_workerThread = new QThread(this);
    m_worker = new OutboundWorker(m_config, m_configFilePath);
    m_worker->moveToThread(m_workerThread);

    connect(m_workerThread, &QThread::started,  m_worker, &OutboundWorker::start);
    connect(m_workerThread, &QThread::finished, m_worker, &QObject::deleteLater);
    connect(m_workerThread, &QThread::finished, m_workerThread, &QObject::deleteLater);

    m_workerThread->start();
}

void QueueManager::stop() {
    if (m_worker) {
        m_worker->stop();
    }
    if (m_workerThread) {
        m_workerThread->quit();
        m_workerThread->wait(5000);
    }
}
