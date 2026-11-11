# SPDX-License-Identifier: GPL-3.0-or-later
#
# Build additions of the "power" shell module (idle actions, low-battery and
# update notices). Included by the shell's CMakeLists.txt after the
# horizon-shell target exists.
#
#   KF6::IdleTime   idle detection (ext-idle-notify-v1 on Wayland)
#   dpms.xml        KWin's org_kde_kwin_dpms protocol (plasma-wayland-protocols),
#                   to switch the screens off and on
#
# UPower, logind, NetworkManager and the notification service are reached
# with plain QtDBus (already linked through Nexus::App).
find_package(KF6IdleTime REQUIRED)

qt6_generate_wayland_protocol_client_sources(horizon-shell
    FILES ${PLASMA_WAYLAND_PROTOCOLS_DIR}/dpms.xml)

target_link_libraries(horizon-shell PRIVATE KF6::IdleTime)
