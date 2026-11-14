# shellcheck shell=bash
# Defaults for the preinstalled programming languages (50-dev-languages.list).

# luarocks depends on Lua 5.1, which then wins the "lua" alternative by
# priority, so typing "lua" would start 5.1. Make the current language the
# default; 5.1 stays available as lua5.1 for luarocks.
if [ -x /usr/bin/lua5.4 ]; then
    update-alternatives --set lua-interpreter /usr/bin/lua5.4
fi
if [ -x /usr/bin/luac5.4 ]; then
    update-alternatives --set lua-compiler /usr/bin/luac5.4
fi
