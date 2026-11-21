# shadow-voiced (offline voice engine helper, Python 3) and shadow-voice-setup (bash).
#
# Installed with the assistant's component (shadow-assistant):
#   <prefix>/lib/shadow-assistant/voice/shadow-voiced            the helper (the app runs it)
#   <prefix>/lib/shadow-assistant/voice/shadow_voice/*.py         its package
#   <prefix>/bin/shadow-voice-setup                               downloads the engine data
#
# The helper is architecture-independent Python and is looked up by the app at
# <applicationDirPath>/../lib/shadow-assistant/voice, so it goes to "lib", not to the
# multiarch CMAKE_INSTALL_LIBDIR (lib/x86_64-linux-gnu). The ~700 MB of models and the
# sherpa-onnx runtime are NOT part of the build: shadow-voice-setup installs them.
set(SHADOW_VOICE_DIR "lib/shadow-assistant/voice")

file(GLOB SHADOW_VOICE_PY CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/shadow_voice/*.py")

# Uninstalled runs: <build>/bin/shadow-assistant looks for the helper at
# <applicationDirPath>/../lib/shadow-assistant/voice/shadow-voiced, so mirror that in the
# build tree with a symlink to the source (the helper resolves its package via realpath).
file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/${SHADOW_VOICE_DIR}")
file(CREATE_LINK "${CMAKE_CURRENT_SOURCE_DIR}/shadow-voiced" "${CMAKE_BINARY_DIR}/${SHADOW_VOICE_DIR}/shadow-voiced"
     SYMBOLIC)

install(PROGRAMS shadow-voiced DESTINATION ${SHADOW_VOICE_DIR})
install(FILES ${SHADOW_VOICE_PY} DESTINATION ${SHADOW_VOICE_DIR}/shadow_voice)
install(PROGRAMS shadow-voice-setup DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES THIRD-PARTY.md DESTINATION ${CMAKE_INSTALL_DATADIR}/doc/shadow-assistant
        RENAME voice-engine-THIRD-PARTY.md)

# Unit and protocol tests (Python unittest; need an installed engine data root):
#   applications/assistant/voice/tests/run-tests.sh
