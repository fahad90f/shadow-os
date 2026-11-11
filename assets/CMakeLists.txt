# NEXUS artwork (package nexus-artwork): wallpapers and the logo family.
# Application and category icons are installed by desktop/themes together
# with the icon theme.
file(GLOB NEXUS_WALLPAPERS CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/wallpapers/*.jpg)
install(FILES ${NEXUS_WALLPAPERS} DESTINATION ${CMAKE_INSTALL_DATADIR}/backgrounds/nexus)

file(GLOB NEXUS_LOGOS CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/logos/*.svg)
install(FILES ${NEXUS_LOGOS} DESTINATION ${CMAKE_INSTALL_DATADIR}/nexus/logos)

# The distributor logo under the names other software looks for.
install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/logos/nexus-symbol.svg
        DESTINATION ${CMAKE_INSTALL_DATADIR}/pixmaps RENAME nexus-os.svg)
install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/logos/nexus-symbol.svg
        DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps RENAME nexus-os-logo.svg)

# Boot branding: GRUB menu theme and Plymouth splash.
install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/boot/grub/nexus DESTINATION ${CMAKE_INSTALL_DATADIR}/grub/themes)
install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/boot/plymouth/nexus DESTINATION ${CMAKE_INSTALL_DATADIR}/plymouth/themes)
