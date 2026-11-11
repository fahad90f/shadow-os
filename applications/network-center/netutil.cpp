// SPDX-License-Identifier: GPL-3.0-or-later
#include "netutil.h"

#include <QClipboard>
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QGuiApplication>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QUrl>

#include <arpa/inet.h>
#include <cstring>
#include <netinet/in.h>
#include <unistd.h>

namespace Net {

namespace {

// Only printable ASCII without spaces may ever reach inet_pton() or a tool's
// argument vector as an address or name; this also rules out embedded NULs.
bool isPlainAscii(const QString &s)
{
    for (const QChar c : s) {
        const ushort u = c.unicode();
        if (u < 0x21 || u > 0x7e)
            return false;
    }
    return true;
}

bool isAsciiAlnum(QChar c)
{
    const ushort u = c.unicode();
    return (u >= '0' && u <= '9') || (u >= 'A' && u <= 'Z') || (u >= 'a' && u <= 'z');
}

bool isNameSyntax(const QString &s, bool allowUnderscore)
{
    QString h = s;
    if (h.endsWith(QLatin1Char('.')))
        h.chop(1);                  // a single trailing dot marks a fully qualified name
    if (h.isEmpty() || h.size() > 253 || !isPlainAscii(h))
        return false;
    const QStringList labels = h.split(QLatin1Char('.'));
    for (const QString &label : labels) {
        if (label.isEmpty() || label.size() > 63)
            return false;
        for (const QChar c : label) {
            if (!isAsciiAlnum(c) && c != QLatin1Char('-') && !(allowUnderscore && c == QLatin1Char('_')))
                return false;
        }
        if (label.startsWith(QLatin1Char('-')) || label.endsWith(QLatin1Char('-')))
            return false;
    }
    // Digits and dots only is a malformed IPv4 literal, never a host name.
    const QString &last = labels.constLast();
    bool allDigits = true;
    for (const QChar c : last)
        allDigits = allDigits && c.isDigit();
    return !allDigits;
}

bool toBytes(const QString &address, int family, unsigned char *out)
{
    const QByteArray a = address.toLatin1();
    return inet_pton(family == 6 ? AF_INET6 : AF_INET, a.constData(), out) == 1;
}

} // namespace

bool isIPv4(const QString &s)
{
    if (s.size() < 7 || s.size() > 15 || !isPlainAscii(s))
        return false;
    // inet_pton() accepts exactly four decimal octets and rejects leading
    // zeros, hexadecimal and the short "a.b" forms that inet_aton() allows.
    in_addr addr {};
    return inet_pton(AF_INET, s.toLatin1().constData(), &addr) == 1;
}

bool isIPv6(const QString &s, bool allowZone)
{
    if (s.size() < 2 || s.size() > 64 || !isPlainAscii(s))
        return false;
    QString address = s;
    const int percent = s.indexOf(QLatin1Char('%'));
    if (percent >= 0) {
        if (!allowZone || !isInterfaceName(s.mid(percent + 1)))
            return false;
        address = s.left(percent);
    }
    if (!address.contains(QLatin1Char(':')))
        return false;
    in6_addr addr {};
    return inet_pton(AF_INET6, address.toLatin1().constData(), &addr) == 1;
}

bool isIpLiteral(const QString &s, bool allowZone)
{
    return isIPv4(s) || isIPv6(s, allowZone);
}

bool isHostName(const QString &s)
{
    return isNameSyntax(s, false);
}

bool isDnsName(const QString &s)
{
    return isNameSyntax(s, true);
}

bool isInterfaceName(const QString &s)
{
    // IFNAMSIZ is 16 including the terminator. The kernel allows more
    // characters than this, but nothing sane uses them and names are passed
    // on to other tools.
    static const QRegularExpression rx(QStringLiteral("^[A-Za-z0-9_][A-Za-z0-9_.:-]{0,14}$"));
    return rx.match(s).hasMatch() && s != QLatin1String(".") && s != QLatin1String("..");
}

bool isUuid(const QString &s)
{
    static const QRegularExpression rx(
        QStringLiteral("^[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12}$"));
    return rx.match(s).hasMatch();
}

QString normalizeHost(const QString &input, QString *error, bool allowZone)
{
    auto fail = [error](const QString &message) {
        if (error)
            *error = message;
        return QString();
    };
    QString host = input.trimmed();
    if (host.isEmpty())
        return fail(QObject::tr("Enter a host name or an IP address."));
    if (host.size() > 255)
        return fail(QObject::tr("The name is too long."));
    // Accept a bracketed IPv6 literal as written in URLs.
    if (host.startsWith(QLatin1Char('[')) && host.endsWith(QLatin1Char(']')))
        host = host.mid(1, host.size() - 2);

    bool ascii = true;
    for (const QChar c : std::as_const(host))
        ascii = ascii && c.unicode() < 0x80;
    if (!ascii) {
        const QByteArray ace = QUrl::toAce(host);
        if (ace.isEmpty())
            return fail(QObject::tr("“%1” is not a valid internationalised host name.").arg(host));
        host = QString::fromLatin1(ace);
    }
    if (isIpLiteral(host, allowZone) || isHostName(host)) {
        if (error)
            error->clear();
        return host;
    }
    if (host.contains(QLatin1Char(':')))
        return fail(QObject::tr("“%1” is not a valid IPv6 address.").arg(host));
    static const QRegularExpression digitsAndDots(QStringLiteral("^[0-9.]+$"));
    if (digitsAndDots.match(host).hasMatch())
        return fail(QObject::tr("“%1” is not a valid IPv4 address (four numbers from 0 to 255).").arg(host));
    return fail(QObject::tr("“%1” is not a valid host name: use letters, digits, hyphens and dots only.").arg(host));
}

bool parseCidr(const QString &text, int family, QString *address, int *prefix, QString *error)
{
    const QString t = text.trimmed();
    const int slash = t.indexOf(QLatin1Char('/'));
    auto fail = [error](const QString &message) {
        if (error)
            *error = message;
        return false;
    };
    if (slash <= 0 || slash == t.size() - 1)
        return fail(QObject::tr("“%1” must be written as address/prefix, for example %2.")
                        .arg(t, family == 6 ? QStringLiteral("2001:db8::10/64") : QStringLiteral("192.168.1.10/24")));
    const QString addr = t.left(slash);
    const QString pfx = t.mid(slash + 1);
    if (family == 6 ? !isIPv6(addr) : !isIPv4(addr))
        return fail(QObject::tr("“%1” is not a valid IPv%2 address.").arg(addr).arg(family));
    static const QRegularExpression digits(QStringLiteral("^[0-9]{1,3}$"));
    const int max = family == 6 ? 128 : 32;
    bool ok = false;
    const int p = pfx.toInt(&ok);
    if (!digits.match(pfx).hasMatch() || !ok || p < 1 || p > max)
        return fail(QObject::tr("The prefix length of “%1” must be a number from 1 to %2.").arg(t).arg(max));
    const QString reason = unusableAddressReason(addr, family);
    if (!reason.isEmpty())
        return fail(reason);
    if (address)
        *address = canonicalAddress(addr, family);
    if (prefix)
        *prefix = p;
    return true;
}

QString canonicalAddress(const QString &address, int family)
{
    if (family == 6 ? !isIPv6(address) : !isIPv4(address))
        return {};
    unsigned char bytes[16] = {};
    char text[INET6_ADDRSTRLEN] = {};
    if (!toBytes(address, family, bytes)
        || !inet_ntop(family == 6 ? AF_INET6 : AF_INET, bytes, text, sizeof text))
        return {};
    return QString::fromLatin1(text);
}

QString unusableAddressReason(const QString &address, int family)
{
    unsigned char b[16] = {};
    if (!toBytes(address, family, b))
        return QObject::tr("“%1” is not a valid IPv%2 address.").arg(address).arg(family);
    if (family == 4) {
        if (b[0] == 0)
            return QObject::tr("%1 is in 0.0.0.0/8, which cannot be assigned or used as a next hop.").arg(address);
        if (b[0] >= 224 && b[0] <= 239)
            return QObject::tr("%1 is a multicast address.").arg(address);
        if (b[0] >= 240)
            return QObject::tr("%1 is in the reserved range 240.0.0.0/4.").arg(address);
        return {};
    }
    bool allZero = true;
    for (int i = 0; i < 16; ++i)
        allZero = allZero && b[i] == 0;
    if (allZero)
        return QObject::tr("%1 is the unspecified address.").arg(address);
    if (b[0] == 0xff)
        return QObject::tr("%1 is a multicast address.").arg(address);
    return {};
}

bool sameSubnet(const QString &a, const QString &b, int prefix, int family)
{
    unsigned char x[16] = {}, y[16] = {};
    if (!toBytes(a, family, x) || !toBytes(b, family, y))
        return false;
    const int bytes = family == 6 ? 16 : 4;
    for (int i = 0; i < bytes && prefix > 0; ++i, prefix -= 8) {
        const unsigned char mask = prefix >= 8 ? 0xff : static_cast<unsigned char>(0xff << (8 - prefix));
        if ((x[i] & mask) != (y[i] & mask))
            return false;
    }
    return true;
}

QString findTool(const QString &name)
{
    const QString inPath = QStandardPaths::findExecutable(name);
    if (!inPath.isEmpty())
        return inPath;
    static const QStringList extra = {
        QStringLiteral("/usr/sbin"), QStringLiteral("/sbin"), QStringLiteral("/usr/local/sbin"),
        QStringLiteral("/usr/bin"), QStringLiteral("/bin"),
    };
    return QStandardPaths::findExecutable(name, extra);
}

QString helperPath()
{
    const QString installed = QStringLiteral(NEXUS_NETWORK_HELPER);
    if (QFileInfo(installed).isFile())
        return installed;
    // Uninstalled build tree: <build>/bin/<app> next to <build>/libexec/nexus/.
    const QString local = QCoreApplication::applicationDirPath() + QStringLiteral("/../libexec/nexus/network-helper");
    const QFileInfo info(local);
    return info.isFile() ? info.canonicalFilePath() : QString();
}

bool elevatedCommand(const QString &program, const QStringList &arguments,
                     QString *command, QStringList *commandArguments, QString *problem)
{
    if (program.isEmpty()) {
        if (problem)
            *problem = QObject::tr("The Network Center helper is not installed.");
        return false;
    }
    if (::geteuid() == 0) {
        *command = program;
        *commandArguments = arguments;
        return true;
    }
    const QString pkexec = findTool(QStringLiteral("pkexec"));
    if (pkexec.isEmpty()) {
        if (problem)
            *problem = QObject::tr("pkexec is not installed (package pkexec), so administrator actions are not available.");
        return false;
    }
    *command = pkexec;
    *commandArguments = QStringList{program} + arguments;
    return true;
}

QString pkexecFailure(int exitCode, const QString &stderrText)
{
    // pkexec(1): 126 = not authorised or the dialog was dismissed, 127 = the
    // authorisation could not be obtained or another error occurred.
    if (::geteuid() == 0)
        return {};
    if (exitCode == 126)
        return QObject::tr("Authentication was cancelled or you are not authorised.");
    if (exitCode == 127) {
        const QString detail = cleanMessage(stderrText).section(QLatin1Char('\n'), 0, 0);
        return detail.isEmpty() ? QObject::tr("Administrator authentication failed.")
                                : QObject::tr("Administrator authentication is not available: %1").arg(detail);
    }
    return {};
}

QString cleanMessage(const QString &text)
{
    static const QRegularExpression csi(QStringLiteral("\\x1b\\[[0-9;?]*[ -/]*[@-~]"));
    QString t = text;
    t.remove(csi);
    t.remove(QLatin1Char('\r'));
    return t.trimmed();
}

} // namespace Net

QString NetUtil::hostError(const QString &text) const
{
    QString error;
    Net::normalizeHost(text, &error);
    return error;
}

QString NetUtil::dnsNameError(const QString &text) const
{
    const QString t = text.trimmed();
    if (t.isEmpty())
        return tr("Enter a name to look up.");
    if (Net::isIpLiteral(t) || Net::isDnsName(t))
        return {};
    QString error;
    const QString host = Net::normalizeHost(t, &error, false);
    return host.isEmpty() ? error : QString();
}

QVariantMap NetUtil::connectivitySummary(bool nmRunning, const QString &nmState, const QString &nmConnectivity,
                                         const QString &linkState) const
{
    QString label, detail, tone, source;
    if (nmRunning) {
        source = QStringLiteral("NetworkManager");
        if (nmConnectivity == QLatin1String("full")) {
            label = tr("Online");
            tone = QStringLiteral("success");
        } else if (nmConnectivity == QLatin1String("portal")) {
            label = tr("Sign-in required");
            tone = QStringLiteral("warning");
        } else if (nmConnectivity == QLatin1String("limited")) {
            label = tr("Limited");
            tone = QStringLiteral("warning");
        } else if (nmConnectivity == QLatin1String("none")) {
            label = tr("Offline");
            tone = QStringLiteral("danger");
        } else {
            label = nmState.isEmpty() ? tr("Unknown") : nmState.left(1).toUpper() + nmState.mid(1);
            tone = QStringLiteral("accent");
        }
        detail = tr("NetworkManager: %1").arg(nmState.isEmpty() ? tr("unknown") : nmState);
    } else {
        // Without NetworkManager nothing has verified reachability; say what
        // the kernel state implies and that it is only that.
        source = QStringLiteral("routing table");
        if (linkState == QLatin1String("connected")) {
            label = tr("Routed");
            tone = QStringLiteral("accent");
            detail = tr("Derived from default route");
        } else if (linkState == QLatin1String("local")) {
            label = tr("Local only");
            tone = QStringLiteral("warning");
            detail = tr("Derived: no default route");
        } else if (linkState == QLatin1String("no-address")) {
            label = tr("No address");
            tone = QStringLiteral("danger");
            detail = tr("Derived: link without address");
        } else if (linkState == QLatin1String("no-link")) {
            label = tr("No link");
            tone = QStringLiteral("danger");
            detail = tr("Derived: no interface has link");
        } else if (linkState == QLatin1String("no-adapter")) {
            label = tr("No adapter");
            tone = QStringLiteral("danger");
            detail = tr("Derived: no network interface");
        } else {
            label = tr("Unknown");
            tone = QStringLiteral("neutral");
            detail = tr("Kernel state not read yet");
        }
    }
    return {{QStringLiteral("label"), label}, {QStringLiteral("detail"), detail}, {QStringLiteral("tone"), tone},
            {QStringLiteral("source"), source}, {QStringLiteral("derived"), !nmRunning}};
}

QString NetUtil::wifiBand(int frequencyMhz) const
{
    if (frequencyMhz >= 2400 && frequencyMhz < 2500)
        return QStringLiteral("2.4 GHz");
    if (frequencyMhz >= 4900 && frequencyMhz < 5925)
        return QStringLiteral("5 GHz");
    if (frequencyMhz >= 5925 && frequencyMhz <= 7125)
        return QStringLiteral("6 GHz");
    if (frequencyMhz >= 57000 && frequencyMhz <= 71000)
        return QStringLiteral("60 GHz");
    return {};
}

void NetUtil::copyToClipboard(const QString &text) const
{
    QGuiApplication::clipboard()->setText(text);
}

QString NetUtil::homePath() const
{
    return QDir::homePath();
}

QString NetUtil::parentPath(const QString &path) const
{
    QDir dir(path);
    return dir.cdUp() ? dir.absolutePath() : QStringLiteral("/");
}

bool NetUtil::isDirectory(const QString &path) const
{
    return QFileInfo(path).isDir();
}

QVariantList NetUtil::listDirectory(const QString &path, const QStringList &nameFilters, bool allFiles) const
{
    QVariantList out;
    const QDir dir(path);
    if (!dir.exists())
        return out;
    QDir::Filters filters = QDir::AllEntries | QDir::NoDotAndDotDot;
    if (allFiles)
        filters |= QDir::Hidden;
    const QFileInfoList entries = dir.entryInfoList(filters, QDir::DirsFirst | QDir::Name | QDir::IgnoreCase);
    for (const QFileInfo &info : entries) {
        if (out.size() >= 2000)
            break;
        if (!info.isDir() && !allFiles && !QDir::match(nameFilters, info.fileName()))
            continue;
        out << QVariantMap{{QStringLiteral("name"), info.fileName()}, {QStringLiteral("path"), info.absoluteFilePath()},
                           {QStringLiteral("directory"), info.isDir()}, {QStringLiteral("size"), info.isDir() ? 0 : info.size()}};
    }
    return out;
}
