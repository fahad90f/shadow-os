# Shared PackageKit plumbing of NEXUS Store and NEXUS Updater.
#
#   include(<this file>)                      # from the application's CMakeLists.txt
#   nexus_pkcommon_qml(<variable>)            # QML components to add to the app's module
#   nexus_add_app(... QML ... ${<variable>} LIBS nexus-pkcommon)
#
# The static library target is created by whichever application is configured
# first, so each application also builds on its own (-DNEXUS_ONLY=store or
# -DNEXUS_ONLY=updater).

# Imported targets are visible per directory: look the package up in every
# directory that links nexus-pkcommon.
find_package(PackageKitQt6 REQUIRED)

if(NOT TARGET nexus-pkcommon)
    add_subdirectory(${CMAKE_CURRENT_LIST_DIR} ${CMAKE_BINARY_DIR}/applications/pkcommon)
endif()

set(NEXUS_PKCOMMON_DIR ${CMAKE_CURRENT_LIST_DIR})

# The QML components in pkcommon/qml/ are compiled into each application's own
# QML module (they only depend on org.nexus.ui and on the objects handed to
# them), so both apps render transactions, prompts and update lists alike.
function(nexus_pkcommon_qml out_var)
    file(GLOB _files CONFIGURE_DEPENDS ${NEXUS_PKCOMMON_DIR}/qml/*.qml)
    foreach(_file IN LISTS _files)
        get_filename_component(_name ${_file} NAME)
        set_source_files_properties(${_file} PROPERTIES QT_RESOURCE_ALIAS ${_name})
    endforeach()
    set(${out_var} ${_files} PARENT_SCOPE)
endfunction()
