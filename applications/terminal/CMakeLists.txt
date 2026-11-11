# SHADOW Terminal — terminal emulator with tabs, split panes, profiles and SSH.
#
# The one SHADOW application built with Qt Widgets instead of QML: the terminal
# view is QTermWidget (a QWidget), so the window around it is made of widgets
# too. Its chrome is styled by Qt style sheets that are generated from the
# design tokens at build time, which keeps it in the same visual language as
# the QML applications. See README.md and docs/applications/development.md.

find_package(Qt6 REQUIRED COMPONENTS Widgets Svg)
find_package(qtermwidget6 REQUIRED)

# Debian's libqtermwidget-dev lists the libutf8proc.so development symlink in
# the imported target's link interface without depending on libutf8proc-dev.
# QTermWidget's public API does not expose utf8proc (the shared library pulls
# it in by itself through DT_NEEDED), so drop link items that do not exist.
get_target_property(_qtw_link qtermwidget6 INTERFACE_LINK_LIBRARIES)
set(_qtw_link_fixed "")
foreach(_item IN LISTS _qtw_link)
    if(IS_ABSOLUTE "${_item}" AND NOT EXISTS "${_item}")
        message(STATUS "shadow-terminal: ignoring missing '${_item}' from the qtermwidget6 link interface")
        continue()
    endif()
    list(APPEND _qtw_link_fixed "${_item}")
endforeach()
set_target_properties(qtermwidget6 PROPERTIES INTERFACE_LINK_LIBRARIES "${_qtw_link_fixed}")

# ---- generated theme data ---------------------------------------------------------
#
# Everything visual is rendered from the design tokens by scripts/gen-tokens.py:
#
#   style/<mode>-<accent>.qss         Qt style sheet of the window chrome
#   theme/<mode>-<accent>.json        colour table used by C++ (icons, palette)
#   glyphs/<mode>-<accent>/*.png      indicator images referenced by the style sheet
#   tokens.h                          sizes, fonts and names as C++ constants
#   color-schemes/*.colorscheme       "SHADOW Obsidian" / "SHADOW Daylight"
#
# One variant per appearance mode and accent, so the application can follow
# Shadow::Appearance at run time without a single hard-coded colour.
set(TERMINAL_TOKENS "${CMAKE_SOURCE_DIR}/desktop/themes/tokens/shadow-tokens.json")
set(TERMINAL_GEN_TOKENS "${CMAKE_SOURCE_DIR}/scripts/gen-tokens.py")
set(TERMINAL_GEN_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
# Laid out like the install tree, so an uninstalled binary in <build>/bin finds
# its data through the same relative path as an installed one.
set(TERMINAL_BUILD_DATA_DIR "${CMAKE_BINARY_DIR}/${SHADOW_DATA_DIR}/terminal")

file(READ "${TERMINAL_TOKENS}" _tokens_json)
string(JSON _accent_count LENGTH "${_tokens_json}" accent)
string(JSON TERMINAL_ACCENT_DEFAULT GET "${_tokens_json}" accentDefault)
math(EXPR _accent_last "${_accent_count} - 1")
set(TERMINAL_ACCENTS "")
foreach(_i RANGE ${_accent_last})
    string(JSON _accent MEMBER "${_tokens_json}" accent ${_i})
    list(APPEND TERMINAL_ACCENTS "${_accent}")
endforeach()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${TERMINAL_TOKENS}")

# terminal_render(<template> <output> <mode> <accent>)
#   <template> is relative to this directory, or absolute.
function(terminal_render template output mode accent)
    if(NOT IS_ABSOLUTE "${template}")
        set(template "${CMAKE_CURRENT_SOURCE_DIR}/${template}")
    endif()
    cmake_path(GET template FILENAME _name)
    add_custom_command(
        OUTPUT "${output}"
        COMMAND ${Python3_EXECUTABLE} "${TERMINAL_GEN_TOKENS}" render
                "${template}" "${output}" --mode ${mode} --accent ${accent}
        # gen-tokens keeps the mtime of unchanged outputs; touch so the rule is
        # not considered out of date on every build.
        COMMAND ${CMAKE_COMMAND} -E touch "${output}"
        DEPENDS "${template}" "${TERMINAL_TOKENS}" "${TERMINAL_GEN_TOKENS}"
        COMMENT "Rendering ${_name} (${mode}, ${accent})"
        VERBATIM)
endfunction()

# The indicator images of the style sheet (check marks, chevrons) are drawn
# as PNG files by tools/glyphs.py in the colours of each variant: Qt can load
# SVG from a style sheet only with the optional qt6-svg-plugins installed.
set(TERMINAL_GEN_GLYPHS "${CMAKE_CURRENT_SOURCE_DIR}/tools/glyphs.py")
execute_process(
    COMMAND ${Python3_EXECUTABLE} "${TERMINAL_GEN_GLYPHS}" --list
    OUTPUT_VARIABLE _glyph_files OUTPUT_STRIP_TRAILING_WHITESPACE
    RESULT_VARIABLE _glyph_rc)
if(_glyph_rc)
    message(FATAL_ERROR "shadow-terminal: tools/glyphs.py --list failed")
endif()
string(REPLACE "\n" ";" TERMINAL_GLYPH_FILES "${_glyph_files}")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${TERMINAL_GEN_GLYPHS}")

set(TERMINAL_THEME_FILES "")
foreach(_accent IN LISTS TERMINAL_ACCENTS)
    # The style sheet refers to its glyphs by the accent's *name*, which is
    # not a token value gen-tokens can substitute: resolve @ACCENT_NAME@ here
    # and hand the result to gen-tokens as the template.
    set(ACCENT_NAME "${_accent}")
    configure_file(style/terminal.qss.in "${TERMINAL_GEN_DIR}/templates/terminal-${_accent}.qss.in" @ONLY)
    foreach(_mode dark light)
        set(_variant "${_mode}-${_accent}")
        terminal_render("${TERMINAL_GEN_DIR}/templates/terminal-${_accent}.qss.in"
                        "${TERMINAL_GEN_DIR}/style/${_variant}.qss" ${_mode} ${_accent})
        terminal_render(style/theme.json.in "${TERMINAL_GEN_DIR}/theme/${_variant}.json" ${_mode} ${_accent})

        set(_glyph_outputs "")
        foreach(_file IN LISTS TERMINAL_GLYPH_FILES)
            list(APPEND _glyph_outputs "${TERMINAL_GEN_DIR}/glyphs/${_variant}/${_file}")
        endforeach()
        add_custom_command(
            OUTPUT ${_glyph_outputs}
            COMMAND ${Python3_EXECUTABLE} "${TERMINAL_GEN_GLYPHS}"
                    "${TERMINAL_GEN_DIR}/theme/${_variant}.json" "${TERMINAL_GEN_DIR}/glyphs/${_variant}"
            DEPENDS "${TERMINAL_GEN_GLYPHS}" "${TERMINAL_GEN_DIR}/theme/${_variant}.json"
            COMMENT "Drawing style sheet glyphs (${_mode}, ${_accent})"
            VERBATIM)

        list(APPEND TERMINAL_THEME_FILES
             "${TERMINAL_GEN_DIR}/style/${_variant}.qss" "${TERMINAL_GEN_DIR}/theme/${_variant}.json"
             ${_glyph_outputs})
    endforeach()
endforeach()

terminal_render(src/tokens.h.in "${TERMINAL_GEN_DIR}/tokens.h" dark ${TERMINAL_ACCENT_DEFAULT})
set_source_files_properties("${TERMINAL_GEN_DIR}/tokens.h" PROPERTIES SKIP_AUTOGEN ON)

# The two SHADOW colour schemes. The file name (without suffix) is the name
# QTermWidget knows a scheme by.
set(TERMINAL_SCHEME_FILES
    "${TERMINAL_BUILD_DATA_DIR}/color-schemes/ShadowObsidian.colorscheme"
    "${TERMINAL_BUILD_DATA_DIR}/color-schemes/ShadowDaylight.colorscheme")
terminal_render(style/ShadowObsidian.colorscheme.in
    "${TERMINAL_BUILD_DATA_DIR}/color-schemes/ShadowObsidian.colorscheme" dark ${TERMINAL_ACCENT_DEFAULT})
terminal_render(style/ShadowDaylight.colorscheme.in
    "${TERMINAL_BUILD_DATA_DIR}/color-schemes/ShadowDaylight.colorscheme" light ${TERMINAL_ACCENT_DEFAULT})

# Additional built-in schemes. Unlike Obsidian/Daylight these are original,
# standalone palettes (not derived from the appearance tokens): they are copied
# verbatim into the data directory next to the generated pair. QTermWidget
# discovers every ".colorscheme" in that directory, so no extra registration is
# needed; a copy is embedded in the binary as the same fallback as the others.
set(TERMINAL_EXTRA_SCHEMES ShadowNeon ShadowMatrix ShadowCyberpit ShadowMonoGhost)
foreach(_scheme IN LISTS TERMINAL_EXTRA_SCHEMES)
    set(_src "${CMAKE_CURRENT_SOURCE_DIR}/style/${_scheme}.colorscheme")
    set(_dst "${TERMINAL_BUILD_DATA_DIR}/color-schemes/${_scheme}.colorscheme")
    add_custom_command(
        OUTPUT "${_dst}"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${_src}" "${_dst}"
        DEPENDS "${_src}"
        COMMENT "Installing colour scheme ${_scheme}"
        VERBATIM)
    list(APPEND TERMINAL_SCHEME_FILES "${_dst}")
endforeach()

# ---- executable ----------------------------------------------------------------------
file(GLOB TERMINAL_SOURCES CONFIGURE_DEPENDS RELATIVE "${CMAKE_CURRENT_SOURCE_DIR}" src/*.cpp src/*.h)
file(GLOB TERMINAL_ICONS CONFIGURE_DEPENDS RELATIVE "${CMAKE_CURRENT_SOURCE_DIR}" icons/*.svg)

qt_add_executable(shadow-terminal
    main.cpp
    ${TERMINAL_SOURCES}
    "${TERMINAL_GEN_DIR}/tokens.h")

qt_add_resources(shadow-terminal terminal_theme
    PREFIX /terminal
    BASE "${TERMINAL_GEN_DIR}"
    FILES ${TERMINAL_THEME_FILES})
# Copy of the colour schemes inside the binary: the fallback when the data
# directory cannot be found (binary copied out of its tree).
qt_add_resources(shadow-terminal terminal_schemes
    PREFIX /terminal
    BASE "${TERMINAL_BUILD_DATA_DIR}"
    FILES ${TERMINAL_SCHEME_FILES})
if(TERMINAL_ICONS)
    qt_add_resources(shadow-terminal terminal_icons
        PREFIX /terminal
        FILES ${TERMINAL_ICONS})
endif()

file(RELATIVE_PATH _data_reldir
     "${CMAKE_INSTALL_FULL_BINDIR}" "${CMAKE_INSTALL_PREFIX}/${SHADOW_DATA_DIR}/terminal")
target_compile_definitions(shadow-terminal PRIVATE
    SHADOW_TERMINAL_DATA_DIR="${CMAKE_INSTALL_PREFIX}/${SHADOW_DATA_DIR}/terminal"
    SHADOW_TERMINAL_DATA_RELDIR="${_data_reldir}")
target_include_directories(shadow-terminal PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src" "${TERMINAL_GEN_DIR}")
target_link_libraries(shadow-terminal PRIVATE Qt6::Widgets Qt6::Svg qtermwidget6 shadowui)

# ---- install ---------------------------------------------------------------------------
install(TARGETS shadow-terminal RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES org.shadow.terminal.desktop DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
install(FILES ${TERMINAL_SCHEME_FILES} DESTINATION ${SHADOW_DATA_DIR}/terminal/color-schemes)
