// SPDX-License-Identifier: GPL-3.0-or-later
//
// Self test of the status module's kill switch and panic state source
// (statuskillswitch.h): the parser of /run/shadow/killswitch.json and
// /run/shadow/panic.json with sound, missing, broken and contradicting
// documents, and the directory watcher with a real QFileSystemWatcher on a
// temporary directory (created late, files written in place, replaced
// atomically, removed). Needs no compositor, bus or root. Exit code 0 if
// every check holds.
#include "statuskillswitch.h"

#include <QCoreApplication>
#include <QDeadlineTimer>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonDocument>
#include <QSaveFile>
#include <QTemporaryDir>
#include <QTextStream>
#include <QThread>

#include <functional>

using namespace StatusModule;
using namespace StatusModule::NetGuard;

static int failures = 0;
static int checks = 0;

static void check(bool ok, const QString &what, const QString &detail = {})
{
    ++checks;
    if (ok)
        return;
    ++failures;
    QTextStream(stderr) << "FAIL: " << what << (detail.isEmpty() ? QString() : QStringLiteral(" — ") + detail) << '\n';
}

static void equal(const QString &actual, const QString &expected, const QString &what)
{
    check(actual == expected, what, QStringLiteral("got \"%1\", expected \"%2\"").arg(actual, expected));
}

static FileRead doc(const QByteArray &json)
{
    FileRead f;
    f.exists = true;
    f.data = json;
    return f;
}

static KillSwitchState ks(const QByteArray &json) { return parseKillSwitch(doc(json)); }
static PanicState panic(const QByteArray &json) { return parsePanic(doc(json)); }

// What shadow-killswitch's build_status() writes, field for field.
static QByteArray toolStatus(const char *mode, bool loaded, const char *error = nullptr)
{
    const bool active = qstrcmp(mode, "off") != 0;
    QJsonObject o{
        {QStringLiteral("mode"), QString::fromLatin1(mode)},
        {QStringLiteral("active"), active},
        {QStringLiteral("loaded"), loaded},
        {QStringLiteral("since"), active ? QJsonValue(QStringLiteral("2026-11-23T10:00:00Z")) : QJsonValue()},
        {QStringLiteral("lan"), false},
        {QStringLiteral("endpoints"), qstrcmp(mode, "vpn") == 0 ? 2 : 0},
        {QStringLiteral("error"), error ? QJsonValue(QString::fromLatin1(error)) : QJsonValue()},
        {QStringLiteral("escape_hatch"), false},
        {QStringLiteral("version"), 1},
        {QStringLiteral("persistent_mode"), QString::fromLatin1(mode)},
        {QStringLiteral("runtime_override"), false},
        {QStringLiteral("tunnel_ifaces"), 2},
        {QStringLiteral("skipped_hostnames"), 0},
        {QStringLiteral("dropped_packets"), loaded && active ? QJsonValue(17) : QJsonValue()},
        {QStringLiteral("updated"), QStringLiteral("2026-11-23T10:00:01Z")},
    };
    return QJsonDocument(o).toJson();
}

static void testKillSwitchParser()
{
    // Missing: nothing known, nothing claimed, nothing shown as an error.
    KillSwitchState s = parseKillSwitch(FileRead{});
    equal(s.state(), QStringLiteral("unknown"), QStringLiteral("missing file: state"));
    check(!s.isProtected() && !s.hasError(), QStringLiteral("missing file: neither protected nor an error"));

    // Broken documents are errors, never "protected".
    const QList<QPair<QByteArray, QString>> broken{
        {QByteArray(), QStringLiteral("is empty")},
        {QByteArray("  \n"), QStringLiteral("is empty")},
        {QByteArray("{\"mode\": \"tor\", \"active\": true, \"loaded\": tru"), QStringLiteral("not valid JSON")},
        {QByteArray("[\"tor\"]"), QStringLiteral("not a JSON object")},
        // Qt refuses a bare value at the top level as invalid JSON; either way an error.
        {QByteArray("\"on\""), QStringLiteral("JSON")},
        {QByteArray("{}"), QStringLiteral("names no mode")},
        {QByteArray("{\"mode\": \"bogus\", \"active\": true, \"loaded\": true}"), QStringLiteral("unknown mode")},
        {QByteArray("{\"mode\": 3, \"loaded\": true}"), QStringLiteral("names no mode")},
        {QByteArray("{\"mode\": \"tor\", \"active\": true, \"loaded\": \"yes\"}"), QStringLiteral("\"loaded\" is not true or false")},
        {QByteArray("{\"mode\": \"off\", \"active\": true, \"loaded\": false}"), QStringLiteral("contradicts itself")},
        {QByteArray("{\"mode\": \"tor\", \"active\": false, \"loaded\": true}"), QStringLiteral("contradicts itself")},
    };
    for (const auto &[json, expected] : broken) {
        s = ks(json);
        const QString name = QStringLiteral("broken document %1").arg(QString::fromUtf8(json.left(40)));
        equal(s.state(), QStringLiteral("error"), name + QStringLiteral(": state"));
        check(!s.isProtected(), name + QStringLiteral(": not protected"));
        check(s.errorText().contains(expected), name + QStringLiteral(": message"), s.errorText());
    }
    FileRead unreadable;
    unreadable.exists = true;
    unreadable.error = QStringLiteral("Permission denied");
    s = parseKillSwitch(unreadable);
    equal(s.state(), QStringLiteral("error"), QStringLiteral("unreadable file: state"));
    check(s.errorText().contains(QStringLiteral("Permission denied")), QStringLiteral("unreadable file: reason"), s.errorText());

    // Each mode, as the tool writes it.
    s = ks(toolStatus("off", false));
    equal(s.state(), QStringLiteral("off"), QStringLiteral("off: state"));
    check(!s.isProtected() && !s.hasError() && !s.active, QStringLiteral("off: not protected, no error"));
    check(s.errorText().isEmpty(), QStringLiteral("off: no message"));
    for (const char *mode : {"tor", "vpn", "tor-or-vpn"}) {
        const QString m = QString::fromLatin1(mode);
        s = ks(toolStatus(mode, true));
        equal(s.state(), QStringLiteral("on"), m + QStringLiteral(": state"));
        equal(s.mode, m, m + QStringLiteral(": mode"));
        check(s.isProtected(), m + QStringLiteral(": protected"));
        check(s.droppedPackets == 17, m + QStringLiteral(": dropped packets"));
        check(s.since == QLatin1String("2026-11-23T10:00:00Z"), m + QStringLiteral(": since"));
        // The same mode without its table: an error, not protection.
        s = ks(toolStatus(mode, false));
        equal(s.state(), QStringLiteral("error"), m + QStringLiteral(" not loaded: state"));
        check(!s.isProtected(), m + QStringLiteral(" not loaded: not protected"));
        check(s.errorText().contains(QStringLiteral("NOT protected")), m + QStringLiteral(" not loaded: message"), s.errorText());
    }
    check(ks(toolStatus("vpn", true)).endpoints == 2, QStringLiteral("vpn: endpoints"));

    // The tool's error wins, even with the table loaded (for example a broken
    // configuration file while the kill switch fails closed).
    s = ks(toolStatus("tor", true, "the configuration cannot be used"));
    equal(s.state(), QStringLiteral("error"), QStringLiteral("error set: state"));
    check(!s.isProtected(), QStringLiteral("error set: not protected"));
    equal(s.errorText(), QStringLiteral("the configuration cannot be used"), QStringLiteral("error set: message"));
    // A change that failed while the previous mode stayed in force is "change_error"
    // (and a warning), not "error": the mode in force protects.
    s = ks("{\"mode\": \"tor\", \"active\": true, \"loaded\": true, \"error\": null, "
           "\"change_error\": \"could not change to mode vpn: nft failed\", "
           "\"warnings\": [\"could not change to mode vpn: nft failed\"]}");
    equal(s.state(), QStringLiteral("on"), QStringLiteral("failed change, old mode in force: state"));
    check(s.isProtected(), QStringLiteral("failed change, old mode in force: protected"));
    s = ks(toolStatus("off", false, "the configuration is broken"));
    equal(s.state(), QStringLiteral("error"), QStringLiteral("error with mode off: state"));
    s = ks("{\"mode\": \"tor\", \"active\": true, \"loaded\": true, \"error\": {\"code\": 3}}");
    equal(s.state(), QStringLiteral("error"), QStringLiteral("error that is not text: state"));
    check(s.errorText().contains(QStringLiteral("code")), QStringLiteral("error that is not text: shown"), s.errorText());
    s = ks("{\"mode\": \"tor\", \"active\": true, \"loaded\": true, \"error\": \"  \"}");
    equal(s.state(), QStringLiteral("error"), QStringLiteral("blank error text: still an error"));

    // Off, but the table is still in the kernel.
    s = ks(toolStatus("off", true));
    equal(s.state(), QStringLiteral("error"), QStringLiteral("off but loaded: state"));
    check(s.errorText().contains(QStringLiteral("still loaded")), QStringLiteral("off but loaded: message"), s.errorText());

    // "active" may be missing: derived from the mode.
    s = ks("{\"mode\": \"vpn\", \"loaded\": true}");
    check(s.isProtected() && s.active, QStringLiteral("active derived from the mode"));
    // "loaded" missing: never assumed.
    s = ks("{\"mode\": \"tor\", \"active\": true}");
    check(!s.isProtected() && s.hasError(), QStringLiteral("missing loaded: not protected, an error"));

    // Escape hatch: off in this boot, something else saved.
    s = ks("{\"mode\": \"off\", \"active\": false, \"loaded\": false, \"escape_hatch\": true, \"persistent_mode\": \"tor\"}");
    equal(s.state(), QStringLiteral("bypassed"), QStringLiteral("escape hatch: state"));
    check(s.bypassed() && !s.hasError(), QStringLiteral("escape hatch: bypassed, no error"));
    s = ks("{\"mode\": \"off\", \"active\": false, \"loaded\": false, \"escape_hatch\": true, \"persistent_mode\": \"off\"}");
    equal(s.state(), QStringLiteral("off"), QStringLiteral("escape hatch with nothing saved: off"));

    // Unknown fields are ignored; numbers that are not counts are ignored.
    s = ks("{\"mode\": \"tor\", \"active\": true, \"loaded\": true, \"future\": [1,2], \"dropped_packets\": -4, \"endpoints\": 1.5}");
    check(s.isProtected() && s.droppedPackets == -1 && s.endpoints == 0, QStringLiteral("unknown fields and bad counts"));
}

static void testPanicParser()
{
    PanicState p = parsePanic(FileRead{});
    equal(p.state(), QStringLiteral("none"), QStringLiteral("panic missing: state"));
    check(!p.isCut() && !p.hasError(), QStringLiteral("panic missing: not cut, no error"));
    check(p.clearClipboard && p.lockSession, QStringLiteral("panic missing: defaults are true"));

    p = panic("{\"engaged\": true, \"loaded\": true, \"error\": null, \"since\": \"2026-11-23T10:00:00Z\", "
              "\"warnings\": [\"radio not blocked: x\"], \"clear_clipboard\": false, \"lock_session\": true, "
              "\"steps\": {\"table\": true}, \"dropped_packets\": 3}");
    equal(p.state(), QStringLiteral("engaged"), QStringLiteral("panic engaged: state"));
    check(p.isCut(), QStringLiteral("panic engaged: cut"));
    check(!p.clearClipboard && p.lockSession, QStringLiteral("panic engaged: clipboard and lock flags"));
    check(p.warnings == QStringList{QStringLiteral("radio not blocked: x")}, QStringLiteral("panic engaged: warnings"));
    check(p.since == QLatin1String("2026-11-23T10:00:00Z"), QStringLiteral("panic engaged: since"));

    p = panic("{\"engaged\": true, \"loaded\": false, \"error\": null}");
    equal(p.state(), QStringLiteral("error"), QStringLiteral("panic engaged, not loaded: state"));
    check(!p.isCut() && p.errorText().contains(QStringLiteral("NOT cut off")), QStringLiteral("panic engaged, not loaded: message"),
          p.errorText());
    p = panic("{\"engaged\": true, \"loaded\": null}");
    equal(p.state(), QStringLiteral("error"), QStringLiteral("panic engaged, loaded unknown: state"));

    p = panic("{\"engaged\": true, \"loaded\": true, \"error\": \"nft failed\"}");
    equal(p.state(), QStringLiteral("error"), QStringLiteral("panic with error: state"));
    equal(p.errorText(), QStringLiteral("nft failed"), QStringLiteral("panic with error: message"));
    check(!p.isCut(), QStringLiteral("panic with error: not cut"));

    p = panic("{\"engaged\": false, \"loaded\": false, \"error\": null, \"since\": null, \"released\": \"2026-11-23T11:00:00Z\"}");
    equal(p.state(), QStringLiteral("released"), QStringLiteral("panic released: state"));
    check(!p.isCut() && !p.hasError(), QStringLiteral("panic released: not cut, no error"));

    p = panic("{\"engaged\": false, \"loaded\": true, \"error\": null, \"released\": \"2026-11-23T11:00:00Z\"}");
    equal(p.state(), QStringLiteral("error"), QStringLiteral("panic released, table still loaded: state"));

    p = panic("{\"engaged\": false, \"loaded\": null}");
    equal(p.state(), QStringLiteral("none"), QStringLiteral("panic never engaged, not checked: none"));

    for (const QByteArray &json : {QByteArray("{\"engaged\": tr"), QByteArray("[]"), QByteArray("{\"engaged\": \"yes\"}")}) {
        p = panic(json);
        equal(p.state(), QStringLiteral("error"), QStringLiteral("panic broken %1: state").arg(QString::fromUtf8(json)));
        check(!p.isCut(), QStringLiteral("panic broken %1: not cut").arg(QString::fromUtf8(json)));
    }
    p = panic("{\"engaged\": true, \"loaded\": true, \"clear_clipboard\": null, \"lock_session\": null}");
    check(p.clearClipboard && p.lockSession && p.isCut(), QStringLiteral("panic null flags mean true"));
}

static void testFileReading(const QString &base)
{
    FileRead f = readStateFile(base + QStringLiteral("/nothing-here.json"));
    check(!f.exists && f.error.isEmpty(), QStringLiteral("read: missing file"));

    QDir().mkpath(base + QStringLiteral("/a-directory.json"));
    f = readStateFile(base + QStringLiteral("/a-directory.json"));
    check(f.exists && !f.error.isEmpty(), QStringLiteral("read: a directory is an error"), f.error);
    equal(parseKillSwitch(f).state(), QStringLiteral("error"), QStringLiteral("read: a directory: state"));

    QFile big(base + QStringLiteral("/big.json"));
    check(big.open(QIODevice::WriteOnly), QStringLiteral("read: create big file"));
    big.write(QByteArray(kMaxStateFileSize + 10, ' '));
    big.close();
    f = readStateFile(big.fileName());
    check(f.exists && f.error.contains(QStringLiteral("larger")), QStringLiteral("read: oversized file refused"), f.error);

    QFile::link(base + QStringLiteral("/gone.json"), base + QStringLiteral("/dangling.json"));
    f = readStateFile(base + QStringLiteral("/dangling.json"));
    check(f.exists && !f.error.isEmpty(), QStringLiteral("read: dangling link is an error"), f.error);
}

// ---- the watcher ---------------------------------------------------------------------------------

static bool waitFor(const std::function<bool()> &condition, int ms = 3000)
{
    QDeadlineTimer deadline(ms);
    while (!deadline.hasExpired()) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        if (condition())
            return true;
        QThread::msleep(5);
    }
    QCoreApplication::processEvents();
    return condition();
}

static bool writeAtomically(const QString &path, const QByteArray &data)
{
    QSaveFile f(path);       // temporary file plus rename, like the tools' os.replace()
    if (!f.open(QIODevice::WriteOnly))
        return false;
    f.write(data);
    return f.commit();
}

static bool writeInPlace(const QString &path, const QByteArray &data)
{
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return false;
    const bool ok = f.write(data) == data.size();
    f.close();
    return ok;
}

static void testWatcher(const QString &base)
{
    // The directory does not exist yet (as before the first tool run).
    const QString dir = base + QStringLiteral("/run/shadow");
    qputenv("SHADOW_STATUS_RUN_DIR", dir.toLocal8Bit());
    KillSwitchBackend backend;
    int changes = 0;
    QObject::connect(&backend, &KillSwitchBackend::changed, [&changes] { ++changes; });

    check(backend.isReady(), QStringLiteral("watcher: ready at once"));
    check(!backend.isAvailable(), QStringLiteral("watcher: no file, not available"));
    equal(backend.state(), QStringLiteral("unknown"), QStringLiteral("watcher: no directory: state"));
    check(backend.watching(), QStringLiteral("watcher: watches a parent while the directory is missing"));
    check(!backend.indicatorVisible(), QStringLiteral("watcher: no file, no indicator"));
    equal(backend.panicState(), QStringLiteral("none"), QStringLiteral("watcher: no panic file"));

    check(QDir().mkpath(dir), QStringLiteral("watcher: create the directory"));
    check(writeAtomically(dir + QStringLiteral("/killswitch.json"), toolStatus("tor", true)),
          QStringLiteral("watcher: write killswitch.json"));
    check(waitFor([&] { return backend.state() == QLatin1String("on"); }),
          QStringLiteral("watcher: directory and file created later are noticed"), backend.state());
    check(backend.isAvailable() && backend.isProtected() && backend.indicatorVisible(),
          QStringLiteral("watcher: protected, indicator shown"));
    equal(backend.mode(), QStringLiteral("tor"), QStringLiteral("watcher: mode"));

    // Atomic replacement (rename over the watched file).
    int before = changes;
    check(writeAtomically(dir + QStringLiteral("/killswitch.json"), toolStatus("vpn", true, "a NetworkManager sync failed")),
          QStringLiteral("watcher: replace killswitch.json"));
    check(waitFor([&] { return backend.state() == QLatin1String("error"); }),
          QStringLiteral("watcher: atomic replacement is noticed"), backend.state());
    check(changes > before, QStringLiteral("watcher: changed() emitted"));
    equal(backend.errorText(), QStringLiteral("a NetworkManager sync failed"), QStringLiteral("watcher: error text"));
    check(backend.hasError() && backend.indicatorVisible() && !backend.isProtected(),
          QStringLiteral("watcher: error state shown, not protected"));

    // A second replacement: the new file must be watched again.
    check(writeAtomically(dir + QStringLiteral("/killswitch.json"), toolStatus("tor-or-vpn", true)),
          QStringLiteral("watcher: replace again"));
    check(waitFor([&] { return backend.mode() == QLatin1String("tor-or-vpn") && backend.isProtected(); }),
          QStringLiteral("watcher: second replacement is noticed"), backend.mode());

    // Written in place (no rename): only the file watch sees this.
    check(writeInPlace(dir + QStringLiteral("/killswitch.json"), toolStatus("tor", false)),
          QStringLiteral("watcher: write in place"));
    check(waitFor([&] { return backend.mode() == QLatin1String("tor") && backend.hasError(); }),
          QStringLiteral("watcher: in-place write is noticed"), backend.state());

    // Broken mid-way (a truncated write) and repaired.
    check(writeInPlace(dir + QStringLiteral("/killswitch.json"), QByteArray("{\"mode\": \"tor\", ")),
          QStringLiteral("watcher: write a broken file"));
    check(waitFor([&] { return backend.state() == QLatin1String("error") && !backend.isAvailable(); }),
          QStringLiteral("watcher: broken file is an error and not available"), backend.unavailableReason());
    check(writeAtomically(dir + QStringLiteral("/killswitch.json"), toolStatus("off", false)),
          QStringLiteral("watcher: repair"));
    check(waitFor([&] { return backend.state() == QLatin1String("off"); }), QStringLiteral("watcher: repaired"), backend.state());
    check(!backend.indicatorVisible(), QStringLiteral("watcher: off, no indicator"));

    // No change, no signal.
    before = changes;
    backend.refresh();
    check(changes == before, QStringLiteral("watcher: refresh without a change emits nothing"));

    // Panic appears, then is released.
    check(writeAtomically(dir + QStringLiteral("/panic.json"),
                          "{\"engaged\": true, \"loaded\": true, \"error\": null, \"since\": \"2026-11-23T10:05:00Z\"}"),
          QStringLiteral("watcher: write panic.json"));
    check(waitFor([&] { return backend.panicState() == QLatin1String("engaged"); }),
          QStringLiteral("watcher: panic noticed"), backend.panicState());
    check(backend.panicCut() && backend.panicEngaged(), QStringLiteral("watcher: panic cut"));
    check(writeAtomically(dir + QStringLiteral("/panic.json"),
                          "{\"engaged\": false, \"loaded\": false, \"error\": null, \"released\": \"2026-11-23T10:09:00Z\"}"),
          QStringLiteral("watcher: release panic.json"));
    check(waitFor([&] { return backend.panicState() == QLatin1String("released"); }),
          QStringLiteral("watcher: release noticed"), backend.panicState());
    check(!backend.panicCut(), QStringLiteral("watcher: released, not cut"));

    // Removed files.
    QFile::remove(dir + QStringLiteral("/killswitch.json"));
    QFile::remove(dir + QStringLiteral("/panic.json"));
    check(waitFor([&] { return backend.state() == QLatin1String("unknown") && backend.panicState() == QLatin1String("none"); }),
          QStringLiteral("watcher: removal noticed"), backend.state() + QLatin1Char('/') + backend.panicState());

    // The directory itself goes away and comes back.
    check(QDir(dir).removeRecursively(), QStringLiteral("watcher: remove the directory"));
    waitFor([] { return false; }, 300);
    check(backend.watching(), QStringLiteral("watcher: still watching (the parent) without the directory"));
    QDir().mkpath(dir);
    check(writeAtomically(dir + QStringLiteral("/killswitch.json"), toolStatus("vpn", true)),
          QStringLiteral("watcher: recreate"));
    check(waitFor([&] { return backend.state() == QLatin1String("on") && backend.mode() == QLatin1String("vpn"); }),
          QStringLiteral("watcher: recreated directory noticed"), backend.state());

    // The --dump document.
    const QJsonObject json = backend.toJson();
    check(json.value(QStringLiteral("available")).toBool() && json.value(QStringLiteral("mode")).toString() == QLatin1String("vpn")
              && json.value(QStringLiteral("protected")).toBool() && json.value(QStringLiteral("panic")).isObject()
              && json.value(QStringLiteral("directory")).toString() == dir,
          QStringLiteral("dump document"), QString::fromUtf8(QJsonDocument(json).toJson(QJsonDocument::Compact)));
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QTemporaryDir tmp;
    if (!tmp.isValid()) {
        QTextStream(stderr) << "cannot create a temporary directory\n";
        return 2;
    }
    QElapsedTimer timer;
    timer.start();
    testKillSwitchParser();
    testPanicParser();
    testFileReading(tmp.path());
    testWatcher(tmp.path());
    QTextStream(stdout) << checks << " checks, " << failures << " failed (" << timer.elapsed() << " ms)\n";
    return failures == 0 ? 0 : 1;
}
