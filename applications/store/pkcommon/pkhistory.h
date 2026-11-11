// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QDBusObjectPath>
#include <QObject>
#include <QString>
#include <QTimer>
#include <QVariantList>

#include <functional>

namespace ShadowPk {

// PackageKit's own record of past transactions (GetOldTransactions): the
// installations, removals and updates made through it, newest first.
//
//   [ { tid, time, timeText, succeeded, role, roleText, durationMs, uid, user,
//       commandLine, packageCount, packages: [ { info, name, version, arch, repo } ] } ]
//
// 'done' receives the list, or an error text, exactly once (asynchronously).
//
// Note that PackageKit also records the *simulations* clients run before a
// real transaction; they cannot be told apart in this list. Use
// mergeHistory() to combine the list with APT's log, which records only
// what really happened.
//
// This one request talks to the daemon's D-Bus interface directly instead of
// going through PackageKit-Qt: in PackageKit-Qt 1.1.2 the slot that should
// receive the daemon's "Transaction" signal has a different signature than
// the signal, so Daemon::getOldTransactions() never delivers anything.
void fetchOldTransactions(QObject *context, uint count,
                          std::function<void(const QVariantList &transactions, const QString &error)> done);

// One dated history: APT's log entries (readAptHistory(): what really changed),
// each PackageKit-driven entry completed with who started it and through which
// program — taken from the matching PackageKit transaction — plus PackageKit
// transactions that failed before APT logged anything. Newest first.
//
// Added to APT entries:  startedBy ("alice (uid 1000)"), client ("shadow-updater"),
//                        kind: "apt"
// Extra entries:         kind: "packagekit-failed", start, startText, summary,
//                        startedBy, client, packageCount, packages
QVariantList mergeHistory(const QVariantList &aptEntries, const QVariantList &packageKitTransactions);

// Implementation detail of fetchOldTransactions(); public only for moc.
class OldTransactionsRequest : public QObject
{
    Q_OBJECT
public:
    using Callback = std::function<void(const QVariantList &, const QString &)>;
    OldTransactionsRequest(uint count, Callback done, QObject *parent);
    void start();

private Q_SLOTS:
    void onTransaction(const QDBusObjectPath &tid, const QString &timespec, bool succeeded, uint role,
                       uint duration, const QString &data, uint uid, const QString &commandLine);
    void onErrorCode(uint code, const QString &details);
    void onFinished(uint exit, uint runtime);

private:
    void finish(const QString &error);

    uint m_count;
    Callback m_done;
    QString m_path;
    QVariantList m_transactions;
    QString m_error;
    QTimer m_timeout;
    bool m_finished = false;
};

} // namespace ShadowPk
