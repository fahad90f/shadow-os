// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QAbstractListModel>
#include <QQmlEngine>
#include <QStringList>
#include <QWindow>

class Applications;
class PlasmaWindow;
class PlasmaWindowManagement;
class ShellSettings;

// The open top-level windows, as reported by the compositor.
class WindowModel : public QAbstractListModel
{
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("Provided by the shell as the 'windows' context property")
    Q_PROPERTY(bool available READ available NOTIFY availableChanged)
    Q_PROPERTY(int count READ count NOTIFY countChanged)
    Q_PROPERTY(QString activeUuid READ activeUuid NOTIFY activeChanged)
    Q_PROPERTY(QString activeTitle READ activeTitle NOTIFY activeChanged)
    Q_PROPERTY(QString activeAppName READ activeAppName NOTIFY activeChanged)
    Q_PROPERTY(bool showingDesktop READ showingDesktop WRITE setShowingDesktop NOTIFY showingDesktopChanged)

public:
    enum Roles { UuidRole = Qt::UserRole + 1, TitleRole, AppIdRole, DesktopIdRole, AppNameRole, IconRole,
                 ActiveRole, MinimizedRole, MaximizedRole, AttentionRole, DesktopsRole, PidRole };

    WindowModel(Applications *apps, QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    // False when the compositor does not offer the window-management
    // interface (not KWin, or the shell is not authorised) — the dock then
    // still launches applications but cannot show running ones.
    bool available() const;
    int count() const { return int(m_windows.size()); }
    QString activeUuid() const;
    QString activeTitle() const;
    QString activeAppName() const;
    bool showingDesktop() const;
    void setShowingDesktop(bool show);

    Q_INVOKABLE void activate(const QString &uuid);
    Q_INVOKABLE void close(const QString &uuid);
    Q_INVOKABLE void setMinimized(const QString &uuid, bool minimized);
    Q_INVOKABLE void toggleMaximized(const QString &uuid);
    Q_INVOKABLE void moveToDesktop(const QString &uuid, const QString &desktopId);

    // Task windows (no transient children, not skip-taskbar), most recently
    // stacked last.
    QList<PlasmaWindow *> taskWindows() const;
    PlasmaWindow *window(const QString &uuid) const;
    QString desktopIdFor(const PlasmaWindow *w) const;

Q_SIGNALS:
    void availableChanged();
    void countChanged();
    void activeChanged();
    void showingDesktopChanged();
    void tasksChanged();            // set of windows or their app assignment changed
    void taskStateChanged();        // only state/title of existing windows changed

private:
    void addWindow(PlasmaWindow *w);
    void removeWindow(PlasmaWindow *w);
    int rowOf(const PlasmaWindow *w) const;
    PlasmaWindow *activeWindow() const;

    Applications *m_apps;
    PlasmaWindowManagement *m_wm;
    QList<PlasmaWindow *> m_windows;
    QStringList m_stacking;
};

struct DockItem
{
    QString key;                 // desktop id, or "window:<app id>" for windows without a launcher
    QString desktopId, name, icon;
    QStringList windows;         // uuids, in stacking order (top last)
    bool pinned = false, active = false, attention = false;
};

// What the dock shows: pinned launchers followed by running applications
// that are not pinned, each with its windows.
class DockModel : public QAbstractListModel
{
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("Provided by the shell as the 'dock' context property")

public:
    enum Roles { KeyRole = Qt::UserRole + 1, DesktopIdRole, NameRole, IconRole, PinnedRole, RunningRole,
                 ActiveRole, AttentionRole, WindowCountRole, WindowsRole };

    DockModel(Applications *apps, WindowModel *windows, ShellSettings *settings, QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    // Primary click: launch, or raise, or (if already in front) minimise;
    // with several windows, cycle through them.
    Q_INVOKABLE void activate(int row, QWindow *origin = nullptr);
    Q_INVOKABLE void launchNew(int row, QWindow *origin = nullptr);
    Q_INVOKABLE void closeAll(int row);
    Q_INVOKABLE void setPinned(int row, bool pinned);
    Q_INVOKABLE void move(int from, int to);            // reorder pinned launchers
    Q_INVOKABLE QVariantList windowsOf(int row) const;  // [{uuid, title, active, minimized}]

private:
    void rebuild(bool structural);

    Applications *m_apps;
    WindowModel *m_windows;
    ShellSettings *m_settings;
    QList<DockItem> m_items;
};
