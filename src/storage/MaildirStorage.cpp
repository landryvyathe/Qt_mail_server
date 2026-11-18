#include "MaildirStorage.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QDateTime>
#include <QDebug>
#include <algorithm>

MaildirStorage::MaildirStorage() :
    m_basePath("./maildir"),
    m_quotaMaxBytes(1073741824),  // 1 GB default
    m_quotaSoftPercent(80) {
}

// ---------------------------------------------------------------------------
// init – accept configurable base path and quota
// ---------------------------------------------------------------------------
bool MaildirStorage::init(const QString& basePath, qint64 quotaMaxBytes, int quotaSoftPercent) {
    m_basePath         = basePath;
    m_quotaMaxBytes    = quotaMaxBytes;
    m_quotaSoftPercent = qBound(50, quotaSoftPercent, 95);
    qInfo() << "MaildirStorage: basePath =" << m_basePath
            << "quota =" << m_quotaMaxBytes << "bytes"
            << "soft =" << m_quotaSoftPercent << "%";
    return ensureDir(m_basePath).exists();
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
QDir MaildirStorage::ensureDir(const QString& path) {
    QDir d(path);
    if (!d.exists()) d.mkpath(".");
    return d;
}

static QString maildirFlags(const QString& fileName) {
    const int idx = fileName.indexOf(":2,");
    return idx >= 0 ? fileName.mid(idx + 3) : QString();
}

QString MaildirStorage::userMaildirPath(const QString& user) const {
    return m_basePath + "/" + user;
}

int MaildirStorage::nextUid(const QString& userPath) {
    QDir cur(userPath + "/cur");
    QDir nw(userPath + "/new");
    int maxUid = 0;
    auto scan = [&](const QDir& d) {
        QStringList files = d.entryList({"*.mailserver*"}, QDir::Files);
        for (const QString& f : files) {
            bool ok;
            int uid = f.section('.', 0, 0).toInt(&ok);
            if (ok && uid > maxUid) maxUid = uid;
        }
    };
    if (cur.exists()) scan(cur);
    if (nw.exists()) scan(nw);

    // UIDs must never be reused after an expunge (RFC 3501), so persist the high-water mark.
    QFile marker(userPath + "/.uidnext");
    int next = maxUid + 1;
    if (marker.open(QIODevice::ReadOnly)) {
        next = std::max(next, marker.readAll().trimmed().toInt());
        marker.close();
    }
    if (marker.open(QIODevice::WriteOnly | QIODevice::Truncate))
        marker.write(QByteArray::number(next + 1));
    return next;
}

QString MaildirStorage::uidToFile(const QString& userPath, int uid) const {
    // search in cur/ and new/
    QDir cur(userPath + "/cur");
    QDir nw(userPath + "/new");
    QString pat = QString::number(uid) + ".*.mailserver*";
    QStringList found = cur.entryList({pat}, QDir::Files);
    if (!found.isEmpty()) return cur.filePath(found.first());
    found = nw.entryList({pat}, QDir::Files);
    if (!found.isEmpty()) return nw.filePath(found.first());
    return {};
}

int MaildirStorage::fileToUid(const QString& fileName) const {
    QString base = QFileInfo(fileName).fileName();
    bool ok;
    int uid = base.section('.', 0, 0).toInt(&ok);
    return ok ? uid : -1;
}

// ---------------------------------------------------------------------------
// Store
// ---------------------------------------------------------------------------
int MaildirStorage::storeMessage(const QString& user, const QByteArray& rawMessage) {
    QString userPath = userMaildirPath(user);

    // Create maildir structure
    ensureDir(userPath + "/tmp");
    ensureDir(userPath + "/new");
    ensureDir(userPath + "/cur");

    int uid = nextUid(userPath);
    QString fileName = QString::number(uid) + ".2.mailserver";
    QString tmpPath  = userPath + "/tmp/" + fileName;
    QString newPath  = userPath + "/new/" + fileName;

    QFile file(tmpPath);
    if (!file.open(QIODevice::WriteOnly)) {
        qCritical() << "MaildirStorage: cannot write tmp" << tmpPath;
        return -1;
    }
    file.write(rawMessage);
    file.close();

    // atomic rename tmp → new
    if (!QFile::rename(tmpPath, newPath)) {
        qCritical() << "MaildirStorage: rename failed" << tmpPath << "→" << newPath;
        QFile::remove(tmpPath);
        return -1;
    }

    qInfo() << "MaildirStorage: stored uid" << uid << "for" << user;
    return uid;
}

// ---------------------------------------------------------------------------
// List (metadata only)
// ---------------------------------------------------------------------------
QList<MailEntry> MaildirStorage::listMessages(const QString& user) {
    QList<MailEntry> list;
    QString userPath = userMaildirPath(user);

    auto scanDir = [&](const QDir& dir, bool isNew) {
        if (!dir.exists()) return;
        QFileInfoList files = dir.entryInfoList({"*.mailserver*"}, QDir::Files);
        for (const QFileInfo& fi : files) {
            MailEntry e;
            e.fileName = fi.fileName();
            e.uid      = fileToUid(fi.fileName());
            e.size     = fi.size();
            e.date     = fi.lastModified();
            const QString flags = maildirFlags(fi.fileName());
            e.seen     = !isNew && flags.contains('S');
            e.flagged  = flags.contains('F');
            e.deleted_ = flags.contains('T');
            list.append(e);
        }
    };

    scanDir(QDir(userPath + "/new"), true);
    scanDir(QDir(userPath + "/cur"), false);
    return list;
}

// ---------------------------------------------------------------------------
// Load
// ---------------------------------------------------------------------------
MailEntry MaildirStorage::loadMessage(const QString& user, int uid) {
    MailEntry e;
    QString userPath = userMaildirPath(user);
    QString filePath = uidToFile(userPath, uid);
    if (filePath.isEmpty()) return e;

    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly)) return e;

    QFileInfo fi(filePath);
    e.fileName = fi.fileName();
    e.uid      = uid;
    e.size     = fi.size();
    e.date     = fi.lastModified();
    const QString flags = maildirFlags(fi.fileName());
    e.seen     = flags.contains('S');
    e.flagged  = flags.contains('F');
    e.deleted_ = flags.contains('T');
    e.rawMessage = file.readAll();
    file.close();
    return e;
}

// ---------------------------------------------------------------------------
// Mark seen
// ---------------------------------------------------------------------------
bool MaildirStorage::markSeen(const QString& user, int uid) {
    QString userPath = userMaildirPath(user);
    QString oldPath  = uidToFile(userPath, uid);
    if (oldPath.isEmpty()) return false;

    bool isNew = oldPath.contains("/new/");
    QString dir = isNew ? (userPath + "/cur/") : oldPath.section('/', 0, -2) + "/";
    QFileInfo fi(oldPath);
    QString base  = fi.fileName();
    // set S flag
    QString newName = base;
    if (!newName.contains(":2,")) newName += ":2,";
    if (!maildirFlags(newName).contains('S')) newName += 'S';
    QString newPath = dir + newName;

    if (oldPath == newPath) return true;
    return QFile::rename(oldPath, newPath);
}

// ---------------------------------------------------------------------------
// Flag
// ---------------------------------------------------------------------------
bool MaildirStorage::markFlagged(const QString& user, int uid, bool flagged) {
    QString userPath = userMaildirPath(user);
    QString oldPath  = uidToFile(userPath, uid);
    if (oldPath.isEmpty()) return false;

    QFileInfo fi(oldPath);
    QString base = fi.fileName();
    QString flags;
    int idx = base.indexOf(":2,");
    if (idx >= 0) flags = base.mid(idx + 3);

    if (flagged) {
        if (!flags.contains('F')) flags += 'F';
    } else {
        flags.remove('F');
    }

    QString newName = base;
    if (idx >= 0) newName = base.left(idx + 3) + flags;
    else          newName = base + ":2," + flags;

    QString newPath = fi.dir().filePath(newName);
    if (oldPath == newPath) return true;
    return QFile::rename(oldPath, newPath);
}

bool MaildirStorage::setFlags(const QString& user, int uid, const QString& flags) {
    const QString userPath = userMaildirPath(user);
    const QString oldPath  = uidToFile(userPath, uid);
    if (oldPath.isEmpty()) return false;

    QString sorted;
    for (QChar c : QStringLiteral("DFRST"))
        if (flags.contains(c)) sorted += c;

    QString base = QFileInfo(oldPath).fileName();
    const int idx = base.indexOf(":2,");
    if (idx >= 0) base = base.left(idx);
    const QString newPath = userPath + "/cur/" + base + ":2," + sorted;
    if (oldPath == newPath) return true;
    ensureDir(userPath + "/cur");
    return QFile::rename(oldPath, newPath);
}

// ---------------------------------------------------------------------------
// Delete
// ---------------------------------------------------------------------------
bool MaildirStorage::deleteMessage(const QString& user, int uid) {
    QString userPath = userMaildirPath(user);
    QString filePath = uidToFile(userPath, uid);
    if (filePath.isEmpty()) return false;
    return QFile::remove(filePath);
}

bool MaildirStorage::createMailbox(const QString& user, const QString& mailbox) {
    if (mailbox.isEmpty()) return false;
    QString sanitized = mailbox;
    sanitized.replace('/', '_').replace('\\', '_');
    QString folderPath = userMaildirPath(user) + "/." + sanitized;
    return ensureDir(folderPath + "/new").exists() &&
           ensureDir(folderPath + "/cur").exists() &&
           ensureDir(folderPath + "/tmp").exists();
}

QStringList MaildirStorage::listMailboxes(const QString& user) const {
    QStringList out;
    const QDir dir(userMaildirPath(user));
    for (const QString& d : dir.entryList(QDir::Dirs | QDir::Hidden | QDir::NoDotAndDotDot)) {
        if (d.startsWith('.') && d.size() > 1 && QDir(dir.filePath(d) + "/cur").exists())
            out << d.mid(1);
    }
    out.sort(Qt::CaseInsensitive);
    return out;
}

quint32 MaildirStorage::uidValidity(const QString& user) {
    const QString userPath = userMaildirPath(user);
    ensureDir(userPath);
    QFile file(userPath + "/.uidvalidity");
    if (file.open(QIODevice::ReadOnly)) {
        const quint32 stored = file.readAll().trimmed().toUInt();
        if (stored > 0) return stored;
        file.close();
    }
    const quint32 value = static_cast<quint32>(QDateTime::currentSecsSinceEpoch());
    if (file.open(QIODevice::WriteOnly | QIODevice::Truncate)) file.write(QByteArray::number(value));
    return value;
}

bool MaildirStorage::mailboxExists(const QString& user, const QString& mailbox) const {
    return QDir(userMaildirPath(user) + "/." + mailbox + "/cur").exists();
}

bool MaildirStorage::deleteMailbox(const QString& user, const QString& mailbox) {
    if (mailbox.isEmpty() || !mailboxExists(user, mailbox)) return false;
    return QDir(userMaildirPath(user) + "/." + mailbox).removeRecursively();
}

bool MaildirStorage::renameMailbox(const QString& user, const QString& from, const QString& to) {
    if (from.isEmpty() || to.isEmpty() || !mailboxExists(user, from) || mailboxExists(user, to))
        return false;
    const QString base = userMaildirPath(user);
    const QStringList all = listMailboxes(user);
    for (const QString& name : all) {
        if (name != from && !name.startsWith(from + '.')) continue;
        const QString target = to + name.mid(from.size());
        if (!QDir().rename(base + "/." + name, base + "/." + target)) return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Quota
// ---------------------------------------------------------------------------
qint64 MaildirStorage::usageBytes(const QString& user) const {
    QString userPath = userMaildirPath(user);
    qint64 total = 0;
    auto scan = [&](const QDir& d) {
        if (!d.exists()) return;
        QFileInfoList files = d.entryInfoList({"*"}, QDir::Files);
        for (const QFileInfo& fi : files)
            total += fi.size();
    };
    scan(QDir(userPath + "/new"));
    scan(QDir(userPath + "/cur"));
    for (const QString& folder : listMailboxes(user)) {
        scan(QDir(userPath + "/." + folder + "/new"));
        scan(QDir(userPath + "/." + folder + "/cur"));
    }
    return total;
}

bool MaildirStorage::isOverQuota(const QString& user, qint64 additionalBytes) const {
    return (usageBytes(user) + additionalBytes) > m_quotaMaxBytes;
}

bool MaildirStorage::isOverSoftQuota(const QString& user) const {
    qint64 softLimit = m_quotaMaxBytes * m_quotaSoftPercent / 100;
    return usageBytes(user) > softLimit;
}
