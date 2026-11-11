// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QAbstractItemModel>
#include <QJsonObject>
#include <QObject>
#include <QProcess>
#include <QQmlEngine>
#include <QTimer>
#include <QVariantList>

namespace NetHw {
class RowModel;
}
class QTemporaryFile;

// Sound page: output and input devices, per-application volume and card
// profiles of the session's sound server.
//
// Source: the PulseAudio protocol through PulseAudioQt (PipeWire's
// pipewire-pulse on NEXUS; a real PulseAudio daemon works the same way).
// PulseAudioQt delivers every change as a signal, so the models are always
// current while the page is open.
//
//   sinks / sources   name, description, volume (percent of the normal
//                     volume), mute, default flag, ports and the active port
//   streams           playback streams (sink inputs) with the application
//                     name, volume and mute
//   cards             profiles and the active profile
//
// Without PulseAudioQt (NEXUS_HAVE_PULSEAUDIOQT undefined) the state is
// "unsupported-build".
//
// Helpers from the sound server's command-line tools:
//   test tone         a WAV file synthesised in memory and played on the
//                     chosen sink with paplay (pulseaudio-utils) or, failing
//                     that, pw-play (pipewire-bin)
//   input level       parec (pulseaudio-utils) records the default source as
//                     raw 16-bit mono at a low rate; the peak of each chunk
//                     drives the meter. Nothing is stored.
//
// The "allow more than 100 %" switch is a preference of this page, kept in
// ~/.config/nexus/controlcenter.conf ([Sound] AllowOveramplification).
class SoundBackend : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(bool ready READ ready NOTIFY readyChanged)
    Q_PROPERTY(bool available READ available NOTIFY stateChanged)
    Q_PROPERTY(QString state READ state NOTIFY stateChanged)
    Q_PROPERTY(QString statusMessage READ statusMessage NOTIFY stateChanged)
    Q_PROPERTY(QString serverName READ serverName NOTIFY stateChanged)

    Q_PROPERTY(QAbstractItemModel *sinks READ sinks CONSTANT)
    Q_PROPERTY(QAbstractItemModel *sources READ sources CONSTANT)
    Q_PROPERTY(QAbstractItemModel *streams READ streams CONSTANT)
    Q_PROPERTY(QAbstractItemModel *cards READ cards CONSTANT)
    Q_PROPERTY(int sinkCount READ sinkCount NOTIFY devicesChanged)
    Q_PROPERTY(int sourceCount READ sourceCount NOTIFY devicesChanged)
    Q_PROPERTY(int streamCount READ streamCount NOTIFY devicesChanged)
    Q_PROPERTY(int cardCount READ cardCount NOTIFY devicesChanged)
    Q_PROPERTY(QString defaultSinkName READ defaultSinkName NOTIFY devicesChanged)
    Q_PROPERTY(QString defaultSourceName READ defaultSourceName NOTIFY devicesChanged)
    Q_PROPERTY(QString defaultSourceDescription READ defaultSourceDescription NOTIFY devicesChanged)

    Q_PROPERTY(bool allowOveramplification READ allowOveramplification WRITE setAllowOveramplification NOTIFY overamplificationChanged)
    Q_PROPERTY(int maxVolume READ maxVolume NOTIFY overamplificationChanged)

    Q_PROPERTY(bool testing READ testing NOTIFY testingChanged)
    Q_PROPERTY(bool meterEnabled READ meterEnabled WRITE setMeterEnabled NOTIFY meterChanged)
    Q_PROPERTY(bool meterRunning READ meterRunning NOTIFY meterChanged)
    Q_PROPERTY(QString meterStatus READ meterStatus NOTIFY meterChanged)
    Q_PROPERTY(qreal inputLevel READ inputLevel NOTIFY inputLevelChanged)

public:
    explicit SoundBackend(QObject *parent = nullptr);
    ~SoundBackend() override;

    bool ready() const { return m_ready; }
    // "ok", "connecting", "no-server" or "unsupported-build".
    QString state() const { return m_state; }
    bool available() const { return m_state == QLatin1String("ok"); }
    QString statusMessage() const { return m_statusMessage; }
    QString serverName() const { return m_serverName; }

    QAbstractItemModel *sinks() const;
    QAbstractItemModel *sources() const;
    QAbstractItemModel *streams() const;
    QAbstractItemModel *cards() const;
    int sinkCount() const;
    int sourceCount() const;
    int streamCount() const;
    int cardCount() const;
    QString defaultSinkName() const { return m_defaultSinkName; }
    QString defaultSourceName() const { return m_defaultSourceName; }
    QString defaultSourceDescription() const { return m_defaultSourceDescription; }

    bool allowOveramplification() const { return m_allowOveramplification; }
    void setAllowOveramplification(bool allow);
    int maxVolume() const { return m_allowOveramplification ? 150 : 100; }

    bool testing() const { return m_testing; }
    bool meterEnabled() const { return m_meterEnabled; }
    void setMeterEnabled(bool enabled);
    bool meterRunning() const { return m_meter != nullptr; }
    QString meterStatus() const { return m_meterStatus; }
    qreal inputLevel() const { return m_inputLevel; }

    // 'index' is the device's / stream's / card's index on the sound server.
    Q_INVOKABLE void setSinkVolume(int index, int percent);
    Q_INVOKABLE void setSinkMuted(int index, bool muted);
    Q_INVOKABLE void setDefaultSink(int index);
    Q_INVOKABLE void setSinkPort(int index, int portIndex);
    Q_INVOKABLE void setSourceVolume(int index, int percent);
    Q_INVOKABLE void setSourceMuted(int index, bool muted);
    Q_INVOKABLE void setDefaultSource(int index);
    Q_INVOKABLE void setSourcePort(int index, int portIndex);
    Q_INVOKABLE void setStreamVolume(int index, int percent);
    Q_INVOKABLE void setStreamMuted(int index, bool muted);
    Q_INVOKABLE void setCardProfile(int index, int profileIndex);

    // Play the test tone on the sink with this index (-1: the default sink).
    Q_INVOKABLE void playTestTone(int sinkIndex = -1);
    Q_INVOKABLE void reconnect();

    // The test tone as a complete RIFF/WAVE file (48 kHz, stereo, 16 bit).
    static QByteArray testToneWav();

    QJsonObject toJson() const;

Q_SIGNALS:
    void readyChanged();
    void stateChanged();
    void devicesChanged();
    void overamplificationChanged();
    void testingChanged();
    void meterChanged();
    void inputLevelChanged();
    void failed(const QString &message);

private Q_SLOTS:
    void scheduleRebuild();
    void rebuild();
    void updateState();

private:
    void setState(const QString &state, const QString &message, const QString &serverName);
    void track(QObject *object);
    void restartMeter();
    void stopMeter();
    void readMeter();

    bool m_ready = false;
    QString m_state;
    QString m_statusMessage;
    QString m_serverName;
    QTimer m_rebuildTimer;
    QTimer m_connectTimer;
    bool m_connectTimedOut = false;
    bool m_serverSynced = false;

    NetHw::RowModel *m_sinks = nullptr;
    NetHw::RowModel *m_sources = nullptr;
    NetHw::RowModel *m_streams = nullptr;
    NetHw::RowModel *m_cards = nullptr;
    QAbstractItemModel *m_cardModel = nullptr;     // PulseAudioQt::CardModel
    QVariantList m_allCards;
    QString m_defaultSinkName;
    QString m_defaultSourceName;
    QString m_defaultSourceDescription;

    bool m_allowOveramplification = false;

    bool m_testing = false;
    QTemporaryFile *m_toneFile = nullptr;

    bool m_meterEnabled = false;
    QProcess *m_meter = nullptr;
    QString m_meterSource;
    QString m_meterStatus;
    QByteArray m_meterRemainder;
    bool m_meterGotData = false;
    qreal m_inputLevel = 0;
    QTimer m_meterDecay;
};
