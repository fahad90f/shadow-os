# NEXUS Archive Manager — browse, extract, test, create and edit archives.
# The work is done by bsdtar (libarchive-tools) and 7z (p7zip-full/7zip),
# always run with argument vectors; see README.md for the security model.
find_package(Qt6 REQUIRED COMPONENTS Concurrent)

nexus_add_app(nexus-archive-manager
    URI org.nexus.archivemanager
    SOURCES main.cpp archivesession.cpp archivesession.h formats.cpp formats.h listing.cpp listing.h
            safemerge.cpp safemerge.h tools.cpp tools.h
    QML Main.qml Notice.qml PathPicker.qml
    DESKTOP org.nexus.archivemanager.desktop
    LIBS Qt6::Concurrent
)
