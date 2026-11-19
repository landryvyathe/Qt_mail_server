#include <QCoreApplication>
#include <QCommandLineParser>
#include <QThreadPool>
#include <QTimer>
#include <QDebug>
#include <QThread>
#include <QDateTime>
#include <QLocale>
#include <QRandomGenerator>
#include <QDir>

#include "src/ConfigManager.h"
#include "src/systemd/ServiceNotifier.h"
#include "src/auth/AuthManager.h"
#include "src/auth/UserRepository.h"
#include "src/tls/TlsContext.h"
#include "src/storage/MaildirStorage.h"
#include "src/queue/QueueManager.h"
#include "src/api/ApiServer.h"
#include "src/services/SmtpServer.h"
#include "src/services/ImapServer.h"
#include "src/services/Pop3Server.h"

// Build a simple RFC 2822 MIME message
static QByteArray buildTestMessage(const QString& from, const QString& to, const QString& subject) {
    QString dateStr = QLocale::c().toString(
        QDateTime::currentDateTimeUtc(), QStringLiteral("ddd, dd MMM yyyy hh:mm:ss +0000"));

    QString msg;
    msg += "Date: " + dateStr + "\r\n";
    msg += "From: <" + from + ">\r\n";
    msg += "To: <" + to + ">\r\n";
    msg += "Subject: " + subject + "\r\n";
    msg += "Message-ID: <" + QDateTime::currentDateTimeUtc().toString("yyyyMMddHHmmss")
           + "." + QString::number(QRandomGenerator::global()->bounded(999999))
           + "@mailserver>\r\n";
    msg += "MIME-Version: 1.0\r\n";
    msg += "Content-Type: text/plain; charset=UTF-8\r\n";
    msg += "\r\n";
    msg += "This is a test email sent via direct SMTP from the MailServer.\r\n";
    msg += "\r\n";
    return msg.toUtf8();
}

int main(int argc, char* argv[]) {
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName("MailServer");
    QCoreApplication::setApplicationVersion("1.0.0");

    // --- CLI ---
    QCommandLineParser parser;
    parser.setApplicationDescription("C++ Mail Server (SMTP/IMAP/POP3)");
    parser.addHelpOption();
    parser.addVersionOption();
    QCommandLineOption configOption(QStringList() << "c" << "config",
        "Path to config file.", "path", "./config.json");
    parser.addOption(configOption);

    // -- create-user mode --
    QCommandLineOption createUserOption("create-user",
        "Create a new mail user  (--create-user user@domain)", "email");
    parser.addOption(createUserOption);
    QCommandLineOption passwordOption("password",
        "Password for --create-user", "password");
    parser.addOption(passwordOption);

    // -- send-test mode --
    QCommandLineOption sendTestOption("send-test",
        "Send a test email through the outbound queue");
    parser.addOption(sendTestOption);
    QCommandLineOption fromOption("from",
        "Envelope-from address for --send-test", "email");
    parser.addOption(fromOption);
    QCommandLineOption toOption("to",
        "Envelope-to address for --send-test", "email");
    parser.addOption(toOption);
    QCommandLineOption subjectOption("subject",
        "Subject for --send-test (default: \"Test email from MailServer\")", "text");
    parser.addOption(subjectOption);

    parser.process(app);

    QString configPath = parser.value(configOption);
    qInfo() << "MailServer: loading config from" << configPath;

    // --- Config ---
    ConfigManager config(configPath);
    if (config.getRawConfig().isEmpty()) {
        qCritical() << "MailServer: Failed to load config. Exiting.";
        return 1;
    }
    qInfo() << "MailServer: config loaded OK, hostname =" << config.hostname();

    // --- Auth ---
    AuthManager authManager;
    {
        QJsonObject db = config.database();
        QString host = db.value("host").toString("127.0.0.1");
        int port     = db.value("port").toInt(5432);
        QString name = db.value("name").toString("mailserver");
        QString user = db.value("user").toString("mailserver");
        QString pass = db.value("password").toString();
        if (!authManager.init(host, port, name, user, pass)) {
            qCritical() << "MailServer: Auth DB init failed. Exiting.";
            return 1;
        }
    }

    // ---- create-user mode ----
    if (parser.isSet(createUserOption)) {
        QString email    = parser.value(createUserOption).trimmed();
        QString password = parser.value(passwordOption).trimmed();

        if (email.isEmpty()) {
            qCritical() << "--create-user requires an email address";
            return 1;
        }
        if (password.isEmpty()) {
            qCritical() << "--create-user requires --password";
            return 1;
        }
        if (!email.contains('@')) {
            qCritical() << "--create-user: invalid email format, expected user@domain";
            return 1;
        }

        bool ok = authManager.createUser(email, password);
        if (ok) {
            qInfo() << "User created successfully:" << email;
            return 0;
        } else {
            qCritical() << "Failed to create user:" << email;
            return 1;
        }
    }

    // ---- send-test mode ----
    if (parser.isSet(sendTestOption)) {
        QString from    = parser.value(fromOption).trimmed();
        QString to      = parser.value(toOption).trimmed();
        QString subject = parser.value(subjectOption).trimmed();
        if (subject.isEmpty())
            subject = QStringLiteral("Test email from MailServer");

        if (from.isEmpty() || to.isEmpty()) {
            qCritical() << "--send-test requires both --from and --to";
            return 1;
        }

        // Build the test message
        QByteArray mime = buildTestMessage(from, to, subject);

        // Init queue manager (OutboundWorker will pick up the spooled message)
        QueueManager queueMgr(config, configPath);
        queueMgr.init();
        queueMgr.start();

        // Spool the test message
        queueMgr.spoolMessage(mime, from, {to});
        qInfo() << "Test email spooled: from=" << from << "to=" << to;

        // Run event loop — quit after 120 seconds or when worker finishes
        QTimer::singleShot(120000, &app, &QCoreApplication::quit);
        int ret = app.exec();

        // Check result
        QString spoolPath = config.outbound().spoolDir;
        QStringList delivered = QDir(spoolPath + "/delivered").entryList({"*.eml"}, QDir::Files);
        QStringList failed    = QDir(spoolPath + "/failed").entryList({"*.eml"}, QDir::Files);
        QStringList deferred  = QDir(spoolPath + "/deferred").entryList({"*.eml"}, QDir::Files);

        if (!delivered.isEmpty()) {
            qInfo() << "SUCCESS: Test email delivered!" << delivered;
            return 0;
        } else if (!failed.isEmpty()) {
            qCritical() << "FAILED: Test email in failed/ folder" << failed;
            return 1;
        } else if (!deferred.isEmpty()) {
            qWarning() << "DEFERRED: Test email in deferred/ folder (will retry)" << deferred;
            return 2;
        } else {
            qWarning() << "UNKNOWN: Test email not found in delivered/failed/deferred";
            return 3;
        }
    }

    // --- TLS ---
    TlsContext::instance().init(config);
    qInfo() << "MailServer: TLS" << (TlsContext::instance().isConfigured() ? "enabled" : "disabled");

    // --- Storage ---
    MaildirStorage storage;
    if (!storage.init(config.maildirBasePath(), config.quotaMaxBytes(), config.quotaSoftPercent())) {
        qCritical() << "MailServer: Storage init failed. Exiting.";
        return 1;
    }

    // --- Queue ---
    QueueManager queueMgr(config, configPath);
    queueMgr.init();
    queueMgr.start();

    // --- API ---
    ApiServer* apiServer = new ApiServer(config, configPath);
    apiServer->setWebmailServices({&storage, &authManager, &queueMgr});
    apiServer->start();
    QObject::connect(&app, &QCoreApplication::aboutToQuit, [apiServer]() {
        apiServer->stop();
        apiServer->deleteLater();
    });
    qInfo() << "MailServer: API server started on port" << config.port("api");

    // --- SMTP (plain + submission) ---
    SmtpServer smtpServer(config, false, &storage, &authManager, &queueMgr);
    if (!smtpServer.start()) {
        qCritical() << "MailServer: Failed to start SMTP. Exiting.";
        return 1;
    }

    // --- SMTPS (implicit TLS) ---
    SmtpServer* smtpsServer = nullptr;
    if (config.port("smtps") > 0) {
        smtpsServer = new SmtpServer(config, true, &storage, &authManager, &queueMgr, &app);
        if (!smtpsServer->start()) {
            qWarning() << "MailServer: SMTPS failed. Continuing without it.";
            smtpsServer->deleteLater();
            smtpsServer = nullptr;
        }
    }

    // --- IMAP ---
    ImapServer imapServer(config, false, &storage, &authManager);
    if (!imapServer.start()) {
        qCritical() << "MailServer: Failed to start IMAP. Exiting.";
        return 1;
    }

    ImapServer imapsServer(config, true, &storage, &authManager);
    if (!imapsServer.start()) {
        qWarning() << "MailServer: IMAPS failed. Continuing without it.";
    }

    // --- POP3 ---
    Pop3Server pop3Server(config, false, &storage, &authManager);
    if (!pop3Server.start()) {
        qCritical() << "MailServer: Failed to start POP3. Exiting.";
        return 1;
    }

    Pop3Server pop3sServer(config, true, &storage, &authManager);
    if (!pop3sServer.start()) {
        qWarning() << "MailServer: POP3S failed. Continuing without it.";
    }

    // --- Thread pool tuning ---
    QThreadPool::globalInstance()->setMaxThreadCount(50);
    qInfo() << "MailServer: thread pool max =" << QThreadPool::globalInstance()->maxThreadCount();

    // --- Systemd ---
    ServiceNotifier::notifyReady();

    int watchdogInterval = config.getRawConfig().value("watchdog_interval_seconds").toInt(10);
    QTimer* watchdogTimer = new QTimer(&app);
    QObject::connect(watchdogTimer, &QTimer::timeout, []() {
        ServiceNotifier::notifyWatchdog();
    });
    watchdogTimer->start(watchdogInterval * 1000);

    qInfo() << "MailServer: running on" << config.hostname();
    return app.exec();
}
