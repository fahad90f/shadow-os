// SPDX-License-Identifier: GPL-3.0-or-later
#include "pkjob.h"

#include "pkservice.h"
#include "pkutil.h"

#include <Daemon>

#include <QCoreApplication>
#include <QDBusPendingCallWatcher>
#include <QJsonArray>
#include <QJsonObject>
#include <QTime>
#include <QTimer>

namespace ShadowPk {

using T = PackageKit::Transaction;

namespace {

QVariantMap entryMap(const PackageEntry &e)
{
    return QVariantMap{
        {QStringLiteral("packageId"), e.id},
        {QStringLiteral("name"), e.name},
        {QStringLiteral("version"), e.version},
        {QStringLiteral("arch"), e.arch},
        {QStringLiteral("repo"), e.repo},
        {QStringLiteral("repoText"), repoText(e.repo)},
        {QStringLiteral("summary"), e.summary},
    };
}

QStringList namesOf(const QVariantList &entries)
{
    QStringList names;
    for (const QVariant &v : entries)
        names << v.toMap().value(QStringLiteral("name")).toString();
    return names;
}

} // namespace

QVariantMap summarizeChanges(const QList<PackageEntry> &simulated, qulonglong downloadSize,
                             const QStringList &requestedNames)
{
    QVariantList install, remove, update, reinstall, downgrade, obsolete, untrusted, additional;
    for (const PackageEntry &e : simulated) {
        const QVariantMap m = entryMap(e);
        switch (e.info) {
        case T::InfoInstalling:
        case T::InfoInstall:
            install << m;
            if (!requestedNames.contains(e.name))
                additional << m;
            break;
        case T::InfoUpdating:
            update << m;
            if (!requestedNames.contains(e.name))
                additional << m;
            break;
        case T::InfoRemoving:
        case T::InfoRemove:
            remove << m;
            break;
        case T::InfoReinstalling:
            reinstall << m;
            break;
        case T::InfoDowngrading:
        case T::InfoDowngrade:
            downgrade << m;
            break;
        case T::InfoObsoleting:
        case T::InfoObsolete:
            obsolete << m;
            break;
        case T::InfoUntrusted:
            untrusted << m;
            break;
        default:
            break;
        }
    }
    return QVariantMap{
        {QStringLiteral("install"), install},
        {QStringLiteral("remove"), remove},
        {QStringLiteral("update"), update},
        {QStringLiteral("reinstall"), reinstall},
        {QStringLiteral("downgrade"), downgrade},
        {QStringLiteral("obsolete"), obsolete},
        {QStringLiteral("untrusted"), untrusted},
        {QStringLiteral("additional"), additional},
        {QStringLiteral("installCount"), install.size()},
        {QStringLiteral("removeCount"), remove.size()},
        {QStringLiteral("updateCount"), update.size()},
        {QStringLiteral("reinstallCount"), reinstall.size()},
        {QStringLiteral("downgradeCount"), downgrade.size()},
        {QStringLiteral("obsoleteCount"), obsolete.size()},
        {QStringLiteral("untrustedCount"), untrusted.size()},
        {QStringLiteral("additionalCount"), additional.size()},
        {QStringLiteral("downloadSize"), static_cast<double>(downloadSize)},
    };
}

Job::Job(const QString &kind, const QString &title, Starter starter, QObject *parent)
    : QObject(parent), m_kind(kind), m_title(title), m_starter(std::move(starter))
{
}

Job::~Job() = default;

void Job::log(const QString &line)
{
    m_log << QStringLiteral("[%1] %2").arg(QTime::currentTime().toString(QStringLiteral("HH:mm:ss")), line);
    Q_EMIT logChanged();
}

void Job::setPhase(const QString &phase)
{
    if (phase == m_phase)
        return;
    m_phase = phase;
    Q_EMIT phaseChanged();
    Q_EMIT progressChanged();
}

QString Job::status() const
{
    return statusKey(m_status);
}

QString Job::statusText() const
{
    if (m_phase == QLatin1String("waiting"))
        return tr("Waiting for your decision");
    if (m_phase == QLatin1String("finished")) {
        if (m_result == QLatin1String("success")) return tr("Finished");
        if (m_result == QLatin1String("cancelled")) return tr("Cancelled");
        return tr("Failed");
    }
    if (m_phase == QLatin1String("simulating")) {
        // The simulation resolves dependencies and checks signatures; show what
        // PackageKit reports once it is past its queue.
        if (m_status == T::StatusUnknown || m_status == T::StatusSetup || m_status == T::StatusWait)
            return tr("Calculating what will change");
        return tr("Calculating what will change — %1").arg(ShadowPk::statusText(m_status));
    }
    if (m_cancelRequested)
        return tr("Cancelling");
    return ShadowPk::statusText(m_status);
}

bool Job::cancellable() const
{
    if (m_phase == QLatin1String("waiting"))
        return true;
    if (m_phase == QLatin1String("simulating") || m_phase == QLatin1String("running"))
        return m_allowCancel && !m_cancelRequested;
    return false;
}

QString Job::restart() const
{
    switch (m_restartRank) {
    case 1: return QStringLiteral("application");
    case 2: return QStringLiteral("session");
    case 3: return QStringLiteral("system");
    default: break;
    }
    return QStringLiteral("none");
}

void Job::resetProgress()
{
    m_status = T::StatusUnknown;
    m_percentage = -1;
    m_allowCancel = false;
    m_downloadRemaining = 0;
    m_downloadRate = 0;
    m_remainingTime = 0;
    m_rateTimer.invalidate();
    m_rateLastRemaining = 0;
    m_currentItem.clear();
    m_cancelRequested = false;
    Q_EMIT progressChanged();
}

void Job::updateFromTransaction()
{
    PackageKit::Transaction *t = m_transaction;
    if (!t)
        return;
    const T::Status status = t->status();
    if (status != m_status) {
        m_status = status;
        log(tr("Status: %1 (%2)").arg(ShadowPk::statusText(status), statusKey(status)));
    }
    const uint percentage = t->percentage();
    m_percentage = percentage <= 100 ? int(percentage) : -1;
    m_allowCancel = t->allowCancel();
    m_remainingTime = int(t->remainingTime());

    const qulonglong remaining = t->downloadSizeRemaining();
    if (remaining != m_downloadRemaining) {
        // Download rate from how fast the remaining byte count shrinks.
        if (m_rateTimer.isValid() && remaining < m_rateLastRemaining) {
            const qint64 ms = m_rateTimer.elapsed();
            if (ms >= 500) {
                const double rate = static_cast<double>(m_rateLastRemaining - remaining) * 1000.0 / static_cast<double>(ms);
                m_downloadRate = m_downloadRate > 0 ? 0.6 * m_downloadRate + 0.4 * rate : rate;
                m_rateLastRemaining = remaining;
                m_rateTimer.restart();
            }
        } else {
            m_rateLastRemaining = remaining;
            m_rateTimer.start();
        }
        m_downloadRemaining = remaining;
    }
    if (m_status != T::StatusDownload)
        m_downloadRate = 0;
    Q_EMIT progressChanged();
}

void Job::start()
{
    if (m_phase != QLatin1String("idle"))
        return;
    log(m_title);
    if (!Service::instance()->available()) {
        // Without the daemon nothing can be changed; say so instead of pretending to start.
        setPhase(QStringLiteral("running"));
        QTimer::singleShot(0, this, [this] {
            m_hasError = true;
            m_errorKey = QStringLiteral("packagekit-unreachable");
            m_errorTitle = tr("PackageKit service is not reachable");
            m_errorDetails = Service::instance()->error();
            log(tr("%1: %2").arg(m_errorTitle, m_errorDetails));
            finishJob(QStringLiteral("failed"));
        });
        return;
    }
    if (m_simulateFirst)
        runSimulation();
    else
        runReal();
}

void Job::runSimulation()
{
    setPhase(QStringLiteral("simulating"));
    resetProgress();
    T::TransactionFlags flags = T::TransactionFlagSimulate;
    if (!m_allowUntrusted)
        flags |= T::TransactionFlagOnlyTrusted;
    log(tr("Simulating the transaction (nothing is changed yet)"));

    PackageKit::Transaction *t = m_starter(flags);
    m_transaction = t;
    if (t) {
        connect(t, &T::statusChanged, this, &Job::updateFromTransaction);
        connect(t, &T::allowCancelChanged, this, &Job::updateFromTransaction);
    }
    m_query = Query::run(this, t, [this](const QueryResult &result) { simulationDone(result); });
}

void Job::simulationDone(const QueryResult &result)
{
    m_transaction.clear();
    m_query.clear();
    log(tr("Simulation finished: %1").arg(exitKey(result.exit)));

    switch (result.exit) {
    case T::ExitSuccess:
    case T::ExitNeedUntrusted: {
        m_simulated = true;
        m_changes = summarizeChanges(result.packages, result.downloadSize, m_requestedNames);
        Q_EMIT changesChanged();
        const QStringList installing = namesOf(m_changes.value(QStringLiteral("install")).toList());
        const QStringList updating = namesOf(m_changes.value(QStringLiteral("update")).toList());
        const QStringList removing = namesOf(m_changes.value(QStringLiteral("remove")).toList());
        const QStringList untrusted = namesOf(m_changes.value(QStringLiteral("untrusted")).toList());
        if (!installing.isEmpty()) log(tr("To install: %1").arg(installing.join(QLatin1Char(' '))));
        if (!updating.isEmpty()) log(tr("To update: %1").arg(updating.join(QLatin1Char(' '))));
        if (!removing.isEmpty()) log(tr("To remove: %1").arg(removing.join(QLatin1Char(' '))));
        if (result.downloadSize > 0) log(tr("Download size: %1 bytes").arg(result.downloadSize));

        if ((result.exit == T::ExitNeedUntrusted || !untrusted.isEmpty()) && !m_allowUntrusted) {
            if (!untrusted.isEmpty())
                log(tr("NOT AUTHENTICATED: %1").arg(untrusted.join(QLatin1Char(' '))));
            if (result.hasError)
                log(tr("PackageKit: %1: %2").arg(ShadowPk::errorKey(result.error), result.errorDetails));
            raisePrompt(QStringLiteral("untrusted"), QVariantMap{
                {QStringLiteral("packages"), m_changes.value(QStringLiteral("untrusted"))},
                {QStringLiteral("details"), result.errorDetails},
                {QStringLiteral("errorKey"), result.hasError ? ShadowPk::errorKey(result.error) : QString()},
                {QStringLiteral("afterSimulation"), true},
            });
            return;
        }
        continueAfterSimulation();
        return;
    }
    case T::ExitCancelled:
    case T::ExitCancelledPriority:
        finishJob(QStringLiteral("cancelled"));
        return;
    case T::ExitEulaRequired:
    case T::ExitKeyRequired:
    case T::ExitMediaChangeRequired:
        // The real run raises the matching question with all its details.
        log(tr("The simulation needs an answer from you; continuing with the real transaction to ask it"));
        runReal();
        return;
    default:
        break;
    }

    if (result.hasError) {
        recordError(result.error, result.errorDetails);
    } else {
        m_hasError = true;
        m_errorKey = exitKey(result.exit);
        m_errorTitle = tr("The changes could not be calculated");
        m_errorDetails = tr("PackageKit ended the simulation with exit code “%1” and reported no error.").arg(exitKey(result.exit));
    }
    if (result.exit == T::ExitRepairRequired)
        m_repairRequired = true;
    finishJob(QStringLiteral("failed"));
}

void Job::continueAfterSimulation()
{
    const bool removals = m_changes.value(QStringLiteral("removeCount")).toInt() > 0;
    const bool ask = m_confirm == Confirm::Always || (m_confirm == Confirm::IfRemovals && removals);
    if (ask && !m_confirmed) {
        raisePrompt(QStringLiteral("confirm"), m_changes);
        return;
    }
    runReal();
}

void Job::runReal()
{
    setPhase(QStringLiteral("running"));
    resetProgress();
    m_hasError = false;
    m_errorKey.clear();
    m_errorTitle.clear();
    m_errorDetails.clear();
    m_pendingKey.clear();
    m_pendingMedia.clear();
    // Only-trusted unless the user explicitly allowed otherwise in the danger prompt.
    const T::TransactionFlags flags = m_allowUntrusted ? T::TransactionFlags(T::TransactionFlagNone)
                                                       : T::TransactionFlags(T::TransactionFlagOnlyTrusted);
    log(m_allowUntrusted ? tr("Starting the transaction — UNTRUSTED packages allowed by the user")
                         : tr("Starting the transaction (only trusted packages)"));
    attach(m_starter(flags));
}

void Job::attach(PackageKit::Transaction *t)
{
    m_transaction = t;
    if (!t) {
        QTimer::singleShot(0, this, [this] {
            recordError(T::ErrorInternalError, tr("PackageKit did not create a transaction"));
            finishJob(QStringLiteral("failed"));
        });
        return;
    }

    connect(t, &T::statusChanged, this, &Job::updateFromTransaction);
    connect(t, &T::percentageChanged, this, &Job::updateFromTransaction);
    connect(t, &T::allowCancelChanged, this, &Job::updateFromTransaction);
    connect(t, &T::downloadSizeRemainingChanged, this, &Job::updateFromTransaction);
    connect(t, &T::remainingTimeChanged, this, &Job::updateFromTransaction);

    connect(t, &T::package, this, [this](T::Info rawInfo, const QString &packageId, const QString &summary) {
        const T::Info info = infoBase(rawInfo);
        const PackageId id = PackageId::parse(packageId);
        const QString text = infoText(info);
        PackageEntry *e = m_items.find(packageId);
        if (!e) {
            PackageEntry fresh = PackageEntry::fromId(packageId, rawInfo, summary);
            fresh.status = text;
            m_items.upsert(fresh);
        } else {
            const int percentage = info == T::InfoFinished ? 100 : e->percentage;
            m_items.setItemInfo(packageId, rawInfo, summary);
            m_items.setItemProgress(packageId, text, percentage);
        }
        m_currentItem = QStringLiteral("%1 %2 %3").arg(text, id.name, id.version).trimmed();
        log(QStringLiteral("%1: %2 %3 (%4)").arg(infoKey(info), id.name, id.version, id.arch));
        Q_EMIT progressChanged();
    });
    connect(t, &T::itemProgress, this, [this](const QString &itemId, T::Status status, uint percentage) {
        const PackageEntry *e = m_items.find(itemId);
        const QString text = e && !e->status.isEmpty() ? e->status : ShadowPk::statusText(status);
        m_items.setItemProgress(itemId, text, percentage <= 100 ? int(percentage) : -1);
    });
    connect(t, &T::errorCode, this, [this](T::Error error, const QString &details) {
        recordError(error, details);
    });
    connect(t, &T::eulaRequired, this,
            [this](const QString &eulaId, const QString &packageId, const QString &vendor, const QString &agreement) {
        m_pendingEulas.append(QVariantMap{
            {QStringLiteral("eulaId"), eulaId},
            {QStringLiteral("packageId"), packageId},
            {QStringLiteral("packageName"), PackageId::parse(packageId).name},
            {QStringLiteral("vendor"), vendor},
            {QStringLiteral("licenseAgreement"), agreement},
        });
        log(tr("Licence agreement required for %1 (vendor: %2)").arg(packageId, vendor));
    });
    connect(t, &T::repoSignatureRequired, this,
            [this](const QString &packageId, const QString &repoName, const QString &keyUrl, const QString &keyUserId,
                   const QString &keyId, const QString &keyFingerprint, const QString &keyTimestamp, T::SigType type) {
        m_pendingKey = QVariantMap{
            {QStringLiteral("packageId"), packageId},
            {QStringLiteral("packageName"), PackageId::parse(packageId).name},
            {QStringLiteral("repository"), repoName},
            {QStringLiteral("keyUrl"), keyUrl},
            {QStringLiteral("keyUserId"), keyUserId},
            {QStringLiteral("keyId"), keyId},
            {QStringLiteral("keyFingerprint"), keyFingerprint},
            {QStringLiteral("keyTimestamp"), keyTimestamp},
            {QStringLiteral("sigType"), static_cast<int>(type)},
            {QStringLiteral("sigTypeKey"), sigTypeKey(type)},
        };
        log(tr("Signing key %1 of repository “%2” is not trusted yet (fingerprint %3)")
                .arg(keyId, repoName, keyFingerprint));
    });
    connect(t, &T::mediaChangeRequired, this, [this](T::MediaType type, const QString &id, const QString &text) {
        m_pendingMedia = QVariantMap{
            {QStringLiteral("mediaType"), mediaTypeKey(type)},
            {QStringLiteral("mediaId"), id},
            {QStringLiteral("text"), text},
        };
        log(tr("Media change required: %1 (%2)").arg(text, id));
    });
    connect(t, &T::requireRestart, this, [this](T::Restart type, const QString &packageId) {
        const int rank = restartRank(type);
        if (rank <= 0)
            return;
        m_restartRank = qMax(m_restartRank, rank);
        m_restartSecurity = m_restartSecurity || restartIsSecurity(type);
        const QString name = PackageId::parse(packageId).name;
        if (!name.isEmpty() && !m_restartPackages.contains(name))
            m_restartPackages << name;
        log(tr("Restart required (%1) by %2").arg(restartKey(type), packageId.isEmpty() ? tr("the transaction") : packageId));
        Q_EMIT restartChanged();
    });
    connect(t, &T::finished, this, &Job::transactionFinished);
    connect(t, &QObject::destroyed, this, [this, t] {
        // The transaction object went away without telling us how it ended
        // (the daemon crashed or was stopped). Never leave the job hanging.
        if (m_phase != QLatin1String("running") || (m_transaction && m_transaction != t))
            return;
        if (!m_hasError)
            recordError(T::ErrorInternalError, tr("The PackageKit transaction ended unexpectedly; the daemon may have stopped."));
        finishJob(QStringLiteral("failed"));
    });
}

void Job::recordError(PackageKit::Transaction::Error error, const QString &details)
{
    m_hasError = true;
    m_errorKey = ShadowPk::errorKey(error);
    m_errorTitle = errorText(error);
    m_errorDetails = details;
    if (error == T::ErrorUnfinishedTransaction)
        m_repairRequired = true;
    log(tr("PackageKit error %1: %2").arg(m_errorKey, details));
}

void Job::transactionFinished(PackageKit::Transaction::Exit exit, uint runtime)
{
    if (m_phase != QLatin1String("running"))
        return;
    PackageKit::Transaction *t = m_transaction;
    if (t)
        disconnect(t, &QObject::destroyed, this, nullptr);
    m_transaction.clear();
    log(tr("PackageKit finished the transaction: %1 (%2 s)").arg(exitKey(exit)).arg(runtime / 1000.0, 0, 'f', 1));

    switch (exit) {
    case T::ExitSuccess:
        finishJob(QStringLiteral("success"));
        return;
    case T::ExitCancelled:
    case T::ExitCancelledPriority:
        finishJob(QStringLiteral("cancelled"));
        return;
    case T::ExitNeedUntrusted:
        if (!m_allowUntrusted) {
            raisePrompt(QStringLiteral("untrusted"), QVariantMap{
                {QStringLiteral("packages"), QVariantList()},
                {QStringLiteral("details"), m_errorDetails},
                {QStringLiteral("errorKey"), m_errorKey},
                {QStringLiteral("afterSimulation"), false},
            });
            return;
        }
        break;
    case T::ExitEulaRequired:
        if (!m_pendingEulas.isEmpty()) {
            raiseNextEula();
            return;
        }
        break;
    case T::ExitKeyRequired:
        if (!m_pendingKey.isEmpty()) {
            raisePrompt(QStringLiteral("key"), m_pendingKey);
            return;
        }
        break;
    case T::ExitMediaChangeRequired:
        if (m_pendingMedia.isEmpty())
            m_pendingMedia.insert(QStringLiteral("text"), m_errorDetails);
        raisePrompt(QStringLiteral("media"), m_pendingMedia);
        return;
    case T::ExitRepairRequired:
        m_repairRequired = true;
        break;
    default:
        break;
    }

    if (!m_hasError) {
        m_hasError = true;
        m_errorKey = exitKey(exit);
        m_errorTitle = tr("The operation failed");
        m_errorDetails = tr("PackageKit ended the transaction with exit code “%1” and reported no error.").arg(exitKey(exit));
    }
    finishJob(QStringLiteral("failed"));
}

void Job::raisePrompt(const QString &kind, const QVariantMap &prompt)
{
    m_promptKind = kind;
    m_prompt = prompt;
    // Nothing is running while the question is open: no stale "100 %".
    m_percentage = -1;
    m_currentItem.clear();
    log(tr("Waiting for the user: %1").arg(kind));
    setPhase(QStringLiteral("waiting"));
    Q_EMIT promptChanged();
}

void Job::raiseNextEula()
{
    QVariantMap prompt = m_pendingEulas.constFirst().toMap();
    prompt.insert(QStringLiteral("remaining"), m_pendingEulas.size() - 1);
    raisePrompt(QStringLiteral("eula"), prompt);
}

void Job::runAuxiliary(PackageKit::Transaction *transaction, const QString &what)
{
    setPhase(QStringLiteral("running"));
    resetProgress();
    log(what);
    m_transaction = transaction;
    if (transaction)
        connect(transaction, &T::statusChanged, this, &Job::updateFromTransaction);
    m_query = Query::run(this, transaction, [this](const QueryResult &result) {
        m_transaction.clear();
        m_query.clear();
        if (!result.ok) {
            if (result.exit == T::ExitCancelled || result.exit == T::ExitCancelledPriority) {
                finishJob(QStringLiteral("cancelled"));
                return;
            }
            if (result.hasError) {
                recordError(result.error, result.errorDetails);
            } else {
                m_hasError = true;
                m_errorKey = exitKey(result.exit);
                m_errorTitle = tr("The operation failed");
                m_errorDetails = result.errorMessage();
            }
            finishJob(QStringLiteral("failed"));
            return;
        }
        if (!m_pendingEulas.isEmpty()) {
            // The accepted agreement is done; ask about the next one, if any.
            m_pendingEulas.removeFirst();
            if (!m_pendingEulas.isEmpty()) {
                raiseNextEula();
                return;
            }
        }
        resume();
    });
}

void Job::resume()
{
    // Run the transaction again now that its question has been answered.
    runReal();
}

void Job::answer(bool accept)
{
    if (m_phase != QLatin1String("waiting"))
        return;
    const QString kind = m_promptKind;
    const QVariantMap prompt = m_prompt;
    m_promptKind.clear();
    m_prompt.clear();
    Q_EMIT promptChanged();

    if (!accept) {
        log(tr("Declined by the user (%1); nothing was changed by this step").arg(kind));
        finishJob(QStringLiteral("cancelled"));
        return;
    }

    if (kind == QLatin1String("untrusted")) {
        m_allowUntrusted = true;
        log(tr("The user explicitly allowed packages that cannot be authenticated"));
        if (prompt.value(QStringLiteral("afterSimulation")).toBool())
            continueAfterSimulation();
        else
            runReal();
    } else if (kind == QLatin1String("confirm")) {
        m_confirmed = true;
        log(tr("Confirmed by the user"));
        runReal();
    } else if (kind == QLatin1String("eula")) {
        const QString eulaId = prompt.value(QStringLiteral("eulaId")).toString();
        runAuxiliary(PackageKit::Daemon::acceptEula(eulaId), tr("Accepting licence agreement %1").arg(eulaId));
    } else if (kind == QLatin1String("key")) {
        const QString keyId = prompt.value(QStringLiteral("keyId")).toString();
        runAuxiliary(PackageKit::Daemon::installSignature(
                         static_cast<T::SigType>(prompt.value(QStringLiteral("sigType")).toInt()), keyId,
                         prompt.value(QStringLiteral("packageId")).toString()),
                     tr("Trusting signing key %1").arg(keyId));
    } else {
        // "media": the user changed the medium; try again.
        resume();
    }
}

void Job::cancel()
{
    if (m_phase == QLatin1String("waiting")) {
        m_promptKind.clear();
        m_prompt.clear();
        Q_EMIT promptChanged();
        log(tr("Cancelled by the user while a question was pending"));
        finishJob(QStringLiteral("cancelled"));
        return;
    }
    PackageKit::Transaction *t = m_transaction;
    if (!t || !active())
        return;
    if (!t->allowCancel()) {
        log(tr("PackageKit does not allow cancelling at this stage"));
        return;
    }
    m_cancelRequested = true;
    log(tr("Cancel requested by the user"));
    Q_EMIT progressChanged();
    auto *watcher = new QDBusPendingCallWatcher(t->cancel(), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this](QDBusPendingCallWatcher *w) {
        w->deleteLater();
        if (w->isError()) {
            m_cancelRequested = false;
            log(tr("PackageKit refused to cancel: %1").arg(w->error().message()));
            Q_EMIT progressChanged();
        }
    });
}

void Job::finishJob(const QString &result)
{
    if (m_phase == QLatin1String("finished"))
        return;
    m_result = result;
    if (result == QLatin1String("success")) {
        m_hasError = false;
        m_errorKey.clear();
        m_errorTitle.clear();
        m_errorDetails.clear();
        m_percentage = 100;
    } else if (result == QLatin1String("cancelled") && m_errorKey == QLatin1String("transaction-cancelled")) {
        // "Cancelled" is the outcome the user asked for, not an error to show.
        m_hasError = false;
        m_errorKey.clear();
        m_errorTitle.clear();
        m_errorDetails.clear();
    }
    m_transaction.clear();
    m_cancelRequested = false;
    m_allowCancel = false;
    log(tr("Result: %1").arg(result));
    setPhase(QStringLiteral("finished"));
    Q_EMIT finished(result == QLatin1String("success"));
}

QJsonObject Job::toJson() const
{
    QJsonObject o{
        {QStringLiteral("kind"), m_kind},
        {QStringLiteral("title"), m_title},
        {QStringLiteral("phase"), m_phase},
        {QStringLiteral("result"), m_result},
        {QStringLiteral("restart"), restart()},
        {QStringLiteral("restartForSecurity"), m_restartSecurity},
        {QStringLiteral("restartPackages"), QJsonArray::fromStringList(m_restartPackages)},
        {QStringLiteral("untrustedAccepted"), m_allowUntrusted},
        {QStringLiteral("repairRequired"), m_repairRequired},
        {QStringLiteral("changes"), QJsonObject::fromVariantMap(m_changes)},
        {QStringLiteral("log"), QJsonArray::fromStringList(m_log)},
    };
    if (m_hasError) {
        o.insert(QStringLiteral("errorCode"), m_errorKey);
        o.insert(QStringLiteral("errorTitle"), m_errorTitle);
        o.insert(QStringLiteral("errorDetails"), m_errorDetails);
    }
    return o;
}

namespace Jobs {

namespace {
QStringList namesOfIds(const QStringList &packageIds)
{
    QStringList names;
    for (const QString &id : packageIds)
        names << PackageId::parse(id).name;
    return names;
}
} // namespace

Job *install(const QStringList &packageIds, const QString &title, QObject *parent)
{
    auto *job = new Job(QStringLiteral("install"), title, [packageIds](T::TransactionFlags flags) {
        return PackageKit::Daemon::installPackages(packageIds, flags);
    }, parent);
    job->setSimulateFirst(true);
    job->setConfirm(Job::Confirm::IfRemovals);
    job->setRequestedNames(namesOfIds(packageIds));
    return job;
}

Job *remove(const QStringList &packageIds, const QString &title, QObject *parent)
{
    // allowDeps: packages that depend on these are removed too — the
    // simulation lists them and the user confirms the complete list.
    // autoRemove off: unrelated, no-longer-needed packages are left alone.
    auto *job = new Job(QStringLiteral("remove"), title, [packageIds](T::TransactionFlags flags) {
        return PackageKit::Daemon::removePackages(packageIds, true, false, flags);
    }, parent);
    job->setSimulateFirst(true);
    job->setConfirm(Job::Confirm::Always);
    job->setRequestedNames(namesOfIds(packageIds));
    return job;
}

Job *update(const QStringList &packageIds, const QString &title, QObject *parent)
{
    auto *job = new Job(QStringLiteral("update"), title, [packageIds](T::TransactionFlags flags) {
        return PackageKit::Daemon::updatePackages(packageIds, flags);
    }, parent);
    job->setSimulateFirst(true);
    job->setConfirm(Job::Confirm::IfRemovals);
    job->setRequestedNames(namesOfIds(packageIds));
    return job;
}

Job *refresh(bool force, QObject *parent)
{
    return new Job(QStringLiteral("refresh"), QCoreApplication::translate("ShadowPk", "Refreshing software lists"),
                   [force](T::TransactionFlags) { return PackageKit::Daemon::refreshCache(force); }, parent);
}

Job *repair(QObject *parent)
{
    auto *job = new Job(QStringLiteral("repair"), QCoreApplication::translate("ShadowPk", "Repairing the package system"),
                        [](T::TransactionFlags flags) { return PackageKit::Daemon::repairSystem(flags); }, parent);
    job->setSimulateFirst(true);
    job->setConfirm(Job::Confirm::IfRemovals);
    return job;
}

Job *repoEnable(const QString &repoId, bool enable, const QString &title, QObject *parent)
{
    return new Job(QStringLiteral("repository"), title,
                   [repoId, enable](T::TransactionFlags) { return PackageKit::Daemon::repoEnable(repoId, enable); }, parent);
}

} // namespace Jobs

} // namespace ShadowPk
