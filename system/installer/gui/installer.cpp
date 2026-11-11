// SPDX-License-Identifier: GPL-3.0-or-later
#include "installer.h"

#include "options.h"
#include "validation.h"

#include <QClipboard>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusPendingCallWatcher>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSet>
#include <QStandardPaths>
#include <QTime>
#include <QUuid>

namespace {

constexpr int kProbeTimeoutMs = 120 * 1000;
constexpr int kProbeSystemsTimeoutMs = 300 * 1000;
constexpr int kPlanTimeoutMs = 120 * 1000;
constexpr int kMaxLogLines = 4000;

// pkexec(1): 126 — not authorised or the dialog was dismissed; 127 — the
// authorisation could not be obtained (or the program could not be run).
constexpr int kPkexecDismissed = 126;
constexpr int kPkexecFailed = 127;

QString tail(const QByteArray &bytes, int lines)
{
    const QStringList all = QString::fromUtf8(bytes).trimmed().split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    return all.mid(qMax(0, all.size() - lines)).join(QLatin1Char('\n'));
}

// The engine reports a refused request as one JSON line on stdout:
// {"event": "error", "code": ..., "message": ..., "hint": ...}
bool lastErrorEvent(const QByteArray &out, QJsonObject *event)
{
    const QList<QByteArray> lines = out.split('\n');
    for (auto it = lines.crbegin(); it != lines.crend(); ++it) {
        const QByteArray line = it->trimmed();
        if (line.isEmpty())
            continue;
        const QJsonDocument doc = QJsonDocument::fromJson(line);
        if (doc.isObject() && doc.object().value(QLatin1String("event")).toString() == QLatin1String("error")) {
            *event = doc.object();
            return true;
        }
        return false;
    }
    return false;
}

// Appends 'utf8' to 'out' as a JSON string, without an intermediate copy.
void appendJsonString(QByteArray &out, const QByteArray &utf8)
{
    static const char hex[] = "0123456789abcdef";
    out.append('"');
    for (const char c : utf8) {
        const auto u = static_cast<unsigned char>(c);
        if (c == '"' || c == '\\') {
            out.append('\\');
            out.append(c);
        } else if (u < 0x20) {
            out.append("\\u00");
            out.append(hex[u >> 4]);
            out.append(hex[u & 0xf]);
        } else {
            out.append(c);
        }
    }
    out.append('"');
}

QString stamp()
{
    return QTime::currentTime().toString(QStringLiteral("HH:mm:ss"));
}

} // namespace

Installer::Installer(QObject *parent)
    : QObject(parent)
{
    m_elapsedTick.setInterval(1000);
    connect(&m_elapsedTick, &QTimer::timeout, this, [this] {
        const int seconds = int(m_elapsed.elapsed() / 1000);
        if (seconds != m_elapsedSeconds) {
            m_elapsedSeconds = seconds;
            Q_EMIT elapsedChanged();
        }
    });
    loadProfiles();
    const Options &o = Options::instance();
    if (o.testMode() && !o.presetFile.isEmpty())
        loadPreset(o.presetFile);
}

Installer::~Installer()
{
    if (m_installProcess && m_installProcess->state() != QProcess::NotRunning) {
        // Never reached in normal use: the window refuses to close while the
        // engine works. Do not wait for a root process that cannot be signalled.
        abandon(m_installProcess);
    }
    clearSecrets();
}

bool Installer::testMode() const { return Options::instance().testMode(); }
bool Installer::simulation() const
{
    const Options &o = Options::instance();
    return o.testMode() && o.customEngine && !o.simulateScenario.isEmpty();
}
bool Installer::liveSession() const { return Options::instance().liveSession; }
QString Installer::startStep() const { return Options::instance().startStep; }
QString Installer::probeFile() const { return Options::instance().probeFile; }
QString Installer::enginePath() const { return Options::instance().enginePath; }
QString Installer::profilesDir() const { return Options::instance().profilesDir; }

QString Installer::installBlockedReason() const
{
    const Options &o = Options::instance();
    if (o.testMode())
        return tr("Test mode: the hardware shown comes from the file %1, not from this computer. "
                  "Nothing can be installed in this mode.").arg(o.probeFile);
    if (!o.liveSession && !o.allowInstalledSystem)
        return tr("This is not a NEXUS OS live session (%1 does not exist). Installing from a system that "
                  "runs from its own disk is refused unless the installer is started with "
                  "--allow-installed-system.").arg(Options::liveSessionMarker());
    return QString();
}

// ---------------------------------------------------------------------------
// Starting the engine

Installer::Invocation Installer::engineCommand(const QStringList &engineArguments) const
{
    const Options &o = Options::instance();
    Invocation inv;
    inv.arguments = engineArguments;
    const QFileInfo engine(o.enginePath);
    if (!engine.isFile()) {
        inv.problem = o.customEngine
            ? tr("The installation engine %1 does not exist.").arg(o.enginePath)
            : tr("The installation engine %1 is not installed (package “nexus-installer”).").arg(o.enginePath);
        return inv;
    }
    if (o.testMode()) {
        // Synthetic hardware: an extension only the test engine understands.
        // The real engine rejects the unknown option before doing anything.
        inv.program = o.enginePath;
        inv.arguments << QStringLiteral("--probe-file") << o.probeFile;
        return inv;
    }
    if (o.runningAsRoot) {
        inv.program = o.enginePath;
        return inv;
    }
    if (!QFileInfo(Options::pkexecPath()).isExecutable()) {
        inv.problem = tr("%1 is missing (package “pkexec”), so the installer cannot ask for administrator "
                         "rights to run its engine.").arg(Options::pkexecPath());
        return inv;
    }
    inv.program = Options::pkexecPath();
    inv.arguments.prepend(o.enginePath);
    inv.viaPkexec = true;
    return inv;
}

QProcess *Installer::createProcess(const Invocation &invocation)
{
    auto *process = new QProcess(this);
    process->setProgram(invocation.program);
    process->setArguments(invocation.arguments);
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert(QStringLiteral("LC_ALL"), QStringLiteral("C.UTF-8"));
    env.insert(QStringLiteral("PYTHONDONTWRITEBYTECODE"), QStringLiteral("1"));
    const Options &o = Options::instance();
    if (o.testMode() && !o.simulateScenario.isEmpty())
        env.insert(QStringLiteral("NEXUS_TEST_ENGINE_SCENARIO"), o.simulateScenario);
    else
        env.remove(QStringLiteral("NEXUS_TEST_ENGINE_SCENARIO"));
    process->setProcessEnvironment(env);
    return process;
}

// A process that no longer matters. It may run as root (pkexec replaces
// itself with the engine), in which case it cannot be signalled from here;
// it is therefore never waited for.
void Installer::abandon(QProcess *process)
{
    process->disconnect();
    process->setParent(nullptr);
    connect(process, &QProcess::finished, process, &QObject::deleteLater);
    process->closeWriteChannel();
    process->closeReadChannel(QProcess::StandardOutput);
    process->closeReadChannel(QProcess::StandardError);
    process->kill();
}

void Installer::runOnce(const QStringList &engineArguments, QByteArray stdinDocument, int timeoutMs,
                        std::function<void(const Outcome &, bool)> done)
{
    const Invocation inv = engineCommand(engineArguments);
    if (!inv.problem.isEmpty()) {
        wipeBytes(stdinDocument);
        Outcome outcome;
        outcome.started = false;
        outcome.startError = inv.problem;
        QTimer::singleShot(0, this, [done, outcome] { done(outcome, false); });
        return;
    }
    QProcess *process = createProcess(inv);
    const bool viaPkexec = inv.viaPkexec;
    auto finished = std::make_shared<bool>(false);
    auto *timer = new QTimer(process);
    timer->setSingleShot(true);

    connect(process, &QProcess::finished, this, [finished, process, done, viaPkexec](int exitCode, QProcess::ExitStatus status) {
        if (*finished)
            return;
        *finished = true;
        Outcome outcome;
        outcome.exitCode = exitCode;
        outcome.crashed = status == QProcess::CrashExit;
        outcome.out = process->readAllStandardOutput();
        outcome.err = process->readAllStandardError();
        process->deleteLater();
        done(outcome, viaPkexec);
    });
    connect(process, &QProcess::errorOccurred, this, [this, finished, process, done, viaPkexec](QProcess::ProcessError error) {
        if (error != QProcess::FailedToStart || *finished)
            return;
        *finished = true;
        Outcome outcome;
        outcome.started = false;
        outcome.startError = tr("%1 could not be started: %2").arg(process->program(), process->errorString());
        process->deleteLater();
        // errorOccurred can fire inside start(); report after it has returned.
        QTimer::singleShot(0, this, [done, outcome, viaPkexec] { done(outcome, viaPkexec); });
    });
    connect(timer, &QTimer::timeout, this, [finished, process, done, viaPkexec] {
        if (*finished)
            return;
        *finished = true;
        Outcome outcome;
        outcome.timedOut = true;
        outcome.err = process->readAllStandardError();
        abandon(process);
        done(outcome, viaPkexec);
    });

    process->start();
    if (process->state() != QProcess::NotRunning) {
        if (!stdinDocument.isEmpty())
            process->write(stdinDocument);
        process->closeWriteChannel();
        timer->start(timeoutMs);
    }
    wipeBytes(stdinDocument);
}

QString Installer::describeFailure(const Outcome &outcome, bool viaPkexec, QString *detail, QString *hint) const
{
    const QString engine = Options::instance().enginePath;
    if (!outcome.started)
        return outcome.startError;
    if (outcome.timedOut) {
        *detail = tail(outcome.err, 12);
        return viaPkexec
            ? tr("The installation engine did not answer in time. If an authentication dialog is still open, "
                 "close it and try again.")
            : tr("The installation engine did not answer in time.");
    }
    if (outcome.crashed) {
        *detail = tail(outcome.err, 12);
        return tr("The installation engine (%1) stopped unexpectedly.").arg(engine);
    }
    if (viaPkexec && outcome.exitCode == kPkexecDismissed && outcome.out.trimmed().isEmpty()) {
        *detail = tail(outcome.err, 6);
        return tr("Administrator rights were not granted: the authentication dialog was dismissed or this "
                  "account is not allowed to run the installer. Nothing was changed.");
    }
    if (viaPkexec && outcome.exitCode == kPkexecFailed && outcome.out.trimmed().isEmpty()) {
        *detail = tail(outcome.err, 6);
        return tr("Administrator rights could not be obtained (pkexec exit status 127). Either the password "
                  "was wrong, no polkit authentication agent is running in this session, or the engine could "
                  "not be executed. Nothing was changed.");
    }
    QJsonObject event;
    if (lastErrorEvent(outcome.out, &event)) {
        const QString eventHint = event.value(QLatin1String("hint")).toString();
        if (hint)
            *hint = eventHint;
        else
            *detail = eventHint;
        return event.value(QLatin1String("message")).toString();
    }
    *detail = tail(outcome.err, 12);
    if (outcome.exitCode == 2 && detail->contains(QLatin1String("unrecognized arguments: --probe-file")))
        return tr("The engine %1 does not accept a synthetic hardware description. Test mode needs the test "
                  "engine: --engine …/system/installer/gui/testing/test-engine.").arg(engine);
    return tr("The installation engine failed (exit status %1) without an explanation.").arg(outcome.exitCode);
}

// ---------------------------------------------------------------------------
// probe

void Installer::runProbe(bool otherSystems)
{
    if (m_probeState == QLatin1String("running") || m_installState == QLatin1String("running"))
        return;
    const Options &o = Options::instance();
    const quint64 serial = ++m_probeSerial;
    const bool keep = otherSystems && m_probeState == QLatin1String("ready");
    m_scanningSystems = keep;
    if (!keep) {
        m_probeState = QStringLiteral("running");
        m_probe.clear();
    }
    m_probeError.clear();
    m_probeErrorDetail.clear();
    Q_EMIT probeChanged();

    if (o.testMode() && !o.customEngine) {
        // Test mode without a test engine: the file is the probe report.
        QTimer::singleShot(0, this, [this, serial] {
            if (serial != m_probeSerial)
                return;
            m_scanningSystems = false;
            QFile file(Options::instance().probeFile);
            QString problem;
            if (!file.open(QIODevice::ReadOnly))
                setProbeFailure(tr("The probe file %1 cannot be read: %2.").arg(file.fileName(), file.errorString()), {});
            else if (!acceptProbe(file.readAll(), &problem))
                setProbeFailure(tr("The probe file %1 is not a probe report.").arg(file.fileName()), problem);
        });
        return;
    }

    QStringList arguments{QStringLiteral("probe")};
    if (otherSystems)
        arguments << QStringLiteral("--other-systems");
    runOnce(arguments, {}, otherSystems ? kProbeSystemsTimeoutMs : kProbeTimeoutMs,
            [this, serial, otherSystems](const Outcome &outcome, bool viaPkexec) {
        if (serial == m_probeSerial)
            finishProbe(outcome, viaPkexec, otherSystems);
    });
}

void Installer::finishProbe(const Outcome &outcome, bool viaPkexec, bool otherSystems)
{
    const bool hadReport = m_scanningSystems;
    m_scanningSystems = false;
    QString error, detail;
    if (!outcome.started || outcome.timedOut || outcome.crashed || outcome.exitCode != 0) {
        error = describeFailure(outcome, viaPkexec, &detail);
    } else {
        QString problem;
        if (acceptProbe(outcome.out, &problem))
            return;
        error = tr("The installation engine answered, but not with a hardware report it is supposed to print.");
        detail = problem + (outcome.err.trimmed().isEmpty() ? QString() : QLatin1Char('\n') + tail(outcome.err, 8));
    }
    if (hadReport && otherSystems) {
        // The report on screen stays valid; only the extra scan failed.
        m_probeError = tr("The scan for installed operating systems failed: %1").arg(error);
        m_probeErrorDetail = detail;
        Q_EMIT probeChanged();
        return;
    }
    setProbeFailure(error, detail);
}

bool Installer::acceptProbe(const QByteArray &json, QString *problem)
{
    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(json, &parseError);
    if (!doc.isObject()) {
        *problem = parseError.error != QJsonParseError::NoError
            ? tr("Not valid JSON: %1 (at byte %2 of %3).").arg(parseError.errorString()).arg(parseError.offset).arg(json.size())
            : tr("The output is JSON, but not an object.");
        return false;
    }
    const QJsonObject report = doc.object();
    QStringList missing;
    if (!report.value(QLatin1String("disks")).isArray())
        missing << QStringLiteral("disks");
    if (!report.value(QLatin1String("checks")).isArray())
        missing << QStringLiteral("checks");
    if (!report.value(QLatin1String("firmware")).isObject())
        missing << QStringLiteral("firmware");
    if (!report.value(QLatin1String("live_medium")).isObject())
        missing << QStringLiteral("live_medium");
    if (!report.value(QLatin1String("can_install")).isBool())
        missing << QStringLiteral("can_install");
    if (!missing.isEmpty()) {
        *problem = tr("The report lacks: %1.").arg(missing.join(QStringLiteral(", ")));
        return false;
    }
    m_probe = report.toVariantMap();
    m_probeState = QStringLiteral("ready");
    m_probeError.clear();
    m_probeErrorDetail.clear();
    // Whatever was planned was planned for the previous picture of the machine.
    discardPlan();
    Q_EMIT probeChanged();
    return true;
}

void Installer::setProbeFailure(const QString &error, const QString &detail)
{
    m_probeState = QStringLiteral("failed");
    m_probe.clear();
    m_probeError = error;
    m_probeErrorDetail = detail;
    Q_EMIT probeChanged();
}

// ---------------------------------------------------------------------------
// secrets

void Installer::setPassword(const QString &password)
{
    m_password.set(password);
    Q_EMIT secretsChanged();
}

void Installer::setPassphrase(const QString &passphrase)
{
    m_passphrase.set(passphrase);
    Q_EMIT secretsChanged();
}

void Installer::clearSecrets()
{
    m_password.clear();
    m_passphrase.clear();
    Q_EMIT secretsChanged();
}

// The complete answer document for the engine's standard input: the recorded
// answers plus the two secrets. The secrets are spliced in as bytes, so they
// never exist as a QString or inside a QJsonObject here.
QByteArray Installer::buildStdinDocument() const
{
    const QString marker = QUuid::createUuid().toString(QUuid::Id128);
    const QString passwordToken = QStringLiteral("NEXUS-SECRET-A-") + marker;
    const QString passphraseToken = QStringLiteral("NEXUS-SECRET-B-") + marker;

    QJsonObject doc = m_answers;
    QJsonObject user = doc.value(QLatin1String("user")).toObject();
    user.insert(QStringLiteral("password"), passwordToken);
    doc.insert(QStringLiteral("user"), user);
    QJsonObject storage = doc.value(QLatin1String("storage")).toObject();
    const bool encrypt = storage.value(QLatin1String("encrypt")).toBool();
    if (encrypt) {
        storage.insert(QStringLiteral("passphrase"), passphraseToken);
        doc.insert(QStringLiteral("storage"), storage);
    }
    const QByteArray skeleton = QJsonDocument(doc).toJson(QJsonDocument::Compact);

    QByteArray out;
    out.reserve(skeleton.size() + 6 * (m_password.utf8().size() + m_passphrase.utf8().size()) + 16);
    const QByteArray quotedPassword = '"' + passwordToken.toLatin1() + '"';
    const QByteArray quotedPassphrase = '"' + passphraseToken.toLatin1() + '"';
    qsizetype pos = 0;
    while (pos < skeleton.size()) {
        const qsizetype a = skeleton.indexOf(quotedPassword, pos);
        const qsizetype b = encrypt ? skeleton.indexOf(quotedPassphrase, pos) : -1;
        if (a < 0 && b < 0) {
            out.append(skeleton.constData() + pos, skeleton.size() - pos);
            break;
        }
        const bool takeA = a >= 0 && (b < 0 || a < b);
        const qsizetype at = takeA ? a : b;
        out.append(skeleton.constData() + pos, at - pos);
        appendJsonString(out, takeA ? m_password.utf8() : m_passphrase.utf8());
        pos = at + (takeA ? quotedPassword.size() : quotedPassphrase.size());
    }
    out.append('\n');
    return out;
}

bool Installer::writeAnswersRecord()
{
    if (!m_privateDir) {
        QString base = QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation);
        if (base.isEmpty())
            base = QDir::tempPath();
        m_privateDir = std::make_unique<QTemporaryDir>(base + QStringLiteral("/nexus-installer-XXXXXX"));   // mode 0700
        if (!m_privateDir->isValid()) {
            m_privateDir.reset();
            m_answersFile.clear();
            return false;
        }
    }
    const QString path = m_privateDir->filePath(QStringLiteral("answers.json"));
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)
        || !file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner)) {
        m_answersFile.clear();
        return false;
    }
    file.write(QJsonDocument(m_answers).toJson(QJsonDocument::Indented));
    file.close();
    m_answersFile = path;
    return true;
}

// ---------------------------------------------------------------------------
// plan

void Installer::discardPlan()
{
    ++m_planSerial;
    if (m_planState == QLatin1String("idle") && m_plan.isEmpty())
        return;
    m_planState = QStringLiteral("idle");
    m_plan.clear();
    m_planError.clear();
    m_planHint.clear();
    Q_EMIT planChanged();
}

void Installer::setPlanFailure(const QString &error, const QString &hint)
{
    m_planState = QStringLiteral("failed");
    m_plan.clear();
    m_planError = error;
    m_planHint = hint;
    Q_EMIT planChanged();
}

void Installer::requestPlan(const QVariantMap &answers)
{
    if (m_installState == QLatin1String("running"))
        return;
    const quint64 serial = ++m_planSerial;
    m_plan.clear();
    m_planError.clear();
    m_planHint.clear();

    // The record never holds a secret, whatever the caller passed.
    QJsonObject doc = QJsonObject::fromVariantMap(answers);
    QJsonObject user = doc.value(QLatin1String("user")).toObject();
    user.remove(QStringLiteral("password"));
    user.remove(QStringLiteral("password_hash"));
    doc.insert(QStringLiteral("user"), user);
    QJsonObject storage = doc.value(QLatin1String("storage")).toObject();
    storage.remove(QStringLiteral("passphrase"));
    doc.insert(QStringLiteral("storage"), storage);
    m_answers = doc;

    if (m_password.isEmpty()) {
        setPlanFailure(tr("No password has been entered for the user account."),
                       tr("Go back to the step “User” and enter the password twice."));
        return;
    }
    if (storage.value(QLatin1String("encrypt")).toBool() && m_passphrase.isEmpty()) {
        setPlanFailure(tr("Disk encryption is selected, but no passphrase has been entered."),
                       tr("Go back to the step “Disk” and enter the passphrase twice."));
        return;
    }
    writeAnswersRecord();       // a record only; the engine reads standard input

    m_planState = QStringLiteral("running");
    Q_EMIT planChanged();
    runOnce({QStringLiteral("plan"), QStringLiteral("--answers"), QStringLiteral("-")}, buildStdinDocument(),
            kPlanTimeoutMs, [this, serial](const Outcome &outcome, bool viaPkexec) {
        if (serial != m_planSerial)
            return;
        if (!outcome.started || outcome.timedOut || outcome.crashed || outcome.exitCode != 0) {
            QString detail, hint;
            const QString error = describeFailure(outcome, viaPkexec, &detail, &hint);
            setPlanFailure(error, hint.isEmpty() ? detail : hint);
            return;
        }
        QJsonParseError parseError;
        const QJsonDocument reply = QJsonDocument::fromJson(outcome.out, &parseError);
        static const QRegularExpression sha256(QStringLiteral("\\A[0-9a-f]{64}\\z"));
        const QJsonObject plan = reply.object();
        if (!reply.isObject() || !sha256.match(plan.value(QLatin1String("hash")).toString()).hasMatch()
            || !plan.value(QLatin1String("steps")).isArray() || !plan.value(QLatin1String("partitions")).isArray()) {
            setPlanFailure(tr("The installation engine answered, but not with an installation plan."),
                           parseError.error != QJsonParseError::NoError
                               ? tr("Not valid JSON: %1 (at byte %2 of %3).").arg(parseError.errorString())
                                     .arg(parseError.offset).arg(outcome.out.size())
                               : tr("The plan lacks its hash, steps or partitions."));
            return;
        }
        m_plan = plan.toVariantMap();
        m_planState = QStringLiteral("ready");
        Q_EMIT planChanged();
    });
}

// ---------------------------------------------------------------------------
// install

void Installer::appendLog(const QString &line)
{
    const QStringList lines = line.split(QLatin1Char('\n'));
    for (const QString &l : lines)
        m_log.append(QStringLiteral("[%1] %2").arg(stamp(), l));
    if (m_log.size() > kMaxLogLines)
        m_log.remove(0, m_log.size() - kMaxLogLines);
    Q_EMIT logChanged();
}

void Installer::markActiveStep(const QString &state)
{
    if (m_steps.isEmpty())
        return;
    QVariantMap last = m_steps.constLast().toMap();
    if (last.value(QStringLiteral("state")).toString() == QLatin1String("active")) {
        last.insert(QStringLiteral("state"), state);
        m_steps.last() = last;
    }
}

void Installer::resetInstall()
{
    if (m_installState == QLatin1String("running") || m_installState == QLatin1String("idle"))
        return;
    m_installState = QStringLiteral("idle");
    Q_EMIT installChanged();
}

bool Installer::startInstall(const QString &confirmedHash)
{
    const Options &o = Options::instance();
    if (m_installState == QLatin1String("running") || m_installState == QLatin1String("succeeded"))
        return false;
    // Independent of what the pages offer: no destructive run in test mode or
    // outside a live session.
    if (o.testMode() ? !simulation() : !installBlockedReason().isEmpty())
        return false;
    if (m_planState != QLatin1String("ready") || confirmedHash.isEmpty()
        || m_plan.value(QStringLiteral("hash")).toString() != confirmedHash)
        return false;

    m_steps.clear();
    m_stepIndex = m_stepTotal = 0;
    m_percent = -1;
    m_progressMessage.clear();
    m_log.clear();
    m_warnings.clear();
    m_errorMessage.clear();
    m_errorHint.clear();
    m_errorCode.clear();
    m_engineLogPath.clear();
    m_doneMessage.clear();
    m_sawDone = m_sawEvent = false;
    m_installBuffer.clear();
    m_elapsedSeconds = 0;
    m_elapsed.start();
    m_elapsedTick.start();
    m_installState = QStringLiteral("running");
    Q_EMIT elapsedChanged();
    Q_EMIT logChanged();
    Q_EMIT installChanged();

    const Invocation inv = engineCommand({QStringLiteral("install"), QStringLiteral("--answers"), QStringLiteral("-"),
                                          QStringLiteral("--confirm"), confirmedHash});
    if (!inv.problem.isEmpty()) {
        m_errorMessage = inv.problem;
        endInstall(QStringLiteral("failed"));
        return false;
    }
    if (simulation())
        appendLog(tr("SIMULATION by the test engine (scenario “%1”): no program is started, no disk is touched.")
                      .arg(o.simulateScenario));
    appendLog(tr("Starting the installation engine: %1 install --answers - --confirm %2").arg(o.enginePath, confirmedHash));

    QProcess *process = createProcess(inv);
    m_installProcess = process;
    m_installViaPkexec = inv.viaPkexec;
    connect(process, &QProcess::readyReadStandardOutput, this, &Installer::readInstallOutput);
    connect(process, &QProcess::readyReadStandardError, this, [this, process] {
        const QString text = QString::fromUtf8(process->readAllStandardError()).trimmed();
        if (!text.isEmpty())
            appendLog(tr("engine (stderr): %1").arg(text));
    });
    connect(process, &QProcess::finished, this, &Installer::installFinished);
    connect(process, &QProcess::errorOccurred, this, [this, process](QProcess::ProcessError error) {
        if (error != QProcess::FailedToStart || m_installProcess != process)
            return;
        m_installProcess = nullptr;
        process->deleteLater();
        m_errorMessage = tr("%1 could not be started: %2").arg(process->program(), process->errorString());
        m_errorHint = tr("Nothing was changed on any disk.");
        endInstall(QStringLiteral("failed"));
    });

    process->start();
    if (process->state() != QProcess::NotRunning) {
        QByteArray document = buildStdinDocument();
        process->write(document);
        process->closeWriteChannel();
        wipeBytes(document);
    }
    return true;
}

void Installer::readInstallOutput()
{
    if (!m_installProcess)
        return;
    m_installBuffer += m_installProcess->readAllStandardOutput();
    qsizetype newline;
    while ((newline = m_installBuffer.indexOf('\n')) >= 0) {
        const QByteArray line = m_installBuffer.left(newline).trimmed();
        m_installBuffer.remove(0, newline + 1);
        if (line.isEmpty())
            continue;
        const QJsonDocument doc = QJsonDocument::fromJson(line);
        if (doc.isObject() && doc.object().value(QLatin1String("event")).isString())
            handleEvent(doc.object());
        else
            appendLog(tr("engine output that is not a progress event: %1").arg(QString::fromUtf8(line)));
    }
}

void Installer::handleEvent(const QJsonObject &event)
{
    m_sawEvent = true;
    const QString kind = event.value(QLatin1String("event")).toString();
    if (kind == QLatin1String("step")) {
        markActiveStep(QStringLiteral("done"));
        m_stepIndex = event.value(QLatin1String("index")).toInt();
        m_stepTotal = event.value(QLatin1String("total")).toInt();
        const QString title = event.value(QLatin1String("title")).toString();
        m_steps.append(QVariantMap{{QStringLiteral("id"), event.value(QLatin1String("id")).toString()},
                                   {QStringLiteral("title"), title},
                                   {QStringLiteral("state"), QStringLiteral("active")}});
        m_progressMessage.clear();
        appendLog(tr("Step %1 of %2: %3").arg(m_stepIndex).arg(m_stepTotal).arg(title));
    } else if (kind == QLatin1String("progress")) {
        if (event.value(QLatin1String("percent")).isDouble())
            m_percent = qBound(0.0, event.value(QLatin1String("percent")).toDouble(), 100.0);
        const QString message = event.value(QLatin1String("message")).toString();
        if (!message.isEmpty() && message != m_progressMessage) {
            m_progressMessage = message;
            appendLog(message);
        }
    } else if (kind == QLatin1String("log")) {
        const QString message = event.value(QLatin1String("message")).toString();
        if (event.value(QLatin1String("level")).toString() == QLatin1String("warning")) {
            m_warnings.append(message);
            appendLog(tr("Warning: %1").arg(message));
        } else {
            appendLog(message);
        }
    } else if (kind == QLatin1String("error")) {
        m_errorCode = event.value(QLatin1String("code")).toString();
        m_errorMessage = event.value(QLatin1String("message")).toString();
        m_errorHint = event.value(QLatin1String("hint")).toString();
        m_engineLogPath = event.value(QLatin1String("log")).toString();
        markActiveStep(QStringLiteral("failed"));
        appendLog(tr("Error (%1): %2").arg(m_errorCode, m_errorMessage));
        if (!m_errorHint.isEmpty())
            appendLog(m_errorHint);
    } else if (kind == QLatin1String("done")) {
        m_sawDone = true;
        m_doneMessage = event.value(QLatin1String("message")).toString();
        markActiveStep(QStringLiteral("done"));
        appendLog(m_doneMessage);
    } else {
        appendLog(tr("engine event of an unknown kind: %1").arg(QString::fromUtf8(QJsonDocument(event).toJson(QJsonDocument::Compact))));
    }
    Q_EMIT installChanged();
}

void Installer::installFinished(int exitCode, QProcess::ExitStatus status)
{
    QProcess *process = m_installProcess;
    if (!process)
        return;
    readInstallOutput();
    if (!m_installBuffer.trimmed().isEmpty())
        appendLog(tr("engine output that is not a progress event: %1").arg(QString::fromUtf8(m_installBuffer.trimmed())));
    m_installBuffer.clear();
    const QString stderrText = QString::fromUtf8(process->readAllStandardError()).trimmed();
    if (!stderrText.isEmpty())
        appendLog(tr("engine (stderr): %1").arg(stderrText));
    m_installProcess = nullptr;
    process->deleteLater();

    const bool crashed = status == QProcess::CrashExit;
    appendLog(crashed ? tr("The engine was ended by a signal.") : tr("The engine ended with exit status %1.").arg(exitCode));

    if (!crashed && (exitCode == 3 || m_errorCode == QLatin1String("plan-changed"))) {
        if (m_errorMessage.isEmpty())
            m_errorMessage = tr("The installation plan differs from the one that was confirmed.");
        endInstall(QStringLiteral("plan-changed"));
        return;
    }
    if (!crashed && exitCode == 0 && m_sawDone) {
        endInstall(QStringLiteral("succeeded"));
        return;
    }
    markActiveStep(QStringLiteral("failed"));
    if (crashed) {
        m_errorMessage = tr("The installation engine stopped unexpectedly (ended by a signal) before it finished.");
        m_errorCode = QStringLiteral("crashed");
    } else if (m_installViaPkexec && !m_sawEvent && exitCode == kPkexecDismissed) {
        m_errorMessage = tr("Administrator rights were not granted: the authentication dialog was dismissed or "
                            "this account is not allowed to run the installer.");
        m_errorHint = tr("The installation did not start. Nothing was changed on any disk.");
        m_errorCode = QStringLiteral("not-authorized");
    } else if (m_installViaPkexec && !m_sawEvent && exitCode == kPkexecFailed) {
        m_errorMessage = tr("Administrator rights could not be obtained (pkexec exit status 127): the password "
                            "was wrong, no polkit authentication agent is running, or the engine could not be executed.");
        m_errorHint = tr("The installation did not start. Nothing was changed on any disk.");
        m_errorCode = QStringLiteral("not-authorized");
    } else if (exitCode == 0) {
        m_errorMessage = tr("The installation engine ended without reporting that the installation is complete.");
        m_errorHint = tr("The state of the target disk is unknown; do not rely on it.");
        m_errorCode = QStringLiteral("incomplete");
    } else if (m_errorMessage.isEmpty()) {
        m_errorMessage = tr("The installation engine failed (exit status %1) without an explanation.").arg(exitCode);
        m_errorHint = stderrText.section(QLatin1Char('\n'), -6);
        m_errorCode = QStringLiteral("unexplained");
    }
    endInstall(QStringLiteral("failed"));
}

void Installer::endInstall(const QString &state)
{
    m_elapsedTick.stop();
    if (m_elapsed.isValid()) {
        m_elapsedSeconds = int(m_elapsed.elapsed() / 1000);
        Q_EMIT elapsedChanged();
    }
    m_installState = state;
    if (state == QLatin1String("plan-changed"))
        discardPlan();
    Q_EMIT installChanged();
    if (state == QLatin1String("succeeded")) {
        // The secrets have done their job.
        clearSecrets();
        Q_EMIT secretsScrubbed();
    }
}

// ---------------------------------------------------------------------------
// afterwards

void Installer::restart()
{
    if (m_restarting)
        return;
    if (Options::instance().testMode()) {
        m_restartError = tr("Test mode: the computer is not restarted.");
        Q_EMIT restartChanged();
        return;
    }
    QDBusConnection bus = QDBusConnection::systemBus();
    if (!bus.isConnected()) {
        m_restartError = tr("The system bus is not available, so the login manager (systemd-logind) cannot be "
                            "asked to restart. Restart from the session menu instead.");
        Q_EMIT restartChanged();
        return;
    }
    m_restartError.clear();
    m_restarting = true;
    Q_EMIT restartChanged();
    QDBusMessage call = QDBusMessage::createMethodCall(QStringLiteral("org.freedesktop.login1"),
                                                       QStringLiteral("/org/freedesktop/login1"),
                                                       QStringLiteral("org.freedesktop.login1.Manager"),
                                                       QStringLiteral("Reboot"));
    call << true;       // interactive: polkit may ask
    auto *watcher = new QDBusPendingCallWatcher(bus.asyncCall(call, 60000), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this](QDBusPendingCallWatcher *w) {
        w->deleteLater();
        const QDBusMessage reply = w->reply();
        if (reply.type() == QDBusMessage::ErrorMessage) {
            m_restarting = false;
            m_restartError = tr("The login manager refused to restart the computer: %1").arg(reply.errorMessage());
            Q_EMIT restartChanged();
        }
        // On success logind ends the session; "restarting" stays set.
    });
}

void Installer::copyLog()
{
    QString text = logText();
    if (!m_errorMessage.isEmpty())
        text += QStringLiteral("\n\n") + m_errorMessage + (m_errorHint.isEmpty() ? QString() : QLatin1Char('\n') + m_errorHint);
    if (!m_engineLogPath.isEmpty())
        text += QStringLiteral("\n") + tr("Detailed engine log (readable by root): %1").arg(m_engineLogPath);
    copyText(text);
}

void Installer::copyText(const QString &text)
{
    if (QClipboard *clipboard = QGuiApplication::clipboard())
        clipboard->setText(text);
}

// ---------------------------------------------------------------------------
// validation (validation.h mirrors the engine's answers.py)

QString Installer::userNameProblem(const QString &name) const { return Validation::userNameProblem(name); }
QString Installer::fullNameProblem(const QString &name) const { return Validation::fullNameProblem(name); }
QString Installer::hostNameProblem(const QString &name) const { return Validation::hostNameProblem(name); }
QString Installer::passwordProblem(const QString &password) const { return Validation::passwordProblem(password); }
QString Installer::passphraseProblem(const QString &passphrase) const { return Validation::passphraseProblem(passphrase); }
QVariantMap Installer::passwordFacts(const QString &password, const QStringList &personalWords) const
{
    return Validation::passwordFacts(password, personalWords);
}
QString Installer::suggestUserName(const QString &fullName) const { return Validation::suggestUserName(fullName); }

// ---------------------------------------------------------------------------
// tool profiles, preset, diagnostics

void Installer::loadProfiles()
{
    // answers.py: PROFILE_RE — the names the engine accepts.
    static const QRegularExpression engineName(QStringLiteral("\\A[a-z0-9][a-z0-9-]{0,40}\\z"));
    const QDir dir(Options::instance().profilesDir);
    struct Raw
    {
        QJsonObject object;
        QStringList packages;
    };
    QMap<QString, Raw> raw;
    const QStringList files = dir.entryList({QStringLiteral("*.json")}, QDir::Files, QDir::Name);
    for (const QString &name : files) {
        QFile file(dir.filePath(name));
        if (!file.open(QIODevice::ReadOnly))
            continue;
        const QJsonObject object = QJsonDocument::fromJson(file.readAll()).object();
        const QString id = object.value(QLatin1String("id")).toString();
        // Other files of the directory (categories.json, schema.json) are not profiles.
        if (!engineName.match(id).hasMatch() || object.value(QLatin1String("name")).toString().isEmpty()
            || !object.value(QLatin1String("tools")).isArray())
            continue;
        Raw entry{object, {}};
        const QJsonArray tools = object.value(QLatin1String("tools")).toArray();
        for (const QJsonValue &tool : tools) {
            const QString package = tool.toObject().value(QLatin1String("package")).toString();
            if (!package.isEmpty())
                entry.packages << package;
        }
        raw.insert(id, entry);
    }
    for (auto it = raw.cbegin(); it != raw.cend(); ++it) {
        const QJsonObject &object = it->object;
        // Tools of included profiles count as well (they are referenced, not copied).
        QSet<QString> packages(it->packages.cbegin(), it->packages.cend());
        QStringList includeNames;
        const QJsonArray includes = object.value(QLatin1String("includes")).toArray();
        for (const QJsonValue &include : includes) {
            const auto other = raw.constFind(include.toString());
            if (other == raw.cend())
                continue;
            includeNames << other->object.value(QLatin1String("name")).toString();
            for (const QString &package : other->packages)
                packages.insert(package);
        }
        QStringList sorted(packages.cbegin(), packages.cend());
        sorted.sort();
        const QString metapackage = object.value(QLatin1String("metapackage")).toString(QStringLiteral("nexus-arsenal-") + it.key());
        m_profiles.append(QVariantMap{
            {QStringLiteral("id"), it.key()},
            {QStringLiteral("name"), object.value(QLatin1String("name")).toString()},
            {QStringLiteral("summary"), object.value(QLatin1String("summary")).toString()},
            {QStringLiteral("description"), object.value(QLatin1String("description")).toString()},
            {QStringLiteral("packages"), sorted},
            {QStringLiteral("toolCount"), int(sorted.size())},
            {QStringLiteral("includes"), includeNames},
            {QStringLiteral("metapackage"), metapackage},
            // Already part of the running (live) system, and therefore of its copy.
            {QStringLiteral("installed"), QFileInfo::exists(QStringLiteral("/var/lib/dpkg/info/%1.list").arg(metapackage))},
        });
    }
}

void Installer::loadPreset(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        qWarning("nexus-installer: cannot read the preset %s", qPrintable(path));
        return;
    }
    QJsonObject doc = QJsonDocument::fromJson(file.readAll()).object();
    QJsonObject user = doc.value(QLatin1String("user")).toObject();
    QJsonObject storage = doc.value(QLatin1String("storage")).toObject();
    m_password.set(user.take(QStringLiteral("password")).toString());
    m_passphrase.set(storage.take(QStringLiteral("passphrase")).toString());
    doc.insert(QStringLiteral("user"), user);
    doc.insert(QStringLiteral("storage"), storage);
    m_preset = doc.toVariantMap();
}

QJsonObject Installer::environmentJson() const
{
    const Options &o = Options::instance();
    return {
        {QStringLiteral("engine"), o.enginePath},
        {QStringLiteral("engineInstalled"), QFileInfo(o.enginePath).isFile()},
        {QStringLiteral("pkexec"), QFileInfo(Options::pkexecPath()).isExecutable()},
        {QStringLiteral("liveSession"), o.liveSession},
        {QStringLiteral("liveSessionMarker"), Options::liveSessionMarker()},
        {QStringLiteral("runningAsRoot"), o.runningAsRoot},
        {QStringLiteral("testMode"), o.testMode()},
        {QStringLiteral("probeFile"), o.probeFile},
        {QStringLiteral("allowInstalledSystem"), o.allowInstalledSystem},
        {QStringLiteral("installBlockedReason"), installBlockedReason()},
        {QStringLiteral("profilesDir"), o.profilesDir},
    };
}

QJsonArray Installer::profilesJson() const
{
    return QJsonArray::fromVariantList(m_profiles);
}
