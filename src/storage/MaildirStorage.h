#ifndef MAILDIRSTORAGE_H
#define MAILDIRSTORAGE_H

#include <QString>
#include <QDir>
#include <QByteArray>
#include <QList>
#include <QDateTime>
#include <QStringList>

struct MailEntry {
    QString fileName;
    int uid;
    qint64 size;
    QDateTime date;
    bool seen;
    bool flagged;
    bool deleted_;
    QByteArray rawMessage;
};

class MaildirStorage {
public:
    MaildirStorage();

    /**
     * Initialize with a configurable base path (replaces hardcoded ./maildir).
     */
    bool init(const QString& basePath, qint64 quotaMaxBytes, int quotaSoftPercent);

    /**
     * Store a new message for a user. Returns uid, or -1 on error.
     */
    int storeMessage(const QString& user, const QByteArray& rawMessage);

    /**
     * List messages for a user. Returns list of entries (metadata only, no body).
     */
    QList<MailEntry> listMessages(const QString& user);

    /**
     * Load full message by uid.
     */
    MailEntry loadMessage(const QString& user, int uid);

    /**
     * Mark a message as seen.
     */
    bool markSeen(const QString& user, int uid);

    /**
     * Mark a message as flagged.
     */
    bool markFlagged(const QString& user, int uid, bool flagged);

    /**
     * Replace a message's maildir flag letters (subset of "DFRST"); moves it to cur/.
     */
    bool setFlags(const QString& user, int uid, const QString& flags);

    /**
     * Delete (expunge) a message.
     */
    bool deleteMessage(const QString& user, int uid);

    /**
     * Create a sub-mailbox folder (e.g. Trash, Sent).
     */
    bool createMailbox(const QString& user, const QString& mailbox);

    /**
     * Folder names (Maildir++ ".Name" subdirectories) for a user, without the leading dot.
     */
    QStringList listMailboxes(const QString& user) const;

    /**
     * Delete an empty-or-not folder and its messages. INBOX cannot be deleted.
     */
    bool deleteMailbox(const QString& user, const QString& mailbox);

    /**
     * Rename a folder (and its sub-folders).
     */
    bool renameMailbox(const QString& user, const QString& from, const QString& to);

    bool mailboxExists(const QString& user, const QString& mailbox) const;

    /**
     * Per-mailbox UIDVALIDITY, created on first use and persisted in ".uidvalidity".
     */
    quint32 uidValidity(const QString& user);

    /**
     * User quota usage in bytes.
     */
    qint64 usageBytes(const QString& user) const;

    /**
     * Check if user is over quota.
     */
    bool isOverQuota(const QString& user, qint64 additionalBytes = 0) const;

    /**
     * Soft quota exceeded (warning threshold).
     */
    bool isOverSoftQuota(const QString& user) const;

private:
    QString userMaildirPath(const QString& user) const;
    int nextUid(const QString& userPath);
    QString uidToFile(const QString& userPath, int uid) const;
    int fileToUid(const QString& fileName) const;

    QString m_basePath;
    qint64  m_quotaMaxBytes;
    int     m_quotaSoftPercent;
    QDir    ensureDir(const QString& path);
};

#endif // MAILDIRSTORAGE_H
