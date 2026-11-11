// SPDX-License-Identifier: GPL-3.0-or-later
#include "statusservice.h"

#include "statusaudio.h"
#include "statusbluetooth.h"
#include "statusbrightness.h"
#include "statuscalendar.h"
#include "statusmedia.h"
#include "statusnetwork.h"
#include "statusnightlight.h"
#include "statuspower.h"
#include "statusradio.h"

namespace StatusModule {

Service::Service(QObject *parent)
    : QObject(parent)
{
    add("audio", m_audio = new AudioBackend(this));
    add("network", m_network = new NetworkBackend(this));
    add("bluetooth", m_bluetooth = new BluetoothBackend(this));
    add("airplaneMode", m_radio = new RadioBackend(this));
    add("battery", m_battery = new BatteryBackend(this));
    add("powerProfiles", m_powerProfiles = new PowerProfilesBackend(this));
    add("brightness", m_brightness = new BrightnessBackend(this));
    add("media", m_media = new MediaBackend(this));
    add("nightLight", m_nightLight = new NightLightBackend(this));
    m_calendar = new CalendarBackend(this);
}

void Service::add(const char *name, Backend *backend)
{
    m_backends.append({name, backend});
    connect(backend, &Backend::failed, this, &Service::failed);
    connect(backend, &Backend::readyChanged, this, &Service::backendReady);
}

QObject *Service::audio() const { return m_audio; }
QObject *Service::network() const { return m_network; }
QObject *Service::bluetooth() const { return m_bluetooth; }
QObject *Service::radio() const { return m_radio; }
QObject *Service::battery() const { return m_battery; }
QObject *Service::powerProfiles() const { return m_powerProfiles; }
QObject *Service::brightness() const { return m_brightness; }
QObject *Service::media() const { return m_media; }
QObject *Service::nightLight() const { return m_nightLight; }
QObject *Service::calendar() const { return m_calendar; }

bool Service::allReady() const
{
    for (const Entry &entry : m_backends) {
        if (!entry.backend->isReady())
            return false;
    }
    return true;
}

QJsonObject Service::toJson() const
{
    QJsonObject json;
    for (const Entry &entry : m_backends)
        json.insert(QString::fromLatin1(entry.name), entry.backend->toJson());
    json.insert(QStringLiteral("calendar"), m_calendar->toJson());
    return json;
}

} // namespace StatusModule
