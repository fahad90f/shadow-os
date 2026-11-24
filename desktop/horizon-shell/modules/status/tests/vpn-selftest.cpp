// SPDX-License-Identifier: GPL-3.0-or-later
//
// Self test of the VPN part of the status module's network back-end
// (statusnetwork.h) against status-test-dbus-fixture on a PRIVATE bus: the
// saved profiles (only VPN and WireGuard ones, the most recently used first),
// connecting the most recent and a named one, what the back-end shows while
// one is up, disconnecting, and what it says when NetworkManager refuses.
// Started by vpn-selftest.sh, which provides the bus and the fixture; it
// refuses to run without DBUS_SYSTEM_BUS_ADDRESS (never a real system bus).
// Exit code 0 if every check holds.
#include "statusnetwork.h"

#include <QCoreApplication>
#include <QDeadlineTimer>
#include <QTextStream>

#include <functional>

using namespace StatusModule;

static int failures = 0;
static int checks = 0;

static void check(bool ok, const QString &what, const QString &detail = {})
{
    ++checks;
    if (ok)
        return;
    ++failures;
    QTextStream(stderr) << "FAIL: " << what << (detail.isEmpty() ? QString() : QStringLiteral(" — ") + detail) << '\n';
}

static bool waitFor(const std::function<bool()> &condition, int ms = 5000)
{
    QDeadlineTimer deadline(ms);
    while (!condition() && !deadline.hasExpired())
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
    return condition();
}

static QStringList names(const QVariantList &profiles)
{
    QStringList list;
    for (const QVariant &profile : profiles)
        list.append(profile.toMap().value(QStringLiteral("name")).toString());
    return list;
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    if (qEnvironmentVariableIsEmpty("DBUS_SYSTEM_BUS_ADDRESS")) {
        QTextStream(stderr) << "status-test-vpn: DBUS_SYSTEM_BUS_ADDRESS is not set; this test only runs on a private bus\n";
        return 2;
    }
    NetworkBackend network;
    QStringList failed;
    QObject::connect(&network, &Backend::failed, &app, [&failed](const QString &message) { failed.append(message); });

    check(waitFor([&] { return network.isAvailable() && network.vpnProfiles().size() == 3; }),
          QStringLiteral("the saved VPN profiles are read"), names(network.vpnProfiles()).join(QLatin1Char(',')));
    // The Wi-Fi connection is not a profile; the most recently used comes first.
    check(names(network.vpnProfiles()) == QStringList{QStringLiteral("Travel VPN"), QStringLiteral("Office tunnel"),
                                                      QStringLiteral("Needs a password")},
          QStringLiteral("profiles in order of last use"), names(network.vpnProfiles()).join(QLatin1Char(',')));
    check(network.vpnProfiles().constFirst().toMap().value(QStringLiteral("kind")).toString() == QLatin1String("vpn"),
          QStringLiteral("a profile's kind"));
    check(!network.vpnActive() && !network.vpnConnecting(), QStringLiteral("no VPN at first"));

    // The tile's switch: the most recently used profile.
    network.connectVpn();
    check(waitFor([&] { return network.vpnActive(); }), QStringLiteral("connecting brings a VPN up"));
    check(network.vpnName() == QLatin1String("Travel VPN"), QStringLiteral("the most recently used profile was connected"),
          network.vpnName());
    check(failed.isEmpty(), QStringLiteral("no error while connecting"), failed.join(QLatin1Char('|')));

    network.disconnectVpn();
    check(waitFor([&] { return !network.vpnActive(); }), QStringLiteral("disconnecting takes it down"));
    check(network.vpnName().isEmpty(), QStringLiteral("no VPN name once it is down"), network.vpnName());

    // A named profile (WireGuard: an active connection of type "wireguard", not flagged Vpn).
    network.connectVpn(QStringLiteral("00000000-0000-4000-8000-000000000005"));
    check(waitFor([&] { return network.vpnActive(); }), QStringLiteral("a WireGuard profile counts as a VPN"));
    check(network.vpnName() == QLatin1String("Office tunnel"), QStringLiteral("the named profile was connected"),
          network.vpnName());
    network.disconnectVpn();
    check(waitFor([&] { return !network.vpnActive(); }), QStringLiteral("and is taken down again"));

    // NetworkManager refuses (a password that is not saved): said as it is, nothing comes up.
    network.connectVpn(QStringLiteral("00000000-0000-4000-8000-000000000007"));
    check(waitFor([&] { return !failed.isEmpty(); }), QStringLiteral("a refusal is reported"));
    check(failed.value(0).contains(QLatin1String("Needs a password"))
              && failed.value(0).contains(QLatin1String("No agents were available")),
          QStringLiteral("the report names the profile and the reason"), failed.value(0));
    check(!network.vpnActive(), QStringLiteral("nothing is up after a refusal"));

    failed.clear();
    network.connectVpn(QStringLiteral("no-such-uuid"));
    check(waitFor([&] { return !failed.isEmpty(); }, 2000) && failed.value(0).contains(QLatin1String("no longer exists")),
          QStringLiteral("a profile that is gone is said to be gone"), failed.value(0));

    QTextStream(stdout) << checks << " checks, " << failures << " failed\n";
    return failures == 0 ? 0 : 1;
}
