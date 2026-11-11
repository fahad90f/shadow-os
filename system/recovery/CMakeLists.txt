# SHADOW Recovery — repair an installed Shadow OS that no longer boots or
# updates. Two parts, one package (shadow-recovery):
#
#   engine/   shadow-recovery-engine, a root command-line tool (Python 3,
#             standard library only) that finds, mounts and repairs an
#             installation
#   (here)    shadow-recovery, the graphical front-end; it runs unprivileged
#             and starts the engine through pkexec
#
# See README.md.
shadow_add_app(shadow-recovery
    URI org.shadow.recovery
    SOURCES main.cpp recovery.cpp recovery.h
    QML Main.qml ActionRow.qml SecretDialog.qml
    DESKTOP org.shadow.recovery.desktop
)

install(PROGRAMS engine/shadow-recovery-engine DESTINATION ${CMAKE_INSTALL_SBINDIR})
install(DIRECTORY engine/shadow_recovery DESTINATION lib/shadow-recovery
        FILES_MATCHING PATTERN "*.py" PATTERN "__pycache__" EXCLUDE)

# Privilege separation: the engine is started through pkexec with this action.
install(FILES polkit/org.shadow.recovery.policy DESTINATION ${CMAKE_INSTALL_DATADIR}/polkit-1/actions)
# Lets the live user run that one action without a password, in the live session only.
install(FILES polkit/49-shadow-recovery-live.rules DESTINATION ${CMAKE_INSTALL_DATADIR}/polkit-1/rules.d)
