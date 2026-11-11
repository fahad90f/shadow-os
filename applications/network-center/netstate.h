// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <command.h>

#include <QElapsedTimer>
#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QObject>
#include <QProcess>
#include <QQmlEngine>
#include <QTimer>
#include <QVariantList>
#include <QVariantMap>

// The kernel's view of the network, independent of any network manager.
//
// Sources (all machine-readable):
//   ip -j -s -details address show          interfaces, addresses, counters
//   ip -j -4|-6 -details route show table all   routing tables
//   ip -j neigh show                        ARP / NDP neighbours
//   /sys/class/net/<if>/…                   speed, duplex, driver, wireless, live counters
//   /etc/resolv.conf                        resolver configuration of the C library
//   org.freedesktop.resolve1 (system bus)   systemd-resolved, when it is running
//
// The object refreshes itself while 'active': counters once per second (rates
// are deltas between two readings), the structure whenever `ip monitor`
// reports a change and on a slow timer as a safety net.
class NetState : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON
    Q_PROPERTY(bool active READ active WRITE setActive NOTIFY activeChanged)
    Q_PROPERTY(bool ready READ ready NOTIFY readyChanged)
    Q_PROPERTY(QString error READ error NOTIFY errorChanged)
    Q_PROPERTY(QVariantList interfaces READ interfaces NOTIFY interfacesChanged)
    Q_PROPERTY(QVariantList routes READ routes NOTIFY routesChanged)
    Q_PROPERTY(QVariantList neighbours READ neighbours NOTIFY neighboursChanged)
    Q_PROPERTY(QVariantMap defaultRoute4 READ defaultRoute4 NOTIFY routesChanged)
    Q_PROPERTY(QVariantMap defaultRoute6 READ defaultRoute6 NOTIFY routesChanged)
    Q_PROPERTY(QString primaryInterface READ primaryInterface NOTIFY summaryChanged)
    Q_PROPERTY(QString primaryAddress4 READ primaryAddress4 NOTIFY summaryChanged)
    Q_PROPERTY(QString primaryAddress6 READ primaryAddress6 NOTIFY summaryChanged)
    Q_PROPERTY(int interfacesUp READ interfacesUp NOTIFY summaryChanged)
    Q_PROPERTY(int interfacesTotal READ interfacesTotal NOTIFY summaryChanged)
    Q_PROPERTY(QString linkState READ linkState NOTIFY summaryChanged)
    Q_PROPERTY(QVariantMap dns READ dns NOTIFY dnsChanged)
    Q_PROPERTY(QVariantList wirelessInterfaces READ wirelessInterfaces NOTIFY interfacesChanged)
    Q_PROPERTY(QVariantList rfkill READ rfkill NOTIFY rfkillChanged)
    // Live figures per interface name, refreshed once per second: rxRate/txRate
    // (bytes per second), rxBytes/txBytes/rxPackets/txPackets/rxErrors/txErrors/
    // rxDropped/txDropped and rxHistory/txHistory (rates, oldest first).
    Q_PROPERTY(QVariantMap liveStats READ liveStats NOTIFY statsTickChanged)
    Q_PROPERTY(int statsTick READ statsTick NOTIFY statsTickChanged)
    Q_PROPERTY(int historyCapacity READ historyCapacity CONSTANT)

public:
    explicit NetState(QObject *parent = nullptr);
    ~NetState() override;

    bool active() const { return m_active; }
    void setActive(bool active);
    bool ready() const { return m_ready; }
    QString error() const { return m_error; }
    QVariantList interfaces() const { return m_interfaces; }
    QVariantList routes() const { return m_routes; }
    QVariantList neighbours() const { return m_neighbours; }
    QVariantMap defaultRoute4() const { return m_default4; }
    QVariantMap defaultRoute6() const { return m_default6; }
    QString primaryInterface() const { return m_primaryInterface; }
    QString primaryAddress4() const { return m_primary4; }
    QString primaryAddress6() const { return m_primary6; }
    int interfacesUp() const { return m_up; }
    int interfacesTotal() const { return m_total; }
    // "connected" (default route), "local" (link and address, no default
    // route), "no-address", "no-link" or "no-adapter" — derived from the
    // kernel state only; says nothing about reachability.
    QString linkState() const { return m_linkState; }
    QVariantMap dns() const { return m_dns; }
    QVariantList wirelessInterfaces() const;
    QVariantList rfkill() const { return m_rfkill; }
    QVariantMap liveStats() const { return m_liveStats; }
    int statsTick() const { return m_statsTick; }
    int historyCapacity() const { return HistoryCapacity; }

    // Start an asynchronous refresh of everything; refreshed() follows.
    Q_INVOKABLE void refresh();
    QVariantMap interfaceByName(const QString &name) const;

    // ---- diagnostic dumps (--dump) ------------------------------------------
    QJsonArray dumpInterfaces() const;      // including counters as read by `ip -s`
    QJsonArray dumpAddresses() const;
    QJsonArray dumpRoutes() const;
    QJsonObject dumpDns() const;
    QJsonObject dumpSummary() const;

Q_SIGNALS:
    void activeChanged();
    void readyChanged();
    void errorChanged();
    void interfacesChanged();
    void routesChanged();
    void neighboursChanged();
    void summaryChanged();
    void dnsChanged();
    void rfkillChanged();
    void statsTickChanged();
    void refreshed();

private:
    enum { HistoryCapacity = 60 };

    struct Counters
    {
        qint64 rxBytes = 0, txBytes = 0, rxPackets = 0, txPackets = 0;
        qint64 rxErrors = 0, txErrors = 0, rxDropped = 0, txDropped = 0;
    };
    struct Live
    {
        bool sampled = false;
        Counters last;
        double rxRate = 0, txRate = 0;
        QVariantList rxHistory, txHistory;
        QVariantMap cached;
    };

    void stepDone();
    void finishRefresh();
    void parseInterfaces(const QByteArray &json);
    void parseRoutes(const QByteArray &json4, const QByteArray &json6);
    void parseNeighbours(const QByteArray &json);
    void computeSummary();
    void readRfkill();
    void refreshDns();
    void queryResolved();
    void queryResolvedLinks(QVariantMap resolved);
    void finishDns(const QVariantMap &resolved);
    void sampleCounters();
    void startMonitor();
    void stopMonitor();
    static bool readCounters(const QString &interface, Counters *out);

    bool m_active = false;
    bool m_ready = false;
    bool m_refreshing = false;
    bool m_refreshQueued = false;
    int m_pending = 0;
    QString m_error;

    Nexus::CommandResult m_addrResult, m_route4Result, m_route6Result, m_neighResult;

    QVariantList m_interfaces;          // without counters (they change every second)
    QVariantList m_interfacesFull;      // with the counters of the last `ip -s` run and address lifetimes
    QVariantList m_routes;
    QVariantList m_neighbours;
    QVariantMap m_default4, m_default6;
    QString m_primaryInterface, m_primary4, m_primary6, m_linkState;
    int m_up = 0, m_total = 0;
    QVariantMap m_dns;
    QVariantList m_rfkill;

    QHash<QString, Live> m_live;
    QVariantMap m_liveStats;
    QElapsedTimer m_sampleClock;
    int m_statsTick = 0;

    QTimer m_sampleTimer;       // counters → rates
    QTimer m_slowTimer;         // structural safety net
    QTimer m_debounce;          // coalesces `ip monitor` events
    QProcess *m_monitor = nullptr;
};
