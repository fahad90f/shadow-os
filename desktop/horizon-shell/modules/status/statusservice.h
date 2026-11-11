// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QJsonObject>
#include <QList>
#include <QObject>

namespace StatusModule {

class AudioBackend;
class Backend;
class BatteryBackend;
class BluetoothBackend;
class BrightnessBackend;
class CalendarBackend;
class MediaBackend;
class NetworkBackend;
class NightLightBackend;
class PowerProfilesBackend;
class RadioBackend;

// Everything the status module's QML shows, as one object: the context
// property "status". Each member is a back-end with its own "available".
//
//   status.audio          volume and microphone       (PulseAudio protocol)
//   status.network        connection, Wi-Fi, VPN      (NetworkManager)
//   status.bluetooth      adapters, connected devices (BlueZ)
//   status.radio          airplane mode               (/dev/rfkill)
//   status.battery        charge and state            (UPower)
//   status.powerProfiles  power profile               (power-profiles-daemon)
//   status.brightness     screen backlight            (/sys + logind)
//   status.media          current media player        (MPRIS2)
//   status.nightLight     Night Light                 (KWin)
//   status.calendar       dates for the calendar popup
class Service : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QObject *audio READ audio CONSTANT)
    Q_PROPERTY(QObject *network READ network CONSTANT)
    Q_PROPERTY(QObject *bluetooth READ bluetooth CONSTANT)
    Q_PROPERTY(QObject *radio READ radio CONSTANT)
    Q_PROPERTY(QObject *battery READ battery CONSTANT)
    Q_PROPERTY(QObject *powerProfiles READ powerProfiles CONSTANT)
    Q_PROPERTY(QObject *brightness READ brightness CONSTANT)
    Q_PROPERTY(QObject *media READ media CONSTANT)
    Q_PROPERTY(QObject *nightLight READ nightLight CONSTANT)
    Q_PROPERTY(QObject *calendar READ calendar CONSTANT)

public:
    explicit Service(QObject *parent = nullptr);

    QObject *audio() const;
    QObject *network() const;
    QObject *bluetooth() const;
    QObject *radio() const;
    QObject *battery() const;
    QObject *powerProfiles() const;
    QObject *brightness() const;
    QObject *media() const;
    QObject *nightLight() const;
    QObject *calendar() const;

    // True once every back-end has finished its first query.
    bool allReady() const;
    // The document of "horizon-shell --dump status".
    QJsonObject toJson() const;

Q_SIGNALS:
    // A back-end could not do what the user asked for.
    void failed(const QString &message);
    // A back-end finished its first query.
    void backendReady();

private:
    struct Entry { const char *name; Backend *backend; };
    void add(const char *name, Backend *backend);

    QList<Entry> m_backends;
    AudioBackend *m_audio;
    NetworkBackend *m_network;
    BluetoothBackend *m_bluetooth;
    RadioBackend *m_radio;
    BatteryBackend *m_battery;
    PowerProfilesBackend *m_powerProfiles;
    BrightnessBackend *m_brightness;
    MediaBackend *m_media;
    NightLightBackend *m_nightLight;
    CalendarBackend *m_calendar;
};

} // namespace StatusModule
