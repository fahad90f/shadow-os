# NEXUS OS Installer — graphical front-end of the installation engine
# (../engine, package nexus-installer). See README.md.
find_package(KF6Config QUIET)

set(_components
    components/StepPage.qml components/ScrollColumn.qml components/PickList.qml components/WizardState.qml
    components/SecretFields.qml components/ChoiceCard.qml components/SectionTitle.qml components/LogView.qml)
set(_pages
    pages/WelcomePage.qml pages/KeyboardPage.qml pages/CheckPage.qml pages/TimeZonePage.qml pages/DiskPage.qml
    pages/UserPage.qml pages/SoftwarePage.qml pages/SummaryPage.qml pages/ProgressPage.qml pages/ResultPage.qml)
# One flat type namespace: pages and components are found without directory imports.
foreach(_file IN LISTS _components _pages)
    get_filename_component(_name "${_file}" NAME)
    set_source_files_properties("${_file}" PROPERTIES QT_RESOURCE_ALIAS "${_name}")
endforeach()

nexus_add_app(nexus-installer
    URI org.nexus.installer
    SOURCES main.cpp options.h secret.h
            installer.cpp installer.h
            keyboard.cpp keyboard.h
            locales.cpp locales.h
            timezones.cpp timezones.h
            validation.cpp validation.h
    QML Main.qml ${_components} ${_pages}
    DESKTOP org.nexus.installer.desktop
)

# Writing the live session's keyboard configuration (kxkbrc) needs KConfig;
# without it the keyboard step says that it cannot switch the session.
if(TARGET KF6::ConfigCore)
    target_link_libraries(nexus-installer PRIVATE KF6::ConfigCore)
    target_compile_definitions(nexus-installer PRIVATE NEXUS_HAVE_KCONFIG)
endif()

# Privilege separation: the engine is started through pkexec with this action.
install(FILES polkit/org.nexus.installer.policy DESTINATION ${CMAKE_INSTALL_DATADIR}/polkit-1/actions)
# Lets the live user run that one action without a password, in the live session only.
install(FILES polkit/49-nexus-installer-live.rules DESTINATION ${CMAKE_INSTALL_DATADIR}/polkit-1/rules.d)
