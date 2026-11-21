# SHADOW Assistant — a personal AI agent (chat + tool calling) backed by
# NVIDIA's hosted models working as a team (also: the Anthropic Claude Messages
# API, and an OpenAI-compatible local/offline mode). No API key is ever stored
# here; it is read at run time from the user's own files or the environment.
file(GLOB ASSISTANT_QML CONFIGURE_DEPENDS RELATIVE ${CMAKE_CURRENT_SOURCE_DIR}
     pages/*.qml components/*.qml components/*.js)
# The design tokens: one shared instance (import org.shadow.assistant; Pe.ink).
# At the module root, where the generated qmldir declares it a singleton.
set_source_files_properties(Pe.qml PROPERTIES QT_QML_SINGLETON_TYPE TRUE)

shadow_add_app(shadow-assistant
    URI org.shadow.assistant
    SOURCES
        main.cpp
        assistant.cpp assistant.h
        assistantconfig.cpp assistantconfig.h
        chatclient.cpp chatclient.h
        conversationstore.cpp conversationstore.h
        council.cpp council.h
        visionteam.cpp visionteam.h
        modelcatalog.cpp modelcatalog.h
        toolregistry.cpp toolregistry.h
        undojournal.cpp undojournal.h
        speechtext.cpp speechtext.h
        voicecontroller.cpp voicecontroller.h
        voiceengine.cpp voiceengine.h
        voicesession.cpp voicesession.h
    QML Main.qml Pe.qml ${ASSISTANT_QML}
    DESKTOP org.shadow.assistant.desktop
    LIBS Qt6::Network
)

# Listen for the wake word from login on (exits at once when it is switched off).
install(FILES org.shadow.assistant-listen.desktop DESTINATION ${CMAKE_INSTALL_FULL_SYSCONFDIR}/xdg/autostart)

# The offline voice engine helper (shadow-voiced) and its setup tool.
add_subdirectory(voice)
