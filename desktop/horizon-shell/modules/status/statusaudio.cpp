// SPDX-License-Identifier: GPL-3.0-or-later
#include "statusaudio.h"

#include <PulseAudioQt/Context>
#include <PulseAudioQt/Server>
#include <PulseAudioQt/Sink>
#include <PulseAudioQt/Source>

#include <QtMath>

using PulseAudioQt::Context;

namespace StatusModule {

namespace {

// What PipeWire's pulse server reports while there is no output device.
bool isPlaceholder(const PulseAudioQt::Sink *sink)
{
    return sink->name() == QLatin1String("auto_null");
}

// The "source" that merely records what an output plays.
bool isMonitor(const PulseAudioQt::Source *source)
{
    return source->properties().value(QStringLiteral("device.class")).toString() == QLatin1String("monitor")
           || source->name().endsWith(QLatin1String(".monitor"));
}

qreal toLevel(qint64 volume)
{
    return qreal(volume) / qreal(PulseAudioQt::normalVolume());
}

qint64 fromLevel(qreal level)
{
    return qRound64(qBound(0.0, level, 1.0) * qreal(PulseAudioQt::normalVolume()));
}

QString label(const PulseAudioQt::Device *device)
{
    return device->description().isEmpty() ? device->name() : device->description();
}

QJsonObject deviceJson(const PulseAudioQt::Device *device)
{
    if (!device)
        return {};
    return {
        {QStringLiteral("name"), device->name()},
        {QStringLiteral("description"), device->description()},
        {QStringLiteral("volumePercent"), qRound(toLevel(device->volume()) * 100)},
        {QStringLiteral("muted"), device->isMuted()},
    };
}

QString contextStateName(Context::State state)
{
    switch (state) {
    case Context::State::Unconnected: return QStringLiteral("unconnected");
    case Context::State::Connecting: return QStringLiteral("connecting");
    case Context::State::Authorizing: return QStringLiteral("authorizing");
    case Context::State::SettingName: return QStringLiteral("setting-name");
    case Context::State::Ready: return QStringLiteral("ready");
    case Context::State::Failed: return QStringLiteral("failed");
    case Context::State::Terminated: return QStringLiteral("terminated");
    }
    return QStringLiteral("unknown");
}

} // namespace

AudioBackend::AudioBackend(QObject *parent)
    : Backend(parent)
{
    Context *context = Context::instance();
    PulseAudioQt::Server *server = context->server();

    connect(context, &Context::stateChanged, this, [this, context] {
        if (context->state() != Context::State::Ready)
            m_serverKnown = false;
        updateState();
    });
    connect(context, &Context::sinkRemoved, this, &AudioBackend::rebind);
    connect(context, &Context::sourceRemoved, this, &AudioBackend::rebind);
    connect(server, &PulseAudioQt::Server::defaultSinkChanged, this, &AudioBackend::rebind);
    connect(server, &PulseAudioQt::Server::defaultSourceChanged, this, &AudioBackend::rebind);
    connect(server, &PulseAudioQt::Server::isPipeWireChanged, this, &AudioBackend::serverChanged);
    // The server information is the last thing PulseAudioQt asks for after
    // connecting; when it is in, the devices and the defaults are known.
    connect(server, &PulseAudioQt::Server::updated, this, [this] {
        m_serverKnown = true;
        updateState();
    });

    // A sound server that is not running shows as a connection attempt that
    // never completes; say "not available" after a moment instead of never.
    m_settle.setSingleShot(true);
    m_settle.setInterval(1500);
    connect(&m_settle, &QTimer::timeout, this, [this] {
        m_settled = true;
        updateState();
    });
    m_settle.start();

    // PulseAudioQt gives up reconnecting after a few attempts; keep asking
    // at a slow pace so a sound server started later is still picked up.
    m_retry.setInterval(20000);
    connect(&m_retry, &QTimer::timeout, this, [context] {
        if (context->state() != Context::State::Ready && !context->isAutoConnecting())
            context->reconnectDaemon();
    });
    m_retry.start();

    updateState();
}

AudioBackend::~AudioBackend() = default;

void AudioBackend::updateState()
{
    const Context *context = Context::instance();
    const bool up = context->state() == Context::State::Ready && m_serverKnown;
    rebind();
    if (up)
        setAvailable(true);
    else if (m_settled || isReady())
        setAvailable(false, tr("No sound server (PipeWire or PulseAudio) is running in this session."));
}

void AudioBackend::rebind()
{
    const PulseAudioQt::Server *server = Context::instance()->server();

    PulseAudioQt::Sink *sink = server->defaultSink();
    if (sink && isPlaceholder(sink))
        sink = nullptr;
    if (sink != m_sink.data()) {
        if (m_sink)
            m_sink->disconnect(this);
        m_sink = sink;
        if (sink) {
            connect(sink, &PulseAudioQt::VolumeObject::volumeChanged, this, &AudioBackend::outputChanged);
            connect(sink, &PulseAudioQt::VolumeObject::mutedChanged, this, &AudioBackend::outputChanged);
            connect(sink, &PulseAudioQt::Device::descriptionChanged, this, &AudioBackend::outputChanged);
            connect(sink, &QObject::destroyed, this, &AudioBackend::outputChanged);
        }
    }
    // Emitted unconditionally: a device that went away has already cleared
    // the guarded pointer, so "changed" cannot be told from the pointers.
    Q_EMIT outputChanged();

    PulseAudioQt::Source *source = server->defaultSource();
    if (source && isMonitor(source))
        source = nullptr;
    if (source != m_source.data()) {
        if (m_source)
            m_source->disconnect(this);
        m_source = source;
        if (source) {
            connect(source, &PulseAudioQt::VolumeObject::volumeChanged, this, &AudioBackend::inputChanged);
            connect(source, &PulseAudioQt::VolumeObject::mutedChanged, this, &AudioBackend::inputChanged);
            connect(source, &PulseAudioQt::Device::descriptionChanged, this, &AudioBackend::inputChanged);
            connect(source, &QObject::destroyed, this, &AudioBackend::inputChanged);
        }
    }
    Q_EMIT inputChanged();
}

QString AudioBackend::serverName() const
{
    if (!isAvailable())
        return {};
    return Context::instance()->server()->isPipeWire() ? QStringLiteral("PipeWire") : QStringLiteral("PulseAudio");
}

bool AudioBackend::outputAvailable() const { return !m_sink.isNull(); }
QString AudioBackend::outputName() const { return m_sink ? label(m_sink) : QString(); }
qreal AudioBackend::volume() const { return m_sink ? toLevel(m_sink->volume()) : 0; }
bool AudioBackend::muted() const { return m_sink && m_sink->isMuted(); }
bool AudioBackend::inputAvailable() const { return !m_source.isNull(); }
QString AudioBackend::inputName() const { return m_source ? label(m_source) : QString(); }
qreal AudioBackend::inputVolume() const { return m_source ? toLevel(m_source->volume()) : 0; }
bool AudioBackend::inputMuted() const { return m_source && m_source->isMuted(); }

void AudioBackend::setVolume(qreal level)
{
    if (!m_sink)
        return;
    level = qBound(0.0, level, 1.0);
    m_sink->setVolume(fromLevel(level));
    if (level > 0 && m_sink->isMuted())
        m_sink->setMuted(false);
}

void AudioBackend::stepVolume(int direction)
{
    if (!m_sink || direction == 0)
        return;
    const int percent = qRound(volume() * 100);
    int target;
    if (direction > 0) {
        target = (percent / 5 + 1) * 5;
        // Never amplify; a level someone else set above 100 % stays.
        target = qMax(percent, qMin(target, 100));
    } else {
        target = qMax(0, ((percent + 4) / 5 - 1) * 5);
    }
    if (target != percent)
        m_sink->setVolume(qRound64(qreal(target) / 100.0 * qreal(PulseAudioQt::normalVolume())));
    if (direction > 0 && m_sink->isMuted())
        m_sink->setMuted(false);
}

void AudioBackend::setMuted(bool muted)
{
    if (m_sink)
        m_sink->setMuted(muted);
}

void AudioBackend::toggleMuted()
{
    if (m_sink)
        m_sink->setMuted(!m_sink->isMuted());
}

void AudioBackend::setInputVolume(qreal level)
{
    if (!m_source)
        return;
    level = qBound(0.0, level, 1.0);
    m_source->setVolume(fromLevel(level));
    if (level > 0 && m_source->isMuted())
        m_source->setMuted(false);
}

void AudioBackend::setInputMuted(bool muted)
{
    if (m_source)
        m_source->setMuted(muted);
}

void AudioBackend::toggleInputMuted()
{
    if (m_source)
        m_source->setMuted(!m_source->isMuted());
}

QJsonObject AudioBackend::details() const
{
    QJsonObject json{
        {QStringLiteral("connection"), contextStateName(Context::instance()->state())},
        {QStringLiteral("outputAvailable"), outputAvailable()},
        {QStringLiteral("inputAvailable"), inputAvailable()},
    };
    if (isAvailable())
        json.insert(QStringLiteral("server"), serverName());
    if (m_sink)
        json.insert(QStringLiteral("output"), deviceJson(m_sink));
    if (m_source)
        json.insert(QStringLiteral("input"), deviceJson(m_source));
    return json;
}

} // namespace StatusModule
