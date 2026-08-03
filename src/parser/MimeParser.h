#ifndef MIMEPARSER_H
#define MIMEPARSER_H

#include <QByteArray>
#include <QString>
#include <QStringList>
#include <QList>

struct MimeHeader {
    QString name;
    QString value;
};

struct MimePart {
    QList<MimeHeader> headers;
    QByteArray body;
    QString contentType;
    QString charset;
    QString contentTransferEncoding;
    QList<MimePart> subParts;
};

class MimeParser {
public:
    MimeParser();

    /**
     * Parse a raw MIME message. Returns true on success.
     */
    bool parse(const QByteArray& rawData);

    /**
     * Returns a description of the last parse error, or empty string on success.
     */
    QString lastError() const { return m_lastError; }

    const MimePart& rootPart() const { return m_rootPart; }

    QString subject() const;
    QString from() const;
    QStringList to() const;
    QStringList cc() const;
    QString plainTextBody() const;
    QString htmlBody() const;
    QByteArray rawHeaders() const { return m_rawHeaders; }

    static QString decodeRFC2047(const QByteArray& input);
    static QByteArray decodeTransferEncoding(const QByteArray& data, const QString& encoding);
    static QString decodeQuotedPrintable(const QByteArray& input);

private:
    MimePart parsePart(const QByteArray& data);
    QList<MimeHeader> parseHeaders(const QByteArray& data, int& bodyStart);
    QString headerValue(const QString& name) const;

    MimePart   m_rootPart;
    QByteArray m_rawHeaders;
    QString    m_lastError;
};

#endif // MIMEPARSER_H
