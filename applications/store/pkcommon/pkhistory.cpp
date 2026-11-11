// SPDX-License-Identifier: GPL-3.0-or-later
#include "pkhistory.h"

#include "pkservice.h"
#include "pkutil.h"

#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDateTime>
#include <QLocale>

#include <algorithm>

namespace NexusPk {

namespace {
const QString kService = QStringLiteral("org.freedesktop.PackageKit");
const QString kTransactionInterface = QStringLiteral("org.freedesktop.PackageKit.Transaction");
} // namespace

void fetchOldTransactions(QObject *context, uint count,
                          std::function<void(const QVariantList &, const QString &)> done)
{
    if (!Service::instance()->available()) {
        QTimer::singleShot(0, context, [done] { done({}, Service::instance()->error()); });
        return;
    }
    auto *request = new OldTransactionsRequest(count, std::move(done), context);
    request->start();
}

QVariantList mergeHistory(const QVariantList &aptEntries, const QVariantList &packageKitTransactions)
{
    struct Pk
    {
        QVariantMap map;
        QDateTime time;
        qint64 durationMs = 0;
        QString role;
        bool used = false;
    };
    QList<Pk> transactions;
    transactions.reserve(packageKitTransactions.size());
    for (const QVariant &v : packageKitTransactions) {
        Pk pk;
        pk.map = v.toMap();
        pk.time = QDateTime::fromString(pk.map.value(QStringLiteral("time")).toString(), Qt::ISODate);
        pk.durationMs = pk.map.value(QStringLiteral("durationMs")).toLongLong();
        pk.role = pk.map.value(QStringLiteral("role")).toString();
        transactions.append(pk);
    }

    auto startedBy = [](const QVariantMap &pk) {
        return QStringLiteral("%1 (uid %2)").arg(pk.value(QStringLiteral("user")).toString())
            .arg(pk.value(QStringLiteral("uid")).toUInt());
    };

    QVariantList merged;
    merged.reserve(aptEntries.size());
    for (const QVariant &v : aptEntries) {
        QVariantMap entry = v.toMap();
        entry.insert(QStringLiteral("kind"), QStringLiteral("apt"));
        const QString commandLine = entry.value(QStringLiteral("commandLine")).toString();
        // APT logs PackageKit runs as:  packagekit role='update-packages'
        const qsizetype quote = commandLine.indexOf(QLatin1Char('\''));
        if (entry.value(QStringLiteral("viaPackageKit")).toBool() && quote >= 0) {
            const QString role = commandLine.mid(quote + 1).section(QLatin1Char('\''), 0, 0);
            const QDateTime start = QDateTime::fromString(entry.value(QStringLiteral("start")).toString(), Qt::ISODate);
            // The transaction that was running when dpkg started: same role,
            // begun before the APT entry and still running at that moment.
            Pk *best = nullptr;
            qint64 bestGap = 0;
            for (Pk &pk : transactions) {
                if (pk.used || pk.role != role || !pk.time.isValid() || !start.isValid())
                    continue;
                const qint64 gap = pk.time.msecsTo(start);
                if (gap < -2000 || gap > pk.durationMs + 2000)
                    continue;
                if (!best || gap < bestGap) {
                    best = &pk;
                    bestGap = gap;
                }
            }
            if (best) {
                best->used = true;
                entry.insert(QStringLiteral("startedBy"), startedBy(best->map));
                entry.insert(QStringLiteral("client"), best->map.value(QStringLiteral("commandLine")).toString());
            }
        }
        merged.append(entry);
    }

    // Transactions that failed and left no trace in APT's log.
    for (const Pk &pk : std::as_const(transactions)) {
        if (pk.used || pk.map.value(QStringLiteral("succeeded")).toBool())
            continue;
        QVariantMap entry{
            {QStringLiteral("kind"), QStringLiteral("packagekit-failed")},
            {QStringLiteral("start"), pk.map.value(QStringLiteral("time"))},
            {QStringLiteral("startText"), pk.map.value(QStringLiteral("timeText"))},
            {QStringLiteral("summary"), pk.map.value(QStringLiteral("roleText"))},
            {QStringLiteral("commandLine"), QStringLiteral("packagekit role='%1'").arg(pk.role)},
            {QStringLiteral("startedBy"), startedBy(pk.map)},
            {QStringLiteral("client"), pk.map.value(QStringLiteral("commandLine"))},
            {QStringLiteral("packages"), pk.map.value(QStringLiteral("packages"))},
            {QStringLiteral("packageCount"), pk.map.value(QStringLiteral("packageCount"))},
            {QStringLiteral("viaPackageKit"), true},
            {QStringLiteral("requestedBy"), QString()},
            {QStringLiteral("error"), QString()},
        };
        merged.append(entry);
    }

    std::stable_sort(merged.begin(), merged.end(), [](const QVariant &a, const QVariant &b) {
        return a.toMap().value(QStringLiteral("start")).toString() > b.toMap().value(QStringLiteral("start")).toString();
    });
    return merged;
}

OldTransactionsRequest::OldTransactionsRequest(uint count, Callback done, QObject *parent)
    : QObject(parent), m_count(count), m_done(std::move(done))
{
    m_timeout.setSingleShot(true);
    connect(&m_timeout, &QTimer::timeout, this, [this] {
        finish(tr("PackageKit did not answer the request for its transaction history in time"));
    });
}

void OldTransactionsRequest::start()
{
    m_timeout.start(60000);
    QDBusConnection bus = QDBusConnection::systemBus();
    const QDBusMessage create = QDBusMessage::createMethodCall(
        kService, QStringLiteral("/org/freedesktop/PackageKit"), kService, QStringLiteral("CreateTransaction"));
    auto *watcher = new QDBusPendingCallWatcher(bus.asyncCall(create), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this](QDBusPendingCallWatcher *w) {
        w->deleteLater();
        const QDBusPendingReply<QDBusObjectPath> reply = *w;
        if (reply.isError()) {
            finish(reply.error().message());
            return;
        }
        m_path = reply.value().path();
        QDBusConnection system = QDBusConnection::systemBus();
        system.connect(kService, m_path, kTransactionInterface, QStringLiteral("Transaction"), this,
                       SLOT(onTransaction(QDBusObjectPath,QString,bool,uint,uint,QString,uint,QString)));
        system.connect(kService, m_path, kTransactionInterface, QStringLiteral("ErrorCode"), this,
                       SLOT(onErrorCode(uint,QString)));
        system.connect(kService, m_path, kTransactionInterface, QStringLiteral("Finished"), this,
                       SLOT(onFinished(uint,uint)));

        QDBusMessage call = QDBusMessage::createMethodCall(kService, m_path, kTransactionInterface,
                                                           QStringLiteral("GetOldTransactions"));
        call << m_count;
        auto *callWatcher = new QDBusPendingCallWatcher(system.asyncCall(call), this);
        connect(callWatcher, &QDBusPendingCallWatcher::finished, this, [this](QDBusPendingCallWatcher *cw) {
            cw->deleteLater();
            if (cw->isError())
                finish(cw->error().message());
        });
    });
}

void OldTransactionsRequest::onTransaction(const QDBusObjectPath &tid, const QString &timespec, bool succeeded,
                                           uint role, uint duration, const QString &data, uint uid,
                                           const QString &commandLine)
{
    const QDateTime when = QDateTime::fromString(timespec, Qt::ISODate).toLocalTime();
    QVariantList packages;
    // The daemon stores one "info<TAB>package-id<TAB>summary" line per package.
    const QStringList lines = data.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    for (const QString &line : lines) {
        const QStringList f = line.split(QLatin1Char('\t'));
        if (f.size() < 2)
            continue;
        const PackageId id = PackageId::parse(f.at(1));
        packages << QVariantMap{
            {QStringLiteral("info"), f.at(0)},
            {QStringLiteral("name"), id.valid ? id.name : f.at(1)},
            {QStringLiteral("version"), id.version},
            {QStringLiteral("arch"), id.arch},
            {QStringLiteral("repo"), id.repo},
        };
    }
    const auto roleValue = static_cast<PackageKit::Transaction::Role>(role);
    m_transactions.append(QVariantMap{
        {QStringLiteral("tid"), tid.path()},
        {QStringLiteral("time"), when.isValid() ? when.toString(Qt::ISODate) : timespec},
        {QStringLiteral("timeText"), when.isValid() ? QLocale().toString(when, QLocale::ShortFormat) : timespec},
        {QStringLiteral("succeeded"), succeeded},
        {QStringLiteral("role"), roleKey(roleValue)},
        {QStringLiteral("roleText"), roleText(roleValue)},
        {QStringLiteral("durationMs"), duration},
        {QStringLiteral("uid"), uid},
        {QStringLiteral("user"), userNameForUid(uid)},
        {QStringLiteral("commandLine"), commandLine},
        {QStringLiteral("packages"), packages},
        {QStringLiteral("packageCount"), packages.size()},
    });
}

void OldTransactionsRequest::onErrorCode(uint code, const QString &details)
{
    const auto error = static_cast<PackageKit::Transaction::Error>(code);
    m_error = tr("%1 (PackageKit error “%2”): %3").arg(errorText(error), errorKey(error), details);
}

void OldTransactionsRequest::onFinished(uint exit, uint)
{
    const auto exitValue = static_cast<PackageKit::Transaction::Exit>(exit);
    if (exitValue != PackageKit::Transaction::ExitSuccess && m_error.isEmpty())
        m_error = tr("PackageKit ended the request with exit code “%1”").arg(exitKey(exitValue));
    finish(m_error);
}

void OldTransactionsRequest::finish(const QString &error)
{
    if (m_finished)
        return;
    m_finished = true;
    m_timeout.stop();
    if (!m_path.isEmpty()) {
        QDBusConnection bus = QDBusConnection::systemBus();
        bus.disconnect(kService, m_path, kTransactionInterface, QStringLiteral("Transaction"), this,
                       SLOT(onTransaction(QDBusObjectPath,QString,bool,uint,uint,QString,uint,QString)));
        bus.disconnect(kService, m_path, kTransactionInterface, QStringLiteral("ErrorCode"), this,
                       SLOT(onErrorCode(uint,QString)));
        bus.disconnect(kService, m_path, kTransactionInterface, QStringLiteral("Finished"), this,
                       SLOT(onFinished(uint,uint)));
    }
    // The daemon sends the oldest transaction first.
    std::reverse(m_transactions.begin(), m_transactions.end());
    const Callback done = std::move(m_done);
    m_done = nullptr;
    deleteLater();
    if (done)
        done(m_transactions, error);
}

} // namespace NexusPk
