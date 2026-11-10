// SPDX-License-Identifier: GPL-3.0-or-later
#include "tasks.h"

#include "applications.h"
#include "plasmawindowmanagement.h"
#include "shellsettings.h"

#include <QWindow>

// ============================================================ WindowModel

WindowModel::WindowModel(Applications *apps, QObject *parent)
    : QAbstractListModel(parent), m_apps(apps), m_wm(new PlasmaWindowManagement(this))
{
    connect(m_wm, &PlasmaWindowManagement::activeChanged, this, &WindowModel::availableChanged);
    connect(m_wm, &PlasmaWindowManagement::windowAdded, this, &WindowModel::addWindow);
    connect(m_wm, &PlasmaWindowManagement::showingDesktopChanged, this, &WindowModel::showingDesktopChanged);
    connect(m_wm, &PlasmaWindowManagement::stackingOrderChanged, this, [this](const QStringList &uuids) {
        m_stacking = uuids;
        Q_EMIT taskStateChanged();
    });
    connect(m_apps, &Applications::reloaded, this, [this] {
        if (!m_windows.isEmpty()) {
            Q_EMIT dataChanged(index(0), index(int(m_windows.size()) - 1), {DesktopIdRole, AppNameRole, IconRole});
            Q_EMIT tasksChanged();
        }
    });
}

bool WindowModel::available() const
{
    return m_wm->isActive();
}

bool WindowModel::showingDesktop() const
{
    return m_wm->showingDesktop();
}

void WindowModel::setShowingDesktop(bool show)
{
    m_wm->setShowingDesktop(show);
}

int WindowModel::rowOf(const PlasmaWindow *w) const
{
    return int(m_windows.indexOf(const_cast<PlasmaWindow *>(w)));
}

void WindowModel::addWindow(PlasmaWindow *w)
{
    // A window joins the model once the compositor has sent its initial state.
    connect(w, &PlasmaWindow::becameReady, this, [this, w] {
        beginInsertRows({}, int(m_windows.size()), int(m_windows.size()));
        m_windows.append(w);
        endInsertRows();
        Q_EMIT countChanged();
        Q_EMIT tasksChanged();
        if (w->is(PlasmaWindow::Active))
            Q_EMIT activeChanged();
    });
    connect(w, &PlasmaWindow::changed, this, [this, w] {
        const int row = rowOf(w);
        if (row < 0)
            return;
        Q_EMIT dataChanged(index(row), index(row));
        Q_EMIT activeChanged();
        Q_EMIT taskStateChanged();
    });
    connect(w, &PlasmaWindow::closed, this, [this, w] { removeWindow(w); });
}

void WindowModel::removeWindow(PlasmaWindow *w)
{
    const int row = rowOf(w);
    if (row >= 0) {
        beginRemoveRows({}, row, row);
        m_windows.removeAt(row);
        endRemoveRows();
        Q_EMIT countChanged();
        Q_EMIT activeChanged();
        Q_EMIT tasksChanged();
    }
    w->deleteLater();
}

PlasmaWindow *WindowModel::window(const QString &uuid) const
{
    for (PlasmaWindow *w : m_windows) {
        if (w->uuid() == uuid)
            return w;
    }
    return nullptr;
}

PlasmaWindow *WindowModel::activeWindow() const
{
    for (PlasmaWindow *w : m_windows) {
        if (w->is(PlasmaWindow::Active))
            return w;
    }
    return nullptr;
}

QString WindowModel::desktopIdFor(const PlasmaWindow *w) const
{
    return m_apps->desktopIdForAppId(w->appId(), w->resourceName());
}

QString WindowModel::activeUuid() const
{
    const PlasmaWindow *w = activeWindow();
    return w ? w->uuid() : QString();
}

QString WindowModel::activeTitle() const
{
    const PlasmaWindow *w = activeWindow();
    return w ? w->title() : QString();
}

QString WindowModel::activeAppName() const
{
    const PlasmaWindow *w = activeWindow();
    if (!w)
        return {};
    const AppEntry *e = m_apps->entry(desktopIdFor(w));
    return e ? e->name : w->appId();
}

QList<PlasmaWindow *> WindowModel::taskWindows() const
{
    QList<PlasmaWindow *> out;
    for (PlasmaWindow *w : m_windows) {
        if (!w->is(PlasmaWindow::SkipTaskbar) && !w->hasParent())
            out.append(w);
    }
    // Stacking order (bottom → top) where known; unknown windows keep model order.
    std::stable_sort(out.begin(), out.end(), [this](PlasmaWindow *a, PlasmaWindow *b) {
        return m_stacking.indexOf(a->uuid()) < m_stacking.indexOf(b->uuid());
    });
    return out;
}

int WindowModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : int(m_windows.size());
}

QVariant WindowModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_windows.size())
        return {};
    const PlasmaWindow *w = m_windows.at(index.row());
    switch (role) {
    case UuidRole: return w->uuid();
    case TitleRole: case Qt::DisplayRole: return w->title();
    case AppIdRole: return w->appId();
    case DesktopIdRole: return desktopIdFor(w);
    case AppNameRole: {
        const AppEntry *e = m_apps->entry(desktopIdFor(w));
        return e ? e->name : w->appId();
    }
    case IconRole: {
        const AppEntry *e = m_apps->entry(desktopIdFor(w));
        if (e && !e->icon.isEmpty())
            return e->icon;
        return w->themedIcon().isEmpty() ? w->appId() : w->themedIcon();
    }
    case ActiveRole: return w->is(PlasmaWindow::Active);
    case MinimizedRole: return w->is(PlasmaWindow::Minimized);
    case MaximizedRole: return w->is(PlasmaWindow::Maximized);
    case AttentionRole: return w->is(PlasmaWindow::DemandsAttention);
    case DesktopsRole: return w->desktops();
    case PidRole: return w->pid();
    }
    return {};
}

QHash<int, QByteArray> WindowModel::roleNames() const
{
    return {{UuidRole, "uuid"}, {TitleRole, "title"}, {AppIdRole, "appId"}, {DesktopIdRole, "desktopId"},
            {AppNameRole, "appName"}, {IconRole, "icon"}, {ActiveRole, "active"}, {MinimizedRole, "minimized"},
            {MaximizedRole, "maximized"}, {AttentionRole, "attention"}, {DesktopsRole, "desktops"}, {PidRole, "pid"}};
}

void WindowModel::activate(const QString &uuid)
{
    if (PlasmaWindow *w = window(uuid))
        w->requestState(PlasmaWindow::Active, PlasmaWindow::Active);
}

void WindowModel::close(const QString &uuid)
{
    if (PlasmaWindow *w = window(uuid))
        w->close();
}

void WindowModel::setMinimized(const QString &uuid, bool minimized)
{
    if (PlasmaWindow *w = window(uuid))
        w->requestState(PlasmaWindow::Minimized, minimized ? PlasmaWindow::Minimized : 0);
}

void WindowModel::toggleMaximized(const QString &uuid)
{
    if (PlasmaWindow *w = window(uuid))
        w->requestState(PlasmaWindow::Maximized, w->is(PlasmaWindow::Maximized) ? 0 : PlasmaWindow::Maximized);
}

void WindowModel::moveToDesktop(const QString &uuid, const QString &desktopId)
{
    PlasmaWindow *w = window(uuid);
    if (!w)
        return;
    const QStringList current = w->desktops();
    w->request_enter_virtual_desktop(desktopId);
    for (const QString &d : current) {
        if (d != desktopId)
            w->request_leave_virtual_desktop(d);
    }
}

// ============================================================== DockModel

DockModel::DockModel(Applications *apps, WindowModel *windows, ShellSettings *settings, QObject *parent)
    : QAbstractListModel(parent), m_apps(apps), m_windows(windows), m_settings(settings)
{
    connect(m_windows, &WindowModel::tasksChanged, this, [this] { rebuild(true); });
    connect(m_windows, &WindowModel::taskStateChanged, this, [this] { rebuild(false); });
    connect(m_settings, &ShellSettings::dockPinnedChanged, this, [this] { rebuild(true); });
    connect(m_apps, &Applications::reloaded, this, [this] { rebuild(true); });
    rebuild(true);
}

void DockModel::rebuild(bool structural)
{
    QList<DockItem> items;
    QHash<QString, int> byKey;

    const QStringList pinned = m_settings->dockPinned();
    for (const QString &id : pinned) {
        const AppEntry *e = m_apps->entry(id);
        if (!e)
            continue;                       // pinned application is not installed (any more)
        DockItem item;
        item.key = item.desktopId = id;
        item.name = e->name;
        item.icon = e->icon;
        item.pinned = true;
        byKey.insert(item.key, int(items.size()));
        items.append(item);
    }
    const QList<PlasmaWindow *> windows = m_windows->taskWindows();
    for (const PlasmaWindow *w : windows) {
        const QString desktopId = m_windows->desktopIdFor(w);
        const QString key = desktopId.isEmpty() ? QStringLiteral("window:") + w->appId() : desktopId;
        auto it = byKey.constFind(key);
        if (it == byKey.constEnd()) {
            DockItem item;
            item.key = key;
            item.desktopId = desktopId;
            const AppEntry *e = m_apps->entry(desktopId);
            item.name = e ? e->name : (w->appId().isEmpty() ? w->title() : w->appId());
            item.icon = e && !e->icon.isEmpty() ? e->icon : (w->themedIcon().isEmpty() ? w->appId() : w->themedIcon());
            byKey.insert(key, int(items.size()));
            items.append(item);
            it = byKey.constFind(key);
        }
        DockItem &item = items[*it];
        item.windows.append(w->uuid());
        item.active = item.active || w->is(PlasmaWindow::Active);
        item.attention = item.attention || w->is(PlasmaWindow::DemandsAttention);
    }

    bool sameShape = !structural || items.size() == m_items.size();
    if (sameShape && items.size() == m_items.size()) {
        for (int i = 0; i < items.size(); ++i) {
            if (items.at(i).key != m_items.at(i).key) { sameShape = false; break; }
        }
    } else {
        sameShape = false;
    }
    if (sameShape) {
        // Same entries in the same order: update in place so delegates (and
        // their hover/animation state) survive.
        m_items = items;
        if (!m_items.isEmpty())
            Q_EMIT dataChanged(index(0), index(int(m_items.size()) - 1));
        return;
    }
    beginResetModel();
    m_items = items;
    endResetModel();
}

int DockModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : int(m_items.size());
}

QVariant DockModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_items.size())
        return {};
    const DockItem &item = m_items.at(index.row());
    switch (role) {
    case KeyRole: return item.key;
    case DesktopIdRole: return item.desktopId;
    case NameRole: case Qt::DisplayRole: return item.name;
    case IconRole: return item.icon;
    case PinnedRole: return item.pinned;
    case RunningRole: return !item.windows.isEmpty();
    case ActiveRole: return item.active;
    case AttentionRole: return item.attention;
    case WindowCountRole: return item.windows.size();
    case WindowsRole: return item.windows;
    }
    return {};
}

QHash<int, QByteArray> DockModel::roleNames() const
{
    return {{KeyRole, "key"}, {DesktopIdRole, "desktopId"}, {NameRole, "name"}, {IconRole, "icon"},
            {PinnedRole, "pinned"}, {RunningRole, "running"}, {ActiveRole, "active"},
            {AttentionRole, "attention"}, {WindowCountRole, "windowCount"}, {WindowsRole, "windows"}};
}

void DockModel::activate(int row, QWindow *origin)
{
    if (row < 0 || row >= m_items.size())
        return;
    const DockItem &item = m_items.at(row);
    if (item.windows.isEmpty()) {
        m_apps->launch(item.desktopId, origin);
        return;
    }
    const QString top = item.windows.last();
    if (item.windows.size() == 1) {
        const PlasmaWindow *w = m_windows->window(top);
        if (w && w->is(PlasmaWindow::Active))
            m_windows->setMinimized(top, true);
        else
            m_windows->activate(top);
        return;
    }
    // Several windows: if one of them is in front, bring the next one up
    // (lowest in the stack first, so repeated clicks cycle through all).
    m_windows->activate(item.active ? item.windows.first() : top);
}

void DockModel::launchNew(int row, QWindow *origin)
{
    if (row >= 0 && row < m_items.size() && !m_items.at(row).desktopId.isEmpty())
        m_apps->launch(m_items.at(row).desktopId, origin);
}

void DockModel::closeAll(int row)
{
    if (row < 0 || row >= m_items.size())
        return;
    const QStringList windows = m_items.at(row).windows;
    for (const QString &uuid : windows)
        m_windows->close(uuid);
}

void DockModel::setPinned(int row, bool pinned)
{
    if (row < 0 || row >= m_items.size() || m_items.at(row).desktopId.isEmpty())
        return;
    QStringList list = m_settings->dockPinned();
    const QString id = m_items.at(row).desktopId;
    if (pinned && !list.contains(id))
        list.append(id);
    else if (!pinned)
        list.removeAll(id);
    m_settings->setDockPinned(list);
}

void DockModel::move(int from, int to)
{
    if (from < 0 || to < 0 || from >= m_items.size() || to >= m_items.size() || from == to)
        return;
    if (!m_items.at(from).pinned || !m_items.at(to).pinned)
        return;
    QStringList list = m_settings->dockPinned();
    const int a = int(list.indexOf(m_items.at(from).desktopId)), b = int(list.indexOf(m_items.at(to).desktopId));
    if (a < 0 || b < 0)
        return;
    list.move(a, b);
    m_settings->setDockPinned(list);
}

QVariantList DockModel::windowsOf(int row) const
{
    QVariantList out;
    if (row < 0 || row >= m_items.size())
        return out;
    for (const QString &uuid : m_items.at(row).windows) {
        if (const PlasmaWindow *w = m_windows->window(uuid)) {
            out << QVariantMap{{QStringLiteral("uuid"), uuid}, {QStringLiteral("title"), w->title()},
                               {QStringLiteral("active"), w->is(PlasmaWindow::Active)},
                               {QStringLiteral("minimized"), w->is(PlasmaWindow::Minimized)}};
        }
    }
    return out;
}
