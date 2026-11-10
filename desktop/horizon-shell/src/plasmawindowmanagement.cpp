// SPDX-License-Identifier: GPL-3.0-or-later
#include "plasmawindowmanagement.h"

#include <wayland-client-core.h>

PlasmaWindow::PlasmaWindow(struct ::org_kde_plasma_window *object, const QString &uuid, QObject *parent)
    : QObject(parent), QtWayland::org_kde_plasma_window(object), m_uuid(uuid)
{
}

PlasmaWindow::~PlasmaWindow()
{
    if (isInitialized())
        destroy();
}

void PlasmaWindow::org_kde_plasma_window_title_changed(const QString &title)
{
    if (m_title != title) { m_title = title; Q_EMIT changed(); }
}

void PlasmaWindow::org_kde_plasma_window_app_id_changed(const QString &app_id)
{
    if (m_appId != app_id) { m_appId = app_id; Q_EMIT changed(); }
}

void PlasmaWindow::org_kde_plasma_window_state_changed(uint32_t flags)
{
    if (m_state != flags) { m_state = flags; Q_EMIT changed(); }
}

void PlasmaWindow::org_kde_plasma_window_themed_icon_name_changed(const QString &name)
{
    if (m_themedIcon != name) { m_themedIcon = name; Q_EMIT changed(); }
}

void PlasmaWindow::org_kde_plasma_window_unmapped()
{
    Q_EMIT closed();
}

void PlasmaWindow::org_kde_plasma_window_initial_state()
{
    if (!m_ready) { m_ready = true; Q_EMIT becameReady(); }
}

void PlasmaWindow::org_kde_plasma_window_parent_window(struct ::org_kde_plasma_window *parent)
{
    const bool has = parent != nullptr;
    if (m_hasParent != has) { m_hasParent = has; Q_EMIT changed(); }
}

void PlasmaWindow::org_kde_plasma_window_pid_changed(uint32_t pid)
{
    m_pid = pid;
}

void PlasmaWindow::org_kde_plasma_window_virtual_desktop_entered(const QString &id)
{
    if (!m_desktops.contains(id)) { m_desktops.append(id); Q_EMIT changed(); }
}

void PlasmaWindow::org_kde_plasma_window_virtual_desktop_left(const QString &id)
{
    if (m_desktops.removeAll(id) > 0) Q_EMIT changed();
}

void PlasmaWindow::org_kde_plasma_window_resource_name_changed(const QString &resource_name)
{
    if (m_resourceName != resource_name) { m_resourceName = resource_name; Q_EMIT changed(); }
}

// Version 16: everything the shell uses (uuid-based windows, virtual desktop
// ids, resource names) without depending on the newest additions.
PlasmaWindowManagement::PlasmaWindowManagement(QObject *parent)
    : QWaylandClientExtensionTemplate<PlasmaWindowManagement>(16)
{
    setParent(parent);
    initialize();
}

PlasmaWindowManagement::~PlasmaWindowManagement()
{
    if (isInitialized())
        wl_proxy_destroy(reinterpret_cast<wl_proxy *>(object()));
}

void PlasmaWindowManagement::setShowingDesktop(bool show)
{
    if (isInitialized())
        show_desktop(show ? show_desktop_enabled : show_desktop_disabled);
}

void PlasmaWindowManagement::org_kde_plasma_window_management_window_with_uuid(uint32_t, const QString &uuid)
{
    auto *window = new PlasmaWindow(get_window_by_uuid(uuid), uuid, this);
    Q_EMIT windowAdded(window);
}

void PlasmaWindowManagement::org_kde_plasma_window_management_stacking_order_uuid_changed(const QString &uuids)
{
    Q_EMIT stackingOrderChanged(uuids.split(QLatin1Char(';'), Qt::SkipEmptyParts));
}

void PlasmaWindowManagement::org_kde_plasma_window_management_show_desktop_changed(uint32_t state)
{
    const bool showing = state == show_desktop_enabled;
    if (m_showingDesktop != showing) { m_showingDesktop = showing; Q_EMIT showingDesktopChanged(); }
}
