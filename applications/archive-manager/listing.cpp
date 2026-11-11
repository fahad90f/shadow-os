// SPDX-License-Identifier: GPL-3.0-or-later
#include "listing.h"

#include <QCoreApplication>
#include <QTimeZone>

namespace Listing {

namespace {

QString tr(const char *s) { return QCoreApplication::translate("Listing", s); }

// bsdtar prints non-printable bytes as C escapes or \ooo; undo that.
QByteArray unescape(const QByteArray &in)
{
    QByteArray out;
    out.reserve(in.size());
    for (qsizetype i = 0; i < in.size(); ++i) {
        const char c = in.at(i);
        if (c != '\\' || i + 1 >= in.size()) {
            out += c;
            continue;
        }
        const char n = in.at(i + 1);
        if (n >= '0' && n <= '7') {
            int value = 0, digits = 0;
            while (digits < 3 && i + 1 < in.size() && in.at(i + 1) >= '0' && in.at(i + 1) <= '7') {
                value = value * 8 + (in.at(i + 1) - '0');
                ++i;
                ++digits;
            }
            out += char(value);
            continue;
        }
        ++i;
        switch (n) {
        case 'n': out += '\n'; break;
        case 't': out += '\t'; break;
        case 'r': out += '\r'; break;
        case 'a': out += '\a'; break;
        case 'b': out += '\b'; break;
        case 'f': out += '\f'; break;
        case 'v': out += '\v'; break;
        case '\\': out += '\\'; break;
        default: out += '\\'; out += n; break;
        }
    }
    return out;
}

int monthNumber(const QByteArray &name)
{
    static const char *months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    for (int i = 0; i < 12; ++i) {
        if (name == months[i])
            return i + 1;
    }
    return 0;
}

// "Nov 11 13:23" (recent) or "Jan  1  1970" (older) — ls-style, local time.
QDateTime parseLsDate(const QByteArray &month, const QByteArray &day, const QByteArray &yearOrTime)
{
    const int m = monthNumber(month);
    const int d = day.toInt();
    if (m == 0 || d == 0)
        return {};
    if (yearOrTime.contains(':')) {
        const QList<QByteArray> hm = yearOrTime.split(':');
        const QDateTime now = QDateTime::currentDateTime();
        QDateTime guess(QDate(now.date().year(), m, d), QTime(hm.value(0).toInt(), hm.value(1).toInt()));
        if (guess > now.addDays(1))
            guess = guess.addYears(-1);
        return guess;
    }
    return QDateTime(QDate(yearOrTime.toInt(), m, d), QTime(0, 0));
}

} // namespace

QString kindName(Kind kind)
{
    switch (kind) {
    case Kind::File: return QStringLiteral("file");
    case Kind::Directory: return QStringLiteral("directory");
    case Kind::SymLink: return QStringLiteral("symlink");
    case Kind::HardLink: return QStringLiteral("hardlink");
    case Kind::Device: return QStringLiteral("device");
    case Kind::Fifo: return QStringLiteral("fifo");
    case Kind::Other: break;
    }
    return QStringLiteral("other");
}

void classify(Entry &e)
{
    e.parts.clear();
    e.hazards.clear();
    bool dotDot = false;
    const QStringList components = e.rawPath.split(QLatin1Char('/'));
    for (const QString &component : components) {
        if (component.isEmpty() || component == QLatin1String("."))
            continue;
        if (component == QLatin1String(".."))
            dotDot = true;
        e.parts << component;
    }
    if (e.rawPath.startsWith(QLatin1Char('/')))
        e.hazards << tr("absolute path — extracted relative to the destination instead");
    if (dotDot)
        e.hazards << tr("path contains “..” — not extracted outside the destination");
    if (e.kind == Kind::SymLink) {
        bool escapes = e.linkTarget.startsWith(QLatin1Char('/'));
        if (!escapes) {
            int depth = int(e.parts.size()) - 1;
            const QStringList target = e.linkTarget.split(QLatin1Char('/'));
            for (const QString &component : target) {
                if (component.isEmpty() || component == QLatin1String("."))
                    continue;
                if (component == QLatin1String("..")) {
                    if (--depth < 0) { escapes = true; break; }
                } else {
                    ++depth;
                }
            }
        }
        if (escapes)
            e.hazards << tr("symbolic link pointing outside the archive — not created");
    } else if (e.kind == Kind::HardLink && (e.linkTarget.startsWith(QLatin1Char('/')) || e.linkTarget.contains(QLatin1String("..")))) {
        e.hazards << tr("hard link to a file outside the archive — not created");
    } else if (e.kind == Kind::Device) {
        e.hazards << tr("device node — not created");
    } else if (e.kind == Kind::Fifo || e.kind == Kind::Other) {
        e.hazards << tr("special file — not created");
    }
    if (e.mode.size() >= 10 && (e.mode.at(3).toLower() == u's' || e.mode.at(6).toLower() == u's'))
        e.hazards << tr("set-user-ID/set-group-ID bit — removed on extraction");
}

Result parseSevenZip(const QByteArray &output)
{
    Result result;
    const QList<QByteArray> lines = output.split('\n');
    bool inEntries = false;
    bool haveEntry = false;
    Entry current;
    QString attributes;
    bool folder = false;
    QByteArray lastKey;
    QString *lastValue = nullptr;

    auto flush = [&] {
        if (!haveEntry)
            return;
        const QString unixMode = attributes.section(QLatin1Char(' '), -1);
        if (unixMode.size() == 10)
            current.mode = unixMode;
        const QChar type = current.mode.isEmpty() ? QChar() : current.mode.at(0);
        if (folder || attributes.startsWith(QLatin1Char('D')) || type == u'd')
            current.kind = Kind::Directory;
        else if (type == u'l' || !current.linkTarget.isEmpty())
            current.kind = Kind::SymLink;
        else if (type == u'c' || type == u'b')
            current.kind = Kind::Device;
        else if (type == u'p' || type == u's')
            current.kind = Kind::Fifo;
        classify(current);
        result.entries.append(current);
        current = Entry();
        attributes.clear();
        folder = false;
        haveEntry = false;
        lastValue = nullptr;
    };

    for (QByteArray line : lines) {
        if (line.endsWith('\r'))
            line.chop(1);
        if (!inEntries) {
            if (line.startsWith("----------"))
                inEntries = true;
            else if (line.startsWith("Type = "))
                result.typeName = QString::fromUtf8(line.mid(7));
            continue;
        }
        if (line.isEmpty()) {
            flush();
            continue;
        }
        const qsizetype separator = line.indexOf(" = ");
        const QByteArray key = separator > 0 ? line.left(separator) : QByteArray();
        const QString value = separator > 0 ? QString::fromUtf8(line.mid(separator + 3)) : QString();
        static const QList<QByteArray> known = {"Path", "Folder", "Size", "Packed Size", "Modified", "Created", "Accessed",
            "Attributes", "Encrypted", "Comment", "CRC", "Method", "Characteristics", "Host OS", "Version", "Volume Index",
            "Offset", "Block", "Symbolic Link", "Hard Link", "User", "Group", "Mode", "Link", "INode", "Links",
            "Device Major", "Device Minor", "User ID", "Group ID", "Change", "Solid", "Commented", "Split Before",
            "Split After", "Alternate Stream", "Position", "Checksum", "NT Security", "Anti", "Volume", "ID", "Flags"};
        if (key.isEmpty() || !known.contains(key)) {
            // A file name containing a line break continues the previous value.
            if (lastValue && lastKey == "Path")
                *lastValue += QLatin1Char('\n') + QString::fromUtf8(line);
            continue;
        }
        lastKey = key;
        lastValue = nullptr;
        if (key == "Path") {
            flush();
            haveEntry = true;
            current.rawPath = value;
            lastValue = &current.rawPath;
        } else if (!haveEntry) {
            continue;
        } else if (key == "Folder") {
            folder = value == QLatin1String("+");
        } else if (key == "Size") {
            current.size = value.toLongLong();
        } else if (key == "Packed Size") {
            current.packedSize = value.isEmpty() ? -1 : value.toLongLong();
        } else if (key == "Modified") {
            current.modified = QDateTime::fromString(value.left(19), QStringLiteral("yyyy-MM-dd HH:mm:ss"));
        } else if (key == "Attributes") {
            attributes = value;
        } else if (key == "Encrypted") {
            current.encrypted = value == QLatin1String("+");
        } else if (key == "Symbolic Link") {
            current.linkTarget = value;
        } else if (key == "Hard Link" && !value.isEmpty()) {
            current.linkTarget = value;
            current.kind = Kind::HardLink;
        }
    }
    flush();
    return result;
}

Result parseBsdTar(const QByteArray &output)
{
    Result result;
    const QList<QByteArray> lines = output.split('\n');
    for (const QByteArray &line : lines) {
        if (line.size() < 12)
            continue;
        // mode nlink uid gid size month day year|time name
        QList<QByteArray> fields;
        qsizetype pos = 0;
        while (fields.size() < 8 && pos < line.size()) {
            while (pos < line.size() && line.at(pos) == ' ')
                ++pos;
            const qsizetype start = pos;
            while (pos < line.size() && line.at(pos) != ' ')
                ++pos;
            if (pos > start)
                fields << line.mid(start, pos - start);
        }
        if (fields.size() < 8 || pos >= line.size())
            continue;
        QByteArray rest = line.mid(pos + 1);
        Entry e;
        e.mode = QString::fromLatin1(fields.at(0));
        switch (fields.at(0).at(0)) {
        case 'd': e.kind = Kind::Directory; break;
        case 'l': e.kind = Kind::SymLink; break;
        case 'h': e.kind = Kind::HardLink; break;
        case 'c': case 'b': e.kind = Kind::Device; break;
        case 'p': case 's': e.kind = Kind::Fifo; break;
        case '-': e.kind = Kind::File; break;
        default: e.kind = Kind::Other; break;
        }
        e.size = fields.at(4).contains(',') ? 0 : fields.at(4).toLongLong();
        e.modified = parseLsDate(fields.at(5), fields.at(6), fields.at(7));
        if (e.kind == Kind::SymLink) {
            const qsizetype arrow = rest.indexOf(" -> ");
            if (arrow >= 0) {
                e.linkTarget = QString::fromUtf8(unescape(rest.mid(arrow + 4)));
                rest.truncate(arrow);
            }
        } else if (e.kind == Kind::HardLink) {
            const qsizetype link = rest.indexOf(" link to ");
            if (link >= 0) {
                e.linkTarget = QString::fromUtf8(unescape(rest.mid(link + 9)));
                rest.truncate(link);
            }
        }
        e.rawPath = QString::fromUtf8(unescape(rest));
        if (e.rawPath.isEmpty())
            continue;
        classify(e);
        result.entries.append(e);
    }
    return result;
}

} // namespace Listing
