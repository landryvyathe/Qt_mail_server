#include "Logger.h"

QFile*       Logger::s_logFile   = nullptr;
QTextStream* Logger::s_logStream = nullptr;

void Logger::init(const QString& logFilePath) {
    qInstallMessageHandler(messageHandler);

    if (!logFilePath.isEmpty()) {
        s_logFile = new QFile(logFilePath);
        if (s_logFile->open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
            s_logStream = new QTextStream(s_logFile);
            qInfo() << "Logger: log file =" << logFilePath;
        } else {
            delete s_logFile;
            s_logFile = nullptr;
        }
    }
}

void Logger::messageHandler(QtMsgType type, const QMessageLogContext& ctx, const QString& msg) {
    Q_UNUSED(ctx);

    QString timestamp = QDateTime::currentDateTime().toString("yyyy-MM-dd hh:mm:ss.zzz");
    QString level;

    switch (type) {
    case QtDebugMsg:    level = "DBG"; break;
    case QtInfoMsg:     level = "INF"; break;
    case QtWarningMsg:  level = "WRN"; break;
    case QtCriticalMsg: level = "ERR"; break;
    case QtFatalMsg:    level = "FAT"; break;
    }

    QString line = QString("[%1] [%2] %3").arg(timestamp, level, msg);

    // Print to stderr (default Qt behavior)
    fprintf(stderr, "%s\n", qPrintable(line));
    fflush(stderr);

    // Write to log file
    if (s_logStream && s_logFile && s_logFile->isOpen()) {
        *s_logStream << line << "\n";
        s_logStream->flush();
    }
}
