// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QDateTime>
#include <QFileSystemWatcher>
#include <QJsonObject>
#include <QObject>
#include <QQmlEngine>
#include <QTimer>
#include <QVariantList>

class QDBusMessage;

// Date & Time page.
//
// System state comes from and is changed through systemd-timedated,
// org.freedesktop.timedate1 on the system bus (object /org/freedesktop/timedate1):
//
//   properties  Timezone, LocalRTC, CanNTP, NTP, NTPSynchronized
//   methods     SetTimezone(s, b interactive), SetNTP(b, b interactive),
//               SetTime(x usec_utc, b relative, b interactive)
//
// The service authorises each change through polkit (interactive = true, so
// the session's agent asks). When the service cannot be reached the page is
// read-only and shows what can be read without it (/etc/localtime,
// /etc/adjtime).
//
// Time zones are listed from tzdata: /usr/share/zoneinfo/zone1970.tab with
// country names from iso3166.tab (falling back to the zones Qt knows).
//
// The panel clock options are the [Panel] keys of ~/.config/nexus/horizon.conf
// (see desktop/horizon-shell/src/shellsettings.h); the shell watches the file.
class DateTimeBackend : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(bool ready READ ready NOTIFY stateChanged)
    Q_PROPERTY(bool available READ available NOTIFY stateChanged)
    Q_PROPERTY(QString statusMessage READ statusMessage NOTIFY stateChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    Q_PROPERTY(QString timezone READ timezone NOTIFY stateChanged)
    Q_PROPERTY(QString timezoneLabel READ timezoneLabel NOTIFY stateChanged)
    Q_PROPERTY(bool canNtp READ canNtp NOTIFY stateChanged)
    Q_PROPERTY(bool ntp READ ntp NOTIFY stateChanged)
    Q_PROPERTY(bool ntpSynchronized READ ntpSynchronized NOTIFY stateChanged)
    Q_PROPERTY(bool localRtcKnown READ localRtcKnown NOTIFY stateChanged)
    Q_PROPERTY(bool localRtc READ localRtc NOTIFY stateChanged)
    Q_PROPERTY(QString localRtcSource READ localRtcSource NOTIFY stateChanged)
    Q_PROPERTY(QString currentTime READ currentTime NOTIFY tick)
    Q_PROPERTY(QString currentDate READ currentDate NOTIFY tick)
    Q_PROPERTY(QString utcOffsetText READ utcOffsetText NOTIFY tick)
    Q_PROPERTY(QString isoDate READ isoDate NOTIFY tick)
    Q_PROPERTY(QString isoTime READ isoTime NOTIFY tick)
    Q_PROPERTY(QStringList regions READ regions CONSTANT)
    Q_PROPERTY(QString region READ region WRITE setRegion NOTIFY zonesChanged)
    Q_PROPERTY(QString filter READ filter WRITE setFilter NOTIFY zonesChanged)
    Q_PROPERTY(QVariantList zones READ zones NOTIFY zonesChanged)
    Q_PROPERTY(int zoneCount READ zoneCount CONSTANT)
    Q_PROPERTY(QString zoneSource READ zoneSource CONSTANT)
    Q_PROPERTY(bool clock24h READ clock24h NOTIFY panelChanged)
    Q_PROPERTY(bool clockSeconds READ clockSeconds NOTIFY panelChanged)
    Q_PROPERTY(bool clockDate READ clockDate NOTIFY panelChanged)
    Q_PROPERTY(QString settingsFile READ settingsFile CONSTANT)

public:
    explicit DateTimeBackend(QObject *parent = nullptr);

    bool ready() const { return m_ready; }
    bool available() const { return m_available; }
    QString statusMessage() const { return m_statusMessage; }
    bool busy() const { return m_busy; }
    QString timezone() const { return m_timezone; }
    QString timezoneLabel() const;
    bool canNtp() const { return m_canNtp; }
    bool ntp() const { return m_ntp; }
    bool ntpSynchronized() const { return m_ntpSynchronized; }
    bool localRtcKnown() const { return m_localRtcKnown; }
    bool localRtc() const { return m_localRtc; }
    QString localRtcSource() const { return m_localRtcSource; }
    QString currentTime() const;
    QString currentDate() const;
    QString utcOffsetText() const;
    QString isoDate() const;
    QString isoTime() const;
    QStringList regions() const { return m_regions; }
    QString region() const { return m_region; }
    QString filter() const { return m_filter; }
    QVariantList zones() const;
    int zoneCount() const { return m_zones.size(); }
    QString zoneSource() const { return m_zoneSource; }
    bool clock24h() const { return m_clock24h; }
    bool clockSeconds() const { return m_clockSeconds; }
    bool clockDate() const { return m_clockDate; }
    QString settingsFile() const;

    void setRegion(const QString &region);
    void setFilter(const QString &filter);

    Q_INVOKABLE void refresh();
    Q_INVOKABLE void setTimezone(const QString &id);
    Q_INVOKABLE void setNtp(bool on);
    // 'date' as yyyy-MM-dd and 'time' as HH:mm or HH:mm:ss, in the system
    // time zone. Returns an empty string if the request was sent, otherwise
    // what is wrong with the input.
    Q_INVOKABLE QString setDateTime(const QString &date, const QString &time);
    Q_INVOKABLE void setClock24h(bool on);
    Q_INVOKABLE void setClockSeconds(bool on);
    Q_INVOKABLE void setClockDate(bool on);

    QJsonObject toJson() const;

Q_SIGNALS:
    void stateChanged();
    void busyChanged();
    void tick();
    void zonesChanged();
    void panelChanged();
    void failed(const QString &message);
    void changed(const QString &message);

private Q_SLOTS:
    void onPropertiesChanged(const QString &interface, const QVariantMap &changed, const QStringList &invalidated);

private:
    struct Zone
    {
        QString id, region, city, countries, comment;
    };

    void loadZones();
    void loadPanel();
    void watchPanel();
    void storePanel(const QString &key, bool value);
    void readWithoutService(const QString &reason);
    void applyProperties(const QVariantMap &properties);
    void call(const QString &method, const QVariantList &arguments, const QString &successMessage);
    QDateTime now() const;
    static QString offsetText(int seconds);

    bool m_ready = false, m_available = false, m_busy = false;
    QString m_statusMessage;
    QString m_timezone;
    bool m_canNtp = false, m_ntp = false, m_ntpSynchronized = false;
    bool m_localRtcKnown = false, m_localRtc = false;
    QString m_localRtcSource;
    QList<Zone> m_zones;
    QStringList m_regions;
    QString m_region, m_filter, m_zoneSource;
    QTimer m_clock, m_poll;
    QFileSystemWatcher m_watcher;
    bool m_clock24h = true, m_clockSeconds = false, m_clockDate = true;
};
