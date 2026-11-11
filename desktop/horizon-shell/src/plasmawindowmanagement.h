// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "qwayland-plasma-window-management.h"

#include <QObject>
#include <QStringList>
#include <QtWaylandClient/QWaylandClientExtension>

// Client side of KWin's org_kde_plasma_window_management protocol: the list of
// top-level windows with their state, and requests to activate, close,
// minimise and move them between virtual desktops.
//
// KWin only offers this global to clients it trusts: the shell's desktop file
// lists the interface in X-KDE-Wayland-Interfaces
// (org.shadow.horizon.shell.desktop).

class PlasmaWindow : public QObject, public QtWayland::org_kde_plasma_window
{
    Q_OBJECT
public:
    // Bits of org_kde_plasma_window_management.state
    enum State : quint32 {
        Active = 0x1, Minimized = 0x2, Maximized = 0x4, Fullscreen = 0x8,
        KeepAbove = 0x10, OnAllDesktops = 0x40, DemandsAttention = 0x80,
        Closeable = 0x100, Minimizable = 0x200, Maximizable = 0x400,
        SkipTaskbar = 0x1000, SkipSwitcher = 0x40000,
    };

    PlasmaWindow(struct ::org_kde_plasma_window *object, const QString &uuid, QObject *parent);
    ~PlasmaWindow() override;

    QString uuid() const { return m_uuid; }
    QString title() const { return m_title; }
    QString appId() const { return m_appId; }
    QString themedIcon() const { return m_themedIcon; }
    QString resourceName() const { return m_resourceName; }
    quint32 state() const { return m_state; }
    quint32 pid() const { return m_pid; }
    QStringList desktops() const { return m_desktops; }
    bool ready() const { return m_ready; }
    bool hasParent() const { return m_hasParent; }
    bool is(State s) const { return m_state & s; }

    void requestState(quint32 flags, quint32 state) { set_state(flags, state); }

Q_SIGNALS:
    void changed();
    void becameReady();
    void closed();

protected:
    void org_kde_plasma_window_title_changed(const QString &title) override;
    void org_kde_plasma_window_app_id_changed(const QString &app_id) override;
    void org_kde_plasma_window_state_changed(uint32_t flags) override;
    void org_kde_plasma_window_themed_icon_name_changed(const QString &name) override;
    void org_kde_plasma_window_unmapped() override;
    void org_kde_plasma_window_initial_state() override;
    void org_kde_plasma_window_parent_window(struct ::org_kde_plasma_window *parent) override;
    void org_kde_plasma_window_pid_changed(uint32_t pid) override;
    void org_kde_plasma_window_virtual_desktop_entered(const QString &id) override;
    void org_kde_plasma_window_virtual_desktop_left(const QString &id) override;
    void org_kde_plasma_window_resource_name_changed(const QString &resource_name) override;

private:
    QString m_uuid, m_title, m_appId, m_themedIcon, m_resourceName;
    quint32 m_state = 0, m_pid = 0;
    QStringList m_desktops;
    bool m_ready = false, m_hasParent = false;
};

class PlasmaWindowManagement : public QWaylandClientExtensionTemplate<PlasmaWindowManagement>,
                               public QtWayland::org_kde_plasma_window_management
{
    Q_OBJECT
public:
    explicit PlasmaWindowManagement(QObject *parent = nullptr);
    ~PlasmaWindowManagement() override;

    void setShowingDesktop(bool show);
    bool showingDesktop() const { return m_showingDesktop; }

Q_SIGNALS:
    void windowAdded(PlasmaWindow *window);
    void stackingOrderChanged(const QStringList &uuids);
    void showingDesktopChanged();

protected:
    void org_kde_plasma_window_management_window_with_uuid(uint32_t id, const QString &uuid) override;
    void org_kde_plasma_window_management_stacking_order_uuid_changed(const QString &uuids) override;
    void org_kde_plasma_window_management_show_desktop_changed(uint32_t state) override;

private:
    bool m_showingDesktop = false;
};
