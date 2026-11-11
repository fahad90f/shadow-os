// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QAbstractListModel>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusObjectPath>
#include <QHash>
#include <QJsonObject>
#include <QQmlEngine>
#include <QTimer>

// One systemd service unit.
struct ServiceRecord
{
    QString unit;                   // "ssh.service"
    QString description;
    QString load;                   // loaded, masked, error, ...; empty: unit file exists but is not loaded
    QString active;                 // active, inactive, failed, activating, deactivating, reloading
    QString sub;                    // running, exited, dead, failed, ...
    QString fileState;              // enabled, disabled, static, masked, indirect, generated, ...; empty: not reported

    bool operator==(const ServiceRecord &other) const = default;

    QString name() const;           // unit name without ".service"
    QString status() const;         // what the State column shows
    QVariantMap toVariant() const;
};

// The system's services, listed and controlled through systemd's D-Bus API
// (org.freedesktop.systemd1.Manager on the system bus):
//
//   ListUnitsByPatterns([], ["*.service"])       loaded units and their state
//   ListUnitFilesByPatterns([], ["*.service"])   installed unit files and their enablement
//   GetUnitFileState(unit)                       enablement of loaded units without an own file entry
//   StartUnit / StopUnit / RestartUnit(unit, "replace"), then the JobRemoved
//       signal of the job they queued            start, stop, restart
//   EnableUnitFiles / DisableUnitFiles, Reload   start-up setting
//
// The changing calls are sent with "interactive authorisation allowed":
// systemd has polkit authorise them and the session's polkit agent asks the
// user to authenticate where the policy demands it.
//
// When systemd is not the running init system (no /run/systemd/system) or
// the system bus cannot be reached, 'available' is false and
// 'unavailableTitle' / 'unavailableReason' say why; nothing is listed.
//
// Role names: unit, name, description, load, active, sub, fileState, status.
class ServiceModel : public QAbstractListModel
{
    Q_OBJECT
    QML_ELEMENT

    Q_PROPERTY(bool available READ available CONSTANT)
    Q_PROPERTY(QString unavailableTitle READ unavailableTitle CONSTANT)
    Q_PROPERTY(QString unavailableReason READ unavailableReason CONSTANT)
    Q_PROPERTY(bool active READ active WRITE setActive NOTIFY activeChanged)
    Q_PROPERTY(bool loaded READ loaded NOTIFY statsChanged)
    Q_PROPERTY(QString error READ error NOTIFY statsChanged)
    Q_PROPERTY(QString filter READ filter WRITE setFilter NOTIFY filterChanged)
    Q_PROPERTY(QString stateFilter READ stateFilter WRITE setStateFilter NOTIFY filterChanged)
    Q_PROPERTY(int count READ count NOTIFY statsChanged)
    Q_PROPERTY(int totalCount READ totalCount NOTIFY statsChanged)
    Q_PROPERTY(int runningCount READ runningCount NOTIFY statsChanged)
    Q_PROPERTY(int failedCount READ failedCount NOTIFY statsChanged)
    Q_PROPERTY(QString sortRole READ sortRole NOTIFY sortChanged)
    Q_PROPERTY(bool sortAscending READ sortAscending NOTIFY sortChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    Q_PROPERTY(QString busyUnit READ busyUnit NOTIFY busyChanged)
    Q_PROPERTY(QString busyAction READ busyAction NOTIFY busyChanged)
    Q_PROPERTY(int selectedRow READ selectedRow NOTIFY selectionChanged)
    Q_PROPERTY(QString selectedUnit READ selectedUnit NOTIFY selectionChanged)
    Q_PROPERTY(QString selectedName READ selectedName NOTIFY selectionChanged)
    Q_PROPERTY(bool canStart READ canStart NOTIFY selectionChanged)
    Q_PROPERTY(bool canStop READ canStop NOTIFY selectionChanged)
    Q_PROPERTY(bool canRestart READ canRestart NOTIFY selectionChanged)
    Q_PROPERTY(bool canEnable READ canEnable NOTIFY selectionChanged)
    Q_PROPERTY(bool canDisable READ canDisable NOTIFY selectionChanged)
    Q_PROPERTY(QString enablementHint READ enablementHint NOTIFY selectionChanged)

public:
    enum Role {
        UnitRole = Qt::UserRole + 1,
        NameRole,
        DescriptionRole,
        LoadRole,
        ActiveRole,
        SubRole,
        FileStateRole,
        StatusRole,
    };

    explicit ServiceModel(QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    bool available() const { return m_available; }
    QString unavailableTitle() const { return m_unavailableTitle; }
    QString unavailableReason() const { return m_unavailableReason; }
    bool active() const { return m_active; }
    void setActive(bool active);
    bool loaded() const { return m_loaded; }
    QString error() const { return m_error; }
    QString filter() const { return m_filter; }
    void setFilter(const QString &filter);
    QString stateFilter() const { return m_stateFilter; }
    void setStateFilter(const QString &filter);
    int count() const { return int(m_rows.size()); }
    int totalCount() const { return int(m_all.size()); }
    int runningCount() const { return m_runningCount; }
    int failedCount() const { return m_failedCount; }
    QString sortRole() const;
    bool sortAscending() const { return m_sortAscending; }
    bool busy() const { return !m_busyUnit.isEmpty(); }
    QString busyUnit() const { return m_busyUnit; }
    QString busyAction() const { return m_busyAction; }
    int selectedRow() const { return m_selectedRow; }
    QString selectedUnit() const { return m_selectedUnit; }
    QString selectedName() const;
    bool canStart() const { return m_canStart; }
    bool canStop() const { return m_canStop; }
    bool canRestart() const { return m_canRestart; }
    bool canEnable() const { return m_canEnable; }
    bool canDisable() const { return m_canDisable; }
    QString enablementHint() const { return m_enablementHint; }

    Q_INVOKABLE void refresh();
    Q_INVOKABLE void sortBy(const QString &role, bool ascending);
    Q_INVOKABLE bool selectRow(int row);
    // action: "start", "stop", "restart", "enable" or "disable".
    Q_INVOKABLE void perform(const QString &action, const QString &unit);

    QJsonObject toJson() const;

Q_SIGNALS:
    void activeChanged();
    void filterChanged();
    void statsChanged();
    void sortChanged();
    void busyChanged();
    void selectionChanged();
    // Emitted around every rearrangement of the rows (see ProcessModel);
    // refreshed() also ends a listing attempt that failed.
    void aboutToRefresh();
    void refreshed();
    void actionFinished(const QString &unit, const QString &action, bool ok, const QString &message);

private:
private Q_SLOTS:
    void jobRemoved(uint id, const QDBusObjectPath &job, const QString &unit, const QString &result);

private:
    QDBusMessage managerCall(const QString &method, const QVariantList &arguments, bool interactive = false) const;
    void listingStepDone();
    void finishListing();
    void enablementStepDone();
    void finishAction(bool ok, const QString &message);
    void settleJob();
    void publish(const QList<ServiceRecord> &records);
    void rebuild();
    void applyRows(const QList<ServiceRecord> &rows);
    void updateSelection();
    bool accepts(const ServiceRecord &record) const;

    bool m_available = false;
    QString m_unavailableTitle;
    QString m_unavailableReason;
    bool m_active = false;
    bool m_loaded = false;
    bool m_listing = false;
    bool m_updating = false;
    int m_pendingSteps = 0;
    QDBusConnection m_bus;
    QDBusMessage m_unitsReply;
    QDBusMessage m_filesReply;
    QList<ServiceRecord> m_pendingRecords;      // while GetUnitFileState calls are outstanding
    QString m_jobPath;                          // job queued by the action in progress
    QHash<QString, QString> m_finishedJobs;     // job path -> result, while an action is in progress
    QTimer m_jobTimeout;
    QString m_error;
    QString m_filter;
    QString m_stateFilter = QStringLiteral("all");
    Role m_sortRole = NameRole;
    bool m_sortAscending = true;

    QList<ServiceRecord> m_all;
    QList<ServiceRecord> m_rows;
    int m_runningCount = 0;
    int m_failedCount = 0;

    QString m_busyUnit;
    QString m_busyAction;
    QString m_selectedUnit;
    int m_selectedRow = -1;
    bool m_canStart = false;
    bool m_canStop = false;
    bool m_canRestart = false;
    bool m_canEnable = false;
    bool m_canDisable = false;
    QString m_enablementHint;
    QTimer m_timer;
};
