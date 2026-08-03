#include "MimeParser.h"
#include <QDebug>
#include <QRegularExpression>

MimeParser::MimeParser() {}

// ---------------------------------------------------------------------------
// parse
// ---------------------------------------------------------------------------
bool MimeParser::parse(const QByteArray& rawData) {
    m_lastError.clear();

    if (rawData.isEmpty()) {
        m_lastError = "Empty MIME message";
        return false;
    }

    int bodyStart = 0;
    QList<MimeHeader> headers = parseHeaders(rawData, bodyStart);
    if (bodyStart < 0) {
        m_lastError = "Failed to parse MIME headers";
        return false;
    }

    m_rawHeaders = rawData.left(bodyStart);
    m_rootPart = parsePart(rawData);

    // Post-parse validation
    if (from().isEmpty() && to().isEmpty() && subject().isEmpty()) {
        m_lastError = "MIME message missing required headers (From, To, or Subject)";
        return false;
    }

    return true;
}

// ---------------------------------------------------------------------------
// parseHeaders – parse header block, return body start offset
// ---------------------------------------------------------------------------
QList<MimeHeader> MimeParser::parseHeaders(const QByteArray& data, int& bodyStart) {
    QList<MimeHeader> headers;
    bodyStart = -1;

    int pos = 0;
    int len = data.size();

    // Find header/body separator (first \r\n\r\n or \n\n)
    QByteArray sep("\r\n\r\n");
    int sepIdx = data.indexOf(sep);
    if (sepIdx < 0) {
        sep = "\n\n";
        sepIdx = data.indexOf(sep);
    }

    int headerEnd = (sepIdx >= 0) ? sepIdx : len;
    if (sepIdx >= 0)
        bodyStart = sepIdx + sep.size();
    else
        bodyStart = len;

    QByteArray headerBlock = data.left(headerEnd);

    // Parse unfolded headers
    QString currentLine;
    QString currentName;

    for (const QByteArray& rawLine : headerBlock.split('\n')) {
        QString line = QString::fromUtf8(rawLine).trimmed();
        if (line.isEmpty()) continue;

        if (line[0].isSpace() || line[0] == '\t') {
            // continuation of previous header
            if (!currentName.isEmpty())
                currentLine += " " + line.trimmed();
        } else {
            // new header – commit previous
            if (!currentName.isEmpty()) {
                headers.append({currentName, currentLine.trimmed()});
            }
            int colon = line.indexOf(':');
            if (colon >= 0) {
                currentName = line.left(colon).trimmed();
                currentLine = line.mid(colon + 1).trimmed();
            } else {
                currentName.clear();
                currentLine.clear();
            }
        }
    }

    // commit last header
    if (!currentName.isEmpty()) {
        headers.append({currentName, currentLine.trimmed()});
    }

    return headers;
}

// ---------------------------------------------------------------------------
// parsePart – recursive MIME part parsing
// ---------------------------------------------------------------------------
MimePart MimeParser::parsePart(const QByteArray& data) {
    MimePart part;

    int bodyStart = 0;
    part.headers = parseHeaders(data, bodyStart);

    // content-type
    for (const auto& h : part.headers) {
        if (h.name.compare("Content-Type", Qt::CaseInsensitive) == 0) {
            part.contentType = h.value.toLower();
            // extract boundary if multipart
            if (part.contentType.startsWith("multipart/")) {
                QRegularExpression re("boundary=\"?([^\";]+)\"?",
                                      QRegularExpression::CaseInsensitiveOption);
                QRegularExpressionMatch m = re.match(h.value);
                if (m.hasMatch()) {
                    QString boundary = m.captured(1).trimmed();
                    // split body by boundary
                    QByteArray body = data.mid(bodyStart);
                    QByteArray boundaryBytes = ("--" + boundary).toUtf8();
                    // Qt6: split(char) only, manual split for QByteArray separator
                    QList<QByteArray> parts;
                    int start = 0, idx = 0;
                    while ((idx = body.indexOf(boundaryBytes, start)) != -1) {
                        parts.append(body.mid(start, idx - start));
                        start = idx + boundaryBytes.size();
                    }
                    parts.append(body.mid(start));
                    for (int i = 1; i < parts.size(); ++i) {
                        QByteArray sub = parts[i];
                        // skip trailing --
                        if (sub.startsWith("--") || sub.trimmed().isEmpty()) continue;
                        // skip the \r\n after boundary
                        if (sub.startsWith("\r\n")) sub = sub.mid(2);
                        else if (sub.startsWith("\n")) sub = sub.mid(1);

                        // include boundary prefix for header parsing
                        MimePart subPart = parsePart(sub);
                        // copy key headers from sub-part
                        part.subParts.append(subPart);
                    }
                }
            }
            break;
        }
        if (h.name.compare("Content-Transfer-Encoding", Qt::CaseInsensitive) == 0) {
            part.contentTransferEncoding = h.value.toLower();
        }
        if (h.name.compare("Content-Type", Qt::CaseInsensitive) == 0 && part.charset.isEmpty()) {
            QRegularExpression ce("charset=\"?([^\";]+)\"?",
                                  QRegularExpression::CaseInsensitiveOption);
            QRegularExpressionMatch m = ce.match(h.value);
            if (m.hasMatch()) part.charset = m.captured(1);
        }
    }

    // If no sub-parts, body is the part body
    if (part.subParts.isEmpty() && bodyStart < data.size()) {
        QByteArray rawBody = data.mid(bodyStart);
        // remove trailing CRLF
        while (rawBody.endsWith("\r\n")) rawBody.chop(2);
        while (rawBody.endsWith("\n")) rawBody.chop(1);
        part.body = decodeTransferEncoding(rawBody, part.contentTransferEncoding);
    }

    return part;
}

// ---------------------------------------------------------------------------
// Header getters
// ---------------------------------------------------------------------------
QString MimeParser::headerValue(const QString& name) const {
    for (const auto& h : m_rootPart.headers) {
        if (h.name.compare(name, Qt::CaseInsensitive) == 0)
            return h.value;
    }
    return {};
}

QString MimeParser::subject() const {
    return decodeRFC2047(headerValue("Subject").toUtf8());
}

QString MimeParser::from() const {
    return headerValue("From");
}

QStringList MimeParser::to() const {
    QString val = headerValue("To");
    QStringList result;
    for (const QString& addr : val.split(',')) {
        QString trimmed = addr.trimmed();
        if (!trimmed.isEmpty()) result << trimmed;
    }
    return result;
}

QStringList MimeParser::cc() const {
    QString val = headerValue("Cc");
    QStringList result;
    for (const QString& addr : val.split(',')) {
        QString trimmed = addr.trimmed();
        if (!trimmed.isEmpty()) result << trimmed;
    }
    return result;
}

QString MimeParser::plainTextBody() const {
    for (const auto& part : m_rootPart.subParts) {
        if (part.contentType.contains("text/plain")) {
            return QString::fromUtf8(part.body);
        }
    }
    if (m_rootPart.contentType.contains("text/plain")) {
        return QString::fromUtf8(m_rootPart.body);
    }
    return {};
}

QString MimeParser::htmlBody() const {
    for (const auto& part : m_rootPart.subParts) {
        if (part.contentType.contains("text/html")) {
            return QString::fromUtf8(part.body);
        }
    }
    if (m_rootPart.contentType.contains("text/html")) {
        return QString::fromUtf8(m_rootPart.body);
    }
    return {};
}

// ---------------------------------------------------------------------------
// RFC 2047 header decoding
// ---------------------------------------------------------------------------
QString MimeParser::decodeRFC2047(const QByteArray& input) {
    QString result;
    int i = 0;
    while (i < input.size()) {
        int eq = input.indexOf("=?", i);
        if (eq < 0) {
            result += QString::fromUtf8(input.mid(i));
            break;
        }
        result += QString::fromUtf8(input.mid(i, eq - i));
        int end = input.indexOf("?=", eq);
        if (end < 0) {
            result += QString::fromUtf8(input.mid(eq));
            break;
        }
        // =?charset?encoding?text?=
        QByteArray part = input.mid(eq + 2, end - eq - 2);
        int sep1 = part.indexOf('?');
        int sep2 = part.indexOf('?', sep1 + 1);
        if (sep1 < 0 || sep2 < 0) {
            result += QString::fromUtf8(input.mid(eq, end - eq + 2));
        } else {
            QByteArray charset = part.left(sep1);
            QByteArray encoding = part.mid(sep1 + 1, sep2 - sep1 - 1);
            QByteArray encodedText = part.mid(sep2 + 1);
            if (encoding == "B" || encoding == "b") {
                result += QString::fromUtf8(QByteArray::fromBase64(encodedText));
            } else { // Q
                QByteArray decoded;
                for (int j = 0; j < encodedText.size(); ++j) {
                    if (encodedText[j] == '=') {
                        if (j + 2 < encodedText.size()) {
                            decoded.append(QByteArray::fromHex(encodedText.mid(j + 1, 2)));
                            j += 2;
                        }
                    } else if (encodedText[j] == '_') {
                        decoded.append(' ');
                    } else {
                        decoded.append(encodedText[j]);
                    }
                }
                result += QString::fromUtf8(decoded);
            }
        }
        i = end + 2;
    }
    return result;
}

// ---------------------------------------------------------------------------
// Transfer-encoding decoding
// ---------------------------------------------------------------------------
QByteArray MimeParser::decodeTransferEncoding(const QByteArray& data, const QString& encoding) {
    if (encoding == "base64") {
        return QByteArray::fromBase64(data);
    }
    if (encoding == "quoted-printable") {
        return decodeQuotedPrintable(data).toUtf8();
    }
    return data; // 7bit, 8bit, binary – no decoding needed
}

QString MimeParser::decodeQuotedPrintable(const QByteArray& input) {
    QByteArray result;
    for (int i = 0; i < input.size(); ++i) {
        if (input[i] == '=') {
            if (i + 2 < input.size()) {
                QByteArray hex = input.mid(i + 1, 2);
                // Check for soft line break (=CRLF)
                if (input[i + 1] == '\r' && i + 2 < input.size() && input[i + 2] == '\n') {
                    i += 2;
                    continue;
                }
                if (input[i + 1] == '\n') {
                    i += 1;
                    continue;
                }
                result.append(QByteArray::fromHex(hex));
                i += 2;
            }
        } else {
            result.append(input[i]);
        }
    }
    return QString::fromUtf8(result);
}
