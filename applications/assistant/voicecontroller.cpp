// SPDX-License-Identifier: GPL-3.0-or-later
#include "voicecontroller.h"

#include "assistant.h"
#include "assistantconfig.h"
#include "voiceengine.h"

#include <QCoreApplication>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMetaEnum>
#include <QProcess>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QTimer>

#include <cmath>

namespace {

constexpr int kLevelIntervalMs = 33;     // level notifications: at most ~30 Hz
constexpr qint64 kLevelStaleMs = 220;    // a raw level older than this counts as silence
constexpr double kCommandTimeoutS = 8;   // push-to-talk: seconds until speech must start
constexpr double kYesNoTimeoutS = 8;
constexpr double kFollowUpTimeoutS = 5;  // spec: listen 5 s for a follow-up
// A listen must end in "final" or "timeout"; if the helper says neither
// within its timeout plus this margin (an utterance may run ~15 s and then be
// transcribed), the listen is abandoned instead of hanging.
constexpr int kListenGraceMs = 30000;
// eSpeak NG's own default rate; speech_rate scales it.
constexpr int kEspeakWpm = 175;

struct CuratedVoice
{
    const char *id;
    const char *name;
    const char *accent;
    const char *description;
};

// The female voices offered in the app (Kokoro v1.0 voice ids).
const CuratedVoice kVoices[] = {
    {"af_heart", QT_TRANSLATE_NOOP("VoiceController", "Heart"), QT_TRANSLATE_NOOP("VoiceController", "American"),
     QT_TRANSLATE_NOOP("VoiceController", "Warm — the default voice")},
    {"af_bella", QT_TRANSLATE_NOOP("VoiceController", "Bella"), QT_TRANSLATE_NOOP("VoiceController", "American"),
     QT_TRANSLATE_NOOP("VoiceController", "Bright")},
    {"af_sarah", QT_TRANSLATE_NOOP("VoiceController", "Sarah"), QT_TRANSLATE_NOOP("VoiceController", "American"),
     QT_TRANSLATE_NOOP("VoiceController", "Lively")},
    {"bf_emma", QT_TRANSLATE_NOOP("VoiceController", "Emma"), QT_TRANSLATE_NOOP("VoiceController", "British"),
     QT_TRANSLATE_NOOP("VoiceController", "Elegant")},
    {"af_nicole", QT_TRANSLATE_NOOP("VoiceController", "Nicole"), QT_TRANSLATE_NOOP("VoiceController", "American"),
     QT_TRANSLATE_NOOP("VoiceController", "Soft and whispery")},
    {"af_sky", QT_TRANSLATE_NOOP("VoiceController", "Sky"), QT_TRANSLATE_NOOP("VoiceController", "American"),
     QT_TRANSLATE_NOOP("VoiceController", "Calm")},
};

QString tr2(const char *s)
{
    return QCoreApplication::translate("VoiceController", s);
}

QString voiceName(const QString &id)
{
    for (const CuratedVoice &v : kVoices)
        if (id == QLatin1String(v.id))
            return tr2(v.name);
    return id;
}

// "Run a command" → "run a command" (but "URL …" stays as it is).
QString lowerFirst(QString s)
{
    if (s.size() >= 2 && s.at(0).isUpper() && !s.at(1).isUpper())
        s[0] = s.at(0).toLower();
    return s;
}

enum class Answer { Yes, No, Unclear };

// A spoken yes/no. The first decisive word wins, so "yes, no problem" is a
// yes and "no, don't run it" a no; anything else is unclear (the on-screen
// card then decides — an unclear answer must never approve an action).
Answer parseYesNo(const QString &text)
{
    static const QStringList yesPhrases = {QStringLiteral("go ahead"), QStringLiteral("do it"),
                                           QStringLiteral("run it"), QStringLiteral("please do"),
                                           QStringLiteral("of course"), QStringLiteral("sounds good")};
    static const QStringList noPhrases = {QStringLiteral("do not"), QStringLiteral("no way"),
                                          QStringLiteral("not now"), QStringLiteral("never mind")};
    static const QSet<QString> yesWords = {
        QStringLiteral("yes"), QStringLiteral("yeah"), QStringLiteral("yep"), QStringLiteral("yup"),
        QStringLiteral("sure"), QStringLiteral("okay"), QStringLiteral("ok"), QStringLiteral("affirmative"),
        QStringLiteral("confirm"), QStringLiteral("confirmed"), QStringLiteral("absolutely"),
        QStringLiteral("correct"), QStringLiteral("proceed")};
    static const QSet<QString> noWords = {
        QStringLiteral("no"), QStringLiteral("nope"), QStringLiteral("nah"), QStringLiteral("negative"),
        QStringLiteral("cancel"), QStringLiteral("stop"), QStringLiteral("deny"), QStringLiteral("don't"),
        QStringLiteral("dont"), QStringLiteral("not"), QStringLiteral("abort")};
    static const QRegularExpression nonWord(QStringLiteral("[^a-z']+"));
    QString t = text.toLower();
    t.replace(QChar(0x2019), u'\'');
    const QStringList words = t.split(nonWord, Qt::SkipEmptyParts);
    for (qsizetype i = 0; i < words.size(); ++i) {
        if (i + 1 < words.size()) {
            const QString two = words.at(i) + u' ' + words.at(i + 1);
            if (yesPhrases.contains(two))
                return Answer::Yes;
            if (noPhrases.contains(two))
                return Answer::No;
        }
        if (noWords.contains(words.at(i)))
            return Answer::No;
        if (yesWords.contains(words.at(i)))
            return Answer::Yes;
    }
    return Answer::Unclear;
}

QJsonArray toJsonArray(const QStringList &l)
{
    return QJsonArray::fromStringList(l);
}

// The sentences a Markdown text is spoken as (SpeechText::forSpeech() joins
// exactly these with spaces).
QStringList speechSentences(const QString &markdown)
{
    SentenceStream stream;
    QStringList all = stream.push(markdown);
    all += stream.flush();
    return all;
}

// How many sentences past the last match a helper sentence is looked for.
constexpr int kSentenceLookahead = 8;

// Words for comparing what the helper says with what was sent: runs of
// letters and digits, lower case, accents removed (the helper drops them
// too). Numbers, units, paths and acronyms come back spelled out from the
// helper and simply do not match; plain words do.
QStringList matchWords(const QString &text)
{
    const QString decomposed = text.normalized(QString::NormalizationForm_KD);
    QStringList out;
    QString word;
    for (const QChar c : decomposed) {
        if (c.category() == QChar::Mark_NonSpacing)
            continue;
        if (c.isLetterOrNumber()) {
            word += c.toLower();
        } else if (!word.isEmpty()) {
            out << word;
            word.clear();
        }
    }
    if (!word.isEmpty())
        out << word;
    return out;
}

// Length of the longest common subsequence of 'a' and 'b'; '*end' gets the
// length of the shortest prefix of 'b' that still contains all of it (where
// the match ends in 'b').
int commonWords(const QStringList &a, const QStringList &b, int *end)
{
    const qsizetype n = a.size(), m = b.size();
    QList<int> prev(m + 1, 0), row(m + 1, 0);
    for (qsizetype i = 1; i <= n; ++i) {
        for (qsizetype j = 1; j <= m; ++j)
            row[j] = a.at(i - 1) == b.at(j - 1) ? prev[j - 1] + 1 : qMax(prev[j], row[j - 1]);
        std::swap(prev, row);
    }
    const int best = prev[m];
    int e = int(m);
    for (qsizetype j = 0; j <= m; ++j) {
        if (prev[j] == best) {
            e = int(j);
            break;
        }
    }
    if (end)
        *end = e;
    return best;
}

} // namespace

// --- construction ---------------------------------------------------------------

VoiceController::VoiceController(QObject *parent)
    : QObject(parent)
{
    m_clock.start();
    m_engine = new VoiceEngine(this);
    connect(m_engine, &VoiceEngine::statusChanged, this, &VoiceController::onEngineStatus);
    connect(m_engine, &VoiceEngine::event, this, &VoiceController::onEngineEvent);
    connect(m_engine, &VoiceEngine::processStarted, this, [this]() {
        // Sent before the models have even loaded: the helper must know about
        // a privacy mute (and an off wake word) before it opens the microphone.
        m_lastConfigSent = {};
        m_lastMuteSent = -1;
        syncHelperConfig(true);
    });

    m_listenWatchdog = new QTimer(this);
    m_listenWatchdog->setSingleShot(true);
    connect(m_listenWatchdog, &QTimer::timeout, this, [this]() {
        if (!m_listenActive)
            return;
        const bool command = m_listenMode == QLatin1String("command");
        m_engine->send({{QStringLiteral("cmd"), QStringLiteral("cancel")}});
        clearListen();
        setPartial({});
        if (command) {
            endVoiceTurn();
            Q_EMIT unavailable(tr("The voice engine stopped responding while listening."));
        }
        updateState();
    });

    m_levelTimer = new QTimer(this);
    m_levelTimer->setInterval(kLevelIntervalMs);
    connect(m_levelTimer, &QTimer::timeout, this, &VoiceController::tickLevels);

    // Screenshot/test hooks: realistic live values without a microphone.
    const QString partial = qEnvironmentVariable("SHADOW_ASSISTANT_TEST_PARTIAL");
    if (!partial.isEmpty()) {
        m_partial = partial;
        m_testPartial = true;
    }
    bool ok = false;
    const double level = qEnvironmentVariable("SHADOW_ASSISTANT_TEST_LEVEL").toDouble(&ok);
    if (ok) {
        m_testLevel = true;
        m_micLevel = m_speechLevel = qBound(0.0, level, 1.0);
    }

    detectTools();
    updateEngine();
    updateState();
}

VoiceController::~VoiceController()
{
    if (m_agent)
        disconnect(m_agent, nullptr, this, nullptr);
    // Application exit: no eSpeak or installer is left running behind us.
    for (QProcess *p : {m_espeak, m_install}) {
        if (!p)
            continue;
        p->disconnect(this);
        p->kill();
        p->waitForFinished(300);
    }
}

void VoiceController::componentComplete()
{
    start();
}

void VoiceController::start()
{
    if (m_started)
        return;
    m_started = true;
    m_engine->start();
    updateEngine();
    updateState();
}

void VoiceController::detectTools()
{
    m_espeakBin = QStandardPaths::findExecutable(QStringLiteral("espeak-ng"));
    m_wtype = !QStandardPaths::findExecutable(QStringLiteral("wtype")).isEmpty();
    m_xdotool = !QStandardPaths::findExecutable(QStringLiteral("xdotool")).isEmpty();
}

QString VoiceController::locateSetupTool(QString *why)
{
    const QString env = qEnvironmentVariable("SHADOW_VOICE_SETUP");
    if (!env.isEmpty()) {
        const QFileInfo fi(env);
        if (fi.isFile() && fi.isExecutable())
            return env;
        if (why)
            *why = tr("SHADOW_VOICE_SETUP points to %1, which is not an executable file.").arg(env);
        return {};
    }
    QStringList candidates;
    const QString appDir = QCoreApplication::applicationDirPath();
    if (!appDir.isEmpty())
        candidates << appDir + QStringLiteral("/shadow-voice-setup");
    candidates << QStringLiteral("/usr/bin/shadow-voice-setup") << QStringLiteral("/usr/local/bin/shadow-voice-setup");
    candidates.removeDuplicates();
    for (const QString &c : std::as_const(candidates)) {
        const QFileInfo fi(c);
        if (fi.isFile() && fi.isExecutable())
            return c;
    }
    if (why)
        *why = tr("The voice setup tool (shadow-voice-setup) is not installed — looked in %1.")
                   .arg(candidates.join(QStringLiteral(", ")));
    return {};
}

void VoiceController::setConfig(AssistantConfig *c)
{
    if (m_config == c)
        return;
    if (m_config)
        disconnect(m_config, nullptr, this, nullptr);
    m_config = c;
    if (m_config)
        connect(m_config, &AssistantConfig::changed, this, &VoiceController::onConfigChanged);
    Q_EMIT configChanged();
    onConfigChanged();
}

void VoiceController::setAgent(Assistant *a)
{
    if (m_agent == a)
        return;
    if (m_agent)
        disconnect(m_agent, nullptr, this, nullptr);
    m_agent = a;
    if (a) {
        connect(a, &Assistant::replyDelta, this, &VoiceController::onReplyDelta);
        connect(a, &Assistant::replySegmentFinished, this, &VoiceController::onReplySegmentFinished);
        connect(a, &Assistant::assistantReplyReady, this, &VoiceController::onAssistantReplyReady);
        connect(a, &Assistant::confirmationNeeded, this, &VoiceController::onConfirmationNeeded);
        connect(a, &Assistant::errorMessage, this, &VoiceController::onAgentError);
        connect(a, &Assistant::stopped, this, &VoiceController::onAgentStopped);
        connect(a, &Assistant::phaseChanged, this, &VoiceController::onAgentPhaseChanged);
        if (!m_config)
            setConfig(a->config());
    }
    Q_EMIT agentChanged();
}

// --- engine status and events -------------------------------------------------

void VoiceController::onEngineStatus()
{
    if (!m_engine->isReady()) {
        // Whatever the helper was doing is gone with it.
        m_helperState.clear();
        m_wakeListen = false;
        m_micStatus.clear();
        m_micMessage.clear();
        if (m_listenActive) {
            const bool command = m_listenMode == QLatin1String("command");
            clearListen();
            setPartial({});
            if (command) {
                endVoiceTurn();
                Q_EMIT unavailable(tr("The voice engine stopped while listening."));
            }
        }
        // Speech queued in the helper will never report its end: run what
        // waited for it as interrupted (a yes/no prompt leaves the card).
        const QSet<QString> lost = m_helperSpeech;
        m_helperSpeech.clear();
        forgetSpeechTracks();
        if (!m_espeak)
            setSpokenSentence({});
        for (const QString &id : lost)
            onSpeechEnded(id, true);
        m_captureTool = m_engine->probeResult().value(QStringLiteral("capture")).toString();
    } else {
        m_captureTool = m_engine->readyInfo().value(QStringLiteral("capture")).toString();
        syncHelperConfig(true);
    }
    updateEngine();
    updateState();
    Q_EMIT capabilitiesChanged();
}

void VoiceController::onEngineEvent(const QJsonObject &ev)
{
    const QString e = ev.value(QStringLiteral("event")).toString();
    if (e == QLatin1String("level")) {
        noteLevel(ev.value(QStringLiteral("source")).toString(), ev.value(QStringLiteral("rms")).toDouble());
    } else if (e == QLatin1String("state")) {
        const QString before = m_helperState;
        m_helperState = ev.value(QStringLiteral("value")).toString();
        if ((before == QLatin1String("listening") || before == QLatin1String("transcribing"))
            && m_helperState != QLatin1String("listening") && m_helperState != QLatin1String("transcribing"))
            m_wakeListen = false; // whatever the helper listened for is over
        updateState();
    } else if (e == QLatin1String("partial")) {
        setPartial(ev.value(QStringLiteral("text")).toString());
    } else if (e == QLatin1String("final")) {
        if (ev.value(QStringLiteral("id")).toString() == QLatin1String("wake"))
            m_wakeListen = false;
        handleFinal(ev);
        updateListenMode();
    } else if (e == QLatin1String("timeout")) {
        if (ev.value(QStringLiteral("id")).toString() == QLatin1String("wake"))
            m_wakeListen = false;
        handleTimeout(ev);
        updateListenMode();
    } else if (e == QLatin1String("wake")) {
        // The helper takes the command itself now (its own listen, id "wake").
        if (!m_listenActive) {
            m_wakeListen = true;
            updateListenMode();
        }
        // "kws" is only an early acoustic hit; the transcript confirms it.
        if (ev.value(QStringLiteral("via")).toString() == QLatin1String("stt"))
            Q_EMIT wakeDetected();
    } else if (e == QLatin1String("wake_rejected")) {
        m_wakeListen = false;
        updateListenMode();
        setPartial({});
    } else if (e == QLatin1String("sentence")) {
        onHelperSentence(ev.value(QStringLiteral("id")).toString(), ev.value(QStringLiteral("text")).toString());
    } else if (e == QLatin1String("speak_end")) {
        const QString id = ev.value(QStringLiteral("id")).toString();
        m_helperSpeech.remove(id);
        m_tracks.remove(id);
        setSpokenSentence({});
        onSpeechEnded(id, ev.value(QStringLiteral("interrupted")).toBool());
    } else if (e == QLatin1String("mic")) {
        handleMic(ev);
    } else if (e == QLatin1String("error")) {
        if (ev.value(QStringLiteral("fatal")).toBool())
            return; // the helper exits; VoiceEngine reports and restarts it
        const QString msg = ev.value(QStringLiteral("message")).toString();
        const qint64 now = m_clock.elapsed();
        if (msg == m_lastErrorText && now - m_lastErrorAt < 10000)
            return; // the same complaint again: one toast is enough
        m_lastErrorText = msg;
        m_lastErrorAt = now;
        Q_EMIT unavailable(tr("Voice engine: %1").arg(msg));
    }
}

void VoiceController::handleMic(const QJsonObject &ev)
{
    const QString status = ev.value(QStringLiteral("status")).toString();
    const QString tool = ev.value(QStringLiteral("tool")).toString();
    const QString before = m_micStatus;
    m_micStatus = status;
    m_micMessage = ev.value(QStringLiteral("message")).toString();
    if (!tool.isEmpty())
        m_captureTool = tool;
    if (status == QLatin1String("error") && before != QLatin1String("error")) {
        if (m_listenActive) {
            const bool command = m_listenMode == QLatin1String("command");
            m_engine->send({{QStringLiteral("cmd"), QStringLiteral("cancel")}});
            clearListen();
            setPartial({});
            if (command)
                endVoiceTurn();
        }
        Q_EMIT unavailable(tr("Microphone not available: %1")
                               .arg(m_micMessage.isEmpty() ? tool : m_micMessage));
    }
    updateState();
    Q_EMIT capabilitiesChanged();
}

void VoiceController::onConfigChanged()
{
    const bool muted = m_config && m_config->micMuted();
    if (muted && m_listenActive) {
        // Privacy first: a mute ends any listen at once (the helper also
        // stops its capture when it gets the mute command).
        const bool command = m_listenMode == QLatin1String("command");
        m_engine->send({{QStringLiteral("cmd"), QStringLiteral("cancel")}});
        clearListen();
        setPartial({});
        m_yesNoTag.clear();
        if (command)
            endVoiceTurn();
    }
    syncHelperConfig(false);
    updateEngine();
    updateState();
    Q_EMIT capabilitiesChanged();
}

void VoiceController::syncHelperConfig(bool force)
{
    if (!m_config || !m_engine->isRunning())
        return;
    const bool muted = m_config->micMuted();
    if (force || m_lastMuteSent != int(muted)) {
        m_engine->send({{QStringLiteral("cmd"), QStringLiteral("mute")}, {QStringLiteral("value"), muted}});
        m_lastMuteSent = int(muted);
    }
    const QJsonObject cfg{
        {QStringLiteral("cmd"), QStringLiteral("config")},
        {QStringLiteral("voice"), m_config->voice()},
        {QStringLiteral("speed"), m_config->speechRate()},
        {QStringLiteral("wake"), m_config->wakeEnabled()},
        {QStringLiteral("wake_sensitivity"), m_config->wakeSensitivity()},
        {QStringLiteral("stt"), m_config->sttModel()},
        {QStringLiteral("input_device"), m_config->inputDevice()},
        {QStringLiteral("muted"), muted},
        {QStringLiteral("follow_up_secs"), m_config->followUp() ? kFollowUpTimeoutS : 0.0},
    };
    if (!force && cfg == m_lastConfigSent)
        return;
    if (m_engine->send(cfg))
        m_lastConfigSent = cfg;
}

void VoiceController::updateEngine()
{
    using S = VoiceEngine::Status;
    QString name;
    QString msg;
    const S st = m_engine->status();
    const bool espeak = !m_espeakBin.isEmpty();
    const QString installHint = tr("To install it, run: shadow-voice-setup — or use Install in Settings.");
    if (st == S::Ready) {
        name = QStringLiteral("neural");
    } else if (m_installing) {
        name = QStringLiteral("installing");
        msg = m_installStatus.isEmpty() ? tr("Installing the natural voice…") : m_installStatus;
    } else {
        switch (st) {
        case S::Stopped:
        case S::Probing:
        case S::Starting:
        case S::Restarting:
            name = QStringLiteral("starting");
            msg = m_engine->message().isEmpty() ? tr("Starting the natural voice…") : m_engine->message();
            break;
        case S::Unavailable: {
            name = espeak ? QStringLiteral("basic") : QStringLiteral("missing");
            const bool helperMissing = m_engine->unavailableKind() == QLatin1String("helper");
            QString fix = helperMissing
                ? tr("Reinstall Shadow Assistant (it ships the voice helper), then run: shadow-voice-setup.")
                : installHint;
            if (!helperMissing && m_engine->message().contains(QLatin1String("shadow-voice-setup")))
                fix.clear(); // the helper's own reason already names the fix
            fix = fix.trimmed();
            msg = (espeak
                ? tr("Basic voice: replies are spoken with eSpeak NG; listening and the wake word need the "
                     "natural voice. %1 %2").arg(m_engine->message(), fix)
                : tr("No voice is available: %1 eSpeak NG (espeak-ng) is not installed either. %2")
                      .arg(m_engine->message(), fix)).trimmed();
            break;
        }
        case S::Failed:
            name = QStringLiteral("error");
            msg = m_engine->message() + QStringLiteral("\n")
                + tr("Check the voice engine again (refresh) to restart it.");
            break;
        case S::Ready:
            break;
        }
    }
    if (!m_started && st == S::Stopped)
        msg = tr("The voice engine has not been started yet.");
    if (name == m_engineName && msg == m_engineMessage)
        return;
    m_engineName = name;
    m_engineMessage = msg;
    Q_EMIT engineChanged();
    Q_EMIT capabilitiesChanged();
}

void VoiceController::updateState()
{
    const bool muted = m_config && m_config->micMuted();
    const bool neural = m_engineName == QLatin1String("neural");
    QString s;
    if (m_helperState == QLatin1String("speaking") || m_espeak || !m_espeakOpen.isEmpty())
        s = QStringLiteral("speaking");
    else if (neural && (m_helperState == QLatin1String("listening") || m_helperState == QLatin1String("transcribing")))
        s = m_helperState;
    else if (muted)
        s = QStringLiteral("muted");
    else if (neural)
        s = QStringLiteral("idle");
    else if (m_engineName == QLatin1String("starting") || m_engineName == QLatin1String("installing"))
        s = QStringLiteral("starting");
    else if (m_engineName == QLatin1String("basic") || (m_engineName == QLatin1String("error") && !m_espeakBin.isEmpty()))
        s = QStringLiteral("idle"); // eSpeak can still speak
    else
        s = QStringLiteral("unavailable");
    updateListenMode();
    if (s == m_state)
        return;
    m_state = s;
    Q_EMIT stateChanged();
    if ((capturing() || speaking()) && !m_levelTimer->isActive() && !m_testLevel)
        m_levelTimer->start();
}

// --- capability flags ---------------------------------------------------------------

QStringList VoiceController::sttModels() const
{
    if (m_engine->status() == VoiceEngine::Status::Unavailable)
        return {}; // nothing usable is installed
    QStringList out;
    auto collect = [&out](const QJsonValue &v) {
        if (v.isArray()) {
            const QJsonArray a = v.toArray();
            for (const QJsonValue &x : a)
                out << x.toString();
        } else if (v.isString()) {
            out << v.toString();
        }
    };
    collect(m_engine->probeResult().value(QStringLiteral("stt")));
    if (out.isEmpty())
        collect(m_engine->readyInfo().value(QStringLiteral("stt_models")));
    if (out.isEmpty())
        collect(m_engine->readyInfo().value(QStringLiteral("stt")));
    out.removeAll(QString());
    out.removeDuplicates();
    return out;
}

bool VoiceController::sttAvailable() const
{
    return m_engine->isReady() && !sttModels().isEmpty();
}

bool VoiceController::wakeAvailable() const
{
    return m_engine->isReady() && (m_engine->readyInfo().value(QStringLiteral("kws")).toBool() || sttAvailable());
}

bool VoiceController::captureAvailable() const
{
    return m_engine->isReady() && !m_captureTool.isEmpty() && m_micStatus != QLatin1String("error");
}

bool VoiceController::armed() const
{
    return m_engineName == QLatin1String("neural") && m_config && m_config->wakeEnabled()
        && !m_config->micMuted() && wakeAvailable() && m_micStatus != QLatin1String("error");
}

QString VoiceController::resolvedTtsEngine() const
{
    const QString pref = m_config ? m_config->ttsEngine() : QStringLiteral("auto");
    const bool neural = m_engine->isReady();
    const bool espeak = !m_espeakBin.isEmpty();
    if (pref == QLatin1String("espeak"))
        return espeak ? QStringLiteral("espeak") : QString();
    if (pref == QLatin1String("neural"))
        return neural ? QStringLiteral("kokoro") : QString();
    if (neural)
        return QStringLiteral("kokoro");
    return espeak ? QStringLiteral("espeak") : QString();
}

QVariantList VoiceController::voices() const
{
    QSet<QString> installed;
    const QJsonArray have = m_engine->readyInfo().value(QStringLiteral("voices")).toArray();
    for (const QJsonValue &v : have)
        installed.insert(v.toString());
    const bool ready = m_engine->isReady();
    QVariantList out;
    for (const CuratedVoice &v : kVoices) {
        const QString id = QString::fromLatin1(v.id);
        out << QVariantMap{
            {QStringLiteral("id"), id},
            {QStringLiteral("name"), tr2(v.name)},
            {QStringLiteral("accent"), tr2(v.accent)},
            {QStringLiteral("description"), tr2(v.description)},
            {QStringLiteral("available"), ready && (installed.isEmpty() || installed.contains(id))},
        };
    }
    return out;
}

QString VoiceController::statusText() const
{
    if (m_installing)
        return tr("Installing the natural voice… %1 %").arg(qRound(m_installProgress * 100));
    const bool muted = m_config && m_config->micMuted();
    if (m_engineName == QLatin1String("neural")) {
        QStringList parts;
        if (resolvedTtsEngine() == QLatin1String("espeak"))
            parts << tr("Natural voice ready") << tr("speaking with eSpeak NG");
        else
            parts << tr("Natural voice ready") << voiceName(m_config ? m_config->voice() : QStringLiteral("af_heart"));
        if (muted)
            parts << tr("microphone muted");
        else if (m_micStatus == QLatin1String("error"))
            parts << tr("microphone unavailable");
        else if (armed())
            parts << tr("listening for “Shadow”");
        else if (m_config && !m_config->wakeEnabled())
            parts << tr("wake word off");
        return parts.join(QStringLiteral(" · "));
    }
    if (m_engineName == QLatin1String("starting"))
        return tr("Starting the natural voice…");
    if (m_engineName == QLatin1String("error"))
        return tr("The voice engine failed · see details");
    // basic / missing: what is missing, and the fix.
    const bool helperMissing = m_engine->unavailableKind() == QLatin1String("helper");
    const QString what = helperMissing ? tr("voice helper not installed — reinstall Shadow Assistant")
                                       : tr("natural voice not installed — run shadow-voice-setup or use Install");
    if (m_engineName == QLatin1String("basic"))
        return tr("Basic voice (eSpeak NG) · %1").arg(what);
    return tr("No voice · %1").arg(what);
}

// --- listening ----------------------------------------------------------------------

QString VoiceController::newId(const QString &prefix)
{
    return prefix + QString::number(++m_seq);
}

QString VoiceController::listenBlocker() const
{
    if (m_config && m_config->micMuted())
        return tr("The microphone is muted.");
    if (m_engineName == QLatin1String("installing") || (m_installing && !m_engine->isReady()))
        return tr("The natural voice is being installed — try again when it is ready.");
    if (m_engineName == QLatin1String("starting"))
        return tr("The voice engine is still starting — try again in a moment.");
    if (m_engineName == QLatin1String("basic") || m_engineName == QLatin1String("missing"))
        return tr("Speech recognition is not installed. Run: shadow-voice-setup — or use Install in Settings.");
    if (m_engineName == QLatin1String("error"))
        return tr("The voice engine failed: %1").arg(m_engineMessage.section(u'\n', 0, 0));
    if (!sttAvailable())
        return tr("No speech recognition model is installed. Run: shadow-voice-setup");
    if (m_micStatus == QLatin1String("error"))
        return tr("Microphone not available: %1").arg(m_micMessage);
    if (m_captureTool.isEmpty())
        return tr("Microphone not available: no capture tool (pw-record, parec or arecord) was found.");
    return {};
}

void VoiceController::startListen(const QString &mode, double timeoutSecs)
{
    const QString prefix = mode == QLatin1String("yesno") ? QStringLiteral("yn")
        : mode == QLatin1String("followup")               ? QStringLiteral("f")
                                                           : QStringLiteral("l");
    const QString id = newId(prefix);
    if (!m_engine->send({{QStringLiteral("cmd"), QStringLiteral("listen")},
                         {QStringLiteral("id"), id},
                         {QStringLiteral("mode"), mode},
                         {QStringLiteral("timeout"), timeoutSecs}}))
        return;
    m_listenActive = true;
    m_listenId = id;
    m_listenMode = mode;
    m_wakeListen = false;
    m_listenWatchdog->start(int(timeoutSecs * 1000) + kListenGraceMs);
    updateListenMode();
}

void VoiceController::clearListen()
{
    m_listenActive = false;
    m_listenId.clear();
    m_listenMode.clear();
    m_listenWatchdog->stop();
    updateListenMode();
}

void VoiceController::updateListenMode()
{
    const bool helperListens = m_helperState == QLatin1String("listening")
        || m_helperState == QLatin1String("transcribing");
    QString mode;
    if (m_listenActive)
        mode = m_listenMode;
    else if (m_wakeListen && helperListens)
        mode = QStringLiteral("wake"); // the helper listens on its own: after "Shadow"
    if (mode == m_listenModeShown)
        return;
    m_listenModeShown = mode;
    Q_EMIT listenModeChanged();
}

void VoiceController::startListening()
{
    const QString why = listenBlocker();
    if (!why.isEmpty()) {
        Q_EMIT unavailable(why); // voiceTurn is left as it was (never switched on here)
        return;
    }
    if (m_listenActive)
        return;
    stopSpeech(true); // barge-in: the user wants to talk, not to listen
    const bool turnWasOn = m_voiceTurn;
    beginVoiceTurn();
    startListen(QStringLiteral("command"), kCommandTimeoutS);
    if (!m_listenActive) {
        if (!turnWasOn)
            endVoiceTurn();
        Q_EMIT unavailable(tr("The voice engine is not running."));
    } else {
        m_turnByListen = !turnWasOn;
    }
    updateState();
}

void VoiceController::finishListening()
{
    // Also ends a listen the helper started itself after a bare "Shadow".
    if (m_listenActive || capturing())
        m_engine->send({{QStringLiteral("cmd"), QStringLiteral("finish")}});
}

void VoiceController::toggleListening()
{
    if (m_listenActive || capturing())
        finishListening();
    else
        startListening();
}

void VoiceController::cancel()
{
    if (m_listenActive || capturing())
        m_engine->send({{QStringLiteral("cmd"), QStringLiteral("cancel")}});
    clearListen();
    m_yesNoTag.clear();
    stopSpeech(true);
    setPartial({});
    endVoiceTurn();
    updateState();
}

void VoiceController::handleFinal(const QJsonObject &ev)
{
    const QString text = ev.value(QStringLiteral("text")).toString().trimmed();
    const QString id = ev.value(QStringLiteral("id")).toString();
    const bool mine = m_listenActive && id == m_listenId;
    const QString mode = mine ? m_listenMode : ev.value(QStringLiteral("mode")).toString();
    if (mine)
        clearListen();
    setPartial({});
    if (!text.isEmpty() && text != m_lastHeard) {
        m_lastHeard = text;
        notifyLevels();
    }
    if (mode == QLatin1String("yesno")) {
        // The helper classifies the answer itself ("answer": "yes"/"no"/null);
        // without that (older helper) the text is classified here, same rules.
        const QString answer = ev.value(QStringLiteral("answer")).toString();
        handleYesNoAnswer(answer == QLatin1String("yes") || answer == QLatin1String("no") ? answer : text);
        updateState();
        return;
    }
    if (text.isEmpty()) {
        if (mode == QLatin1String("command") && mine) {
            endVoiceTurn();
            Q_EMIT unavailable(tr("I didn't catch that."));
        }
        updateState();
        return;
    }
    routeTranscript(text);
    updateState();
}

void VoiceController::handleTimeout(const QJsonObject &ev)
{
    const QString id = ev.value(QStringLiteral("id")).toString();
    setPartial({});
    if (!m_listenActive || id != m_listenId)
        return; // e.g. the helper's own listen after a bare "Shadow"
    const QString mode = m_listenMode;
    clearListen();
    if (mode == QLatin1String("command")) {
        endVoiceTurn();
        Q_EMIT unavailable(tr("I didn't catch that."));
    } else if (mode == QLatin1String("yesno")) {
        m_yesNoTag.clear(); // no answer: the card decides, nothing is said
    }
    updateState();
}

void VoiceController::routeTranscript(const QString &text)
{
    if (!m_agent) {
        // No agent wiring: QML routes the transcript.
        beginVoiceTurn();
        Q_EMIT transcriptReady(text);
        return;
    }
    if (m_agent->canSend()) {
        beginVoiceTurn();
        m_turnByListen = false;
        m_replySuppressed = false;
        m_replyId.clear();
        m_stream.reset();
        Q_EMIT transcriptReady(text);
        m_agent->sendMessage(text);
        return;
    }
    if (m_agent->hasPending()) {
        // "yes" / "no" while a card waits: it answers the card's question —
        // the first waiting call only. Any other call is asked on its own
        // (aloud, now that this is a voice turn), never run unseen.
        const Answer a = parseYesNo(text);
        const QString first = m_agent->firstPendingCall().value(QStringLiteral("id")).toString();
        if (a != Answer::Unclear && !first.isEmpty()) {
            beginVoiceTurn();
            m_turnByListen = false;
            if (m_yesNoTag == first)
                m_yesNoTag.clear(); // answered: no yes/no listen for it after the question
            m_agent->confirmTool(-1, first, a == Answer::Yes);
            askNextPending();
            return;
        }
    }
    // Not taken: a push-to-talk must not turn the running (typed) request
    // into a spoken one.
    if (m_turnByListen) {
        m_turnByListen = false;
        endVoiceTurn();
    }
    Q_EMIT heard(text);
    if (m_agent->config() && !m_agent->config()->apiKeyConfigured())
        Q_EMIT unavailable(tr("Add your Claude API key in Settings first."));
    else
        Q_EMIT unavailable(tr("I'm still working on the last request."));
}

void VoiceController::handleYesNoAnswer(const QString &text)
{
    const QString tag = m_yesNoTag;
    m_yesNoTag.clear();
    if (tag.isEmpty())
        return;
    const Answer a = parseYesNo(text);
    if (a == Answer::Unclear) {
        if (!text.isEmpty())
            Q_EMIT unavailable(tr("I didn't catch a yes or no — use the buttons on the card."));
        return;
    }
    const bool yes = a == Answer::Yes;
    Q_EMIT yesNoResult(tag, yes);
    if (m_agent) {
        m_agent->confirmTool(-1, tag, yes);
        askNextPending();
    }
}

void VoiceController::askNextPending()
{
    // One call answered by voice, another one still waits (several calls in
    // one turn): during a voice turn it is asked aloud on its own.
    if (!m_agent || !m_voiceTurn || !m_yesNoTag.isEmpty())
        return;
    const QVariantMap next = m_agent->firstPendingCall();
    if (next.isEmpty())
        return;
    onConfirmationNeeded(next.value(QStringLiteral("summary")).toString(),
                         next.value(QStringLiteral("preview")).toString(), next.value(QStringLiteral("id")).toString());
}

// --- speaking -----------------------------------------------------------------------

void VoiceController::afterSpeech(const QString &id, std::function<void(bool)> then)
{
    m_afterSpeech.insert(id, std::move(then));
}

void VoiceController::onSpeechEnded(const QString &id, bool interrupted)
{
    const auto it = m_afterSpeech.find(id);
    if (it != m_afterSpeech.end()) {
        const std::function<void(bool)> then = it.value();
        m_afterSpeech.erase(it);
        then(interrupted);
    }
    updateState();
}

void VoiceController::queueSpeech(const QString &id, const QString &text, bool more, const QStringList &sentences)
{
    QStringList parts = sentences;
    if (parts.isEmpty() && !text.trimmed().isEmpty())
        parts << text;
    const QString tts = resolvedTtsEngine();
    // A reply eSpeak already reads (the helper vanished in the middle of it)
    // stays with eSpeak to its end, even once the helper is back: two voices
    // must never read different parts of one reply at the same time.
    const bool espeakOwns = !m_espeakBin.isEmpty() && m_espeakOpen.contains(id);
    if (!espeakOwns && tts == QLatin1String("kokoro")
        && m_engine->send({{QStringLiteral("cmd"), QStringLiteral("speak")},
                           {QStringLiteral("id"), id},
                           {QStringLiteral("text"), text},
                           {QStringLiteral("more"), more}})) {
        m_helperSpeech.insert(id);
        if (!parts.isEmpty()) {
            SpeechTrack &track = m_tracks[id];
            for (const QString &p : std::as_const(parts)) {
                track.texts << p;
                track.words << matchWords(p);
            }
        }
        return;
    }
    if (!m_espeakBin.isEmpty() && (!tts.isEmpty() || espeakOwns)) {
        // eSpeak NG ("basic voice"), also when the helper vanished just now.
        m_espeakOpen.insert(id);
        m_espeakQueue.append({id, text, more, parts.size() == 1 ? parts.first() : QString()});
        pumpEspeak();
        updateState();
        return;
    }
    // Nothing can speak: the reply stays on screen; what waits for the end of
    // this speech still runs (on the next turn of the event loop).
    if (!more)
        QTimer::singleShot(0, this, [this, id]() { onSpeechEnded(id, false); });
}

void VoiceController::pumpEspeak()
{
    if (m_espeak)
        return;
    while (!m_espeakQueue.isEmpty()) {
        const EspeakItem item = m_espeakQueue.takeFirst();
        if (item.text.trimmed().isEmpty()) {
            if (!item.more) {
                m_espeakOpen.remove(item.id);
                onSpeechEnded(item.id, false);
            }
            continue;
        }
        m_espeakCurrent = item;
        m_espeak = new QProcess(this);
        m_espeak->setStandardOutputFile(QProcess::nullDevice());
        m_espeak->setStandardErrorFile(QProcess::nullDevice());
        const double rate = m_config ? m_config->speechRate() : 1.0;
        const int wpm = qBound(80, int(std::lround(kEspeakWpm * rate)), 450);
        connect(m_espeak, &QProcess::finished, this, [this](int, QProcess::ExitStatus) {
            QProcess *p = m_espeak;
            m_espeak = nullptr;
            if (p)
                p->deleteLater();
            const EspeakItem done = m_espeakCurrent;
            m_espeakCurrent = {};
            if (!done.more) {
                m_espeakOpen.remove(done.id);
                onSpeechEnded(done.id, false);
            }
            pumpEspeak();
            if (!m_espeak)
                setSpokenSentence({}); // nothing follows (yet)
            updateState();
        });
        connect(m_espeak, &QProcess::errorOccurred, this, [this](QProcess::ProcessError err) {
            if (err != QProcess::FailedToStart || !m_espeak)
                return;
            // No finished() follows a failed start.
            const QString why = m_espeak->errorString();
            m_espeak->deleteLater();
            m_espeak = nullptr;
            const EspeakItem failed = m_espeakCurrent;
            m_espeakCurrent = {};
            m_espeakQueue.clear();
            m_espeakOpen.clear();
            setSpokenSentence({});
            detectTools();
            Q_EMIT unavailable(tr("eSpeak NG could not be started: %1").arg(why));
            onSpeechEnded(failed.id, true);
            updateEngine();
            updateState();
        });
        m_espeak->start(m_espeakBin, {QStringLiteral("-v"), QStringLiteral("en-us+f3"), QStringLiteral("-s"),
                                      QString::number(wpm), QStringLiteral("--"), item.text});
        // eSpeak says exactly what it was given: one sentence is known
        // exactly; several in one item cannot be followed.
        setSpokenSentence(item.sentence);
        if (!m_testLevel && !m_levelTimer->isActive())
            m_levelTimer->start();
        return;
    }
}

void VoiceController::stopSpeech(bool suppressReply)
{
    if (!m_helperSpeech.isEmpty() || m_helperState == QLatin1String("speaking"))
        m_engine->send({{QStringLiteral("cmd"), QStringLiteral("stop")}});
    m_helperSpeech.clear();
    forgetSpeechTracks();
    setSpokenSentence({});
    m_espeakQueue.clear();
    m_espeakOpen.clear();
    if (m_espeak) {
        m_espeakCurrent.more = true; // its finished() must not report a normal end
        m_espeak->kill();            // finished() cleans up
    }
    // Everything that waited for this speech learns it was interrupted
    // (a yes/no prompt then leaves the decision to the card).
    const auto pending = m_afterSpeech;
    m_afterSpeech.clear();
    for (auto it = pending.cbegin(); it != pending.cend(); ++it)
        it.value()(true);
    if (suppressReply) {
        m_replySuppressed = true;
        m_replyId.clear();
        m_stream.reset();
    }
}

void VoiceController::speak(const QString &text)
{
    if (!m_config || !m_config->speakReplies())
        return;
    const QStringList sentences = speechSentences(text);
    const QString t = sentences.join(u' '); // = SpeechText::forSpeech(text)
    if (t.isEmpty())
        return;
    if (!ttsAvailable()) {
        Q_EMIT unavailable(m_engineMessage.isEmpty() ? tr("No voice is available to speak with.") : m_engineMessage);
        return;
    }
    stopSpeech(true);
    queueSpeech(newId(QStringLiteral("s")), t, false, sentences);
    updateState();
}

void VoiceController::stopSpeaking()
{
    stopSpeech(true);
    updateState();
}

void VoiceController::previewVoice(const QString &id)
{
    if (!m_engine->isReady()) {
        Q_EMIT unavailable(tr("The natural voices need the natural voice engine. Run: shadow-voice-setup — or "
                              "use Install in Settings."));
        return;
    }
    stopSpeech(true);
    QJsonObject cmd{{QStringLiteral("cmd"), QStringLiteral("preview")}, {QStringLiteral("voice"), id}};
    const QString name = voiceName(id);
    if (name != id)
        cmd.insert(QStringLiteral("text"), tr("Hi, I'm %1. This is how I sound as your Shadow assistant.").arg(name));
    if (m_engine->send(cmd))
        m_helperSpeech.insert(QStringLiteral("preview"));
}

void VoiceController::beginVoiceTurn()
{
    if (m_voiceTurn)
        return;
    if (!m_agent || !m_agent->busy()) {
        // A new turn starts: nothing of an earlier reply is pending.
        m_replySuppressed = false;
        m_replyId.clear();
        m_stream.reset();
    }
    m_voiceTurn = true;
    Q_EMIT voiceTurnChanged();
}

void VoiceController::endVoiceTurn()
{
    m_turnByListen = false;
    if (!m_voiceTurn)
        return;
    if (!m_replyId.isEmpty()) {
        // Ended from outside (QML) while a reply was being spoken: close its
        // speech so the voice does not wait for text that will never come.
        const QString id = m_replyId;
        m_replyId.clear();
        m_stream.reset();
        queueSpeech(id, QString(), false);
    }
    m_voiceTurn = false;
    Q_EMIT voiceTurnChanged();
}

void VoiceController::confirmBySpeech(const QString &tag, const QString &prompt)
{
    m_yesNoTag = tag;
    const QStringList promptSentences = speechSentences(prompt);
    const QString text = promptSentences.join(u' '); // = SpeechText::forSpeech(prompt)
    if (!m_config || !m_config->speakReplies() || !ttsAvailable() || text.isEmpty()) {
        // Without a spoken question there is no spoken answer: the card decides.
        m_yesNoTag.clear();
        return;
    }
    // The question closes the reply stream that announced the tool call, so
    // it is heard after whatever was said before it.
    QString id = m_replySuppressed ? QString() : m_replyId;
    m_replyId.clear();
    const QStringList rest = m_stream.flush();
    if (!m_replySuppressed) {
        for (const QString &s : rest) {
            if (id.isEmpty())
                id = newId(QStringLiteral("r"));
            queueSpeech(id, s, true);
        }
    }
    if (id.isEmpty())
        id = newId(QStringLiteral("c"));
    afterSpeech(id, [this, tag](bool interrupted) {
        if (m_yesNoTag != tag)
            return; // answered or abandoned meanwhile
        if (interrupted || m_listenActive || !listenBlocker().isEmpty()) {
            m_yesNoTag.clear(); // cannot listen for the answer: the card decides
            return;
        }
        startListen(QStringLiteral("yesno"), kYesNoTimeoutS);
        if (!m_listenActive)
            m_yesNoTag.clear();
        updateState();
    });
    queueSpeech(id, text, false, promptSentences);
    updateState();
}

// --- agent wiring (spec §2.4) ---------------------------------------------------------

bool VoiceController::replySpeechWanted() const
{
    return m_voiceTurn && m_config && m_config->speakReplies() && !m_replySuppressed && ttsAvailable();
}

void VoiceController::onReplyDelta(const QString &chunk)
{
    if (!replySpeechWanted())
        return;
    const QStringList sentences = m_stream.push(chunk);
    for (const QString &s : sentences) {
        if (m_replyId.isEmpty())
            m_replyId = newId(QStringLiteral("r"));
        queueSpeech(m_replyId, s, true);
    }
}

void VoiceController::onReplySegmentFinished()
{
    if (!replySpeechWanted()) {
        m_stream.reset();
        return;
    }
    // A tool call may follow: say the rest of this part now.
    const QStringList rest = m_stream.flush();
    for (const QString &s : rest) {
        if (m_replyId.isEmpty())
            m_replyId = newId(QStringLiteral("r"));
        queueSpeech(m_replyId, s, true);
    }
}

void VoiceController::onAssistantReplyReady(const QString &text)
{
    Q_UNUSED(text); // spoken as it streamed (replyDelta)
    if (m_voiceTurn)
        finishVoiceReply(true);
}

void VoiceController::finishVoiceReply(bool followUp)
{
    const bool wanted = replySpeechWanted();
    QStringList rest = m_stream.flush();
    if (wanted && (!m_replyId.isEmpty() || !rest.isEmpty())) {
        const QString id = m_replyId.isEmpty() ? newId(QStringLiteral("r")) : m_replyId;
        const QString last = rest.isEmpty() ? QString() : rest.takeLast();
        for (const QString &s : std::as_const(rest))
            queueSpeech(id, s, true);
        if (followUp)
            afterSpeech(id, [this](bool interrupted) {
                if (!interrupted)
                    maybeFollowUp();
            });
        queueSpeech(id, last, false); // more:false — the end of this reply
    }
    m_replyId.clear();
    m_replySuppressed = false;
    endVoiceTurn();
    updateState();
}

void VoiceController::maybeFollowUp()
{
    if (!m_config || !m_config->followUp())
        return;
    if (m_voiceTurn || m_listenActive || !m_yesNoTag.isEmpty())
        return; // something else already started
    if (!listenBlocker().isEmpty())
        return; // muted, no speech recognition, no microphone: just don't
    if (m_agent && !m_agent->canSend())
        return;
    startListen(QStringLiteral("followup"), kFollowUpTimeoutS);
    updateState();
}

void VoiceController::onConfirmationNeeded(const QString &summary, const QString &preview, const QString &callId)
{
    Q_UNUSED(preview); // shown on the card; a command line read aloud helps nobody
    if (!m_voiceTurn)
        return;
    confirmBySpeech(callId, tr("I'd like to %1. Shall I go ahead?").arg(lowerFirst(summary)));
}

void VoiceController::onAgentError(const QString &text)
{
    Q_UNUSED(text); // the UI shows it
    if (!m_voiceTurn)
        return;
    stopSpeech(true);
    if (m_listenActive) {
        m_engine->send({{QStringLiteral("cmd"), QStringLiteral("cancel")}});
        clearListen();
        setPartial({});
    }
    m_yesNoTag.clear();
    if (m_config && m_config->speakReplies())
        queueSpeech(newId(QStringLiteral("e")), tr("Sorry, that didn't work."), false);
    m_replySuppressed = false;
    endVoiceTurn();
    updateState();
}

void VoiceController::onAgentStopped()
{
    // The Stop button: the reply is abandoned, so is its speech.
    stopSpeech(true);
    if (m_listenActive && m_listenMode != QLatin1String("command")) {
        m_engine->send({{QStringLiteral("cmd"), QStringLiteral("cancel")}});
        clearListen();
        setPartial({});
    }
    m_yesNoTag.clear();
    m_replySuppressed = false;
    if (!m_listenActive) // a push-to-talk in progress is a new turn: keep it
        endVoiceTurn();
    updateState();
}

void VoiceController::onAgentPhaseChanged()
{
    if (!m_voiceTurn || !m_agent)
        return;
    // A turn can end without assistantReplyReady (an empty final answer, the
    // tool-round limit). When the agent has settled, close the turn — on the
    // next event-loop turn, so a reply signal emitted right after the phase
    // change is handled first.
    QTimer::singleShot(0, this, [this]() {
        if (!m_voiceTurn || !m_agent || m_listenActive)
            return;
        const QString phase = m_agent->phase();
        const bool settled = !m_agent->busy() && !m_agent->hasPending()
            && (phase == QLatin1String("idle") || phase == QLatin1String("setup"));
        if (settled)
            finishVoiceReply(false);
    });
}

// --- levels ---------------------------------------------------------------------------

void VoiceController::notifyLevels()
{
    // partialText/lastHeard share the NOTIFY of the levels: while the level
    // timer runs they go out with its next tick, so QML is never notified
    // more than ~30 times a second.
    if (m_levelTimer->isActive())
        m_levelsDirty = true;
    else
        Q_EMIT levelsChanged();
}

void VoiceController::setPartial(const QString &text)
{
    if (m_testPartial || m_partial == text)
        return;
    m_partial = text;
    notifyLevels();
}

// --- the sentence being spoken ------------------------------------------------------------

void VoiceController::setSpokenSentence(const QString &sentence)
{
    if (sentence == m_spokenSentence)
        return;
    m_spokenSentence = sentence;
    Q_EMIT spokenSentenceChanged();
}

void VoiceController::forgetSpeechTracks()
{
    m_tracks.clear();
}

// The helper says a sentence (event "sentence", just before its audio plays).
// Its text is the helper's normalised form — numbers, units, paths, URLs and
// acronyms spelled out ("nine point four gigabytes"), long sentences cut at
// commas — so it is matched word by word against what was sent for this id,
// in order: the sent sentence that shares the most words with it, clearly
// more than any other, at least half of the helper's words (a third when no
// other sentence shares any). A further piece of the sentence already shown
// keeps it. Anything less certain shows nothing rather than a wrong sentence.
void VoiceController::onHelperSentence(const QString &id, const QString &text)
{
    const auto it = m_tracks.find(id);
    if (it == m_tracks.end()) {
        setSpokenSentence({}); // e.g. a voice preview: not part of any reply
        return;
    }
    SpeechTrack &t = it.value();
    const QStringList heard = matchWords(text);
    if (heard.isEmpty()) {
        setSpokenSentence({});
        return;
    }
    int best = -1, bestScore = 0, second = 0, bestEnd = 0;
    const int last = qMin(int(t.texts.size()) - 1, t.cur + kSentenceLookahead);
    for (int c = t.cur; c <= last; ++c) {
        const int start = c == t.cur ? t.off : 0;
        int end = 0;
        const int score = commonWords(heard, t.words.at(c).mid(start), &end);
        if (score > bestScore) {
            second = bestScore;
            bestScore = score;
            best = c;
            bestEnd = start + end;
        } else if (score > second) {
            second = score; // a tie with the best makes it ambiguous
        }
    }
    const int n = int(heard.size());
    const bool confident = best >= 0 && bestScore > second && bestScore >= qMin(2, n)
        && (2 * bestScore >= n || (second == 0 && 3 * bestScore >= n));
    // The helper cut the sentence on screen into pieces: this one shares
    // words only with what is left of it.
    const bool continues = !confident && best == t.cur && t.off > 0 && bestScore >= 1 && second == 0
        && m_spokenSentence == t.texts.at(t.cur);
    if (confident || continues) {
        t.cur = best;
        t.off = bestEnd;
        setSpokenSentence(t.texts.at(best));
    } else {
        setSpokenSentence({});
    }
}

void VoiceController::noteLevel(const QString &source, double rms)
{
    if (m_testLevel)
        return;
    const qint64 now = m_clock.elapsed();
    const double v = qBound(0.0, rms, 1.0);
    const bool tts = source == QLatin1String("tts");
    if (tts) {
        m_ttsRaw = v;
        m_ttsAt = now;
    } else {
        m_micRaw = v;
        m_micAt = now;
    }
    // While the wake word is armed the helper reports the microphone level
    // continuously; it is only shown while capturing, so idle levels must not
    // keep a 30 Hz timer running. (updateState() starts it when needed.)
    if (!m_levelTimer->isActive() && (tts ? speaking() : capturing()))
        m_levelTimer->start();
}

void VoiceController::tickLevels()
{
    const qint64 now = m_clock.elapsed();
    const double micTarget = capturing() && now - m_micAt < kLevelStaleMs ? m_micRaw : 0.0;
    double ttsTarget = 0.0;
    if (m_helperState == QLatin1String("speaking") && now - m_ttsAt < kLevelStaleMs) {
        ttsTarget = m_ttsRaw;
    } else if (m_espeak) {
        // eSpeak NG plays by itself, so there is no real envelope to show:
        // a calm synthetic one keeps the presence alive while it talks.
        const double t = double(now);
        ttsTarget = 0.28 + 0.22 * std::abs(std::sin(t / 95.0)) * (0.6 + 0.4 * std::sin(t / 410.0));
    }
    // Fast attack, slower release: speech looks lively, silence settles smoothly.
    auto approach = [](qreal current, double target) {
        const double k = target > current ? 0.55 : 0.2;
        double v = current + (target - current) * k;
        if (target <= 0.0 && v < 0.004)
            v = 0.0;
        if (std::abs(v - current) < 0.002 && target > 0.0)
            v = current; // not worth a notification
        return qreal(v);
    };
    const qreal mic = approach(m_micLevel, micTarget);
    const qreal tts = approach(m_speechLevel, ttsTarget);
    const bool changed = mic != m_micLevel || tts != m_speechLevel || m_levelsDirty;
    m_micLevel = mic;
    m_speechLevel = tts;
    m_levelsDirty = false;
    if (changed)
        Q_EMIT levelsChanged();
    if (mic == 0.0 && tts == 0.0 && micTarget == 0.0 && ttsTarget == 0.0)
        m_levelTimer->stop();
}

// --- refresh and install -----------------------------------------------------------------

void VoiceController::refresh()
{
    detectTools();
    m_micStatus.clear();
    m_micMessage.clear();
    m_started = true;
    m_engine->restart();
    updateEngine();
    updateState();
    Q_EMIT capabilitiesChanged();
}

void VoiceController::installEngine()
{
    if (m_installing)
        return;
    QString why;
    const QString tool = locateSetupTool(&why);
    if (tool.isEmpty()) {
        m_installStatus = why;
        Q_EMIT installChanged();
        Q_EMIT unavailable(why);
        return;
    }
    m_install = new QProcess(this);
    m_install->setProcessChannelMode(QProcess::SeparateChannels);
    m_installBuf.clear();
    connect(m_install, &QProcess::readyReadStandardOutput, this, &VoiceController::onInstallOutput);
    connect(m_install, &QProcess::finished, this, [this](int code, QProcess::ExitStatus st) {
        onInstallFinished(st == QProcess::NormalExit ? code : -1, QString());
    });
    connect(m_install, &QProcess::errorOccurred, this, [this](QProcess::ProcessError err) {
        if (err == QProcess::FailedToStart && m_install)
            onInstallFinished(-1, m_install->errorString());
    });
    m_installing = true;
    m_installCancelled = false;
    m_installProgress = 0;
    m_installError.clear();
    m_installStatus = tr("Starting the installer…");
    Q_EMIT installChanged();
    updateEngine();
    updateState();
    Q_EMIT capabilitiesChanged();
    m_install->start(tool, {QStringLiteral("--user"), QStringLiteral("--progress-json")});
    m_install->closeWriteChannel();
}

void VoiceController::cancelInstall()
{
    if (!m_install)
        return;
    m_installCancelled = true;
    m_install->terminate();
    QTimer::singleShot(3000, this, [p = QPointer<QProcess>(m_install)]() {
        if (p && p->state() != QProcess::NotRunning)
            p->kill();
    });
}

void VoiceController::onInstallOutput()
{
    if (!m_install)
        return;
    m_installBuf += m_install->readAllStandardOutput();
    bool changed = false;
    qsizetype nl;
    while ((nl = m_installBuf.indexOf('\n')) >= 0) {
        const QByteArray line = m_installBuf.left(nl).trimmed();
        m_installBuf.remove(0, nl + 1);
        if (!line.startsWith('{'))
            continue;
        QJsonParseError perr{};
        const QJsonObject o = QJsonDocument::fromJson(line, &perr).object();
        if (perr.error != QJsonParseError::NoError)
            continue;
        // Progress as a fraction, a percentage or bytes done/total — whatever
        // the line carries; the status text from message/status/stage.
        double p = -1;
        if (o.value(QStringLiteral("progress")).isDouble())
            p = o.value(QStringLiteral("progress")).toDouble();
        else if (o.value(QStringLiteral("fraction")).isDouble())
            p = o.value(QStringLiteral("fraction")).toDouble();
        else if (o.value(QStringLiteral("percent")).isDouble())
            p = o.value(QStringLiteral("percent")).toDouble() / 100.0;
        else if (o.value(QStringLiteral("total")).toDouble() > 0)
            p = o.value(QStringLiteral("done")).toDouble() / o.value(QStringLiteral("total")).toDouble();
        if (p > 1.0 && p <= 100.0 && !o.contains(QStringLiteral("percent")))
            p /= 100.0; // a percentage in "progress"
        if (p >= 0) {
            p = qBound(0.0, p, 1.0);
            if (std::abs(p - m_installProgress) >= 0.002) {
                m_installProgress = p;
                changed = true;
            }
        }
        QString text = o.value(QStringLiteral("message")).toString();
        if (text.isEmpty())
            text = o.value(QStringLiteral("status")).toString();
        if (text.isEmpty())
            text = o.value(QStringLiteral("stage")).toString();
        // The phase is called "event" or "step" ({"step":"error","code":N,…}).
        QString ev = o.value(QStringLiteral("event")).toString();
        if (ev.isEmpty())
            ev = o.value(QStringLiteral("step")).toString();
        const bool isError = ev == QLatin1String("error") || o.value(QStringLiteral("error")).isString()
            || (o.contains(QStringLiteral("ok")) && !o.value(QStringLiteral("ok")).toBool());
        if (isError) {
            const QString err = o.value(QStringLiteral("error")).toString();
            m_installError = !text.isEmpty() ? text : !err.isEmpty() ? err : tr("The installer reported an error.");
        }
        if (!text.isEmpty() && text != m_installStatus) {
            m_installStatus = text;
            changed = true;
        }
    }
    if (changed) {
        Q_EMIT installChanged();
        Q_EMIT capabilitiesChanged(); // statusText shows the percentage
        if (m_engineName == QLatin1String("installing"))
            updateEngine();
    }
}

void VoiceController::onInstallFinished(int exitCode, const QString &failure)
{
    if (!m_install)
        return;
    // The last progress lines (an error line may lack its newline).
    m_installBuf += m_install->readAllStandardOutput();
    if (!m_installBuf.isEmpty() && !m_installBuf.endsWith('\n'))
        m_installBuf += '\n';
    onInstallOutput();
    QProcess *p = m_install;
    m_install = nullptr;
    p->disconnect(this);
    const QString err = QString::fromUtf8(p->readAllStandardError()).trimmed();
    p->deleteLater();
    m_installing = false;
    if (m_installCancelled) {
        m_installStatus = tr("Installation cancelled.");
    } else if (exitCode == 0 && failure.isEmpty()) {
        m_installProgress = 1;
        m_installStatus = tr("The natural voice is installed.");
    } else {
        m_installStatus = !failure.isEmpty() ? tr("The installer could not be started: %1").arg(failure)
            : !m_installError.isEmpty()      ? m_installError
                                             : tr("The installer failed (exit %1). %2")
                                                   .arg(exitCode)
                                                   .arg(err.section(u'\n', -1).trimmed());
        Q_EMIT unavailable(m_installStatus);
    }
    m_installBuf.clear();
    Q_EMIT installChanged();
    // Whatever happened, look again: the engine may be complete now.
    refresh();
}

// --- diagnostics ------------------------------------------------------------------------

QJsonObject VoiceController::diagnostics() const
{
    const QMetaEnum statusEnum = QMetaEnum::fromType<VoiceEngine::Status>();
    QJsonObject config;
    if (m_config) {
        config = QJsonObject{
            {QStringLiteral("wakeEnabled"), m_config->wakeEnabled()},
            {QStringLiteral("micMuted"), m_config->micMuted()},
            {QStringLiteral("speakReplies"), m_config->speakReplies()},
            {QStringLiteral("voice"), m_config->voice()},
            {QStringLiteral("speechRate"), m_config->speechRate()},
            {QStringLiteral("sttModel"), m_config->sttModel()},
            {QStringLiteral("wakeSensitivity"), m_config->wakeSensitivity()},
            {QStringLiteral("followUp"), m_config->followUp()},
            {QStringLiteral("inputDevice"), m_config->inputDevice()},
            {QStringLiteral("ttsEngine"), m_config->ttsEngine()},
        };
    }
    return QJsonObject{
        {QStringLiteral("engine"), m_engineName},
        {QStringLiteral("engineMessage"), m_engineMessage},
        {QStringLiteral("state"), m_state},
        {QStringLiteral("statusText"), statusText()},
        {QStringLiteral("helper"), QJsonObject{
             {QStringLiteral("program"), m_engine->program()},
             {QStringLiteral("status"), QString::fromLatin1(statusEnum.valueToKey(int(m_engine->status())))},
             {QStringLiteral("state"), m_helperState},
             {QStringLiteral("probe"), m_engine->probeResult()},
             {QStringLiteral("probeExit"), m_engine->probeExitCode()},
             {QStringLiteral("ready"), m_engine->readyInfo()},
             {QStringLiteral("recentFailures"), m_engine->recentFailures()},
             {QStringLiteral("stderrTail"), toJsonArray(m_engine->stderrTail(12))},
         }},
        {QStringLiteral("capture"), QJsonObject{
             {QStringLiteral("tool"), m_captureTool},
             {QStringLiteral("micStatus"), m_micStatus},
             {QStringLiteral("micMessage"), m_micMessage},
         }},
        {QStringLiteral("capabilities"), QJsonObject{
             {QStringLiteral("tts"), ttsAvailable()},
             {QStringLiteral("stt"), sttAvailable()},
             {QStringLiteral("wake"), wakeAvailable()},
             {QStringLiteral("capture"), captureAvailable()},
             {QStringLiteral("typeText"), typeAvailable()},
             {QStringLiteral("armed"), armed()},
             {QStringLiteral("resolvedTtsEngine"), resolvedTtsEngine()},
             {QStringLiteral("sttModels"), toJsonArray(sttModels())},
         }},
        {QStringLiteral("tools"), QJsonObject{
             {QStringLiteral("espeakNg"), m_espeakBin},
             {QStringLiteral("setup"), locateSetupTool()},
             {QStringLiteral("wtype"), m_wtype},
             {QStringLiteral("xdotool"), m_xdotool},
         }},
        {QStringLiteral("voiceTurn"), m_voiceTurn},
        {QStringLiteral("spokenSentence"), m_spokenSentence},
        {QStringLiteral("listen"), QJsonObject{
             {QStringLiteral("active"), m_listenActive},
             {QStringLiteral("id"), m_listenId},
             {QStringLiteral("mode"), m_listenMode},
             {QStringLiteral("shown"), m_listenModeShown},
         }},
        {QStringLiteral("install"), QJsonObject{
             {QStringLiteral("installing"), m_installing},
             {QStringLiteral("progress"), m_installProgress},
             {QStringLiteral("status"), m_installStatus},
         }},
        {QStringLiteral("config"), config},
    };
}

QJsonObject VoiceController::probeDiagnostics(AssistantConfig *config)
{
    QString helperWhy;
    const QString helper = VoiceEngine::locateHelper(&helperWhy);
    QJsonObject probe;
    int exitCode = -1;
    QString probeError;
    if (!helper.isEmpty())
        probe = VoiceEngine::probeSync(helper, &exitCode, &probeError);
    const QString espeak = QStandardPaths::findExecutable(QStringLiteral("espeak-ng"));
    QString setupWhy;
    const QString setup = locateSetupTool(&setupWhy);
    const bool usable = !helper.isEmpty() && (exitCode == 0 || exitCode == 5);
    const QString engine = usable ? QStringLiteral("neural")
        : espeak.isEmpty()        ? QStringLiteral("missing")
                                  : QStringLiteral("basic");
    QJsonObject out{
        {QStringLiteral("probeOnly"), true}, // the helper was not started; no microphone was opened
        {QStringLiteral("engine"), engine},
        {QStringLiteral("helper"), helper},
        {QStringLiteral("probe"), probe},
        {QStringLiteral("probeExit"), exitCode},
        {QStringLiteral("espeakNg"), espeak},
        {QStringLiteral("setupTool"), setup},
        {QStringLiteral("typeTools"), QJsonObject{
             {QStringLiteral("wtype"), !QStandardPaths::findExecutable(QStringLiteral("wtype")).isEmpty()},
             {QStringLiteral("xdotool"), !QStandardPaths::findExecutable(QStringLiteral("xdotool")).isEmpty()},
         }},
    };
    if (helper.isEmpty())
        out.insert(QStringLiteral("helperMissing"), helperWhy);
    if (!probeError.isEmpty())
        out.insert(QStringLiteral("probeError"), probeError);
    if (setup.isEmpty())
        out.insert(QStringLiteral("setupMissing"), setupWhy);
    if (config) {
        out.insert(QStringLiteral("config"), QJsonObject{
            {QStringLiteral("wakeEnabled"), config->wakeEnabled()},
            {QStringLiteral("micMuted"), config->micMuted()},
            {QStringLiteral("speakReplies"), config->speakReplies()},
            {QStringLiteral("voice"), config->voice()},
            {QStringLiteral("speechRate"), config->speechRate()},
            {QStringLiteral("sttModel"), config->sttModel()},
            {QStringLiteral("wakeSensitivity"), config->wakeSensitivity()},
            {QStringLiteral("followUp"), config->followUp()},
            {QStringLiteral("inputDevice"), config->inputDevice()},
            {QStringLiteral("ttsEngine"), config->ttsEngine()},
        });
    }
    return out;
}
