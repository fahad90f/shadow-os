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
    if (backend.scanning()) {
        QEventLoop loop;
        QObject::connect(&backend, &PrivacyBackend::cleanupChanged, &loop, [&] { if (!backend.scanning()) loop.quit(); });
        QTimer::singleShot(120000, &loop, &QEventLoop::quit);
        loop.exec();
    }
    return QJsonValue(backend.toJson());
})

namespace {
const QString kScreenSaverService = QStringLiteral("org.kde.screensaver");

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

QStringList nmConfigFiles()
{
    const QString vendor = QStringLiteral("/usr/lib/NetworkManager/conf.d");
    const QString runtime = QStringLiteral("/run/NetworkManager/conf.d");
    const QString admin = QStringLiteral("/etc/NetworkManager/conf.d");
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
    files << QStringLiteral("/etc/NetworkManager/NetworkManager.conf");
    for (const QString &name : adminNames)
        files << admin + QLatin1Char('/') + name;
    return files;
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
    return QFile::exists(QStringLiteral("/usr/sbin/NetworkManager"));
}

void PrivacyBackend::loadNetwork()
{
    // (section, key) pairs shown on the page, in display order.
    const QList<QPair<QString, QString>> wanted = {
        {QStringLiteral("device"), QStringLiteral("wifi.scan-rand-mac-address")},
        {QStringLiteral("connection"), QStringLiteral("wifi.cloned-mac-address")},
        {QStringLiteral("connection"), QStringLiteral("ethernet.cloned-mac-address")},
        {QStringLiteral("connection"), QStringLiteral("ipv6.ip6-privacy")},
    };
    QHash<QString, NmValue> values;
    m_shadowPrivacyFile.clear();
    for (const QString &path : nmConfigFiles()) {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly))
            continue;
        if (path.endsWith(QLatin1String("/80-shadow-privacy.conf")))
            m_shadowPrivacyFile = path;
        QString section;
        const QStringList lines = QString::fromUtf8(file.readAll()).split(QLatin1Char('\n'));
        for (const QString &raw : lines) {
            const QString line = raw.trimmed();
            if (line.isEmpty() || line.startsWith(QLatin1Char('#')) || line.startsWith(QLatin1Char(';')))
                continue;
            if (line.startsWith(QLatin1Char('[')) && line.endsWith(QLatin1Char(']'))) {
                section = line.mid(1, line.size() - 2).trimmed();
                continue;
            }
            const int eq = line.indexOf(QLatin1Char('='));
            if (eq <= 0)
                continue;
            const QString key = line.left(eq).trimmed();
            if (wanted.contains({section, key}))
                values.insert(key, {line.mid(eq + 1).trimmed(), path, true});
        }
    }
    m_macSettings.clear();
    for (const auto &[section, key] : wanted) {
        const NmValue value = values.value(key);
        QVariantMap entry{{QStringLiteral("key"), key}, {QStringLiteral("section"), section},
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
    QThread *thread = QThread::create([this, alive] {
        bool ghostMode = false, transparentProxy = false, dnsProtected = false;
        bool btKilled = false, usbKilled = false, kernelHardened = false;
        bool torRunning = false, autoRotate = false;
        QString threatModel = QStringLiteral("basic");

        QFile stateFile(QStringLiteral("/run/shadow-anon-state.json"));
        if (stateFile.open(QIODevice::ReadOnly)) {
            const QJsonObject s = QJsonDocument::fromJson(stateFile.readAll()).object();
            ghostMode       = s.value(QStringLiteral("ghost_mode")).toBool();
            threatModel     = s.value(QStringLiteral("threat_model")).toString(QStringLiteral("basic"));
            transparentProxy= s.value(QStringLiteral("transparent_proxy")).toBool();
            dnsProtected    = s.value(QStringLiteral("dns_protected")).toBool();
            btKilled        = s.value(QStringLiteral("bt_killed")).toBool();
            usbKilled       = s.value(QStringLiteral("usb_killed")).toBool();
            kernelHardened  = s.value(QStringLiteral("kernel_hardened")).toBool();
        }
        // Cross-check on-disk evidence (survives reboots that clear /run)
        kernelHardened  = kernelHardened || QFile::exists(QStringLiteral("/etc/sysctl.d/90-shadow-anon.conf"));
        usbKilled       = usbKilled     || QFile::exists(QStringLiteral("/etc/udev/rules.d/90-shadow-nousb.rules"));
        dnsProtected    = dnsProtected  || QFile::exists(QStringLiteral("/etc/NetworkManager/conf.d/90-shadow-dns.conf"));

        // Tor service
        { QProcess p; p.start(QStringLiteral("systemctl"), {QStringLiteral("is-active"), QStringLiteral("--quiet"), QStringLiteral("tor")});
          p.waitForFinished(2000); torRunning = (p.exitCode() == 0); }
        // Auto-rotate timer
        { QProcess p; p.start(QStringLiteral("systemctl"), {QStringLiteral("is-active"), QStringLiteral("--quiet"), QStringLiteral("shadow-anon-rotate.timer")});
          p.waitForFinished(2000); autoRotate = (p.exitCode() == 0); }

        QMetaObject::invokeMethod(qApp, [=, this, alive] {
            if (!*alive) return;
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

void PrivacyBackend::runShadowAnon(const QStringList &args)
{
    if (m_anonBusy)
        return;
    m_anonOutput.clear();
    m_anonBusy = true;
    Q_EMIT anonChanged();

    auto *proc = new QProcess(this);
    connect(proc, &QProcess::readyReadStandardOutput, this, [this, proc] {
        m_anonOutput += QString::fromUtf8(proc->readAllStandardOutput());
        Q_EMIT anonChanged();
    });
    connect(proc, &QProcess::readyReadStandardError, this, [this, proc] {
        m_anonOutput += QString::fromUtf8(proc->readAllStandardError());
        Q_EMIT anonChanged();
    });
    connect(proc, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, [this, proc](int code, QProcess::ExitStatus) {
        m_anonBusy = false;
        proc->deleteLater();
        refreshAnon();
        if (code == 0)
            Q_EMIT succeeded(tr("Identity update complete."));
        else if (code != 1) // code 1 = pkexec cancelled by user
            Q_EMIT failed(tr("shadow-anon exited with code %1.").arg(code));
        Q_EMIT anonChanged();
    });

    QStringList fullArgs = {QStringLiteral("/usr/bin/shadow-anon")};
    fullArgs += args;
    proc->start(QStringLiteral("pkexec"), fullArgs);
}

void PrivacyBackend::rotateIdentity()          { runShadowAnon({QStringLiteral("rotate")}); }
void PrivacyBackend::enableGhostMode()         { runShadowAnon({QStringLiteral("ghost-on")}); }
void PrivacyBackend::disableGhostMode()        { runShadowAnon({QStringLiteral("ghost-off")}); }
void PrivacyBackend::nukeIdentity()            { runShadowAnon({QStringLiteral("nuke")}); }
void PrivacyBackend::runLeakTest()             { runShadowAnon({QStringLiteral("leak-test")}); }
void PrivacyBackend::wipeLogsNow()             { runShadowAnon({QStringLiteral("wipe-logs")}); }
void PrivacyBackend::hardenKernelNow()         { runShadowAnon({QStringLiteral("harden-kernel")}); }
void PrivacyBackend::stopAutoRotate()          { runShadowAnon({QStringLiteral("stop")}); }

void PrivacyBackend::setThreatModel(const QString &model)
{
    runShadowAnon({QStringLiteral("threat"), model});
}

void PrivacyBackend::killBluetooth(bool kill)
{
    runShadowAnon({kill ? QStringLiteral("kill-bluetooth") : QStringLiteral("restore-bluetooth")});
}

void PrivacyBackend::killUsbStorage(bool kill)
{
    runShadowAnon({kill ? QStringLiteral("kill-usb") : QStringLiteral("restore-usb")});
}

void PrivacyBackend::enableTransparentProxy(bool enable)
{
    runShadowAnon({enable ? QStringLiteral("transparent-proxy-on") : QStringLiteral("transparent-proxy-off")});
}

void PrivacyBackend::enableDnsProtect(bool enable)
{
    runShadowAnon({enable ? QStringLiteral("dns-protect-on") : QStringLiteral("dns-protect-off")});
}

void PrivacyBackend::startAutoRotate(int intervalSeconds)
{
    runShadowAnon({QStringLiteral("start"),
                   QStringLiteral("--interval"), QString::number(qMax(5, intervalSeconds))});
}

// ---- OS Integrity (shadow-harden) ------------------------------------------

void PrivacyBackend::refreshHarden()
{
    const std::shared_ptr<bool> alive = m_alive;
    QThread *thread = QThread::create([this, alive] {
        bool aideInit = false, appArmor = false, auditd = false, bootH = false;
        QString aideLastCheck;
        int aideChanges = 0;

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

        QMetaObject::invokeMethod(qApp, [=, this, alive] {
            if (!*alive) return;
            m_aideInitialized   = aideInit;
            m_aideLastCheck     = aideLastCheck;
            m_aideChanges       = aideChanges;
            m_appArmorEnforcing = appArmor;
            m_auditdRunning     = auditd;
            m_bootHardened      = bootH;
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
    m_hardenOutput.clear();
    m_hardenBusy = true;
    Q_EMIT hardenChanged();

    auto *proc = new QProcess(this);
    connect(proc, &QProcess::readyReadStandardOutput, this, [this, proc] {
        m_hardenOutput += QString::fromUtf8(proc->readAllStandardOutput());
        Q_EMIT hardenChanged();
    });
    connect(proc, &QProcess::readyReadStandardError, this, [this, proc] {
        m_hardenOutput += QString::fromUtf8(proc->readAllStandardError());
        Q_EMIT hardenChanged();
    });
    connect(proc, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, [this, proc](int code, QProcess::ExitStatus) {
        m_hardenBusy = false;
        proc->deleteLater();
        refreshHarden();
        if (code == 0)
            Q_EMIT succeeded(tr("Hardening applied."));
        else if (code != 1)
            Q_EMIT failed(tr("shadow-harden exited with code %1.").arg(code));
        Q_EMIT hardenChanged();
    });

    QStringList fullArgs = {QStringLiteral("/usr/bin/shadow-harden")};
    fullArgs += args;
    proc->start(QStringLiteral("pkexec"), fullArgs);
}

void PrivacyBackend::aideInit()           { runShadowHarden({QStringLiteral("aide-init")}); }
void PrivacyBackend::aideCheck()          { runShadowHarden({QStringLiteral("aide-check")}); }
void PrivacyBackend::enforceAppArmor()    { runShadowHarden({QStringLiteral("enforce-apparmor")}); }
void PrivacyBackend::applyBootHardening() { runShadowHarden({QStringLiteral("boot-harden")}); }
void PrivacyBackend::hardenFilesystems()  { runShadowHarden({QStringLiteral("fs-harden")}); }

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
            {QStringLiteral("settings"), QJsonArray::fromVariantList(m_macSettings)}}},
    };
}
