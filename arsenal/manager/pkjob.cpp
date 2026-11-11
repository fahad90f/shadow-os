// SPDX-License-Identifier: GPL-3.0-or-later
#include "pkjob.h"

#include <Daemon>

#include <QDBusConnection>
#include <QDBusError>
#include <QDBusMessage>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDBusServiceWatcher>
#include <QMetaEnum>
#include <QRegularExpression>
#include <QTimer>

namespace Arsenal {

namespace {
const QString kService = QStringLiteral("org.freedesktop.PackageKit");
const QString kPath = QStringLiteral("/org/freedesktop/PackageKit");

QVariantMap packageEntry(const QString &packageId)
{
    return {{QStringLiteral("name"), PackageKit::Daemon::packageName(packageId)},
            {QStringLiteral("version"), PackageKit::Daemon::packageVersion(packageId)},
            {QStringLiteral("arch"), PackageKit::Daemon::packageArch(packageId)}};
}
} // namespace

// ---- PkService -----------------------------------------------------------------

PkService::PkService(QObject *parent) : QObject(parent) {}

void PkService::fail(const QString &name, const QString &message)
{
    m_available = false;
    m_checking = false;
    m_error = name.isEmpty() ? message : QStringLiteral("%1 (%2)").arg(message, name);
    Q_EMIT changed();
    Q_EMIT probed();
}

void PkService::probe()
{
    const quint64 serial = ++m_serial;
    m_checking = true;
    Q_EMIT changed();

    QDBusConnection bus = QDBusConnection::systemBus();
    if (!bus.isConnected()) {
        const QString detail = bus.lastError().isValid() ? bus.lastError().message()
                                                         : tr("no connection to the system message bus");
        QTimer::singleShot(0, this, [this, serial, detail] {
            if (serial == m_serial)
                fail(QString(), tr("The system D-Bus is not available: %1").arg(detail));
        });
        return;
    }
    if (!m_watching) {
        m_watching = true;
        auto *watcher = new QDBusServiceWatcher(kService, bus, QDBusServiceWatcher::WatchForRegistration, this);
        connect(watcher, &QDBusServiceWatcher::serviceRegistered, this, [this] {
            if (!m_available && !m_checking)
                probe();
        });
    }
    // Reading the daemon's properties answers "is it there?" — the bus starts
    // the daemon on demand — and names the back-end in use.
    QDBusMessage call = QDBusMessage::createMethodCall(kService, kPath,
                                                       QStringLiteral("org.freedesktop.DBus.Properties"),
                                                       QStringLiteral("GetAll"));
    call << kService;
    auto *watcher = new QDBusPendingCallWatcher(bus.asyncCall(call, 30000), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this, serial](QDBusPendingCallWatcher *w) {
        w->deleteLater();
        if (serial != m_serial)
            return;
        const QDBusPendingReply<QVariantMap> reply = *w;
        if (reply.isError()) {
            fail(reply.error().name(), reply.error().message());
            return;
        }
        m_backend = reply.value().value(QStringLiteral("BackendName")).toString();
        m_available = true;
        m_checking = false;
        m_error.clear();
        Q_EMIT changed();
        Q_EMIT probed();
    });
}

// ---- PkJob ---------------------------------------------------------------------

PkJob::PkJob(QObject *parent) : QObject(parent) {}

QString PkJob::errorKeyOf(T::Error error)
{
    const QMetaEnum meta = QMetaEnum::fromType<T::Error>();
    const char *key = meta.valueToKey(error);
    return key ? QString::fromLatin1(key) : QStringLiteral("ErrorUnknown");
}

bool PkJob::cancellable() const
{
    if (m_phase == QLatin1String("confirm") || m_phase == QLatin1String("resolving")
        || m_phase == QLatin1String("simulating"))
        return !m_cancelRequested;
    return m_phase == QLatin1String("running") && m_allowCancel && !m_cancelRequested;
}

QString PkJob::statusText() const
{
    if (m_cancelRequested && active())
        return tr("Cancelling…");
    if (m_phase == QLatin1String("resolving"))
        return tr("Looking up the packages…");
    if (m_phase == QLatin1String("simulating"))
        return tr("Working out what will change…");
    if (m_phase != QLatin1String("running"))
        return QString();
    switch (m_status) {
    case T::StatusWait: return tr("Waiting for the package manager…");
    case T::StatusWaitingForLock: return tr("Waiting for another package manager to finish…");
    case T::StatusWaitingForAuth: return tr("Waiting for authentication…");
    case T::StatusSetup: return tr("Starting…");
    case T::StatusQuery:
    case T::StatusInfo: return tr("Querying…");
    case T::StatusLoadingCache: return tr("Loading the package lists…");
    case T::StatusRefreshCache:
    case T::StatusDownloadRepository:
    case T::StatusDownloadPackagelist: return tr("Refreshing the package lists…");
    case T::StatusDepResolve: return tr("Resolving dependencies…");
    case T::StatusDownload: return tr("Downloading packages…");
    case T::StatusSigCheck: return tr("Checking signatures…");
    case T::StatusTestCommit: return tr("Testing the changes…");
    case T::StatusInstall: return tr("Installing packages…");
    case T::StatusUpdate: return tr("Updating packages…");
    case T::StatusRemove: return tr("Removing packages…");
    case T::StatusCleanup:
    case T::StatusObsolete: return tr("Cleaning up…");
    case T::StatusCommit: return tr("Applying the changes…");
    case T::StatusRunHook: return tr("Running package scripts…");
    case T::StatusCancel: return tr("Cancelling…");
    case T::StatusFinished: return tr("Finishing…");
    default: return tr("Working…");
    }
}

void PkJob::setPhase(const QString &phase)
{
    m_phase = phase;
    Q_EMIT phaseChanged();
    Q_EMIT progressChanged();
}

void PkJob::begin(const QString &kind, const QStringList &names, const QString &title, const QString &context)
{
    m_kind = kind;
    m_names = names;
    m_title = title;
    m_context = context;
    m_cancelRequested = false;
    m_ids.clear();
    m_installedNames.clear();
    m_availableIds.clear();
    m_unavailable.clear();
    m_install.clear();
    m_removeList.clear();
    m_update.clear();
    m_untrusted.clear();
    m_simulatedDownload = 0;
    m_changes.clear();
    m_status = T::StatusUnknown;
    m_percentage = -1;
    m_allowCancel = false;
    m_downloadRemaining = 0;
    m_currentItem.clear();
    m_hasError = false;
    m_errorText.clear();
    m_result.clear();
    m_errorKey.clear();
    m_errorTitle.clear();
    m_errorDetails.clear();
    m_note.clear();
    resolve();
}

bool PkJob::install(const QStringList &names, const QString &title, const QString &context)
{
    if (active() || names.isEmpty())
        return false;
    m_autoRemove = false;
    begin(QStringLiteral("install"), names, title, context);
    return true;
}

bool PkJob::remove(const QStringList &names, bool autoRemove, const QString &title, const QString &context)
{
    if (active() || names.isEmpty())
        return false;
    m_autoRemove = autoRemove;
    begin(QStringLiteral("remove"), names, title, context);
    return true;
}

void PkJob::watch(T *transaction)
{
    m_transaction = transaction;
    m_hasError = false;
    connect(transaction, &T::errorCode, this, [this](T::Error error, const QString &details) {
        m_hasError = true;
        m_error = error;
        m_errorText = details;
    });
    // PackageKit-Qt deletes a transaction when the daemon drops it; without a
    // 'finished' that means the daemon went away.
    connect(transaction, &QObject::destroyed, this, [this, transaction] {
        if (active() && m_phase != QLatin1String("confirm") && m_transaction.isNull()
            && sender() == transaction) {
            fail(QStringLiteral("daemon-lost"), tr("PackageKit stopped answering"),
                 tr("The package service ended before the operation was complete. "
                    "Check the package state with “sudo dpkg --audit”."));
        }
    });
}

// ---- 1. names → package ids -------------------------------------------------------

void PkJob::resolve()
{
    setPhase(QStringLiteral("resolving"));
    T *t = PackageKit::Daemon::resolve(m_names, T::FilterArch | T::FilterNewest);
    watch(t);
    connect(t, &T::package, this, [this](T::Info info, const QString &packageId, const QString &) {
        const QString name = PackageKit::Daemon::packageName(packageId);
        if (info == T::InfoInstalled) {
            m_installedNames.insert(name, true);
            if (m_kind == QLatin1String("remove"))
                m_availableIds.insert(name, packageId);
        } else if (m_kind == QLatin1String("install") && !m_availableIds.contains(name)) {
            m_availableIds.insert(name, packageId);
        }
    });
    connect(t, &T::finished, this, [this, t](T::Exit exit, uint) {
        disconnect(t, &QObject::destroyed, this, nullptr);
        resolved(exit);
    });
}

void PkJob::resolved(T::Exit exit)
{
    if (m_cancelRequested || exit == T::ExitCancelled) {
        finish(QStringLiteral("cancelled"));
        return;
    }
    if (exit != T::ExitSuccess) {
        failFromTransaction(exit, tr("The packages could not be looked up"));
        return;
    }
    if (m_kind == QLatin1String("install")) {
        for (const QString &name : std::as_const(m_names)) {
            if (m_installedNames.value(name))
                continue;
            if (m_availableIds.contains(name))
                m_ids << m_availableIds.value(name);
            else
                m_unavailable << name;
        }
        if (m_ids.isEmpty()) {
            if (m_unavailable.isEmpty()) {
                m_note = tr("Everything is already installed.");
                finish(QStringLiteral("success"));
            } else {
                fail(QStringLiteral("unavailable"), tr("Not available from the configured repositories"),
                     tr("No repository that is enabled on this system offers: %1.\n"
                        "Refresh the package lists (NEXUS Updater, or “sudo apt update”) and check that the "
                        "repository is enabled.").arg(m_unavailable.join(QStringLiteral(", "))));
            }
            return;
        }
    } else {
        for (const QString &name : std::as_const(m_names)) {
            if (m_installedNames.value(name) && m_availableIds.contains(name))
                m_ids << m_availableIds.value(name);
        }
        if (m_ids.isEmpty()) {
            m_note = tr("Nothing to remove: none of the packages is installed.");
            finish(QStringLiteral("success"));
            return;
        }
    }
    simulate();
}

// ---- 2. what would change ----------------------------------------------------------

void PkJob::simulate()
{
    setPhase(QStringLiteral("simulating"));
    T *t = m_kind == QLatin1String("install")
               ? PackageKit::Daemon::installPackages(m_ids, T::TransactionFlagSimulate)
               : PackageKit::Daemon::removePackages(m_ids, true, m_autoRemove, T::TransactionFlagSimulate);
    watch(t);
    connect(t, &T::package, this, [this](T::Info info, const QString &packageId, const QString &) {
        switch (info) {
        case T::InfoInstalling:
        case T::InfoReinstalling:
            m_install << packageEntry(packageId);
            break;
        case T::InfoRemoving:
        case T::InfoObsoleting:
            m_removeList << packageEntry(packageId);
            break;
        case T::InfoUpdating:
        case T::InfoDowngrading:
            m_update << packageEntry(packageId);
            break;
        case T::InfoUntrusted:
            m_untrusted << PackageKit::Daemon::packageName(packageId);
            m_install << packageEntry(packageId);
            break;
        default:
            break;
        }
    });
    connect(t, &T::downloadSizeRemainingChanged, this, [this, t] {
        m_simulatedDownload = qMax(m_simulatedDownload, t->downloadSizeRemaining());
    });
    connect(t, &T::finished, this, [this, t](T::Exit exit, uint) {
        m_simulatedDownload = qMax(m_simulatedDownload, t->downloadSizeRemaining());
        disconnect(t, &QObject::destroyed, this, nullptr);
        simulated(exit);
    });
}

void PkJob::simulated(T::Exit exit)
{
    if (m_cancelRequested || exit == T::ExitCancelled) {
        finish(QStringLiteral("cancelled"));
        return;
    }
    if (exit == T::ExitNeedUntrusted || !m_untrusted.isEmpty()) {
        fail(QStringLiteral("untrusted"), tr("Packages from an unsigned source"),
             tr("These packages do not come from a signed repository and were not installed: %1.")
                 .arg(m_untrusted.isEmpty() ? tr("(not named by PackageKit)")
                                            : m_untrusted.join(QStringLiteral(", "))));
        return;
    }
    if (exit != T::ExitSuccess) {
        failFromTransaction(exit, m_kind == QLatin1String("install")
                                      ? tr("The installation cannot be carried out")
                                      : tr("The removal cannot be carried out"));
        return;
    }

    // Never take the desktop down with a tool.
    static const QRegularExpression protectedName(QStringLiteral("^nexus-(?!arsenal-)"));
    QStringList blocked;
    for (const QVariant &entry : std::as_const(m_removeList)) {
        const QString name = entry.toMap().value(QStringLiteral("name")).toString();
        if (protectedName.match(name).hasMatch())
            blocked << name;
    }
    if (!blocked.isEmpty()) {
        fail(QStringLiteral("protected"), tr("This would remove parts of NEXUS OS"),
             tr("The operation would also remove %1, so it was not started. Nothing was changed.")
                 .arg(blocked.join(QStringLiteral(", "))));
        return;
    }

    const auto byName = [](const QVariant &a, const QVariant &b) {
        return a.toMap().value(QStringLiteral("name")).toString() < b.toMap().value(QStringLiteral("name")).toString();
    };
    std::sort(m_install.begin(), m_install.end(), byName);
    std::sort(m_removeList.begin(), m_removeList.end(), byName);
    std::sort(m_update.begin(), m_update.end(), byName);
    m_changes = {{QStringLiteral("install"), m_install},
                 {QStringLiteral("remove"), m_removeList},
                 {QStringLiteral("update"), m_update},
                 {QStringLiteral("downloadBytes"), double(m_simulatedDownload)}};
    setPhase(QStringLiteral("confirm"));
}

void PkJob::confirm()
{
    if (m_phase == QLatin1String("confirm"))
        run();
}

// ---- 3. the real transaction -------------------------------------------------------

void PkJob::run()
{
    m_percentage = -1;
    m_status = T::StatusSetup;
    setPhase(QStringLiteral("running"));
    T *t = m_kind == QLatin1String("install")
               ? PackageKit::Daemon::installPackages(m_ids, T::TransactionFlagOnlyTrusted)
               : PackageKit::Daemon::removePackages(m_ids, true, m_autoRemove, T::TransactionFlagNone);
    watch(t);
    const auto update = [this, t] {
        m_status = t->status();
        const uint percentage = t->percentage();
        m_percentage = percentage <= 100 ? int(percentage) : -1;
        m_allowCancel = t->allowCancel();
        m_downloadRemaining = t->downloadSizeRemaining();
        Q_EMIT progressChanged();
    };
    connect(t, &T::statusChanged, this, update);
    connect(t, &T::percentageChanged, this, update);
    connect(t, &T::allowCancelChanged, this, update);
    connect(t, &T::downloadSizeRemainingChanged, this, update);
    connect(t, &T::package, this, [this](T::Info info, const QString &packageId, const QString &) {
        const QString name = PackageKit::Daemon::packageName(packageId);
        switch (info) {
        case T::InfoDownloading: m_currentItem = tr("Downloading %1").arg(name); break;
        case T::InfoInstalling: m_currentItem = tr("Installing %1").arg(name); break;
        case T::InfoUpdating: m_currentItem = tr("Updating %1").arg(name); break;
        case T::InfoRemoving: m_currentItem = tr("Removing %1").arg(name); break;
        case T::InfoPreparing: m_currentItem = tr("Preparing %1").arg(name); break;
        case T::InfoDecompressing: m_currentItem = tr("Unpacking %1").arg(name); break;
        case T::InfoCleanup: m_currentItem = tr("Cleaning up %1").arg(name); break;
        default: return;
        }
        Q_EMIT progressChanged();
    });
    connect(t, &T::finished, this, [this, t](T::Exit exit, uint) {
        disconnect(t, &QObject::destroyed, this, nullptr);
        ran(exit);
    });
}

void PkJob::ran(T::Exit exit)
{
    switch (exit) {
    case T::ExitSuccess:
        finish(QStringLiteral("success"));
        return;
    case T::ExitCancelled:
    case T::ExitCancelledPriority:
        finish(QStringLiteral("cancelled"));
        return;
    case T::ExitNeedUntrusted:
        fail(QStringLiteral("untrusted"), tr("Packages from an unsigned source"),
             tr("PackageKit refused because a package is not from a signed repository. Nothing was installed."));
        return;
    case T::ExitEulaRequired:
        fail(QStringLiteral("eula"), tr("A licence agreement must be accepted"),
             tr("One of the packages requires accepting a licence agreement, which NEXUS Arsenal does not "
                "do for you. Install it with NEXUS Store or apt to read and accept the agreement."));
        return;
    case T::ExitKeyRequired:
        fail(QStringLiteral("key"), tr("A repository signing key is not trusted"),
             tr("A package is signed with a key this system does not trust yet. NEXUS Arsenal does not "
                "import keys; review the repository in NEXUS Store."));
        return;
    case T::ExitMediaChangeRequired:
        fail(QStringLiteral("media"), tr("Installation media required"),
             tr("A package would have to be read from removable installation media."));
        return;
    case T::ExitRepairRequired:
        fail(QStringLiteral("repair"), tr("The package system needs repair"),
             tr("An earlier package operation was interrupted. Run “sudo dpkg --configure -a” and "
                "“sudo apt --fix-broken install”, then try again."));
        return;
    default:
        failFromTransaction(exit, m_kind == QLatin1String("install") ? tr("The installation failed")
                                                                     : tr("The removal failed"));
    }
}

void PkJob::failFromTransaction(T::Exit exit, const QString &fallbackTitle)
{
    Q_UNUSED(exit)
    if (!m_hasError) {
        fail(QStringLiteral("failed"), fallbackTitle, tr("PackageKit gave no further details."));
        return;
    }
    const QString key = errorKeyOf(m_error);
    switch (m_error) {
    case T::ErrorNotAuthorized:
        // The user closed or failed the authentication prompt: nothing happened.
        m_note = tr("Authentication was not given; nothing was changed.");
        m_errorKey = key;
        finish(QStringLiteral("cancelled"));
        return;
    case T::ErrorTransactionCancelled:
        finish(QStringLiteral("cancelled"));
        return;
    case T::ErrorNoNetwork:
        fail(key, tr("No network connection"), m_errorText);
        return;
    case T::ErrorNoSpaceOnDevice:
        fail(key, tr("Not enough free disk space"), m_errorText);
        return;
    case T::ErrorCannotGetLock:
    case T::ErrorLockRequired:
        fail(key, tr("Another package manager is running"), m_errorText);
        return;
    case T::ErrorDepResolutionFailed:
    case T::ErrorPackageConflicts:
        fail(key, tr("The dependencies cannot be satisfied"), m_errorText);
        return;
    case T::ErrorPackageDownloadFailed:
    case T::ErrorNoMoreMirrorsToTry:
    case T::ErrorCannotFetchSources:
        fail(key, tr("A download failed"), m_errorText);
        return;
    case T::ErrorCannotRemoveSystemPackage:
        fail(key, tr("This is a system package and cannot be removed"), m_errorText);
        return;
    case T::ErrorBadGpgSignature:
    case T::ErrorMissingGpgSignature:
    case T::ErrorGpgFailure:
        fail(key, tr("A signature could not be verified"), m_errorText);
        return;
    default:
        fail(key, fallbackTitle, m_errorText);
    }
}

void PkJob::fail(const QString &key, const QString &title, const QString &details)
{
    m_errorKey = key;
    m_errorTitle = title;
    m_errorDetails = details.trimmed();
    finish(QStringLiteral("failed"));
}

void PkJob::finish(const QString &result)
{
    m_result = result;
    m_transaction.clear();
    m_cancelRequested = false;
    setPhase(QStringLiteral("finished"));
    Q_EMIT finished(result == QLatin1String("success"));
}

void PkJob::cancel()
{
    if (m_phase == QLatin1String("confirm")) {
        m_note = tr("Nothing was changed.");
        finish(QStringLiteral("cancelled"));
        return;
    }
    if (!cancellable())
        return;
    m_cancelRequested = true;
    if (m_transaction)
        m_transaction->cancel();
    Q_EMIT progressChanged();
}

void PkJob::dismiss()
{
    if (m_phase == QLatin1String("finished"))
        setPhase(QStringLiteral("idle"));
}

} // namespace Arsenal
