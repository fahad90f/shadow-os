// SPDX-License-Identifier: GPL-3.0-or-later
#include "assistant.h"
#include "assistantconfig.h"
#include "speechtext.h"
#include "toolregistry.h"
#include "voicecontroller.h"
#include "voiceengine.h"
#include "voicesession.h"

#include <shadowapp.h>

#include <QCommandLineOption>
#include <QCoreApplication>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>

#include <cstdio>

namespace {

// The headless voice test modes open no window: they must neither hand their
// arguments to a running assistant nor become the instance that a later
// launch hands over to.
bool isHeadlessVoiceMode(int argc, char **argv)
{
    for (int i = 1; i < argc; ++i) {
        const QByteArray a(argv[i]);
        for (const char *opt : {"--voice-session", "--voice-selftest", "--voice-speech-text"})
            if (a == opt || a.startsWith(QByteArray(opt) + '='))
                return true;
    }
    return false;
}

void printJson(const QJsonObject &o)
{
    const QByteArray out = QJsonDocument(o).toJson(QJsonDocument::Indented);
    std::fwrite(out.constData(), 1, size_t(out.size()), stdout);
    std::fflush(stdout);
}

// --voice-selftest: the helper's own end-to-end check (TTS → STT, no audio
// devices). Its JSON goes straight to our stdout, its log to our stderr.
int runVoiceSelftest()
{
    QString why;
    const QString helper = VoiceEngine::locateHelper(&why);
    if (helper.isEmpty()) {
        printJson({{QStringLiteral("event"), QStringLiteral("selftest")}, {QStringLiteral("ok"), false},
                   {QStringLiteral("error"), why}});
        return 3;
    }
    QProcess p;
    p.setProcessChannelMode(QProcess::ForwardedChannels);
    p.start(helper, {QStringLiteral("--selftest")});
    if (!p.waitForStarted(10000)) {
        printJson({{QStringLiteral("event"), QStringLiteral("selftest")}, {QStringLiteral("ok"), false},
                   {QStringLiteral("error"), QStringLiteral("%1 could not be started: %2").arg(helper, p.errorString())}});
        return 3;
    }
    p.closeWriteChannel();
    // Loading the models and one synthesis + recognition round take seconds;
    // nothing else runs in this mode, so waiting here blocks no UI.
    if (!p.waitForFinished(10 * 60 * 1000)) {
        p.kill();
        p.waitForFinished(2000);
        printJson({{QStringLiteral("event"), QStringLiteral("selftest")}, {QStringLiteral("ok"), false},
                   {QStringLiteral("error"), QStringLiteral("the self-test did not finish within 10 minutes")}});
        return 4;
    }
    return p.exitStatus() == QProcess::NormalExit ? p.exitCode() : 4;
}

// --voice-speech-text FILE|-: how a Markdown reply is spoken (tests of the
// text-for-speech rules), whole and as if it had been streamed in small chunks.
int printSpeechText(const QString &path)
{
    QFile f(path);
    const bool ok = path == QLatin1String("-") ? f.open(stdin, QIODevice::ReadOnly) : f.open(QIODevice::ReadOnly);
    if (!ok) {
        std::fprintf(stderr, "cannot read %s\n", qPrintable(path));
        return 2;
    }
    const QString text = QString::fromUtf8(f.readAll());
    SentenceStream whole;
    QStringList sentences = whole.push(text);
    sentences += whole.flush();
    SentenceStream streamed;
    QJsonArray steps;
    QStringList chunked;
    for (qsizetype i = 0; i < text.size(); i += 7) {
        const QStringList got = streamed.push(text.mid(i, 7));
        chunked += got;
        if (!got.isEmpty())
            steps.append(QJsonObject{{QStringLiteral("at"), int(qMin(i + 7, text.size()))},
                                     {QStringLiteral("sentences"), QJsonArray::fromStringList(got)}});
    }
    const QStringList tail = streamed.flush();
    chunked += tail;
    printJson({{QStringLiteral("speech"), SpeechText::forSpeech(text)},
               {QStringLiteral("sentences"), QJsonArray::fromStringList(sentences)},
               {QStringLiteral("streamed"), QJsonArray::fromStringList(chunked)},
               {QStringLiteral("flushed"), QJsonArray::fromStringList(tail)},
               {QStringLiteral("steps"), steps}});
    return 0;
}

} // namespace

int main(int argc, char **argv)
{
    const bool headlessVoice = isHeadlessVoiceMode(argc, argv);
    if (headlessVoice)
        qputenv("QT_QPA_PLATFORM", "offscreen"); // no display server needed (CI, SSH)

    Shadow::App app(argc, argv, {
        QStringLiteral("org.shadow.assistant"),
        QStringLiteral("Shadow Assistant"),
        QStringLiteral("org.shadow.assistant"),
        QStringLiteral("A personal AI agent that can chat and carry out tasks on this computer"),
        !headlessVoice, // single instance — except the headless voice test modes
    });
    app.setDumpTopics({QStringLiteral("config"), QStringLiteral("models"), QStringLiteral("tools"),
                       QStringLiteral("voice")});

    // Headless voice test entry points (spec voice v2 §3).
    QCommandLineOption selftestOpt(QStringLiteral("voice-selftest"),
        QStringLiteral("Run the voice engine's self-test (shadow-voiced --selftest: speech synthesis, then "
                       "recognition of it, no audio devices) and print its JSON."));
    QCommandLineOption sessionOpt(QStringLiteral("voice-session"),
        QStringLiteral("Run the whole voice loop without a window: the voice engine hears <wav> instead of the "
                       "microphone, the assistant answers (SHADOW_ASSISTANT_BASE_URL), and every step is printed "
                       "as one JSON line."),
        QStringLiteral("wav"));
    QCommandLineOption outOpt(QStringLiteral("voice-out"),
        QStringLiteral("With --voice-session: write the spoken replies here (one WAV each) instead of playing them."),
        QStringLiteral("dir"));
    QCommandLineOption timeoutOpt(QStringLiteral("voice-timeout"),
        QStringLiteral("With --voice-session: stop after <seconds> (default 120)."), QStringLiteral("seconds"));
    QCommandLineOption actionsOpt(QStringLiteral("voice-actions"),
        QStringLiteral("With --voice-session (tests): \"T:ACTION[:ARG];...\" runs start, finish, toggle, cancel, "
                       "stop-agent, stop-speaking, mute, unmute, refresh, speak:TEXT, preview:VOICE, send:TEXT, "
                       "install or cancel-install T seconds after the voice engine settled."),
        QStringLiteral("list"));
    QCommandLineOption speechTextOpt(QStringLiteral("voice-speech-text"),
        QStringLiteral("Print how a Markdown text (<file>, or - for stdin) would be spoken, as JSON."),
        QStringLiteral("file"));
    // Started at login (org.shadow.assistant-listen.desktop): no window until
    // the user says "Shadow" or opens the assistant; exits at once when the
    // wake word is switched off.
    QCommandLineOption backgroundOpt(QStringLiteral("background"),
        QStringLiteral("Start without a window and listen for the wake word."));
    app.parser().addOptions({selftestOpt, sessionOpt, outOpt, timeoutOpt, actionsOpt, speechTextOpt, backgroundOpt});
    if (!app.processArguments())
        return 0;

    if (app.parser().isSet(selftestOpt))
        return runVoiceSelftest();
    if (app.parser().isSet(speechTextOpt))
        return printSpeechText(app.parser().value(speechTextOpt));
    if (app.parser().isSet(sessionOpt)) {
        VoiceSession::Options o;
        o.wav = app.parser().value(sessionOpt);
        o.outDir = app.parser().value(outOpt);
        o.actions = app.parser().value(actionsOpt);
        if (app.parser().isSet(timeoutOpt)) {
            bool ok = false;
            o.timeoutSecs = app.parser().value(timeoutOpt).toDouble(&ok);
            if (!ok || o.timeoutSecs <= 0) {
                std::fprintf(stderr, "--voice-timeout needs a number of seconds\n");
                return 2;
            }
        }
        VoiceSession session(o);
        return session.run();
    }

    if (app.parser().isSet(backgroundOpt)) {
        AssistantConfig config;
        if (!config.wakeEnabled())
            return 0;   // nothing to listen for
        // Started at every login: stay resident only when the wake word can
        // actually be heard (helper, voice data and a capture tool present).
        // Opening the assistant normally still works and explains what is missing.
        QString why;
        const QString helper = VoiceEngine::locateHelper(&why);
        int probeExit = -1;
        QString probeError;
        const QJsonObject probe = helper.isEmpty() ? QJsonObject()
                                                   : VoiceEngine::probeSync(helper, &probeExit, &probeError);
        if (probeExit != 0) {
            const QString reason = !why.isEmpty() ? why
                : !probe.value(QStringLiteral("reason")).toString().isEmpty() ? probe.value(QStringLiteral("reason")).toString()
                                                                                : probeError;
            std::fprintf(stderr, "shadow-assistant --background: the wake word cannot be heard here (%s); not staying "
                                 "in the background.\n", qPrintable(reason));
            return 0;
        }
    }

    if (!app.dumpTopic().isEmpty()) {
        AssistantConfig config;
        const QString topic = app.dumpTopic();
        if (topic == QLatin1String("config")) {
            // Never includes the key itself, only whether one is configured.
            return app.finishDump(config.toJson());
        }
        if (topic == QLatin1String("models")) {
            return app.finishDump(QJsonObject{
                {QStringLiteral("brain"), config.brainModel()},
                {QStringLiteral("planner"), config.plannerModel()},
                {QStringLiteral("fast"), config.fastModel()},
                {QStringLiteral("omni"), config.omniModel()},
                {QStringLiteral("baseUrl"), config.baseUrl()},
            });
        }
        if (topic == QLatin1String("tools")) {
            ToolRegistry registry(&config);
            QJsonArray tools;
            const QJsonArray api = registry.apiTools();
            for (const QJsonValue &v : api) {
                const QJsonObject fn = v.toObject().value(QStringLiteral("function")).toObject();
                const QString name = fn.value(QStringLiteral("name")).toString();
                tools.append(QJsonObject{
                    {QStringLiteral("name"), name},
                    {QStringLiteral("description"), fn.value(QStringLiteral("description")).toString()},
                    {QStringLiteral("changesSystem"), registry.toolChangesSystem(name)},
                    {QStringLiteral("risky"), registry.toolRisky(name)},
                });
            }
            return app.finishDump(QJsonObject{
                {QStringLiteral("enabled"), tools},
                {QStringLiteral("autonomy"), config.autonomy()},
            });
        }
        if (topic == QLatin1String("voice")) {
            // Probe only: the helper is asked what it has, never started, so
            // the microphone is not opened by a diagnostic.
            return app.finishDump(VoiceController::probeDiagnostics(&config));
        }
        return app.dumpError(QStringLiteral("unknown dump topic '%1'").arg(topic));
    }

    return app.run(QStringLiteral("org.shadow.assistant"));
}
