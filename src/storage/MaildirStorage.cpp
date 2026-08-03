#include "MaildirStorage.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QDateTime>
#include <QDebug>

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

QString MaildirStorage::userMaildirPath(const QString& user) const {
    return m_basePath + "/" + user;
}

int MaildirStorage::nextUid(const QString& userPath) {
    QDir cur(userPath + "/cur");
    QDir nw(userPath + "/new");
    int maxUid = 0;
    auto scan = [&](const QDir& d) {
        QStringList files = d.entryList({"*.mailserver"}, QDir::Files);
        for (const QString& f : files) {
            bool ok;
            int uid = f.section('.', 0, 0).toInt(&ok);
            if (ok && uid > maxUid) maxUid = uid;
        }
    };
    if (cur.exists()) scan(cur);
    if (nw.exists()) scan(nw);
    return maxUid + 1;
}

QString MaildirStorage::uidToFile(const QString& userPath, int uid) const {
    // search in cur/ and new/
    QDir cur(userPath + "/cur");
    QDir nw(userPath + "/new");
    QString pat = QString::number(uid) + ".*.mailserver";
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
        QFileInfoList files = dir.entryInfoList({"*.mailserver"}, QDir::Files);
        for (const QFileInfo& fi : files) {
            MailEntry e;
            e.fileName = fi.fileName();
            e.uid      = fileToUid(fi.fileName());
            e.size     = fi.size();
            e.date     = fi.lastModified();
            e.seen     = !fi.fileName().endsWith(":2,") && fi.fileName().contains("S");
            e.seen     = isNew ? false : e.seen; // new/ = not seen
            e.flagged  = fi.fileName().contains("F");
            e.deleted_ = fi.fileName().contains("T");
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
    e.seen     = fi.fileName().contains("S");
    e.flagged  = fi.fileName().contains("F");
    e.deleted_ = fi.fileName().contains("T");
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
    if (!newName.contains('S')) newName += 'S';
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
    return total;
}

bool MaildirStorage::isOverQuota(const QString& user, qint64 additionalBytes) const {
    return (usageBytes(user) + additionalBytes) > m_quotaMaxBytes;
}

bool MaildirStorage::isOverSoftQuota(const QString& user) const {
    qint64 softLimit = m_quotaMaxBytes * m_quotaSoftPercent / 100;
    return usageBytes(user) > softLimit;
}
