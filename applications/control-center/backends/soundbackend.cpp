// SPDX-License-Identifier: GPL-3.0-or-later
#include "soundbackend.h"

#include "dumpregistry.h"
#include "networkcommon.h"

#include <QDir>
#include <QJsonArray>
#include <QMetaMethod>
#include <QSettings>
#include <QTemporaryFile>
#include <QtEndian>
#include <QtMath>

#include <command.h>

#include <cmath>

#ifdef NEXUS_HAVE_PULSEAUDIOQT
// <PulseAudioQt/Card> is deliberately not used: it pulls in libpulse's own
// headers (pulse/introspect.h), which a PulseAudioQt build environment need
// not have. Cards are reached through CardModel and their Qt properties.
#include <PulseAudioQt/Client>
#include <PulseAudioQt/Context>
#include <PulseAudioQt/Models>
#include <PulseAudioQt/Port>
#include <PulseAudioQt/Profile>
#include <PulseAudioQt/Server>
#include <PulseAudioQt/Sink>
#include <PulseAudioQt/SinkInput>
#include <PulseAudioQt/Source>
#endif

using namespace NetHw;

NEXUS_CC_DUMP("sound", [] {
    SoundBackend backend;
    waitUntilReady(&backend);
    return QJsonValue(backend.toJson());
})

namespace {
const QStringList kDeviceRoles = {
    QStringLiteral("key"), QStringLiteral("deviceIndex"), QStringLiteral("name"), QStringLiteral("description"),
    QStringLiteral("volume"), QStringLiteral("muted"), QStringLiteral("isDefault"), QStringLiteral("volumeWritable"),
    QStringLiteral("ports"), QStringLiteral("activePort"),
};
const QStringList kStreamRoles = {
    QStringLiteral("key"), QStringLiteral("streamIndex"), QStringLiteral("name"), QStringLiteral("application"),
    QStringLiteral("applicationIcon"), QStringLiteral("volume"), QStringLiteral("muted"), QStringLiteral("hasVolume"),
    QStringLiteral("volumeWritable"), QStringLiteral("sinkIndex"),
};
const QStringList kCardRoles = {
    QStringLiteral("key"), QStringLiteral("cardIndex"), QStringLiteral("name"), QStringLiteral("description"),
    QStringLiteral("profiles"), QStringLiteral("activeProfile"),
};

QSettings pageSettings()
{
    return QSettings(QSettings::NativeFormat, QSettings::UserScope, QStringLiteral("nexus"), QStringLiteral("controlcenter"));
}

#ifdef NEXUS_HAVE_PULSEAUDIOQT
using PulseAudioQt::Context;

int toPercent(qint64 volume)
{
    return qRound(double(volume) * 100.0 / double(PulseAudioQt::normalVolume()));
}

qint64 fromPercent(int percent)
{
    return qRound64(double(percent) * double(PulseAudioQt::normalVolume()) / 100.0);
}

template<typename T>
T *byIndex(const QList<T *> &objects, int index)
{
    for (T *object : objects) {
        if (int(object->index()) == index)
            return object;
    }
    return nullptr;
}

QVariantList portList(const QList<PulseAudioQt::Port *> &ports)
{
    QVariantList list;
    for (int i = 0; i < ports.size(); ++i) {
        const PulseAudioQt::Port *port = ports.at(i);
        QString text = port->description().isEmpty() ? port->name() : port->description();
        if (port->availability() == PulseAudioQt::Profile::Unavailable)
            text = SoundBackend::tr("%1 (not plugged in)").arg(text);
        list.append(QVariantMap{{QStringLiteral("text"), text}, {QStringLiteral("portIndex"), i},
                                {QStringLiteral("name"), port->name()}});
    }
    return list;
}

QVariantMap deviceRow(PulseAudioQt::Device *device)
{
    const QVariantList ports = portList(device->ports());
    const int activePort = int(device->activePortIndex());
    return {
        {QStringLiteral("key"), QString::number(device->index())},
        {QStringLiteral("deviceIndex"), int(device->index())},
        {QStringLiteral("name"), device->name()},
        {QStringLiteral("description"), device->description().isEmpty() ? device->name() : device->description()},
        {QStringLiteral("volume"), toPercent(device->volume())},
        {QStringLiteral("muted"), device->isMuted()},
        {QStringLiteral("isDefault"), device->isDefault()},
        {QStringLiteral("volumeWritable"), device->isVolumeWritable()},
        {QStringLiteral("ports"), ports},
        {QStringLiteral("activePort"), activePort >= 0 && activePort < ports.size() ? activePort : -1},
    };
}

bool isMonitor(const PulseAudioQt::Source *source)
{
    return source->properties().value(QStringLiteral("device.class")).toString() == QLatin1String("monitor")
           || source->name().endsWith(QLatin1String(".monitor"));
}
#endif
} // namespace

SoundBackend::SoundBackend(QObject *parent)
    : QObject(parent)
{
    m_sinks = new RowModel(kDeviceRoles, QStringLiteral("key"), this);
    m_sources = new RowModel(kDeviceRoles, QStringLiteral("key"), this);
    m_streams = new RowModel(kStreamRoles, QStringLiteral("key"), this);
    m_cards = new RowModel(kCardRoles, QStringLiteral("key"), this);

    {
        QSettings settings = pageSettings();
        m_allowOveramplification = settings.value(QStringLiteral("Sound/AllowOveramplification"), false).toBool();
    }

    m_rebuildTimer.setSingleShot(true);
    m_rebuildTimer.setInterval(15);
    connect(&m_rebuildTimer, &QTimer::timeout, this, &SoundBackend::rebuild);

    // The meter falls back to silence when the recorder stops delivering.
    m_meterDecay.setInterval(120);
    connect(&m_meterDecay, &QTimer::timeout, this, [this] {
        if (!m_meterGotData && m_inputLevel > 0) {
            m_inputLevel = m_inputLevel < 0.02 ? 0 : m_inputLevel * 0.6;
            Q_EMIT inputLevelChanged();
        }
        m_meterGotData = false;
    });

#ifdef NEXUS_HAVE_PULSEAUDIOQT
    Context *context = Context::instance();
    connect(context, &Context::stateChanged, this, &SoundBackend::updateState);
    connect(context, &Context::autoConnectingChanged, this, &SoundBackend::updateState);
    connect(context, &Context::sinkAdded, this, [this](PulseAudioQt::Sink *sink) { track(sink); });
    connect(context, &Context::sourceAdded, this, [this](PulseAudioQt::Source *source) { track(source); });
    connect(context, &Context::sinkInputAdded, this, [this](PulseAudioQt::SinkInput *input) { track(input); });
    connect(context, &Context::sinkRemoved, this, &SoundBackend::scheduleRebuild);
    connect(context, &Context::sourceRemoved, this, &SoundBackend::scheduleRebuild);
    connect(context, &Context::sinkInputRemoved, this, &SoundBackend::scheduleRebuild);
    connect(context->server(), &PulseAudioQt::Server::defaultSinkChanged, this, &SoundBackend::scheduleRebuild);
    connect(context->server(), &PulseAudioQt::Server::defaultSourceChanged, this, &SoundBackend::scheduleRebuild);
    connect(context->server(), &PulseAudioQt::Server::isPipeWireChanged, this, &SoundBackend::updateState);
    // The server information is the last thing PulseAudioQt asks for after
    // connecting: once it is in, the device and stream lists are complete.
    connect(context->server(), &PulseAudioQt::Server::updated, this, [this] {
        if (!m_serverSynced) {
            m_serverSynced = true;
            updateState();
        } else {
            scheduleRebuild();
        }
    });
    const auto sinks = context->sinks();
    for (PulseAudioQt::Sink *sink : sinks)
        track(sink);
    const auto sources = context->sources();
    for (PulseAudioQt::Source *source : sources)
        track(source);
    const auto inputs = context->sinkInputs();
    for (PulseAudioQt::SinkInput *input : inputs)
        track(input);
    m_cardModel = new PulseAudioQt::CardModel(this);
    connect(m_cardModel, &QAbstractItemModel::rowsInserted, this, &SoundBackend::scheduleRebuild);
    connect(m_cardModel, &QAbstractItemModel::rowsRemoved, this, &SoundBackend::scheduleRebuild);
    connect(m_cardModel, &QAbstractItemModel::modelReset, this, &SoundBackend::scheduleRebuild);
    connect(m_cardModel, &QAbstractItemModel::dataChanged, this, &SoundBackend::scheduleRebuild);

    // A server that is not there shows as repeated connection attempts; give
    // it a moment before calling it absent.
    m_connectTimer.setSingleShot(true);
    m_connectTimer.setInterval(2500);
    connect(&m_connectTimer, &QTimer::timeout, this, [this] {
        m_connectTimedOut = true;
        updateState();
    });
    m_connectTimer.start();
    updateState();
#else
    QTimer::singleShot(0, this, &SoundBackend::updateState);
#endif
}

SoundBackend::~SoundBackend()
{
    stopMeter();
}

QAbstractItemModel *SoundBackend::sinks() const { return m_sinks; }
QAbstractItemModel *SoundBackend::sources() const { return m_sources; }
QAbstractItemModel *SoundBackend::streams() const { return m_streams; }
QAbstractItemModel *SoundBackend::cards() const { return m_cards; }
int SoundBackend::sinkCount() const { return m_sinks->count(); }
int SoundBackend::sourceCount() const { return m_sources->count(); }
int SoundBackend::streamCount() const { return m_streams->count(); }
int SoundBackend::cardCount() const { return m_cards->count(); }

void SoundBackend::setState(const QString &state, const QString &message, const QString &serverName)
{
    if (state == m_state && message == m_statusMessage && serverName == m_serverName)
        return;
    m_state = state;
    m_statusMessage = message;
    m_serverName = serverName;
    Q_EMIT stateChanged();
}

void SoundBackend::track(QObject *object)
{
    // Every property of a PulseAudioQt object has a change signal; any of
    // them means "publish again".
    const QMetaMethod slot = metaObject()->method(metaObject()->indexOfSlot("scheduleRebuild()"));
    const QMetaObject *meta = object->metaObject();
    for (int i = 0; i < meta->propertyCount(); ++i) {
        const QMetaProperty property = meta->property(i);
        if (property.hasNotifySignal())
            connect(object, property.notifySignal(), this, slot, Qt::UniqueConnection);
    }
    scheduleRebuild();
}

void SoundBackend::scheduleRebuild()
{
    if (!m_rebuildTimer.isActive())
        m_rebuildTimer.start();
}

void SoundBackend::updateState()
{
#ifdef NEXUS_HAVE_PULSEAUDIOQT
    Context *context = Context::instance();
    const Context::State contextState = context->state();
    if (contextState == Context::State::Ready && m_serverSynced) {
        m_connectTimer.stop();
        rebuild();                                  // fills the models before "ok" is announced
        setState(QStringLiteral("ok"), QString(), context->server()->isPipeWire() ? tr("PipeWire") : tr("PulseAudio"));
    } else {
        if (contextState != Context::State::Ready)
            m_serverSynced = false;
        // libpulse can sit in "Connecting" for good when there is no server
        // socket at all, so every state short of Ready counts as an attempt
        // only until the timer has run out. A server that appears later still
        // turns the page to "ok" through stateChanged().
        const bool trying = contextState == Context::State::Ready
                            || (!m_connectTimedOut
                                && (contextState == Context::State::Connecting || contextState == Context::State::Authorizing
                                    || contextState == Context::State::SettingName || context->isAutoConnecting()));
        rebuild();
        if (trying) {
            setState(QStringLiteral("connecting"), tr("Connecting to the sound server…"), QString());
        } else {
            setState(QStringLiteral("no-server"),
                     tr("No sound server is reachable. NEXUS uses PipeWire with its PulseAudio service: check that "
                        "pipewire, wireplumber and pipewire-pulse are running in this session."), QString());
        }
    }
    if (!m_ready && m_state != QLatin1String("connecting")) {
        m_ready = true;
        Q_EMIT readyChanged();
    }
#else
    setState(QStringLiteral("unsupported-build"),
             tr("This build of the Control Center has no PulseAudioQt support, so sound devices cannot be shown or changed."),
             QString());
    if (!m_ready) {
        m_ready = true;
        Q_EMIT readyChanged();
    }
#endif
    restartMeter();
}

void SoundBackend::reconnect()
{
#ifdef NEXUS_HAVE_PULSEAUDIOQT
    m_connectTimedOut = false;
    m_connectTimer.start();
    Context::instance()->reconnectDaemon();
    updateState();
#endif
}

void SoundBackend::rebuild()
{
    QVariantList sinks, sources, streams, cards;
    QString defaultSink, defaultSource, defaultSourceDescription;

#ifdef NEXUS_HAVE_PULSEAUDIOQT
    Context *context = Context::instance();
    if (context->state() == Context::State::Ready && m_serverSynced) {
        const auto sinkObjects = context->sinks();
        for (PulseAudioQt::Sink *sink : sinkObjects) {
            sinks.append(deviceRow(sink));
            if (sink->isDefault())
                defaultSink = sink->name();
        }
        const auto sourceObjects = context->sources();
        for (PulseAudioQt::Source *source : sourceObjects) {
            if (isMonitor(source))
                continue;           // "what is playing on output X", not a microphone
            sources.append(deviceRow(source));
            if (source->isDefault()) {
                defaultSource = source->name();
                defaultSourceDescription = source->description();
            }
        }
        const auto inputObjects = context->sinkInputs();
        for (PulseAudioQt::SinkInput *input : inputObjects) {
            if (input->isVirtualStream())
                continue;
            const QVariantMap properties = input->properties();
            QString application = input->client() ? input->client()->name() : QString();
            if (application.isEmpty())
                application = properties.value(QStringLiteral("application.name")).toString();
            if (application.isEmpty())
                application = tr("Unknown application");
            streams.append(QVariantMap{
                {QStringLiteral("key"), QString::number(input->index())},
                {QStringLiteral("streamIndex"), int(input->index())},
                {QStringLiteral("name"), input->name()},
                {QStringLiteral("application"), application},
                {QStringLiteral("applicationIcon"), properties.value(QStringLiteral("application.icon-name")).toString()},
                {QStringLiteral("volume"), toPercent(input->volume())},
                {QStringLiteral("muted"), input->isMuted()},
                {QStringLiteral("hasVolume"), input->hasVolume()},
                {QStringLiteral("volumeWritable"), input->isVolumeWritable()},
                {QStringLiteral("sinkIndex"), int(input->deviceIndex())},
            });
        }
        for (int row = 0; m_cardModel && row < m_cardModel->rowCount(); ++row) {
            const QObject *card = m_cardModel->index(row, 0).data(PulseAudioQt::AbstractModel::PulseObjectRole).value<QObject *>();
            if (!card)
                continue;
            QVariantList profiles;
            const auto profileObjects = card->property("profiles").value<QList<PulseAudioQt::Profile *>>();
            for (int i = 0; i < profileObjects.size(); ++i) {
                const PulseAudioQt::Profile *profile = profileObjects.at(i);
                QString text = profile->description().isEmpty() ? profile->name() : profile->description();
                if (profile->availability() == PulseAudioQt::Profile::Unavailable)
                    text = tr("%1 (unavailable)").arg(text);
                profiles.append(QVariantMap{{QStringLiteral("text"), text}, {QStringLiteral("profileIndex"), i},
                                            {QStringLiteral("name"), profile->name()}});
            }
            const QString name = card->property("name").toString();
            QString description = card->property("properties").toMap().value(QStringLiteral("device.description")).toString();
            if (description.isEmpty())
                description = name;
            const int index = card->property("index").toInt();
            const int active = card->property("activeProfileIndex").toInt();
            cards.append(QVariantMap{
                {QStringLiteral("key"), QString::number(index)},
                {QStringLiteral("cardIndex"), index},
                {QStringLiteral("name"), name},
                {QStringLiteral("description"), description},
                {QStringLiteral("profiles"), profiles},
                {QStringLiteral("activeProfile"), active >= 0 && active < profiles.size() ? active : -1},
            });
        }
    }
#endif

    // The page only lists cards that offer a choice; the dump has them all.
    QVariantList selectableCards;
    for (const QVariant &card : std::as_const(cards)) {
        if (card.toMap().value(QStringLiteral("profiles")).toList().size() > 1)
            selectableCards.append(card);
    }
    m_allCards = cards;
    m_sinks->setRows(sinks);
    m_sources->setRows(sources);
    m_streams->setRows(streams);
    m_cards->setRows(selectableCards);
    m_defaultSinkName = defaultSink;
    m_defaultSourceName = defaultSource;
    m_defaultSourceDescription = defaultSourceDescription;
    Q_EMIT devicesChanged();
    restartMeter();
}

// ---- changes ------------------------------------------------------------------

void SoundBackend::setAllowOveramplification(bool allow)
{
    if (allow == m_allowOveramplification)
        return;
    m_allowOveramplification = allow;
    QSettings settings = pageSettings();
    settings.setValue(QStringLiteral("Sound/AllowOveramplification"), allow);
    Q_EMIT overamplificationChanged();
}

#ifdef NEXUS_HAVE_PULSEAUDIOQT
#define NEXUS_SOUND_OBJECT(type, list, index) \
    PulseAudioQt::type *object = available() ? byIndex(Context::instance()->list(), index) : nullptr; \
    if (!object) { \
        Q_EMIT failed(tr("The device or stream is no longer there.")); \
        return; \
    }
#else
#define NEXUS_SOUND_OBJECT(type, list, index) \
    Q_UNUSED(index) \
    Q_EMIT failed(m_statusMessage); \
    return;
#endif

void SoundBackend::setSinkVolume(int index, int percent)
{
    NEXUS_SOUND_OBJECT(Sink, sinks, index)
#ifdef NEXUS_HAVE_PULSEAUDIOQT
    object->setVolume(fromPercent(qBound(0, percent, maxVolume())));
#else
    Q_UNUSED(percent)
#endif
}

void SoundBackend::setSinkMuted(int index, bool muted)
{
    NEXUS_SOUND_OBJECT(Sink, sinks, index)
#ifdef NEXUS_HAVE_PULSEAUDIOQT
    object->setMuted(muted);
#else
    Q_UNUSED(muted)
#endif
}

void SoundBackend::setDefaultSink(int index)
{
    NEXUS_SOUND_OBJECT(Sink, sinks, index)
#ifdef NEXUS_HAVE_PULSEAUDIOQT
    // By name through the context: Sink::setDefault() does nothing when
    // PulseAudioQt (sometimes wrongly, while lists are loading) thinks the
    // device already is the default.
    Context::instance()->setDefaultSink(object->name());
#endif
}

void SoundBackend::setSinkPort(int index, int portIndex)
{
    NEXUS_SOUND_OBJECT(Sink, sinks, index)
#ifdef NEXUS_HAVE_PULSEAUDIOQT
    if (portIndex >= 0 && portIndex < object->ports().size())
        object->setActivePortIndex(quint32(portIndex));
#else
    Q_UNUSED(portIndex)
#endif
}

void SoundBackend::setSourceVolume(int index, int percent)
{
    NEXUS_SOUND_OBJECT(Source, sources, index)
#ifdef NEXUS_HAVE_PULSEAUDIOQT
    object->setVolume(fromPercent(qBound(0, percent, maxVolume())));
#else
    Q_UNUSED(percent)
#endif
}

void SoundBackend::setSourceMuted(int index, bool muted)
{
    NEXUS_SOUND_OBJECT(Source, sources, index)
#ifdef NEXUS_HAVE_PULSEAUDIOQT
    object->setMuted(muted);
#else
    Q_UNUSED(muted)
#endif
}

void SoundBackend::setDefaultSource(int index)
{
    NEXUS_SOUND_OBJECT(Source, sources, index)
#ifdef NEXUS_HAVE_PULSEAUDIOQT
    Context::instance()->setDefaultSource(object->name());
#endif
}

void SoundBackend::setSourcePort(int index, int portIndex)
{
    NEXUS_SOUND_OBJECT(Source, sources, index)
#ifdef NEXUS_HAVE_PULSEAUDIOQT
    if (portIndex >= 0 && portIndex < object->ports().size())
        object->setActivePortIndex(quint32(portIndex));
#else
    Q_UNUSED(portIndex)
#endif
}

void SoundBackend::setStreamVolume(int index, int percent)
{
    NEXUS_SOUND_OBJECT(SinkInput, sinkInputs, index)
#ifdef NEXUS_HAVE_PULSEAUDIOQT
    object->setVolume(fromPercent(qBound(0, percent, maxVolume())));
#else
    Q_UNUSED(percent)
#endif
}

void SoundBackend::setStreamMuted(int index, bool muted)
{
    NEXUS_SOUND_OBJECT(SinkInput, sinkInputs, index)
#ifdef NEXUS_HAVE_PULSEAUDIOQT
    object->setMuted(muted);
#else
    Q_UNUSED(muted)
#endif
}

void SoundBackend::setCardProfile(int index, int profileIndex)
{
#ifdef NEXUS_HAVE_PULSEAUDIOQT
    for (const QVariant &row : std::as_const(m_allCards)) {
        const QVariantMap card = row.toMap();
        if (card.value(QStringLiteral("cardIndex")).toInt() != index)
            continue;
        const QVariantList profiles = card.value(QStringLiteral("profiles")).toList();
        if (profileIndex < 0 || profileIndex >= profiles.size())
            break;
        Context::instance()->setCardProfile(quint32(index), profiles.at(profileIndex).toMap().value(QStringLiteral("name")).toString());
        return;
    }
    Q_EMIT failed(tr("The device or profile is no longer there."));
#else
    Q_UNUSED(index)
    Q_UNUSED(profileIndex)
    Q_EMIT failed(m_statusMessage);
#endif
}

#undef NEXUS_SOUND_OBJECT

// ---- test tone ------------------------------------------------------------------

QByteArray SoundBackend::testToneWav()
{
    // Two soft notes, the first on the left channel and the second on the
    // right, so the user hears that both speakers work and are not swapped.
    const int rate = 48000;
    const int channels = 2;
    struct Segment
    {
        double seconds;
        double frequency;   // 0 = silence
        int channel;
    };
    const Segment segments[] = {
        {0.55, 523.25, 0},      // C5, left
        {0.12, 0, 0},
        {0.55, 659.25, 1},      // E5, right
        {0.10, 0, 0},
    };
    const double amplitude = 0.32;
    const double fade = 0.025;  // seconds, avoids clicks

    QByteArray pcm;
    for (const Segment &segment : segments) {
        const int frames = int(segment.seconds * rate);
        for (int i = 0; i < frames; ++i) {
            double sample = 0;
            if (segment.frequency > 0) {
                const double t = double(i) / rate;
                const double remaining = segment.seconds - t;
                double envelope = 1;
                if (t < fade)
                    envelope = 0.5 - 0.5 * std::cos(M_PI * t / fade);
                else if (remaining < fade)
                    envelope = 0.5 - 0.5 * std::cos(M_PI * remaining / fade);
                // A little second harmonic makes the tone audible on small speakers.
                sample = amplitude * envelope * (std::sin(2 * M_PI * segment.frequency * t)
                                                 + 0.25 * std::sin(4 * M_PI * segment.frequency * t)) / 1.25;
            }
            const qint16 value = qToLittleEndian(qint16(qRound(sample * 32767)));
            const qint16 silence = 0;
            for (int channel = 0; channel < channels; ++channel) {
                const qint16 out = channel == segment.channel ? value : silence;
                pcm.append(reinterpret_cast<const char *>(&out), sizeof(out));
            }
        }
    }

    QByteArray wav;
    const auto u32 = [&wav](quint32 v) { v = qToLittleEndian(v); wav.append(reinterpret_cast<const char *>(&v), 4); };
    const auto u16 = [&wav](quint16 v) { v = qToLittleEndian(v); wav.append(reinterpret_cast<const char *>(&v), 2); };
    wav.append("RIFF");
    u32(quint32(36 + pcm.size()));
    wav.append("WAVEfmt ");
    u32(16);                                // PCM format chunk
    u16(1);                                 // linear PCM
    u16(quint16(channels));
    u32(quint32(rate));
    u32(quint32(rate * channels * 2));      // bytes per second
    u16(quint16(channels * 2));             // block align
    u16(16);                                // bits per sample
    wav.append("data");
    u32(quint32(pcm.size()));
    wav.append(pcm);
    return wav;
}

void SoundBackend::playTestTone(int sinkIndex)
{
    if (m_testing)
        return;
    QString sinkName = m_defaultSinkName;
    if (sinkIndex >= 0) {
        sinkName.clear();
        const QVariantList rows = m_sinks->rows();
        for (const QVariant &row : rows) {
            const QVariantMap map = row.toMap();
            if (map.value(QStringLiteral("deviceIndex")).toInt() == sinkIndex)
                sinkName = map.value(QStringLiteral("name")).toString();
        }
    }
    if (!available() || sinkName.isEmpty()) {
        Q_EMIT failed(tr("There is no output device to play the test tone on."));
        return;
    }

    auto *file = new QTemporaryFile(QDir::tempPath() + QStringLiteral("/nexus-test-tone-XXXXXX.wav"), this);
    const QByteArray wav = testToneWav();
    if (!file->open() || file->write(wav) != wav.size() || !file->flush()) {
        delete file;
        Q_EMIT failed(tr("The test tone could not be written to a temporary file."));
        return;
    }

    QString program;
    QStringList arguments;
    if (Nexus::Command::available(QStringLiteral("paplay"))) {
        program = QStringLiteral("paplay");
        arguments = {QStringLiteral("--device=") + sinkName, QStringLiteral("--client-name=NEXUS Control Center"),
                     QStringLiteral("--stream-name=Test tone"), file->fileName()};
    } else if (Nexus::Command::available(QStringLiteral("pw-play"))) {
        program = QStringLiteral("pw-play");
        arguments = {QStringLiteral("--target"), sinkName, file->fileName()};
    } else {
        delete file;
        Q_EMIT failed(tr("Neither paplay (package pulseaudio-utils) nor pw-play (package pipewire-bin) is installed, "
                         "so the test tone cannot be played."));
        return;
    }

    m_toneFile = file;
    m_testing = true;
    Q_EMIT testingChanged();
    Nexus::Command::run(this, program, arguments, [this](const Nexus::CommandResult &result) {
        delete m_toneFile;
        m_toneFile = nullptr;
        m_testing = false;
        Q_EMIT testingChanged();
        if (!result.ok()) {
            QString reason = firstErrorLine(result.errText());
            if (reason.isEmpty())
                reason = result.exitCode == -2 ? tr("the player did not finish in time") : tr("exit code %1").arg(result.exitCode);
            Q_EMIT failed(tr("The test tone could not be played: %1").arg(reason));
        }
    }, 15000);
}

// ---- input level meter ----------------------------------------------------------------

void SoundBackend::setMeterEnabled(bool enabled)
{
    if (enabled == m_meterEnabled)
        return;
    m_meterEnabled = enabled;
    restartMeter();
    Q_EMIT meterChanged();
}

void SoundBackend::stopMeter()
{
    m_meterDecay.stop();
    if (m_meter) {
        m_meter->disconnect(this);
        m_meter->kill();
        m_meter->waitForFinished(300);
        delete m_meter;
        m_meter = nullptr;
    }
    m_meterRemainder.clear();
    if (m_inputLevel > 0) {
        m_inputLevel = 0;
        Q_EMIT inputLevelChanged();
    }
}

void SoundBackend::restartMeter()
{
    const QString wanted = m_meterEnabled && available() ? m_defaultSourceName : QString();
    if (wanted == m_meterSource && (m_meter || wanted.isEmpty()))
        return;
    stopMeter();
    m_meterSource = wanted;
    m_meterStatus.clear();
    if (wanted.isEmpty()) {
        Q_EMIT meterChanged();
        return;
    }
    if (!Nexus::Command::available(QStringLiteral("parec"))) {
        m_meterStatus = tr("The level meter needs parec from the pulseaudio-utils package.");
        Q_EMIT meterChanged();
        return;
    }

    m_meter = new QProcess(this);
    m_meter->setProgram(QStringLiteral("parec"));
    m_meter->setArguments({QStringLiteral("--device=") + wanted, QStringLiteral("--raw"), QStringLiteral("--format=s16le"),
                           QStringLiteral("--rate=8000"), QStringLiteral("--channels=1"), QStringLiteral("--latency-msec=40"),
                           QStringLiteral("--client-name=NEXUS Control Center"), QStringLiteral("--stream-name=Input level meter")});
    m_meter->setProcessEnvironment(Nexus::Command::parseEnvironment());
    m_meter->setStandardErrorFile(QProcess::nullDevice());
    connect(m_meter, &QProcess::readyReadStandardOutput, this, &SoundBackend::readMeter);
    const auto stopped = [this] {
        // Not restarted automatically: a recorder that keeps failing must not spin.
        QProcess *process = m_meter;
        m_meter = nullptr;
        if (process)
            process->deleteLater();
        m_meterDecay.stop();
        m_meterStatus = tr("The level meter stopped because the recorder (parec) ended.");
        if (m_inputLevel > 0) {
            m_inputLevel = 0;
            Q_EMIT inputLevelChanged();
        }
        Q_EMIT meterChanged();
    };
    connect(m_meter, &QProcess::finished, this, stopped);
    connect(m_meter, &QProcess::errorOccurred, this, [stopped](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart)
            stopped();
    });
    m_meter->start(QIODevice::ReadOnly);
    m_meterDecay.start();
    Q_EMIT meterChanged();
}

void SoundBackend::readMeter()
{
    if (!m_meter)
        return;
    QByteArray data = m_meterRemainder + m_meter->readAllStandardOutput();
    const qsizetype samples = data.size() / 2;
    m_meterRemainder = data.mid(samples * 2);
    if (samples == 0)
        return;
    m_meterGotData = true;
    int peak = 0;
    const auto *bytes = reinterpret_cast<const uchar *>(data.constData());
    for (qsizetype i = 0; i < samples; ++i) {
        const int value = qFromLittleEndian<qint16>(bytes + i * 2);
        peak = qMax(peak, qAbs(value));
    }
    const qreal level = qMin(1.0, peak / 32767.0);
    // Rise at once, fall smoothly (the decay timer takes over during silence).
    const qreal next = level >= m_inputLevel ? level : qMax(level, m_inputLevel * 0.8);
    if (qAbs(next - m_inputLevel) > 0.004) {
        m_inputLevel = next;
        Q_EMIT inputLevelChanged();
    }
}

QJsonObject SoundBackend::toJson() const
{
    return {
        {QStringLiteral("state"), m_state},
        {QStringLiteral("available"), available()},
        {QStringLiteral("statusMessage"), m_statusMessage},
        {QStringLiteral("server"), m_serverName},
        {QStringLiteral("defaultSink"), m_defaultSinkName},
        {QStringLiteral("defaultSource"), m_defaultSourceName},
        {QStringLiteral("sinks"), QJsonArray::fromVariantList(m_sinks->rows())},
        {QStringLiteral("sources"), QJsonArray::fromVariantList(m_sources->rows())},
        {QStringLiteral("streams"), QJsonArray::fromVariantList(m_streams->rows())},
        {QStringLiteral("cards"), QJsonArray::fromVariantList(m_allCards)},
        {QStringLiteral("allowOveramplification"), m_allowOveramplification},
        {QStringLiteral("maxVolume"), maxVolume()},
        {QStringLiteral("testToneBytes"), int(testToneWav().size())},
    };
}
