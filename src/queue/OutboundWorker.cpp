#include "OutboundWorker.h"

#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QJsonDocument>
#include <QJsonObject>
#include <QHostInfo>
#include <QDnsLookup>
#include <QEventLoop>
#include <QDateTime>
#include <QCryptographicHash>
#include <QTimer>
#include <QDebug>
#include <QProcess>

// ---------------------------------------------------------------------------
// constructor
// ---------------------------------------------------------------------------
OutboundWorker::OutboundWorker(const ConfigManager& config, const QString& configFilePath, QObject* parent)
    : QObject(parent), m_config(config), m_configFilePath(configFilePath), m_stopping(false), m_maxRetries(8) {

    auto oc = m_config.outbound();
    m_spoolDir = oc.spoolDir.isEmpty() ? QStringLiteral("./spool") : oc.spoolDir;
    m_hostname = m_config.hostname();
    m_relay = oc.relay;

    m_timer = new QTimer(this);
    connect(m_timer, &QTimer::timeout, this, &OutboundWorker::processQueue);
}

OutboundWorker::~OutboundWorker() {
    stop();
}

void OutboundWorker::start() {
    qInfo() << "OutboundWorker: starting, spool =" << m_spoolDir;
    if (m_relay.enabled) {
        qInfo() << "OutboundWorker: relay enabled ->" << m_relay.host << ":" << m_relay.port
                << "auth=" << m_relay.authMethod;
    }
    // ensure spool directory exists
    QDir root(m_spoolDir);
    root.mkpath("new");
    root.mkpath("deferred");
    root.mkpath("failed");
    root.mkpath("delivered");

    loadPendingFromDisk();
    m_stopping = false;
    m_timer->start(5000);  // check queue every 5 s
}

void OutboundWorker::stop() {
    m_stopping = true;
    m_timer->stop();
}

// ---------------------------------------------------------------------------
// queue processing (called by timer)
// ---------------------------------------------------------------------------
void OutboundWorker::processQueue() {
    if (m_stopping) return;

    // reload deferred items whose backoff has passed
    loadPendingFromDisk();

    while (!m_queue.isEmpty()) {
        if (m_stopping) break;

        OutboundEmail& email = m_queue.head();

        // honour backoff
        qint64 now = QDateTime::currentSecsSinceEpoch();
        if (email.nextAttemptAt > now) {
            break; // still in back-off, keep in queue
        }

        email.retryCount++;

        qInfo() << "OutboundWorker: delivering" << email.filePath
                << "attempt" << email.retryCount << "/" << m_maxRetries;

        bool delivered = false;

        auto result = m_relay.enabled ? deliverViaRelay(email) : deliverDirect(email);
        if (result.temporary) {
            markDeferred(email, result.errorText);
        } else if (!result.errorText.isEmpty()) {
            qCritical() << "OutboundWorker: permanent failure:" << result.errorText;
            markFailed(email);
        } else {
            delivered = true;
        }

        if (delivered) {
            markDelivered(email);
            m_queue.dequeue();
        } else {
            // not delivered – if retries exhausted, move to failed/
            if (email.retryCount >= m_maxRetries) {
                qWarning() << "OutboundWorker: exhausted retries for" << email.filePath;
                markFailed(email);
            }
            m_queue.dequeue(); // dequeued; file already moved to deferred/ or failed/
        }
    }
}

// ---------------------------------------------------------------------------
// MX lookup (synchronous via local event loop – called from a timer slot)
// ---------------------------------------------------------------------------
QByteArray OutboundWorker::resolveMX(const QString& domain) {
    QDnsLookup dns;
    dns.setType(QDnsLookup::MX);
    dns.setName(domain);

    QEventLoop loop;
    connect(&dns, &QDnsLookup::finished, &loop, &QEventLoop::quit);
    dns.lookup();
    loop.exec();

    if (dns.error() != QDnsLookup::NoError) {
        qWarning() << "OutboundWorker: MX lookup failed for" << domain << ":" << dns.errorString();
        return {};
    }

    const auto records = dns.mailExchangeRecords();
    if (records.isEmpty()) {
        qWarning() << "OutboundWorker: No MX records for" << domain;
        return {};
    }

    // dns-sorted by priority; pick first
    QString best = records.first().exchange();
    if (best.endsWith('.')) best.chop(1);
    qInfo() << "OutboundWorker: MX for" << domain << "=" << best;
    return best.toLatin1();
}

// ---------------------------------------------------------------------------
// SMTP helpers
// ---------------------------------------------------------------------------
bool OutboundWorker::smtpReadResponse(QSslSocket* s, int& code, QString& text, int timeoutMs) {
    if (!s->waitForReadyRead(timeoutMs)) {
        text = QStringLiteral("read timeout / connection lost");
        return false;
    }
    QByteArray line;
    while (s->canReadLine()) {
        QByteArray chunk = s->readLine();
        if (chunk.size() >= 3 && chunk[3] == ' ') {
            // final line
            line = chunk;
            break;
        }
    }
    if (line.isEmpty()) {
        text = QStringLiteral("empty SMTP response");
        return false;
    }
    code = line.left(3).toInt();
    text = QString::fromLatin1(line.mid(4)).trimmed();
    return true;
}

bool OutboundWorker::smtpSendCommand(QSslSocket* s, const QByteArray& cmd,
                                      int expectedCode, int& code, QString& text) {
    s->write(cmd);
    if (!s->waitForBytesWritten(15000)) {
        text = QStringLiteral("write timeout");
        return false;
    }
    if (!smtpReadResponse(s, code, text)) return false;
    if (code != expectedCode) {
        text = QStringLiteral("SMTP %1: got %2 %3").arg(QString::fromLatin1(cmd.trimmed()))
                   .arg(code).arg(text);
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Direct SMTP delivery
// ---------------------------------------------------------------------------
OutboundWorker::SmtpDeliveryResult OutboundWorker::deliverDirect(OutboundEmail& email) {
    SmtpDeliveryResult result;

    // --- 1. MX lookup ---
    if (email.envelopeRcpt.isEmpty()) {
        result.errorText = QStringLiteral("No envelope recipients");
        return result;
    }

    QString domain = email.envelopeRcpt.first().section('@', -1).trimmed();
    if (domain.isEmpty()) {
        result.errorText = QStringLiteral("Could not extract domain from recipient");
        return result;
    }

    QByteArray mxHost = resolveMX(domain);
    if (mxHost.isEmpty()) {
        result.errorText = QStringLiteral("MX lookup failed for ") + domain;
        result.temporary = true;
        return result;
    }

    // --- 2. Sign DKIM if enabled ---
    QByteArray message = email.rawData;
    if (m_config.dkim().enabled) {
        message = signDkim(message);
    }

    // --- 3. Connect to port 25 ---
    QSslSocket socket;
    socket.connectToHost(QString::fromLatin1(mxHost), 25);
    if (!socket.waitForConnected(30000)) {
        result.errorText = QStringLiteral("Connection to %1:25 failed: %2")
                               .arg(QString::fromLatin1(mxHost), socket.errorString());
        result.temporary = true;
        return result;
    }

    int    code = 0;
    QString text;

    // --- 4. Read greeting ---
    if (!smtpReadResponse(&socket, code, text)) {
        result.errorText = QStringLiteral("No greeting from %1: %2").arg(QString::fromLatin1(mxHost), text);
        result.temporary = true;
        return result;
    }
    if (code != 220) {
        result.errorText = QStringLiteral("Unexpected greeting %1 %2 from %3").arg(code).arg(text, QString::fromLatin1(mxHost));
        result.temporary = true;
        return result;
    }

    // --- 5. EHLO ---
    QByteArray ehloCmd = "EHLO " + m_hostname.toLatin1() + "\r\n";
    if (!smtpSendCommand(&socket, ehloCmd, 250, code, text)) {
        // fallback to HELO
        ehloCmd = "HELO " + m_hostname.toLatin1() + "\r\n";
        if (!smtpSendCommand(&socket, ehloCmd, 250, code, text)) {
            result.errorText = QStringLiteral("EHLO/HELO failed: ") + text;
            result.temporary = true;
            return result;
        }
    }

    // --- 6. STARTTLS (opportunistic) ---
    bool upgraded = false;
    if (text.contains(QStringLiteral("STARTTLS"), Qt::CaseInsensitive) && !m_hostname.isEmpty()) {
        smtpSendCommand(&socket, QByteArray("STARTTLS\r\n"), 220, code, text);
        if (code == 220) {
            socket.startClientEncryption();
            if (socket.waitForEncrypted(15000)) {
                upgraded = true;
                // re-EHLO
                smtpSendCommand(&socket, QByteArray("EHLO " + m_hostname.toLatin1() + "\r\n"), 250, code, text);
            }
        }
    }
    Q_UNUSED(upgraded);

    // --- 7. MAIL FROM ---
    QString fromAddr = email.envelopeFrom.trimmed();
    if (fromAddr.startsWith('<') && fromAddr.contains('>')) {
        int closePos = fromAddr.indexOf('>');
        fromAddr = fromAddr.mid(1, closePos - 1);
    }
    QByteArray mailFromCmd = "MAIL FROM:<" + fromAddr.toLatin1() + ">\r\n";
    if (!smtpSendCommand(&socket, mailFromCmd, 250, code, text)) {
        result.errorText = QStringLiteral("MAIL FROM rejected: ") + text;
        return result;
    }

    // --- 8. RCPT TO (all recipients) ---
    for (const QString& rcpt : email.envelopeRcpt) {
        QByteArray rcptCmd = "RCPT TO:<" + rcpt.trimmed().toLatin1() + ">\r\n";
        if (!smtpSendCommand(&socket, rcptCmd, 250, code, text)) {
            result.errorText = QStringLiteral("RCPT TO %1 rejected: %2").arg(rcpt, text);
            return result;
        }
    }

    // --- 9. DATA ---
    if (!smtpSendCommand(&socket, QByteArray("DATA\r\n"), 354, code, text)) {
        result.errorText = QStringLiteral("DATA command rejected: ") + text;
        return result;
    }

    // send message content – ensure each line ends with \r\n
    QByteArray data = message;
    // normalize bare \n → \r\n (but do not double \r\n)
    data.replace("\r\n", "\n");
    data.replace("\n", "\r\n");
    data.append("\r\n.\r\n");
    socket.write(data);
    if (!socket.waitForBytesWritten(60000)) {
        result.errorText = QStringLiteral("Write timeout during DATA");
        result.temporary = true;
        return result;
    }

    if (!smtpReadResponse(&socket, code, text)) {
        result.errorText = QStringLiteral("No response after DATA");
        result.temporary = true;
        return result;
    }
    if (code != 250) {
        result.errorText = QStringLiteral("Message rejected: %1 %2").arg(code).arg(text);
        return result;
    }

    // --- 10. QUIT ---
    socket.write(QByteArray("QUIT\r\n"));
    socket.waitForBytesWritten(5000);
    socket.disconnectFromHost();

    qInfo() << "OutboundWorker: delivered OK to" << email.envelopeRcpt;
    return result; // success – errorText empty
}

// ---------------------------------------------------------------------------
// Relay delivery (smarthost) — Alibaba Cloud DirectMail, etc.
// ---------------------------------------------------------------------------
OutboundWorker::SmtpDeliveryResult OutboundWorker::deliverViaRelay(OutboundEmail& email) {
    SmtpDeliveryResult result;

    if (email.envelopeRcpt.isEmpty()) {
        result.errorText = QStringLiteral("No envelope recipients");
        return result;
    }

    // Sign DKIM if enabled
    QByteArray message = email.rawData;
    if (m_config.dkim().enabled) {
        message = signDkim(message);
    }

    QSslSocket socket;
    int port = m_relay.port > 0 ? m_relay.port : 25;

    qInfo() << "OutboundWorker: connecting to relay" << m_relay.host << ":" << port;

    if (m_relay.useTls) {
        socket.connectToHostEncrypted(m_relay.host, port);
        if (!socket.waitForEncrypted(30000)) {
            result.errorText = QStringLiteral("TLS connection to relay %1:%2 failed: %3")
                                   .arg(m_relay.host).arg(port).arg(socket.errorString());
            result.temporary = true;
            return result;
        }
    } else {
        socket.connectToHost(m_relay.host, port);
        if (!socket.waitForConnected(30000)) {
            result.errorText = QStringLiteral("Connection to relay %1:%2 failed: %3")
                                   .arg(m_relay.host).arg(port).arg(socket.errorString());
            result.temporary = true;
            return result;
        }
    }

    int code = 0;
    QString text;

    // Read greeting
    if (!smtpReadResponse(&socket, code, text)) {
        result.errorText = QStringLiteral("No greeting from relay: ") + text;
        result.temporary = true;
        return result;
    }
    if (code != 220) {
        result.errorText = QStringLiteral("Unexpected relay greeting %1 %2").arg(code).arg(text);
        result.temporary = true;
        return result;
    }

    // EHLO
    QByteArray ehloCmd = "EHLO " + m_hostname.toLatin1() + "\r\n";
    if (!smtpSendCommand(&socket, ehloCmd, 250, code, text)) {
        // fallback to HELO
        ehloCmd = "HELO " + m_hostname.toLatin1() + "\r\n";
        if (!smtpSendCommand(&socket, ehloCmd, 250, code, text)) {
            result.errorText = QStringLiteral("EHLO/HELO failed at relay: ") + text;
            result.temporary = true;
            return result;
        }
    }

    // STARTTLS if requested (and not already using implicit TLS)
    if (m_relay.useStartTls && !m_relay.useTls) {
        if (text.contains(QStringLiteral("STARTTLS"), Qt::CaseInsensitive)) {
            if (smtpSendCommand(&socket, QByteArray("STARTTLS\r\n"), 220, code, text)) {
                socket.startClientEncryption();
                if (socket.waitForEncrypted(15000)) {
                    // re-EHLO after TLS
                    smtpSendCommand(&socket, QByteArray("EHLO " + m_hostname.toLatin1() + "\r\n"), 250, code, text);
                } else {
                    result.errorText = QStringLiteral("STARTTLS handshake failed: ") + socket.errorString();
                    result.temporary = true;
                    return result;
                }
            }
        }
    }

    // AUTH if credentials provided (some relays like Alibaba require auth but don't advertise it)
    // Alibaba DirectMail only lets an account send as itself: log in as the sender when it (or
    // its domain) has an SMTP password configured, otherwise fall back to the shared relay account.
    // A per-address entry takes priority since DirectMail registers senders individually.
    QString authUser = m_relay.username;
    QString authPass = m_relay.password;
    const QString senderAddress = email.envelopeFrom.trimmed().toLower();
    const QString senderDomain = senderAddress.section('@', 1);
    const QHash<QString, QString> senderPasswords = liveSenderPasswords();
    if (senderPasswords.contains(senderAddress)) {
        authUser = senderAddress;
        authPass = senderPasswords.value(senderAddress);
    } else if (senderPasswords.contains(senderDomain)) {
        authUser = senderAddress;
        authPass = senderPasswords.value(senderDomain);
    }
    if (!authUser.isEmpty() && !authPass.isEmpty()) {
        qInfo() << "OutboundWorker: authenticating to relay as" << authUser;
        {
            QString authMethod = m_relay.authMethod;
            if (authMethod == "PLAIN") {
                if (!smtpAuthPlain(&socket, authUser, authPass, code, text)) {
                    result.errorText = QStringLiteral("AUTH PLAIN failed: ") + text;
                    result.temporary = false;
                    return result;
                }
            } else if (authMethod == "LOGIN") {
                if (!smtpAuthLogin(&socket, authUser, authPass, code, text)) {
                    result.errorText = QStringLiteral("AUTH LOGIN failed: ") + text;
                    result.temporary = false;
                    return result;
                }
            } else {
                result.errorText = QStringLiteral("Unsupported auth method: ") + authMethod;
                return result;
            }
        }
    }

    // MAIL FROM
    QString fromAddr = email.envelopeFrom.trimmed();
    if (fromAddr.startsWith('<') && fromAddr.contains('>')) {
        int closePos = fromAddr.indexOf('>');
        fromAddr = fromAddr.mid(1, closePos - 1);
    }
    QByteArray mailFromCmd = "MAIL FROM:<" + fromAddr.toLatin1() + ">\r\n";
    if (!smtpSendCommand(&socket, mailFromCmd, 250, code, text)) {
        result.errorText = QStringLiteral("MAIL FROM rejected by relay: ") + text;
        return result;
    }

    // RCPT TO
    for (const QString& rcpt : email.envelopeRcpt) {
        QByteArray rcptCmd = "RCPT TO:<" + rcpt.trimmed().toLatin1() + ">\r\n";
        if (!smtpSendCommand(&socket, rcptCmd, 250, code, text)) {
            result.errorText = QStringLiteral("RCPT TO %1 rejected by relay: %2").arg(rcpt, text);
            return result;
        }
    }

    // DATA
    if (!smtpSendCommand(&socket, QByteArray("DATA\r\n"), 354, code, text)) {
        result.errorText = QStringLiteral("DATA command rejected by relay: ") + text;
        return result;
    }

    // Send message body
    QByteArray data = message;
    data.replace("\r\n", "\n");
    data.replace("\n", "\r\n");
    data.append("\r\n.\r\n");
    socket.write(data);
    if (!socket.waitForBytesWritten(60000)) {
        result.errorText = QStringLiteral("Write timeout during DATA to relay");
        result.temporary = true;
        return result;
    }

    if (!smtpReadResponse(&socket, code, text)) {
        result.errorText = QStringLiteral("No response after DATA from relay");
        result.temporary = true;
        return result;
    }
    if (code != 250) {
        result.errorText = QStringLiteral("Relay rejected message: %1 %2").arg(code).arg(text);
        return result;
    }

    // QUIT
    socket.write(QByteArray("QUIT\r\n"));
    socket.waitForBytesWritten(5000);
    socket.disconnectFromHost();

    qInfo() << "OutboundWorker: delivered via relay to" << email.envelopeRcpt;
    return result;
}

// ---------------------------------------------------------------------------
// SMTP AUTH LOGIN
// ---------------------------------------------------------------------------
bool OutboundWorker::smtpAuthLogin(QSslSocket* s, const QString& user, const QString& pass,
                                   int& code, QString& text) {
    if (!smtpSendCommand(s, QByteArray("AUTH LOGIN\r\n"), 334, code, text)) {
        return false;
    }

    QByteArray usernameB64 = user.toLatin1().toBase64();
    if (!smtpSendCommand(s, usernameB64 + "\r\n", 334, code, text)) {
        return false;
    }

    QByteArray passwordB64 = pass.toLatin1().toBase64();
    if (!smtpSendCommand(s, passwordB64 + "\r\n", 235, code, text)) {
        return false;
    }

    return true;
}

// ---------------------------------------------------------------------------
// SMTP AUTH PLAIN
// ---------------------------------------------------------------------------
bool OutboundWorker::smtpAuthPlain(QSslSocket* s, const QString& user, const QString& pass,
                                   int& code, QString& text) {
    // base64(\0 username \0 password)
    QByteArray auth;
    auth.append('\0');
    auth.append(user.toLatin1());
    auth.append('\0');
    auth.append(pass.toLatin1());

    QByteArray cmd = "AUTH PLAIN " + auth.toBase64() + "\r\n";
    return smtpSendCommand(s, cmd, 235, code, text);
}

// ---------------------------------------------------------------------------
// Live sender_passwords (re-read from disk so a newly auto-provisioned
// DirectMail sender can send without waiting for a service restart)
// ---------------------------------------------------------------------------
QHash<QString, QString> OutboundWorker::liveSenderPasswords() const {
    if (m_configFilePath.isEmpty()) return m_relay.senderPasswords;

    QFile file(m_configFilePath);
    if (!file.open(QIODevice::ReadOnly)) return m_relay.senderPasswords;

    QJsonParseError err;
    QJsonDocument doc = QJsonDocument::fromJson(file.readAll(), &err);
    file.close();
    if (err.error != QJsonParseError::NoError) return m_relay.senderPasswords;

    QJsonObject senderPw = doc.object().value("outbound").toObject()
                               .value("relay").toObject()
                               .value("sender_passwords").toObject();
    QHash<QString, QString> result;
    for (auto it = senderPw.constBegin(); it != senderPw.constEnd(); ++it) {
        result.insert(it.key().toLower(), it.value().toString());
    }
    return result;
}

// ---------------------------------------------------------------------------
// DKIM signing (Python dkimpy for portability)
// ---------------------------------------------------------------------------
QByteArray OutboundWorker::signDkim(const QByteArray& rawEmail) {
    auto dk = m_config.dkim();
    if (!dk.enabled || dk.privateKeyPath.isEmpty()) return rawEmail;

    QProcess proc;
    proc.setProgram(QStringLiteral("python3"));
    proc.setArguments({
        QStringLiteral("./certs/dkim/dkim_sign.py"),
        QStringLiteral("-d"), dk.domain,
        QStringLiteral("-s"), dk.selector,
        QStringLiteral("-k"), dk.privateKeyPath
    });
    proc.start();
    if (!proc.waitForStarted(5000)) {
        qWarning() << "OutboundWorker: dkim_sign.py failed to start";
        return rawEmail;
    }
    proc.write(rawEmail);
    proc.closeWriteChannel();
    if (!proc.waitForFinished(15000)) {
        qWarning() << "OutboundWorker: dkim_sign.py timed out";
        return rawEmail;
    }
    QByteArray signedMsg = proc.readAllStandardOutput();
    QByteArray stderrOut = proc.readAllStandardError();
    if (!stderrOut.isEmpty()) {
        qWarning() << "OutboundWorker: dkim_sign stderr:" << stderrOut;
    }
    if (!signedMsg.isEmpty()) return signedMsg;
    return rawEmail;
}

// ---------------------------------------------------------------------------
// Disk queue management
// ---------------------------------------------------------------------------
void OutboundWorker::loadPendingFromDisk() {
    QDir newDir(m_spoolDir + "/new");
    QDir deferredDir(m_spoolDir + "/deferred");

    QStringList files;
    auto listDir = [&](const QDir& d) {
        QStringList entries = d.entryList({QStringLiteral("*.eml")}, QDir::Files);
        for (const QString& e : entries)
            files << d.filePath(e);
    };
    listDir(newDir);
    listDir(deferredDir);

    for (const QString& fp : files) {
        // skip already queued
        bool already = false;
        for (const auto& e : m_queue) {
            if (e.filePath == fp) { already = true; break; }
        }
        if (already) continue;

        QFile file(fp);
        if (!file.open(QIODevice::ReadOnly)) continue;

        // Envelope stored as JSON header at front of .eml
        QByteArray data = file.readAll();
        file.close();

        int sepIdx = data.indexOf("\r\n\r\n");
        if (sepIdx < 0) sepIdx = data.indexOf("\n\n");
        // If header starts with '{', it's JSON envelope metadata
        int jsonEnd = -1;
        bool hasJsonEnvelope = false;
        if (data.startsWith('{')) {
            jsonEnd = data.indexOf("\n");
            hasJsonEnvelope = true;
        }

        OutboundEmail email;
        email.filePath = fp;

        if (hasJsonEnvelope && jsonEnd > 0) {
            QByteArray jsonBytes = data.left(jsonEnd);
            QJsonDocument doc = QJsonDocument::fromJson(jsonBytes);
            if (doc.isObject()) {
                QJsonObject obj = doc.object();
                email.envelopeFrom   = obj.value("from").toString();
                QJsonArray arr = obj.value("rcpt").toArray();
                for (const auto& v : arr)
                    email.envelopeRcpt << v.toString();
            }
            email.rawData = data.mid(jsonEnd + 1);
        } else {
            email.rawData = data;
        }

        // extract envelope from message headers if not JSON
        if (email.envelopeFrom.isEmpty()) {
            int headerEnd = data.indexOf("\r\n\r\n");
            if (headerEnd < 0) headerEnd = data.indexOf("\n\n");
            QByteArray headers = data.left(headerEnd);
            for (const QByteArray& line : headers.split('\n')) {
                if (line.startsWith("X-Envelope-From:")) {
                    email.envelopeFrom = QString::fromLatin1(line.mid(17).trimmed());
                }
                if (line.startsWith("X-Envelope-To:")) {
                    email.envelopeRcpt << QString::fromLatin1(line.mid(15).trimmed());
                }
            }
        }

        // last resort: extract From + To from message
        if (email.envelopeFrom.isEmpty()) {
            int headerEnd = data.indexOf("\r\n\r\n");
            if (headerEnd < 0) headerEnd = data.indexOf("\n\n");
            QByteArray headers = data.left(headerEnd);
            for (const QByteArray& line : headers.split('\n')) {
                if (line.startsWith("From:") && email.envelopeFrom.isEmpty()) {
                    int a = line.indexOf('<');
                    int b = line.indexOf('>');
                    if (a >= 0 && b > a)
                        email.envelopeFrom = QString::fromLatin1(line.mid(a+1, b-a-1).trimmed());
                }
                if (line.startsWith("To:")) {
                    int a = line.indexOf('<');
                    int b = line.indexOf('>');
                    if (a >= 0 && b > a)
                        email.envelopeRcpt << QString::fromLatin1(line.mid(a+1, b-a-1).trimmed());
                }
            }
        }

        if (!email.rawData.isEmpty()) {
            m_queue.enqueue(email);
        }
    }
}

void OutboundWorker::markDelivered(const OutboundEmail& email) {
    QString deliveredDir = m_spoolDir + "/delivered";
    QDir().mkpath(deliveredDir);
    QString dest = deliveredDir + "/" + QFileInfo(email.filePath).fileName();
    QFile::rename(email.filePath, dest);
    qInfo() << "OutboundWorker: delivered ->" << dest;
}

void OutboundWorker::markDeferred(OutboundEmail& email, const QString& reason) {
    // exponential backoff: 60s * 2^attempt
    qint64 delay = 60 * (1 << email.retryCount);
    if (delay > 86400) delay = 86400; // cap at 24h
    email.nextAttemptAt = QDateTime::currentSecsSinceEpoch() + delay;

    QString deferredDir = m_spoolDir + "/deferred";
    QDir().mkpath(deferredDir);
    QString dest = deferredDir + "/" + QFileInfo(email.filePath).fileName();
    QFile::rename(email.filePath, dest);
    email.filePath = dest;

    qWarning() << "OutboundWorker: deferred" << email.filePath
               << "(" << reason << ") – next attempt in" << delay << "s";
}

void OutboundWorker::markFailed(const OutboundEmail& email) {
    QString failedDir = m_spoolDir + "/failed";
    QDir().mkpath(failedDir);
    QString dest = failedDir + "/" + QFileInfo(email.filePath).fileName();
    QFile::rename(email.filePath, dest);
    qCritical() << "OutboundWorker: permanently failed ->" << dest;
}
