// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QByteArray>
#include <QHash>
#include <QList>
#include <QString>
#include <QStringList>
#include <QVariantMap>

// One network interface: counters from /proc/net/dev, link facts from
// /sys/class/net/<name>/, addresses from getifaddrs(3).
struct InterfaceInfo
{
    QString name;
    QString kind;                   // "ethernet", "wifi", "loopback", "virtual" or "other"
    QString state;                  // operstate: "up", "down", "dormant", ...
    bool up = false;                // link is operational
    QString mac;
    int mtu = 0;
    int speedMbps = -1;             // negotiated link speed; -1 when the driver does not report one
    QStringList addresses;          // "192.0.2.10/24", "2001:db8::1/64"
    quint64 rxBytes = 0;            // since boot
    quint64 txBytes = 0;
    quint64 rxPackets = 0;
    quint64 txPackets = 0;
    quint64 rxErrors = 0;
    quint64 txErrors = 0;
    quint64 rxDropped = 0;
    quint64 txDropped = 0;
    double rxRate = 0;              // bytes per second since the previous sample
    double txRate = 0;

    bool loopback() const { return kind == QLatin1String("loopback"); }
    QVariantMap toVariant() const;
};

struct NetworkSample
{
    bool ratesValid = false;        // rates need two readings
    QList<InterfaceInfo> interfaces;
    double rxRate = 0;              // all interfaces except loopback
    double txRate = 0;

    QVariantMap toVariant() const;
};

class NetworkSource
{
public:
    NetworkSample sample();
    void resetRates();

private:
    struct Counters
    {
        quint64 rxBytes = 0;
        quint64 txBytes = 0;
    };

    QByteArray m_buffer;
    QHash<QString, Counters> m_previous;
    double m_previousTime = 0;
};
