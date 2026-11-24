// SPDX-License-Identifier: GPL-3.0-or-later
#include "privacybackend.h"

#include "dumpregistry.h"

#include <QCoreApplication>
#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusMessage>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocale>
#include <QProcess>
#include <QSettings>
#include <QStandardPaths>
#include <QThread>
#include <QTimer>

#ifdef SHADOW_HAVE_KCONFIG
#include <KConfig>
#include <KConfigGroup>
#endif

#include <ctime>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

SHADOW_CC_DUMP("privacy", [] {
    PrivacyBackend backend;
    backend.scan();
    if (backend.scanning() || !backend.settled()) {
        QEventLoop loop;
        const auto check = [&] { if (!backend.scanning() && backend.settled()) loop.quit(); };
        QObject::connect(&backend, &PrivacyBackend::cleanupChanged, &loop, check);
        QObject::connect(&backend, &PrivacyBackend::anonChanged, &loop, check);
        QTimer::singleShot(120000, &loop, &QEventLoop::quit);
        loop.exec();
    }
    return QJsonValue(backend.toJson());
})

namespace {
const QString kScreenSaverService = QStringLiteral("org.kde.screensaver");

// shadow-anon's TIMER_MIN_INTERVAL (Tor's spacing between NEWNYM signals) and
// the default of shadow-anon-rotate.timer.
constexpr int kAutoRotateMinimum = 60;
constexpr int kAutoRotateDefault = 300;

QString envPath(const char *name, const QString &fallback)
{
    const QString value = qEnvironmentVariable(name);
    return value.isEmpty() ? fallback : value;
}
QString anonTool() { return envPath("SHADOW_ANON_TOOL", QStringLiteral("/usr/bin/shadow-anon")); }
QString hardenTool() { return envPath("SHADOW_HARDEN_TOOL", QStringLiteral("/usr/bin/shadow-harden")); }
QString anonStateFile() { return envPath("SHADOW_ANON_STATE", QStringLiteral("/run/shadow-anon-state.json")); }
QString rotateTimerDropIn()
{
    // Written by "shadow-anon start --interval N".
    return envPath("SHADOW_ANON_TIMER_DROPIN", QStringLiteral("/etc/systemd/system/shadow-anon-rotate.timer.d/interval.conf"));
}

// The interval of the rotation timer: its drop-in's last OnUnitActiveSec=
// (plain seconds, as shadow-anon writes it), else the unit's default; never
// below the minimum, which shadow-anon tor-newnym enforces anyway.
int rotateInterval()
{
    QFile file(rotateTimerDropIn());
    int seconds = kAutoRotateDefault;
    if (file.open(QIODevice::ReadOnly)) {
        const QStringList lines = QString::fromUtf8(file.read(64 * 1024)).split(QLatin1Char('\n'));
        for (const QString &raw : lines) {
            const QString line = raw.trimmed();
            if (!line.startsWith(QLatin1String("OnUnitActiveSec=")))
                continue;
            QString value = line.mid(16).trimmed();
            if (value.endsWith(QLatin1Char('s')))
                value.chop(1);
            bool ok = false;
            const int n = value.toInt(&ok);
            if (ok && n > 0)
                seconds = n;
        }
    }
    return qMax(kAutoRotateMinimum, seconds);
}

// The last line a tool wrote on stderr that says something, without the
// tool's "name: error: " or "name: " prefix.
QString lastErrorLine(const QByteArray &err, const QString &toolName)
{
    const QStringList lines = QString::fromUtf8(err).split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    for (auto it = lines.crbegin(); it != lines.crend(); ++it) {
        QString line = it->trimmed();
        if (line.isEmpty())
            continue;
        for (const QString &prefix : {toolName + QStringLiteral(": error: "), toolName + QStringLiteral(": ")}) {
            if (line.startsWith(prefix)) {
                line = line.mid(prefix.size());
                break;
            }
        }
        if (line.startsWith(QLatin1String("- ")))       // shadow-harden lists its problems
            line = line.mid(2);
        return line.left(600);
    }
    return {};
}

// What the user is told when a privileged tool run did not succeed.
QString failureMessage(int exitCode, bool crashed, bool viaPkexec, const QByteArray &err, const QString &toolPath)
{
    const QString name = QFileInfo(toolPath).fileName();
    if (exitCode == -1)
        return viaPkexec ? PrivacyBackend::tr("pkexec could not be started. Is polkit installed?")
                         : PrivacyBackend::tr("%1 could not be started.").arg(toolPath);
    if (crashed)
        return PrivacyBackend::tr("%1 stopped unexpectedly. Nothing more is known; see the log below.").arg(name);
    if (viaPkexec && exitCode == 126)
        return PrivacyBackend::tr("Nothing was changed: the authentication was cancelled or refused.");
    if (viaPkexec && exitCode == 127)
        return PrivacyBackend::tr("Nothing was changed: not authorised. polkit refused the request, "
                                  "or no authentication agent is running in this session.");
    const QString reason = lastErrorLine(err, name);
    if (reason.isEmpty())
        return PrivacyBackend::tr("%1 failed (exit status %2); see the log below.").arg(name).arg(exitCode);
    return reason;
}

QString dataDir() { return QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation); }
QString trashDir() { return dataDir() + QStringLiteral("/Trash"); }
QString thumbnailDir() { return QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation) + QStringLiteral("/thumbnails"); }
QString recentDocumentsDir() { return dataDir() + QStringLiteral("/RecentDocuments"); }

QSettings launcherUsage()
{
    // The store the launcher of horizon-shell counts application starts in.
    return QSettings(QSettings::NativeFormat, QSettings::UserScope, QStringLiteral("shadow"), QStringLiteral("horizon-usage"));
}

// ---- disk walking (worker thread; plain POSIX so odd file names are no problem) ----

struct Tally
{
    qint64 bytes = 0;
    int files = 0;
    int failures = 0;
};

struct WalkRules
{
    bool remove = false;
    // Temporary directories: only the caller's own regular files and symlinks
    // that have not been modified since 'cutoff'.
    bool ownOldFilesOnly = false;
    uid_t uid = 0;
    time_t cutoff = 0;
};

void walk(const QByteArray &directory, const WalkRules &rules, Tally &tally, int depth = 0)
{
    if (depth > 64)
        return;
    DIR *handle = opendir(directory.constData());
    if (!handle)
        return;
    while (const dirent *entry = readdir(handle)) {
        const QByteArray name(entry->d_name);
        if (name == "." || name == "..")
            continue;
        const QByteArray path = directory + '/' + name;
        struct stat st {};
        if (lstat(path.constData(), &st) != 0)
            continue;
        if (S_ISDIR(st.st_mode)) {
            if (rules.ownOldFilesOnly && st.st_uid != rules.uid)
                continue;
            walk(path, rules, tally, depth + 1);
            if (rules.remove && (!rules.ownOldFilesOnly || st.st_mtime < rules.cutoff))
                rmdir(path.constData());   // succeeds only when nothing is left inside
            continue;
        }
        if (rules.ownOldFilesOnly
            && !((S_ISREG(st.st_mode) || S_ISLNK(st.st_mode)) && st.st_uid == rules.uid && st.st_mtime < rules.cutoff))
            continue;
        if (rules.remove) {
            if (unlink(path.constData()) != 0) {
                ++tally.failures;
                continue;
            }
        }
        tally.bytes += st.st_size;
        ++tally.files;
    }
    closedir(handle);
}

struct Category
{
    QString id;
    QStringList locations;
    bool temporary = false;
};

QList<Category> categories()
{
    return {
        {QStringLiteral("trash"), {trashDir()}, false},
        {QStringLiteral("thumbnails"), {thumbnailDir()}, false},
        {QStringLiteral("temporary"), {QStringLiteral("/tmp"), QStringLiteral("/var/tmp")}, true},
    };
}

Tally process(const Category &category, bool remove)
{
    Tally tally;
    WalkRules rules;
    rules.remove = remove;
    rules.ownOldFilesOnly = category.temporary;
    rules.uid = getuid();
    rules.cutoff = time(nullptr) - 24 * 3600;
    for (const QString &location : category.locations)
        walk(QFile::encodeName(location), rules, tally);
    if (remove && category.id == QLatin1String("trash")) {
        // The trash specification expects these two directories to exist.
        QDir().mkpath(trashDir() + QStringLiteral("/files"));
        QDir().mkpath(trashDir() + QStringLiteral("/info"));
    }
    return tally;
}

// ---- NetworkManager configuration ---------------------------------------------------

struct NmValue
{
    QString value, source;
    bool set = false;
};

// Test hook (development only): a directory that stands in for "/" when the
// NetworkManager configuration files are read.
QString nmConfigRoot() { return qEnvironmentVariable("SHADOW_NM_CONFIG_ROOT"); }
QString nmRuntimeDir() { return nmConfigRoot() + QStringLiteral("/run/NetworkManager/conf.d"); }

// Written by "shadow-netprivacy boot" (shadow-netprivacy.service) when the
// kernel command line has shadow.netprivacy=off: for that boot a
// [connection-shadow-escape] section (no match-device, so every device) keeps
// every device's own MAC address.
const QString kNmEscapeName = QStringLiteral("93-shadow-escape.conf");

QStringList nmConfigFiles()
{
    const QString root = nmConfigRoot();
    const QString vendor = root + QStringLiteral("/usr/lib/NetworkManager/conf.d");
    const QString runtime = nmRuntimeDir();
    const QString admin = root + QStringLiteral("/etc/NetworkManager/conf.d");
    const auto list = [](const QString &dir) { return QDir(dir).entryList({QStringLiteral("*.conf")}, QDir::Files, QDir::Name); };
    const QStringList adminNames = list(admin), runtimeNames = list(runtime);
    QStringList files;
    // A file of the same name in /etc or /run replaces the one shipped in /usr/lib.
    for (const QString &name : list(vendor)) {
        if (!adminNames.contains(name) && !runtimeNames.contains(name))
            files << vendor + QLatin1Char('/') + name;
    }
    for (const QString &name : runtimeNames) {
        if (!adminNames.contains(name))
            files << runtime + QLatin1Char('/') + name;
    }
    files << root + QStringLiteral("/etc/NetworkManager/NetworkManager.conf");
    for (const QString &name : adminNames)
        files << admin + QLatin1Char('/') + name;
    return files;
}

// One [section] of NetworkManager's merged configuration.
struct NmSection
{
    QString name;
    QHash<QString, NmValue> keys;
};

// The family of a section: [connection*] and [device*] sections give defaults
// for connections and devices; the literal [connection] / [device] is one of them.
QString nmFamily(const QString &section)
{
    for (const QString &family : {QStringLiteral("connection"), QStringLiteral("device")}) {
        if (section.startsWith(family))
            return family;
    }
    return {};
}

// NetworkManager's merged configuration (nm-config.c read_config()): the files
// in the order they are read; a section that occurs again in a later file gets
// that file's keys (the later value wins) but keeps its place. Within one file
// the [connection*] and [device*] sections are added bottom to top, so that
// after the final reversal (below) the file's own order is kept.
QList<NmSection> nmSections(const QStringList &files)
{
    QList<NmSection> merged;
    QHash<QString, qsizetype> index;
    for (const QString &path : files) {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly))
            continue;
        QList<NmSection> own;           // in order of first appearance (a repeated section is merged)
        QHash<QString, qsizetype> ownIndex;
        qsizetype current = -1;
        const QStringList lines = QString::fromUtf8(file.read(1024 * 1024)).split(QLatin1Char('\n'));
        for (const QString &raw : lines) {
            const QString line = raw.trimmed();
            if (line.isEmpty() || line.startsWith(QLatin1Char('#')) || line.startsWith(QLatin1Char(';')))
                continue;
            if (line.startsWith(QLatin1Char('[')) && line.endsWith(QLatin1Char(']'))) {
                const QString name = line.mid(1, line.size() - 2).trimmed();
                if (!ownIndex.contains(name)) {
                    ownIndex.insert(name, own.size());
                    own.append({name, {}});
                }
                current = ownIndex.value(name);
                continue;
            }
            const int eq = line.indexOf(QLatin1Char('='));
            if (eq <= 0 || current < 0)
                continue;
            own[current].keys.insert(line.left(eq).trimmed(), {line.mid(eq + 1).trimmed(), path, true});
        }
        QList<NmSection> ordered;
        for (const NmSection &section : own) {
            if (nmFamily(section.name).isEmpty())
                ordered.append(section);
        }
        for (const QString &family : {QStringLiteral("device"), QStringLiteral("connection")}) {
            for (auto it = own.crbegin(); it != own.crend(); ++it) {
                if (nmFamily(it->name) == family)
                    ordered.append(*it);
            }
        }
        for (const NmSection &section : ordered) {
            const auto found = index.constFind(section.name);
            if (found == index.cend()) {
                index.insert(section.name, merged.size());
                merged.append(section);
                continue;
            }
            for (auto key = section.keys.cbegin(); key != section.keys.cend(); ++key)
                merged[*found].keys.insert(key.key(), key.value());
        }
    }
    return merged;
}

bool nmBoolean(const QString &value)
{
    const QString v = value.toLower();
    return v == QLatin1String("yes") || v == QLatin1String("true") || v == QLatin1String("on") || v == QLatin1String("1");
}

// The default NetworkManager applies to every device for KEY of FAMILY
// ("connection" or "device"), as nm-config-data.c looks it up
// (_match_section_infos_construct, _match_section_infos_lookup): the named
// sections ([connection-…]) from the highest priority (the last added) down,
// the literal [connection] always last; the first section that applies and
// has the key gives the value. A named section without match-device applies
// to every device (e.g. shadow.netprivacy=off's [connection-shadow-escape]).
// One with match-device applies to some devices only and is skipped here.
// One with stop-match=yes and no match-device ends the search: the key keeps
// NetworkManager's built-in default. SECTION is set to the section the value
// comes from (empty when the key is not set).
NmValue nmLookup(const QList<NmSection> &sections, const QString &family, const QString &key, QString *section)
{
    QList<const NmSection *> order;
    const NmSection *literal = nullptr;
    for (auto it = sections.crbegin(); it != sections.crend(); ++it) {
        if (it->name == family)
            literal = &*it;
        else if (nmFamily(it->name) == family)
            order.append(&*it);
    }
    if (literal)
        order.append(literal);
    for (const NmSection *candidate : order) {
        const bool has = candidate->keys.contains(key);
        const bool stop = nmBoolean(candidate->keys.value(QStringLiteral("stop-match")).value);
        if (!has && !stop)
            continue;
        if (candidate->keys.contains(QStringLiteral("match-device")))
            continue;
        if (!has)
            break;
        *section = candidate->name;
        return candidate->keys.value(key);
    }
    section->clear();
    return {};
}
} // namespace

PrivacyBackend::PrivacyBackend(QObject *parent)
    : QObject(parent)
    , m_alive(std::make_shared<bool>(true))
{
    for (const Category &category : categories())
        m_cleanup.append(QVariantMap{{QStringLiteral("id"), category.id}, {QStringLiteral("bytes"), -1},
                                     {QStringLiteral("files"), -1}, {QStringLiteral("locations"), category.locations}});
    refresh();
}

PrivacyBackend::~PrivacyBackend()
{
    *m_alive = false;
}

void PrivacyBackend::refresh()
{
    loadLock();
    loadHistory();
    loadNetwork();
    refreshAnon();
    refreshHarden();
}

// ---- screen lock ------------------------------------------------------------------------

bool PrivacyBackend::lockConfigurable() const
{
#ifdef SHADOW_HAVE_KCONFIG
    return true;
#else
    return false;
#endif
}

void PrivacyBackend::loadLock()
{
#ifdef SHADOW_HAVE_KCONFIG
    const KConfig config(QStringLiteral("kscreenlockerrc"), KConfig::NoGlobals);
    const KConfigGroup daemon = config.group(QStringLiteral("Daemon"));
    // Fallbacks are KScreenLocker's own defaults.
    m_autolock = daemon.readEntry("Autolock", true);
    m_timeout = daemon.readEntry("Timeout", 5);
    m_lockOnResume = daemon.readEntry("LockOnResume", true);
    m_grace = daemon.readEntry("LockGrace", 5);
#endif
    const QDBusConnection bus = QDBusConnection::sessionBus();
    m_lockerRunning = bus.isConnected() && bus.interface() && bus.interface()->isServiceRegistered(kScreenSaverService);
    Q_EMIT lockChanged();
}

void PrivacyBackend::writeLock(const char *key, const QVariant &value)
{
#ifdef SHADOW_HAVE_KCONFIG
    {
        KConfig config(QStringLiteral("kscreenlockerrc"), KConfig::NoGlobals);
        KConfigGroup daemon = config.group(QStringLiteral("Daemon"));
        daemon.writeEntry(key, value);
        if (!config.sync())
            Q_EMIT failed(tr("The screen lock settings could not be saved to %1.")
                              .arg(QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) + QStringLiteral("/kscreenlockerrc")));
    }
    // The locker (inside KWin) re-reads its configuration on request.
    QDBusConnection::sessionBus().asyncCall(QDBusMessage::createMethodCall(
        kScreenSaverService, QStringLiteral("/ScreenSaver"), kScreenSaverService, QStringLiteral("configure")));
#else
    Q_UNUSED(key)
    Q_UNUSED(value)
    Q_EMIT failed(tr("This build of the Control Center has no KConfig support."));
#endif
    loadLock();
}

void PrivacyBackend::setAutolock(bool on) { writeLock("Autolock", on); }
void PrivacyBackend::setLockTimeoutMinutes(int minutes) { writeLock("Timeout", qBound(1, minutes, 24 * 60)); }
void PrivacyBackend::setLockOnResume(bool on) { writeLock("LockOnResume", on); }
void PrivacyBackend::setLockGraceSeconds(int seconds) { writeLock("LockGrace", qBound(0, seconds, 300)); }

// ---- history ----------------------------------------------------------------------------

QString PrivacyBackend::recentFilesPath() const
{
    return dataDir() + QStringLiteral("/recently-used.xbel");
}

QString PrivacyBackend::launcherUsagePath() const
{
    return launcherUsage().fileName();
}

void PrivacyBackend::loadHistory()
{
    int count = 0;
    QFile xbel(recentFilesPath());
    m_recentPresent = xbel.exists();
    if (xbel.open(QIODevice::ReadOnly)) {
        const QByteArray data = xbel.read(32 * 1024 * 1024);
        for (qsizetype at = data.indexOf("<bookmark "); at >= 0; at = data.indexOf("<bookmark ", at + 1))
            ++count;
    }
    const int kdeEntries = int(QDir(recentDocumentsDir()).entryList({QStringLiteral("*.desktop")}, QDir::Files).size());
    m_recentCount = count + kdeEntries;
    m_recentPresent = m_recentPresent || kdeEntries > 0;

    QSettings usage = launcherUsage();
    usage.sync();
    usage.beginGroup(QStringLiteral("Launches"));
    m_usageCount = int(usage.childKeys().size());
    Q_EMIT historyChanged();
}

void PrivacyBackend::clearRecentFiles()
{
    bool ok = true;
    if (QFile::exists(recentFilesPath()))
        ok = QFile::remove(recentFilesPath());
    QDir kde(recentDocumentsDir());
    for (const QString &name : kde.entryList({QStringLiteral("*.desktop")}, QDir::Files))
        ok = kde.remove(name) && ok;
    loadHistory();
    if (ok)
        Q_EMIT succeeded(tr("The list of recently used files was cleared."));
    else
        Q_EMIT failed(tr("The list of recently used files could not be removed completely."));
}

void PrivacyBackend::clearLauncherUsage()
{
    QSettings usage = launcherUsage();
    usage.remove(QStringLiteral("Launches"));
    usage.sync();
    const bool ok = usage.status() == QSettings::NoError;
    loadHistory();
    if (ok)
        Q_EMIT succeeded(tr("The launcher's usage history was cleared."));
    else
        Q_EMIT failed(tr("%1 could not be written.").arg(launcherUsagePath()));
}

// ---- clean-up ---------------------------------------------------------------------------

void PrivacyBackend::scan()
{
    if (m_scanning || !m_cleaning.isEmpty())
        return;
    startWorker(QString());
}

void PrivacyBackend::clean(const QString &id)
{
    if (m_scanning || !m_cleaning.isEmpty())
        return;
    for (const Category &category : categories()) {
        if (category.id == id) {
            startWorker(id);
            return;
        }
    }
}

void PrivacyBackend::startWorker(const QString &cleanId)
{
    m_scanning = true;
    m_cleaning = cleanId;
    Q_EMIT cleanupChanged();

    const std::shared_ptr<bool> alive = m_alive;
    QThread *thread = QThread::create([this, alive, cleanId] {
        Tally removed;
        QVariantList results;
        for (const Category &category : categories()) {
            if (category.id == cleanId)
                removed = process(category, true);
            const Tally tally = process(category, false);
            results.append(QVariantMap{{QStringLiteral("id"), category.id}, {QStringLiteral("bytes"), tally.bytes},
                                       {QStringLiteral("files"), tally.files}, {QStringLiteral("locations"), category.locations}});
        }
        QMetaObject::invokeMethod(qApp, [this, alive, cleanId, removed, results] {
            if (!*alive)
                return;
            m_cleanup = results;
            m_scanning = false;
            m_cleaning.clear();
            Q_EMIT cleanupChanged();
            if (cleanId.isEmpty())
                return;
            if (removed.failures > 0)
                Q_EMIT failed(tr("%1 file(s) could not be removed; %2 were removed.").arg(removed.failures).arg(removed.files));
            else
                Q_EMIT succeeded(tr("Removed %1 file(s), %2 freed.").arg(removed.files).arg(QLocale().formattedDataSize(removed.bytes)));
        }, Qt::QueuedConnection);
    });
    QObject::connect(thread, &QThread::finished, thread, &QObject::deleteLater);
    thread->start();
}

// ---- NetworkManager ---------------------------------------------------------------------

bool PrivacyBackend::networkManagerInstalled() const
{
    return QFile::exists(nmConfigRoot() + QStringLiteral("/usr/sbin/NetworkManager"));
}

void PrivacyBackend::loadNetwork()
{
    // (section family, key) pairs shown on the page, in display order.
    const QList<QPair<QString, QString>> wanted = {
        {QStringLiteral("device"), QStringLiteral("wifi.scan-rand-mac-address")},
        {QStringLiteral("connection"), QStringLiteral("wifi.cloned-mac-address")},
        // ${CONNECTION}/${BOOT} (the Shadow default): a new private address per boot.
        {QStringLiteral("connection"), QStringLiteral("connection.stable-id")},
        {QStringLiteral("connection"), QStringLiteral("ethernet.cloned-mac-address")},
        {QStringLiteral("connection"), QStringLiteral("ipv6.ip6-privacy")},
    };
    const QStringList files = nmConfigFiles();
    m_shadowPrivacyFile.clear();
    m_netPrivacyEscapeFile.clear();
    const QString escapePath = nmRuntimeDir() + QLatin1Char('/') + kNmEscapeName;
    for (const QString &path : files) {
        if (!QFile::exists(path))
            continue;
        if (path.endsWith(QLatin1String("/80-shadow-privacy.conf")))
            m_shadowPrivacyFile = path;
        else if (path == escapePath)
            m_netPrivacyEscapeFile = path;
    }
    const QList<NmSection> sections = nmSections(files);
    m_macSettings.clear();
    for (const auto &[section, key] : wanted) {
        QString group;
        const NmValue value = nmLookup(sections, section, key, &group);
        QVariantMap entry{{QStringLiteral("key"), key}, {QStringLiteral("section"), section},
                          {QStringLiteral("group"), group},
                          {QStringLiteral("set"), value.set}, {QStringLiteral("value"), value.value},
                          {QStringLiteral("source"), value.source}};
        if (key == QLatin1String("ipv6.ip6-privacy") && !value.set) {
            // NetworkManager then follows the kernel default.
            QFile sysctl(QStringLiteral("/proc/sys/net/ipv6/conf/default/use_tempaddr"));
            if (sysctl.open(QIODevice::ReadOnly))
                entry.insert(QStringLiteral("kernelDefault"), QString::fromLatin1(sysctl.readAll()).trimmed());
        }
        m_macSettings.append(entry);
    }
    Q_EMIT networkChanged();
}

// ---- Hide Me (shadow-anon) --------------------------------------------------

void PrivacyBackend::refreshAnon()
{
    const std::shared_ptr<bool> alive = m_alive;
    ++m_anonReads;
    QThread *thread = QThread::create([this, alive] {
        bool ghostMode = false, transparentProxy = false, dnsProtected = false;
        bool btKilled = false, usbKilled = false, kernelHardened = false;
        bool torRunning = false, autoRotate = false;
        QString threatModel = QStringLiteral("basic");
        QString error;

        // shadow-anon rewrites this at every start (shadow-anon-boot.service)
        // from what is really in effect, and after every change; it never
        // records a protection that is not in force. Whether the kill switch
        // is in force is read by KillSwitchBackend from shadow-killswitch's
        // own status file instead.
        QFile stateFile(anonStateFile());
        const bool haveState = stateFile.open(QIODevice::ReadOnly);
        if (haveState) {
            const QJsonObject s = QJsonDocument::fromJson(stateFile.read(256 * 1024)).object();
            ghostMode       = s.value(QStringLiteral("ghost_mode")).toBool();
            threatModel     = s.value(QStringLiteral("threat_model")).toString(QStringLiteral("basic"));
            transparentProxy= s.value(QStringLiteral("transparent_proxy")).toBool();
            dnsProtected    = s.value(QStringLiteral("dns_protected")).toBool();
            btKilled        = s.value(QStringLiteral("bt_killed")).toBool();
            usbKilled       = s.value(QStringLiteral("usb_killed")).toBool();
            kernelHardened  = s.value(QStringLiteral("kernel_hardened")).toBool();
            const QJsonValue e = s.value(QStringLiteral("error"));
            if (e.isString())
                error = e.toString().trimmed().left(1000);
        }
        if (threatModel != QLatin1String("basic") && threatModel != QLatin1String("enhanced")
            && threatModel != QLatin1String("maximum"))
            threatModel = QStringLiteral("basic");
        const int interval = rotateInterval();
        // Cross-check on-disk evidence (survives reboots that clear /run)
        kernelHardened  = kernelHardened || QFile::exists(QStringLiteral("/etc/sysctl.d/90-shadow-anon.conf"));
        usbKilled       = usbKilled     || QFile::exists(QStringLiteral("/etc/udev/rules.d/90-shadow-nousb.rules"));
        // Without a state file (an older shadow-anon after a restart) the
        // DNS lock file is the only evidence; with one, the lock file alone
        // does not mean DNS works through Tor.
        if (!haveState)
            dnsProtected = QFile::exists(QStringLiteral("/etc/NetworkManager/conf.d/90-shadow-dns.conf"));

        // Tor itself, checked as shadow-anon's tor_running() does: Debian's
        // tor.service is a oneshot running /bin/true that stays active for
        // the whole boot whatever Tor does, so only tor@default (the daemon)
        // says whether Tor runs. No answer within 2 s is not "running".
        { QProcess p; p.start(QStringLiteral("systemctl"), {QStringLiteral("is-active"), QStringLiteral("--quiet"), QStringLiteral("tor@default")});
          torRunning = p.waitForFinished(2000) && p.exitStatus() == QProcess::NormalExit && p.exitCode() == 0; }
        // Auto-rotate timer
        { QProcess p; p.start(QStringLiteral("systemctl"), {QStringLiteral("is-active"), QStringLiteral("--quiet"), QStringLiteral("shadow-anon-rotate.timer")});
          p.waitForFinished(2000); autoRotate = (p.exitCode() == 0); }

        QMetaObject::invokeMethod(qApp, [=, this] {
            if (!*alive) return;
            --m_anonReads;
            m_anonError        = error;
            m_autoRotateInterval = interval;
            m_ghostMode        = ghostMode;
            m_threatModel      = threatModel;
            m_torRunning       = torRunning;
            m_transparentProxy = transparentProxy;
            m_dnsProtected     = dnsProtected;
            m_btKilled         = btKilled;
            m_usbKilled        = usbKilled;
            m_kernelHardened   = kernelHardened;
            m_autoRotateActive = autoRotate;
            Q_EMIT anonChanged();
        }, Qt::QueuedConnection);
    });
    connect(thread, &QThread::finished, thread, &QObject::deleteLater);
    thread->start();
}

bool PrivacyBackend::anonToolInstalled() const
{
    return QFileInfo(anonTool()).isExecutable();
}

int PrivacyBackend::autoRotateMinimumSeconds()
{
    return kAutoRotateMinimum;
}

bool PrivacyBackend::settled() const
{
    return !m_anonBusy && !m_hardenBusy && m_anonReads == 0 && m_hardenReads == 0;
}

// Root runs the tool itself; everybody else goes through polkit (pkexec).
// Output is shown as it comes (the leak test takes a while); stderr is also
// kept apart, for the message when the command fails.
void PrivacyBackend::runShadowAnon(const QString &action, const QStringList &args, const QString &successMessage)
{
    if (m_anonBusy)
        return;
    const QString tool = anonTool();
    if (!anonToolInstalled()) {
        Q_EMIT failed(tr("Hide Me's tool %1 is not installed (package shadow-hardening).").arg(tool));
        return;
    }
    m_anonOutput.clear();
    m_anonBusy = true;
    m_anonAction = action;
    Q_EMIT anonChanged();

    const bool viaPkexec = geteuid() != 0;
    auto *proc = new QProcess(this);
    auto err = std::make_shared<QByteArray>();
    connect(proc, &QProcess::readyReadStandardOutput, this, [this, proc] {
        m_anonOutput += QString::fromUtf8(proc->readAllStandardOutput());
        Q_EMIT anonChanged();
    });
    connect(proc, &QProcess::readyReadStandardError, this, [this, proc, err] {
        const QByteArray chunk = proc->readAllStandardError();
        err->append(chunk);
        m_anonOutput += QString::fromUtf8(chunk);
        Q_EMIT anonChanged();
    });
    const auto finish = [this, proc, err, tool, viaPkexec, successMessage](int code, bool crashed) {
        m_anonBusy = false;
        m_anonAction.clear();
        proc->deleteLater();
        refreshAnon();
        if (code == 0 && !crashed)
            Q_EMIT succeeded(successMessage);
        else
            Q_EMIT failed(failureMessage(code, crashed, viaPkexec, *err, tool));
        Q_EMIT anonChanged();
    };
    connect(proc, &QProcess::finished, this, [finish](int code, QProcess::ExitStatus status) {
        finish(code, status == QProcess::CrashExit);
    });
    connect(proc, &QProcess::errorOccurred, this, [finish](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart)
            finish(-1, false);
    });
    if (viaPkexec)
        proc->start(QStringLiteral("pkexec"), QStringList{tool} + args);
    else
        proc->start(tool, args);
}

void PrivacyBackend::rotateIdentity()
{
    runShadowAnon(QStringLiteral("rotate"), {QStringLiteral("rotate")},
                  tr("Identity rotated. The log below shows what changed (the network address changes at most once in 5 minutes)."));
}
void PrivacyBackend::enableGhostMode()
{
    runShadowAnon(QStringLiteral("ghost-on"), {QStringLiteral("ghost-on")},
                  tr("Ghost Mode is on: only Tor can reach the network until you turn it off or restart."));
}
void PrivacyBackend::disableGhostMode()
{
    runShadowAnon(QStringLiteral("ghost-off"), {QStringLiteral("ghost-off")}, tr("Ghost Mode is off."));
}
void PrivacyBackend::nukeIdentity()
{
    runShadowAnon(QStringLiteral("nuke"), {QStringLiteral("nuke")}, tr("NUKE finished: Ghost Mode is on and the logs are wiped."));
}
void PrivacyBackend::runLeakTest()
{
    runShadowAnon(QStringLiteral("leak-test"), {QStringLiteral("leak-test")}, tr("Leak test finished; the results are below."));
}
void PrivacyBackend::wipeLogsNow()
{
    runShadowAnon(QStringLiteral("wipe-logs"), {QStringLiteral("wipe-logs")}, tr("Logs wiped. The log below lists each step."));
}
void PrivacyBackend::hardenKernelNow()
{
    runShadowAnon(QStringLiteral("harden-kernel"), {QStringLiteral("harden-kernel")},
                  tr("Kernel hardening written. The log below shows any setting the kernel did not take."));
}
void PrivacyBackend::stopAutoRotate()
{
    runShadowAnon(QStringLiteral("stop"), {QStringLiteral("stop")}, tr("Tor circuits are no longer renewed on a timer."));
}

void PrivacyBackend::setThreatModel(const QString &model)
{
    if (model != QLatin1String("basic") && model != QLatin1String("enhanced") && model != QLatin1String("maximum")) {
        Q_EMIT failed(tr("Unknown threat level \"%1\".").arg(model.left(40)));
        return;
    }
    const QString name = model == QLatin1String("maximum") ? tr("Maximum") : model == QLatin1String("enhanced") ? tr("Enhanced") : tr("Basic");
    runShadowAnon(QStringLiteral("threat"), {QStringLiteral("threat"), model}, tr("Threat level: %1.").arg(name));
}

void PrivacyBackend::killBluetooth(bool kill)
{
    runShadowAnon(QStringLiteral("bluetooth"), {kill ? QStringLiteral("kill-bluetooth") : QStringLiteral("restore-bluetooth")},
                  kill ? tr("Bluetooth block requested; the row shows whether rfkill blocked it.")
                       : tr("Bluetooth unblock requested; the row shows whether rfkill unblocked it."));
}

void PrivacyBackend::killUsbStorage(bool kill)
{
    runShadowAnon(QStringLiteral("usb"), {kill ? QStringLiteral("kill-usb") : QStringLiteral("restore-usb")},
                  kill ? tr("USB storage is blocked for devices plugged in from now on.") : tr("USB storage is allowed again."));
}

void PrivacyBackend::enableTransparentProxy(bool enable)
{
    runShadowAnon(QStringLiteral("proxy"), {enable ? QStringLiteral("transparent-proxy-on") : QStringLiteral("transparent-proxy-off")},
                  enable ? tr("The transparent Tor proxy is on, behind the kill switch.") : tr("The transparent Tor proxy is off."));
}

void PrivacyBackend::enableDnsProtect(bool enable)
{
    runShadowAnon(QStringLiteral("dns"), {enable ? QStringLiteral("dns-protect-on") : QStringLiteral("dns-protect-off")},
                  enable ? tr("DNS-over-Tor is on, also after a restart.") : tr("DNS goes to the system resolver again."));
}

void PrivacyBackend::startAutoRotate(int intervalSeconds)
{
    const int seconds = qMax(kAutoRotateMinimum, intervalSeconds);
    runShadowAnon(QStringLiteral("start"), {QStringLiteral("start"), QStringLiteral("--interval"), QString::number(seconds)},
                  tr("New Tor circuits every %n second(s).", nullptr, seconds));
}

void PrivacyBackend::enableKillSwitch(bool enable)
{
    if (enable)
        runShadowAnon(QStringLiteral("kill-switch"), {QStringLiteral("kill-switch-on")},
                      tr("Kill switch on (Tor only) until you turn it off here or restart."));
    else
        runShadowAnon(QStringLiteral("kill-switch"), {QStringLiteral("kill-switch-off")},
                      tr("Hide Me's kill switch is off; the mode in force before it is back."));
}

// ---- OS Integrity (shadow-harden) ------------------------------------------

void PrivacyBackend::refreshHarden()
{
    const std::shared_ptr<bool> alive = m_alive;
    ++m_hardenReads;
    QThread *thread = QThread::create([this, alive] {
        bool aideInit = false, appArmor = false, auditd = false, bootH = false;
        QString aideLastCheck;
        int aideChanges = 0;
        QString level = QStringLiteral("hardened");

        QFile stateFile(QStringLiteral("/run/shadow-harden-state.json"));
        if (stateFile.open(QIODevice::ReadOnly)) {
            const QJsonObject s = QJsonDocument::fromJson(stateFile.readAll()).object();
            aideInit      = s.value(QStringLiteral("aide_initialized")).toBool();
            aideLastCheck = s.value(QStringLiteral("aide_last_check")).toString();
            aideChanges   = s.value(QStringLiteral("aide_changes")).toInt();
            appArmor      = s.value(QStringLiteral("apparmor_enforcing")).toBool();
            bootH         = s.value(QStringLiteral("boot_hardened")).toBool();
        }
        // Cross-check on-disk evidence (survives reboots that clear /run)
        aideInit = aideInit || QFile::exists(QStringLiteral("/var/lib/aide/aide.db"));
        bootH    = bootH    || QFile::exists(QStringLiteral("/etc/default/grub.d/90-shadow-hardening.cfg"));

        // Hardening level: the marker file is the source of truth (survives reboot).
        QFile levelFile(QStringLiteral("/etc/shadow-os/hardening-level"));
        if (levelFile.open(QIODevice::ReadOnly)) {
            const QString name = QString::fromUtf8(levelFile.readAll()).trimmed();
            if (name == QLatin1String("standard") || name == QLatin1String("hardened")
                || name == QLatin1String("paranoid"))
                level = name;
        }

        // AppArmor status
        if (!appArmor) {
            QProcess p;
            p.start(QStringLiteral("aa-status"), {QStringLiteral("--enabled")});
            p.waitForFinished(2000);
            appArmor = (p.exitCode() == 0);
        }
        // Auditd
        { QProcess p; p.start(QStringLiteral("systemctl"), {QStringLiteral("is-active"), QStringLiteral("--quiet"), QStringLiteral("auditd")});
          p.waitForFinished(2000); auditd = (p.exitCode() == 0); }

        QMetaObject::invokeMethod(qApp, [=, this] {
            if (!*alive) return;
            --m_hardenReads;
            m_aideInitialized   = aideInit;
            m_aideLastCheck     = aideLastCheck;
            m_aideChanges       = aideChanges;
            m_appArmorEnforcing = appArmor;
            m_auditdRunning     = auditd;
            m_bootHardened      = bootH;
            m_hardeningLevel    = level;
            Q_EMIT hardenChanged();
        }, Qt::QueuedConnection);
    });
    connect(thread, &QThread::finished, thread, &QObject::deleteLater);
    thread->start();
}

void PrivacyBackend::runShadowHarden(const QStringList &args)
{
    if (m_hardenBusy)
        return;
    const QString tool = hardenTool();
    if (!QFileInfo(tool).isExecutable()) {
        Q_EMIT failed(tr("%1 is not installed (package shadow-hardening).").arg(tool));
        return;
    }
    m_hardenOutput.clear();
    m_hardenBusy = true;
    Q_EMIT hardenChanged();

    const bool viaPkexec = geteuid() != 0;
    auto *proc = new QProcess(this);
    auto err = std::make_shared<QByteArray>();
    connect(proc, &QProcess::readyReadStandardOutput, this, [this, proc] {
        m_hardenOutput += QString::fromUtf8(proc->readAllStandardOutput());
        Q_EMIT hardenChanged();
    });
    connect(proc, &QProcess::readyReadStandardError, this, [this, proc, err] {
        const QByteArray chunk = proc->readAllStandardError();
        err->append(chunk);
        m_hardenOutput += QString::fromUtf8(chunk);
        Q_EMIT hardenChanged();
    });
    // shadow-harden exits 1 (aide-check: AIDE's own status above 1) with the
    // reason on stderr; that is a failure to report, not a cancelled dialog.
    const auto finish = [this, proc, err, tool, viaPkexec](int code, bool crashed) {
        m_hardenBusy = false;
        proc->deleteLater();
        refreshHarden();
        if (code == 0 && !crashed)
            Q_EMIT succeeded(tr("Hardening applied."));
        else
            Q_EMIT failed(failureMessage(code, crashed, viaPkexec, *err, tool));
        Q_EMIT hardenChanged();
    };
    connect(proc, &QProcess::finished, this, [finish](int code, QProcess::ExitStatus status) {
        finish(code, status == QProcess::CrashExit);
    });
    connect(proc, &QProcess::errorOccurred, this, [finish](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart)
            finish(-1, false);
    });
    if (viaPkexec)
        proc->start(QStringLiteral("pkexec"), QStringList{tool} + args);
    else
        proc->start(tool, args);
}

void PrivacyBackend::aideInit()           { runShadowHarden({QStringLiteral("aide-init")}); }
void PrivacyBackend::aideCheck()          { runShadowHarden({QStringLiteral("aide-check")}); }
void PrivacyBackend::enforceAppArmor()    { runShadowHarden({QStringLiteral("enforce-apparmor")}); }
void PrivacyBackend::applyBootHardening() { runShadowHarden({QStringLiteral("boot-harden")}); }
void PrivacyBackend::hardenFilesystems()  { runShadowHarden({QStringLiteral("fs-harden")}); }

void PrivacyBackend::setHardeningLevel(const QString &level)
{
    if (level != QLatin1String("standard") && level != QLatin1String("hardened")
        && level != QLatin1String("paranoid"))
        return;
    if (level == m_hardeningLevel)
        return;
    runShadowHarden({QStringLiteral("level"), level});
}

QJsonObject PrivacyBackend::toJson() const
{
    return {
        {QStringLiteral("screenLock"), QJsonObject{
            {QStringLiteral("configurable"), lockConfigurable()},
            {QStringLiteral("lockerRunning"), m_lockerRunning},
            {QStringLiteral("autolock"), m_autolock},
            {QStringLiteral("timeoutMinutes"), m_timeout},
            {QStringLiteral("lockOnResume"), m_lockOnResume},
            {QStringLiteral("lockGraceSeconds"), m_grace}}},
        {QStringLiteral("history"), QJsonObject{
            {QStringLiteral("recentFilesPath"), recentFilesPath()},
            {QStringLiteral("recentFilesPresent"), m_recentPresent},
            {QStringLiteral("recentFilesCount"), m_recentCount},
            {QStringLiteral("launcherUsagePath"), launcherUsagePath()},
            {QStringLiteral("launcherUsageCount"), m_usageCount}}},
        {QStringLiteral("cleanup"), QJsonArray::fromVariantList(m_cleanup)},
        {QStringLiteral("network"), QJsonObject{
            {QStringLiteral("networkManagerInstalled"), networkManagerInstalled()},
            {QStringLiteral("shadowPrivacyFile"), m_shadowPrivacyFile},
            {QStringLiteral("netPrivacyEscapeFile"), m_netPrivacyEscapeFile},
            {QStringLiteral("settings"), QJsonArray::fromVariantList(m_macSettings)}}},
        {QStringLiteral("hideMe"), QJsonObject{
            {QStringLiteral("toolPath"), anonTool()},
            {QStringLiteral("toolInstalled"), anonToolInstalled()},
            {QStringLiteral("busy"), m_anonBusy},
            {QStringLiteral("action"), m_anonAction},
            {QStringLiteral("ghostMode"), m_ghostMode},
            {QStringLiteral("threatModel"), m_threatModel},
            {QStringLiteral("transparentProxy"), m_transparentProxy},
            {QStringLiteral("dnsProtected"), m_dnsProtected},
            {QStringLiteral("bluetoothKilled"), m_btKilled},
            {QStringLiteral("usbStorageKilled"), m_usbKilled},
            {QStringLiteral("kernelHardened"), m_kernelHardened},
            {QStringLiteral("torRunning"), m_torRunning},
            {QStringLiteral("autoRotateActive"), m_autoRotateActive},
            {QStringLiteral("autoRotateIntervalSeconds"), m_autoRotateInterval},
            {QStringLiteral("autoRotateMinimumSeconds"), kAutoRotateMinimum},
            {QStringLiteral("error"), m_anonError},
            {QStringLiteral("output"), m_anonOutput}}},
    };
}
