# SHADOW Assistant — a personal AI agent (chat + tool calling) backed by
# NVIDIA's OpenAI-compatible Nemotron API. No API key is ever stored here; it
# is read at run time from ~/.config/shadow/assistant.conf or the environment.
file(GLOB ASSISTANT_QML CONFIGURE_DEPENDS RELATIVE ${CMAKE_CURRENT_SOURCE_DIR}
     pages/*.qml components/*.qml)

shadow_add_app(shadow-assistant
    URI org.shadow.assistant
    SOURCES
        main.cpp
        assistant.cpp assistant.h
        assistantconfig.cpp assistantconfig.h
        chatclient.cpp chatclient.h
        conversationstore.cpp conversationstore.h
        toolregistry.cpp toolregistry.h
    QML Main.qml ${ASSISTANT_QML}
    DESKTOP org.shadow.assistant.desktop
    LIBS Qt6::Network
)
