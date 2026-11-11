# NEXUS Recovery — repair an installed NEXUS OS that no longer boots or
# updates. Two parts, one package (nexus-recovery):
#
#   engine/   nexus-recovery-engine, a root command-line tool (Python 3,
#             standard library only) that finds, mounts and repairs an
#             installation
#   (here)    nexus-recovery, the graphical front-end; it runs unprivileged
#             and starts the engine through pkexec
#
# See README.md.
nexus_add_app(nexus-recovery
    URI org.nexus.recovery
    SOURCES main.cpp recovery.cpp recovery.h
    QML Main.qml ActionRow.qml SecretDialog.qml
    DESKTOP org.nexus.recovery.desktop
)

install(PROGRAMS engine/nexus-recovery-engine DESTINATION ${CMAKE_INSTALL_SBINDIR})
install(DIRECTORY engine/nexus_recovery DESTINATION lib/nexus-recovery
        FILES_MATCHING PATTERN "*.py" PATTERN "__pycache__" EXCLUDE)

# Privilege separation: the engine is started through pkexec with this action.
install(FILES polkit/org.nexus.recovery.policy DESTINATION ${CMAKE_INSTALL_DATADIR}/polkit-1/actions)
# Lets the live user run that one action without a password, in the live session only.
install(FILES polkit/49-nexus-recovery-live.rules DESTINATION ${CMAKE_INSTALL_DATADIR}/polkit-1/rules.d)
