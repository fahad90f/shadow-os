// SPDX-License-Identifier: GPL-3.0-or-later
#include "arsenal.h"

#include <command.h>

#include <QClipboard>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QIcon>
#include <QLocale>
#include <QProcess>
#include <QRegularExpression>
#include <QSettings>
#include <QStandardPaths>
#include <QSysInfo>
#include <QUrl>

namespace Arsenal {

namespace {

// Bump when the text of the responsible-use statement changes materially;
// everyone is then asked again.
const int kTermsVersion = 1;
const QString kTerminal = QStringLiteral("shadow-terminal");

QString settingsPath()
{
    return QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation)
           + QStringLiteral("/shadow/arsenal.conf");
}

QString debianArchitecture()
{
    const Shadow::CommandResult result = Shadow::Command::runSync(
        QStringLiteral("dpkg"), {QStringLiteral("--print-architecture")}, 3000);
    const QString answer = result.outText().trimmed();
    if (result.ok() && !answer.isEmpty())
        return answer;
    const QString cpu = QSysInfo::currentCpuArchitecture();
    if (cpu == QLatin1String("x86_64"))
        return QStringLiteral("amd64");
    return cpu;     // "arm64" is the same word in Qt and Debian
}

QString shellQuote(const QString &argument)
{
    static const QRegularExpression safe(QStringLiteral("^[A-Za-z0-9_@%+=:,./-]+$"));
    if (safe.match(argument).hasMatch())
        return argument;
    QString quoted = argument;
    quoted.replace(QLatin1Char('\''), QStringLiteral("'\\''"));
    return QLatin1Char('\'') + quoted + QLatin1Char('\'');
}

} // namespace

Controller::Controller(const QString &profilesDir, QObject *parent)
    : QObject(parent), m_arch(debianArchitecture())
{
    m_manifests.load(profilesDir);

    connect(&m_db, &PackageDb::refreshed, this, [this] {
        requestEstimates();
        bump();
    });
    connect(&m_db, &PackageDb::estimated, this, [this] {
        bump();
        if (!estimatesPending())
            Q_EMIT estimatesFinished();
    });
    connect(&m_job, &PkJob::finished, this, [this](bool) { refresh(); });
    connect(&m_service, &PkService::changed, this, &Controller::bump);
}

void Controller::bump()
{
    ++m_revision;
    Q_EMIT revisionChanged();
}

void Controller::refresh()
{
    m_db.clearEstimates();
    m_db.refresh(m_manifests.allPackages() + m_manifests.allMetapackages());
    bump();
}

void Controller::requestEstimates()
{
    m_db.clearEstimates();
    if (!m_estimatesEnabled)
        return;
    bool any = false;
    for (const Profile &profile : m_manifests.profiles()) {
        m_db.estimate(profile.id, missingTools(profile.id));
        any = true;
    }
    if (!any)
        Q_EMIT estimatesFinished();
}

bool Controller::estimatesPending() const
{
    for (const Profile &profile : m_manifests.profiles()) {
        const QString state = m_db.estimateOf(profile.id).state;
        if (state == QLatin1String("pending") || state == QLatin1String("none"))
            return true;
    }
    return false;
}

QString Controller::pageFromArguments(const QStringList &arguments) const
{
    for (int i = 0; i < arguments.size(); ++i) {
        if (arguments.at(i) == QLatin1String("--page") && i + 1 < arguments.size())
            return arguments.at(i + 1);
        if (arguments.at(i).startsWith(QLatin1String("--page=")))
            return arguments.at(i).mid(7);
    }
    return QString();
}

// ---- acknowledgement ---------------------------------------------------------------

bool Controller::acknowledged() const
{
    const QSettings settings(settingsPath(), QSettings::IniFormat);
    return settings.value(QStringLiteral("ResponsibleUse/AcknowledgedVersion"), 0).toInt() >= kTermsVersion;
}

QString Controller::acknowledgedAt() const
{
    const QSettings settings(settingsPath(), QSettings::IniFormat);
    const QDateTime when = QDateTime::fromString(
        settings.value(QStringLiteral("ResponsibleUse/AcknowledgedAt")).toString(), Qt::ISODate);
    return when.isValid() ? QLocale().toString(when.toLocalTime(), QLocale::LongFormat) : QString();
}

void Controller::acknowledge()
{
    QDir().mkpath(QFileInfo(settingsPath()).absolutePath());
    QSettings settings(settingsPath(), QSettings::IniFormat);
    settings.setValue(QStringLiteral("ResponsibleUse/AcknowledgedVersion"), kTermsVersion);
    settings.setValue(QStringLiteral("ResponsibleUse/AcknowledgedAt"),
                      QDateTime::currentDateTimeUtc().toString(Qt::ISODate));
    settings.sync();
    Q_EMIT acknowledgedChanged();
}

// ---- lookups -------------------------------------------------------------------------

bool Controller::terminalAvailable() const
{
    return Shadow::Command::available(kTerminal);
}

QString Controller::resolveIcon(const QString &name) const
{
    if (name.isEmpty())
        return QString();
    if (QIcon::hasThemeIcon(name))
        return name;
    // Not in the current icon theme: look where the SHADOW icons are installed,
    // and — when the manifests are read from a source tree — in its assets.
    QStringList roots = QStandardPaths::standardLocations(QStandardPaths::GenericDataLocation);
    for (QString &root : roots)
        root += QStringLiteral("/icons/hicolor/scalable");
    roots << QDir::cleanPath(m_manifests.directory() + QStringLiteral("/../../assets/icons"));
    for (const QString &root : std::as_const(roots)) {
        for (const char *sub : {"categories", "apps"}) {
            const QString path = QStringLiteral("%1/%2/%3.svg").arg(root, QLatin1String(sub), name);
            if (QFile::exists(path))
                return path;
        }
    }
    return QString();
}

QString Controller::desktopFile(const QString &desktopId) const
{
    if (desktopId.isEmpty())
        return QString();
    return QStandardPaths::locate(QStandardPaths::ApplicationsLocation, desktopId + QStringLiteral(".desktop"));
}

QString Controller::executable(const QString &command) const
{
    if (command.isEmpty())
        return QString();
    QString path = QStandardPaths::findExecutable(command);
    if (path.isEmpty()) {
        // Administrative tools live in sbin, which is not on every user's PATH.
        path = QStandardPaths::findExecutable(command, {QStringLiteral("/usr/sbin"), QStringLiteral("/sbin"),
                                                         QStringLiteral("/usr/local/sbin"),
                                                         QStringLiteral("/usr/games")});
    }
    return path;
}

QString Controller::toolState(const Tool &tool, const PackageState &state) const
{
    if (!m_db.ready())
        return QStringLiteral("unknown");
    if (state.installed)
        return QStringLiteral("installed");
    if (!m_db.dpkgError().isEmpty())
        return QStringLiteral("unknown");
    if (!tool.availableOn(m_arch))
        return QStringLiteral("unsupported");
    if (state.available)
        return QStringLiteral("available");
    return m_db.aptError().isEmpty() ? QStringLiteral("unavailable") : QStringLiteral("unknown");
}

QString Controller::removeBlockedReason(const Tool &tool, const PackageState &state) const
{
    Q_UNUSED(tool)
    if (!state.installed)
        return QString();
    if (state.essential)
        return tr("This is an essential system package and cannot be removed.");
    if (state.priority == QLatin1String("required") || state.priority == QLatin1String("important")
        || state.priority == QLatin1String("standard"))
        return tr("This package is part of the Debian base system (priority “%1”); SHADOW Arsenal does "
                  "not remove it.").arg(state.priority);
    return QString();
}

QVariantMap Controller::toolMap(const Tool &tool) const
{
    const PackageState state = m_db.state(tool.package);
    const Category category = m_manifests.category(tool.category);
    const QString toolStateKey = toolState(tool, state);

    QVariantList profiles;
    for (const QString &id : m_manifests.profilesOf(tool.package)) {
        if (const Profile *profile = m_manifests.profile(id))
            profiles << QVariantMap{{QStringLiteral("id"), id}, {QStringLiteral("name"), profile->name}};
    }

    const bool hasGui = tool.interface != QLatin1String("cli");
    const bool hasCli = tool.interface != QLatin1String("gui");
    const QString desktop = state.installed && hasGui ? desktopFile(tool.desktopId) : QString();
    const QString binary = state.installed && hasCli ? executable(tool.command) : QString();

    return {
        {QStringLiteral("package"), tool.package},
        {QStringLiteral("name"), tool.name},
        {QStringLiteral("summary"), tool.summary},
        {QStringLiteral("category"), tool.category},
        {QStringLiteral("categoryName"), category.name},
        {QStringLiteral("categoryIcon"), category.icon},
        {QStringLiteral("interface"), tool.interface},
        {QStringLiteral("homepage"), tool.homepage},
        {QStringLiteral("command"), tool.command},
        {QStringLiteral("desktopId"), tool.desktopId},
        {QStringLiteral("notes"), tool.notes},
        {QStringLiteral("architectures"), tool.architectures},
        {QStringLiteral("supported"), tool.availableOn(m_arch)},
        {QStringLiteral("state"), toolStateKey},
        {QStringLiteral("installed"), state.installed},
        {QStringLiteral("installedVersion"), state.installedVersion},
        {QStringLiteral("candidateVersion"), state.candidateVersion},
        {QStringLiteral("updateAvailable"), state.installed && state.available
                                               && state.candidateVersion != state.installedVersion},
        {QStringLiteral("downloadBytes"), double(state.downloadBytes)},
        {QStringLiteral("installedBytes"), double(state.installed ? state.installedBytes
                                                                  : state.candidateInstalledBytes)},
        {QStringLiteral("description"), state.description},
        {QStringLiteral("profiles"), profiles},
        {QStringLiteral("canLaunch"), !desktop.isEmpty()},
        {QStringLiteral("desktopFile"), desktop},
        {QStringLiteral("canOpenTerminal"), !binary.isEmpty()},
        {QStringLiteral("executable"), binary},
        {QStringLiteral("removeBlocked"), removeBlockedReason(tool, state)},
    };
}

QStringList Controller::missingTools(const QString &profileId) const
{
    QStringList names;
    for (const Tool &tool : m_manifests.effectiveTools(profileId)) {
        const PackageState state = m_db.state(tool.package);
        if (tool.availableOn(m_arch) && state.available && !state.installed)
            names << tool.package;
    }
    return names;
}

QVariantMap Controller::profileMap(const Profile &profile) const
{
    const QList<Tool> tools = m_manifests.effectiveTools(profile.id);
    int supported = 0, installed = 0, unavailable = 0, unsupported = 0, gui = 0;
    QHash<QString, int> perCategory;
    for (const Tool &tool : tools) {
        if (!tool.availableOn(m_arch)) {
            ++unsupported;
            continue;
        }
        ++supported;
        ++perCategory[tool.category];
        if (tool.interface != QLatin1String("cli"))
            ++gui;
        const PackageState state = m_db.state(tool.package);
        if (state.installed)
            ++installed;
        else if (m_db.ready() && !state.available)
            ++unavailable;
    }
    // Glyph of the category most of the tools belong to: the fallback when the
    // profile icon is not in the icon theme.
    QString dominant;
    int best = 0;
    for (const Category &category : m_manifests.categories()) {
        if (perCategory.value(category.id) > best) {
            best = perCategory.value(category.id);
            dominant = category.icon;
        }
    }

    QStringList includeNames;
    for (const QString &id : profile.includes) {
        if (const Profile *other = m_manifests.profile(id))
            includeNames << other->name;
    }

    const PackageState meta = m_db.state(profile.metapackage);
    const SizeEstimate estimate = m_db.estimateOf(profile.id);
    return {
        {QStringLiteral("id"), profile.id},
        {QStringLiteral("name"), profile.name},
        {QStringLiteral("summary"), profile.summary},
        {QStringLiteral("description"), profile.description},
        {QStringLiteral("icon"), resolveIcon(profile.icon)},
        {QStringLiteral("glyph"), !profile.includes.isEmpty() ? QStringLiteral("layers")
                                  : dominant.isEmpty() ? QStringLiteral("shield") : dominant},
        {QStringLiteral("metapackage"), profile.metapackage},
        {QStringLiteral("metapackageInstalled"), meta.installed},
        {QStringLiteral("metapackageVersion"), meta.installedVersion},
        {QStringLiteral("metapackageAvailable"), meta.available},
        {QStringLiteral("includes"), profile.includes},
        {QStringLiteral("includeNames"), includeNames},
        {QStringLiteral("toolCount"), supported},
        {QStringLiteral("installedCount"), installed},
        {QStringLiteral("unavailableCount"), unavailable},
        {QStringLiteral("unsupportedCount"), unsupported},
        {QStringLiteral("guiCount"), gui},
        {QStringLiteral("externalCount"), int(m_manifests.effectiveExternal(profile.id).size())},
        {QStringLiteral("sizeState"), estimate.state},
        {QStringLiteral("sizeError"), estimate.error},
        {QStringLiteral("pendingPackages"), estimate.packages},
        {QStringLiteral("downloadBytes"), double(estimate.downloadBytes)},
        {QStringLiteral("installBytes"), double(estimate.installedBytes)},
    };
}

QVariantList Controller::profiles() const
{
    QVariantList list;
    for (const Profile &profile : m_manifests.profiles())
        list << profileMap(profile);
    return list;
}

QVariantMap Controller::profile(const QString &id) const
{
    const Profile *profile = m_manifests.profile(id);
    return profile ? profileMap(*profile) : QVariantMap();
}

QVariantList Controller::categories() const
{
    QVariantList list;
    for (const Category &category : m_manifests.categories()) {
        list << QVariantMap{{QStringLiteral("id"), category.id}, {QStringLiteral("name"), category.name},
                            {QStringLiteral("icon"), category.icon},
                            {QStringLiteral("description"), category.description}};
    }
    return list;
}

int Controller::toolTotal() const
{
    int count = 0;
    for (const QString &package : m_manifests.allPackages()) {
        const Tool *tool = m_manifests.tool(package);
        if (tool && tool->availableOn(m_arch))
            ++count;
    }
    return count;
}

int Controller::installedTotal() const
{
    int count = 0;
    for (const QString &package : m_manifests.allPackages()) {
        if (m_db.state(package).installed)
            ++count;
    }
    return count;
}

bool Controller::matches(const Tool &tool, const QVariantMap &map, const QStringList &terms,
                         const QString &stateFilter, const QString &interfaceFilter) const
{
    const bool installed = map.value(QStringLiteral("installed")).toBool();
    if (stateFilter == QLatin1String("installed") && !installed)
        return false;
    if (stateFilter == QLatin1String("missing") && installed)
        return false;
    if (interfaceFilter == QLatin1String("cli") && tool.interface == QLatin1String("gui"))
        return false;
    if (interfaceFilter == QLatin1String("gui") && tool.interface == QLatin1String("cli"))
        return false;
    if (terms.isEmpty())
        return true;
    const QString haystack = QStringList{tool.name, tool.package, tool.summary, tool.command,
                                         map.value(QStringLiteral("categoryName")).toString()}
                                 .join(QLatin1Char('\n'));
    for (const QString &term : terms) {
        if (!haystack.contains(term, Qt::CaseInsensitive))
            return false;
    }
    return true;
}

QVariantList Controller::toolGroups(const QString &profileId, const QString &query,
                                    const QString &stateFilter, const QString &interfaceFilter) const
{
    const QStringList terms = query.split(QLatin1Char(' '), Qt::SkipEmptyParts);
    QHash<QString, QVariantList> byCategory;
    QStringList order;
    for (const Tool &tool : m_manifests.effectiveTools(profileId)) {
        const QVariantMap map = toolMap(tool);
        if (!matches(tool, map, terms, stateFilter, interfaceFilter))
            continue;
        if (!byCategory.contains(tool.category))
            order << tool.category;
        byCategory[tool.category] << map;
    }
    // Categories in the order of categories.json; unknown ones last.
    QStringList sorted;
    for (const Category &category : m_manifests.categories()) {
        if (order.removeAll(category.id) > 0)
            sorted << category.id;
    }
    sorted << order;

    QVariantList groups;
    for (const QString &id : std::as_const(sorted)) {
        const Category category = m_manifests.category(id);
        groups << QVariantMap{{QStringLiteral("id"), id}, {QStringLiteral("name"), category.name},
                              {QStringLiteral("icon"), category.icon},
                              {QStringLiteral("description"), category.description},
                              {QStringLiteral("tools"), byCategory.value(id)}};
    }
    return groups;
}

QVariantList Controller::searchTools(const QString &query, const QString &stateFilter,
                                     const QString &interfaceFilter, const QString &category) const
{
    const QStringList terms = query.split(QLatin1Char(' '), Qt::SkipEmptyParts);
    QVariantList results;
    for (const QString &package : m_manifests.allPackages()) {
        const Tool *tool = m_manifests.tool(package);
        if (!tool || (!category.isEmpty() && tool->category != category))
            continue;
        const QVariantMap map = toolMap(*tool);
        if (matches(*tool, map, terms, stateFilter, interfaceFilter))
            results << map;
    }
    std::sort(results.begin(), results.end(), [](const QVariant &a, const QVariant &b) {
        return a.toMap().value(QStringLiteral("name")).toString().compare(
                   b.toMap().value(QStringLiteral("name")).toString(), Qt::CaseInsensitive) < 0;
    });
    return results;
}

QVariantMap Controller::tool(const QString &package) const
{
    const Tool *tool = m_manifests.tool(package);
    return tool ? toolMap(*tool) : QVariantMap();
}

QVariantList Controller::externalTools(const QString &profileId) const
{
    QList<External> entries;
    QHash<QString, QStringList> profilesOf;
    if (profileId.isEmpty()) {
        for (const Profile &profile : m_manifests.profiles()) {
            for (const External &entry : profile.external) {
                if (!profilesOf.contains(entry.name))
                    entries << entry;
                profilesOf[entry.name] << profile.name;
            }
        }
    } else {
        entries = m_manifests.effectiveExternal(profileId);
    }
    QVariantList list;
    for (const External &entry : std::as_const(entries)) {
        const Category category = m_manifests.category(entry.category);
        list << QVariantMap{
            {QStringLiteral("name"), entry.name},
            {QStringLiteral("summary"), entry.summary},
            {QStringLiteral("category"), entry.category},
            {QStringLiteral("categoryName"), category.name},
            {QStringLiteral("categoryIcon"), category.icon},
            {QStringLiteral("homepage"), entry.homepage},
            {QStringLiteral("license"), entry.license},
            {QStringLiteral("reason"), entry.reason},
            {QStringLiteral("why"), entry.why},
            {QStringLiteral("how"), entry.how},
            {QStringLiteral("howUrl"), entry.howUrl},
            {QStringLiteral("profileNames"), profilesOf.value(entry.name)},
        };
    }
    return list;
}

QString Controller::aptCommand(const QString &kind, const QString &target) const
{
    const Profile *profile = m_manifests.profile(target);
    if (kind == QLatin1String("install-profile") && profile)
        return QStringLiteral("sudo apt install %1").arg(profile->metapackage);
    if (kind == QLatin1String("install-profile-tools") && profile) {
        const QStringList names = missingTools(target);
        return names.isEmpty() ? QString() : QStringLiteral("sudo apt install %1").arg(names.join(QLatin1Char(' ')));
    }
    if (kind == QLatin1String("remove-profile") && profile)
        return QStringLiteral("sudo apt remove --autoremove %1").arg(profile->metapackage);
    if (!m_manifests.tool(target))
        return QString();
    if (kind == QLatin1String("install-tool"))
        return QStringLiteral("sudo apt install %1").arg(target);
    if (kind == QLatin1String("remove-tool"))
        return QStringLiteral("sudo apt remove %1").arg(target);
    return QString();
}

QString Controller::terminalCommand(const QString &package) const
{
    const Tool *tool = m_manifests.tool(package);
    if (!tool || tool->command.isEmpty())
        return QString();
    QStringList words{tool->command};
    for (const QString &argument : tool->helpArguments())
        words << shellQuote(argument);
    return words.join(QLatin1Char(' '));
}

// ---- actions --------------------------------------------------------------------------

bool Controller::installProfile(const QString &id)
{
    const Profile *profile = m_manifests.profile(id);
    if (!profile || !m_service.available())
        return false;
    return m_job.install({profile->metapackage}, tr("Install the %1 profile").arg(profile->name),
                         QStringLiteral("profile:") + id);
}

bool Controller::installProfileTools(const QString &id)
{
    const Profile *profile = m_manifests.profile(id);
    const QStringList names = missingTools(id);
    if (!profile || names.isEmpty() || !m_service.available())
        return false;
    return m_job.install(names, tr("Install the tools of %1").arg(profile->name), QStringLiteral("tools:") + id);
}

bool Controller::removeProfile(const QString &id)
{
    const Profile *profile = m_manifests.profile(id);
    if (!profile || !m_service.available() || !m_db.state(profile->metapackage).installed)
        return false;
    // The tools were installed automatically with the metapackage; autoremove
    // takes those nothing else needs and leaves what was installed by hand.
    return m_job.remove({profile->metapackage}, true, tr("Remove the %1 profile").arg(profile->name),
                        QStringLiteral("profile:") + id);
}

bool Controller::installTool(const QString &package)
{
    const Tool *tool = m_manifests.tool(package);
    if (!tool || !tool->availableOn(m_arch) || !m_service.available())
        return false;
    return m_job.install({package}, tr("Install %1").arg(tool->name), QStringLiteral("tool:") + package);
}

bool Controller::removeTool(const QString &package)
{
    const Tool *tool = m_manifests.tool(package);
    if (!tool || !m_service.available())
        return false;
    const PackageState state = m_db.state(package);
    if (!state.installed || !removeBlockedReason(*tool, state).isEmpty())
        return false;
    return m_job.remove({package}, false, tr("Remove %1").arg(tool->name), QStringLiteral("tool:") + package);
}

QStringList Controller::parseExec(const QString &exec)
{
    // Desktop Entry Specification, "The Exec key": arguments are separated by
    // spaces; an argument may be enclosed in double quotes, inside which
    // backslash escapes ", `, $ and \.
    QStringList arguments;
    QString current;
    bool inQuotes = false, have = false;
    for (int i = 0; i < exec.size(); ++i) {
        const QChar c = exec.at(i);
        if (inQuotes) {
            if (c == QLatin1Char('\\') && i + 1 < exec.size()) {
                current += exec.at(++i);
            } else if (c == QLatin1Char('"')) {
                inQuotes = false;
            } else {
                current += c;
            }
        } else if (c == QLatin1Char('"')) {
            inQuotes = true;
            have = true;
        } else if (c == QLatin1Char(' ')) {
            if (have)
                arguments << current;
            current.clear();
            have = false;
        } else {
            current += c;
            have = true;
        }
    }
    if (inQuotes)
        return {};
    if (have)
        arguments << current;

    // Field codes: nothing is being opened, so they expand to nothing.
    QStringList result;
    for (QString argument : std::as_const(arguments)) {
        if (argument.size() == 2 && argument.at(0) == QLatin1Char('%') && argument.at(1) != QLatin1Char('%'))
            continue;
        static const QRegularExpression code(QStringLiteral("%[fFuUdDnNickvm]"));
        argument.replace(QStringLiteral("%%"), QStringLiteral("\x01"));
        argument.remove(code);
        argument.replace(QLatin1Char('\x01'), QLatin1Char('%'));
        result << argument;
    }
    return result;
}

QString Controller::launch(const QString &package)
{
    const Tool *tool = m_manifests.tool(package);
    if (!tool || tool->desktopId.isEmpty())
        return tr("This tool has no graphical interface.");
    const QString path = desktopFile(tool->desktopId);
    if (path.isEmpty())
        return tr("The desktop entry %1.desktop was not found. Is the package %2 installed?")
            .arg(tool->desktopId, tool->package);

    QSettings entry(path, QSettings::IniFormat);
    entry.beginGroup(QStringLiteral("Desktop Entry"));
    // QSettings would split a value at commas; ask for the raw string list and join it again.
    const QString exec = entry.value(QStringLiteral("Exec")).toStringList().join(QLatin1Char(','));
    const bool inTerminal = entry.value(QStringLiteral("Terminal")).toString() == QLatin1String("true");
    QStringList arguments = parseExec(exec);
    if (arguments.isEmpty())
        return tr("The desktop entry %1 has no usable Exec line.").arg(path);
    if (inTerminal) {
        if (!terminalAvailable())
            return tr("%1 runs in a terminal, but SHADOW Terminal (package shadow-terminal) is not installed.")
                .arg(tool->name);
        arguments = QStringList{kTerminal, QStringLiteral("-e")} + arguments;
    }
    const QString program = arguments.takeFirst();
    if (!QProcess::startDetached(program, arguments))
        return tr("%1 could not be started (%2).").arg(tool->name, program);
    return QString();
}

QString Controller::openInTerminal(const QString &package)
{
    const Tool *tool = m_manifests.tool(package);
    if (!tool || tool->command.isEmpty())
        return tr("This tool has no command-line interface.");
    if (!terminalAvailable())
        return tr("SHADOW Terminal (package shadow-terminal) is not installed.");
    const QString binary = executable(tool->command);
    if (binary.isEmpty())
        return tr("The command “%1” was not found. Is the package %2 installed?").arg(tool->command, tool->package);
    // shadow-terminal: everything after -e is the command's argument vector;
    // --hold keeps the tab open after the usage text has been printed.
    const QStringList arguments = QStringList{QStringLiteral("--hold"), QStringLiteral("-T"), tool->name,
                                              QStringLiteral("-e"), binary} + tool->helpArguments();
    if (!QProcess::startDetached(kTerminal, arguments))
        return tr("SHADOW Terminal could not be started.");
    return QString();
}

void Controller::copyText(const QString &text)
{
    if (QClipboard *clipboard = QGuiApplication::clipboard())
        clipboard->setText(text);
}

bool Controller::openUrl(const QString &url)
{
    const QUrl parsed(url, QUrl::StrictMode);
    if (!parsed.isValid() || (parsed.scheme() != QLatin1String("https") && parsed.scheme() != QLatin1String("http")))
        return false;
    return QDesktopServices::openUrl(parsed);
}

QString Controller::documentPath(const QString &name) const
{
    static const QRegularExpression plain(QStringLiteral("^[a-z-]+\\.md$"));
    if (!plain.match(name).hasMatch())
        return QString();
    const QStringList candidates = {
        QStringLiteral("/usr/share/doc/shadow-arsenal/") + name,
        QDir::cleanPath(m_manifests.directory() + QStringLiteral("/../docs/") + name),   // source tree
    };
    for (const QString &path : candidates) {
        if (QFile::exists(path))
            return path;
    }
    return QString();
}

bool Controller::openDocument(const QString &name)
{
    const QString path = documentPath(name);
    return !path.isEmpty() && QDesktopServices::openUrl(QUrl::fromLocalFile(path));
}

// ---- diagnostics -------------------------------------------------------------------------

QJsonArray Controller::dumpProfiles() const
{
    QJsonArray array;
    for (const Profile &profile : m_manifests.profiles()) {
        QJsonObject object = QJsonObject::fromVariantMap(profileMap(profile));
        QJsonArray tools;
        for (const Tool &tool : m_manifests.effectiveTools(profile.id))
            tools << tool.package;
        object.insert(QStringLiteral("tools"), tools);
        object.insert(QStringLiteral("external"), QJsonArray::fromVariantList(externalTools(profile.id)));
        object.insert(QStringLiteral("aptInstall"), aptCommand(QStringLiteral("install-profile"), profile.id));
        array << object;
    }
    return array;
}

QJsonArray Controller::dumpTools() const
{
    return QJsonArray::fromVariantList(searchTools(QString(), QStringLiteral("all"), QStringLiteral("all"),
                                                   QString()));
}

QJsonObject Controller::dumpState() const
{
    QJsonObject installed;
    for (const QString &package : m_manifests.allPackages() + m_manifests.allMetapackages()) {
        const PackageState state = m_db.state(package);
        if (state.installed)
            installed.insert(package, state.installedVersion);
    }
    return {
        {QStringLiteral("profilesDir"), profilesDir()},
        {QStringLiteral("architecture"), m_arch},
        {QStringLiteral("loadError"), loadError()},
        {QStringLiteral("loadWarnings"), QJsonArray::fromStringList(loadWarnings())},
        {QStringLiteral("profileCount"), int(m_manifests.profiles().size())},
        {QStringLiteral("toolCount"), toolTotal()},
        {QStringLiteral("installedCount"), installedTotal()},
        {QStringLiteral("installed"), installed},
        {QStringLiteral("dpkgError"), dpkgError()},
        {QStringLiteral("aptError"), aptError()},
        {QStringLiteral("packageKit"), QJsonObject{{QStringLiteral("available"), m_service.available()},
                                                   {QStringLiteral("error"), m_service.error()},
                                                   {QStringLiteral("backend"), m_service.backend()}}},
        {QStringLiteral("terminalAvailable"), terminalAvailable()},
        {QStringLiteral("acknowledged"), acknowledged()},
        {QStringLiteral("acknowledgedAt"), acknowledgedAt()},
        {QStringLiteral("settingsFile"), settingsPath()},
    };
}

} // namespace Arsenal
