// SPDX-License-Identifier: GPL-3.0-or-later
#include "processmodel.h"

#include <command.h>

#include <QClipboard>
#include <QFileInfo>
#include <QGuiApplication>

#include <algorithm>
#include <cerrno>
#include <csignal>
#include <cstring>

#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>

using namespace Qt::StringLiterals;

namespace {

const int kWarmUpMs = 500;
const char kKill[] = "/usr/bin/kill";
const char kRenice[] = "/usr/bin/renice";

// Deliver 'sig' to exactly the process that was chosen. A pidfd pins the
// process: once it is open, checking the start time rules out that the PID
// was recycled, and the signal can no longer hit anything else.
// Returns 0 or an errno value.
int deliverSignal(int pid, quint64 startTicks, int sig)
{
#if defined(SYS_pidfd_open) && defined(SYS_pidfd_send_signal)
    const int fd = int(::syscall(SYS_pidfd_open, pid, 0));
    if (fd >= 0) {
        int result = 0;
        const ProcessReader::Identity identity = ProcessReader::identify(pid);
        if (!identity.exists || identity.startTicks != startTicks)
            result = ESRCH;
        else if (::syscall(SYS_pidfd_send_signal, fd, sig, nullptr, 0) != 0)
            result = errno;
        ::close(fd);
        return result;
    }
    if (errno == ESRCH)
        return ESRCH;
    // Kernel without pidfd support (before 5.3) or a seccomp filter: fall through.
#endif
    Q_UNUSED(startTicks)
    return ::kill(pid, sig) == 0 ? 0 : errno;
}

int compareText(const QString &a, const QString &b)
{
    return QString::compare(a, b, Qt::CaseInsensitive);
}

template <typename T>
int compareNumber(T a, T b)
{
    return a < b ? -1 : (b < a ? 1 : 0);
}

} // namespace

ProcessModel::ProcessModel(QObject *parent)
    : QAbstractListModel(parent)
    , m_reader(std::make_shared<ProcessReader>())
    , m_worker(u"nexus-monitor-processes"_s)
{
    m_ownUid = ::geteuid();

    m_timer.setInterval(m_interval);
    connect(&m_timer, &QTimer::timeout, this, &ProcessModel::refresh);
    m_warmUp.setSingleShot(true);
    m_warmUp.setInterval(kWarmUpMs);
    connect(&m_warmUp, &QTimer::timeout, this, [this] {
        refresh();
        if (m_active)
            m_timer.start();
    });
}

ProcessModel::~ProcessModel() = default;

int ProcessModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : int(m_rows.size());
}

QHash<int, QByteArray> ProcessModel::roleNames() const
{
    return {
        {PidRole, "pid"},
        {PpidRole, "ppid"},
        {NameRole, "name"},
        {UserRole, "user"},
        {CpuRole, "cpu"},
        {MemoryRole, "memory"},
        {StateRole, "state"},
        {StateCodeRole, "stateCode"},
        {CommandRole, "command"},
        {ThreadsRole, "threads"},
        {NiceRole, "nice"},
        {KernelThreadRole, "kernelThread"},
        {CpuTimeRole, "cpuTime"},
        {DepthRole, "depth"},
    };
}

QVariant ProcessModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_rows.size())
        return {};
    const ProcessRecord &r = m_rows.at(index.row());
    switch (role) {
    case PidRole: return r.pid;
    case PpidRole: return r.ppid;
    case Qt::DisplayRole:
    case NameRole: return r.name;
    case UserRole: return r.user;
    case CpuRole: return r.cpu;
    case MemoryRole: return r.memory;
    case StateRole: return r.stateText();
    case StateCodeRole: return QString(QLatin1Char(r.state));
    case CommandRole: return r.command;
    case ThreadsRole: return r.threads;
    case NiceRole: return r.nice;
    case KernelThreadRole: return r.kernelThread;
    case CpuTimeRole: return r.cpuTime;
    case DepthRole: return r.depth;
    default: return {};
    }
}

// ---- sampling ------------------------------------------------------------------

void ProcessModel::setActive(bool active)
{
    if (m_active == active)
        return;
    m_active = active;
    if (m_active) {
        // CPU shares need two scans; take the second one soon.
        m_warmUpPending = true;
        refresh();
        m_timer.start();
    } else {
        m_timer.stop();
        m_warmUp.stop();
    }
    Q_EMIT activeChanged();
}

void ProcessModel::setInterval(int milliseconds)
{
    if (milliseconds < 250 || milliseconds == m_interval)
        return;
    m_interval = milliseconds;
    m_timer.setInterval(m_interval);
    Q_EMIT intervalChanged();
}

void ProcessModel::refresh()
{
    const std::shared_ptr<ProcessReader> reader = m_reader;
    m_worker.run([reader] { return reader->scan(); },
                 [this](const QList<ProcessRecord> &processes) { setProcesses(processes); });
}

void ProcessModel::setProcesses(const QList<ProcessRecord> &processes)
{
    m_all = processes;
    m_processCount = m_kernelThreadCount = m_runningCount = 0;
    for (const ProcessRecord &r : std::as_const(m_all)) {
        if (r.kernelThread)
            ++m_kernelThreadCount;
        else
            ++m_processCount;
        if (r.state == 'R')
            ++m_runningCount;
    }
    m_loaded = true;
    rebuild();

    if (m_warmUpPending && m_active) {
        m_warmUpPending = false;
        m_warmUp.start();
    }
}

// ---- filtering and sorting -------------------------------------------------------

void ProcessModel::setFilter(const QString &filter)
{
    const QString trimmed = filter.trimmed();
    if (trimmed == m_filter)
        return;
    m_filter = trimmed;
    Q_EMIT filterChanged();
    rebuild();
}

void ProcessModel::setShowKernelThreads(bool show)
{
    if (show == m_showKernelThreads)
        return;
    m_showKernelThreads = show;
    Q_EMIT optionsChanged();
    rebuild();
}

void ProcessModel::setOnlyOwnProcesses(bool only)
{
    if (only == m_onlyOwn)
        return;
    m_onlyOwn = only;
    Q_EMIT optionsChanged();
    rebuild();
}

void ProcessModel::setTree(bool tree)
{
    if (tree == m_tree)
        return;
    m_tree = tree;
    Q_EMIT optionsChanged();
    rebuild();
}

QString ProcessModel::sortRole() const
{
    return QString::fromLatin1(roleNames().value(m_sortRole));
}

void ProcessModel::sortBy(const QString &role, bool ascending)
{
    const int key = roleNames().key(role.toLatin1(), -1);
    if (key < 0)
        return;
    if (key == m_sortRole && ascending == m_sortAscending)
        return;
    m_sortRole = Role(key);
    m_sortAscending = ascending;
    Q_EMIT sortChanged();
    rebuild();
}

bool ProcessModel::accepts(const ProcessRecord &r) const
{
    if (r.kernelThread && !m_showKernelThreads)
        return false;
    if (m_onlyOwn && r.uid != m_ownUid)
        return false;
    if (m_filter.isEmpty())
        return true;
    return r.name.contains(m_filter, Qt::CaseInsensitive)
        || r.command.contains(m_filter, Qt::CaseInsensitive)
        || r.user.contains(m_filter, Qt::CaseInsensitive)
        || QString::number(r.pid).startsWith(m_filter);
}

void ProcessModel::rebuild()
{
    QList<ProcessRecord> rows;
    rows.reserve(m_all.size());
    for (const ProcessRecord &r : std::as_const(m_all)) {
        if (accepts(r))
            rows << r;
    }

    const Role role = m_sortRole;
    const bool ascending = m_sortAscending;
    std::sort(rows.begin(), rows.end(), [role, ascending](const ProcessRecord &a, const ProcessRecord &b) {
        int order = 0;
        switch (role) {
        case PidRole: order = compareNumber(a.pid, b.pid); break;
        case PpidRole: order = compareNumber(a.ppid, b.ppid); break;
        case NameRole: order = compareText(a.name, b.name); break;
        case UserRole: order = compareText(a.user, b.user); break;
        case CpuRole: order = compareNumber(a.cpu, b.cpu); break;
        case MemoryRole: order = compareNumber(a.memory, b.memory); break;
        case StateRole:
        case StateCodeRole: order = compareText(a.stateText(), b.stateText()); break;
        case CommandRole: order = compareText(a.command, b.command); break;
        case ThreadsRole: order = compareNumber(a.threads, b.threads); break;
        case NiceRole: order = compareNumber(a.nice, b.nice); break;
        case KernelThreadRole: order = compareNumber(a.kernelThread, b.kernelThread); break;
        case CpuTimeRole: order = compareNumber(a.cpuTime, b.cpuTime); break;
        case DepthRole: break;
        }
        if (order != 0)
            return ascending ? order < 0 : order > 0;
        return a.pid < b.pid;
    });

    if (m_tree) {
        // Depth-first from the processes whose parent is not among the rows
        // shown; siblings keep the order the sort above gave them.
        QHash<int, int> indexOf;
        indexOf.reserve(rows.size());
        for (int i = 0; i < rows.size(); ++i)
            indexOf.insert(rows.at(i).pid, i);
        QList<QList<int>> children(rows.size());
        QList<int> roots;
        for (int i = 0; i < rows.size(); ++i) {
            const auto parent = indexOf.constFind(rows.at(i).ppid);
            if (parent != indexOf.constEnd() && *parent != i)
                children[*parent].append(i);
            else
                roots.append(i);
        }
        QList<ProcessRecord> ordered;
        ordered.reserve(rows.size());
        QList<bool> placed(rows.size(), false);
        QList<std::pair<int, int>> stack;       // row index, depth
        for (auto root = roots.crbegin(); root != roots.crend(); ++root)
            stack.append({*root, 0});
        while (!stack.isEmpty()) {
            const auto [i, depth] = stack.takeLast();
            if (placed.at(i))
                continue;
            placed[i] = true;
            ordered.append(rows.at(i));
            ordered.last().depth = depth;
            const QList<int> &kids = children.at(i);
            for (auto kid = kids.crbegin(); kid != kids.crend(); ++kid)
                stack.append({*kid, depth + 1});
        }
        rows = ordered;
    }

    Q_EMIT aboutToRefresh();
    applyRows(rows);
    updateSelection();
    Q_EMIT statsChanged();
    Q_EMIT refreshed();
}

// Rows are slots: the list grows or shrinks at its end and every slot whose
// content differs is refreshed in place. The view therefore never resets,
// keeps its scroll position and only repaints what changed.
void ProcessModel::applyRows(const QList<ProcessRecord> &rows)
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

const ProcessRecord *ProcessModel::find(int pid) const
{
    for (const ProcessRecord &r : m_all) {
        if (r.pid == pid)
            return &r;
    }
    return nullptr;
}

bool ProcessModel::selectRow(int row)
{
    if (m_updating)
        return false;
    if (row < 0 || row >= m_rows.size()) {
        m_selectedPid = -1;
        m_selectedStart = 0;
    } else {
        m_selectedPid = m_rows.at(row).pid;
        m_selectedStart = m_rows.at(row).startTicks;
    }
    updateSelection();
    return true;
}

void ProcessModel::updateSelection()
{
    int row = -1;
    QString name;
    bool isProtected = false;
    bool isStopped = false;
    if (m_selectedPid > 0) {
        const ProcessRecord *record = find(m_selectedPid);
        if (!record || record->startTicks != m_selectedStart) {
            m_selectedPid = -1;     // the process exited (its PID may already belong to another one)
            m_selectedStart = 0;
        } else {
            name = record->name;
            isProtected = record->pid <= 1 || record->kernelThread;
            isStopped = record->state == 'T';
            for (int i = 0; i < m_rows.size(); ++i) {
                if (m_rows.at(i).pid == m_selectedPid) {
                    row = i;
                    break;
                }
            }
        }
    }
    const bool changed = row != m_selectedRow || name != m_selectedName || isProtected != m_selectedProtected
        || isStopped != m_selectedStopped;
    m_selectedStopped = isStopped;
    m_selectedRow = row;
    m_selectedName = name;
    m_selectedProtected = isProtected;
    if (changed)
        Q_EMIT selectionChanged();
}

QVariantMap ProcessModel::describe(int row) const
{
    if (row < 0 || row >= m_rows.size())
        return {};
    const ProcessRecord &r = m_rows.at(row);
    QString reason;
    if (r.pid <= 1)
        reason = tr("PID 1 is the init process; the whole system depends on it, so it is never touched from here.");
    else if (r.kernelThread)
        reason = tr("Kernel threads are part of the kernel and cannot be ended, suspended or re-prioritised from here.");
    return {
        {u"pid"_s, r.pid},
        {u"name"_s, r.name},
        {u"user"_s, r.user},
        {u"command"_s, r.command},
        {u"nice"_s, r.nice},
        {u"stopped"_s, r.state == 'T'},
        {u"self"_s, r.pid == int(::getpid())},
        {u"privileged"_s, m_ownUid == 0},
        {u"token"_s, QString::number(r.startTicks)},
        {u"protected"_s, !reason.isEmpty()},
        {u"protectedReason"_s, reason},
        {u"otherUser"_s, m_ownUid != 0 && r.uid != m_ownUid},
    };
}

// ---- actions ---------------------------------------------------------------------

namespace {

struct SignalAction
{
    const char *key;
    int signal;
    const char *name;       // as kill(1) spells it
};
const SignalAction kSignals[] = {
    {"end", SIGTERM, "TERM"},
    {"kill", SIGKILL, "KILL"},
    {"stop", SIGSTOP, "STOP"},
    {"continue", SIGCONT, "CONT"},
};

const SignalAction *signalAction(const QString &key)
{
    for (const SignalAction &action : kSignals) {
        if (key == QLatin1StringView(action.key))
            return &action;
    }
    return nullptr;
}

} // namespace

// Why nothing may be done to 'pid' right now — empty if it may. 'outcome' is
// set to "gone" or "refused" accordingly.
QString ProcessModel::refusal(int pid, const QString &token, const QString &name, const QString &action,
                              QString *outcome) const
{
    *outcome = u"refused"_s;
    if (pid <= 1)
        return tr("PID 1 is the init process and is never touched.");
    const ProcessReader::Identity identity = ProcessReader::identify(pid);
    if (!identity.exists || QString::number(identity.startTicks) != token) {
        *outcome = u"gone"_s;
        return tr("%1 (PID %2) has already exited.").arg(name).arg(pid);
    }
    if (identity.kernelThread)
        return tr("%1 is a kernel thread and cannot be controlled from here.").arg(name);
    if (action == "stop"_L1 && pid == int(::getpid()))
        return tr("System Monitor cannot suspend itself: nothing could resume it from here.");
    outcome->clear();
    return {};
}

void ProcessModel::sendSignal(int pid, const QString &token, const QString &action, bool asAdministrator)
{
    const ProcessRecord *record = find(pid);
    const QString name = record ? record->name : tr("process");
    const auto finish = [this, pid, name, action](const QString &outcome, const QString &message) {
        Q_EMIT actionFinished(pid, name, action, outcome, message);
    };

    const SignalAction *what = signalAction(action);
    if (!what) {
        finish(u"failed"_s, tr("“%1” is not something that can be done to a process.").arg(action));
        return;
    }
    QString outcome;
    const QString reason = refusal(pid, token, name, action, &outcome);
    if (!reason.isEmpty()) {
        finish(outcome, reason);
        return;
    }

    const QString done = action == "kill"_L1 ? tr("%1 (PID %2) was killed.").arg(name).arg(pid)
        : action == "stop"_L1 ? tr("%1 (PID %2) was suspended.").arg(name).arg(pid)
        : action == "continue"_L1 ? tr("%1 (PID %2) continues to run.").arg(name).arg(pid)
        : tr("%1 (PID %2) was asked to quit.").arg(name).arg(pid);

    const int error = deliverSignal(pid, token.toULongLong(), what->signal);
    if (error == 0) {
        finish(u"done"_s, done);
        QTimer::singleShot(250, this, &ProcessModel::refresh);
    } else if (error == ESRCH) {
        finish(u"gone"_s, tr("%1 (PID %2) has already exited.").arg(name).arg(pid));
    } else if (error == EPERM) {
        if (asAdministrator) {
            runAsAdministrator(pid, name, action, QString::fromLatin1(kKill),
                               {u"-s"_s, QString::fromLatin1(what->name), QString::number(pid)}, done);
        } else {
            finish(u"needs-administrator"_s, tr("You are not permitted to signal %1 (PID %2).").arg(name).arg(pid));
        }
    } else {
        finish(u"failed"_s, tr("%1 (PID %2) could not be signalled: %3")
                                .arg(name).arg(pid).arg(QString::fromLocal8Bit(std::strerror(error))));
    }
}

void ProcessModel::setPriority(int pid, const QString &token, int nice, bool asAdministrator)
{
    const QString action = u"renice"_s;
    const ProcessRecord *record = find(pid);
    const QString name = record ? record->name : tr("process");
    const auto finish = [this, pid, name, action](const QString &outcome, const QString &message) {
        Q_EMIT actionFinished(pid, name, action, outcome, message);
    };

    if (nice < -20 || nice > 19) {
        finish(u"failed"_s, tr("A nice value lies between -20 and 19."));
        return;
    }
    QString outcome;
    const QString reason = refusal(pid, token, name, action, &outcome);
    if (!reason.isEmpty()) {
        finish(outcome, reason);
        return;
    }

    const QString done = tr("%1 (PID %2) now runs with nice value %3.").arg(name).arg(pid).arg(nice);
    // Like renice(1). The kernel answers EACCES when an unprivileged caller
    // asks for a higher priority than RLIMIT_NICE allows, EPERM when the
    // process belongs to someone else.
    if (::setpriority(PRIO_PROCESS, id_t(pid), nice) == 0) {
        finish(u"done"_s, done);
        QTimer::singleShot(250, this, &ProcessModel::refresh);
        return;
    }
    const int error = errno;
    if (error == ESRCH) {
        finish(u"gone"_s, tr("%1 (PID %2) has already exited.").arg(name).arg(pid));
    } else if (error == EPERM || error == EACCES) {
        if (asAdministrator) {
            runAsAdministrator(pid, name, action, QString::fromLatin1(kRenice),
                               {u"-n"_s, QString::number(nice), u"-p"_s, QString::number(pid)}, done);
        } else {
            finish(u"needs-administrator"_s,
                   error == EACCES ? tr("Raising the priority of %1 (PID %2) needs administrator rights.").arg(name).arg(pid)
                                   : tr("You are not permitted to change the priority of %1 (PID %2).").arg(name).arg(pid));
        }
    } else {
        finish(u"failed"_s, tr("The priority of %1 (PID %2) could not be changed: %3")
                                .arg(name).arg(pid).arg(QString::fromLocal8Bit(std::strerror(error))));
    }
}

// Not permitted as this user: kill(1) or renice(1) run through pkexec, which
// makes the session's polkit agent ask for administrator authentication.
// 'program' is one of two fixed absolute paths, every argument is a keyword
// or a number formatted here, and the command is an argument vector — no shell.
void ProcessModel::runAsAdministrator(int pid, const QString &name, const QString &action, const QString &program,
                                      const QStringList &arguments, const QString &doneMessage)
{
    const auto finish = [this, pid, name, action](const QString &outcome, const QString &message) {
        Q_EMIT actionFinished(pid, name, action, outcome, message);
    };
    if (!Nexus::Command::available(u"pkexec"_s)) {
        finish(u"failed"_s, tr("pkexec is not installed, so nothing can be done to %1 as administrator.").arg(name));
        return;
    }
    if (!QFileInfo(program).isExecutable()) {
        finish(u"failed"_s, tr("%1 is missing, so nothing can be done to %2 as administrator.").arg(program, name));
        return;
    }

    // No time limit of our own: the authentication dialog has its own.
    Nexus::Command::run(this, u"pkexec"_s, QStringList{program} + arguments,
                        [this, pid, name, finish, doneMessage](const Nexus::CommandResult &r) {
        const QString detail = r.errText().trimmed().section(u'\n', 0, 0).trimmed();
        if (r.ok()) {
            finish(u"done"_s, doneMessage);
            QTimer::singleShot(250, this, &ProcessModel::refresh);
        } else if (r.exitCode == 126) {
            finish(u"cancelled"_s, tr("Authentication was dismissed; %1 is left as it was.").arg(name));
        } else if (r.exitCode == 127) {
            finish(u"failed"_s, detail.isEmpty() ? tr("Administrator authentication is not available; %1 is left as it was.").arg(name)
                                                : tr("Not authorised to act on %1: %2").arg(name, detail));
        } else if (r.exitCode == -1) {
            finish(u"failed"_s, tr("pkexec could not be started; %1 is left as it was.").arg(name));
        } else {
            finish(u"failed"_s, detail.isEmpty() ? tr("The request for %1 (PID %2) failed.").arg(name).arg(pid)
                                                : tr("The request for %1 (PID %2) failed: %3").arg(name).arg(pid).arg(detail));
        }
    }, 0);
}

void ProcessModel::copyToClipboard(const QString &text) const
{
    QGuiApplication::clipboard()->setText(text);
}
