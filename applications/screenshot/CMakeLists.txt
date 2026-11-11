# SHADOW Screenshot — capture the screen, a monitor, a window or a region;
# annotate, save, copy.
#
# KGuiAddons (KSystemClipboard) is used when present: it sets the clipboard
# through the compositor's data-control protocol, which works without a
# focused window (needed by "shadow-screenshot --clipboard"). Without it the
# application falls back to QClipboard.
find_package(KF6GuiAddons QUIET)

shadow_add_app(shadow-screenshot
    URI org.shadow.screenshot
    SOURCES
        main.cpp
        controller.cpp controller.h
        screengrabber.cpp screengrabber.h
        document.cpp document.h
        annotationview.cpp annotationview.h
    QML Main.qml RegionOverlay.qml
    DESKTOP org.shadow.screenshot.desktop
    LIBS Qt6::DBus Qt6::Concurrent
)
if(KF6GuiAddons_FOUND)
    target_link_libraries(shadow-screenshot PRIVATE KF6::GuiAddons)
    target_compile_definitions(shadow-screenshot PRIVATE SHADOW_HAVE_KGUIADDONS)
else()
    message(STATUS "shadow-screenshot: KF6GuiAddons not found — clipboard needs a focused window on Wayland")
endif()

# Test client (not installed): reads the clipboard the way any other
# application does and writes the picture it finds to a file.
qt_add_executable(shadow-screenshot-cliptest tests/cliptest.cpp)
target_link_libraries(shadow-screenshot-cliptest PRIVATE Qt6::Gui)

# Test double (not installed) of the compositor's org.kde.KWin.ScreenShot2
# interface, for environments where KWin cannot provide it; see tests/README
# section in README.md.
qt_add_executable(shadow-screenshot-fakekwin tests/fakekwin.cpp)
target_link_libraries(shadow-screenshot-fakekwin PRIVATE Qt6::Gui Qt6::DBus)
