// SPDX-License-Identifier: GPL-3.0-or-later
#include "systemfacts.h"

#include <command.h>

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLocale>
#include <QRegularExpression>
#include <QUrl>

#include <algorithm>

#include <sys/utsname.h>

namespace ShadowPk {

namespace {

QString tr(const char *text)
{
    return QCoreApplication::translate("ShadowPk", text);
}

QByteArray readFile(const QString &path, qint64 limit = 64 * 1024 * 1024)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return {};
    return f.read(limit);
}

bool isAsciiDigit(QChar c)
{
    return c >= QLatin1Char('0') && c <= QLatin1Char('9');
}

// dpkg's character weights: '~' sorts before everything (even the end of the
// string), letters before all other punctuation.
int weight(QChar c)
{
    if (isAsciiDigit(c))
        return 0;
    if (c.isLetter())
        return c.unicode();
    if (c == QLatin1Char('~'))
        return -1;
    return c.unicode() + 256;
}

int compareFragment(QStringView a, QStringView b)
{
    qsizetype i = 0, j = 0;
    while (i < a.size() || j < b.size()) {
        while ((i < a.size() && !isAsciiDigit(a.at(i))) || (j < b.size() && !isAsciiDigit(b.at(j)))) {
            const int wa = i < a.size() ? weight(a.at(i)) : 0;
            const int wb = j < b.size() ? weight(b.at(j)) : 0;
            if (wa != wb)
                return wa < wb ? -1 : 1;
            ++i;
            ++j;
        }
        while (i < a.size() && a.at(i) == QLatin1Char('0')) ++i;
        while (j < b.size() && b.at(j) == QLatin1Char('0')) ++j;
        int firstDifference = 0;
        while (i < a.size() && j < b.size() && isAsciiDigit(a.at(i)) && isAsciiDigit(b.at(j))) {
            if (firstDifference == 0)
                firstDifference = a.at(i).unicode() - b.at(j).unicode();
            ++i;
            ++j;
        }
        if (i < a.size() && isAsciiDigit(a.at(i)))
            return 1;
        if (j < b.size() && isAsciiDigit(b.at(j)))
            return -1;
        if (firstDifference != 0)
            return firstDifference < 0 ? -1 : 1;
    }
    return 0;
}

} // namespace

int compareVersions(const QString &a, const QString &b)
{
    auto split = [](const QString &v, qlonglong &epoch, QString &upstream, QString &revision) {
        QString rest = v;
        epoch = 0;
        const qsizetype colon = rest.indexOf(QLatin1Char(':'));
        if (colon > 0) {
            bool ok = false;
            const qlonglong e = rest.left(colon).toLongLong(&ok);
            if (ok) {
                epoch = e;
                rest = rest.mid(colon + 1);
            }
        }
        const qsizetype dash = rest.lastIndexOf(QLatin1Char('-'));
        upstream = dash >= 0 ? rest.left(dash) : rest;
        revision = dash >= 0 ? rest.mid(dash + 1) : QString();
    };
    qlonglong ea = 0, eb = 0;
    QString ua, ub, ra, rb;
    split(a, ea, ua, ra);
    split(b, eb, ub, rb);
    if (ea != eb)
        return ea < eb ? -1 : 1;
    const int c = compareFragment(ua, ub);
    return c != 0 ? c : compareFragment(ra, rb);
}

// ---------------------------------------------------------------- APT history

namespace {

QDateTime parseHistoryDate(const QString &value)
{
    // "2026-11-10  21:04:01" — local time, two spaces.
    return QDateTime::fromString(value.simplified(), QStringLiteral("yyyy-MM-dd HH:mm:ss"));
}

QString actionKey(const QString &field)
{
    static const QHash<QString, QString> keys = {
        {QStringLiteral("Install"), QStringLiteral("install")},
        {QStringLiteral("Upgrade"), QStringLiteral("upgrade")},
        {QStringLiteral("Downgrade"), QStringLiteral("downgrade")},
        {QStringLiteral("Reinstall"), QStringLiteral("reinstall")},
        {QStringLiteral("Remove"), QStringLiteral("remove")},
        {QStringLiteral("Purge"), QStringLiteral("purge")},
    };
    return keys.value(field);
}

QJsonArray parseHistoryPackages(const QString &value, bool transition)
{
    // name[:arch] (version[, automatic])   or   name[:arch] (old, new)
    static const QRegularExpression rx(QStringLiteral(R"(([^\s,()]+?)(?::([A-Za-z0-9-]+))? \(([^()]*)\))"));
    QJsonArray packages;
    auto it = rx.globalMatch(value);
    while (it.hasNext()) {
        const QRegularExpressionMatch m = it.next();
        QJsonObject p{
            {QStringLiteral("name"), m.captured(1)},
            {QStringLiteral("arch"), m.captured(2)},
        };
        const QStringList inner = m.captured(3).split(QStringLiteral(", "));
        if (transition) {
            p.insert(QStringLiteral("from"), inner.value(0));
            p.insert(QStringLiteral("to"), inner.value(1));
        } else {
            p.insert(QStringLiteral("version"), inner.value(0));
            p.insert(QStringLiteral("automatic"), inner.contains(QStringLiteral("automatic")));
        }
        packages.append(p);
    }
    return packages;
}

QString historySummary(const QJsonObject &counts)
{
    QStringList parts;
    auto add = [&](const char *key, const QString &text) {
        const int n = counts.value(QLatin1String(key)).toInt();
        if (n > 0)
            parts << text.arg(n);
    };
    add("install", tr("Installed %1"));
    add("upgrade", tr("Upgraded %1"));
    add("downgrade", tr("Downgraded %1"));
    add("reinstall", tr("Reinstalled %1"));
    add("remove", tr("Removed %1"));
    add("purge", tr("Purged %1"));
    return parts.isEmpty() ? tr("No package changes recorded") : parts.join(QStringLiteral(" · "));
}

} // namespace

QJsonArray parseAptHistory(const QByteArray &text, const QString &sourceName)
{
    QJsonArray entries;
    QJsonObject current;
    QJsonArray actions;
    QJsonObject counts;
    int packageCount = 0;
    bool open = false;

    auto flush = [&] {
        if (!open)
            return;
        current.insert(QStringLiteral("actions"), actions);
        current.insert(QStringLiteral("counts"), counts);
        current.insert(QStringLiteral("packageCount"), packageCount);
        current.insert(QStringLiteral("summary"), historySummary(counts));
        current.insert(QStringLiteral("source"), sourceName);
        if (!current.contains(QStringLiteral("commandLine")))
            current.insert(QStringLiteral("commandLine"), QString());
        if (!current.contains(QStringLiteral("requestedBy")))
            current.insert(QStringLiteral("requestedBy"), QString());
        if (!current.contains(QStringLiteral("error")))
            current.insert(QStringLiteral("error"), QString());
        current.insert(QStringLiteral("viaPackageKit"),
                       current.value(QStringLiteral("commandLine")).toString().startsWith(QLatin1String("packagekit")));
        entries.append(current);
        current = QJsonObject();
        actions = QJsonArray();
        counts = QJsonObject();
        packageCount = 0;
        open = false;
    };

    const QList<QByteArray> lines = text.split('\n');
    for (const QByteArray &raw : lines) {
        if (raw.trimmed().isEmpty()) {
            flush();
            continue;
        }
        const qsizetype colon = raw.indexOf(": ");
        if (colon <= 0)
            continue;
        const QString key = QString::fromLatin1(raw.left(colon));
        const QString value = QString::fromUtf8(raw.mid(colon + 2)).trimmed();

        if (key == QLatin1String("Start-Date")) {
            flush();
            open = true;
            const QDateTime when = parseHistoryDate(value);
            current.insert(QStringLiteral("start"), when.isValid() ? when.toString(Qt::ISODate) : value);
            current.insert(QStringLiteral("startText"),
                           when.isValid() ? QLocale().toString(when, QLocale::ShortFormat) : value);
        } else if (key == QLatin1String("End-Date")) {
            open = true;
            const QDateTime when = parseHistoryDate(value);
            current.insert(QStringLiteral("end"), when.isValid() ? when.toString(Qt::ISODate) : value);
        } else if (key == QLatin1String("Commandline")) {
            open = true;
            current.insert(QStringLiteral("commandLine"), value);
        } else if (key == QLatin1String("Requested-By")) {
            open = true;
            current.insert(QStringLiteral("requestedBy"), value);
        } else if (key == QLatin1String("Error")) {
            open = true;
            current.insert(QStringLiteral("error"), value);
        } else {
            const QString action = actionKey(key);
            if (action.isEmpty())
                continue;
            open = true;
            const bool transition = action == QLatin1String("upgrade") || action == QLatin1String("downgrade");
            const QJsonArray packages = parseHistoryPackages(value, transition);
            actions.append(QJsonObject{{QStringLiteral("action"), action}, {QStringLiteral("packages"), packages}});
            counts.insert(action, counts.value(action).toInt() + int(packages.size()));
            packageCount += int(packages.size());
        }
    }
    flush();
    return entries;
}

QJsonObject readAptHistory(int limit, const QString &directory)
{
    QJsonObject result{{QStringLiteral("directory"), directory}};
    QJsonArray files;
    QJsonArray entries;
    bool truncated = false;

    const QDir dir(directory);
    QStringList names = dir.entryList({QStringLiteral("history.log*")}, QDir::Files);
    // history.log (current) first, then .1, .2.gz … — newest to oldest.
    auto rotation = [](const QString &name) {
        const QString rest = name.mid(int(qstrlen("history.log")));
        if (rest.isEmpty())
            return 0;
        return rest.section(QLatin1Char('.'), 1, 1).toInt();
    };
    std::sort(names.begin(), names.end(), [&](const QString &a, const QString &b) { return rotation(a) < rotation(b); });

    if (!dir.exists())
        result.insert(QStringLiteral("error"), tr("%1 does not exist").arg(directory));
    else if (names.isEmpty())
        result.insert(QStringLiteral("error"), tr("No history.log files in %1").arg(directory));

    for (const QString &name : std::as_const(names)) {
        const QString path = dir.filePath(name);
        QJsonObject fileInfo{{QStringLiteral("file"), path}};
        if (limit > 0 && entries.size() >= limit) {
            truncated = true;
            fileInfo.insert(QStringLiteral("skipped"), true);
            files.append(fileInfo);
            continue;
        }

        QByteArray text;
        QString error;
        static const QHash<QString, QString> decompressors = {
            {QStringLiteral("gz"), QStringLiteral("gzip")}, {QStringLiteral("xz"), QStringLiteral("xz")},
            {QStringLiteral("zst"), QStringLiteral("zstd")}, {QStringLiteral("bz2"), QStringLiteral("bzip2")},
        };
        const QString suffix = QFileInfo(name).suffix();
        if (decompressors.contains(suffix)) {
            const QString tool = decompressors.value(suffix);
            if (!Shadow::Command::available(tool)) {
                error = tr("%1 is needed to read this file but is not installed").arg(tool);
            } else {
                const Shadow::CommandResult r = Shadow::Command::runSync(tool, {QStringLiteral("-dc"), QStringLiteral("--"), path}, 30000);
                if (r.ok())
                    text = r.out;
                else
                    error = tr("%1 failed: %2").arg(tool, r.errText().trimmed());
            }
        } else {
            QFile f(path);
            if (f.open(QIODevice::ReadOnly))
                text = f.readAll();
            else
                error = f.errorString();
        }

        if (!error.isEmpty()) {
            fileInfo.insert(QStringLiteral("error"), error);
            files.append(fileInfo);
            continue;
        }
        const QJsonArray parsed = parseAptHistory(text, name);
        fileInfo.insert(QStringLiteral("entries"), parsed.size());
        files.append(fileInfo);
        for (qsizetype i = parsed.size() - 1; i >= 0; --i) {
            if (limit > 0 && entries.size() >= limit) {
                truncated = true;
                break;
            }
            entries.append(parsed.at(i));
        }
    }

    result.insert(QStringLiteral("files"), files);
    result.insert(QStringLiteral("entries"), entries);
    result.insert(QStringLiteral("total"), entries.size());
    result.insert(QStringLiteral("truncated"), truncated);
    return result;
}

// ------------------------------------------------------------ restart required

QJsonObject readRebootState()
{
    const QString flagFile = QStringLiteral("/run/reboot-required");
    const bool present = QFileInfo::exists(flagFile);
    QJsonObject o{
        {QStringLiteral("flagFile"), flagFile},
        {QStringLiteral("flagFilePresent"), present},
        {QStringLiteral("required"), present},
    };
    if (present)
        o.insert(QStringLiteral("message"), QString::fromUtf8(readFile(flagFile, 4096)).trimmed());

    QJsonArray packages;
    const QStringList names = QString::fromUtf8(readFile(flagFile + QStringLiteral(".pkgs"), 1 << 20))
                                  .split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    QStringList seen;
    for (const QString &name : names) {
        const QString n = name.trimmed();
        if (!n.isEmpty() && !seen.contains(n)) {
            seen << n;
            packages.append(n);
        }
    }
    o.insert(QStringLiteral("packages"), packages);

    // Kernels: the one running and the ones installed in /boot. A newer
    // installed kernel of the same flavour only runs after a restart.
    struct utsname u {};
    QString running;
    if (uname(&u) == 0)
        running = QString::fromLocal8Bit(u.release);
    o.insert(QStringLiteral("runningKernel"), running);

    QStringList kernels;
    const QStringList images = QDir(QStringLiteral("/boot")).entryList({QStringLiteral("vmlinuz-*")}, QDir::Files);
    for (const QString &image : images)
        kernels << image.mid(int(qstrlen("vmlinuz-")));
    std::sort(kernels.begin(), kernels.end(), [](const QString &a, const QString &b) { return compareVersions(a, b) < 0; });
    o.insert(QStringLiteral("installedKernels"), QJsonArray::fromStringList(kernels));

    // Flavour = what follows the version ("amd64", "rt-amd64", "cloud-arm64").
    auto flavour = [](const QString &release) { return release.section(QLatin1Char('-'), 1); };
    QString newest;
    bool runningInstalled = false;
    for (const QString &k : std::as_const(kernels)) {
        if (k == running)
            runningInstalled = true;
        if (flavour(k) == flavour(running) && (newest.isEmpty() || compareVersions(k, newest) > 0))
            newest = k;
    }
    const bool newer = runningInstalled && !newest.isEmpty() && compareVersions(newest, running) > 0;
    o.insert(QStringLiteral("newestKernel"), newest);
    o.insert(QStringLiteral("newerKernelInstalled"), newer);
    return o;
}

// ------------------------------------------------------------------ dpkg health

QJsonObject checkDpkgHealth()
{
    QJsonObject o{{QStringLiteral("checkedAt"), QDateTime::currentDateTime().toString(Qt::ISODate)}};
    const bool haveDpkg = Shadow::Command::available(QStringLiteral("dpkg"))
                          && Shadow::Command::available(QStringLiteral("dpkg-query"));
    o.insert(QStringLiteral("dpkgAvailable"), haveDpkg);
    if (!haveDpkg) {
        o.insert(QStringLiteral("healthy"), false);
        o.insert(QStringLiteral("error"), tr("dpkg is not installed; the package database cannot be checked"));
        return o;
    }

    // 1. dpkg's own consistency report.
    const Shadow::CommandResult audit = Shadow::Command::runSync(QStringLiteral("dpkg"), {QStringLiteral("--audit")}, 120000);
    const QString auditOutput = audit.outText().trimmed();
    QJsonObject auditJson{
        {QStringLiteral("exitCode"), audit.exitCode},
        {QStringLiteral("output"), auditOutput},
    };
    if (!audit.errText().trimmed().isEmpty())
        auditJson.insert(QStringLiteral("error"), audit.errText().trimmed());
    o.insert(QStringLiteral("audit"), auditJson);

    // 2. Journal of an interrupted dpkg run.
    const QDir updates(QStringLiteral("/var/lib/dpkg/updates"));
    QStringList pending = updates.entryList(QDir::Files);
    pending.removeAll(QStringLiteral("tmp.i"));
    o.insert(QStringLiteral("interruptedUpdates"), QJsonArray::fromStringList(pending));

    // 3. Packages left in an intermediate state.
    QJsonArray broken;
    const Shadow::CommandResult query = Shadow::Command::runSync(
        QStringLiteral("dpkg-query"),
        {QStringLiteral("-W"), QStringLiteral("-f=${db:Status-Abbrev}\\t${Package}\\t${Architecture}\\t${Version}\\n")}, 120000);
    if (!query.ok()) {
        o.insert(QStringLiteral("queryError"), query.errText().trimmed().isEmpty()
                     ? tr("dpkg-query failed (exit code %1)").arg(query.exitCode) : query.errText().trimmed());
    } else {
        const QStringList lines = query.outText().split(QLatin1Char('\n'), Qt::SkipEmptyParts);
        for (const QString &line : lines) {
            const QStringList f = line.split(QLatin1Char('\t'));
            if (f.size() < 2 || f.at(0).size() < 2)
                continue;
            const QChar state = f.at(0).at(1);
            const bool reinstall = f.at(0).size() > 2 && f.at(0).at(2) == QLatin1Char('R');
            QString stateName, meaning;
            if (state == QLatin1Char('H')) { stateName = QStringLiteral("half-installed"); meaning = tr("Installation was interrupted"); }
            else if (state == QLatin1Char('U')) { stateName = QStringLiteral("unpacked"); meaning = tr("Unpacked but not configured"); }
            else if (state == QLatin1Char('F')) { stateName = QStringLiteral("half-configured"); meaning = tr("Configuration was interrupted or failed"); }
            else if (state == QLatin1Char('W')) { stateName = QStringLiteral("triggers-awaited"); meaning = tr("Waiting for another package's triggers"); }
            else if (state == QLatin1Char('t')) { stateName = QStringLiteral("triggers-pending"); meaning = tr("Has triggers that still need to run"); }
            else if (reinstall) { stateName = QStringLiteral("reinstall-required"); meaning = tr("Must be reinstalled"); }
            else continue;
            if (reinstall && stateName != QLatin1String("reinstall-required"))
                meaning += QStringLiteral("; ") + tr("must be reinstalled");
            broken.append(QJsonObject{
                {QStringLiteral("package"), f.at(1)},
                {QStringLiteral("architecture"), f.value(2)},
                {QStringLiteral("version"), f.value(3)},
                {QStringLiteral("status"), f.at(0).trimmed()},
                {QStringLiteral("state"), stateName},
                {QStringLiteral("meaning"), meaning},
            });
        }
    }
    o.insert(QStringLiteral("brokenPackages"), broken);
    o.insert(QStringLiteral("healthy"), auditOutput.isEmpty() && pending.isEmpty() && broken.isEmpty() && query.ok());
    return o;
}

// ------------------------------------------------------------------ APT sources

QString repoCategory(const QString &repoIdOrUri, const QString &origin)
{
    // 1. What the repository says about itself (Origin field of its Release file).
    const QString o = origin.trimmed().toLower();
    if (o == QLatin1String("debian") || o.startsWith(QLatin1String("debian ")))
        return QStringLiteral("debian");
    if (o == QLatin1String("shadow") || o.startsWith(QLatin1String("shadow ")))
        return QStringLiteral("shadow");

    // 2. The sources file Shadow OS ships for its own repository (shadow.sources / shadow*.list).
    //    PackageKit repository IDs start with that file's path.
    const QString text = repoIdOrUri.toLower();
    if (text.startsWith(QLatin1Char('/'))) {
        const QString file = text.section(QLatin1Char(':'), 0, 0).section(QLatin1Char(' '), 0, 0);
        if (QFileInfo(file).fileName().startsWith(QLatin1String("shadow")))
            return QStringLiteral("shadow");
    }

    // 3. Debian's own archive hosts. (Host names merely containing "shadow"
    //    are not trusted as SHADOW: that is also the name of a common
    //    repository-manager product.)
    static const QRegularExpression debianHost(QStringLiteral(R"((^|[/@.\s])([a-z0-9-]+\.)*debian\.(org|net)([/:\s]|$))"));
    if (debianHost.match(text).hasMatch())
        return QStringLiteral("debian");
    return QStringLiteral("third-party");
}

QString repoCategoryText(const QString &category)
{
    if (category == QLatin1String("debian"))
        return tr("Debian base");
    if (category == QLatin1String("shadow"))
        return tr("SHADOW");
    return tr("Third party");
}

namespace {

struct SourceLine
{
    QString file;
    QString format;
    QStringList types;
    QString uri;
    QString suite;
    QStringList components;
    bool enabled = true;
    QString signedBy;
    QString trusted;
    bool allowInsecure = false;
};

// APT's URItoFileName(): drop the scheme and credentials, quote, '/' -> '_'.
QString uriToListName(const QString &uri)
{
    QString s = uri;
    const qsizetype scheme = s.indexOf(QStringLiteral("://"));
    if (scheme >= 0)
        s = s.mid(scheme + 3);
    else if (s.contains(QLatin1Char(':')))
        s = s.mid(s.indexOf(QLatin1Char(':')) + 1);
    const qsizetype slash = s.indexOf(QLatin1Char('/'));
    const qsizetype at = s.indexOf(QLatin1Char('@'));
    if (at >= 0 && (slash < 0 || at < slash))
        s = s.mid(at + 1);
    static const QString quoted = QStringLiteral("\\|{}[]<>\"^~_=!@#$%^&*");
    QString out;
    for (const QChar c : std::as_const(s)) {
        if (c == QLatin1Char('/'))
            out += QLatin1Char('_');
        else if (quoted.contains(c) || c.unicode() <= 0x20 || c.unicode() >= 0x7f)
            out += QStringLiteral("%%1").arg(c.unicode(), 2, 16, QLatin1Char('0'));
        else
            out += c;
    }
    return out;
}

QHash<QString, QString> parseOptions(const QString &text)
{
    QHash<QString, QString> options;
    const QStringList parts = text.split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
    for (const QString &part : parts) {
        const qsizetype eq = part.indexOf(QLatin1Char('='));
        if (eq > 0)
            options.insert(part.left(eq).toLower(), part.mid(eq + 1));
    }
    return options;
}

bool truthy(const QString &value)
{
    const QString v = value.trimmed().toLower();
    return v == QLatin1String("yes") || v == QLatin1String("true") || v == QLatin1String("1")
           || v == QLatin1String("on") || v == QLatin1String("with") || v == QLatin1String("enable");
}

void parseOneLineFile(const QString &path, QList<SourceLine> &out)
{
    const QStringList lines = QString::fromUtf8(readFile(path, 4 << 20)).split(QLatin1Char('\n'));
    for (QString line : lines) {
        const qsizetype hash = line.indexOf(QLatin1Char('#'));
        if (hash >= 0)
            line.truncate(hash);
        line = line.trimmed();
        if (line.isEmpty())
            continue;
        SourceLine s;
        s.file = path;
        s.format = QStringLiteral("one-line");
        qsizetype pos = line.indexOf(QRegularExpression(QStringLiteral("\\s")));
        if (pos < 0)
            continue;
        const QString type = line.left(pos);
        if (type != QLatin1String("deb") && type != QLatin1String("deb-src"))
            continue;
        s.types << type;
        QString rest = line.mid(pos).trimmed();
        if (rest.startsWith(QLatin1Char('['))) {
            const qsizetype close = rest.indexOf(QLatin1Char(']'));
            if (close < 0)
                continue;
            const QHash<QString, QString> options = parseOptions(rest.mid(1, close - 1));
            s.signedBy = options.value(QStringLiteral("signed-by"));
            s.trusted = options.value(QStringLiteral("trusted"));
            s.allowInsecure = truthy(options.value(QStringLiteral("allow-insecure")));
            rest = rest.mid(close + 1).trimmed();
        }
        const QStringList tokens = rest.split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
        if (tokens.size() < 2)
            continue;
        s.uri = tokens.at(0);
        s.suite = tokens.at(1);
        s.components = tokens.mid(2);
        out.append(s);
    }
}

void parseDeb822File(const QString &path, QList<SourceLine> &out)
{
    const QStringList lines = QString::fromUtf8(readFile(path, 4 << 20)).split(QLatin1Char('\n'));
    QHash<QString, QString> stanza;
    QString lastKey;
    auto flush = [&] {
        if (stanza.isEmpty())
            return;
        const QRegularExpression space(QStringLiteral("\\s+"));
        const QStringList types = stanza.value(QStringLiteral("types")).split(space, Qt::SkipEmptyParts);
        const QStringList uris = stanza.value(QStringLiteral("uris")).split(space, Qt::SkipEmptyParts);
        const QStringList suites = stanza.value(QStringLiteral("suites")).split(space, Qt::SkipEmptyParts);
        for (const QString &uri : uris) {
            for (const QString &suite : suites) {
                SourceLine s;
                s.file = path;
                s.format = QStringLiteral("deb822");
                s.types = types;
                s.uri = uri;
                s.suite = suite;
                s.components = stanza.value(QStringLiteral("components")).split(space, Qt::SkipEmptyParts);
                s.enabled = !stanza.contains(QStringLiteral("enabled")) || truthy(stanza.value(QStringLiteral("enabled")));
                // Signed-By may be a key ring path or an embedded key block; show the first line only.
                s.signedBy = stanza.value(QStringLiteral("signed-by")).section(QLatin1Char('\n'), 0, 0).trimmed();
                if (s.signedBy.isEmpty() && stanza.contains(QStringLiteral("signed-by")))
                    s.signedBy = tr("(key embedded in the sources file)");
                s.trusted = stanza.value(QStringLiteral("trusted"));
                s.allowInsecure = truthy(stanza.value(QStringLiteral("allow-insecure")));
                out.append(s);
            }
        }
        stanza.clear();
        lastKey.clear();
    };
    for (const QString &line : lines) {
        if (line.trimmed().isEmpty()) {
            flush();
            continue;
        }
        if (line.startsWith(QLatin1Char('#')))
            continue;
        if ((line.startsWith(QLatin1Char(' ')) || line.startsWith(QLatin1Char('\t'))) && !lastKey.isEmpty()) {
            stanza[lastKey] += QLatin1Char('\n') + line.trimmed();
            continue;
        }
        const qsizetype colon = line.indexOf(QLatin1Char(':'));
        if (colon <= 0)
            continue;
        lastKey = line.left(colon).trimmed().toLower();
        stanza.insert(lastKey, line.mid(colon + 1).trimmed());
    }
    flush();
}

// The header fields of a (possibly clear-signed) Release file.
QHash<QString, QString> releaseFields(const QString &path)
{
    QHash<QString, QString> fields;
    const QStringList lines = QString::fromUtf8(readFile(path, 16 * 1024)).split(QLatin1Char('\n'));
    for (const QString &line : lines) {
        if (line.startsWith(QLatin1Char(' ')) || line.startsWith(QLatin1Char('-')))
            continue;
        const qsizetype colon = line.indexOf(QLatin1Char(':'));
        if (colon <= 0)
            continue;
        const QString key = line.left(colon);
        if (key == QLatin1String("MD5Sum") || key == QLatin1String("SHA1") || key == QLatin1String("SHA256"))
            break;
        fields.insert(key.toLower(), line.mid(colon + 1).trimmed());
    }
    return fields;
}

} // namespace

QJsonObject readAptSources(const QString &root)
{
    QList<SourceLine> lines;
    QJsonArray errors;
    const QString etc = root + QStringLiteral("/etc/apt");
    const QString listsDir = root + QStringLiteral("/var/lib/apt/lists");

    if (QFileInfo::exists(etc + QStringLiteral("/sources.list")))
        parseOneLineFile(etc + QStringLiteral("/sources.list"), lines);
    const QDir partsDir(etc + QStringLiteral("/sources.list.d"));
    const QStringList parts = partsDir.entryList({QStringLiteral("*.list"), QStringLiteral("*.sources")}, QDir::Files, QDir::Name);
    for (const QString &name : parts) {
        const QString path = partsDir.filePath(name);
        if (!QFileInfo(path).isReadable()) {
            errors.append(tr("%1 is not readable").arg(path));
            continue;
        }
        if (name.endsWith(QLatin1String(".sources")))
            parseDeb822File(path, lines);
        else
            parseOneLineFile(path, lines);
    }

    // One entry per repository (URI + suite); "deb" and "deb-src" lines share a Release file.
    QJsonArray sources;
    QHash<QString, int> index;
    for (const SourceLine &s : std::as_const(lines)) {
        const QString key = s.file + QLatin1Char('|') + s.uri + QLatin1Char('|') + s.suite;
        if (index.contains(key)) {
            QJsonObject existing = sources.at(index.value(key)).toObject();
            QStringList types = existing.value(QStringLiteral("types")).toVariant().toStringList();
            for (const QString &t : s.types) {
                if (!types.contains(t))
                    types << t;
            }
            existing.insert(QStringLiteral("types"), QJsonArray::fromStringList(types));
            sources.replace(index.value(key), existing);
            continue;
        }

        QString base = s.uri;
        while (base.endsWith(QLatin1Char('/')))
            base.chop(1);
        // Flat repositories ("deb uri ./") keep Release next to the packages.
        const bool flat = s.suite.endsWith(QLatin1Char('/'));
        const QString prefix = flat ? uriToListName(base + QLatin1Char('/') + s.suite)
                                    : uriToListName(base + QStringLiteral("/dists/") + s.suite + QLatin1Char('/'));
        const QString inRelease = listsDir + QLatin1Char('/') + prefix + QStringLiteral("InRelease");
        const QString release = listsDir + QLatin1Char('/') + prefix + QStringLiteral("Release");
        const QString releaseGpg = release + QStringLiteral(".gpg");

        QString metadata, metadataFile;
        if (QFileInfo::exists(inRelease)) { metadata = QStringLiteral("inrelease"); metadataFile = inRelease; }
        else if (QFileInfo::exists(release) && QFileInfo::exists(releaseGpg)) { metadata = QStringLiteral("release+gpg"); metadataFile = release; }
        else if (QFileInfo::exists(release)) { metadata = QStringLiteral("release-unsigned"); metadataFile = release; }
        else metadata = QStringLiteral("missing");

        if (metadata == QLatin1String("missing")) {
            // Package indexes without any Release file: APT used the source
            // (allow-insecure / trusted) although nothing vouches for it.
            // Indexes of file: sources are read in place, not copied to the lists.
            bool indexes = !QDir(listsDir).entryList({prefix + QStringLiteral("*Packages*")}, QDir::Files).isEmpty();
            if (!indexes && base.startsWith(QLatin1String("file:"))) {
                QString directory = base.mid(5);
                while (directory.startsWith(QLatin1String("//")))
                    directory.remove(0, 1);
                directory = root + directory + QLatin1Char('/') + (flat ? s.suite : QStringLiteral("dists/") + s.suite);
                const QDir dir(directory);
                if (dir.exists(QStringLiteral("InRelease")) || dir.exists(QStringLiteral("Release")))
                    indexes = false;    // has an index that was simply not fetched yet
                else
                    indexes = !dir.entryList({QStringLiteral("Packages*")}, QDir::Files).isEmpty();
            }
            if (indexes)
                metadata = QStringLiteral("packages-without-release");
        }

        QString verification, verificationText;
        if (!s.enabled) {
            verification = QStringLiteral("disabled");
            verificationText = tr("Disabled — not used");
        } else if (truthy(s.trusted)) {
            verification = QStringLiteral("signature-check-disabled");
            verificationText = tr("Signature checking is switched off for this source (trusted=yes)");
        } else if (metadata == QLatin1String("inrelease") || metadata == QLatin1String("release+gpg")) {
            verification = QStringLiteral("signed");
            verificationText = s.allowInsecure
                ? tr("Signed index on disk, but this source is allowed to be insecure (allow-insecure=yes)")
                : tr("Signed — APT verified the repository index when it was downloaded");
        } else if (metadata == QLatin1String("release-unsigned")) {
            verification = QStringLiteral("unsigned");
            verificationText = tr("NOT signed — the repository index has no signature");
        } else if (metadata == QLatin1String("packages-without-release")) {
            verification = QStringLiteral("unsigned");
            verificationText = tr("NOT signed — this source has package lists but no repository index (Release file) at all");
        } else {
            verification = QStringLiteral("not-downloaded");
            verificationText = tr("No repository index downloaded yet — refresh the software lists");
        }

        const QHash<QString, QString> fields = metadataFile.isEmpty() ? QHash<QString, QString>() : releaseFields(metadataFile);
        const QString origin = fields.value(QStringLiteral("origin"));
        const QString category = repoCategory(s.file + QLatin1Char(' ') + s.uri, origin);

        index.insert(key, int(sources.size()));
        sources.append(QJsonObject{
            {QStringLiteral("file"), s.file},
            {QStringLiteral("format"), s.format},
            {QStringLiteral("types"), QJsonArray::fromStringList(s.types)},
            {QStringLiteral("uri"), s.uri},
            {QStringLiteral("suite"), s.suite},
            {QStringLiteral("components"), QJsonArray::fromStringList(s.components)},
            {QStringLiteral("enabled"), s.enabled},
            {QStringLiteral("signedBy"), s.signedBy},
            {QStringLiteral("signedByExists"), s.signedBy.startsWith(QLatin1Char('/'))
                                                    ? QJsonValue(QFileInfo::exists(root + s.signedBy.section(QLatin1Char(','), 0, 0)))
                                                    : QJsonValue()},
            {QStringLiteral("trustedOption"), s.trusted},
            {QStringLiteral("allowInsecure"), s.allowInsecure},
            {QStringLiteral("metadata"), metadata},
            {QStringLiteral("metadataFile"), metadataFile},
            {QStringLiteral("verification"), verification},
            {QStringLiteral("verificationText"), verificationText},
            {QStringLiteral("origin"), origin},
            {QStringLiteral("label"), fields.value(QStringLiteral("label"))},
            {QStringLiteral("codename"), fields.value(QStringLiteral("codename"))},
            {QStringLiteral("date"), fields.value(QStringLiteral("date"))},
            {QStringLiteral("validUntil"), fields.value(QStringLiteral("valid-until"))},
            {QStringLiteral("category"), category},
            {QStringLiteral("categoryText"), repoCategoryText(category)},
        });
    }

    return QJsonObject{
        {QStringLiteral("sources"), sources},
        {QStringLiteral("errors"), errors},
        {QStringLiteral("listsDirectory"), listsDir},
    };
}

} // namespace ShadowPk
