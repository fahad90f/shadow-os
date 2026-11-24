// SPDX-License-Identifier: GPL-3.0-or-later
#include "panic.h"

#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusMessage>
#include <QDBusPendingCallWatcher>
#include <QDateTime>
#include <QFile>
#include <QGuiApplication>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QProcess>
#include <QStandardPaths>
#include <QTimer>

#include <csignal>
#include <memory>
#include <sys/types.h>

namespace {

const QString kUnit = QStringLiteral("shadow-panic.service");
const QString kPanicTool = QStringLiteral("/usr/sbin/shadow-panic");
const QString kDefaultConfig = QStringLiteral("/etc/shadow-os/panic.json");
constexpr int kBusTimeoutMs = 10000;
constexpr int kToolTimeoutMs = 5000;

QString dbusError(const QDBusMessage &reply)
{
    const QString text = reply.errorMessage().trimmed();
    return text.isEmpty() ? reply.errorName() : text;
}

// A message as a sentence of its own (D-Bus error texts often lack the
// full stop), so that another sentence can follow it.
QString sentence(const QString &text)
{
    const QString t = text.trimmed();
    if (t.isEmpty() || t.endsWith(QLatin1Char('.')) || t.endsWith(QLatin1Char('!')) || t.endsWith(QLatin1Char('?')))
        return t;
    return t + QLatin1Char('.');
}

// The last non-empty line a program printed (its error message).
QString lastLine(const QByteArray &out)
{
    const QStringList lines = QString::fromUtf8(out).split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    for (auto it = lines.crbegin(); it != lines.crend(); ++it) {
        const QString line = it->trimmed();
        if (!line.isEmpty())
            return line.left(300);
    }
    return {};
}

void call(const QDBusConnection &bus, QDBusMessage message, QObject *context,
          const std::function<void(const QDBusMessage &reply)> &done)
{
    // A panic must never wait for a password dialog.
    message.setInteractiveAuthorizationAllowed(false);
    if (!bus.isConnected()) {
        QTimer::singleShot(0, context, [done, bus] {
            done(QDBusMessage::createError(QStringLiteral("org.freedesktop.DBus.Error.Disconnected"),
                                           bus.lastError().message().isEmpty()
                                               ? QObject::tr("the bus is not reachable")
                                               : bus.lastError().message()));
        });
        return;
    }
    auto *watcher = new QDBusPendingCallWatcher(bus.asyncCall(message, kBusTimeoutMs), context);
    QObject::connect(watcher, &QDBusPendingCallWatcher::finished, context, [done](QDBusPendingCallWatcher *w) {
        w->deleteLater();
        done(w->reply());
    });
}

} // namespace

Panic::Panic(QObject *parent)
    : QObject(parent)
{
}

Panic::~Panic()
{
    // Never leave a half-run helper behind (the release keeps running: it
    // belongs to pkexec and the administrator's decision).
    if (m_clipboardProcess)
        m_clipboardProcess->kill();
}

void Panic::set(QString *field, const QString &value)
{
    if (*field != value) {
        *field = value;
        Q_EMIT changed();
    }
}

Panic::Config Panic::readConfig()
{
    Config config;
    const QString path = qEnvironmentVariable("SHADOW_PANIC_CONFIG", kDefaultConfig);
    QFile file(path);
    if (!file.exists())
        return config;
    if (!file.open(QIODevice::ReadOnly)) {
        config.problem = tr("%1 cannot be read (%2); using the defaults").arg(path, file.errorString());
        return config;
    }
    QJsonParseError error;
    const QJsonDocument doc = QJsonDocument::fromJson(file.read(64 * 1024), &error);
    if (error.error != QJsonParseError::NoError || !doc.isObject()) {
        config.problem = tr("%1 is not a JSON object; using the defaults").arg(path);
        return config;
    }
    const QJsonObject obj = doc.object();
    const auto flag = [&](const char *key, bool *out) {
        const QJsonValue v = obj.value(QLatin1String(key));
        if (v.isBool())
            *out = v.toBool();
        else if (!v.isUndefined() && !v.isNull() && config.problem.isEmpty())
            config.problem = tr("\"%1\" in %2 is not true or false; using true").arg(QLatin1String(key), path);
    };
    flag("lock_session", &config.lockSession);
    flag("clear_clipboard", &config.clearClipboard);
    return config;
}

QJsonObject Panic::dump()
{
    const Config config = readConfig();
    const auto exe = [](const char *name) {
        const QString path = QStandardPaths::findExecutable(QLatin1String(name));
        return path.isEmpty() ? QJsonValue() : QJsonValue(path);
    };
    return QJsonObject{
        {QStringLiteral("unit"), kUnit},
        {QStringLiteral("config"), qEnvironmentVariable("SHADOW_PANIC_CONFIG", kDefaultConfig)},
        {QStringLiteral("lockSession"), config.lockSession},
        {QStringLiteral("clearClipboard"), config.clearClipboard},
        {QStringLiteral("configProblem"), config.problem},
        {QStringLiteral("systemBus"), QDBusConnection::systemBus().isConnected()},
        {QStringLiteral("wlCopy"), exe("wl-copy")},
        {QStringLiteral("wlPaste"), exe("wl-paste")},
        {QStringLiteral("pkexec"), exe("pkexec")},
        {QStringLiteral("releaseCommand"), QStringLiteral("pkexec %1 release").arg(kPanicTool)},
    };
}

// ---- engage -----------------------------------------------------------------------------------

void Panic::engage()
{
    if (m_phase == QLatin1String("starting"))
        return;                     // the first request is still on its way
    // A release that is still running would undo this panic when it
    // finishes: withdraw it (withdrawRelease()).
    // (A NetworkManager Enable(true) already sent cannot be withdrawn; its
    // answer is ignored below, and the panic service turns networking off
    // again.)
    if (m_releaseProcess)
        withdrawRelease();
    m_releasing = false;
    if (m_clipboardProcess) {
        m_clipboardProcess->disconnect(this);
        m_clipboardProcess->kill();
        m_clipboardProcess->deleteLater();
        m_clipboardProcess = nullptr;
    }
    ++m_generation;
    const Config config = readConfig();
    m_phase = QStringLiteral("starting");
    m_error.clear();
    m_fallback.clear();
    m_fallbackError.clear();
    m_releaseError.clear();
    m_lock = config.lockSession ? QStringLiteral("pending") : QStringLiteral("skipped");
    m_lockError.clear();
    m_clipboard = config.clearClipboard ? QStringLiteral("pending") : QStringLiteral("skipped");
    m_clipboardError.clear();
    m_clipboardWarnings.clear();
    m_primaryCleared = false;
    m_requestedAt = QDateTime::currentMSecsSinceEpoch();
    // A record that already says engaged (or a problem) answers this request
    // too; an old "released" one does not.
    m_serviceAnswered = m_serviceState == QLatin1String("engaged") || m_serviceState == QLatin1String("error");
    Q_EMIT changed();
    if (!config.problem.isEmpty())
        qWarning("horizon-shell: panic: %s", qPrintable(config.problem));

    // 1. the network (first: it matters most), 2. the screen, 3. the clipboard.
    startUnit();
    if (config.lockSession)
        lockScreen();
    if (config.clearClipboard)
        clearClipboard();
}

void Panic::startUnit()
{
    QDBusMessage message = QDBusMessage::createMethodCall(QStringLiteral("org.freedesktop.systemd1"),
        QStringLiteral("/org/freedesktop/systemd1"), QStringLiteral("org.freedesktop.systemd1.Manager"),
        QStringLiteral("StartUnit"));
    message << kUnit << QStringLiteral("replace");
    const quint64 generation = m_generation;
    call(QDBusConnection::systemBus(), message, this, [this, generation](const QDBusMessage &reply) {
        if (generation != m_generation)
            return;
        if (reply.type() == QDBusMessage::ReplyMessage) {
            set(&m_phase, QStringLiteral("started"));
            endIfReleasedElsewhere();       // the service answered and was released before this reply
            return;
        }
        m_error = tr("The panic service could not be started: %1").arg(sentence(dbusError(reply)));
        m_phase = QStringLiteral("failed");
        Q_EMIT changed();
        qWarning("horizon-shell: panic: %s", qPrintable(m_error));
        networkingFallback();
    });
}

void Panic::setNetworking(bool enabled, const std::function<void(const QString &error)> &done)
{
    QDBusMessage message = QDBusMessage::createMethodCall(QStringLiteral("org.freedesktop.NetworkManager"),
        QStringLiteral("/org/freedesktop/NetworkManager"), QStringLiteral("org.freedesktop.NetworkManager"),
        QStringLiteral("Enable"));
    message << enabled;
    call(QDBusConnection::systemBus(), message, this, [done](const QDBusMessage &reply) {
        done(reply.type() == QDBusMessage::ReplyMessage ? QString() : dbusError(reply));
    });
}

// What can be done without the service: NetworkManager stops every
// connection it manages. Not a firewall: links NetworkManager does not
// manage stay up, and any program allowed to may turn networking back on.
void Panic::networkingFallback()
{
    set(&m_fallback, QStringLiteral("pending"));
    const quint64 generation = m_generation;
    setNetworking(false, [this, generation](const QString &error) {
        if (generation != m_generation)
            return;
        if (error.isEmpty()) {
            m_fallbackNetworkingOff = true;
            m_fallback = QStringLiteral("networking-off");
            Q_EMIT changed();
            Q_EMIT failed(tr("%1 NetworkManager networking was turned off instead; this is not a firewall cut.").arg(m_error));
        } else {
            m_fallbackError = error;
            m_fallback = QStringLiteral("failed");
            Q_EMIT changed();
            Q_EMIT failed(tr("%1 Turning NetworkManager networking off failed too (%2): the network is NOT cut.")
                              .arg(m_error, error));
        }
    });
}

void Panic::lockScreen()
{
    const QDBusMessage message = QDBusMessage::createMethodCall(QStringLiteral("org.freedesktop.ScreenSaver"),
        QStringLiteral("/ScreenSaver"), QStringLiteral("org.freedesktop.ScreenSaver"), QStringLiteral("Lock"));
    const quint64 generation = m_generation;
    call(QDBusConnection::sessionBus(), message, this, [this, generation](const QDBusMessage &reply) {
        if (generation != m_generation)
            return;
        if (reply.type() == QDBusMessage::ReplyMessage)
            set(&m_lock, QStringLiteral("locked"));
        else
            lockThroughLogind(dbusError(reply));
    });
}

// logind tells the screen locker of the session to lock (it listens for the
// session's Lock signal); the answer only says logind accepted the request.
void Panic::lockThroughLogind(const QString &screenSaverError)
{
    const QDBusMessage message = QDBusMessage::createMethodCall(QStringLiteral("org.freedesktop.login1"),
        QStringLiteral("/org/freedesktop/login1/session/auto"), QStringLiteral("org.freedesktop.login1.Session"),
        QStringLiteral("Lock"));
    const quint64 generation = m_generation;
    call(QDBusConnection::systemBus(), message, this, [this, generation, screenSaverError](const QDBusMessage &reply) {
        if (generation != m_generation)
            return;
        if (reply.type() == QDBusMessage::ReplyMessage) {
            set(&m_lock, QStringLiteral("requested"));
            return;
        }
        m_lockError = tr("the screen locker: %1; logind: %2").arg(screenSaverError, dbusError(reply));
        m_lock = QStringLiteral("failed");
        Q_EMIT changed();
        qWarning("horizon-shell: panic: the screen was not locked (%s)", qPrintable(m_lockError));
    });
}

// ---- clipboard --------------------------------------------------------------------------------

void Panic::clearClipboard()
{
    if (!QGuiApplication::platformName().startsWith(QLatin1String("wayland"))) {
        m_clipboard = QStringLiteral("failed");
        m_clipboardError = tr("not a Wayland session (%1)").arg(QGuiApplication::platformName());
        Q_EMIT changed();
        return;
    }
    if (QStandardPaths::findExecutable(QStringLiteral("wl-copy")).isEmpty()
        || QStandardPaths::findExecutable(QStringLiteral("wl-paste")).isEmpty()) {
        m_clipboard = QStringLiteral("failed");
        m_clipboardError = tr("wl-copy and wl-paste (package wl-clipboard) are not installed");
        Q_EMIT changed();
        return;
    }
    runClipboardStep(0);
}

// 0  wl-copy --clear                     must succeed
// 1  wl-copy --primary --clear           a failure is a warning (no primary selection)
// 2  wl-paste --list-types               must say "Nothing is copied"
// 3  wl-paste --primary --list-types     only after step 1 worked; a failure is a warning
void Panic::runClipboardStep(int step)
{
    static const QList<QStringList> commands{
        {QStringLiteral("wl-copy"), QStringLiteral("--clear")},
        {QStringLiteral("wl-copy"), QStringLiteral("--primary"), QStringLiteral("--clear")},
        {QStringLiteral("wl-paste"), QStringLiteral("--list-types")},
        {QStringLiteral("wl-paste"), QStringLiteral("--primary"), QStringLiteral("--list-types")},
    };
    if (step == 3 && !m_primaryCleared)
        step = 4;
    if (step >= commands.size()) {
        m_clipboard = QStringLiteral("cleared");
        m_clipboardError = m_clipboardWarnings.join(QStringLiteral("; "));
        Q_EMIT changed();
        return;
    }
    const QStringList command = commands.at(step);
    auto *process = new QProcess(this);
    m_clipboardProcess = process;
    process->setProgram(QStandardPaths::findExecutable(command.first()));
    process->setArguments(command.mid(1));
    process->setStandardInputFile(QProcess::nullDevice());
    auto *timeout = new QTimer(process);
    timeout->setSingleShot(true);
    connect(timeout, &QTimer::timeout, process, [process] {
        process->setProperty("timedOut", true);
        process->kill();
    });
    const quint64 generation = m_generation;
    const auto finish = [this, process, step, command, generation](bool started) {
        if (m_clipboardProcess != process || generation != m_generation)
            return;
        m_clipboardProcess = nullptr;
        process->deleteLater();
        const QString shown = command.join(QLatin1Char(' '));
        const bool timedOut = process->property("timedOut").toBool();
        const bool exited = started && !timedOut && process->exitStatus() == QProcess::NormalExit;
        const int code = exited ? process->exitCode() : -1;
        const QByteArray out = started ? process->readAllStandardOutput() : QByteArray();
        const QByteArray err = started ? process->readAllStandardError() : QByteArray();
        const QString why = !started ? tr("%1 could not be started").arg(shown)
            : timedOut ? tr("%1 did not finish within %2 s").arg(shown).arg(kToolTimeoutMs / 1000)
            : !exited ? tr("%1 crashed").arg(shown)
            : tr("%1 failed: %2").arg(shown, lastLine(err).isEmpty() ? tr("status %1").arg(code) : lastLine(err));
        switch (step) {
        case 0:
            if (code != 0) {
                m_clipboard = QStringLiteral("failed");
                m_clipboardError = why;
                Q_EMIT changed();
                return;
            }
            break;
        case 1:
            m_primaryCleared = code == 0;
            if (!m_primaryCleared)
                m_clipboardWarnings << tr("the primary selection was not cleared (%1)").arg(why);
            break;
        default: {
            // wl-paste exits with status 1 and "Nothing is copied" for an
            // empty selection; anything else is not proof of an empty one.
            const bool empty = exited && code != 0 && out.trimmed().isEmpty() && err.contains("Nothing is copied");
            if (!empty) {
                const QString problem = code == 0 ? tr("something is still copied (%1)").arg(lastLine(out)) : why;
                if (step == 2) {
                    m_clipboard = QStringLiteral("failed");
                    m_clipboardError = tr("the clipboard could not be verified as empty: %1").arg(problem);
                    Q_EMIT changed();
                    return;
                }
                m_clipboardWarnings << tr("the primary selection could not be verified as empty: %1").arg(problem);
            }
            break;
        }
        }
        runClipboardStep(step + 1);
    };
    connect(process, &QProcess::finished, this, [finish, timeout] {
        timeout->stop();
        finish(true);
    });
    connect(process, &QProcess::errorOccurred, this, [finish](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart)
            finish(false);
    });
    timeout->start(kToolTimeoutMs);
    process->start();
}

// ---- release ----------------------------------------------------------------------------------

void Panic::release(bool serviceEngaged)
{
    if (m_releasing)
        return;
    m_releaseError.clear();
    if (!serviceEngaged) {
        if (!m_fallbackNetworkingOff) {
            dismiss();
            Q_EMIT releaseFinished(true);
            return;
        }
        m_releasing = true;
        Q_EMIT changed();
        const quint64 generation = m_generation;
        setNetworking(true, [this, generation](const QString &error) {
            if (generation != m_generation)
                return;             // a new panic was asked for meanwhile
            if (error.isEmpty())
                m_fallbackNetworkingOff = false;
            finishRelease(error.isEmpty(), error.isEmpty() ? QString()
                                                           : tr("NetworkManager networking could not be turned back on: %1").arg(error));
        });
        return;
    }
    const QString pkexec = QStandardPaths::findExecutable(QStringLiteral("pkexec"));
    if (pkexec.isEmpty()) {
        m_releaseError = tr("pkexec is not installed; release the panic from a terminal: sudo shadow-panic release");
        Q_EMIT changed();
        Q_EMIT releaseFinished(false);
        return;
    }
    m_releasing = true;
    Q_EMIT changed();
    auto *process = new QProcess(this);
    m_releaseProcess = process;
    process->setProgram(pkexec);
    process->setArguments({kPanicTool, QStringLiteral("release")});
    process->setStandardInputFile(QProcess::nullDevice());
    process->setProcessChannelMode(QProcess::MergedChannels);
    connect(process, &QProcess::errorOccurred, this, [this, process](QProcess::ProcessError error) {
        if (error != QProcess::FailedToStart || m_releaseProcess != process)
            return;
        m_releaseProcess = nullptr;
        process->deleteLater();
        finishRelease(false, tr("pkexec could not be started: %1").arg(process->errorString()));
    });
    connect(process, &QProcess::finished, this, [this, process](int code, QProcess::ExitStatus status) {
        if (m_releaseProcess != process)
            return;
        m_releaseProcess = nullptr;
        process->deleteLater();
        const QString output = lastLine(process->readAll());
        if (status != QProcess::NormalExit) {
            finishRelease(false, tr("the release was interrupted"));
            return;
        }
        if (code == 126 || code == 127) {
            // pkexec: the dialog was dismissed, or authorisation was refused.
            finishRelease(false, tr("Not released: administrator authentication was cancelled or refused."));
            return;
        }
        if (code != 0) {
            finishRelease(false, output.isEmpty() ? tr("shadow-panic release failed (status %1)").arg(code) : output);
            return;
        }
        if (m_fallbackNetworkingOff) {
            const quint64 generation = m_generation;
            setNetworking(true, [this, generation](const QString &error) {
                if (generation != m_generation)
                    return;         // a new panic was asked for meanwhile
                if (error.isEmpty())
                    m_fallbackNetworkingOff = false;
                finishRelease(error.isEmpty(), error.isEmpty() ? QString()
                                                               : tr("Released, but NetworkManager networking could not be turned back on: %1").arg(error));
            });
            return;
        }
        finishRelease(true, {});
    });
    process->start();
}

// A release that is still running when a new panic is asked for. While
// pkexec waits for the administrator it runs as this user: killing it
// cancels the release, and nothing was changed. Once the administrator has
// authenticated, pkexec runs "shadow-panic release" as root (real, effective
// and saved user id 0): the kill is refused (EPERM) and the release goes on.
// Such a release is never deleted while it runs (the destructor of a running
// QProcess waits up to 30 s for it, which would freeze the shell): it is kept
// until it ends, its result ignored, and then the panic is asserted again
// (reassert()), so that the panic, asked for last, wins whichever of the two
// got shadow-panic's lock first.
void Panic::withdrawRelease()
{
    QProcess *process = m_releaseProcess;
    m_releaseProcess = nullptr;
    process->disconnect(this);
    if (process->state() == QProcess::NotRunning) {
        process->deleteLater();
        return;
    }
    const qint64 pid = process->processId();
    const bool killed = pid > 0 && ::kill(static_cast<pid_t>(pid), SIGKILL) == 0;
    if (!killed)
        qWarning("horizon-shell: panic: the release was already authorised and cannot be withdrawn; "
                 "the panic is asserted again when it ends");
    ++m_withdrawnReleases;
    const auto done = std::make_shared<bool>(false);
    const auto ended = [this, process, killed, done](bool mayHaveReleased) {
        if (*done)
            return;
        *done = true;
        process->deleteLater();
        --m_withdrawnReleases;
        if (!killed)
            reassert(mayHaveReleased);
    };
    connect(process, &QProcess::finished, this, [ended](int code, QProcess::ExitStatus status) {
        // A release that failed (normal exit, status not 0) published no
        // "released" record; anything else may have.
        ended(status != QProcess::NormalExit || code == 0);
    });
    connect(process, &QProcess::errorOccurred, this, [ended](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart)
            ended(false);
    });
}

// After a withdrawn, authorised release has ended: if this session's panic
// is still what was asked for last (not released, dismissed or followed by a
// new release since), cut again.
void Panic::reassert(bool mayHaveReleased)
{
    if (m_releasing)
        return;
    if (m_phase == QLatin1String("failed")) {
        // The service could not be started; the release may have turned
        // NetworkManager networking back on after the fallback turned it off.
        if (m_fallbackNetworkingOff) {
            setNetworking(false, [](const QString &error) {
                if (!error.isEmpty())
                    qWarning("horizon-shell: panic: turning NetworkManager networking off again failed: %s",
                             qPrintable(error));
            });
        }
        return;
    }
    if (m_phase != QLatin1String("starting") && m_phase != QLatin1String("started"))
        return;
    // The release's "released" record must not end this request: only a
    // release after the service has answered again does.
    if (mayHaveReleased)
        m_serviceAnswered = false;
    m_requestedAt = QDateTime::currentMSecsSinceEpoch();
    Q_EMIT changed();
    startUnit();
}

void Panic::setServiceState(const QString &state)
{
    if (state == m_serviceState)
        return;
    m_serviceState = state;
    if (state == QLatin1String("engaged") || state == QLatin1String("error"))
        m_serviceAnswered = true;
    else
        endIfReleasedElsewhere();
}

// Released by someone else (the Control Center's "Reconnect…", sudo
// shadow-panic release) after the service answered this request: the request
// is over, as after the banner's own release. Not while the banner's own
// release runs (it ends the request itself), nor while a withdrawn release
// still runs (its record is about to be overridden by reassert()).
void Panic::endIfReleasedElsewhere()
{
    if (m_serviceState != QLatin1String("released") || !m_serviceAnswered || m_phase != QLatin1String("started")
        || m_releasing || m_withdrawnReleases > 0)
        return;
    dismiss();
}

void Panic::finishRelease(bool ok, const QString &error)
{
    m_releasing = false;
    m_releaseError = error;
    if (ok) {
        ++m_generation;             // late answers of the engage no longer matter
        m_phase = QStringLiteral("idle");
        m_error.clear();
        m_fallback.clear();
        m_fallbackError.clear();
        m_lock.clear();
        m_lockError.clear();
        m_clipboard.clear();
        m_clipboardError.clear();
        m_requestedAt = 0;
    } else {
        qWarning("horizon-shell: panic release: %s", qPrintable(error));
    }
    Q_EMIT changed();
    Q_EMIT releaseFinished(ok);
}

void Panic::dismiss()
{
    if (m_releasing || m_phase == QLatin1String("starting"))
        return;
    if (m_fallbackNetworkingOff)
        return;                     // networking is still off: only a release ends this
    if (m_withdrawnReleases > 0)
        return;                     // the panic is asserted again when that release ends
    ++m_generation;
    m_phase = QStringLiteral("idle");
    m_error.clear();
    m_fallback.clear();
    m_fallbackError.clear();
    m_lock.clear();
    m_lockError.clear();
    m_clipboard.clear();
    m_clipboardError.clear();
    m_releaseError.clear();
    m_requestedAt = 0;
    Q_EMIT changed();
}
