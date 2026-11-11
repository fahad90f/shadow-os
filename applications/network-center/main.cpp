// SPDX-License-Identifier: GPL-3.0-or-later
#include "capture.h"
#include "netstate.h"
#include "netutil.h"
#include "nmclient.h"
#include "sockets.h"

#include <shadowapp.h>

#include <QElapsedTimer>
#include <QEventLoop>
#include <QJsonArray>
#include <QJsonObject>
#include <QTimer>

#include <cstring>
#include <functional>
#include <vector>

namespace {

const char *kPages = "overview, wifi, interfaces, ip, dns, routes, connections, vpn, diagnostics, capture";

// "--page <key>" and "--page=<key>" are spellings of the positional page
// argument. They are folded into it before the command line is parsed, so
// that a second launch hands the page to the running instance the same way
// (Shadow::App forwards positional arguments).
std::vector<char *> foldPageOption(int argc, char **argv)
{
    std::vector<char *> out;
    for (int i = 0; i < argc; ++i) {
        if (i > 0 && std::strcmp(argv[i], "--page") == 0 && i + 1 < argc) {
            out.push_back(argv[++i]);
        } else if (i > 0 && std::strncmp(argv[i], "--page=", 7) == 0) {
            out.push_back(argv[i] + 7);
        } else {
            out.push_back(argv[i]);
        }
    }
    out.push_back(nullptr);
    return out;
}

// Spin the event loop until every asynchronous back-end has answered.
bool waitUntil(const std::function<bool()> &done, int timeoutMs = 30000)
{
    QEventLoop loop;
    QElapsedTimer clock;
    clock.start();
    QTimer poll;
    poll.setInterval(10);
    QObject::connect(&poll, &QTimer::timeout, &loop, [&] {
        if (done() || clock.elapsed() > timeoutMs)
            loop.quit();
    });
    poll.start();
    loop.exec();
    return done();
}

int runDump(Shadow::App &app)
{
    const QString topic = app.dumpTopic();
    const auto is = [&topic](const char *name) { return topic == QLatin1String(name); };
    const auto timeout = [&app] { return app.dumpError(QStringLiteral("timed out waiting for the system tools")); };

    if (is("interfaces") || is("addresses") || is("routes")) {
        NetState state;
        state.refresh();
        if (!app.waitFor(&state, SIGNAL(refreshed()), 30000))
            return timeout();
        if (is("interfaces"))
            return app.finishDump(state.dumpInterfaces());
        if (is("addresses"))
            return app.finishDump(state.dumpAddresses());
        return app.finishDump(state.dumpRoutes());
    }

    if (is("sockets")) {
        SocketList sockets;
        sockets.refresh();
        if (!app.waitFor(&sockets, SIGNAL(refreshed()), 30000))
            return timeout();
        return app.finishDump(sockets.dump());
    }

    if (is("capture")) {
        Capture capture;
        capture.refreshCapability();
        if (!app.waitFor(&capture, SIGNAL(capabilityReady()), 30000))
            return timeout();
        return app.finishDump(capture.dump());
    }

    if (is("connections") || is("vpn")) {
        Nm nm;
        nm.refresh();
        if (!app.waitFor(&nm, SIGNAL(refreshed()), 30000))
            return timeout();
        return app.finishDump(is("vpn") ? nm.dumpVpn() : nm.dumpConnections());
    }

    if (is("dns") || is("wifi") || is("overview")) {
        NetState state;
        Nm nm;
        SocketList sockets;
        bool stateDone = false, nmDone = false, socketsDone = !is("overview");
        QObject::connect(&state, &NetState::refreshed, &state, [&stateDone] { stateDone = true; });
        QObject::connect(&nm, &Nm::refreshed, &nm, [&nmDone] { nmDone = true; });
        QObject::connect(&sockets, &SocketList::refreshed, &sockets, [&socketsDone] { socketsDone = true; });
        state.refresh();
        nm.refresh();
        if (is("overview"))
            sockets.refresh();
        if (!waitUntil([&] { return stateDone && nmDone && socketsDone; }))
            return timeout();

        if (is("dns")) {
            // What each layer knows: the resolver configuration in effect and,
            // when NetworkManager runs, the servers it holds per device.
            QJsonObject out = state.dumpDns();
            QJsonArray devices;
            const QVariantList nmDevices = nm.devices();
            for (const QVariant &v : nmDevices) {
                const QVariantMap d = v.toMap();
                devices.append(QJsonObject{
                    {QStringLiteral("device"), d.value(QStringLiteral("name")).toString()},
                    {QStringLiteral("connection"), d.value(QStringLiteral("connection")).toString()},
                    {QStringLiteral("dns4"), QJsonArray::fromStringList(d.value(QStringLiteral("dns4")).toStringList())},
                    {QStringLiteral("dns6"), QJsonArray::fromStringList(d.value(QStringLiteral("dns6")).toStringList())},
                    {QStringLiteral("domains4"), QJsonArray::fromStringList(d.value(QStringLiteral("domains4")).toStringList())},
                    {QStringLiteral("domains6"), QJsonArray::fromStringList(d.value(QStringLiteral("domains6")).toStringList())},
                });
            }
            out.insert(QStringLiteral("networkManager"), QJsonObject{
                {QStringLiteral("status"), nm.status()},
                {QStringLiteral("statusText"), nm.statusText()},
                {QStringLiteral("devices"), devices},
            });
            return app.finishDump(out);
        }

        if (is("wifi")) {
            QJsonObject out = nm.dumpWifi();
            // The kernel's own view, which does not depend on NetworkManager.
            out.insert(QStringLiteral("kernelWirelessInterfaces"), QJsonArray::fromVariantList(state.wirelessInterfaces()));
            out.insert(QStringLiteral("rfkill"), QJsonArray::fromVariantList(state.rfkill()));
            QString availability;
            if (!nm.running())
                availability = nm.statusText();
            else if (nm.wifiDevices().isEmpty())
                availability = QStringLiteral("NetworkManager reports no Wi-Fi device (hardware: %1).").arg(nm.wifiHardware());
            out.insert(QStringLiteral("usable"), availability.isEmpty());
            out.insert(QStringLiteral("unavailableReason"), availability);
            return app.finishDump(out);
        }

        const NetUtil util;
        const QVariantMap connectivity = util.connectivitySummary(nm.running(), nm.state(), nm.connectivity(), state.linkState());
        QJsonObject out = state.dumpSummary();
        out.insert(QStringLiteral("connectivity"), QJsonObject::fromVariantMap(connectivity));
        out.insert(QStringLiteral("networkManager"), nm.dumpGeneral());
        out.insert(QStringLiteral("sockets"), QJsonObject{
            {QStringLiteral("error"), sockets.error()},
            {QStringLiteral("total"), sockets.total()},
            {QStringLiteral("listening"), sockets.listening()},
            {QStringLiteral("exposedListening"), sockets.exposed()},
            {QStringLiteral("establishedTcp"), sockets.established()},
        });
        out.insert(QStringLiteral("vpn"), QJsonObject{
            {QStringLiteral("available"), nm.running()},
            {QStringLiteral("configured"), int(nm.vpnConnections().size())},
            {QStringLiteral("active"), nm.activeVpnCount()},
        });
        return app.finishDump(out);
    }

    return app.dumpError(QStringLiteral("unknown dump topic '%1'").arg(topic));
}

} // namespace

int main(int argc, char **argv)
{
    std::vector<char *> arguments = foldPageOption(argc, argv);
    int count = int(arguments.size()) - 1;

    Shadow::AppInfo info;
    info.id = QStringLiteral("org.shadow.networkcenter");
    info.displayName = QStringLiteral("Network Center");
    info.iconName = QStringLiteral("org.shadow.networkcenter");
    info.description = QStringLiteral("SHADOW Network Center: network status, configuration and diagnostics.\n"
                                      "Pass a page key (or --page <key>) to open a page directly: %1.")
                           .arg(QLatin1String(kPages));
    info.singleInstance = true;

    Shadow::App app(count, arguments.data(), info);
    app.parser().addPositionalArgument(QStringLiteral("page"), QStringLiteral("Page to open: %1.").arg(QLatin1String(kPages)),
                                       QStringLiteral("[page]"));
    app.setDumpTopics({QStringLiteral("overview"), QStringLiteral("interfaces"), QStringLiteral("addresses"),
                       QStringLiteral("routes"), QStringLiteral("dns"), QStringLiteral("sockets"), QStringLiteral("wifi"),
                       QStringLiteral("connections"), QStringLiteral("vpn"), QStringLiteral("capture")});
    if (!app.processArguments())
        return 0;
    if (!app.dumpTopic().isEmpty())
        return runDump(app);
    return app.run(QStringLiteral("org.shadow.networkcenter"));
}
