// SPDX-License-Identifier: GPL-3.0-or-later
#include "network.h"

#include "sysfs.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>

using namespace Qt::StringLiterals;

namespace {

struct AddressInfo
{
    unsigned int flags = 0;
    bool haveFlags = false;
    QStringList addresses;
};

int prefixLength(const unsigned char *mask, int bytes)
{
    int bits = 0;
    for (int i = 0; i < bytes; ++i) {
        unsigned char b = mask[i];
        while (b & 0x80) {
            ++bits;
            b = static_cast<unsigned char>(b << 1);
        }
        if (mask[i] != 0xff)
            break;
    }
    return bits;
}

// Interface flags and IPv4/IPv6 addresses, by interface name.
QHash<QString, AddressInfo> readAddresses()
{
    QHash<QString, AddressInfo> result;
    struct ifaddrs *list = nullptr;
    if (::getifaddrs(&list) != 0)
        return result;
    for (const struct ifaddrs *ifa = list; ifa; ifa = ifa->ifa_next) {
        if (!ifa->ifa_name)
            continue;
        AddressInfo &info = result[QString::fromLocal8Bit(ifa->ifa_name)];
        info.flags = ifa->ifa_flags;
        info.haveFlags = true;
        if (!ifa->ifa_addr)
            continue;
        char text[INET6_ADDRSTRLEN] = {};
        if (ifa->ifa_addr->sa_family == AF_INET) {
            const auto *addr = reinterpret_cast<const struct sockaddr_in *>(ifa->ifa_addr);
            if (!::inet_ntop(AF_INET, &addr->sin_addr, text, sizeof text))
                continue;
            QString entry = QString::fromLatin1(text);
            if (ifa->ifa_netmask) {
                const auto *mask = reinterpret_cast<const struct sockaddr_in *>(ifa->ifa_netmask);
                entry += u'/';
                entry += QString::number(prefixLength(reinterpret_cast<const unsigned char *>(&mask->sin_addr), 4));
            }
            info.addresses.prepend(entry);      // IPv4 first
        } else if (ifa->ifa_addr->sa_family == AF_INET6) {
            const auto *addr = reinterpret_cast<const struct sockaddr_in6 *>(ifa->ifa_addr);
            if (!::inet_ntop(AF_INET6, &addr->sin6_addr, text, sizeof text))
                continue;
            QString entry = QString::fromLatin1(text);
            if (ifa->ifa_netmask) {
                const auto *mask = reinterpret_cast<const struct sockaddr_in6 *>(ifa->ifa_netmask);
                entry += u'/';
                entry += QString::number(prefixLength(mask->sin6_addr.s6_addr, 16));
            }
            info.addresses.append(entry);
        }
    }
    ::freeifaddrs(list);
    return result;
}

} // namespace

QVariantMap InterfaceInfo::toVariant() const
{
    return {
        {u"name"_s, name},
        {u"kind"_s, kind},
        {u"state"_s, state},
        {u"up"_s, up},
        {u"mac"_s, mac},
        {u"mtu"_s, mtu},
        {u"speedMbps"_s, speedMbps > 0 ? QVariant(speedMbps) : QVariant::fromValue(nullptr)},
        {u"addresses"_s, addresses},
        {u"rxBytes"_s, qint64(rxBytes)},
        {u"txBytes"_s, qint64(txBytes)},
        {u"rxPackets"_s, qint64(rxPackets)},
        {u"txPackets"_s, qint64(txPackets)},
        {u"rxErrors"_s, qint64(rxErrors)},
        {u"txErrors"_s, qint64(txErrors)},
        {u"rxDropped"_s, qint64(rxDropped)},
        {u"txDropped"_s, qint64(txDropped)},
        {u"rxBytesPerSecond"_s, std::round(rxRate)},
        {u"txBytesPerSecond"_s, std::round(txRate)},
    };
}

QVariantMap NetworkSample::toVariant() const
{
    QVariantList list;
    for (const InterfaceInfo &iface : interfaces)
        list << iface.toVariant();
    return {
        {u"ratesValid"_s, ratesValid},
        {u"interfaces"_s, list},
        {u"rxBytesPerSecond"_s, std::round(rxRate)},
        {u"txBytesPerSecond"_s, std::round(txRate)},
    };
}

void NetworkSource::resetRates()
{
    m_previousTime = 0;
    m_previous.clear();
}

NetworkSample NetworkSource::sample()
{
    NetworkSample s;
    const double now = Sys::monotonicSeconds();
    const double elapsed = m_previousTime > 0 ? now - m_previousTime : 0;
    s.ratesValid = elapsed > 0;

    if (!Sys::readFile("/proc/net/dev", m_buffer))
        return s;
    const QHash<QString, AddressInfo> addresses = readAddresses();

    QHash<QString, Counters> current;
    QList<bool> hardware;
    const QList<QByteArray> lines = m_buffer.split('\n');
    // Two header lines, then "  name: rx-bytes packets errs drop fifo frame compressed multicast
    //                                  tx-bytes packets errs drop fifo colls carrier compressed"
    for (qsizetype n = 2; n < lines.size(); ++n) {
        const QByteArray &line = lines.at(n);
        const qsizetype colon = line.indexOf(':');
        if (colon < 0)
            continue;
        const QList<QByteArray> fields = line.mid(colon + 1).simplified().split(' ');
        if (fields.size() < 16)
            continue;

        InterfaceInfo iface;
        iface.name = QString::fromLocal8Bit(line.left(colon).trimmed());
        iface.rxBytes = fields.at(0).toULongLong();
        iface.rxPackets = fields.at(1).toULongLong();
        iface.rxErrors = fields.at(2).toULongLong();
        iface.rxDropped = fields.at(3).toULongLong();
        iface.txBytes = fields.at(8).toULongLong();
        iface.txPackets = fields.at(9).toULongLong();
        iface.txErrors = fields.at(10).toULongLong();
        iface.txDropped = fields.at(11).toULongLong();

        const QString base = u"/sys/class/net/"_s + iface.name;
        const qint64 type = Sys::readInteger(base + u"/type"_s).value_or(-1);
        const bool hasDevice = Sys::exists(base + u"/device"_s);
        const AddressInfo address = addresses.value(iface.name);
        if (type == 772 || (address.haveFlags && (address.flags & IFF_LOOPBACK)))
            iface.kind = u"loopback"_s;
        else if (Sys::exists(base + u"/wireless"_s) || Sys::exists(base + u"/phy80211"_s))
            iface.kind = u"wifi"_s;
        else if (type == 1)
            iface.kind = hasDevice ? u"ethernet"_s : u"virtual"_s;
        else
            iface.kind = hasDevice ? u"other"_s : u"virtual"_s;

        iface.mac = Sys::readText(base + u"/address"_s);
        if (iface.mac == "00:00:00:00:00:00"_L1)
            iface.mac.clear();
        iface.mtu = int(Sys::readInteger(base + u"/mtu"_s).value_or(0));
        // 'speed' fails with EINVAL while the link is down and reads -1 when unknown.
        const qint64 speed = Sys::readInteger(base + u"/speed"_s).value_or(-1);
        iface.speedMbps = speed > 0 && speed < 10000000 ? int(speed) : -1;
        iface.addresses = address.addresses;

        const bool running = address.haveFlags && (address.flags & IFF_UP) && (address.flags & IFF_RUNNING);
        const QString operstate = Sys::readText(base + u"/operstate"_s);
        iface.up = operstate == "up"_L1 || (operstate.isEmpty() || operstate == "unknown"_L1 ? running : false);
        iface.state = operstate.isEmpty() || operstate == "unknown"_L1 ? (running ? u"up"_s : u"down"_s) : operstate;

        Counters c;
        c.rxBytes = iface.rxBytes;
        c.txBytes = iface.txBytes;
        const auto before = m_previous.constFind(iface.name);
        if (before != m_previous.constEnd() && elapsed > 0) {
            // A counter that went backwards means the interface was re-created.
            if (c.rxBytes >= before->rxBytes)
                iface.rxRate = double(c.rxBytes - before->rxBytes) / elapsed;
            if (c.txBytes >= before->txBytes)
                iface.txRate = double(c.txBytes - before->txBytes) / elapsed;
        }
        current.insert(iface.name, c);
        s.interfaces << iface;
        hardware << (hasDevice && !iface.loopback());
    }
    m_previous = current;
    m_previousTime = now;

    // Totals: hardware interfaces only, because bridges, bonds, VLANs and
    // tunnels carry the same bytes a second time. Without any hardware
    // interface (containers) everything but the loopback counts.
    const bool anyHardware = hardware.contains(true);
    for (qsizetype i = 0; i < s.interfaces.size(); ++i) {
        const InterfaceInfo &iface = s.interfaces.at(i);
        if (iface.loopback() || (anyHardware && !hardware.at(i)))
            continue;
        s.rxRate += iface.rxRate;
        s.txRate += iface.txRate;
    }

    // Connected interfaces first, the loopback last.
    auto group = [](const InterfaceInfo &iface) { return iface.loopback() ? 2 : iface.up ? 0 : 1; };
    std::stable_sort(s.interfaces.begin(), s.interfaces.end(), [&group](const InterfaceInfo &a, const InterfaceInfo &b) {
        if (group(a) != group(b))
            return group(a) < group(b);
        return Sys::naturalLess(a.name, b.name);
    });
    return s;
}
