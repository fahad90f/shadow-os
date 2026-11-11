// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "statusbackend.h"

#include <QPointer>
#include <QTimer>

namespace PulseAudioQt {
class Sink;
class Source;
}

namespace StatusModule {

// Volume and microphone of the session's sound server.
//
// Source: the PulseAudio protocol through PulseAudioQt — PipeWire's
// pipewire-pulse on NEXUS; a real PulseAudio daemon works the same way.
// PulseAudioQt reports every change as a signal, so the values are live.
//
//   output   the server's default sink. PipeWire's "auto_null" stand-in
//            ("Dummy Output", what it reports while there is no output
//            device at all) is not treated as a device.
//   input    the server's default source, unless that is only the monitor
//            of an output (then the machine has no microphone).
//
// Levels are fractions of the normal volume (1.0 = 100 %). They can be
// above 1 when another program amplified the device; this back-end never
// raises a level above 1 itself.
class AudioBackend : public Backend
{
    Q_OBJECT
    Q_PROPERTY(QString serverName READ serverName NOTIFY serverChanged)
    Q_PROPERTY(bool outputAvailable READ outputAvailable NOTIFY outputChanged)
    Q_PROPERTY(QString outputName READ outputName NOTIFY outputChanged)
    Q_PROPERTY(qreal volume READ volume NOTIFY outputChanged)
    Q_PROPERTY(bool muted READ muted NOTIFY outputChanged)
    Q_PROPERTY(bool inputAvailable READ inputAvailable NOTIFY inputChanged)
    Q_PROPERTY(QString inputName READ inputName NOTIFY inputChanged)
    Q_PROPERTY(qreal inputVolume READ inputVolume NOTIFY inputChanged)
    Q_PROPERTY(bool inputMuted READ inputMuted NOTIFY inputChanged)

public:
    explicit AudioBackend(QObject *parent = nullptr);
    ~AudioBackend() override;

    QString serverName() const;
    bool outputAvailable() const;
    QString outputName() const;
    qreal volume() const;
    bool muted() const;
    bool inputAvailable() const;
    QString inputName() const;
    qreal inputVolume() const;
    bool inputMuted() const;

    // Sets the output level (0–1). Raising it from silence also unmutes.
    Q_INVOKABLE void setVolume(qreal level);
    // Volume keys: moves to the next multiple of 5 % in that direction;
    // going up also unmutes.
    Q_INVOKABLE void stepVolume(int direction);
    Q_INVOKABLE void setMuted(bool muted);
    Q_INVOKABLE void toggleMuted();
    Q_INVOKABLE void setInputVolume(qreal level);
    Q_INVOKABLE void setInputMuted(bool muted);
    Q_INVOKABLE void toggleInputMuted();

Q_SIGNALS:
    void serverChanged();
    void outputChanged();
    void inputChanged();

protected:
    QJsonObject details() const override;

private:
    void updateState();
    void rebind();

    QPointer<PulseAudioQt::Sink> m_sink;
    QPointer<PulseAudioQt::Source> m_source;
    QTimer m_settle;
    QTimer m_retry;
    bool m_serverKnown = false;
    bool m_settled = false;
};

} // namespace StatusModule
