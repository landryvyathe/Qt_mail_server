#include "ImapMime.h"

#include <QRegularExpression>
#include <QSet>
#include <algorithm>

namespace ImapMime {

namespace {

const int kMaxMimeDepth = 20;

struct ContentType {
    QByteArray type = "TEXT";
    QByteArray subtype = "PLAIN";
    QList<QPair<QByteArray, QByteArray>> params;

    QByteArray param(const QByteArray& name) const {
        for (const auto& p : params)
            if (p.first.compare(name, Qt::CaseInsensitive) == 0) return p.second;
        return {};
    }
};

struct Part {
    QByteArray header;
    QByteArray body;
    ContentType ct;
    QList<Part> children;           // multipart children
    QList<Part> embedded;           // message/rfc822: exactly one parsed message
    bool isMultipart() const { return ct.type == "MULTIPART"; }
    bool isMessage() const { return ct.type == "MESSAGE" && ct.subtype == "RFC822"; }
};

int headerEnd(const QByteArray& msg) {
    if (msg.startsWith("\r\n")) return 2;
    const int idx = msg.indexOf("\r\n\r\n");
    return idx < 0 ? msg.size() : idx + 4;
}

ContentType parseContentType(const QByteArray& value, bool digestParent) {
    ContentType ct;
    if (digestParent) { ct.type = "MESSAGE"; ct.subtype = "RFC822"; }
    if (value.trimmed().isEmpty()) return ct;

    const QList<QByteArray> pieces = value.split(';');
    const QByteArray mime = pieces.first().trimmed().toUpper();
    const int slash = mime.indexOf('/');
    if (slash > 0) {
        ct.type = mime.left(slash).trimmed();
        ct.subtype = mime.mid(slash + 1).trimmed();
    }
    for (int i = 1; i < pieces.size(); ++i) {
        const QByteArray kv = pieces[i].trimmed();
        const int eq = kv.indexOf('=');
        if (eq <= 0) continue;
        QByteArray v = kv.mid(eq + 1).trimmed();
        if (v.size() >= 2 && v.startsWith('"') && v.endsWith('"')) v = v.mid(1, v.size() - 2);
        ct.params.append({kv.left(eq).trimmed().toUpper(), v});
    }
    return ct;
}

Part parsePart(const QByteArray& data, bool digestParent, int depth);

QList<Part> splitMultipart(const QByteArray& body, const QByteArray& boundary,
                           bool isDigest, int depth) {
    QList<Part> parts;
    const QByteArray data = "\r\n" + body;
    const QByteArray delim = "\r\n--" + boundary;
    int pos = data.indexOf(delim);
    while (pos >= 0) {
        const int afterDelim = pos + delim.size();
        if (data.mid(afterDelim, 2) == "--") break;
        const int lineEnd = data.indexOf("\r\n", afterDelim);
        if (lineEnd < 0) break;
        const int start = lineEnd + 2;
        int next = data.indexOf(delim, start - 2);
        if (next < 0) next = data.size();
        parts.append(parsePart(data.mid(start, next - start), isDigest, depth + 1));
        pos = next < data.size() ? next : -1;
    }
    return parts;
}

Part parsePart(const QByteArray& data, bool digestParent, int depth) {
    Part p;
    const int end = headerEnd(data);
    p.header = data.left(end);
    p.body = data.mid(end);
    p.ct = parseContentType(headerValue(p.header, "Content-Type"), digestParent);
    if (depth >= kMaxMimeDepth) return p;

    if (p.isMultipart()) {
        const QByteArray boundary = p.ct.param("BOUNDARY");
        if (!boundary.isEmpty())
            p.children = splitMultipart(p.body, boundary, p.ct.subtype == "DIGEST", depth);
    } else if (p.isMessage()) {
        p.embedded.append(parsePart(p.body, false, depth + 1));
    }
    return p;
}

QByteArray quoted(const QByteArray& v) {
    QByteArray out = v;
    out.replace('\\', "\\\\").replace('"', "\\\"");
    return '"' + out + '"';
}

QByteArray astringOrNil(const QByteArray& v) {
    return v.isEmpty() ? QByteArray("NIL") : nstring(v);
}

QList<QByteArray> splitAddresses(const QByteArray& value) {
    QList<QByteArray> out;
    QByteArray cur;
    bool inQuote = false;
    int angle = 0;
    for (int i = 0; i < value.size(); ++i) {
        const char c = value[i];
        if (c == '\\' && inQuote && i + 1 < value.size()) { cur += c; cur += value[++i]; continue; }
        if (c == '"') inQuote = !inQuote;
        else if (!inQuote && c == '<') ++angle;
        else if (!inQuote && c == '>' && angle > 0) --angle;
        if (c == ',' && !inQuote && angle == 0) { out.append(cur.trimmed()); cur.clear(); continue; }
        cur += c;
    }
    if (!cur.trimmed().isEmpty()) out.append(cur.trimmed());
    return out;
}

QByteArray addressList(const QByteArray& value) {
    if (value.trimmed().isEmpty()) return "NIL";
    QByteArray out = "(";
    for (const QByteArray& addr : splitAddresses(value)) {
        QByteArray name;
        QByteArray email = addr;
        const int lt = addr.lastIndexOf('<');
        const int gt = addr.lastIndexOf('>');
        if (lt >= 0 && gt > lt) {
            name = addr.left(lt).trimmed();
            if (name.size() >= 2 && name.startsWith('"') && name.endsWith('"'))
                name = name.mid(1, name.size() - 2);
            email = addr.mid(lt + 1, gt - lt - 1).trimmed();
        }
        const int at = email.lastIndexOf('@');
        const QByteArray mailbox = at >= 0 ? email.left(at) : email;
        const QByteArray host = at >= 0 ? email.mid(at + 1) : QByteArray();
        if (mailbox.isEmpty()) continue;
        out += '(' + astringOrNil(name) + " NIL " + astringOrNil(mailbox) + ' '
             + astringOrNil(host) + ')';
    }
    if (out == "(") return "NIL";
    return out + ')';
}

QByteArray paramList(const ContentType& ct) {
    if (ct.params.isEmpty()) return "NIL";
    QByteArray out = "(";
    for (int i = 0; i < ct.params.size(); ++i) {
        if (i) out += ' ';
        out += quoted(ct.params[i].first) + ' ' + astringOrNil(ct.params[i].second);
    }
    return out + ')';
}

QByteArray structureOf(const Part& p) {
    if (p.isMultipart() && !p.children.isEmpty()) {
        QByteArray out = "(";
        for (const Part& c : p.children) out += structureOf(c);
        return out + ' ' + quoted(p.ct.subtype) + ')';
    }
    QByteArray encoding = headerValue(p.header, "Content-Transfer-Encoding").trimmed().toUpper();
    if (encoding.isEmpty()) encoding = "7BIT";
    ContentType ct = p.ct;
    if (p.isMultipart()) { ct.type = "TEXT"; ct.subtype = "PLAIN"; ct.params.clear(); }

    QByteArray out = '(' + quoted(ct.type) + ' ' + quoted(ct.subtype) + ' ' + paramList(ct) + ' '
        + astringOrNil(headerValue(p.header, "Content-ID").trimmed()) + ' '
        + astringOrNil(headerValue(p.header, "Content-Description").trimmed()) + ' '
        + quoted(encoding) + ' ' + QByteArray::number(p.body.size());
    const QByteArray lines = QByteArray::number(p.body.count("\r\n"));
    if (p.isMessage() && !p.embedded.isEmpty()) {
        const Part& m = p.embedded.first();
        out += ' ' + envelope(m.header) + ' ' + structureOf(m) + ' ' + lines;
    } else if (ct.type == "TEXT") {
        out += ' ' + lines;
    }
    return out + ')';
}

// Resolves a dotted part path like "2.1" to a part; null when missing.
const Part* resolvePath(const Part& root, const QList<int>& path) {
    const Part* cur = &root;
    for (int n : path) {
        if (cur->isMessage() && !cur->embedded.isEmpty()) cur = &cur->embedded.first();
        if (cur->isMultipart()) {
            if (n < 1 || n > cur->children.size()) return nullptr;
            cur = &cur->children[n - 1];
        } else if (n != 1) {
            return nullptr;
        }
    }
    return cur;
}

QStringList fieldNames(const QString& spec) {
    const int open = spec.indexOf('(');
    const int close = spec.lastIndexOf(')');
    if (open < 0 || close <= open) return {};
    return spec.mid(open + 1, close - open - 1).split(' ', Qt::SkipEmptyParts);
}

} // namespace

QByteArray toCrlf(const QByteArray& raw) {
    QByteArray out = raw;
    out.replace("\r\n", "\n");
    out.replace('\r', "\n");
    out.replace("\n", "\r\n");
    return out;
}

QByteArray headerBlock(const QByteArray& msg) { return msg.left(headerEnd(msg)); }
QByteArray bodyBlock(const QByteArray& msg) { return msg.mid(headerEnd(msg)); }

QByteArray headerValue(const QByteArray& header, const QByteArray& name) {
    const QList<QByteArray> lines = header.split('\n');
    for (int i = 0; i < lines.size(); ++i) {
        const QByteArray line = lines[i];
        const int colon = line.indexOf(':');
        if (colon <= 0 || line.startsWith(' ') || line.startsWith('\t')) continue;
        if (line.left(colon).trimmed().compare(name, Qt::CaseInsensitive) != 0) continue;
        QByteArray value = line.mid(colon + 1).trimmed();
        for (int j = i + 1; j < lines.size(); ++j) {
            const QByteArray next = lines[j];
            if (!next.startsWith(' ') && !next.startsWith('\t')) break;
            value += ' ' + next.trimmed();
        }
        return value;
    }
    return {};
}

QByteArray headerFields(const QByteArray& header, const QStringList& names, bool exclude) {
    QSet<QByteArray> wanted;
    for (const QString& n : names) wanted.insert(unquote(n).toUpper().toLatin1());

    QByteArray out;
    bool keep = false;
    const QList<QByteArray> lines = toCrlf(header).split('\n');
    for (QByteArray line : lines) {
        if (line.endsWith('\r')) line.chop(1);
        if (line.isEmpty()) continue;
        if (!line.startsWith(' ') && !line.startsWith('\t')) {
            const int colon = line.indexOf(':');
            const QByteArray field = colon > 0 ? line.left(colon).trimmed().toUpper() : QByteArray();
            keep = wanted.contains(field) != exclude;
        }
        if (keep) out += line + "\r\n";
    }
    return out + "\r\n";
}

QByteArray nstring(const QByteArray& value) {
    if (value.isNull()) return "NIL";
    for (char c : value) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (u < 0x20 || u > 0x7e)
            return '{' + QByteArray::number(value.size()) + "}\r\n" + value;
    }
    return quoted(value);
}

QByteArray envelope(const QByteArray& msg) {
    const QByteArray h = headerBlock(msg);
    const QByteArray from = headerValue(h, "From");
    QByteArray sender = headerValue(h, "Sender");
    QByteArray replyTo = headerValue(h, "Reply-To");
    if (sender.isEmpty()) sender = from;
    if (replyTo.isEmpty()) replyTo = from;

    return '(' + astringOrNil(headerValue(h, "Date")) + ' '
        + astringOrNil(headerValue(h, "Subject")) + ' '
        + addressList(from) + ' ' + addressList(sender) + ' ' + addressList(replyTo) + ' '
        + addressList(headerValue(h, "To")) + ' ' + addressList(headerValue(h, "Cc")) + ' '
        + addressList(headerValue(h, "Bcc")) + ' '
        + astringOrNil(headerValue(h, "In-Reply-To")) + ' '
        + astringOrNil(headerValue(h, "Message-ID")) + ')';
}

QByteArray bodyStructure(const QByteArray& msg) {
    return structureOf(parsePart(msg, false, 0));
}

bool section(const QByteArray& msg, const QString& specIn, QByteArray& out) {
    const QString spec = specIn.trimmed().toUpper();
    if (spec.isEmpty()) { out = msg; return true; }

    QList<int> path;
    QString rest = spec;
    static const QRegularExpression numRe("^(\\d+)(\\.|$)");
    for (auto m = numRe.match(rest); m.hasMatch(); m = numRe.match(rest)) {
        path.append(m.captured(1).toInt());
        rest = rest.mid(m.capturedLength());
    }

    const Part root = parsePart(msg, false, 0);
    const Part* part = path.isEmpty() ? &root : resolvePath(root, path);
    if (!part) return false;

    if (rest.isEmpty()) {
        out = path.isEmpty() ? msg : part->body;
        return true;
    }
    if (rest == "MIME") {
        if (path.isEmpty()) return false;
        out = part->header;
        return true;
    }

    // HEADER / TEXT / HEADER.FIELDS apply to a message: the root or an embedded message/rfc822.
    const Part* message = part;
    if (!path.isEmpty()) {
        if (!part->isMessage() || part->embedded.isEmpty()) return false;
        message = &part->embedded.first();
    }
    if (rest == "HEADER") { out = message->header; return true; }
    if (rest == "TEXT") { out = message->body; return true; }
    if (rest.startsWith("HEADER.FIELDS.NOT")) {
        out = headerFields(message->header, fieldNames(rest), true);
        return true;
    }
    if (rest.startsWith("HEADER.FIELDS")) {
        out = headerFields(message->header, fieldNames(rest), false);
        return true;
    }
    return false;
}

QList<int> parseSequenceSet(const QString& set, int maxValue) {
    QSet<int> values;
    if (maxValue < 1) return {};
    auto toNum = [maxValue](const QString& s, bool& ok) -> qint64 {
        if (s == "*") { ok = true; return maxValue; }
        return s.toLongLong(&ok);
    };
    for (const QString& item : set.split(',', Qt::SkipEmptyParts)) {
        const int colon = item.indexOf(':');
        bool okA = false, okB = false;
        qint64 a = toNum(colon < 0 ? item : item.left(colon), okA);
        qint64 b = colon < 0 ? a : toNum(item.mid(colon + 1), okB);
        if (colon < 0) okB = okA;
        if (!okA || !okB) continue;
        if (a > b) std::swap(a, b);
        if (a > maxValue) continue;
        b = std::min<qint64>(b, maxValue);
        for (qint64 v = std::max<qint64>(a, 1); v <= b; ++v) values.insert(static_cast<int>(v));
    }
    QList<int> out = values.values();
    std::sort(out.begin(), out.end());
    return out;
}

QStringList tokenize(const QString& args) {
    QStringList out;
    QString cur;
    int paren = 0, bracket = 0;
    bool inQuote = false;
    for (int i = 0; i < args.size(); ++i) {
        const QChar c = args[i];
        if (inQuote) {
            cur += c;
            if (c == '\\' && i + 1 < args.size()) { cur += args[++i]; continue; }
            if (c == '"') inQuote = false;
            continue;
        }
        if (c == '"') inQuote = true;
        else if (c == '(') ++paren;
        else if (c == ')' && paren > 0) --paren;
        else if (c == '[') ++bracket;
        else if (c == ']' && bracket > 0) --bracket;
        if (c == ' ' && paren == 0 && bracket == 0) {
            if (!cur.isEmpty()) out.append(cur);
            cur.clear();
            continue;
        }
        cur += c;
    }
    if (!cur.isEmpty()) out.append(cur);
    return out;
}

QString unquote(const QString& s) {
    if (s.size() < 2 || !s.startsWith('"') || !s.endsWith('"')) return s;
    QString out;
    const QString inner = s.mid(1, s.size() - 2);
    for (int i = 0; i < inner.size(); ++i) {
        if (inner[i] == '\\' && i + 1 < inner.size()) { out += inner[++i]; continue; }
        out += inner[i];
    }
    return out;
}

} // namespace ImapMime
