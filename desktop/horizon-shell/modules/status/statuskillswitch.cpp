// SPDX-License-Identifier: GPL-3.0-or-later
#include "statuskillswitch.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>

#include <cmath>

namespace StatusModule {

namespace NetGuard {

namespace {

QString tr(const char *text)
{
    return QCoreApplication::translate("StatusModule::KillSwitchBackend", text);
}

const QStringList &knownModes()
{
    static const QStringList modes{QStringLiteral("off"), QStringLiteral("tor"), QStringLiteral("vpn"),
                                   QStringLiteral("tor-or-vpn")};
    return modes;
}

// Collects the first thing wrong with a document; later problems are not
// worth a longer message (the tool's own status command says it all).
void addProblem(QString *problem, const QString &text)
{
    if (problem->isEmpty())
        *problem = text;
}

// A boolean field. Missing or null: 'fallback'. Anything else than true or
// false: 'fallback' and a problem.
bool boolField(const QJsonObject &obj, const char *key, bool fallback, QString *problem, bool *known = nullptr)
{
    const QJsonValue v = obj.value(QLatin1String(key));
    if (known)
        *known = v.isBool();
    if (v.isBool())
        return v.toBool();
    if (!v.isUndefined() && !v.isNull())
        addProblem(problem, tr("its field \"%1\" is not true or false").arg(QLatin1String(key)));
    return fallback;
}

QString stringField(const QJsonObject &obj, const char *key, QString *problem)
{
    const QJsonValue v = obj.value(QLatin1String(key));
    if (v.isString())
        return v.toString();
    if (!v.isUndefined() && !v.isNull())
        addProblem(problem, tr("its field \"%1\" is not text").arg(QLatin1String(key)));
    return {};
}

// A count: a non-negative whole number. -1 when missing, null or invalid.
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

// The tool's "error": null or text. Anything else is still an error (shown
// as JSON), never silently dropped.
QString errorField(const QJsonObject &obj)
{
    const QJsonValue v = obj.value(QLatin1String("error"));
    if (v.isUndefined() || v.isNull())
        return {};
    if (v.isString())
        return v.toString().trimmed().isEmpty() ? tr("an unspecified error") : v.toString().trimmed();
    if (v.isBool() && !v.toBool())
        return {};
    QJsonArray wrap{v};
    QByteArray text = QJsonDocument(wrap).toJson(QJsonDocument::Compact);
    return QString::fromUtf8(text.mid(1, text.size() - 2));
}

// Parses an existing file's bytes into a JSON object; empty object and a
// problem otherwise.
QJsonObject objectOf(const FileRead &file, QString *problem)
{
    if (!file.error.isEmpty()) {
        addProblem(problem, tr("cannot be read: %1").arg(file.error));
        return {};
    }
    QJsonParseError pe;
    const QJsonDocument doc = QJsonDocument::fromJson(file.data, &pe);
    if (pe.error != QJsonParseError::NoError) {
        addProblem(problem, file.data.trimmed().isEmpty() ? tr("is empty")
                                                          : tr("is not valid JSON (%1 at byte %2)").arg(pe.errorString()).arg(pe.offset));
        return {};
    }
    if (!doc.isObject()) {
        addProblem(problem, tr("is not a JSON object"));
        return {};
    }
    return doc.object();
}

} // namespace

FileRead readStateFile(const QString &path)
{
    FileRead out;
    const QFileInfo info(path);
    if (!info.exists() && !info.isSymLink())
        return out;
    out.exists = true;
    if (!info.exists()) {
        out.error = tr("it is a dangling symbolic link");
        return out;
    }
    if (!info.isFile()) {
        out.error = tr("it is not a regular file");
        return out;
    }
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        out.error = file.errorString();
        return out;
    }
    out.data = file.read(kMaxStateFileSize + 1);
    if (out.data.size() > kMaxStateFileSize) {
        out.data.clear();
        out.error = tr("it is larger than %1 bytes").arg(kMaxStateFileSize);
    } else if (file.error() != QFileDevice::NoError) {
        out.data.clear();
        out.error = file.errorString();
    }
    return out;
}

// ---- kill switch ------------------------------------------------------------------------------

KillSwitchState parseKillSwitch(const FileRead &file)
{
    KillSwitchState s;
    s.present = file.exists;
    if (!file.exists)
        return s;
    const QJsonObject obj = objectOf(file, &s.problem);
    if (!s.problem.isEmpty())
        return s;
    s.readable = true;

    const QJsonValue mode = obj.value(QLatin1String("mode"));
    if (!mode.isString())
        addProblem(&s.problem, tr("it names no mode"));
    else if (!knownModes().contains(mode.toString()))
        addProblem(&s.problem, tr("it names an unknown mode \"%1\"").arg(mode.toString().left(40)));
    else
        s.mode = mode.toString();

    bool activeKnown = false;
    const bool active = boolField(obj, "active", true, &s.problem, &activeKnown);
    if (!s.mode.isEmpty()) {
        s.active = s.mode != QLatin1String("off");
        if (activeKnown && active != s.active)
            addProblem(&s.problem, tr("it contradicts itself (mode \"%1\", active %2)")
                                       .arg(s.mode, active ? QStringLiteral("true") : QStringLiteral("false")));
    } else {
        // Unknown mode: assume something was meant to be on; the problem
        // makes this an error either way.
        s.active = !activeKnown || active;
    }
    // Never assume the table is loaded.
    s.loaded = boolField(obj, "loaded", false, &s.problem);
    s.error = errorField(obj);
    s.since = stringField(obj, "since", &s.problem);
    s.lan = boolField(obj, "lan", false, &s.problem);
    s.endpoints = int(qBound(qint64(0), countField(obj, "endpoints"), qint64(1000000)));
    s.escapeHatch = boolField(obj, "escape_hatch", false, &s.problem);
    s.runtimeOverride = boolField(obj, "runtime_override", false, &s.problem);
    const QString persistent = stringField(obj, "persistent_mode", &s.problem);
    s.persistentMode = knownModes().contains(persistent) ? persistent : QString();
    s.droppedPackets = countField(obj, "dropped_packets");
    s.updated = stringField(obj, "updated", &s.problem);
    return s;
}

bool KillSwitchState::isProtected() const
{
    return present && readable && problem.isEmpty() && active && loaded && error.isEmpty();
}

bool KillSwitchState::hasError() const
{
    if (!present)
        return false;
    if (!problem.isEmpty() || !error.isEmpty())
        return true;
    // On but not loaded: nothing is protected. Off but loaded: traffic is
    // blocked although the switch says off.
    return active != loaded;
}

bool KillSwitchState::bypassed() const
{
    return present && !hasError() && !active && escapeHatch && !persistentMode.isEmpty()
           && persistentMode != QLatin1String("off");
}

QString KillSwitchState::state() const
{
    if (!present)
        return QStringLiteral("unknown");
    if (hasError())
        return QStringLiteral("error");
    if (active)
        return QStringLiteral("on");
    return bypassed() ? QStringLiteral("bypassed") : QStringLiteral("off");
}

QString KillSwitchState::errorText() const
{
    if (!hasError())
        return {};
    if (!problem.isEmpty())
        return tr("The kill switch status cannot be trusted: the status file %1.").arg(problem);
    if (!error.isEmpty())
        return error;
    if (active)
        return tr("The kill switch is set to \"%1\" but its firewall table is not loaded: traffic is NOT protected.").arg(mode);
    return tr("The kill switch is off but its firewall table is still loaded.");
}

QJsonObject KillSwitchState::toJson() const
{
    QJsonObject json{{QStringLiteral("present"), present}, {QStringLiteral("state"), state()}};
    if (!present)
        return json;
    json.insert(QStringLiteral("readable"), readable);
    json.insert(QStringLiteral("mode"), mode.isEmpty() ? QJsonValue() : QJsonValue(mode));
    json.insert(QStringLiteral("active"), active);
    json.insert(QStringLiteral("loaded"), loaded);
    json.insert(QStringLiteral("protected"), isProtected());
    json.insert(QStringLiteral("hasError"), hasError());
    json.insert(QStringLiteral("errorText"), errorText());
    json.insert(QStringLiteral("problem"), problem);
    json.insert(QStringLiteral("error"), error.isEmpty() ? QJsonValue() : QJsonValue(error));
    json.insert(QStringLiteral("since"), since);
    json.insert(QStringLiteral("lan"), lan);
    json.insert(QStringLiteral("endpoints"), endpoints);
    json.insert(QStringLiteral("escapeHatch"), escapeHatch);
    json.insert(QStringLiteral("runtimeOverride"), runtimeOverride);
    json.insert(QStringLiteral("persistentMode"), persistentMode);
    json.insert(QStringLiteral("droppedPackets"), droppedPackets);
    json.insert(QStringLiteral("updated"), updated);
    return json;
}

// ---- panic ----------------------------------------------------------------------------------------

PanicState parsePanic(const FileRead &file)
{
    PanicState p;
    p.present = file.exists;
    if (!file.exists)
        return p;
    const QJsonObject obj = objectOf(file, &p.problem);
    if (!p.problem.isEmpty())
        return p;
    p.readable = true;
    p.engaged = boolField(obj, "engaged", false, &p.problem);
    // Non-root "status" writes null: not checked against the kernel.
    p.loaded = boolField(obj, "loaded", false, &p.problem, &p.loadedKnown);
    p.error = errorField(obj);
    p.since = stringField(obj, "since", &p.problem);
    p.released = stringField(obj, "released", &p.problem);
    p.clearClipboard = boolField(obj, "clear_clipboard", true, &p.problem);
    p.lockSession = boolField(obj, "lock_session", true, &p.problem);
    const QJsonValue warnings = obj.value(QLatin1String("warnings"));
    if (warnings.isArray()) {
        for (const QJsonValue &w : warnings.toArray()) {
            if (w.isString() && p.warnings.size() < 20)
                p.warnings << w.toString().left(300);
        }
    }
    p.droppedPackets = countField(obj, "dropped_packets");
    return p;
}

bool PanicState::isCut() const
{
    return present && readable && problem.isEmpty() && engaged && loaded && error.isEmpty();
}

bool PanicState::hasError() const
{
    if (!present)
        return false;
    if (!problem.isEmpty() || !error.isEmpty())
        return true;
    return engaged != loaded && (engaged || loadedKnown);
}

QString PanicState::state() const
{
    if (!present)
        return QStringLiteral("none");
    if (hasError())
        return QStringLiteral("error");
    if (engaged)
        return QStringLiteral("engaged");
    return released.isEmpty() ? QStringLiteral("none") : QStringLiteral("released");
}

QString PanicState::errorText() const
{
    if (!hasError())
        return {};
    if (!problem.isEmpty())
        return tr("The panic status cannot be trusted: the status file %1.").arg(problem);
    if (!error.isEmpty())
        return error;
    if (engaged)
        return tr("A panic is recorded, but its firewall table is not loaded: this computer is NOT cut off.");
    return tr("The panic was released, but its firewall table is still loaded.");
}

QJsonObject PanicState::toJson() const
{
    QJsonObject json{{QStringLiteral("present"), present}, {QStringLiteral("state"), state()}};
    if (!present)
        return json;
    json.insert(QStringLiteral("readable"), readable);
    json.insert(QStringLiteral("engaged"), engaged);
    json.insert(QStringLiteral("loaded"), loadedKnown ? QJsonValue(loaded) : QJsonValue());
    json.insert(QStringLiteral("cut"), isCut());
    json.insert(QStringLiteral("hasError"), hasError());
    json.insert(QStringLiteral("errorText"), errorText());
    json.insert(QStringLiteral("problem"), problem);
    json.insert(QStringLiteral("error"), error.isEmpty() ? QJsonValue() : QJsonValue(error));
    json.insert(QStringLiteral("since"), since);
    json.insert(QStringLiteral("released"), released);
    json.insert(QStringLiteral("clearClipboard"), clearClipboard);
    json.insert(QStringLiteral("lockSession"), lockSession);
    json.insert(QStringLiteral("warnings"), QJsonArray::fromStringList(warnings));
    json.insert(QStringLiteral("droppedPackets"), droppedPackets);
    return json;
}

} // namespace NetGuard

// ---- the back-end -------------------------------------------------------------------------------

KillSwitchBackend::KillSwitchBackend(QObject *parent)
    : Backend(parent)
{
    m_dir = QDir::cleanPath(qEnvironmentVariable("SHADOW_STATUS_RUN_DIR", QStringLiteral("/run/shadow")));
    m_debounce.setSingleShot(true);
    m_debounce.setInterval(50);
    m_poll.setInterval(5000);
    connect(&m_debounce, &QTimer::timeout, this, &KillSwitchBackend::reload);
    connect(&m_poll, &QTimer::timeout, this, &KillSwitchBackend::reload);
    connect(&m_watcher, &QFileSystemWatcher::directoryChanged, this, &KillSwitchBackend::scheduleReload);
    connect(&m_watcher, &QFileSystemWatcher::fileChanged, this, &KillSwitchBackend::scheduleReload);
    reload();
}

void KillSwitchBackend::scheduleReload()
{
    m_debounce.start();
}

void KillSwitchBackend::refresh()
{
    reload();
}

void KillSwitchBackend::updateWatches()
{
    QStringList wanted;
    if (QFileInfo(m_dir).isDir()) {
        wanted << m_dir;
        for (const char *name : {"killswitch.json", "panic.json"}) {
            const QString file = m_dir + QLatin1Char('/') + QLatin1String(name);
            if (QFileInfo(file).isFile())
                wanted << file;
        }
    } else {
        // Wait for the directory to be created: watch the nearest parent
        // that exists.
        QString parent = m_dir;
        while (true) {
            const QString up = QFileInfo(parent).absolutePath();
            if (up == parent)
                break;
            parent = up;
            if (QFileInfo(parent).isDir())
                break;
        }
        if (QFileInfo(parent).isDir())
            wanted << parent;
    }

    QStringList watched = m_watcher.files() + m_watcher.directories();
    QStringList stale;
    for (const QString &path : std::as_const(watched)) {
        if (!wanted.contains(path))
            stale << path;
    }
    if (!stale.isEmpty())
        m_watcher.removePaths(stale);
    QStringList missing;
    for (const QString &path : std::as_const(wanted)) {
        if (!watched.contains(path))
            missing << path;
    }
    const QStringList failed = missing.isEmpty() ? QStringList() : m_watcher.addPaths(missing);
    watched = m_watcher.files() + m_watcher.directories();
    bool ok = !wanted.isEmpty();
    for (const QString &path : std::as_const(wanted))
        ok = ok && watched.contains(path);
    if (!failed.isEmpty())
        qWarning("horizon-shell: cannot watch %s for kill switch and panic changes; checking every 5 s",
                 qPrintable(failed.join(QStringLiteral(", "))));
    if (ok != m_watching) {
        m_watching = ok;
        Q_EMIT changed();
    }
    if (m_watching)
        m_poll.stop();
    else if (!m_poll.isActive())
        m_poll.start();
}

void KillSwitchBackend::reload()
{
    updateWatches();
    m_ks = NetGuard::parseKillSwitch(NetGuard::readStateFile(m_dir + QStringLiteral("/killswitch.json")));
    m_panic = NetGuard::parsePanic(NetGuard::readStateFile(m_dir + QStringLiteral("/panic.json")));
    const QJsonObject ks = m_ks.toJson();
    const QJsonObject panic = m_panic.toJson();
    const bool differs = ks != m_lastKs || panic != m_lastPanic;
    m_lastKs = ks;
    m_lastPanic = panic;
    if (!m_ks.present)
        setAvailable(false, tr("The kill switch has published no status: %1/killswitch.json does not exist "
                               "(shadow-killswitch is not installed or has not run in this boot).").arg(m_dir));
    else if (!m_ks.readable)
        setAvailable(false, m_ks.errorText());
    else
        setAvailable(true);
    if (differs)
        Q_EMIT changed();
}

bool KillSwitchBackend::indicatorVisible() const
{
    return m_ks.present && (m_ks.active || m_ks.hasError() || m_ks.bypassed());
}

QJsonObject KillSwitchBackend::details() const
{
    QJsonObject json = m_lastKs;
    json.insert(QStringLiteral("directory"), m_dir);
    json.insert(QStringLiteral("watching"), m_watching);
    json.insert(QStringLiteral("indicatorVisible"), indicatorVisible());
    json.insert(QStringLiteral("panic"), m_lastPanic);
    return json;
}

} // namespace StatusModule
