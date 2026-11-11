// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QList>
#include <QObject>
#include <QQmlEngine>
#include <QString>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>

// Shared, side-effect free helpers of the Network Center back-ends:
// strict syntax validation of everything that is later handed to an external
// tool as an argument.
namespace Net {

// ---- validation (pure syntax; nothing is resolved or contacted) -------------
bool isIPv4(const QString &s);                              // dotted quad, no leading zeros
bool isIPv6(const QString &s, bool allowZone = false);      // RFC 4291 text form, optional %zone
bool isIpLiteral(const QString &s, bool allowZone = false);
bool isHostName(const QString &s);                          // RFC 1123 host name, ASCII (IDN in ACE form)
bool isDnsName(const QString &s);                           // like isHostName, labels may start with '_'
bool isInterfaceName(const QString &s);
bool isUuid(const QString &s);

// Trim, convert an internationalised name to its ASCII (ACE) form and check
// that the result is a host name or an IP literal. Returns the normalised
// target, or an empty string with *error set.
QString normalizeHost(const QString &input, QString *error, bool allowZone = true);

// "address/prefix" for the given family (4 or 6). On success *address and
// *prefix are set; on failure *error says what is wrong.
bool parseCidr(const QString &text, int family, QString *address, int *prefix, QString *error);
// Canonical text form of an address (lower case, "::" compression), or an
// empty string if it is not a valid address of that family.
QString canonicalAddress(const QString &address, int family);
// Reasons why a syntactically valid address cannot be a host/gateway/DNS address.
QString unusableAddressReason(const QString &address, int family);
bool sameSubnet(const QString &a, const QString &b, int prefix, int family);

// ---- external tools -----------------------------------------------------------
// Absolute path of a tool, searching PATH and then the sbin directories that
// are not in a regular user's PATH on Debian. Empty if not installed.
QString findTool(const QString &name);
// The privileged helper (network-helper): the installed copy, or the copy in
// the build tree when running uninstalled. Empty if neither exists.
QString helperPath();
// Run 'program' with administrator rights: directly when this process is
// already root, through pkexec otherwise. Returns false (with *problem set)
// if neither is possible; on success *command and *arguments are what to start.
bool elevatedCommand(const QString &program, const QStringList &arguments,
                     QString *command, QStringList *commandArguments, QString *problem);
// What a failed pkexec invocation means (exit status 126/127), or empty.
QString pkexecFailure(int exitCode, const QString &stderrText);

// Remove terminal control sequences and surrounding whitespace from tool output
// before it is shown to the user.
QString cleanMessage(const QString &text);

} // namespace Net

// QML access to the validators (forms validate while the user types).
class NetUtil : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

public:
    explicit NetUtil(QObject *parent = nullptr) : QObject(parent) {}

    // Empty string if 'text' is a valid host name or IP literal, else the reason.
    Q_INVOKABLE QString hostError(const QString &text) const;
    Q_INVOKABLE QString dnsNameError(const QString &text) const;
    Q_INVOKABLE bool isIPv4(const QString &text) const { return Net::isIPv4(text); }
    Q_INVOKABLE bool isIPv6(const QString &text) const { return Net::isIPv6(text); }
    Q_INVOKABLE bool isIpLiteral(const QString &text) const { return Net::isIpLiteral(text, true); }
    // One-line connectivity verdict for the overview: label, detail, tone
    // ("success" | "warning" | "danger" | "accent") and source. With a
    // running NetworkManager it is NetworkManager's own state and
    // connectivity check; otherwise it is derived from the kernel's link
    // state (NetState.linkState) and says so.
    Q_INVOKABLE QVariantMap connectivitySummary(bool nmRunning, const QString &nmState, const QString &nmConnectivity,
                                                const QString &linkState) const;
    // Radio band of a Wi-Fi centre frequency in MHz: "2.4 GHz", "5 GHz", "6 GHz" or "".
    Q_INVOKABLE QString wifiBand(int frequencyMhz) const;
    Q_INVOKABLE void copyToClipboard(const QString &text) const;
    // ---- for the file chooser (FilePickerDialog.qml) --------------------------
    Q_INVOKABLE QString homePath() const;
    Q_INVOKABLE QString parentPath(const QString &path) const;
    Q_INVOKABLE bool isDirectory(const QString &path) const;
    // Entries of a directory, folders first: [{name, path, directory, size}].
    // Files are limited to those matching 'nameFilters' ("*.conf") unless
    // 'allFiles' is set, which also shows hidden entries.
    Q_INVOKABLE QVariantList listDirectory(const QString &path, const QStringList &nameFilters, bool allFiles) const;
};
