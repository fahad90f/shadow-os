// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "jobthread.h"
#include "processreader.h"

#include <QAbstractListModel>
#include <QQmlEngine>
#include <QTimer>

#include <memory>

// The process table.
//
// While 'active', /proc is scanned every 'interval' milliseconds on a worker
// thread (ProcessReader). Filtering and sorting happen here; rows are updated
// in place, so the view keeps its scroll position. The selection is tied to
// the process (PID + start time), not to a row: 'selectedRow' follows the
// process around as the order changes and becomes -1 when it exits.
//
// Role names (for NxDataTable columns): pid, ppid, name, user, cpu, memory,
// state, stateCode, command, threads, nice, kernelThread, cpuTime, depth.
class ProcessModel : public QAbstractListModel
{
    Q_OBJECT
    QML_ELEMENT

    Q_PROPERTY(bool active READ active WRITE setActive NOTIFY activeChanged)
    Q_PROPERTY(int interval READ interval WRITE setInterval NOTIFY intervalChanged)
    Q_PROPERTY(QString filter READ filter WRITE setFilter NOTIFY filterChanged)
    Q_PROPERTY(bool showKernelThreads READ showKernelThreads WRITE setShowKernelThreads NOTIFY optionsChanged)
    Q_PROPERTY(bool onlyOwnProcesses READ onlyOwnProcesses WRITE setOnlyOwnProcesses NOTIFY optionsChanged)
    Q_PROPERTY(bool tree READ tree WRITE setTree NOTIFY optionsChanged)
    Q_PROPERTY(bool loaded READ loaded NOTIFY statsChanged)
    Q_PROPERTY(int count READ count NOTIFY statsChanged)
    Q_PROPERTY(int processCount READ processCount NOTIFY statsChanged)
    Q_PROPERTY(int kernelThreadCount READ kernelThreadCount NOTIFY statsChanged)
    Q_PROPERTY(int runningCount READ runningCount NOTIFY statsChanged)
    Q_PROPERTY(QString sortRole READ sortRole NOTIFY sortChanged)
    Q_PROPERTY(bool sortAscending READ sortAscending NOTIFY sortChanged)
    Q_PROPERTY(int selectedPid READ selectedPid NOTIFY selectionChanged)
    Q_PROPERTY(int selectedRow READ selectedRow NOTIFY selectionChanged)
    Q_PROPERTY(QString selectedName READ selectedName NOTIFY selectionChanged)
    Q_PROPERTY(bool selectedProtected READ selectedProtected NOTIFY selectionChanged)
    Q_PROPERTY(bool selectedStopped READ selectedStopped NOTIFY selectionChanged)

public:
    enum Role {
        PidRole = Qt::UserRole + 1,
        PpidRole,
        NameRole,
        UserRole,
        CpuRole,
        MemoryRole,
        StateRole,
        StateCodeRole,
        CommandRole,
        ThreadsRole,
        NiceRole,
        KernelThreadRole,
        CpuTimeRole,
        DepthRole,
    };

    explicit ProcessModel(QObject *parent = nullptr);
    ~ProcessModel() override;

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    bool active() const { return m_active; }
    void setActive(bool active);
    int interval() const { return m_interval; }
    void setInterval(int milliseconds);
    QString filter() const { return m_filter; }
    void setFilter(const QString &filter);
    bool showKernelThreads() const { return m_showKernelThreads; }
    void setShowKernelThreads(bool show);
    bool onlyOwnProcesses() const { return m_onlyOwn; }
    void setOnlyOwnProcesses(bool only);
    // Tree view: every process follows its parent, children in the chosen
    // sort order; 'depth' is the nesting level among the rows shown.
    bool tree() const { return m_tree; }
    void setTree(bool tree);

    bool loaded() const { return m_loaded; }
    int count() const { return int(m_rows.size()); }
    int processCount() const { return m_processCount; }
    int kernelThreadCount() const { return m_kernelThreadCount; }
    int runningCount() const { return m_runningCount; }
    QString sortRole() const;
    bool sortAscending() const { return m_sortAscending; }
    int selectedPid() const { return m_selectedPid; }
    int selectedRow() const { return m_selectedRow; }
    QString selectedName() const { return m_selectedName; }
    bool selectedProtected() const { return m_selectedProtected; }
    bool selectedStopped() const { return m_selectedStopped; }

    // Sort by a role name; ties are broken by PID so equal rows do not jump.
    Q_INVOKABLE void sortBy(const QString &role, bool ascending);
    // Select the process shown in 'row' (-1 clears). Returns false if the
    // request was ignored because the rows are being rearranged right now.
    Q_INVOKABLE bool selectRow(int row);
    Q_INVOKABLE void refresh();

    // Facts the confirmation dialogs need about the process in 'row':
    // pid, name, user, command, nice, stopped, token (identity to pass back
    // to sendSignal / setPriority), protected + protectedReason (PID 1 and
    // kernel threads are never touched), self (this program; it cannot
    // suspend itself), otherUser (acting on it needs administrator rights),
    // privileged (this program runs as root or with CAP_SYS_NICE-like rights:
    // euid 0).
    Q_INVOKABLE QVariantMap describe(int row) const;

    // Send a signal to the process identified by pid + token. 'action' is
    // "end" (SIGTERM), "kill" (SIGKILL), "stop" (SIGSTOP) or "continue"
    // (SIGCONT). The signal is delivered directly; if the kernel refuses
    // because the process belongs to someone else, it is sent through
    // "pkexec /usr/bin/kill" — but only when 'asAdministrator' is set, i.e.
    // after the user agreed to authenticate. The outcome arrives through
    // actionFinished().
    Q_INVOKABLE void sendSignal(int pid, const QString &token, const QString &action, bool asAdministrator);

    // Change the nice value (-20 … 19) of the process identified by pid +
    // token, like renice(1). Raising the priority (a lower nice value) or
    // touching another user's process is refused by the kernel; with
    // 'asAdministrator' it is then done through "pkexec /usr/bin/renice".
    // The outcome arrives through actionFinished() with the action "renice".
    Q_INVOKABLE void setPriority(int pid, const QString &token, int nice, bool asAdministrator);

    Q_INVOKABLE void copyToClipboard(const QString &text) const;

Q_SIGNALS:
    void activeChanged();
    void intervalChanged();
    void filterChanged();
    void optionsChanged();
    void statsChanged();
    void sortChanged();
    void selectionChanged();
    // Emitted around every rearrangement of the rows. The view drops its
    // current row before and re-applies selectedRow after, so that it never
    // moves the current row on its own while rows come and go.
    void aboutToRefresh();
    void refreshed();
    // action: "end", "kill", "stop", "continue" or "renice".
    // outcome: "done", "gone" (already exited), "refused" (protected process),
    // "needs-administrator" (permission denied, not yet tried as administrator),
    // "cancelled" (authentication dismissed) or "failed".
    void actionFinished(int pid, const QString &name, const QString &action, const QString &outcome, const QString &message);

private:
    void setProcesses(const QList<ProcessRecord> &processes);
    void rebuild();
    void applyRows(const QList<ProcessRecord> &rows);
    void updateSelection();
    bool accepts(const ProcessRecord &record) const;
    const ProcessRecord *find(int pid) const;
    QString refusal(int pid, const QString &token, const QString &name, const QString &action, QString *outcome) const;
    void runAsAdministrator(int pid, const QString &name, const QString &action, const QString &program,
                            const QStringList &arguments, const QString &doneMessage);

    bool m_active = false;
    int m_interval = 1000;
    QString m_filter;
    bool m_showKernelThreads = false;
    bool m_onlyOwn = false;
    bool m_tree = false;
    bool m_loaded = false;
    bool m_warmUpPending = false;
    bool m_updating = false;
    Role m_sortRole = CpuRole;
    bool m_sortAscending = false;

    QList<ProcessRecord> m_all;     // every process of the last scan
    QList<ProcessRecord> m_rows;    // filtered and sorted: what the view shows
    int m_processCount = 0;
    int m_kernelThreadCount = 0;
    int m_runningCount = 0;

    int m_selectedPid = -1;
    quint64 m_selectedStart = 0;
    int m_selectedRow = -1;
    QString m_selectedName;
    bool m_selectedProtected = false;
    bool m_selectedStopped = false;

    quint32 m_ownUid = 0;
    QTimer m_timer;
    QTimer m_warmUp;

    // Declared last: the thread stops before anything above is destroyed.
    std::shared_ptr<ProcessReader> m_reader;
    JobThread m_worker;
};
