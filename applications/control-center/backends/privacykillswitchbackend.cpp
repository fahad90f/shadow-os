// SPDX-License-Identifier: GPL-3.0-or-later
#include "privacykillswitchbackend.h"

#include "dumpregistry.h"

#include <command.h>

#include <QCryptographicHash>
#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusMessage>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDateTime>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QKeySequence>
#include <QSet>

#include <cmath>
#include <unistd.h>

SHADOW_CC_DUMP("killswitch", [] {
    KillSwitchBackend backend;
    if (!backend.settled()) {
        QEventLoop loop;
        const auto check = [&] { if (backend.settled()) loop.quit(); };
        QObject::connect(&backend, &KillSwitchBackend::liveCheckChanged, &loop, check);
        QObject::connect(&backend, &KillSwitchBackend::shortcutChanged, &loop, check);
        QTimer::singleShot(30000, &loop, &QEventLoop::quit);
        loop.exec();
    }
    return QJsonValue(backend.toJson());
})

namespace {

// The real state files are about 1 KiB; anything larger is not read.
constexpr qint64 kMaxFileSize = 256 * 1024;
constexpr int kToolTimeoutMs = 300000;      // includes the time the password dialog is open
constexpr int kCheckTimeoutMs = 60000;

const QStringList &knownModes()
{
    static const QStringList modes{QStringLiteral("off"), QStringLiteral("tor"), QStringLiteral("vpn"),
                                   QStringLiteral("tor-or-vpn")};
    return modes;
}

QString envPath(const char *name, const QString &fallback)
{
    const QString value = qEnvironmentVariable(name);
    return value.isEmpty() ? fallback : value;
}

QString runDir() { return QDir::cleanPath(envPath("SHADOW_NETGUARD_RUN_DIR", QStringLiteral("/run/shadow"))); }
QString configDir() { return QDir::cleanPath(envPath("SHADOW_NETGUARD_CONFIG_DIR", QStringLiteral("/etc/shadow-os"))); }
QString anonStateFile() { return envPath("SHADOW_ANON_STATE", QStringLiteral("/run/shadow-anon-state.json")); }
QString statusTool() { return envPath("SHADOW_SECURITY_STATUS_TOOL", QStringLiteral("/usr/sbin/shadow-security-status")); }

// ---- reading documents ----------------------------------------------------------------

struct FileRead
{
    bool exists = false;
    QByteArray data;
    QString error;          // why an existing file could not be read
};

FileRead readFile(const QString &path)
{
    FileRead out;
    const QFileInfo info(path);
    if (!info.exists() && !info.isSymLink())
        return out;
    out.exists = true;
    if (!info.exists()) {
        out.error = KillSwitchBackend::tr("it is a dangling symbolic link");
        return out;
    }
    if (!info.isFile()) {
        out.error = KillSwitchBackend::tr("it is not a regular file");
        return out;
    }
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        out.error = file.errorString();
        return out;
    }
    out.data = file.read(kMaxFileSize + 1);
    if (out.data.size() > kMaxFileSize) {
        out.data.clear();
        out.error = KillSwitchBackend::tr("it is larger than %1 bytes").arg(kMaxFileSize);
    } else if (file.error() != QFileDevice::NoError) {
        out.data.clear();
        out.error = file.errorString();
    }
    return out;
}

void addProblem(QString *problem, const QString &text)
{
    if (problem->isEmpty())
        *problem = text;
}

QJsonObject objectOf(const FileRead &file, QString *problem)
{
    if (!file.error.isEmpty()) {
        addProblem(problem, KillSwitchBackend::tr("cannot be read: %1").arg(file.error));
        return {};
    }
    QJsonParseError pe;
    const QJsonDocument doc = QJsonDocument::fromJson(file.data, &pe);
    if (pe.error != QJsonParseError::NoError) {
        addProblem(problem, file.data.trimmed().isEmpty()
                                ? KillSwitchBackend::tr("is empty")
                                : KillSwitchBackend::tr("is not valid JSON (%1 at byte %2)").arg(pe.errorString()).arg(pe.offset));
        return {};
    }
    if (!doc.isObject()) {
        addProblem(problem, KillSwitchBackend::tr("is not a JSON object"));
        return {};
    }
    return doc.object();
}

// Missing or null: 'fallback'. Anything but true/false: 'fallback' and a problem.
bool boolField(const QJsonObject &obj, const char *key, bool fallback, QString *problem, bool *known = nullptr)
{
    const QJsonValue v = obj.value(QLatin1String(key));
    if (known)
        *known = v.isBool();
    if (v.isBool())
        return v.toBool();
    if (!v.isUndefined() && !v.isNull())
        addProblem(problem, KillSwitchBackend::tr("has a field \"%1\" that is not true or false").arg(QLatin1String(key)));
    return fallback;
}

QString stringField(const QJsonObject &obj, const char *key, QString *problem)
{
    const QJsonValue v = obj.value(QLatin1String(key));
    if (v.isString())
        return v.toString();
    if (!v.isUndefined() && !v.isNull())
        addProblem(problem, KillSwitchBackend::tr("has a field \"%1\" that is not text").arg(QLatin1String(key)));
    return {};
}

// A non-negative whole number; -1 when missing, null or invalid.
qint64 countField(const QJsonObject &obj, const char *key)
{
    const QJsonValue v = obj.value(QLatin1String(key));
    if (!v.isDouble())
        return -1;
    const double d = v.toDouble();
    if (!std::isfinite(d) || d < 0 || d > 9.0e15 || std::floor(d) != d)
        return -1;
    return qint64(d);
}

// The tool's "error": null or text. Anything else is still shown, never dropped.
QString errorField(const QJsonObject &obj)
{
    const QJsonValue v = obj.value(QLatin1String("error"));
    if (v.isUndefined() || v.isNull() || (v.isBool() && !v.toBool()))
        return {};
    if (v.isString())
        return v.toString().trimmed().isEmpty() ? KillSwitchBackend::tr("an unspecified error") : v.toString().trimmed().left(2000);
    const QByteArray text = QJsonDocument(QJsonArray{v}).toJson(QJsonDocument::Compact);
    return QString::fromUtf8(text.mid(1, text.size() - 2)).left(2000);
}

QStringList stringList(const QJsonObject &obj, const char *key, int max = 20)
{
    QStringList out;
    const QJsonValue v = obj.value(QLatin1String(key));
    if (!v.isArray())
        return out;
    for (const QJsonValue &item : v.toArray()) {
        if (item.isString() && out.size() < max)
            out << item.toString().left(400);
    }
    return out;
}

QString modeName(const QString &mode)
{
    if (mode == QLatin1String("off"))
        return KillSwitchBackend::tr("Off");
    if (mode == QLatin1String("tor"))
        return KillSwitchBackend::tr("Tor only");
    if (mode == QLatin1String("vpn"))
        return KillSwitchBackend::tr("VPN only");
    if (mode == QLatin1String("tor-or-vpn"))
        return KillSwitchBackend::tr("Tor or VPN");
    return mode;
}

// ---- kill switch ------------------------------------------------------------------------

// /etc/shadow-os/killswitch.json: the mode after a restart.
QVariantMap parseKillSwitchConfig(const FileRead &file)
{
    QVariantMap c{{QStringLiteral("present"), file.exists}};
    if (!file.exists) {
        // shadow-killswitch: a missing file means "off".
        c.insert(QStringLiteral("mode"), QStringLiteral("off"));
        c.insert(QStringLiteral("lan"), false);
        c.insert(QStringLiteral("endpoints"), 0);
        c.insert(QStringLiteral("problem"), QString());
        return c;
    }
    QString problem;
    const QJsonObject obj = objectOf(file, &problem);
    QString mode;
    bool lan = false;
    int endpoints = 0;
    if (problem.isEmpty()) {
        const QJsonValue m = obj.value(QLatin1String("mode"));
        if (m.isUndefined())
            mode = QStringLiteral("off");
        else if (m.isString() && knownModes().contains(m.toString()))
            mode = m.toString();
        else
            addProblem(&problem, KillSwitchBackend::tr("names no known mode"));
        lan = boolField(obj, "lan", false, &problem);
        const QJsonValue eps = obj.value(QLatin1String("vpn_endpoints"));
        if (eps.isArray())
            endpoints = int(eps.toArray().size());
        else if (!eps.isUndefined())
            addProblem(&problem, KillSwitchBackend::tr("has a field \"vpn_endpoints\" that is not a list"));
    }
    if (!problem.isEmpty())
        mode.clear();   // shadow-killswitch then fails closed (see the page)
    c.insert(QStringLiteral("mode"), mode);
    c.insert(QStringLiteral("lan"), lan);
    c.insert(QStringLiteral("endpoints"), endpoints);
    c.insert(QStringLiteral("problem"), problem);
    return c;
}

// /run/shadow-anon-state.json: who in Hide Me needs the kill switch.
QStringList hideMeHolders(const FileRead &file)
{
    QStringList holders;
    if (!file.exists || !file.error.isEmpty())
        return holders;
    QString ignored;
    const QJsonObject obj = objectOf(file, &ignored);
    for (const QString &h : stringList(obj, "killswitch_by", 8)) {
        if (h == QLatin1String("ghost") || h == QLatin1String("proxy") || h == QLatin1String("user"))
            holders << h;
    }
    return holders;
}

QVariantMap parseKillSwitch(const FileRead &file)
{
    QVariantMap s;
    QString problem;
    QString mode;
    bool active = false, loaded = false, lan = false, escapeHatch = false, runtimeOverride = false;
    QString error, since, persistent, updated;
    qint64 endpoints = 0, dropped = -1, skipped = 0, tunnels = 0;
    QStringList warnings;
    bool readable = false;

    if (file.exists) {
        const QJsonObject obj = objectOf(file, &problem);
        if (problem.isEmpty()) {
            readable = true;
            const QJsonValue m = obj.value(QLatin1String("mode"));
            if (!m.isString())
                addProblem(&problem, KillSwitchBackend::tr("names no mode"));
            else if (!knownModes().contains(m.toString()))
                addProblem(&problem, KillSwitchBackend::tr("names an unknown mode \"%1\"").arg(m.toString().left(40)));
            else
                mode = m.toString();
            bool activeKnown = false;
            const bool activeField = boolField(obj, "active", true, &problem, &activeKnown);
            if (!mode.isEmpty()) {
                active = mode != QLatin1String("off");
                if (activeKnown && activeField != active)
                    addProblem(&problem, KillSwitchBackend::tr("contradicts itself (mode \"%1\", active %2)")
                                             .arg(mode, activeField ? QStringLiteral("true") : QStringLiteral("false")));
            } else {
                active = !activeKnown || activeField;
            }
            loaded = boolField(obj, "loaded", false, &problem);       // never assumed
            error = errorField(obj);
            since = stringField(obj, "since", &problem);
            lan = boolField(obj, "lan", false, &problem);
            endpoints = qMax<qint64>(0, countField(obj, "endpoints"));
            escapeHatch = boolField(obj, "escape_hatch", false, &problem);
            runtimeOverride = boolField(obj, "runtime_override", false, &problem);
            const QString p = stringField(obj, "persistent_mode", &problem);
            persistent = knownModes().contains(p) ? p : QString();
            dropped = countField(obj, "dropped_packets");
            skipped = qMax<qint64>(0, countField(obj, "skipped_hostnames"));
            tunnels = qMax<qint64>(0, countField(obj, "tunnel_ifaces"));
            updated = stringField(obj, "updated", &problem);
            warnings = stringList(obj, "warnings");
        }
    }
    s.insert(QStringLiteral("present"), file.exists);
    s.insert(QStringLiteral("readable"), readable);
    s.insert(QStringLiteral("problem"), problem);
    s.insert(QStringLiteral("mode"), mode);
    s.insert(QStringLiteral("modeName"), modeName(mode));
    s.insert(QStringLiteral("active"), active);
    s.insert(QStringLiteral("loaded"), loaded);
    s.insert(QStringLiteral("error"), error);
    s.insert(QStringLiteral("since"), since);
    s.insert(QStringLiteral("lan"), lan);
    s.insert(QStringLiteral("endpoints"), endpoints);
    s.insert(QStringLiteral("escapeHatch"), escapeHatch);
    s.insert(QStringLiteral("runtimeOverride"), runtimeOverride);
    s.insert(QStringLiteral("statusPersistentMode"), persistent);
    s.insert(QStringLiteral("droppedPackets"), dropped);
    s.insert(QStringLiteral("skippedHostnames"), skipped);
    s.insert(QStringLiteral("tunnelInterfaces"), tunnels);
    s.insert(QStringLiteral("updated"), updated);
    s.insert(QStringLiteral("warnings"), warnings);
    return s;
}

// Everything that follows from the parsed document, the configuration file
// and the live check: state, protection, the text the user must see.
void deriveKillSwitch(QVariantMap &s, const QVariantMap &config, const QVariantMap &live)
{
    const bool present = s.value(QStringLiteral("present")).toBool();
    const QString problem = s.value(QStringLiteral("problem")).toString();
    const QString error = s.value(QStringLiteral("error")).toString();
    const QString mode = s.value(QStringLiteral("mode")).toString();
    const bool active = s.value(QStringLiteral("active")).toBool();
    const bool loaded = s.value(QStringLiteral("loaded")).toBool();

    // The mode after a restart: what the tool reported, else its configuration file.
    QString persistent = s.value(QStringLiteral("statusPersistentMode")).toString();
    QString persistentSource = QStringLiteral("status");
    if (persistent.isEmpty()) {
        persistent = config.value(QStringLiteral("mode")).toString();
        persistentSource = config.value(QStringLiteral("present")).toBool() ? QStringLiteral("config") : QStringLiteral("default");
    }
    if (!config.value(QStringLiteral("problem")).toString().isEmpty()) {
        persistent = QStringLiteral("tor-or-vpn");      // shadow-killswitch's FAIL_CLOSED_MODE
        persistentSource = QStringLiteral("fail-closed");
    }
    s.insert(QStringLiteral("persistentMode"), persistent);
    s.insert(QStringLiteral("persistentModeName"), modeName(persistent));
    s.insert(QStringLiteral("persistentSource"), persistentSource);
    s.insert(QStringLiteral("config"), config);

    // The live check counts only when it was made against this very document.
    QString liveProblem;
    const QVariant table = live.value(QStringLiteral("killSwitchTable"));
    if (present && problem.isEmpty() && live.value(QStringLiteral("state")) == QLatin1String("done")
        && !live.value(QStringLiteral("outdated")).toBool() && table.isValid() && !table.isNull()) {
        const bool inKernel = table.toBool();
        if (loaded && !inKernel)
            liveProblem = KillSwitchBackend::tr("The status file says the kill switch table is loaded, but the kernel has no such table: traffic is NOT protected.");
        else if (!loaded && inKernel && !active)
            liveProblem = KillSwitchBackend::tr("The kill switch is recorded as off, but its table is in the kernel: traffic may be blocked.");
    }

    // The documented rule: protected only when active, loaded, without error.
    const bool hasError = present && (!problem.isEmpty() || !error.isEmpty() || active != loaded || !liveProblem.isEmpty());
    const bool isProtected = present && !hasError && active && loaded;
    const bool bypassed = present && !hasError && !active && s.value(QStringLiteral("escapeHatch")).toBool()
                          && !persistent.isEmpty() && persistent != QLatin1String("off");
    QString state;
    if (!present)
        state = QStringLiteral("unknown");
    else if (hasError)
        state = QStringLiteral("error");
    else if (active)
        state = QStringLiteral("on");
    else
        state = bypassed ? QStringLiteral("bypassed") : QStringLiteral("off");

    QString errorText;
    if (hasError) {
        if (!problem.isEmpty())
            errorText = KillSwitchBackend::tr("The kill switch status cannot be trusted: the status file %1.").arg(problem);
        else if (!liveProblem.isEmpty())
            errorText = liveProblem;
        else if (!error.isEmpty())
            errorText = error;
        else if (active)
            errorText = KillSwitchBackend::tr("The kill switch is set to \"%1\" but its firewall table is not loaded: traffic is NOT protected.").arg(modeName(mode));
        else
            errorText = KillSwitchBackend::tr("The kill switch is off but its firewall table is still loaded.");
    }
    s.insert(QStringLiteral("hasError"), hasError);
    s.insert(QStringLiteral("protected"), isProtected);
    // Traffic may leave only when a mode is on but its table is not in force (or the
    // document cannot be trusted). An error with the table loaded and confirmed
    // (for example a broken configuration file while the kill switch fails closed)
    // is a problem to fix, not a leak.
    s.insert(QStringLiteral("trafficMayLeave"),
             present && hasError && (!problem.isEmpty() || (active && (!loaded || !liveProblem.isEmpty()))));
    s.insert(QStringLiteral("bypassed"), bypassed);
    s.insert(QStringLiteral("state"), state);
    s.insert(QStringLiteral("errorText"), errorText);
}

// ---- panic ------------------------------------------------------------------------------

// /etc/shadow-os/panic.json, read the way shadow-panic reads it: a missing or
// broken file means the defaults (all true) — a panic must still work.
QVariantMap parsePanicConfig(const FileRead &file)
{
    QVariantMap c{{QStringLiteral("rfkill"), true}, {QStringLiteral("lockSession"), true},
                  {QStringLiteral("clearClipboard"), true}, {QStringLiteral("present"), file.exists},
                  {QStringLiteral("source"), QStringLiteral("default")}, {QStringLiteral("problem"), QString()}};
    if (!file.exists)
        return c;
    QString problem;
    const QJsonObject obj = objectOf(file, &problem);
    if (problem.isEmpty()) {
        c.insert(QStringLiteral("source"), QStringLiteral("file"));
        c.insert(QStringLiteral("rfkill"), boolField(obj, "rfkill", true, &problem));
        c.insert(QStringLiteral("lockSession"), boolField(obj, "lock_session", true, &problem));
        c.insert(QStringLiteral("clearClipboard"), boolField(obj, "clear_clipboard", true, &problem));
    }
    c.insert(QStringLiteral("problem"), problem);
    return c;
}

QVariantMap parsePanic(const FileRead &file, const QVariantMap &config, const QVariantMap &live)
{
    QVariantMap p;
    QString problem, error, since, released;
    bool readable = false, engaged = false, loaded = false, loadedKnown = false;
    QStringList warnings;
    qint64 dropped = -1;
    if (file.exists) {
        const QJsonObject obj = objectOf(file, &problem);
        if (problem.isEmpty()) {
            readable = true;
            engaged = boolField(obj, "engaged", false, &problem);
            // A non-root "status" writes null: not checked against the kernel.
            loaded = boolField(obj, "loaded", false, &problem, &loadedKnown);
            error = errorField(obj);
            since = stringField(obj, "since", &problem);
            released = stringField(obj, "released", &problem);
            warnings = stringList(obj, "warnings");
            dropped = countField(obj, "dropped_packets");
        }
    }
    QString liveProblem;
    const QVariant table = live.value(QStringLiteral("panicTable"));
    if (file.exists && problem.isEmpty() && live.value(QStringLiteral("state")) == QLatin1String("done")
        && !live.value(QStringLiteral("outdated")).toBool() && table.isValid() && !table.isNull()) {
        if (engaged && !table.toBool())
            liveProblem = KillSwitchBackend::tr("A panic is recorded, but the kernel has no panic table: this computer is NOT cut off.");
        else if (!engaged && table.toBool())
            liveProblem = KillSwitchBackend::tr("No panic is recorded, but the panic table is in the kernel: every connection is blocked.");
    }
    const bool hasError = file.exists
                          && (!problem.isEmpty() || !error.isEmpty() || !liveProblem.isEmpty()
                              || (engaged != loaded && (engaged || loadedKnown)));
    QString state;
    if (!file.exists)
        state = QStringLiteral("none");
    else if (hasError)
        state = QStringLiteral("error");
    else if (engaged)
        state = QStringLiteral("engaged");
    else
        state = released.isEmpty() ? QStringLiteral("none") : QStringLiteral("released");
    QString errorText;
    if (hasError) {
        if (!problem.isEmpty())
            errorText = KillSwitchBackend::tr("The panic status cannot be trusted: the status file %1.").arg(problem);
        else if (!liveProblem.isEmpty())
            errorText = liveProblem;
        else if (!error.isEmpty())
            errorText = error;
        else if (engaged)
            errorText = KillSwitchBackend::tr("A panic is recorded, but its firewall table is not loaded: this computer is NOT cut off.");
        else
            errorText = KillSwitchBackend::tr("The panic was released, but its firewall table is still loaded.");
    }
    p.insert(QStringLiteral("present"), file.exists);
    p.insert(QStringLiteral("readable"), readable);
    p.insert(QStringLiteral("problem"), problem);
    p.insert(QStringLiteral("engaged"), engaged);
    p.insert(QStringLiteral("loaded"), loadedKnown ? QVariant(loaded) : QVariant());
    p.insert(QStringLiteral("error"), error);
    p.insert(QStringLiteral("since"), since);
    p.insert(QStringLiteral("released"), released);
    p.insert(QStringLiteral("warnings"), warnings);
    p.insert(QStringLiteral("droppedPackets"), dropped);
    p.insert(QStringLiteral("hasError"), hasError);
    p.insert(QStringLiteral("cut"), file.exists && !hasError && engaged && loaded);
    p.insert(QStringLiteral("state"), state);
    p.insert(QStringLiteral("errorText"), errorText);
    p.insert(QStringLiteral("config"), config);
    return p;
}

// ---- running the tools ------------------------------------------------------------------

// Root runs the tool itself; everybody else goes through polkit.
void runPrivileged(QObject *context, const QString &tool, const QStringList &arguments, Shadow::Command::Callback done,
                   int timeoutMs)
{
    if (geteuid() == 0)
        Shadow::Command::run(context, tool, arguments, std::move(done), timeoutMs);
    else
        Shadow::Command::run(context, QStringLiteral("pkexec"), QStringList{tool} + arguments, std::move(done), timeoutMs);
}

// The tool's own message without its "name: " prefix and without its
// warnings (those are in the status file and shown there).
QString failureText(const Shadow::CommandResult &result, const QString &tool)
{
    if (geteuid() != 0) {
        if (result.exitCode == -1)
            return KillSwitchBackend::tr("pkexec could not be started. Is polkit installed?");
        if (result.exitCode == 126)
            return KillSwitchBackend::tr("The change was not authorised.");
        if (result.exitCode == 127)
            return KillSwitchBackend::tr("Authentication failed: polkit is not running, or no authentication agent is running in this session.");
    } else if (result.exitCode == -1) {
        return KillSwitchBackend::tr("%1 could not be started.").arg(tool);
    }
    if (result.exitCode == -2)
        return KillSwitchBackend::tr("%1 did not finish in time.").arg(QFileInfo(tool).fileName());
    const QString prefix = QFileInfo(tool).fileName() + QStringLiteral(": ");
    QStringList messages, warnings;
    const QString text = result.errText().trimmed().isEmpty() ? result.outText() : result.errText();
    for (QString line : text.split(QLatin1Char('\n'), Qt::SkipEmptyParts)) {
        line = line.trimmed();
        if (line.startsWith(prefix))
            line = line.mid(prefix.size());
        if (line.startsWith(QLatin1String("warning: ")))
            warnings << line.mid(9);
        else if (!line.isEmpty())
            messages << line;
    }
    if (!messages.isEmpty())
        return messages.join(QLatin1Char(' ')).left(1000);
    if (!warnings.isEmpty())
        return warnings.join(QLatin1Char(' ')).left(1000);
    return KillSwitchBackend::tr("%1 failed (exit code %2).").arg(QFileInfo(tool).fileName()).arg(result.exitCode);
}

QSet<QString> allowances(const QString &mode, bool lan)
{
    QSet<QString> a;
    if (mode == QLatin1String("tor") || mode == QLatin1String("tor-or-vpn"))
        a.insert(QStringLiteral("tor"));
    if (mode == QLatin1String("vpn") || mode == QLatin1String("tor-or-vpn"))
        a.insert(QStringLiteral("vpn"));
    if (lan)
        a.insert(QStringLiteral("lan"));
    return a;
}

QByteArray signatureOf(const FileRead &a, const FileRead &b)
{
    QCryptographicHash h(QCryptographicHash::Sha256);
    h.addData(a.exists ? a.data + a.error.toUtf8() : QByteArrayLiteral("-"));
    h.addData(QByteArrayView("\0", 1));
    h.addData(b.exists ? b.data + b.error.toUtf8() : QByteArrayLiteral("-"));
    return h.result();
}

QJsonValue jsonOf(const QVariant &v)
{
    return QJsonValue::fromVariant(v);
}

} // namespace

// ---- the back-end -----------------------------------------------------------------------

KillSwitchBackend::KillSwitchBackend(QObject *parent)
    : QObject(parent)
{
    m_liveCheck = {{QStringLiteral("state"), QStringLiteral("unknown")}};
    m_debounce.setSingleShot(true);
    m_debounce.setInterval(150);
    connect(&m_debounce, &QTimer::timeout, this, [this] { load(); watch(); });
    connect(&m_watcher, &QFileSystemWatcher::directoryChanged, &m_debounce, qOverload<>(&QTimer::start));
    connect(&m_watcher, &QFileSystemWatcher::fileChanged, &m_debounce, qOverload<>(&QTimer::start));
    // A directory created after start (the tools create /run/shadow) is only
    // noticed by this timer.
    m_safetyNet.setInterval(10000);
    connect(&m_safetyNet, &QTimer::timeout, this, [this] { load(); watch(); });
    m_safetyNet.start();
    load();
    watch();
    queryShortcut();
    checkLive();
}

QString KillSwitchBackend::defaultPanicKeys()
{
    // The shell's default (desktop/horizon-shell/src/shortcuts.cpp).
    return QKeySequence(Qt::CTRL | Qt::ALT | Qt::SHIFT | Qt::Key_Escape).toString(QKeySequence::NativeText);
}

QString KillSwitchBackend::toolPath() const { return envPath("SHADOW_KILLSWITCH_TOOL", QStringLiteral("/usr/sbin/shadow-killswitch")); }
QString KillSwitchBackend::panicToolPath() const { return envPath("SHADOW_PANIC_TOOL", QStringLiteral("/usr/sbin/shadow-panic")); }
bool KillSwitchBackend::toolInstalled() const { return QFileInfo(toolPath()).isExecutable(); }
bool KillSwitchBackend::panicToolInstalled() const { return QFileInfo(panicToolPath()).isExecutable(); }
QString KillSwitchBackend::statusFile() const { return runDir() + QStringLiteral("/killswitch.json"); }
QString KillSwitchBackend::panicStatusFile() const { return runDir() + QStringLiteral("/panic.json"); }
QString KillSwitchBackend::configFile() const { return configDir() + QStringLiteral("/killswitch.json"); }
QString KillSwitchBackend::panicConfigFile() const { return configDir() + QStringLiteral("/panic.json"); }

void KillSwitchBackend::refresh()
{
    load();
    watch();
    queryShortcut();
}

void KillSwitchBackend::watch()
{
    QStringList wanted;
    for (const QString &dir : {runDir(), configDir()}) {
        if (QFileInfo(dir).isDir())
            wanted << dir;
    }
    const QString anon = anonStateFile();
    if (QFileInfo::exists(anon))
        wanted << anon;
    const QStringList watched = m_watcher.directories() + m_watcher.files();
    for (const QString &path : wanted) {
        if (!watched.contains(path))
            m_watcher.addPath(path);
    }
}

void KillSwitchBackend::load()
{
    const FileRead ks = readFile(statusFile());
    const FileRead pn = readFile(panicStatusFile());
    // A live check made before the documents changed says nothing about them.
    const QByteArray signature = signatureOf(ks, pn);
    const QByteArray checked = m_liveCheck.value(QStringLiteral("signature")).toByteArray();
    if (!checked.isEmpty() && checked != signature && !m_liveCheck.value(QStringLiteral("outdated")).toBool()) {
        m_liveCheck.insert(QStringLiteral("outdated"), true);
        Q_EMIT liveCheckChanged();
    }

    QVariantMap state = parseKillSwitch(ks);
    deriveKillSwitch(state, parseKillSwitchConfig(readFile(configFile())), m_liveCheck);
    state.insert(QStringLiteral("hideMe"), hideMeHolders(readFile(anonStateFile())));
    const QVariantMap panic = parsePanic(pn, parsePanicConfig(readFile(panicConfigFile())), m_liveCheck);

    // keepAfterRestart follows the state until the user chooses, and again
    // whenever the state changes underneath (another program set a mode).
    const int runtime = state.value(QStringLiteral("runtimeOverride")).toBool() ? 1 : 0;
    if (runtime != m_lastRuntimeOverride) {
        m_lastRuntimeOverride = runtime;
        m_keepTouched = false;
    }
    if (!m_keepTouched && m_keepAfterRestart != (runtime == 0)) {
        m_keepAfterRestart = runtime == 0;
        Q_EMIT keepAfterRestartChanged();
    }

    const bool wasReady = m_ready;
    m_ready = true;
    if (!wasReady || state != m_killSwitch || panic != m_panic) {
        m_killSwitch = state;
        m_panic = panic;
        Q_EMIT changed();
    }
}

void KillSwitchBackend::queryShortcut()
{
    const QDBusConnection bus = QDBusConnection::sessionBus();
    const QString service = QStringLiteral("org.kde.kglobalaccel");
    // Never start the service by activation just to read a key.
    if (!bus.isConnected() || !bus.interface() || !bus.interface()->isServiceRegistered(service).value()) {
        m_shortcut = {{QStringLiteral("keys"), defaultPanicKeys()}, {QStringLiteral("source"), QStringLiteral("default")},
                      {QStringLiteral("note"), tr("The shortcut service (KGlobalAccel) is not running in this session, so the key in use cannot be read; this is the default.")}};
        m_shortcutDone = true;
        Q_EMIT shortcutChanged();
        return;
    }
    QDBusMessage call = QDBusMessage::createMethodCall(service, QStringLiteral("/kglobalaccel"), QStringLiteral("org.kde.KGlobalAccel"),
                                                       QStringLiteral("shortcut"));
    // actionId: component, action, component display name, action display name.
    call << QStringList{QStringLiteral("shadow-horizon"), QStringLiteral("panic"), QStringLiteral("Shadow Horizon"), QStringLiteral("Panic")};
    auto *watcher = new QDBusPendingCallWatcher(bus.asyncCall(call, 5000), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this](QDBusPendingCallWatcher *w) {
        w->deleteLater();
        const QDBusPendingReply<QList<int>> reply = *w;
        if (reply.isError()) {
            m_shortcut = {{QStringLiteral("keys"), defaultPanicKeys()}, {QStringLiteral("source"), QStringLiteral("default")},
                          {QStringLiteral("note"), tr("The shortcut service did not answer (%1); this is the default key.").arg(reply.error().message().left(200))}};
        } else {
            QStringList keys;
            for (const int combined : reply.value()) {
                if (combined != 0)
                    keys << QKeySequence(QKeyCombination::fromCombined(combined)).toString(QKeySequence::NativeText);
            }
            if (keys.isEmpty())
                m_shortcut = {{QStringLiteral("keys"), QString()}, {QStringLiteral("source"), QStringLiteral("session")},
                              {QStringLiteral("note"), tr("No key is bound to the panic action in this session: the desktop shell is not running, or the key was removed.")}};
            else
                m_shortcut = {{QStringLiteral("keys"), keys.join(QStringLiteral(", "))}, {QStringLiteral("source"), QStringLiteral("session")},
                              {QStringLiteral("note"), QString()}};
        }
        m_shortcutDone = true;
        Q_EMIT shortcutChanged();
    });
}

void KillSwitchBackend::checkLive()
{
    if (m_liveCheck.value(QStringLiteral("state")) == QLatin1String("checking"))
        return;
    const QString tool = statusTool();
    if (!QFileInfo(tool).isExecutable()) {
        m_liveCheck = {{QStringLiteral("state"), QStringLiteral("failed")},
                       {QStringLiteral("reason"), tr("%1 is not installed, so the kernel cannot be asked.").arg(tool)}};
        Q_EMIT liveCheckChanged();
        load();
        return;
    }
    const int generation = ++m_liveGeneration;
    const QByteArray signature = signatureOf(readFile(statusFile()), readFile(panicStatusFile()));
    m_liveCheck = {{QStringLiteral("state"), QStringLiteral("checking")}};
    Q_EMIT liveCheckChanged();
    runPrivileged(this, tool, {QStringLiteral("--section"), QStringLiteral("killswitch,panic")},
                  [this, generation, signature, tool](const Shadow::CommandResult &result) {
        if (generation != m_liveGeneration)
            return;
        QVariantMap check{{QStringLiteral("checked"), QDateTime::currentDateTimeUtc().toString(Qt::ISODate)},
                          {QStringLiteral("signature"), signature}};
        const QJsonObject doc = QJsonDocument::fromJson(result.out).object();
        const QJsonObject ks = doc.value(QLatin1String("killswitch")).toObject();
        const QJsonObject pn = doc.value(QLatin1String("panic")).toObject();
        if (!result.ok() || ks.isEmpty()) {
            check.insert(QStringLiteral("state"), QStringLiteral("failed"));
            check.insert(QStringLiteral("reason"), result.ok() ? tr("%1 gave no answer about the kill switch.").arg(tool)
                                                               : failureText(result, tool));
        } else {
            check.insert(QStringLiteral("state"), QStringLiteral("done"));
            const auto table = [](const QJsonObject &section) {
                const QJsonValue v = section.value(QLatin1String("table_present"));
                return section.value(QLatin1String("available")).toBool() && v.isBool() ? QVariant(v.toBool()) : QVariant();
            };
            check.insert(QStringLiteral("killSwitchTable"), table(ks));
            check.insert(QStringLiteral("panicTable"), table(pn));
            QString reason = ks.value(QLatin1String("table_present_reason")).toString();
            if (!ks.value(QLatin1String("available")).toBool())
                reason = ks.value(QLatin1String("reason")).toString();
            check.insert(QStringLiteral("reason"), reason.left(400));
        }
        m_liveCheck = check;
        Q_EMIT liveCheckChanged();
        load();
    }, kCheckTimeoutMs);
}

bool KillSwitchBackend::settled() const
{
    return m_shortcutDone && m_liveCheck.value(QStringLiteral("state")) != QLatin1String("checking");
}

void KillSwitchBackend::setKeepAfterRestart(bool keep)
{
    if (keep == m_keepAfterRestart)
        return;
    m_keepTouched = true;
    m_keepAfterRestart = keep;
    Q_EMIT keepAfterRestartChanged();
    // Saving the mode in force for start-up loosens or tightens nothing now,
    // so it is safe to do at once; switching the preference off only changes
    // how the next change is made.
    if (keep && m_killSwitch.value(QStringLiteral("runtimeOverride")).toBool()
        && knownModes().contains(m_killSwitch.value(QStringLiteral("mode")).toString())
        && !m_killSwitch.value(QStringLiteral("hasError")).toBool())
        runSet(m_killSwitch.value(QStringLiteral("mode")).toString(), m_killSwitch.value(QStringLiteral("lan")).toBool(), true);
}

void KillSwitchBackend::setMode(const QString &mode)
{
    if (!knownModes().contains(mode)) {
        Q_EMIT failed(tr("Unknown kill switch mode \"%1\".").arg(mode.left(40)));
        return;
    }
    runSet(mode, m_killSwitch.value(QStringLiteral("lan")).toBool(), m_keepAfterRestart);
}

void KillSwitchBackend::setLan(bool lan)
{
    const QString mode = m_killSwitch.value(QStringLiteral("mode")).toString();
    if (!knownModes().contains(mode) || mode == QLatin1String("off")) {
        Q_EMIT failed(tr("The local network setting can be changed while the kill switch is on."));
        return;
    }
    runSet(mode, lan, m_keepAfterRestart);
}

void KillSwitchBackend::runSet(const QString &mode, bool lan, bool persist)
{
    QStringList arguments{QStringLiteral("set"), mode, lan ? QStringLiteral("--lan") : QStringLiteral("--no-lan")};
    if (!persist)
        arguments << QStringLiteral("--runtime");
    QString message;
    if (mode == QLatin1String("off"))
        message = persist ? tr("The kill switch is off, also after a restart.") : tr("The kill switch is off until the next restart.");
    else
        message = (persist ? tr("Kill switch: %1, also after a restart.") : tr("Kill switch: %1, until the next restart.")).arg(modeName(mode));
    runTool(QStringLiteral("set"), toolPath(), arguments, message);
}

void KillSwitchBackend::reassert()
{
    runTool(QStringLiteral("reassert"), toolPath(), {QStringLiteral("reassert")}, tr("The kill switch was loaded again."));
}

void KillSwitchBackend::releasePanic()
{
    runTool(QStringLiteral("release"), panicToolPath(), {QStringLiteral("release")},
            tr("The panic was released: networking and radios are as they were before it."));
}

void KillSwitchBackend::runTool(const QString &action, const QString &tool, const QStringList &arguments, const QString &successMessage)
{
    if (busy())
        return;
    if (!QFileInfo(tool).isExecutable()) {
        Q_EMIT failed(tr("%1 is not installed (package shadow-hardening).").arg(tool));
        return;
    }
    m_busyAction = action;
    Q_EMIT busyChanged();
    runPrivileged(this, tool, arguments, [this, action, tool, successMessage](const Shadow::CommandResult &result) {
        m_busyAction.clear();
        Q_EMIT busyChanged();
        m_keepTouched = false;           // follow what is now in force
        load();
        // "Load again" succeeded only if the status it published is now sound: the
        // tool exits 0 when the kernel already had the table, whatever the status
        // file still says.
        if (result.ok() && action == QLatin1String("reassert")
            && m_killSwitch.value(QStringLiteral("state")) == QLatin1String("error"))
            Q_EMIT failed(tr("The kill switch was checked again, but it still reports a problem: %1")
                              .arg(m_killSwitch.value(QStringLiteral("errorText")).toString()));
        else if (result.ok())
            Q_EMIT succeeded(successMessage);
        else
            Q_EMIT failed(failureText(result, tool));
        // The tool published its own status; ask the kernel as well (the
        // user just acted, so a check now is expected).
        if (result.exitCode != 126 && result.exitCode != 127 && result.exitCode != -1)
            checkLive();
    }, kToolTimeoutMs);
}

bool KillSwitchBackend::closesConnections(const QString &mode, bool lan) const
{
    // shadow-killswitch needs_flush(): engaging, or a mode that allows less.
    if (mode == QLatin1String("off"))
        return false;
    const QString old = m_killSwitch.value(QStringLiteral("mode")).toString();
    if (!m_killSwitch.value(QStringLiteral("loaded")).toBool() || old.isEmpty() || old == QLatin1String("off"))
        return true;
    return !allowances(mode, lan).contains(allowances(old, m_killSwitch.value(QStringLiteral("lan")).toBool()));
}

QJsonObject KillSwitchBackend::toJson() const
{
    QVariantMap live = m_liveCheck;
    live.remove(QStringLiteral("signature"));
    return {
        {QStringLiteral("toolPath"), toolPath()},
        {QStringLiteral("toolInstalled"), toolInstalled()},
        {QStringLiteral("panicToolInstalled"), panicToolInstalled()},
        {QStringLiteral("statusFile"), statusFile()},
        {QStringLiteral("configFile"), configFile()},
        {QStringLiteral("panicConfigFile"), panicConfigFile()},
        {QStringLiteral("keepAfterRestart"), m_keepAfterRestart},
        {QStringLiteral("killSwitch"), jsonOf(m_killSwitch)},
        {QStringLiteral("panic"), jsonOf(m_panic)},
        {QStringLiteral("liveCheck"), jsonOf(live)},
        {QStringLiteral("panicShortcut"), jsonOf(m_shortcut)},
    };
}
