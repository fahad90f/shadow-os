# SPDX-License-Identifier: GPL-3.0-or-later
#
# SHADOW themes (Debian package shadow-themes): the SHADOW look outside SHADOW's
# own applications, and the login and lock screens.
#
# Data only — install() rules, nothing is compiled, so this file is also
# configured without a compiler (-DSHADOW_DATA_ONLY=ON). Token-derived files
# are committed next to their templates and installed as they are;
# regenerate them with tools/generate.py after changing a template or the
# tokens (tools/generate.py --check fails when one is stale).
#
# Not installed here: wallpapers and logos (assets/CMakeLists.txt,
# shadow-artwork), the tokens, templates, tools and tests.

set(_xdg "${CMAKE_INSTALL_FULL_SYSCONFDIR}/xdg")

# ---- Qt / KDE Frameworks applications, KWin decorations ---------------------------
file(GLOB _shadow_color_schemes CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/colorschemes/Shadow*.colors")
install(FILES ${_shadow_color_schemes} DESTINATION "${CMAKE_INSTALL_DATADIR}/color-schemes")
install(FILES defaults/kdeglobals DESTINATION "${_xdg}")

# ---- GTK 3 / GTK 4 / libadwaita ------------------------------------------------------
install(FILES defaults/gtk-3.0/settings.ini DESTINATION "${_xdg}/gtk-3.0")
install(FILES defaults/gtk-4.0/settings.ini DESTINATION "${_xdg}/gtk-4.0")
install(FILES defaults/90_shadow-desktop.gschema.override DESTINATION "${CMAKE_INSTALL_DATADIR}/glib-2.0/schemas")

# ---- appearance switch (dark/light, accent) for everything above -----------------
install(PROGRAMS tools/shadow-apply-appearance DESTINATION "${CMAKE_INSTALL_BINDIR}")

# ---- icon themes -------------------------------------------------------------------------
install(DIRECTORY icons/shadow icons/shadow-light DESTINATION "${CMAKE_INSTALL_DATADIR}/icons"
        FILES_MATCHING PATTERN "*.svg" PATTERN "index.theme")
# Application and category icons go to hicolor so that every icon theme and
# every toolkit finds them.
file(GLOB _shadow_app_icons CONFIGURE_DEPENDS
     "${PROJECT_SOURCE_DIR}/assets/icons/apps/org.shadow.*.svg"
     "${PROJECT_SOURCE_DIR}/assets/icons/apps/shadow-os.svg")
install(FILES ${_shadow_app_icons} DESTINATION "${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps")
file(GLOB _shadow_category_icons CONFIGURE_DEPENDS "${PROJECT_SOURCE_DIR}/assets/icons/categories/*.svg")
install(FILES ${_shadow_category_icons} DESTINATION "${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/categories")

# ---- login screen (SDDM) -----------------------------------------------------------------
install(DIRECTORY sddm/shadow DESTINATION "${CMAKE_INSTALL_DATADIR}/sddm/themes")
# Distribution defaults of SDDM live below /usr/lib; /etc/sddm.conf.d stays
# free for the administrator and overrides this file.
install(FILES sddm/10-shadow.conf DESTINATION "${CMAKE_INSTALL_PREFIX}/lib/sddm/sddm.conf.d")

# ---- lock screen (KScreenLocker greeter) -----------------------------------------------
install(DIRECTORY lockscreen/org.shadow.horizon DESTINATION "${CMAKE_INSTALL_DATADIR}/plasma/shells")
install(FILES lockscreen/plasmashellrc DESTINATION "${_xdg}")
install(FILES lockscreen/pam.d/kde lockscreen/pam.d/kde-fingerprint lockscreen/pam.d/kde-smartcard
        DESTINATION "${CMAKE_INSTALL_FULL_SYSCONFDIR}/pam.d")
