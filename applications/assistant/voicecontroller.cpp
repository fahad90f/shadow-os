// SPDX-License-Identifier: GPL-3.0-or-later
#include "voicecontroller.h"

#include "assistantconfig.h"

#include <command.h>

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QTimer>

using Shadow::Command;
using Shadow::CommandResult;

namespace {

// Candidate acoustic-model directories for pocketsphinx (en-us), probed in
// order. The package pocketsphinx-en-us installs under /usr/share/pocketsphinx.
const QStringList kModelProbe = {
    QStringLiteral("/usr/share/pocketsphinx/model/en-us/en-us"),
    QStringLiteral("/usr/share/pocketsphinx/model/en-us"),
    QStringLiteral("/usr/local/share/pocketsphinx/model/en-us/en-us"),
    QStringLiteral("/usr/local/share/pocketsphinx/model/en-us"),
};

QString tmpWav(const QString &stem)
{
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::TempLocation);
    return dir + QStringLiteral("/shadow-assistant-%1-%2.wav")
                     .arg(stem)
                     .arg(QDateTime::currentMSecsSinceEpoch());
}

// Interpret a transcript hypothesis as a yes/no answer.
bool looksLikeYes(const QString &text)
{
    const QString t = text.toLower();
    static const QStringList yes = {QStringLiteral("yes"), QStringLiteral("yeah"), QStringLiteral("yep"),
                                    QStringLiteral("sure"), QStringLiteral("okay"), QStringLiteral("ok"),
                                    QStringLiteral("do it"), QStringLiteral("go ahead"), QStringLiteral("run it"),
                                    QStringLiteral("affirmative"), QStringLiteral("confirm")};
    for (const QString &w : yes)
        if (t.contains(w))
            return true;
    return false;
}

} // namespace

VoiceController::VoiceController(QObject *parent)
    : QObject(parent)
{
    refresh();
}

VoiceController::~VoiceController()
{
    stopWakeListener();
    if (m_capture) { m_capture->kill(); m_capture->waitForFinished(500); }
    if (m_speak) { m_speak->kill(); m_speak->waitForFinished(500); }
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

void VoiceController::onConfigChanged()
{
    syncWakeListener();
    // Muted/engine changes affect the resting state and capability summary.
    if (m_state == QLatin1String("idle") || m_state == QLatin1String("muted")
        || m_state == QLatin1String("unavailable"))
        setState(m_config && m_config->micMuted() ? QStringLiteral("muted")
                 : anyEngine() || captureAvailable() ? QStringLiteral("idle")
                 : QStringLiteral("unavailable"));
    Q_EMIT capabilitiesChanged();
}

void VoiceController::refresh()
{
    m_piper = Command::available(QStringLiteral("piper"));
    m_speechd = Command::available(QStringLiteral("spd-say"));
    m_espeak = Command::available(QStringLiteral("espeak-ng")) || Command::available(QStringLiteral("espeak"));
    m_wtype = Command::available(QStringLiteral("wtype"));
    m_xdotool = Command::available(QStringLiteral("xdotool"));

    m_pocketsphinxBin.clear();
    if (Command::available(QStringLiteral("pocketsphinx_continuous")))
        m_pocketsphinxBin = QStringLiteral("pocketsphinx_continuous");
    else if (Command::available(QStringLiteral("pocketsphinx")))
        m_pocketsphinxBin = QStringLiteral("pocketsphinx");
    m_sttPocketsphinx = !m_pocketsphinxBin.isEmpty() && !modelDir().isEmpty();
    m_wakePocketsphinx = !m_pocketsphinxBin.isEmpty();

    // Resolve the capture tool (config override, then availability).
    m_captureTool.clear();
    const QString want = m_config ? m_config->captureTool() : QStringLiteral("auto");
    auto pick = [&](const QString &t) { return Command::available(t) ? t : QString(); };
    if (want != QLatin1String("auto") && !want.isEmpty())
        m_captureTool = pick(want);
    if (m_captureTool.isEmpty()) {
        for (const QString &t : {QStringLiteral("parecord"), QStringLiteral("pw-record"), QStringLiteral("arecord")}) {
            m_captureTool = pick(t);
            if (!m_captureTool.isEmpty())
                break;
        }
    }
    onConfigChanged();
    Q_EMIT capabilitiesChanged();
}

QString VoiceController::modelDir() const
{
    if (m_config && !m_config->pocketsphinxModel().isEmpty()
        && QFileInfo::exists(m_config->pocketsphinxModel()))
        return m_config->pocketsphinxModel();
    for (const QString &d : kModelProbe)
        if (QFileInfo(d).isDir() && QFileInfo::exists(d + QStringLiteral("/means")))
            return d;
    // Fall back to the first that merely exists as a directory.
    for (const QString &d : kModelProbe)
        if (QFileInfo(d).isDir())
            return d;
    return {};
}

QString VoiceController::resolvedTtsEngine() const
{
    const QString want = m_config ? m_config->ttsEngine() : QStringLiteral("auto");
    auto ok = [&](const QString &e) {
        return (e == QLatin1String("piper") && m_piper && m_config && !m_config->piperModel().isEmpty())
            || (e == QLatin1String("speechd") && m_speechd)
            || (e == QLatin1String("espeak") && m_espeak);
    };
    if (want != QLatin1String("auto") && ok(want))
        return want;
    // auto (or an unavailable forced choice): best available, piper first.
    if (m_piper && m_config && !m_config->piperModel().isEmpty())
        return QStringLiteral("piper");
    if (m_speechd)
        return QStringLiteral("speechd");
    if (m_espeak)
        return QStringLiteral("espeak");
    return QString();
}

QString VoiceController::statusText() const
{
    if (m_config && m_config->micMuted())
        return tr("Microphone muted");
    QStringList parts;
    const QString tts = resolvedTtsEngine();
    parts << (tts.isEmpty() ? tr("speech output unavailable")
                            : tr("speaks via %1").arg(tts == QLatin1String("speechd") ? QStringLiteral("speech-dispatcher") : tts));
    parts << (m_sttPocketsphinx ? tr("offline STT ready") : tr("STT engine not installed"));
    if (wakeAvailable())
        parts << tr("wake word ready");
    else if (!captureAvailable())
        parts << tr("no microphone capture tool");
    return parts.join(QStringLiteral(" · "));
}

void VoiceController::setState(const QString &s)
{
    if (s == m_state)
        return;
    // The microphone still hears the end of the assistant's own speech (and
    // the listener reports a phrase only after a short silence): ignore what
    // it hears for a moment after speaking, so a reply that mentions "Shadow"
    // can never wake it or answer its own question.
    if (m_state == QLatin1String("speaking"))
        m_quietUntil = QDateTime::currentMSecsSinceEpoch() + 1200;
    m_state = s;
    Q_EMIT stateChanged();
}

void VoiceController::beginVoiceTurn()
{
    if (m_voiceTurn)
        return;
    m_voiceTurn = true;
    Q_EMIT voiceTurnChanged();
}

void VoiceController::endVoiceTurn()
{
    if (!m_voiceTurn)
        return;
    m_voiceTurn = false;
    Q_EMIT voiceTurnChanged();
}

// --- capture + transcription -------------------------------------------------

QStringList VoiceController::captureArgv(const QString &outWav, int seconds) const
{
    const QString dev = m_config ? m_config->inputDevice() : QString();
    if (m_captureTool == QLatin1String("parecord")) {
        QStringList a{QStringLiteral("--rate=16000"), QStringLiteral("--channels=1"),
                      QStringLiteral("--format=s16le"), QStringLiteral("--file-format=wav")};
        if (!dev.isEmpty())
            a << QStringLiteral("--device=") + dev;
        a << outWav;
        return a;
    }
    if (m_captureTool == QLatin1String("pw-record")) {
        QStringList a{QStringLiteral("--rate"), QStringLiteral("16000"),
                      QStringLiteral("--channels"), QStringLiteral("1"),
                      QStringLiteral("--format"), QStringLiteral("s16")};
        if (!dev.isEmpty())
            a << QStringLiteral("--target") << dev;
        a << outWav;
        return a;
    }
    if (m_captureTool == QLatin1String("arecord")) {
        QStringList a{QStringLiteral("-f"), QStringLiteral("S16_LE"), QStringLiteral("-r"), QStringLiteral("16000"),
                      QStringLiteral("-c"), QStringLiteral("1"), QStringLiteral("-t"), QStringLiteral("wav"),
                      QStringLiteral("-d"), QString::number(seconds)};
        if (!dev.isEmpty())
            a << QStringLiteral("-D") << dev;
        a << outWav;
        return a;
    }
    return {};
}

void VoiceController::startCommandCapture()
{
    if (m_config && m_config->micMuted()) {
        Q_EMIT unavailable(tr("The microphone is muted."));
        return;
    }
    if (!captureAvailable()) {
        Q_EMIT unavailable(tr("No microphone capture tool (parecord / pw-record / arecord) is available."));
        return;
    }
    if (!m_sttPocketsphinx) {
        Q_EMIT unavailable(tr("No speech-to-text engine is installed (pocketsphinx + an en-us model)."));
        return;
    }
    if (m_capture || m_listenerCommand)
        return; // already capturing
    stopSpeaking();                 // barge-in: stop any reply first
    if (listenerTranscribes()) {
        // The running listener hears the command and knows when it ends.
        awaitCommandFromListener();
        return;
    }
    const int secs = m_config ? m_config->maxUtteranceSecs() : 12;
    m_captureWav = tmpWav(QStringLiteral("cmd"));
    const QStringList argv = captureArgv(m_captureWav, secs);
    m_capture = new QProcess(this);
    connect(m_capture, &QProcess::finished, this, [this](int, QProcess::ExitStatus) {
        const QString wav = m_captureWav;
        m_capture->deleteLater();
        m_capture = nullptr;
        transcribeCapturedFile(wav);
    });
    setState(QStringLiteral("listening"));
    m_capture->start(argv.first(), argv.mid(1));
    // Stop capturing after the max utterance window (parecord/pw-record have no
    // built-in duration; arecord self-terminates with -d but we still guard).
    QTimer::singleShot(secs * 1000 + 300, this, [this]() {
        if (m_capture && m_capture->state() != QProcess::NotRunning)
            m_capture->terminate();
    });
}

void VoiceController::transcribeCapturedFile(const QString &wavPath)
{
    setState(QStringLiteral("transcribing"));
    QProcess *p = new QProcess(this);
    const QStringList argv = pocketsphinxTranscribeArgv(wavPath);
    const bool yesNo = m_awaitingYesNo;
    const QString tag = m_pendingTag;
    connect(p, &QProcess::finished, this, [this, p, wavPath, yesNo, tag](int, QProcess::ExitStatus) {
        QString out = QString::fromUtf8(p->readAllStandardOutput());
        p->deleteLater();
        QFile::remove(wavPath);
        // pocketsphinx 5.x may print JSON; classic prints the plain hypothesis.
        QString text = out.trimmed();
        if (text.startsWith(QLatin1Char('{')) || text.startsWith(QLatin1Char('['))) {
            // Extract any "w" word tokens or a top-level hypothesis field.
            QStringList words;
            for (const QString &line : text.split(QLatin1Char('\n'))) {
                QJsonParseError e{};
                const QJsonDocument d = QJsonDocument::fromJson(line.toUtf8(), &e);
                if (e.error != QJsonParseError::NoError)
                    continue;
                const QJsonObject o = d.object();
                if (o.contains(QStringLiteral("t")))
                    words << o.value(QStringLiteral("t")).toString();
                else if (o.contains(QStringLiteral("hypothesis")))
                    words << o.value(QStringLiteral("hypothesis")).toString();
            }
            if (!words.isEmpty())
                text = words.join(QLatin1Char(' '));
        }
        text = text.simplified();
        setState(m_config && m_config->micMuted() ? QStringLiteral("muted") : QStringLiteral("idle"));
        if (yesNo) {
            m_awaitingYesNo = false;
            m_pendingTag.clear();
            Q_EMIT yesNoResult(tag, looksLikeYes(text));
            return;
        }
        if (text.isEmpty())
            Q_EMIT unavailable(tr("I didn't catch that."));
        else
            Q_EMIT transcriptReady(text);
    });
    p->start(argv.first(), argv.mid(1));
}

QStringList VoiceController::pocketsphinxTranscribeArgv(const QString &wavPath) const
{
    const QString md = modelDir();
    if (m_pocketsphinxBin == QLatin1String("pocketsphinx_continuous")) {
        QStringList a{QStringLiteral("-infile"), wavPath, QStringLiteral("-logfn"), QStringLiteral("/dev/null")};
        if (!md.isEmpty())
            a << QStringLiteral("-hmm") << md;
        return QStringList{m_pocketsphinxBin} << a;
    }
    // pocketsphinx 5.x: `pocketsphinx single WAV`.
    QStringList a{QStringLiteral("single"), wavPath};
    if (!md.isEmpty())
        a << QStringLiteral("-hmm") << md;
    return QStringList{m_pocketsphinxBin} << a;
}

QString VoiceController::transcribeWavSync(const QString &wavPath, QString *error) const
{
    if (m_pocketsphinxBin.isEmpty()) {
        if (error) *error = tr("pocketsphinx is not installed");
        return {};
    }
    if (!QFileInfo::exists(wavPath)) {
        if (error) *error = tr("no such file: %1").arg(wavPath);
        return {};
    }
    const QStringList argv = pocketsphinxTranscribeArgv(wavPath);
    const CommandResult r = Command::runSync(argv.first(), argv.mid(1), 60000);
    if (r.exitCode != 0 && r.outText().trimmed().isEmpty()) {
        if (error) *error = r.errText().isEmpty() ? tr("exit code %1").arg(r.exitCode) : r.errText();
        return {};
    }
    QString text = r.outText().trimmed();
    if (text.startsWith(QLatin1Char('{'))) {
        QStringList words;
        for (const QString &line : text.split(QLatin1Char('\n'))) {
            const QJsonObject o = QJsonDocument::fromJson(line.toUtf8()).object();
            if (o.contains(QStringLiteral("t")))
                words << o.value(QStringLiteral("t")).toString();
        }
        if (!words.isEmpty())
            text = words.join(QLatin1Char(' '));
    }
    return text.simplified();
}

bool VoiceController::wakePhraseInWavSync(const QString &wavPath, const QString &phrase,
                                          double threshold, QString *error) const
{
    if (m_pocketsphinxBin.isEmpty()) {
        if (error) *error = tr("pocketsphinx is not installed");
        return false;
    }
    const QString md = modelDir();
    QStringList argv;
    if (m_pocketsphinxBin == QLatin1String("pocketsphinx_continuous")) {
        argv << m_pocketsphinxBin << QStringLiteral("-infile") << wavPath
             << QStringLiteral("-keyphrase") << phrase
             << QStringLiteral("-kws_threshold") << QString::number(threshold, 'g', 3)
             << QStringLiteral("-logfn") << QStringLiteral("/dev/null");
        if (!md.isEmpty())
            argv << QStringLiteral("-hmm") << md;
    } else {
        argv << m_pocketsphinxBin << QStringLiteral("-keyphrase") << phrase
             << QStringLiteral("single") << wavPath;
        if (!md.isEmpty())
            argv << QStringLiteral("-hmm") << md;
    }
    const CommandResult r = Command::runSync(argv.first(), argv.mid(1), 60000);
    const QString hay = (r.outText() + QLatin1Char('\n') + r.errText()).toLower();
    return hay.contains(phrase.toLower());
}

// --- wake listener -----------------------------------------------------------

QStringList VoiceController::pocketsphinxKeyphraseLiveArgv() const
{
    const QString phrase = m_config ? m_config->wakePhrase() : QStringLiteral("shadow");
    const double thr = m_config ? m_config->wakeThreshold() : 1e-20;
    const QString md = modelDir();
    QStringList argv;
    if (m_pocketsphinxBin == QLatin1String("pocketsphinx_continuous")) {
        // Transcribe every phrase (with the general English language model)
        // rather than spot the keyphrase alone: the words after "Shadow" are
        // the command, and the end of a phrase is where the user stops
        // talking. Keyphrase spotting reported "shadow" only at the end of the
        // whole sentence, by which time the command had already been said.
        Q_UNUSED(phrase);
        Q_UNUSED(thr);
        argv << m_pocketsphinxBin << QStringLiteral("-inmic") << QStringLiteral("yes")
             << QStringLiteral("-logfn") << QStringLiteral("/dev/null");
        if (!md.isEmpty())
            argv << QStringLiteral("-hmm") << md;
        const QString dev = m_config ? m_config->inputDevice() : QString();
        if (!dev.isEmpty())
            argv << QStringLiteral("-adcdev") << dev;
    } else {
        argv << m_pocketsphinxBin << QStringLiteral("-keyphrase") << phrase << QStringLiteral("live");
        if (!md.isEmpty())
            argv << QStringLiteral("-hmm") << md;
    }
    return argv;
}

void VoiceController::listenNow()
{
    m_forceListen = true;
    syncWakeListener();
}

bool VoiceController::listenerTranscribes() const
{
    return m_wake && m_wake->state() == QProcess::Running
        && m_pocketsphinxBin == QLatin1String("pocketsphinx_continuous");
}

QString VoiceController::wakeRemainder(const QString &utterance, const QString &phrase, bool *hit)
{
    *hit = false;
    const QString text = utterance.toLower().simplified();
    QStringList words = phrase.toLower().simplified().split(QLatin1Char(' '), Qt::SkipEmptyParts);
    if (words.isEmpty() || text.isEmpty())
        return {};
    for (QString &w : words)
        w = QRegularExpression::escape(w);
    // "shadow", "hey shadow", "ok shadow", "shadow's"/"shadows" (how the
    // recogniser sometimes hears it), as whole words anywhere in the phrase.
    const QRegularExpression re(QStringLiteral("(?:^|\\s)(?:(?:hey|hi|ok|okay)\\s+)?%1(?:'s|s)?(?=\\s|$)")
                                    .arg(words.join(QStringLiteral("\\s+"))));
    const QRegularExpressionMatch m = re.match(text);
    if (!m.hasMatch())
        return {};
    *hit = true;
    return text.mid(m.capturedEnd()).trimmed();
}

void VoiceController::onListenerLine(const QString &line)
{
    const QString text = line.toLower().simplified();
    if (text.isEmpty() || (m_config && m_config->micMuted()))
        return;
    if (m_state == QLatin1String("speaking") || QDateTime::currentMSecsSinceEpoch() < m_quietUntil)
        return;
    const QString phrase = m_config ? m_config->wakePhrase() : QStringLiteral("shadow");
    bool hit = false;
    const QString rest = wakeRemainder(text, phrase, &hit);

    if (m_listenerCommand) {
        // The command after a bare "Shadow" (or push-to-talk); a repeated wake
        // word in front of it is dropped, a wake word alone keeps waiting.
        if (hit && rest.isEmpty())
            return;
        finishListenerCommand(hit ? rest : text);
        return;
    }
    if (!hit || m_state != QLatin1String("idle"))
        return;
    Q_EMIT wakeDetected();
    beginVoiceTurn();
    if (!listenerTranscribes()) {
        // Keyphrase spotting only: record the command separately.
        setState(QStringLiteral("wake"));
        startCommandCapture();
        return;
    }
    if (!rest.isEmpty()) {
        Q_EMIT transcriptReady(rest);   // "Shadow, open the terminal"
        return;
    }
    setState(QStringLiteral("wake"));
    awaitCommandFromListener();         // "Shadow" … "open the terminal"
}

void VoiceController::awaitCommandFromListener()
{
    m_listenerCommand = true;
    setState(QStringLiteral("listening"));
    if (!m_commandTimer) {
        m_commandTimer = new QTimer(this);
        m_commandTimer->setSingleShot(true);
        connect(m_commandTimer, &QTimer::timeout, this, [this]() {
            if (!m_listenerCommand)
                return;
            m_listenerCommand = false;
            setState(m_config && m_config->micMuted() ? QStringLiteral("muted") : QStringLiteral("idle"));
            if (m_awaitingYesNo) {
                // No spoken answer: the on-screen card decides.
                m_awaitingYesNo = false;
                m_pendingTag.clear();
                return;
            }
            endVoiceTurn();
            Q_EMIT unavailable(tr("I didn't catch that."));
        });
    }
    // Time to start talking plus the longest command.
    const int secs = m_config ? m_config->maxUtteranceSecs() : 12;
    m_commandTimer->start((secs + 6) * 1000);
}

void VoiceController::finishListenerCommand(const QString &text)
{
    m_listenerCommand = false;
    if (m_commandTimer)
        m_commandTimer->stop();
    setState(m_config && m_config->micMuted() ? QStringLiteral("muted") : QStringLiteral("idle"));
    if (m_awaitingYesNo) {
        const QString tag = m_pendingTag;
        m_awaitingYesNo = false;
        m_pendingTag.clear();
        Q_EMIT yesNoResult(tag, looksLikeYes(text));
        return;
    }
    Q_EMIT transcriptReady(text);
}

void VoiceController::syncWakeListener()
{
    const bool want = m_config && (m_config->wakeEnabled() || m_forceListen) && !m_config->micMuted() && wakeAvailable();
    if (want && !m_wake)
        startWakeListener();
    else if (!want && m_wake)
        stopWakeListener();
}

void VoiceController::startWakeListener()
{
    if (m_wake || !wakeAvailable())
        return;
    const QStringList argv = pocketsphinxKeyphraseLiveArgv();
    m_wake = new QProcess(this);
    // Only the recognised phrases (stdout); diagnostics go nowhere.
    m_wake->setProcessChannelMode(QProcess::SeparateChannels);
    m_wake->setStandardErrorFile(QProcess::nullDevice());
    m_wakeBuffer.clear();
    connect(m_wake, &QProcess::readyReadStandardOutput, this, [this]() {
        if (!m_wake)
            return;
        m_wakeBuffer += m_wake->readAllStandardOutput();
        int nl;
        while ((nl = m_wakeBuffer.indexOf('\n')) >= 0) {
            const QString line = QString::fromUtf8(m_wakeBuffer.left(nl));
            m_wakeBuffer.remove(0, nl + 1);
            onListenerLine(line);
            if (!m_wake)
                return;
        }
    });
    connect(m_wake, &QProcess::finished, this, [this](int, QProcess::ExitStatus) {
        if (!m_wake)
            return;
        // The microphone went away (or was not there yet at login), or the
        // engine stopped: start it again, waiting longer while it keeps failing.
        const bool ranLong = m_wakeRunning.isValid() && m_wakeRunning.elapsed() > 60000;
        m_wake->deleteLater();
        m_wake = nullptr;
        if (m_listenerCommand) {
            m_listenerCommand = false;
            if (m_commandTimer)
                m_commandTimer->stop();
            setState(m_config && m_config->micMuted() ? QStringLiteral("muted") : QStringLiteral("idle"));
        }
        m_wakeRestartDelay = ranLong || m_wakeRestartDelay == 0 ? 2000 : qMin(m_wakeRestartDelay * 2, 15000);
        QTimer::singleShot(m_wakeRestartDelay, this, &VoiceController::syncWakeListener);
    });
    m_wakeRunning.start();
    m_wake->start(argv.first(), argv.mid(1));
}

void VoiceController::stopWakeListener()
{
    if (!m_wake)
        return;
    QProcess *w = m_wake;
    m_wake = nullptr;
    disconnect(w, nullptr, this, nullptr);
    if (m_listenerCommand) {
        m_listenerCommand = false;
        if (m_commandTimer)
            m_commandTimer->stop();
        setState(m_config && m_config->micMuted() ? QStringLiteral("muted") : QStringLiteral("idle"));
    }
    w->terminate();
    if (!w->waitForFinished(500))
        w->kill();
    w->deleteLater();
}

// --- speaking ---------------------------------------------------------------

QStringList VoiceController::ttsSpeakArgv(const QString &engine, const QString &text) const
{
    if (engine == QLatin1String("speechd")) {
        QStringList a{QStringLiteral("-w")}; // -w: wait until spoken
        const QString v = m_config ? m_config->ttsVoice() : QString();
        if (!v.isEmpty())
            a << QStringLiteral("-y") << v;
        a << QStringLiteral("--") << text;
        return QStringList{QStringLiteral("spd-say")} << a;
    }
    if (engine == QLatin1String("espeak")) {
        const QString bin = Command::available(QStringLiteral("espeak-ng")) ? QStringLiteral("espeak-ng")
                                                                            : QStringLiteral("espeak");
        QStringList a;
        const QString v = m_config ? m_config->ttsVoice() : QString();
        if (!v.isEmpty())
            a << QStringLiteral("-v") << v;
        a << QStringLiteral("--") << text;
        return QStringList{bin} << a;
    }
    // piper is handled separately (needs a pipe + playback); never argv-only.
    return {};
}

void VoiceController::speak(const QString &text)
{
    if (!m_config || !m_config->speakReplies())
        return;
    const QString t = text.trimmed();
    if (t.isEmpty())
        return;
    const QString engine = resolvedTtsEngine();
    if (engine.isEmpty()) {
        Q_EMIT unavailable(tr("No speech engine (piper / speech-dispatcher / espeak-ng) is available."));
        return;
    }
    stopSpeaking();
    setState(QStringLiteral("speaking"));

    if (engine == QLatin1String("piper")) {
        // piper reads text on stdin, writes a WAV, which we then play. Use a
        // shell-free two-step via QProcess: synth to temp WAV, then play.
        const QString wav = tmpWav(QStringLiteral("say"));
        QString err;
        if (!synthesizeToWavSync(t, wav, &err)) {
            setState(QStringLiteral("idle"));
            Q_EMIT unavailable(tr("piper failed: %1").arg(err));
            return;
        }
        QString player;
        for (const QString &p : {QStringLiteral("paplay"), QStringLiteral("aplay"), QStringLiteral("pw-play")})
            if (Command::available(p)) { player = p; break; }
        if (player.isEmpty()) {
            QFile::remove(wav);
            setState(QStringLiteral("idle"));
            Q_EMIT unavailable(tr("No audio player (paplay / aplay / pw-play) to play speech."));
            return;
        }
        m_speak = new QProcess(this);
        connect(m_speak, &QProcess::finished, this, [this, wav](int, QProcess::ExitStatus) {
            QFile::remove(wav);
            if (m_speak) { m_speak->deleteLater(); m_speak = nullptr; }
            if (m_state == QLatin1String("speaking"))
                setState(m_config && m_config->micMuted() ? QStringLiteral("muted") : QStringLiteral("idle"));
        });
        m_speak->start(player, {wav});
        return;
    }

    const QStringList argv = ttsSpeakArgv(engine, t);
    m_speak = new QProcess(this);
    connect(m_speak, &QProcess::finished, this, [this](int, QProcess::ExitStatus) {
        if (m_speak) { m_speak->deleteLater(); m_speak = nullptr; }
        if (m_state == QLatin1String("speaking"))
            setState(m_config && m_config->micMuted() ? QStringLiteral("muted") : QStringLiteral("idle"));
    });
    m_speak->start(argv.first(), argv.mid(1));
}

bool VoiceController::synthesizeToWavSync(const QString &text, const QString &outWav, QString *error) const
{
    const QString engine = resolvedTtsEngine();
    if (engine == QLatin1String("piper")) {
        const QString bin = m_config ? m_config->piperBin() : QStringLiteral("piper");
        const QString model = m_config ? m_config->piperModel() : QString();
        if (model.isEmpty() || !QFileInfo::exists(model)) {
            if (error) *error = tr("no piper voice model configured");
            return false;
        }
        QProcess p;
        p.start(bin, {QStringLiteral("--model"), model, QStringLiteral("--output_file"), outWav});
        if (!p.waitForStarted(3000)) {
            if (error) *error = tr("could not start piper");
            return false;
        }
        p.write(text.toUtf8());
        p.closeWriteChannel();
        p.waitForFinished(30000);
        if (QFileInfo(outWav).size() > 44)
            return true;
        if (error) *error = p.readAllStandardError().isEmpty() ? tr("piper produced no audio")
                                                               : QString::fromUtf8(p.readAllStandardError());
        return false;
    }
    if (engine == QLatin1String("espeak")) {
        const QString bin = Command::available(QStringLiteral("espeak-ng")) ? QStringLiteral("espeak-ng")
                                                                            : QStringLiteral("espeak");
        QStringList a;
        const QString v = m_config ? m_config->ttsVoice() : QString();
        if (!v.isEmpty())
            a << QStringLiteral("-v") << v;
        a << QStringLiteral("-w") << outWav << QStringLiteral("--") << text;
        const CommandResult r = Command::runSync(bin, a, 30000);
        if (r.exitCode == 0 && QFileInfo(outWav).size() > 44)
            return true;
        if (error) *error = r.errText().isEmpty() ? tr("espeak produced no audio") : r.errText();
        return false;
    }
    if (engine == QLatin1String("speechd")) {
        // spd-say speaks directly; synthesising to a file needs espeak/piper.
        if (error) *error = tr("speech-dispatcher speaks directly and cannot render to a WAV");
        return false;
    }
    if (error) *error = tr("no TTS engine available");
    return false;
}

void VoiceController::stopSpeaking()
{
    if (m_speak) {
        QProcess *s = m_speak;
        m_speak = nullptr;
        disconnect(s, nullptr, this, nullptr);
        s->terminate();
        if (!s->waitForFinished(300))
            s->kill();
        s->deleteLater();
    }
    if (m_state == QLatin1String("speaking"))
        setState(m_config && m_config->micMuted() ? QStringLiteral("muted") : QStringLiteral("idle"));
}

void VoiceController::confirmBySpeech(const QString &tag, const QString &prompt)
{
    // Speak the prompt; when done, capture a short yes/no answer. If capture is
    // unavailable we still speak, and fall back to the on-screen card.
    m_pendingTag = tag;
    m_awaitingYesNo = true;
    if (m_config && m_config->speakReplies() && !resolvedTtsEngine().isEmpty()) {
        // Speak synchronously-ish: start capture after the speak process ends.
        const QString engine = resolvedTtsEngine();
        if (engine != QLatin1String("piper")) {
            const QStringList argv = ttsSpeakArgv(engine, prompt);
            setState(QStringLiteral("speaking"));
            QProcess *s = new QProcess(this);
            connect(s, &QProcess::finished, this, [this, s](int, QProcess::ExitStatus) {
                s->deleteLater();
                if (!m_config || m_config->micMuted() || !captureAvailable() || !m_sttPocketsphinx) {
                    // Can't listen for the answer; leave the on-screen card to decide.
                    m_awaitingYesNo = false;
                    setState(m_config && m_config->micMuted() ? QStringLiteral("muted") : QStringLiteral("idle"));
                    return;
                }
                startCommandCapture();
            });
            s->start(argv.first(), argv.mid(1));
            return;
        }
    }
    // No speech available: just try to capture the answer, or give up to the card.
    if (m_config && !m_config->micMuted() && captureAvailable() && m_sttPocketsphinx)
        startCommandCapture();
    else
        m_awaitingYesNo = false;
}

void VoiceController::cancel()
{
    stopSpeaking();
    if (m_capture) {
        QProcess *c = m_capture;
        m_capture = nullptr;
        disconnect(c, nullptr, this, nullptr);
        c->terminate();
        if (!c->waitForFinished(300))
            c->kill();
        c->deleteLater();
        if (!m_captureWav.isEmpty())
            QFile::remove(m_captureWav);
    }
    m_listenerCommand = false;
    if (m_commandTimer)
        m_commandTimer->stop();
    m_awaitingYesNo = false;
    m_pendingTag.clear();
    setState(m_config && m_config->micMuted() ? QStringLiteral("muted") : QStringLiteral("idle"));
}

QJsonObject VoiceController::diagnostics() const
{
    return QJsonObject{
        {QStringLiteral("engines"), QJsonObject{
            {QStringLiteral("piper"), m_piper},
            {QStringLiteral("speechDispatcher"), m_speechd},
            {QStringLiteral("espeakNg"), m_espeak},
            {QStringLiteral("pocketsphinxBin"), m_pocketsphinxBin},
            {QStringLiteral("sttAvailable"), m_sttPocketsphinx},
            {QStringLiteral("wtype"), m_wtype},
            {QStringLiteral("xdotool"), m_xdotool},
        }},
        {QStringLiteral("capture"), QJsonObject{
            {QStringLiteral("tool"), m_captureTool},
            {QStringLiteral("available"), captureAvailable()},
        }},
        {QStringLiteral("resolved"), QJsonObject{
            {QStringLiteral("ttsEngine"), resolvedTtsEngine()},
            {QStringLiteral("modelDir"), modelDir()},
        }},
        {QStringLiteral("capabilities"), QJsonObject{
            {QStringLiteral("tts"), ttsAvailable()},
            {QStringLiteral("stt"), sttAvailable()},
            {QStringLiteral("wake"), wakeAvailable()},
            {QStringLiteral("listenerRunning"), m_wake != nullptr},
            {QStringLiteral("typeText"), typeAvailable()},
        }},
        {QStringLiteral("config"), m_config ? QJsonObject{
            {QStringLiteral("wakeEnabled"), m_config->wakeEnabled()},
            {QStringLiteral("wakePhrase"), m_config->wakePhrase()},
            {QStringLiteral("wakeThreshold"), m_config->wakeThreshold()},
            {QStringLiteral("micMuted"), m_config->micMuted()},
            {QStringLiteral("speakReplies"), m_config->speakReplies()},
            {QStringLiteral("ttsEngine"), m_config->ttsEngine()},
            {QStringLiteral("sttMode"), m_config->sttMode()},
            {QStringLiteral("maxUtteranceSecs"), m_config->maxUtteranceSecs()},
        } : QJsonObject{}},
    };
}
