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
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QTimer>

#include <cstdio>

namespace {

// The headless voice test modes open no window: they must neither hand their
// arguments to a running assistant nor become the instance that a later
// launch hands over to.
bool isHeadlessVoiceMode(int argc, char **argv)
{
    for (int i = 1; i < argc; ++i) {
        const QByteArray a(argv[i]);
        for (const char *opt : {"--voice-session", "--voice-selftest", "--voice-speech-text", "--ask",
                                "--set-api-key", "--remove-api-key"})
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

void printJsonLine(const QJsonObject &o)
{
    const QByteArray out = QJsonDocument(o).toJson(QJsonDocument::Compact) + '\n';
    std::fwrite(out.constData(), 1, size_t(out.size()), stdout);
    std::fflush(stdout);
}

// --ask: requests without a window. Each --ask is one turn of the same
// conversation. The model team's progress is printed as it changes and the
// outcome of every turn as a "turn" line ("done" for the last), one JSON
// object per line. 'approve' answers the "may I do this?" questions: "yes",
// "no", or "" to stop there and report awaiting = true.
struct AskOptions
{
    QStringList texts;
    double timeoutSecs = 180;
    bool thinkHard = false;
    QString approve;
    double stopAfterSecs = 0; // > 0: press Stop this long after each request was sent
};

int runAsk(const AskOptions &options)
{
    Assistant agent;
    // With the list read first, the team is the one the service really offers.
    if (agent.config()->nvidia() && !agent.config()->localMode())
        agent.catalog()->refreshSync(15000);
    agent.setThinkHard(options.thinkHard);

    QElapsedTimer clock;
    clock.start();
    QString error, lastTeam;
    QObject::connect(&agent, &Assistant::errorMessage, &agent, [&error](const QString &m) { error = m; });
    auto teamJson = [&agent]() {
        QJsonArray members;
        for (const QVariant &v : agent.team()) {
            const QVariantMap m = v.toMap();
            members.append(QJsonObject{{QStringLiteral("id"), m.value(QStringLiteral("id")).toString()},
                                       {QStringLiteral("role"), m.value(QStringLiteral("role")).toString()},
                                       {QStringLiteral("state"), m.value(QStringLiteral("state")).toString()},
                                       {QStringLiteral("seconds"), m.value(QStringLiteral("seconds")).toDouble()}});
        }
        return members;
    };
    QObject::connect(&agent, &Assistant::teamChanged, &agent, [&]() {
        const QJsonArray members = teamJson();
        const QString now = QString::fromUtf8(QJsonDocument(members).toJson(QJsonDocument::Compact))
            + (agent.teamGathering() ? QLatin1String("g") : QLatin1String(""));
        if (now == lastTeam || members.isEmpty())
            return;
        lastTeam = now;
        printJsonLine({{QStringLiteral("event"), QStringLiteral("team")},
                       {QStringLiteral("t"), clock.elapsed() / 1000.0},
                       {QStringLiteral("kind"), agent.teamKind()},
                       {QStringLiteral("gathering"), agent.teamGathering()},
                       {QStringLiteral("members"), members}});
    });
    QObject::connect(&agent, &Assistant::confirmationNeeded, &agent,
                     [&](const QString &summary, const QString &preview, const QString &) {
        printJsonLine({{QStringLiteral("event"), QStringLiteral("confirm")},
                       {QStringLiteral("t"), clock.elapsed() / 1000.0},
                       {QStringLiteral("summary"), summary},
                       {QStringLiteral("preview"), preview},
                       {QStringLiteral("answer"), options.approve}});
    });

    QEventLoop loop;
    auto settled = [&agent]() {
        return !agent.busy() && agent.phase() != QLatin1String("thinking") && agent.phase() != QLatin1String("streaming");
    };
    auto onSettled = [&]() {
        if (!settled())
            return;
        if (agent.hasPending() && !options.approve.isEmpty()) {
            agent.confirmAll(options.approve == QLatin1String("yes"));
            return; // the turn goes on; phaseChanged comes again
        }
        loop.quit();
    };
    QObject::connect(&agent, &Assistant::phaseChanged, &loop, [&]() {
        if (settled())
            QTimer::singleShot(80, &loop, onSettled);
    });
    bool timedOut = false;
    QTimer::singleShot(int(options.timeoutSecs * 1000), &loop, [&]() { timedOut = true; loop.quit(); });

    const bool haveKey = agent.config()->apiKeyConfigured() || agent.config()->localMode();
    for (qsizetype turn = 0; turn < options.texts.size(); ++turn) {
        error.clear();
        const int rowsBefore = agent.messages()->rowCount();
        if (!haveKey) {
            error = QStringLiteral("no API key is set");
        } else if (!timedOut) {
            agent.sendMessage(options.texts.at(turn));
            if (options.stopAfterSecs > 0)
                QTimer::singleShot(int(options.stopAfterSecs * 1000), &loop, [&]() {
                    printJsonLine({{QStringLiteral("event"), QStringLiteral("stop")},
                                   {QStringLiteral("t"), clock.elapsed() / 1000.0},
                                   {QStringLiteral("phase"), agent.phase()}});
                    agent.stop();
                    if (settled())
                        loop.quit();
                });
            if (!settled())
                loop.exec();
        }
        // Timed out: the requests in flight are not stopped here. They end
        // when the agent is destroyed on return, exactly as when the app is
        // closed in mid-answer (tests rely on that path being safe).
        if (timedOut)
            error = QStringLiteral("timed out after %1 s").arg(options.timeoutSecs);

        QString reply;
        QJsonArray tools;
        MessageModel *messages = agent.messages();
        for (int r = rowsBefore; r < messages->rowCount(); ++r) {
            const QModelIndex idx = messages->index(r);
            if (messages->data(idx, MessageModel::KindRole).toString() != QLatin1String("assistant"))
                continue;
            const QString content = messages->data(idx, MessageModel::ContentRole).toString();
            if (!content.trimmed().isEmpty())
                reply = content;
            for (const QVariant &cv : messages->data(idx, MessageModel::ToolCallsRole).toList()) {
                const QVariantMap c = cv.toMap();
                tools.append(QJsonObject{{QStringLiteral("name"), c.value(QStringLiteral("name")).toString()},
                                         {QStringLiteral("status"), c.value(QStringLiteral("status")).toString()},
                                         {QStringLiteral("preview"), c.value(QStringLiteral("preview")).toString()}});
            }
        }
        const bool last = turn + 1 == options.texts.size() || timedOut || !haveKey;
        printJsonLine({{QStringLiteral("event"), last ? QStringLiteral("done") : QStringLiteral("turn")},
                       {QStringLiteral("turn"), int(turn)},
                       {QStringLiteral("t"), clock.elapsed() / 1000.0},
                       {QStringLiteral("provider"), agent.config()->localMode() ? QStringLiteral("local") : agent.config()->provider()},
                       {QStringLiteral("kind"), agent.teamKind()},
                       {QStringLiteral("model"), agent.activeModel()},
                       {QStringLiteral("team"), teamJson()},
                       {QStringLiteral("reply"), reply},
                       {QStringLiteral("tools"), tools},
                       {QStringLiteral("awaiting"), agent.hasPending()},
                       {QStringLiteral("error"), error}});
        if (last)
            break;
    }
    return error.isEmpty() ? 0 : 1;
}

// --set-api-key: the key is read from standard input (never from the command
// line, where other programs could see it) and saved like the Settings page
// saves it.
int runSetApiKey()
{
    QFile in;
    if (!in.open(stdin, QIODevice::ReadOnly)) {
        std::fprintf(stderr, "cannot read standard input\n");
        return 2;
    }
    const QString key = QString::fromUtf8(in.readLine(2048)).trimmed();
    AssistantConfig config;
    const QString problem = config.saveApiKey(key);
    if (!problem.isEmpty()) {
        std::fprintf(stderr, "%s\n", qPrintable(problem));
        return 1;
    }
    printJson({{QStringLiteral("saved"), true},
               {QStringLiteral("file"), config.keyFilePath()},
               {QStringLiteral("provider"), config.provider()},
               {QStringLiteral("inEffect"), config.apiKeySource() == QLatin1String("Settings")},
               {QStringLiteral("source"), config.apiKeySource()}});
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
                       QStringLiteral("voice"), QStringLiteral("team")});

    // One request without a window, and the API key from the command line.
    QCommandLineOption askOpt(QStringLiteral("ask"),
        QStringLiteral("Send one request without opening a window and print what happens as JSON lines "
                       "(the model team's progress, then the reply). Give it several times for several turns of "
                       "one conversation."),
        QStringLiteral("text"));
    QCommandLineOption askTimeoutOpt(QStringLiteral("ask-timeout"),
        QStringLiteral("With --ask: give up after <seconds> (default 180)."), QStringLiteral("seconds"));
    QCommandLineOption askThinkOpt(QStringLiteral("ask-think-hard"),
        QStringLiteral("With --ask: as with \"Think hard\" switched on (every model of every group takes part)."));
    QCommandLineOption askStopOpt(QStringLiteral("ask-stop-after"),
        QStringLiteral("With --ask: press Stop <seconds> after each request was sent."), QStringLiteral("seconds"));
    QCommandLineOption askApproveOpt(QStringLiteral("ask-approve"),
        QStringLiteral("With --ask: answer \"may I do this?\" questions with <yes|no> instead of stopping there."),
        QStringLiteral("yes|no"));
    QCommandLineOption setKeyOpt(QStringLiteral("set-api-key"),
        QStringLiteral("Save the API key read from standard input, as the Settings page does."));
    QCommandLineOption removeKeyOpt(QStringLiteral("remove-api-key"),
        QStringLiteral("Remove the API key saved from Settings."));

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
    app.parser().addOptions({selftestOpt, sessionOpt, outOpt, timeoutOpt, actionsOpt, speechTextOpt, backgroundOpt,
                             askOpt, askTimeoutOpt, askThinkOpt, askApproveOpt, askStopOpt, setKeyOpt, removeKeyOpt});
    if (!app.processArguments())
        return 0;

    if (app.parser().isSet(setKeyOpt))
        return runSetApiKey();
    if (app.parser().isSet(removeKeyOpt)) {
        AssistantConfig config;
        config.removeSavedApiKey();
        printJson({{QStringLiteral("removed"), true}, {QStringLiteral("apiKeyConfigured"), config.apiKeyConfigured()}});
        return 0;
    }
    if (app.parser().isSet(askOpt)) {
        AskOptions ask;
        ask.texts = app.parser().values(askOpt);
        ask.thinkHard = app.parser().isSet(askThinkOpt);
        ask.approve = app.parser().value(askApproveOpt);
        if (!ask.approve.isEmpty() && ask.approve != QLatin1String("yes") && ask.approve != QLatin1String("no")) {
            std::fprintf(stderr, "--ask-approve needs yes or no\n");
            return 2;
        }
        if (app.parser().isSet(askTimeoutOpt)) {
            bool ok = false;
            ask.timeoutSecs = app.parser().value(askTimeoutOpt).toDouble(&ok);
            if (!ok || ask.timeoutSecs <= 0) {
                std::fprintf(stderr, "--ask-timeout needs a number of seconds\n");
                return 2;
            }
        }
        ask.stopAfterSecs = app.parser().value(askStopOpt).toDouble();
        return runAsk(ask);
    }

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
        if (topic == QLatin1String("team")) {
            // The groups as the service offers them now (reads the model list).
            ModelCatalog catalog(&config);
            if (config.nvidia())
                catalog.refreshSync(15000);
            Council council(&config, &catalog);
            QJsonObject leads;
            for (const char *kind : {"coding", "agents", "chat"})
                leads.insert(QLatin1String(kind), catalog.lead(QLatin1String(kind)));
            return app.finishDump(QJsonObject{
                {QStringLiteral("provider"), config.localMode() ? QStringLiteral("local") : config.provider()},
                {QStringLiteral("mode"), config.teamMode() ? QStringLiteral("team") : QStringLiteral("solo")},
                {QStringLiteral("listKnown"), catalog.known()},
                {QStringLiteral("listError"), catalog.error()},
                {QStringLiteral("modelsOffered"), catalog.count()},
                {QStringLiteral("leads"), leads},
                {QStringLiteral("groups"), QJsonArray::fromVariantList(catalog.groups())},
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
