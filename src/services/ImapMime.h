#ifndef IMAPMIME_H
#define IMAPMIME_H

#include <QByteArray>
#include <QList>
#include <QString>
#include <QStringList>

namespace ImapMime {

// Converts bare LF line endings to CRLF, as IMAP literals require.
QByteArray toCrlf(const QByteArray& raw);

// Header block including the terminating blank line.
QByteArray headerBlock(const QByteArray& msg);
QByteArray bodyBlock(const QByteArray& msg);

// Unfolded value of the first header named `name` (case-insensitive), or empty.
QByteArray headerValue(const QByteArray& header, const QByteArray& name);

// Header lines whose name is (or with `exclude`, is not) in `names`, plus a blank line.
QByteArray headerFields(const QByteArray& header, const QStringList& names, bool exclude);

// IMAP string: quoted when safe, otherwise a {n} literal; NIL when null.
QByteArray nstring(const QByteArray& value);

QByteArray envelope(const QByteArray& msg);
QByteArray bodyStructure(const QByteArray& msg);

// Body section such as "", "HEADER", "TEXT", "1", "2.1.HEADER", "1.MIME",
// "HEADER.FIELDS (FROM TO)". Returns false if the section does not exist.
bool section(const QByteArray& msg, const QString& spec, QByteArray& out);

// Parses an IMAP sequence set ("1:*", "3,5:7") into ascending values.
// `maxValue` replaces '*'; values above it are dropped.
QList<int> parseSequenceSet(const QString& set, int maxValue);

// Splits an IMAP argument list into atoms, honouring quotes, () and [].
QStringList tokenize(const QString& args);

// Removes surrounding quotes and unescapes \" and \\.
QString unquote(const QString& s);

} // namespace ImapMime

#endif // IMAPMIME_H
