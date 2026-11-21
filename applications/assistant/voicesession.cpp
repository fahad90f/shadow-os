// SPDX-License-Identifier: GPL-3.0-or-later
#include "voicesession.h"

#include "assistant.h"
#include "assistantconfig.h"
#include "voicecontroller.h"
#include "voiceengine.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QTimer>

#include <cstdio>

namespace {
// Voice and agent must have been quiet this long before the session ends
// (a reply may still be on its way, a follow-up listen may still start).
constexpr int kQuietMs = 1500;

// Seconds of audio in a RIFF/WAVE file (0 when it is not one, e.g. a
// scenario file of the test helper).
double wavSeconds(const QString &path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return 0;
    const QByteArray head = f.read(12);
    if (head.size() < 12 || !head.startsWith("RIFF") || head.mid(8, 4) != "WAVE")
        return 0;
    auto le32 = [](const QByteArray &b, int at) {
        return quint32(quint8(b[at])) | quint32(quint8(b[at + 1])) << 8 | quint32(quint8(b[at + 2])) << 16
            | quint32(quint8(b[at + 3])) << 24;
    };
    quint32 byteRate = 0;
    while (!f.atEnd()) {
        const QByteArray chunk = f.read(8);
        if (chunk.size() < 8)
            break;
        const quint32 size = le32(chunk, 4);
        if (chunk.startsWith("fmt ")) {
            const QByteArray fmt = f.read(size);
            if (fmt.size() >= 12)
                byteRate = le32(fmt, 8);
        } else if (chunk.startsWith("data")) {
            return byteRate ? double(size) / byteRate : 0;
        } else if (!f.seek(f.pos() + size + (size & 1))) {
            break;
        }
    }
    return 0;
}
} // namespace

VoiceSession::VoiceSession(const Options &options, QObject *parent)
    : QObject(parent)
    , m_opt(options)
{
}

void VoiceSession::emitLine(const QString &event, QJsonObject fields)
{
    QJsonObject o{{QStringLiteral("event"), event},
                  {QStringLiteral("t"), double(m_clock.elapsed()) / 1000.0}};
    for (auto it = fields.constBegin(); it != fields.constEnd(); ++it)
        if (it.key() != QLatin1String("event") && it.key() != QLatin1String("t"))
            o.insert(it.key(), it.value());
    const QByteArray line = QJsonDocument(o).toJson(QJsonDocument::Compact) + '\n';
    std::fwrite(line.constData(), 1, size_t(line.size()), stdout);
    std::fflush(stdout);
}

QString VoiceSession::parseActions(const QString &spec)
{
    const QStringList items = spec.split(u';', Qt::SkipEmptyParts);
    for (const QString &raw : items) {
        const QString item = raw.trimmed();
        const qsizetype c1 = item.indexOf(u':');
        if (c1 <= 0)
            return item;
        bool ok = false;
        Action a;
        a.at = item.left(c1).toDouble(&ok);
        if (!ok || a.at < 0)
            return item;
        const QString rest = item.mid(c1 + 1);
        const qsizetype c2 = rest.indexOf(u':');
        a.name = (c2 < 0 ? rest : rest.left(c2)).trimmed();
        a.arg = c2 < 0 ? QString() : rest.mid(c2 + 1);
        static const QStringList known = {
            QStringLiteral("start"), QStringLiteral("finish"), QStringLiteral("toggle"), QStringLiteral("cancel"),
            QStringLiteral("stop-agent"), QStringLiteral("stop-speaking"), QStringLiteral("mute"),
            QStringLiteral("unmute"), QStringLiteral("refresh"), QStringLiteral("speak"), QStringLiteral("preview"),
            QStringLiteral("send"), QStringLiteral("install"), QStringLiteral("cancel-install")};
        if (!known.contains(a.name))
            return item;
        m_actions << a;
    }
    return {};
}

int VoiceSession::run()
{
    m_clock.start();
    if (m_opt.wav.isEmpty() || !QFileInfo(m_opt.wav).isFile()) {
        emitLine(QStringLiteral("error"), {{QStringLiteral("message"),
                                            QStringLiteral("no such input file: %1").arg(m_opt.wav)}});
        return 2;
    }
    const QString bad = parseActions(m_opt.actions);
    if (!bad.isEmpty()) {
        emitLine(QStringLiteral("error"), {{QStringLiteral("message"),
                                            QStringLiteral("bad --voice-actions item: %1").arg(bad)}});
        return 2;
    }
    m_actionsPending = int(m_actions.size());
    if (m_opt.outDir.isEmpty()) {
        QTemporaryDir tmp(QDir::tempPath() + QStringLiteral("/shadow-voice-session-XXXXXX"));
        tmp.setAutoRemove(false); // the speech files are the result
        m_opt.outDir = tmp.path();
    }
    QDir().mkpath(m_opt.outDir);

    m_agent = new Assistant(this);
    m_voice = new VoiceController(this);
    m_voice->voiceEngine()->setServeArguments({QStringLiteral("--input-wav"), QFileInfo(m_opt.wav).absoluteFilePath(),
                                               QStringLiteral("--output-dir"), QFileInfo(m_opt.outDir).absoluteFilePath()});
    // The log connects first, so each line shows a cause before its effects
    // (an agent error before the spoken apology it triggers).
    wire();
    m_voice->setAgent(m_agent); // config is adopted from the agent

    m_quietTimer = new QTimer(this);
    m_quietTimer->setSingleShot(true);
    m_quietTimer->setInterval(kQuietMs);
    connect(m_quietTimer, &QTimer::timeout, this, [this]() {
        if (quiet())
            finish(m_eof ? QStringLiteral("eof") : QStringLiteral("engine-settled"), 0);
    });
    m_deadline = new QTimer(this);
    m_deadline->setSingleShot(true);
    connect(m_deadline, &QTimer::timeout, this, [this]() { finish(QStringLiteral("timeout"), 1); });
    m_deadline->start(int(qMax(1.0, m_opt.timeoutSecs) * 1000));

    QString why;
    emitLine(QStringLiteral("session"), {
        {QStringLiteral("input"), QFileInfo(m_opt.wav).absoluteFilePath()},
        {QStringLiteral("out"), QFileInfo(m_opt.outDir).absoluteFilePath()},
        {QStringLiteral("helper"), VoiceEngine::locateHelper(&why)},
        {QStringLiteral("baseUrl"), m_agent->config()->baseUrl()},
        {QStringLiteral("actions"), int(m_actions.size())},
        {QStringLiteral("micLevel"), m_voice->micLevel()},
        {QStringLiteral("speechLevel"), m_voice->speechLevel()},
        {QStringLiteral("partial"), m_voice->partialText()},
    });
    m_voice->start();
    if (m_voice->engine() != QLatin1String("starting"))
        onSettled();
    const int rc = QCoreApplication::exec();
    return m_finished ? m_exitCode : rc;
}

void VoiceSession::wire()
{
    VoiceController *v = m_voice;
    connect(v, &VoiceController::engineChanged, this, [this]() {
        emitLine(QStringLiteral("engine"), {{QStringLiteral("engine"), m_voice->engine()},
                                            {QStringLiteral("message"), m_voice->engineMessage()}});
        if (!m_settled && m_voice->engine() != QLatin1String("starting"))
            onSettled();
        checkQuiet();
    });
    connect(v, &VoiceController::stateChanged, this, [this]() {
        emitLine(QStringLiteral("state"), {{QStringLiteral("value"), m_voice->state()}});
        checkQuiet();
    });
    connect(v, &VoiceController::voiceTurnChanged, this, [this]() {
        emitLine(QStringLiteral("voice_turn"), {{QStringLiteral("value"), m_voice->voiceTurn()}});
        checkQuiet();
    });
    connect(v, &VoiceController::transcriptReady, this, [this](const QString &text) {
        ++m_sent;
        emitLine(QStringLiteral("sent"), {{QStringLiteral("text"), text}});
        if (m_conversation.isEmpty())
            QTimer::singleShot(0, this, [this]() { m_conversation = m_agent->activeId(); });
    });
    connect(v, &VoiceController::heard, this, [this](const QString &text) {
        emitLine(QStringLiteral("heard"), {{QStringLiteral("text"), text}});
    });
    connect(v, &VoiceController::yesNoResult, this, [this](const QString &tag, bool yes) {
        emitLine(QStringLiteral("yesno"), {{QStringLiteral("tag"), tag}, {QStringLiteral("yes"), yes}});
    });
    connect(v, &VoiceController::unavailable, this, [this](const QString &reason) {
        emitLine(QStringLiteral("unavailable"), {{QStringLiteral("reason"), reason}});
    });
    connect(v, &VoiceController::installChanged, this, [this]() {
        emitLine(QStringLiteral("install"), {{QStringLiteral("installing"), m_voice->installing()},
                                             {QStringLiteral("progress"), m_voice->installProgress()},
                                             {QStringLiteral("status"), m_voice->installStatus()}});
    });
    connect(v, &VoiceController::spokenSentenceChanged, this, [this]() {
        emitLine(QStringLiteral("spoken"), {{QStringLiteral("text"), m_voice->spokenSentence()}});
    });
    connect(v, &VoiceController::listenModeChanged, this, [this]() {
        emitLine(QStringLiteral("listen_mode"), {{QStringLiteral("value"), m_voice->listenMode()}});
    });
    connect(v, &VoiceController::levelsChanged, this, [this]() {
        // How often QML would be notified: the busiest second of the run.
        ++m_levelEmits;
        const int bucket = int(m_clock.elapsed() / 1000);
        if (bucket != m_levelBucket) {
            m_levelBucket = bucket;
            m_levelBucketCount = 0;
        }
        m_levelMaxHz = qMax(m_levelMaxHz, ++m_levelBucketCount);
    });

    VoiceEngine *engine = v->voiceEngine();
    connect(engine, &VoiceEngine::eventReceived, this, &VoiceSession::onEngineEvent);
    connect(engine, &VoiceEngine::commandSent, this, &VoiceSession::onCommand);
    connect(engine, &VoiceEngine::restartScheduled, this, [this](int delayMs, int failures) {
        emitLine(QStringLiteral("engine_restart"), {{QStringLiteral("delay_ms"), delayMs},
                                                    {QStringLiteral("failures"), failures}});
    });

    connect(m_agent, &Assistant::assistantReplyReady, this, [this](const QString &text) {
        ++m_replies;
        emitLine(QStringLiteral("reply"), {{QStringLiteral("text"), text}});
    });
    connect(m_agent, &Assistant::errorMessage, this, [this](const QString &text) {
        emitLine(QStringLiteral("error"), {{QStringLiteral("source"), QStringLiteral("agent")},
                                           {QStringLiteral("message"), text}});
    });
    connect(m_agent, &Assistant::confirmationNeeded, this,
            [this](const QString &summary, const QString &preview, const QString &callId) {
        emitLine(QStringLiteral("confirm"), {{QStringLiteral("tag"), callId},
                                             {QStringLiteral("summary"), summary},
                                             {QStringLiteral("preview"), preview}});
    });
    connect(m_agent, &Assistant::toolLogChanged, this, [this]() {
        const QVariantList log = m_agent->toolLog();
        if (log.isEmpty())
            return;
        const QVariantMap last = log.first().toMap();
        emitLine(QStringLiteral("tool"), {{QStringLiteral("name"), last.value(QStringLiteral("name")).toString()},
                                          {QStringLiteral("status"), last.value(QStringLiteral("status")).toString()}});
    });
    connect(m_agent, &Assistant::phaseChanged, this, [this]() {
        const QString phase = m_agent->phase();
        if (phase != m_lastPhase) {
            m_lastPhase = phase;
            emitLine(QStringLiteral("phase"), {{QStringLiteral("value"), phase},
                                               {QStringLiteral("busy"), m_agent->busy()}});
        }
        checkQuiet();
    });
}

void VoiceSession::onEngineEvent(const QJsonObject &ev)
{
    const QString e = ev.value(QStringLiteral("event")).toString();
    // Helper events worth a line of their own (levels and VAD would drown the log).
    static const QStringList forwarded = {
        QStringLiteral("hello"), QStringLiteral("ready"), QStringLiteral("wake"), QStringLiteral("wake_rejected"),
        QStringLiteral("partial"), QStringLiteral("final"), QStringLiteral("timeout"), QStringLiteral("speak_start"),
        QStringLiteral("first_audio"), QStringLiteral("sentence"), QStringLiteral("speak_end"), QStringLiteral("mic"),
        QStringLiteral("eof"), QStringLiteral("config_ok")};
    if (e == QLatin1String("final"))
        ++m_finals;
    if (e == QLatin1String("eof"))
        m_eof = true;
    if (e == QLatin1String("ready") && !m_eof && !m_inputTimerStarted) {
        // A helper that reports no "eof" has heard all of a WAV (played in
        // real time) at the latest this long after it became ready.
        const double secs = wavSeconds(m_opt.wav);
        if (secs > 0) {
            m_inputTimerStarted = true;
            QTimer::singleShot(int((secs + 1.0) * 1000), this, [this]() {
                if (!m_eof) {
                    m_eof = true;
                    emitLine(QStringLiteral("input_done"), {{QStringLiteral("seconds"), wavSeconds(m_opt.wav)}});
                    checkQuiet();
                }
            });
        }
    }
    if (e == QLatin1String("error")) {
        QJsonObject f = ev;
        f.insert(QStringLiteral("source"), QStringLiteral("helper"));
        emitLine(QStringLiteral("error"), f);
    } else if (forwarded.contains(e)) {
        QJsonObject f = ev;
        f.insert(QStringLiteral("helper_t"), ev.value(QStringLiteral("t")));
        emitLine(e, f);
    }
    checkQuiet();
}

void VoiceSession::onCommand(const QJsonObject &cmd)
{
    const QString c = cmd.value(QStringLiteral("cmd")).toString();
    if (c == QLatin1String("speak")) {
        ++m_speakChunks;
        emitLine(QStringLiteral("speak"), {{QStringLiteral("id"), cmd.value(QStringLiteral("id"))},
                                           {QStringLiteral("text"), cmd.value(QStringLiteral("text"))},
                                           {QStringLiteral("more"), cmd.value(QStringLiteral("more"))}});
    } else if (c == QLatin1String("listen")) {
        const bool followUp = cmd.value(QStringLiteral("mode")).toString() == QLatin1String("followup");
        emitLine(followUp ? QStringLiteral("follow_up") : QStringLiteral("listen"),
                 {{QStringLiteral("id"), cmd.value(QStringLiteral("id"))},
                  {QStringLiteral("mode"), cmd.value(QStringLiteral("mode"))},
                  {QStringLiteral("timeout"), cmd.value(QStringLiteral("timeout"))}});
    } else {
        QJsonObject f = cmd;
        f.remove(QStringLiteral("cmd"));
        f.insert(QStringLiteral("name"), c);
        emitLine(QStringLiteral("command"), f);
    }
}

void VoiceSession::onSettled()
{
    if (m_settled)
        return;
    m_settled = true;
    for (const Action &a : std::as_const(m_actions))
        QTimer::singleShot(int(a.at * 1000), this, [this, a]() { runAction(a); });
    checkQuiet();
}

void VoiceSession::runAction(const Action &a)
{
    if (m_finished)
        return;
    emitLine(QStringLiteral("action"), {{QStringLiteral("name"), a.name}, {QStringLiteral("arg"), a.arg}});
    VoiceController *v = m_voice;
    if (a.name == QLatin1String("start"))
        v->startListening();
    else if (a.name == QLatin1String("finish"))
        v->finishListening();
    else if (a.name == QLatin1String("toggle"))
        v->toggleListening();
    else if (a.name == QLatin1String("cancel"))
        v->cancel();
    else if (a.name == QLatin1String("stop-agent"))
        m_agent->stop();
    else if (a.name == QLatin1String("stop-speaking"))
        v->stopSpeaking();
    else if (a.name == QLatin1String("mute"))
        m_agent->config()->setMicMuted(true);
    else if (a.name == QLatin1String("unmute"))
        m_agent->config()->setMicMuted(false);
    else if (a.name == QLatin1String("refresh"))
        v->refresh();
    else if (a.name == QLatin1String("speak"))
        v->speak(a.arg);
    else if (a.name == QLatin1String("preview"))
        v->previewVoice(a.arg);
    else if (a.name == QLatin1String("install"))
        v->installEngine();
    else if (a.name == QLatin1String("cancel-install"))
        v->cancelInstall();
    else if (a.name == QLatin1String("send")) {
        // Like the UI's test prompt: a voice turn that starts from text.
        v->beginVoiceTurn();
        if (m_agent->canSend()) {
            m_agent->sendMessage(a.arg);
            if (m_conversation.isEmpty())
                m_conversation = m_agent->activeId();
        }
    }
    --m_actionsPending;
    checkQuiet();
}

bool VoiceSession::quiet() const
{
    if (m_finished || !m_settled || m_actionsPending > 0)
        return false;
    const QString engine = m_voice->engine();
    // Normally the end of the helper's input ends the session; without a
    // working helper (no "eof" will ever come) a settled engine does.
    const bool inputDone = m_eof || engine == QLatin1String("basic") || engine == QLatin1String("missing")
        || engine == QLatin1String("error");
    if (!inputDone)
        return false;
    if (m_voice->voiceTurn() || m_voice->installing())
        return false;
    const QString st = m_voice->state();
    if (st == QLatin1String("listening") || st == QLatin1String("transcribing") || st == QLatin1String("speaking"))
        return false;
    if (m_voice->listenPending())
        return false;
    if (m_agent->busy() || m_agent->hasPending())
        return false;
    const QString phase = m_agent->phase();
    return phase == QLatin1String("idle") || phase == QLatin1String("setup");
}

void VoiceSession::checkQuiet()
{
    if (m_finished || !m_quietTimer)
        return;
    // The log's handlers run before VoiceController's (causal order in the
    // log), so look again once every handler of the current event has run —
    // e.g. a listen the helper refuses at once changes no state afterwards.
    if (!m_recheckQueued) {
        m_recheckQueued = true;
        QTimer::singleShot(0, this, [this]() {
            m_recheckQueued = false;
            if (!m_finished && quiet() && !m_quietTimer->isActive())
                m_quietTimer->start();
        });
    }
    if (quiet()) {
        if (!m_quietTimer->isActive())
            m_quietTimer->start();
    } else {
        m_quietTimer->stop();
    }
}

void VoiceSession::finish(const QString &reason, int exitCode)
{
    if (m_finished)
        return;
    m_finished = true;
    m_exitCode = exitCode;
    m_quietTimer->stop();
    m_deadline->stop();
    const QJsonObject diag = m_voice->diagnostics();
    emitLine(QStringLiteral("done"), {
        {QStringLiteral("reason"), reason},
        {QStringLiteral("exit"), exitCode},
        {QStringLiteral("finals"), m_finals},
        {QStringLiteral("sent"), m_sent},
        {QStringLiteral("replies"), m_replies},
        {QStringLiteral("speak_chunks"), m_speakChunks},
        {QStringLiteral("level_notifications"), m_levelEmits},
        {QStringLiteral("levels_max_hz"), m_levelMaxHz},
        {QStringLiteral("engine"), m_voice->engine()},
        {QStringLiteral("engineMessage"), m_voice->engineMessage()},
        {QStringLiteral("statusText"), m_voice->statusText()},
        {QStringLiteral("state"), m_voice->state()},
        {QStringLiteral("voiceTurn"), m_voice->voiceTurn()},
        {QStringLiteral("micLevel"), m_voice->micLevel()},
        {QStringLiteral("speechLevel"), m_voice->speechLevel()},
        {QStringLiteral("partial"), m_voice->partialText()},
        {QStringLiteral("lastHeard"), m_voice->lastHeard()},
        {QStringLiteral("resolvedTtsEngine"), m_voice->resolvedTtsEngine()},
        {QStringLiteral("capabilities"), diag.value(QStringLiteral("capabilities"))},
        {QStringLiteral("helper_stderr"), diag.value(QStringLiteral("helper")).toObject().value(QStringLiteral("stderrTail"))},
        {QStringLiteral("out"), QFileInfo(m_opt.outDir).absoluteFilePath()},
    });
    // The session's conversation is test traffic, not the user's history.
    if (!m_conversation.isEmpty())
        m_agent->deleteConversation(m_conversation);
    QCoreApplication::exit(exitCode);
}
