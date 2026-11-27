# SHADOW Trading: market analysis by a panel of NVIDIA-hosted models plus fixed rules,
# and a paper account the resulting trades are placed on. No API key is ever
# stored here; the key is read at run time from the user's own files or the
# environment. See README.md.
file(GLOB TRADING_QML CONFIGURE_DEPENDS RELATIVE ${CMAKE_CURRENT_SOURCE_DIR}
     pages/*.qml components/*.qml components/*.js)
# The skill briefs the models are given, compiled into the binary.
file(GLOB TRADING_PROMPTS CONFIGURE_DEPENDS RELATIVE ${CMAKE_CURRENT_SOURCE_DIR} prompts/*.md)

# The shared formatting script is imported by path. Listed in the module's
# qmldir it would make the module "ambiguous" for every file that both sits in
# a subdirectory (an implicit import of the module) and imports it by name.
set_source_files_properties(components/Format.js PROPERTIES QT_QML_SKIP_QMLDIR_ENTRY TRUE)

# The live price stream is a WebSocket.
find_package(Qt6 REQUIRED COMPONENTS WebSockets)

shadow_add_app(shadow-trading
    URI org.shadow.trading
    SOURCES
        main.cpp
        chartrenderer.cpp chartrenderer.h
        chatstream.cpp chatstream.h
        deskengine.cpp deskengine.h
        deskmessages.cpp deskmessages.h
        marketdata.cpp marketdata.h
        marketstream.cpp marketstream.h
        modelclient.cpp modelclient.h
        tradesmodel.cpp tradesmodel.h
        tradingconfig.cpp tradingconfig.h
        tradingengine.cpp tradingengine.h tradingengine_sbt.cpp
        core/desk.cpp core/desk.h
        core/paper.cpp core/paper.h
        core/risk.cpp core/risk.h
        core/sbt.cpp core/sbt.h
        core/signal.cpp core/signal.h
        skills/candle.h
        skills/indicators.cpp skills/indicators.h
        skills/smc.cpp skills/smc.h
    QML Main.qml ${TRADING_QML}
    RESOURCES ${TRADING_PROMPTS}
    DESKTOP org.shadow.trading.desktop
    LIBS Qt6::Network Qt6::WebSockets
)
