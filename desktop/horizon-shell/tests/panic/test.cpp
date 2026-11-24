// SPDX-License-Identifier: GPL-3.0-or-later
//
// The shell's panic action (src/panic.h) against stand-ins, without a
// compositor: fake-panic-services.py (systemd, NetworkManager, logind) on a
// private "system" bus, and pkexec played by small scripts and by
// root-pkexec.c (an administrator who has authenticated: the release then
// runs as root and can no longer be killed by the user). inner.sh builds and
// runs it; see there for the environment it sets.
//
// What it checks:
//  * a panic released elsewhere (Control Center, sudo shadow-panic release)
//    ends the shell's request once panic.json says "released" (and an old
//    "released" record from before the request does not);
//  * a panic pressed while the banner's release still waits for the
//    administrator withdraws that release (pkexec is killed, nothing undone);
//  * a panic pressed after the administrator authenticated (the release runs
//    as root) neither freezes the shell (a QProcess destroyed while running
//    waits up to 30 s for it) nor loses to that release: the panic service is
//    started again once the release has ended, and the request is not ended
//    by the release's "released" record;
//  * the banner's own release still ends the request.
#include "panic.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTimer>

#include <cerrno>
#include <csignal>
#include <cstdio>
#include <functional>

namespace {

int failures = 0;
int skipped = 0;

void check(const char *what, bool ok, const QString &detail = {})
{
    std::printf("%s  %s%s\n", ok ? "ok   " : "FAIL ", what,
                detail.isEmpty() ? "" : qPrintable(QStringLiteral(" (%1)").arg(detail)));
    std::fflush(stdout);
    if (!ok)
        ++failures;
}

void skip(const char *what, const char *why)
{
    std::printf("SKIP  %s: %s\n", what, why);
    ++skipped;
}

// One pass of the shell's main event loop. processEvents() alone leaves
// deleteLater() undone (the shell's exec() loop does it as soon as control
// returns to it), so the deferred deletions are run here explicitly.
void pass()
{
    QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
}

void spin(int ms)
{
    QElapsedTimer t;
    t.start();
    while (t.elapsed() < ms)
        pass();
}

bool waitFor(const std::function<bool()> &condition, int ms)
{
    QElapsedTimer t;
    t.start();
    while (t.elapsed() < ms) {
        if (condition())
            return true;
        pass();
    }
    return condition();
}

QString env(const char *name) { return qEnvironmentVariable(name); }
QString runDir() { return env("TEST_RUN_DIR"); }

QByteArray readFile(const QString &path)
{
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

// panic.json as the status module reads it (statuskillswitch.cpp,
// PanicState::state): none | engaged | released | error.
QString recordState()
{
    const QByteArray data = readFile(runDir() + QStringLiteral("/panic.json"));
    if (data.isEmpty())
        return QStringLiteral("none");
    const QJsonObject o = QJsonDocument::fromJson(data).object();
    const bool engaged = o.value(QStringLiteral("engaged")).toBool();
    const bool loaded = o.value(QStringLiteral("loaded")).toBool();
    const bool error = o.value(QStringLiteral("error")).isString();
    if (error || engaged != loaded)
        return QStringLiteral("error");
    if (engaged)
        return QStringLiteral("engaged");
    return o.value(QStringLiteral("released")).toString().isEmpty() ? QStringLiteral("none") : QStringLiteral("released");
}

void writeRecord(const QByteArray &json)
{
    const QString tmp = runDir() + QStringLiteral("/.panic.test.tmp");
    QFile f(tmp);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
        qFatal("cannot write %s", qPrintable(tmp));
    f.write(json);
    f.close();
    QFile::remove(runDir() + QStringLiteral("/panic.json"));
    QFile::rename(tmp, runDir() + QStringLiteral("/panic.json"));
}

const QByteArray kReleased = "{\"version\": 1, \"engaged\": false, \"loaded\": false, \"error\": null, "
                             "\"since\": null, \"released\": \"2026-11-24T00:00:00Z\", \"warnings\": []}\n";
const QByteArray kEngaged = "{\"version\": 1, \"engaged\": true, \"loaded\": true, \"error\": null, "
                            "\"since\": \"2026-11-24T00:00:00Z\", \"warnings\": []}\n";

int count(const QString &path, const QByteArray &prefix)
{
    int n = 0;
    for (const QByteArray &line : readFile(path).split('\n'))
        n += line.startsWith(prefix);
    return n;
}
int startUnits() { return count(env("TEST_FAKE_LOG"), "call systemd StartUnit shadow-panic.service"); }

void fakeControl(const char *command)
{
    QFile fifo(env("TEST_FAKE_CTL"));
    if (fifo.open(QIODevice::WriteOnly)) {
        fifo.write(command);
        fifo.write("\n");
    }
    spin(300);
}

// fakebin/pkexec is a link to the stand-in the next release() runs.
void usePkexec(const QString &target)
{
    const QString link = env("TEST_FAKEBIN") + QStringLiteral("/pkexec");
    QFile::remove(link);
    if (!QFile::link(target, link))
        qFatal("cannot link %s", qPrintable(link));
}

QString writeScript(const QString &name, const QByteArray &body)
{
    const QString path = env("TEST_WORK") + QStringLiteral("/") + name;
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
        qFatal("cannot write %s", qPrintable(path));
    f.write("#!/bin/sh\n" + body);
    f.close();
    f.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner | QFile::ReadGroup | QFile::ExeGroup
                     | QFile::ReadOther | QFile::ExeOther);
    return path;
}

// Passes panic.json's state on to the core, as the panic banner does
// (StatusPanicBanner.qml). Without Panic::setServiceState (the shell before
// the fix) there is nothing to pass it to.
template<typename P>
bool feed(P &panic, const QString &state)
{
    if constexpr (requires { panic.setServiceState(state); }) {
        panic.setServiceState(state);
        return true;
    } else {
        return false;
    }
}

class Banner
{
public:
    explicit Banner(Panic &panic)
        : m_panic(panic)
    {
        QObject::connect(&m_poll, &QTimer::timeout, [this] { poll(); });
        m_poll.start(50);
        poll();
    }
    void poll()
    {
        const QString state = recordState();
        if (state != m_last || m_first) {
            m_first = false;
            m_last = state;
            m_canFeed = feed(m_panic, state);
        }
    }
    bool canFeed() const { return m_canFeed; }

private:
    Panic &m_panic;
    QTimer m_poll;
    QString m_last;
    bool m_first = true;
    bool m_canFeed = false;
};

// The longest time the event loop did not run, while it is measured.
class Heartbeat
{
public:
    Heartbeat()
    {
        QObject::connect(&m_timer, &QTimer::timeout, [this] {
            m_maxGap = std::max(m_maxGap, m_clock.restart());
        });
    }
    void start()
    {
        m_maxGap = 0;
        m_clock.start();
        m_timer.start(20);
    }
    qint64 stop()
    {
        m_timer.stop();
        return m_maxGap;
    }

private:
    QTimer m_timer;
    QElapsedTimer m_clock;
    qint64 m_maxGap = 0;
};

bool engaged(Panic &panic)
{
    return waitFor([&] { return panic.phase() == QLatin1String("started") && recordState() == QLatin1String("engaged"); }, 5000);
}

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    for (const char *name : {"DBUS_SYSTEM_BUS_ADDRESS", "TEST_RUN_DIR", "TEST_FAKE_LOG", "TEST_FAKE_CTL", "TEST_FAKEBIN", "TEST_WORK"}) {
        if (qEnvironmentVariableIsEmpty(name))
            qFatal("%s is not set (run inner.sh)", name);
    }
    const QString work = env("TEST_WORK");
    const QByteArray released = "d=\"$TEST_RUN_DIR\"; printf '%s\\n' '" + kReleased.trimmed()
        + "' > \"$d/.panic.sh.tmp\" && mv -f \"$d/.panic.sh.tmp\" \"$d/panic.json\"\n";
    // Waits for an administrator who never answers (pkexec before
    // authentication runs as the user, so the user can kill it).
    const QString waiting = writeScript(QStringLiteral("pkexec-waiting"),
        "echo $$ > \"$TEST_WORK/pkexec-waiting.pid\"\necho \"pkexec-waiting $*\" >> \"$TEST_WORK/pkexec.log\"\nsleep 30\n"
        + released + "echo released >> \"$TEST_WORK/pkexec.log\"\n");
    // Authenticated at once, releases, exits 0.
    const QString ok = writeScript(QStringLiteral("pkexec-ok"),
        "echo \"pkexec-ok $*\" >> \"$TEST_WORK/pkexec.log\"\n" + released);
    const QString rootPkexec = env("TEST_ROOT_PKEXEC");
    const QString rootLog = env("TEST_ROOT_LOG");

    Panic panic;
    Banner banner(panic);
    Heartbeat heartbeat;
    QStringList phases;
    QObject::connect(&panic, &Panic::changed, [&] {
        if (phases.isEmpty() || phases.last() != panic.phase())
            phases << panic.phase();
    });

    // ---- 1. released elsewhere ---------------------------------------------------------------
    std::printf("# released elsewhere (Control Center, sudo shadow-panic release)\n");
    panic.engage();
    bool done = engaged(panic);
    check("engaged: StartUnit answered and panic.json engaged", done, panic.phase() + QLatin1Char('/') + recordState());
    check("the shell passes panic.json on to the core (Panic::setServiceState)", banner.canFeed());
    spin(300);                                     // the banner has seen "engaged"
    writeRecord(kReleased);                        // what shadow-panic release publishes
    done = waitFor([&] { return panic.phase() == QLatin1String("idle"); }, 2000);
    check("released elsewhere: the request ends (phase idle)", done, panic.phase());
    check("released elsewhere: requestedAt cleared", panic.requestedAt() == 0);

    // ---- 2. an old "released" record does not end a new request -----------------------------
    std::printf("# an old \"released\" record\n");
    fakeControl("systemd silent");                // accepts the job, writes nothing
    panic.engage();
    done = waitFor([&] { return panic.phase() == QLatin1String("started"); }, 5000);
    check("silent service: StartUnit answered", done, panic.phase());
    spin(800);
    check("the old released record does not end the request", panic.phase() == QLatin1String("started"), panic.phase());
    writeRecord(kEngaged);                         // the service answers late
    spin(300);
    writeRecord(kReleased);                        // then a release elsewhere
    done = waitFor([&] { return panic.phase() == QLatin1String("idle"); }, 2000);
    check("engaged, then released: the request ends", done, panic.phase());
    fakeControl("systemd accept");

    // ---- 3. panic while the release waits for the administrator -----------------------------
    std::printf("# panic while the release waits for the administrator\n");
    panic.engage();
    done = engaged(panic);
    check("engaged", done, panic.phase() + QLatin1Char('/') + recordState());
    QFile::remove(work + QStringLiteral("/pkexec-waiting.pid"));
    usePkexec(waiting);
    panic.release(true);
    check("release started (waiting for the administrator)", panic.releasing());
    const bool waitingStarted = waitFor([&] { return QFile::exists(work + QStringLiteral("/pkexec-waiting.pid")); }, 3000);
    check("the waiting pkexec runs", waitingStarted);
    const pid_t waitingPid = readFile(work + QStringLiteral("/pkexec-waiting.pid")).trimmed().toInt();
    int units = startUnits();
    heartbeat.start();
    panic.engage();
    spin(2500);
    qint64 gap = heartbeat.stop();
    check("the shell did not freeze", gap < 1000, QStringLiteral("longest event-loop gap %1 ms").arg(gap));
    check("the waiting pkexec was killed", waitingPid > 0 && ::kill(waitingPid, 0) == -1 && errno == ESRCH);
    check("nothing was released", count(work + QStringLiteral("/pkexec.log"), "released") == 0);
    check("panic.json still engaged", recordState() == QLatin1String("engaged"), recordState());
    check("one StartUnit for the new panic (no second one needed)", startUnits() == units + 1,
          QStringLiteral("%1 new").arg(startUnits() - units));
    check("not releasing, phase started", !panic.releasing() && panic.phase() == QLatin1String("started"),
          panic.phase());

    // ---- 4. panic after the administrator authenticated ---------------------------------------
    std::printf("# panic after the administrator authenticated (the release runs as root)\n");
    if (rootPkexec.isEmpty()) {
        skip("authorised release", "no setuid-root stand-in (run inner.sh as root on a file system without nosuid)");
    } else {
        usePkexec(rootPkexec);
        const int rootRuns = count(rootLog, "pkexec");
        panic.release(true);
        check("release started", panic.releasing());
        check("the release runs as root",
              waitFor([&] { return count(rootLog, "pkexec") > rootRuns; }, 3000) && readFile(rootLog).contains("euid 0"));
        spin(200);
        units = startUnits();
        phases.clear();
        heartbeat.start();
        panic.engage();
        const int releases = count(rootLog, "released");
        // The release takes 1.5 s; then the service is started again and
        // writes engaged 0.4 s later.
        const bool releaseEnded = waitFor([&] { return count(rootLog, "released") > releases; }, 5000);
        spin(2500);
        gap = heartbeat.stop();
        check("the authorised release ran to its end", releaseEnded);
        check("the shell did not freeze", gap < 1000, QStringLiteral("longest event-loop gap %1 ms").arg(gap));
        check("the panic service was started again after the release",
              startUnits() == units + 2, QStringLiteral("%1 new StartUnit calls, expected 2").arg(startUnits() - units));
        check("the panic wins: panic.json engaged at the end", recordState() == QLatin1String("engaged"), recordState());
        check("the release's \"released\" record did not end the request", !phases.contains(QStringLiteral("idle")),
              phases.join(QLatin1Char(' ')));
        check("not releasing, phase started", !panic.releasing() && panic.phase() == QLatin1String("started"),
              panic.phase());
        check("no release error shown for the withdrawn release", panic.releaseError().isEmpty(), panic.releaseError());
    }

    // ---- 5. the banner's own release ----------------------------------------------------------
    std::printf("# the banner's own release\n");
    usePkexec(ok);
    bool finished = false, finishedOk = false;
    QObject::connect(&panic, &Panic::releaseFinished, [&](bool result) { finished = true; finishedOk = result; });
    panic.release(true);
    done = waitFor([&] { return finished; }, 5000);
    check("release finished ok", done && finishedOk, panic.releaseError());
    check("phase idle, panic.json released", panic.phase() == QLatin1String("idle") && recordState() == QLatin1String("released"),
          panic.phase() + QLatin1Char('/') + recordState());

    std::printf("FAILED: %d  SKIPPED: %d\n", failures, skipped);
    return failures == 0 ? 0 : 1;
}
