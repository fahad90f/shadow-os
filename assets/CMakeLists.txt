# SHADOW artwork (package shadow-artwork): wallpapers and the logo family.
# Application and category icons are installed by desktop/themes together
# with the icon theme.
file(GLOB SHADOW_WALLPAPERS CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/wallpapers/*.jpg)
install(FILES ${SHADOW_WALLPAPERS} DESTINATION ${CMAKE_INSTALL_DATADIR}/backgrounds/shadow)

file(GLOB SHADOW_LOGOS CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/logos/*.svg)
install(FILES ${SHADOW_LOGOS} DESTINATION ${CMAKE_INSTALL_DATADIR}/shadow/logos)

# The distributor logo under the names other software looks for.
install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/logos/shadow-symbol.svg
        DESTINATION ${CMAKE_INSTALL_DATADIR}/pixmaps RENAME shadow-os.svg)
install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/logos/shadow-symbol.svg
        DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps RENAME shadow-os-logo.svg)

# Boot branding: GRUB menu theme and Plymouth splash.
install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/boot/grub/shadow DESTINATION ${CMAKE_INSTALL_DATADIR}/grub/themes)
install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/boot/plymouth/shadow DESTINATION ${CMAKE_INSTALL_DATADIR}/plymouth/themes)
