#ifndef LOGGER_H
#define LOGGER_H

#include <QString>
#include <QDebug>
#include <QDateTime>
#include <QFile>
#include <QTextStream>

/**
 * Centralized logging utility.
 * Standardizes on: qInfo() for info, qWarning() for warnings, qCritical() for errors.
 * Also writes to a log file if configured.
 */
class Logger {
public:
    static void init(const QString& logFilePath = QString());

private:
    static void messageHandler(QtMsgType type, const QMessageLogContext& ctx, const QString& msg);
    static QFile* s_logFile;
    static QTextStream* s_logStream;
};

#endif // LOGGER_H
