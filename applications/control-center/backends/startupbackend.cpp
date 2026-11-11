// SPDX-License-Identifier: GPL-3.0-or-later
#include "startupbackend.h"

#include "dumpregistry.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QLocale>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStandardPaths>

NEXUS_CC_DUMP("startup", [] { return QJsonValue(StartupBackend().toJson()); })

namespace {

const QString kGroup = QStringLiteral("[Desktop Entry]");
const QString kOverrideKey = QStringLiteral("X-NEXUS-Autostart-Override");

// The [Desktop Entry] group of a desktop file, kept as lines so that a change
// touches only the keys it sets (comments, translations and other groups stay).
class DesktopFile
{
public:
    bool load(const QString &path)
    {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly))
            return false;
        m_lines = QString::fromUtf8(file.readAll()).split(QLatin1Char('\n'));
        while (!m_lines.isEmpty() && m_lines.last().isEmpty())
            m_lines.removeLast();
        return true;
    }

    bool save(const QString &path) const
    {
        QDir().mkpath(QFileInfo(path).absolutePath());
        QSaveFile file(path);
        if (!file.open(QIODevice::WriteOnly))
            return false;
        file.write((m_lines.join(QLatin1Char('\n')) + QLatin1Char('\n')).toUtf8());
        return file.commit();
    }

    bool hasGroup() const { return groupStart() >= 0; }

    QString value(const QString &key) const
    {
        const int line = findKey(key);
        if (line < 0)
            return QString();
        return unescape(m_lines.at(line).mid(m_lines.at(line).indexOf(QLatin1Char('=')) + 1).trimmed());
    }

    // Name[de_DE], Name[de], Name — in the order the specification gives.
    QString localized(const QString &key) const
    {
        const QString locale = QLocale().name();            // de_DE
        const QString language = locale.section(QLatin1Char('_'), 0, 0);
        for (const QString &suffix : {locale, language}) {
            if (suffix.isEmpty() || suffix == QLatin1String("C"))
                continue;
            const QString v = value(key + QLatin1Char('[') + suffix + QLatin1Char(']'));
            if (!v.isEmpty())
                return v;
        }
        return value(key);
    }

    bool boolean(const QString &key, bool fallback) const
    {
        const QString v = value(key).toLower();
        if (v == QLatin1String("true"))
            return true;
        if (v == QLatin1String("false"))
            return false;
        return fallback;
    }

    QStringList list(const QString &key) const
    {
        return value(key).split(QLatin1Char(';'), Qt::SkipEmptyParts);
    }

    void set(const QString &key, const QString &value)
    {
        const QString text = key + QLatin1Char('=') + escape(value);
        const int line = findKey(key);
        if (line >= 0) {
            m_lines[line] = text;
            return;
        }
        int start = groupStart();
        if (start < 0) {
            m_lines.append(kGroup);
            start = m_lines.size() - 1;
        }
        int end = groupEnd(start);
        while (end > start + 1 && m_lines.at(end - 1).trimmed().isEmpty())
            --end;
        m_lines.insert(end, text);
    }

    void unset(const QString &key)
    {
        const int line = findKey(key);
        if (line >= 0)
            m_lines.removeAt(line);
    }

private:
    static QString escape(QString v)
    {
        return v.replace(QLatin1Char('\\'), QLatin1String("\\\\")).replace(QLatin1Char('\n'), QLatin1String("\\n"));
    }
    static QString unescape(const QString &v)
    {
        QString out;
        for (int i = 0; i < v.size(); ++i) {
            if (v.at(i) == QLatin1Char('\\') && i + 1 < v.size()) {
                const QChar n = v.at(++i);
                out += n == QLatin1Char('n') ? QLatin1Char('\n') : n == QLatin1Char('t') ? QLatin1Char('\t')
                     : n == QLatin1Char('s') ? QLatin1Char(' ') : n;
            } else {
                out += v.at(i);
            }
        }
        return out;
    }
    int groupStart() const
    {
        for (int i = 0; i < m_lines.size(); ++i) {
            if (m_lines.at(i).trimmed() == kGroup)
                return i;
        }
        return -1;
    }
    int groupEnd(int start) const
    {
        for (int i = start + 1; i < m_lines.size(); ++i) {
            if (m_lines.at(i).trimmed().startsWith(QLatin1Char('[')))
                return i;
        }
        return m_lines.size();
    }
    int findKey(const QString &key) const
    {
        const int start = groupStart();
        if (start < 0)
            return -1;
        const int end = groupEnd(start);
        for (int i = start + 1; i < end; ++i) {
            const QString line = m_lines.at(i);
            if (line.startsWith(QLatin1Char('#')))
                continue;
            const int eq = line.indexOf(QLatin1Char('='));
            if (eq > 0 && line.left(eq).trimmed() == key)
                return i;
        }
        return -1;
    }

    QStringList m_lines;
};

// First word of an Exec= value, honouring double quotes.
QString execProgram(const QString &exec)
{
    const QString s = exec.trimmed();
    if (s.startsWith(QLatin1Char('"'))) {
        const int end = s.indexOf(QLatin1Char('"'), 1);
        return end > 0 ? s.mid(1, end - 1) : s.mid(1);
    }
    return s.section(QRegularExpression(QStringLiteral("\\s+")), 0, 0);
}

bool programExists(const QString &program)
{
    if (program.isEmpty())
        return false;
    if (program.contains(QLatin1Char('/'))) {
        const QFileInfo info(program);
        return info.isFile() && info.isExecutable();
    }
    return !QStandardPaths::findExecutable(program).isEmpty();
}

QStringList currentDesktops()
{
    return qEnvironmentVariable("XDG_CURRENT_DESKTOP").split(QLatin1Char(':'), Qt::SkipEmptyParts);
}

// Why the session will not start this entry even though it is enabled.
QString conditionOf(const DesktopFile &file)
{
    const QString tryExec = file.value(QStringLiteral("TryExec"));
    if (!tryExec.isEmpty() && !programExists(tryExec))
        return StartupBackend::tr("Skipped: %1 is not installed").arg(tryExec);
    const QString program = execProgram(file.value(QStringLiteral("Exec")));
    if (program.isEmpty())
        return StartupBackend::tr("Skipped: the entry has no command");
    if (!programExists(program))
        return StartupBackend::tr("Skipped: %1 is not installed").arg(program);

    const QStringList desktops = currentDesktops();
    const QStringList only = file.list(QStringLiteral("OnlyShowIn"));
    const QStringList notIn = file.list(QStringLiteral("NotShowIn"));
    auto intersects = [&](const QStringList &names) {
        for (const QString &name : names) {
            if (desktops.contains(name))
                return true;
        }
        return false;
    };
    if (!only.isEmpty() && !desktops.isEmpty() && !intersects(only))
        return StartupBackend::tr("Only starts in: %1").arg(only.join(QStringLiteral(", ")));
    if (!notIn.isEmpty() && intersects(notIn))
        return StartupBackend::tr("Does not start in this desktop (%1)").arg(notIn.join(QStringLiteral(", ")));
    return QString();
}

bool isEnabled(const DesktopFile &file)
{
    return !file.boolean(QStringLiteral("Hidden"), false)
           && file.boolean(QStringLiteral("X-GNOME-Autostart-enabled"), true);
}

void writeEnabled(DesktopFile &file, bool enabled)
{
    if (enabled)
        file.unset(QStringLiteral("Hidden"));
    else
        file.set(QStringLiteral("Hidden"), QStringLiteral("true"));
    file.set(QStringLiteral("X-GNOME-Autostart-enabled"), enabled ? QStringLiteral("true") : QStringLiteral("false"));
}

bool validId(const QString &id)
{
    return id.endsWith(QLatin1String(".desktop")) && !id.contains(QLatin1Char('/')) && !id.startsWith(QLatin1Char('.'));
}

} // namespace

StartupBackend::StartupBackend(QObject *parent)
    : QObject(parent)
{
    m_reload.setSingleShot(true);
    m_reload.setInterval(150);
    connect(&m_reload, &QTimer::timeout, this, &StartupBackend::refresh);
    connect(&m_watcher, &QFileSystemWatcher::directoryChanged, &m_reload, qOverload<>(&QTimer::start));
    connect(&m_watcher, &QFileSystemWatcher::fileChanged, &m_reload, qOverload<>(&QTimer::start));
    refresh();
}

QString StartupBackend::userDirectory() const
{
    return QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) + QStringLiteral("/autostart");
}

QStringList StartupBackend::systemDirectories() const
{
    QString dirs = qEnvironmentVariable("XDG_CONFIG_DIRS");
    if (dirs.isEmpty())
        dirs = QStringLiteral("/etc/xdg");
    QStringList out;
    const QStringList parts = dirs.split(QLatin1Char(':'), Qt::SkipEmptyParts);
    for (const QString &dir : parts) {
        const QString path = QDir::cleanPath(dir + QStringLiteral("/autostart"));
        if (!out.contains(path) && path != userDirectory())
            out << path;
    }
    return out;
}

void StartupBackend::watch()
{
    const QString user = userDirectory();
    // Watch the autostart directory, or its parent until it exists.
    const QString target = QFileInfo(user).isDir() ? user : QFileInfo(user).absolutePath();
    QStringList wanted{target};
    const QStringList systemDirs = systemDirectories();
    for (const QString &dir : systemDirs) {
        if (QFileInfo(dir).isDir())
            wanted << dir;
    }
    for (const QString &dir : std::as_const(wanted)) {
        if (QFileInfo(dir).isDir() && !m_watcher.directories().contains(dir))
            m_watcher.addPath(dir);
    }
    // Content changes of existing files.
    const QStringList files = QDir(user).entryList({QStringLiteral("*.desktop")}, QDir::Files);
    for (const QString &name : files) {
        const QString path = user + QLatin1Char('/') + name;
        if (!m_watcher.files().contains(path))
            m_watcher.addPath(path);
    }
}

void StartupBackend::refresh()
{
    // System entries: the first directory in $XDG_CONFIG_DIRS wins.
    QMap<QString, QString> systemFiles;
    const QStringList systemDirs = systemDirectories();
    for (const QString &dir : systemDirs) {
        const QStringList names = QDir(dir).entryList({QStringLiteral("*.desktop")}, QDir::Files);
        for (const QString &name : names) {
            if (!systemFiles.contains(name))
                systemFiles.insert(name, dir + QLatin1Char('/') + name);
        }
    }
    QMap<QString, QString> userFiles;
    const QString user = userDirectory();
    const QStringList names = QDir(user).entryList({QStringLiteral("*.desktop")}, QDir::Files);
    for (const QString &name : names)
        userFiles.insert(name, user + QLatin1Char('/') + name);

    QStringList ids = systemFiles.keys() + userFiles.keys();
    ids.removeDuplicates();

    QVariantList entries;
    for (const QString &id : std::as_const(ids)) {
        const bool hasUser = userFiles.contains(id);
        const bool hasSystem = systemFiles.contains(id);
        DesktopFile effective;
        const QString path = hasUser ? userFiles.value(id) : systemFiles.value(id);
        if (!effective.load(path) || !effective.hasGroup())
            continue;
        // What is shown (name, icon, command) comes from the file in effect;
        // a bare override without those keys falls back to the system file.
        DesktopFile shown = effective;
        if (hasUser && hasSystem && effective.value(QStringLiteral("Exec")).isEmpty()) {
            DesktopFile system;
            if (system.load(systemFiles.value(id)))
                shown = system;
        }
        const QString type = shown.value(QStringLiteral("Type"));
        if (!type.isEmpty() && type != QLatin1String("Application"))
            continue;
        // A hidden user file without a system counterpart is "deleted" for the
        // session but is still the user's entry: listed as disabled.
        const bool enabled = isEnabled(effective);
        QString name = shown.localized(QStringLiteral("Name"));
        if (name.isEmpty())
            name = id.chopped(8);
        entries << QVariantMap{
            {QStringLiteral("id"), id},
            {QStringLiteral("name"), name},
            {QStringLiteral("comment"), shown.localized(QStringLiteral("Comment"))},
            {QStringLiteral("icon"), shown.value(QStringLiteral("Icon"))},
            {QStringLiteral("command"), shown.value(QStringLiteral("Exec"))},
            {QStringLiteral("path"), path},
            {QStringLiteral("enabled"), enabled},
            {QStringLiteral("system"), hasSystem},
            {QStringLiteral("userFile"), hasUser},
            {QStringLiteral("overridden"), hasUser && hasSystem},
            {QStringLiteral("condition"), enabled ? conditionOf(shown) : QString()},
        };
    }
    std::sort(entries.begin(), entries.end(), [](const QVariant &a, const QVariant &b) {
        return QString::localeAwareCompare(a.toMap().value(QStringLiteral("name")).toString(),
                                           b.toMap().value(QStringLiteral("name")).toString()) < 0;
    });
    watch();
    if (entries != m_entries) {
        m_entries = entries;
        Q_EMIT entriesChanged();
    }
}

void StartupBackend::loadApplications()
{
    QVariantList apps;
    QStringList seen;
    const QStringList dirs = QStandardPaths::standardLocations(QStandardPaths::ApplicationsLocation);
    for (const QString &dir : dirs) {
        const QStringList names = QDir(dir).entryList({QStringLiteral("*.desktop")}, QDir::Files);
        for (const QString &name : names) {
            if (seen.contains(name))
                continue;
            seen << name;       // a file in an earlier directory shadows later ones, even if hidden
            DesktopFile file;
            if (!file.load(dir + QLatin1Char('/') + name) || !file.hasGroup())
                continue;
            if (file.value(QStringLiteral("Type")) != QLatin1String("Application")
                || file.boolean(QStringLiteral("NoDisplay"), false) || file.boolean(QStringLiteral("Hidden"), false))
                continue;
            const QString exec = file.value(QStringLiteral("Exec"));
            if (!programExists(execProgram(exec)))
                continue;
            const QString tryExec = file.value(QStringLiteral("TryExec"));
            if (!tryExec.isEmpty() && !programExists(tryExec))
                continue;
            apps << QVariantMap{
                {QStringLiteral("id"), name},
                {QStringLiteral("name"), file.localized(QStringLiteral("Name"))},
                {QStringLiteral("comment"), file.localized(QStringLiteral("Comment"))},
                {QStringLiteral("icon"), file.value(QStringLiteral("Icon"))},
                {QStringLiteral("command"), exec},
                {QStringLiteral("path"), dir + QLatin1Char('/') + name},
            };
        }
    }
    std::sort(apps.begin(), apps.end(), [](const QVariant &a, const QVariant &b) {
        return QString::localeAwareCompare(a.toMap().value(QStringLiteral("name")).toString(),
                                           b.toMap().value(QStringLiteral("name")).toString()) < 0;
    });
    if (apps != m_applications) {
        m_applications = apps;
        Q_EMIT applicationsChanged();
    }
}

bool StartupBackend::setEnabled(const QString &id, bool enabled)
{
    if (!validId(id))
        return false;
    const QString userPath = userDirectory() + QLatin1Char('/') + id;
    QString systemPath;
    const QStringList systemDirs = systemDirectories();
    for (const QString &dir : systemDirs) {
        if (QFileInfo::exists(dir + QLatin1Char('/') + id)) {
            systemPath = dir + QLatin1Char('/') + id;
            break;
        }
    }

    DesktopFile file;
    bool ok = false;
    if (QFileInfo::exists(userPath)) {
        if (!file.load(userPath)) {
            Q_EMIT failed(tr("%1 could not be read.").arg(userPath));
            return false;
        }
        if (enabled && !systemPath.isEmpty() && file.boolean(kOverrideKey, false)) {
            // The copy existed only to switch the system entry off.
            ok = QFile::remove(userPath);
        } else {
            writeEnabled(file, enabled);
            ok = file.save(userPath);
        }
    } else if (!systemPath.isEmpty()) {
        if (!file.load(systemPath)) {
            Q_EMIT failed(tr("%1 could not be read.").arg(systemPath));
            return false;
        }
        if (isEnabled(file) == enabled) {
            ok = true;
        } else {
            writeEnabled(file, enabled);
            file.set(kOverrideKey, QStringLiteral("true"));
            ok = file.save(userPath);
        }
    } else {
        Q_EMIT failed(tr("The startup entry %1 no longer exists.").arg(id));
        refresh();
        return false;
    }
    if (!ok)
        Q_EMIT failed(tr("%1 could not be written.").arg(userPath));
    refresh();
    return ok;
}

bool StartupBackend::remove(const QString &id)
{
    if (!validId(id))
        return false;
    const QString userPath = userDirectory() + QLatin1Char('/') + id;
    const bool ok = QFile::remove(userPath);
    if (!ok)
        Q_EMIT failed(tr("%1 could not be removed.").arg(userPath));
    refresh();
    return ok;
}

bool StartupBackend::addApplication(const QString &desktopFilePath)
{
    const QString id = QFileInfo(desktopFilePath).fileName();
    if (!validId(id))
        return false;
    const QString userPath = userDirectory() + QLatin1Char('/') + id;
    if (QFileInfo::exists(userPath))
        return setEnabled(id, true);
    for (const QVariant &entry : std::as_const(m_entries)) {
        if (entry.toMap().value(QStringLiteral("id")).toString() == id)
            return setEnabled(id, true);        // already a system entry
    }
    DesktopFile file;
    if (!file.load(desktopFilePath) || file.value(QStringLiteral("Exec")).isEmpty()) {
        Q_EMIT failed(tr("%1 is not a usable application entry.").arg(desktopFilePath));
        return false;
    }
    // The copy is started by the session itself, not through D-Bus activation,
    // and must not be filtered by menu-only keys.
    file.unset(QStringLiteral("NoDisplay"));
    file.unset(QStringLiteral("Hidden"));
    file.set(QStringLiteral("X-GNOME-Autostart-enabled"), QStringLiteral("true"));
    const bool ok = file.save(userPath);
    if (!ok)
        Q_EMIT failed(tr("%1 could not be written.").arg(userPath));
    refresh();
    return ok;
}

QString StartupBackend::commandProblem(const QString &command) const
{
    const QString trimmed = command.trimmed();
    if (trimmed.isEmpty())
        return tr("Enter a command.");
    if (trimmed.contains(QLatin1Char('\n')))
        return tr("The command must be a single line.");
    const QString program = execProgram(trimmed);
    if (!programExists(program))
        return tr("“%1” was not found. Give the full path or a program that is in the search path.").arg(program);
    return QString();
}

bool StartupBackend::addCommand(const QString &name, const QString &command)
{
    const QString cleanName = name.simplified();
    const QString cleanCommand = command.trimmed();
    if (cleanName.isEmpty()) {
        Q_EMIT failed(tr("Enter a name."));
        return false;
    }
    const QString problem = commandProblem(cleanCommand);
    if (!problem.isEmpty()) {
        Q_EMIT failed(problem);
        return false;
    }
    // File name derived from the name; never overwrites an existing entry.
    QString slug = cleanName.toLower();
    slug.replace(QRegularExpression(QStringLiteral("[^a-z0-9]+")), QStringLiteral("-"));
    slug = slug.left(40);
    while (slug.startsWith(QLatin1Char('-')))
        slug.remove(0, 1);
    while (slug.endsWith(QLatin1Char('-')))
        slug.chop(1);
    if (slug.isEmpty())
        slug = QStringLiteral("command");
    const QString dir = userDirectory();
    QString id = QStringLiteral("nexus-startup-%1.desktop").arg(slug);
    for (int n = 2; QFileInfo::exists(dir + QLatin1Char('/') + id); ++n)
        id = QStringLiteral("nexus-startup-%1-%2.desktop").arg(slug).arg(n);
    for (const QVariant &entry : std::as_const(m_entries)) {
        if (entry.toMap().value(QStringLiteral("id")).toString() == id)
            id = QStringLiteral("nexus-startup-%1-user.desktop").arg(slug);
    }

    DesktopFile file;
    file.set(QStringLiteral("Type"), QStringLiteral("Application"));
    file.set(QStringLiteral("Name"), cleanName);
    file.set(QStringLiteral("Exec"), cleanCommand);
    file.set(QStringLiteral("Terminal"), QStringLiteral("false"));
    file.set(QStringLiteral("X-GNOME-Autostart-enabled"), QStringLiteral("true"));
    const bool ok = file.save(dir + QLatin1Char('/') + id);
    if (!ok)
        Q_EMIT failed(tr("%1 could not be written.").arg(dir + QLatin1Char('/') + id));
    refresh();
    return ok;
}

QJsonObject StartupBackend::toJson() const
{
    QJsonArray entries;
    for (const QVariant &entry : m_entries)
        entries << QJsonObject::fromVariantMap(entry.toMap());
    return {
        {QStringLiteral("userDirectory"), userDirectory()},
        {QStringLiteral("systemDirectories"), QJsonArray::fromStringList(systemDirectories())},
        {QStringLiteral("entries"), entries},
    };
}
