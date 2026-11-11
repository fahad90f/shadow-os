// SPDX-License-Identifier: GPL-3.0-or-later
#include "servicemodel.h"

#include "sysfs.h"

#include <QDBusArgument>
#include <QDBusError>
#include <QDBusPendingCall>
#include <QDBusPendingCallWatcher>
#include <QFileInfo>
#include <QJsonArray>
#include <QRegularExpression>

#include <algorithm>

using namespace Qt::StringLiterals;

namespace {

const QString kService = u"org.freedesktop.systemd1"_s;
const QString kPath = u"/org/freedesktop/systemd1"_s;
const QString kManager = u"org.freedesktop.systemd1.Manager"_s;

const int kListTimeoutMs = 20000;
// A changing call is answered only after the user authenticated.
const int kCallTimeoutMs = 5 * 60 * 1000;
// Starting or stopping a unit may take as long as its own timeouts allow (90 s by default).
const int kJobTimeoutMs = 5 * 60 * 1000;
const int kRefreshMs = 10000;

// The characters systemd allows in unit names — and nothing else is sent to it.
bool validUnitName(const QString &unit)
{
    static const QRegularExpression pattern(u"^[A-Za-z0-9:_.@\\\\-]{1,247}\\.service$"_s);
    return pattern.match(unit).hasMatch();
}

// What to tell the user about a failed D-Bus call.
QString describeError(const QDBusMessage &reply)
{
    const QString name = reply.errorName();
    if (name == "org.freedesktop.DBus.Error.AccessDenied"_L1
        || name == "org.freedesktop.DBus.Error.InteractiveAuthorizationRequired"_L1
        || name == "org.freedesktop.PolicyKit1.Error.NotAuthorized"_L1) {
        return ServiceModel::tr("Not authorised: %1").arg(reply.errorMessage().isEmpty() ? name : reply.errorMessage());
    }
    if (name == "org.freedesktop.DBus.Error.NoReply"_L1 || name == "org.freedesktop.DBus.Error.Timeout"_L1)
        return ServiceModel::tr("systemd did not answer in time.");
    if (name == "org.freedesktop.DBus.Error.ServiceUnknown"_L1 || name == "org.freedesktop.DBus.Error.NameHasNoOwner"_L1)
        return ServiceModel::tr("systemd is not reachable on the system bus.");
    return reply.errorMessage().isEmpty() ? name : reply.errorMessage();
}

// Run 'done' with the reply (or error) of an asynchronous call.
template <typename Done>
void whenAnswered(QObject *context, const QDBusPendingCall &call, Done done)
{
    auto *watcher = new QDBusPendingCallWatcher(call, context);
    QObject::connect(watcher, &QDBusPendingCallWatcher::finished, context, [done](QDBusPendingCallWatcher *w) {
        w->deleteLater();
        done(w->reply());
    });
}

} // namespace

// ---- ServiceRecord -------------------------------------------------------------

QString ServiceRecord::name() const
{
    return unit.endsWith(".service"_L1) ? unit.chopped(8) : unit;
}

QString ServiceRecord::status() const
{
    if (active == "active"_L1)
        return sub.isEmpty() ? active : sub;        // running, exited, ...
    return active.isEmpty() ? u"inactive"_s : active;
}

QVariantMap ServiceRecord::toVariant() const
{
    return {
        {u"unit"_s, unit},
        {u"name"_s, name()},
        {u"description"_s, description},
        {u"load"_s, load},
        {u"active"_s, active},
        {u"sub"_s, sub},
        {u"fileState"_s, fileState},
        {u"status"_s, status()},
    };
}

// ---- model ---------------------------------------------------------------------

ServiceModel::ServiceModel(QObject *parent)
    : QAbstractListModel(parent)
    , m_bus(QDBusConnection::systemBus())
{
    // sd_booted(3): systemd is the running init system if /run/systemd/system exists.
    if (!QFileInfo(u"/run/systemd/system"_s).isDir()) {
        const QString init = Sys::readText(u"/proc/1/comm"_s);
        m_unavailableTitle = tr("systemd is not running");
        m_unavailableReason = init.isEmpty()
            ? tr("Services are managed by systemd, which is not the init system of this environment, "
                 "so there is nothing to list or control.")
            : tr("Services are managed by systemd, but the init system of this environment (PID 1) is “%1”, "
                 "so there is nothing to list or control.").arg(init);
    } else if (!m_bus.isConnected()) {
        m_unavailableTitle = tr("The system bus is not reachable");
        m_unavailableReason = tr("systemd is running, but the D-Bus system bus it is controlled through "
                                 "could not be reached (%1), so services cannot be listed.")
                                  .arg(m_bus.lastError().message().isEmpty() ? tr("no connection")
                                                                             : m_bus.lastError().message());
    } else {
        m_available = true;
        // Job results are broadcast only to clients that subscribed.
        m_bus.asyncCall(managerCall(u"Subscribe"_s, {}));
        m_bus.connect(kService, kPath, kManager, u"JobRemoved"_s, this,
                      SLOT(jobRemoved(uint,QDBusObjectPath,QString,QString)));
    }

    m_timer.setInterval(kRefreshMs);
    connect(&m_timer, &QTimer::timeout, this, &ServiceModel::refresh);
    m_jobTimeout.setSingleShot(true);
    m_jobTimeout.setInterval(kJobTimeoutMs);
    connect(&m_jobTimeout, &QTimer::timeout, this, [this] {
        if (!m_jobPath.isEmpty()) {
            finishAction(false, tr("%1 did not finish within five minutes; the job may still be running.")
                                    .arg(m_busyUnit));
        }
    });
}

QDBusMessage ServiceModel::managerCall(const QString &method, const QVariantList &arguments, bool interactive) const
{
    QDBusMessage message = QDBusMessage::createMethodCall(kService, kPath, kManager, method);
    message.setArguments(arguments);
    message.setInteractiveAuthorizationAllowed(interactive);
    return message;
}

int ServiceModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : int(m_rows.size());
}

QHash<int, QByteArray> ServiceModel::roleNames() const
{
    return {
        {UnitRole, "unit"},
        {NameRole, "name"},
        {DescriptionRole, "description"},
        {LoadRole, "load"},
        {ActiveRole, "active"},
        {SubRole, "sub"},
        {FileStateRole, "fileState"},
        {StatusRole, "status"},
    };
}

QVariant ServiceModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_rows.size())
        return {};
    const ServiceRecord &r = m_rows.at(index.row());
    switch (role) {
    case UnitRole: return r.unit;
    case Qt::DisplayRole:
    case NameRole: return r.name();
    case DescriptionRole: return r.description;
    case LoadRole: return r.load;
    case ActiveRole: return r.active;
    case SubRole: return r.sub;
    case FileStateRole: return r.fileState;
    case StatusRole: return r.status();
    default: return {};
    }
}

void ServiceModel::setActive(bool active)
{
    if (m_active == active)
        return;
    m_active = active;
    if (m_active && m_available) {
        refresh();
        m_timer.start();
    } else {
        m_timer.stop();
    }
    Q_EMIT activeChanged();
}

// ---- listing -------------------------------------------------------------------

void ServiceModel::refresh()
{
    // While an action runs the states are in flux; a listing follows it anyway.
    if (!m_available || m_listing || busy())
        return;
    m_listing = true;
    m_pendingSteps = 2;
    const QVariantList arguments = {QStringList(), QStringList{u"*.service"_s}};
    whenAnswered(this, m_bus.asyncCall(managerCall(u"ListUnitsByPatterns"_s, arguments), kListTimeoutMs),
                 [this](const QDBusMessage &reply) {
        m_unitsReply = reply;
        listingStepDone();
    });
    whenAnswered(this, m_bus.asyncCall(managerCall(u"ListUnitFilesByPatterns"_s, arguments), kListTimeoutMs),
                 [this](const QDBusMessage &reply) {
        m_filesReply = reply;
        listingStepDone();
    });
}

void ServiceModel::listingStepDone()
{
    if (--m_pendingSteps == 0)
        finishListing();
}

void ServiceModel::finishListing()
{
    const auto fail = [this](const QString &message) {
        m_error = message;
        m_listing = false;
        m_loaded = true;
        Q_EMIT statsChanged();
        Q_EMIT refreshed();
    };

    if (m_unitsReply.type() != QDBusMessage::ReplyMessage) {
        fail(describeError(m_unitsReply));
        return;
    }
    if (m_unitsReply.signature() != "a(ssssssouso)"_L1) {
        fail(tr("systemd answered the unit listing in an unexpected format (%1).").arg(m_unitsReply.signature()));
        return;
    }

    // a(ssssssouso): name, description, load, active, sub, followed unit,
    // object path, job id, job type, job path.
    QList<ServiceRecord> records;
    QHash<QString, qsizetype> byUnit;
    {
        const QDBusArgument units = m_unitsReply.arguments().constFirst().value<QDBusArgument>();
        units.beginArray();
        while (!units.atEnd()) {
            ServiceRecord r;
            QString following, jobType;
            QDBusObjectPath unitPath, jobPath;
            uint jobId = 0;
            units.beginStructure();
            units >> r.unit >> r.description >> r.load >> r.active >> r.sub >> following >> unitPath >> jobId
                  >> jobType >> jobPath;
            units.endStructure();
            // "not-found" units are only names other units refer to; nothing is installed.
            if (r.unit.isEmpty() || r.load == "not-found"_L1)
                continue;
            byUnit.insert(r.unit, records.size());
            records << r;
        }
        units.endArray();
    }

    // Installed unit files — a(ss): path, state — give the enablement of the
    // loaded units, plus the services that are installed but not loaded
    // (never started in this boot). If this listing fails, enablement simply
    // stays unknown.
    const bool haveFiles = m_filesReply.type() == QDBusMessage::ReplyMessage && m_filesReply.signature() == "a(ss)"_L1;
    if (haveFiles) {
        const QDBusArgument files = m_filesReply.arguments().constFirst().value<QDBusArgument>();
        files.beginArray();
        while (!files.atEnd()) {
            QString path, state;
            files.beginStructure();
            files >> path >> state;
            files.endStructure();
            const QString unit = path.section(u'/', -1);
            // Templates ("getty@.service") cannot be started without an instance name.
            if (unit.isEmpty() || unit.contains("@."_L1))
                continue;
            const auto it = byUnit.constFind(unit);
            if (it != byUnit.constEnd()) {
                records[*it].fileState = state;
            } else {
                ServiceRecord r;
                r.unit = unit;
                r.active = u"inactive"_s;
                r.sub = u"dead"_s;
                r.fileState = state;
                byUnit.insert(unit, records.size());
                records << r;
            }
        }
        files.endArray();
    }
    m_unitsReply = QDBusMessage();
    m_filesReply = QDBusMessage();

    // Loaded units without an entry of their own in that list — instances of
    // templates, units created at run time — are asked about individually.
    QStringList unknown;
    for (const ServiceRecord &r : std::as_const(records)) {
        if (r.fileState.isEmpty() && validUnitName(r.unit))
            unknown << r.unit;
    }
    if (unknown.isEmpty() || !haveFiles) {
        publish(records);
        return;
    }
    m_pendingRecords = records;
    m_pendingSteps = int(unknown.size());
    for (const QString &unit : std::as_const(unknown)) {
        whenAnswered(this, m_bus.asyncCall(managerCall(u"GetUnitFileState"_s, {unit}), kListTimeoutMs),
                     [this, unit](const QDBusMessage &reply) {
            // An error (no unit file at all) leaves the state unknown.
            if (reply.type() == QDBusMessage::ReplyMessage && !reply.arguments().isEmpty()) {
                for (ServiceRecord &record : m_pendingRecords) {
                    if (record.unit == unit) {
                        record.fileState = reply.arguments().constFirst().toString();
                        break;
                    }
                }
            }
            enablementStepDone();
        });
    }
}

void ServiceModel::enablementStepDone()
{
    if (--m_pendingSteps > 0)
        return;
    const QList<ServiceRecord> records = m_pendingRecords;
    m_pendingRecords.clear();
    publish(records);
}

void ServiceModel::publish(const QList<ServiceRecord> &records)
{
    m_all = records;
    m_runningCount = m_failedCount = 0;
    for (const ServiceRecord &r : std::as_const(m_all)) {
        if (r.active == "active"_L1 && r.sub == "running"_L1)
            ++m_runningCount;
        else if (r.active == "failed"_L1)
            ++m_failedCount;
    }
    m_error.clear();
    m_loaded = true;
    m_listing = false;
    rebuild();
}

// ---- filtering and sorting -------------------------------------------------------

void ServiceModel::setFilter(const QString &filter)
{
    const QString trimmed = filter.trimmed();
    if (trimmed == m_filter)
        return;
    m_filter = trimmed;
    Q_EMIT filterChanged();
    rebuild();
}

void ServiceModel::setStateFilter(const QString &filter)
{
    if (filter == m_stateFilter || (filter != "all"_L1 && filter != "running"_L1 && filter != "failed"_L1))
        return;
    m_stateFilter = filter;
    Q_EMIT filterChanged();
    rebuild();
}

QString ServiceModel::sortRole() const
{
    return QString::fromLatin1(roleNames().value(m_sortRole));
}

void ServiceModel::sortBy(const QString &role, bool ascending)
{
    const int key = roleNames().key(role.toLatin1(), -1);
    if (key < 0 || (key == m_sortRole && ascending == m_sortAscending))
        return;
    m_sortRole = Role(key);
    m_sortAscending = ascending;
    Q_EMIT sortChanged();
    rebuild();
}

bool ServiceModel::accepts(const ServiceRecord &r) const
{
    if (m_stateFilter == "running"_L1 && !(r.active == "active"_L1 && r.sub == "running"_L1))
        return false;
    if (m_stateFilter == "failed"_L1 && r.active != "failed"_L1)
        return false;
    if (m_filter.isEmpty())
        return true;
    return r.unit.contains(m_filter, Qt::CaseInsensitive) || r.description.contains(m_filter, Qt::CaseInsensitive);
}

void ServiceModel::rebuild()
{
    QList<ServiceRecord> rows;
    rows.reserve(m_all.size());
    for (const ServiceRecord &r : std::as_const(m_all)) {
        if (accepts(r))
            rows << r;
    }

    const Role role = m_sortRole;
    const bool ascending = m_sortAscending;
    const auto key = [role](const ServiceRecord &r) {
        switch (role) {
        case DescriptionRole: return r.description;
        case LoadRole: return r.load;
        case ActiveRole: return r.active;
        case SubRole: return r.sub;
        case FileStateRole: return r.fileState;
        case StatusRole: return r.status();
        case UnitRole:
        case NameRole: break;
        }
        return r.unit;
    };
    std::sort(rows.begin(), rows.end(), [&key, ascending](const ServiceRecord &a, const ServiceRecord &b) {
        const int order = QString::compare(key(a), key(b), Qt::CaseInsensitive);
        if (order != 0)
            return ascending ? order < 0 : order > 0;
        return QString::compare(a.unit, b.unit, Qt::CaseInsensitive) < 0;
    });

    Q_EMIT aboutToRefresh();
    applyRows(rows);
    updateSelection();
    Q_EMIT statsChanged();
    Q_EMIT refreshed();
}

// Same in-place update as the process table: the view never resets.
void ServiceModel::applyRows(const QList<ServiceRecord> &rows)
{
    m_updating = true;
    const int before = int(m_rows.size());
    const int after = int(rows.size());

    if (after < before) {
        beginRemoveRows(QModelIndex(), after, before - 1);
        m_rows.remove(after, before - after);
        endRemoveRows();
    }

    const int common = qMin(before, after);
    int firstChanged = -1;
    for (int i = 0; i <= common; ++i) {
        const bool changed = i < common && !(m_rows.at(i) == rows.at(i));
        if (changed) {
            m_rows[i] = rows.at(i);
            if (firstChanged < 0)
                firstChanged = i;
        } else if (firstChanged >= 0) {
            Q_EMIT dataChanged(index(firstChanged), index(i - 1));
            firstChanged = -1;
        }
    }

    if (after > before) {
        beginInsertRows(QModelIndex(), before, after - 1);
        m_rows.append(rows.mid(before));
        endInsertRows();
    }
    m_updating = false;
}

// ---- selection -------------------------------------------------------------------

QString ServiceModel::selectedName() const
{
    return m_selectedUnit.endsWith(".service"_L1) ? m_selectedUnit.chopped(8) : m_selectedUnit;
}

bool ServiceModel::selectRow(int row)
{
    if (m_updating)
        return false;
    m_selectedUnit = row >= 0 && row < m_rows.size() ? m_rows.at(row).unit : QString();
    updateSelection();
    return true;
}

void ServiceModel::updateSelection()
{
    const ServiceRecord *record = nullptr;
    for (const ServiceRecord &r : std::as_const(m_all)) {
        if (r.unit == m_selectedUnit) {
            record = &r;
            break;
        }
    }
    int row = -1;
    if (!record) {
        m_selectedUnit.clear();     // the unit is gone
    } else {
        for (int i = 0; i < m_rows.size(); ++i) {
            if (m_rows.at(i).unit == m_selectedUnit) {
                row = i;
                break;
            }
        }
    }
    m_selectedRow = row;

    m_canStart = m_canStop = m_canRestart = m_canEnable = m_canDisable = false;
    m_enablementHint.clear();
    if (record && row >= 0) {
        const bool masked = record->load == "masked"_L1 || record->fileState.startsWith("masked"_L1);
        const bool up = record->active == "active"_L1 || record->active == "activating"_L1
            || record->active == "reloading"_L1;
        m_canStart = !masked && (record->active == "inactive"_L1 || record->active == "failed"_L1);
        m_canStop = up || record->active == "deactivating"_L1;
        m_canRestart = !masked && up;
        m_canEnable = record->fileState == "disabled"_L1;
        m_canDisable = record->fileState == "enabled"_L1 || record->fileState == "enabled-runtime"_L1;
        if (!m_canEnable && !m_canDisable) {
            if (masked)
                m_enablementHint = tr("This service is masked: it is blocked from being started at all.");
            else if (record->fileState == "static"_L1)
                m_enablementHint = tr("This service is static: it has no start-up rules of its own and runs when another unit needs it.");
            else if (record->fileState.isEmpty())
                m_enablementHint = tr("systemd reports no start-up setting for this service.");
            else
                m_enablementHint = tr("The start-up setting “%1” cannot be switched here.").arg(record->fileState);
        }
    }
    Q_EMIT selectionChanged();
}

// ---- actions ---------------------------------------------------------------------

void ServiceModel::perform(const QString &action, const QString &unit)
{
    static const QStringList actions = {u"start"_s, u"stop"_s, u"restart"_s, u"enable"_s, u"disable"_s};
    if (!m_available || !actions.contains(action) || !validUnitName(unit)) {
        Q_EMIT actionFinished(unit, action, false, tr("“%1” is not something that can be done to “%2”.").arg(action, unit));
        return;
    }
    if (busy()) {
        Q_EMIT actionFinished(unit, action, false, tr("Another service action is still in progress."));
        return;
    }
    m_busyUnit = unit;
    m_busyAction = action;
    m_jobPath.clear();
    m_finishedJobs.clear();
    Q_EMIT busyChanged();

    const QString name = unit.endsWith(".service"_L1) ? unit.chopped(8) : unit;

    if (action == "enable"_L1 || action == "disable"_L1) {
        // EnableUnitFiles(files, runtime, force) / DisableUnitFiles(files, runtime),
        // then Reload so that systemd acts on the changed links — what
        // "systemctl enable|disable" does.
        const bool enable = action == "enable"_L1;
        const QVariantList arguments = enable ? QVariantList{QStringList{unit}, false, false}
                                              : QVariantList{QStringList{unit}, false};
        whenAnswered(this, m_bus.asyncCall(managerCall(enable ? u"EnableUnitFiles"_s : u"DisableUnitFiles"_s,
                                                       arguments, true), kCallTimeoutMs),
                     [this, enable, name](const QDBusMessage &reply) {
            if (reply.type() != QDBusMessage::ReplyMessage) {
                finishAction(false, describeError(reply));
                return;
            }
            // EnableUnitFiles answers (b carries_install_info, a(sss) changes).
            const bool nothingToEnable = enable && !reply.arguments().isEmpty()
                && reply.arguments().constFirst().metaType().id() == QMetaType::Bool
                && !reply.arguments().constFirst().toBool();
            whenAnswered(this, m_bus.asyncCall(managerCall(u"Reload"_s, {}, true), kCallTimeoutMs),
                         [this, enable, name, nothingToEnable](const QDBusMessage &reload) {
                if (reload.type() != QDBusMessage::ReplyMessage) {
                    finishAction(false, tr("The start-up setting of %1 was changed, but systemd could not reload its "
                                           "configuration: %2").arg(name, describeError(reload)));
                } else if (nothingToEnable) {
                    finishAction(false, tr("%1 has no installation rules ([Install] section), so there is nothing "
                                           "to enable.").arg(name));
                } else {
                    finishAction(true, enable ? tr("%1 will start automatically at boot.").arg(name)
                                              : tr("%1 will no longer start automatically at boot.").arg(name));
                }
            });
        });
        return;
    }

    // StartUnit / StopUnit / RestartUnit(name, mode) queue a job and answer
    // with its object path; the job's outcome arrives as JobRemoved.
    const QString method = action == "start"_L1 ? u"StartUnit"_s : action == "stop"_L1 ? u"StopUnit"_s : u"RestartUnit"_s;
    whenAnswered(this, m_bus.asyncCall(managerCall(method, {unit, u"replace"_s}, true), kCallTimeoutMs),
                 [this](const QDBusMessage &reply) {
        if (reply.type() != QDBusMessage::ReplyMessage) {
            finishAction(false, describeError(reply));
            return;
        }
        m_jobPath = reply.arguments().isEmpty() ? QString()
                                                : reply.arguments().constFirst().value<QDBusObjectPath>().path();
        if (m_jobPath.isEmpty()) {
            finishAction(false, tr("systemd answered without naming the job it queued."));
            return;
        }
        m_jobTimeout.start();
        settleJob();        // the job may already be over
    });
}

void ServiceModel::jobRemoved(uint, const QDBusObjectPath &job, const QString &, const QString &result)
{
    if (!busy())
        return;
    m_finishedJobs.insert(job.path(), result);
    settleJob();
}

// Finish the action in progress once the job it queued has been removed.
void ServiceModel::settleJob()
{
    if (m_jobPath.isEmpty())
        return;
    const auto it = m_finishedJobs.constFind(m_jobPath);
    if (it == m_finishedJobs.constEnd())
        return;
    const QString result = *it;
    const QString name = m_busyUnit.endsWith(".service"_L1) ? m_busyUnit.chopped(8) : m_busyUnit;
    if (result == "done"_L1) {
        finishAction(true, m_busyAction == "start"_L1 ? tr("%1 was started.").arg(name)
                         : m_busyAction == "stop"_L1 ? tr("%1 was stopped.").arg(name)
                         : tr("%1 was restarted.").arg(name));
    } else {
        const QString what = m_busyAction == "start"_L1 ? tr("Starting %1").arg(name)
            : m_busyAction == "stop"_L1 ? tr("Stopping %1").arg(name) : tr("Restarting %1").arg(name);
        finishAction(false, result == "failed"_L1 ? tr("%1 failed; the service's log (journalctl -u %2) says why.").arg(what, m_busyUnit)
                          : result == "timeout"_L1 ? tr("%1 timed out.").arg(what)
                          : result == "canceled"_L1 ? tr("%1 was cancelled by another request.").arg(what)
                          : result == "dependency"_L1 ? tr("%1 failed because a unit it depends on failed.").arg(what)
                          : result == "skipped"_L1 ? tr("%1 was skipped: it does not apply to the unit's current state.").arg(what)
                          : tr("%1 ended with the result “%2”.").arg(what, result));
    }
}

void ServiceModel::finishAction(bool ok, const QString &message)
{
    const QString unit = m_busyUnit;
    const QString action = m_busyAction;
    m_jobTimeout.stop();
    m_jobPath.clear();
    m_finishedJobs.clear();
    m_busyUnit.clear();
    m_busyAction.clear();
    Q_EMIT busyChanged();
    Q_EMIT actionFinished(unit, action, ok, message);
    refresh();
}

QJsonObject ServiceModel::toJson() const
{
    QJsonArray list;
    for (const ServiceRecord &r : m_all)
        list.append(QJsonObject::fromVariantMap(r.toVariant()));
    return {
        {u"available"_s, m_available},
        {u"unavailableReason"_s, m_unavailableReason},
        {u"error"_s, m_error},
        {u"services"_s, list},
    };
}
