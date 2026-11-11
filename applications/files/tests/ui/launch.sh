# SPDX-License-Identifier: GPL-3.0-or-later
# Opening files ("Open with…") and a terminal in the current folder.
T=/tmp/files-test-launch; rm -rf "$T"; mkdir -p "$T/work/inner dir"
echo "plain text" > "$T/work/notes.txt"
app_start "$T/work"
echo "terminal available: $(command -v nexus-terminal || echo no)"
key F4; sleep 6
echo "1 F4: $(pgrep -a nexus-terminal | sed 's/^[0-9]* //' | tr '\n' '|')"
snap 1-terminal
pkill -x nexus-terminal; sleep 1.5
# folder context menu > Open terminal here (13th entry of a folder's menu)
key i; park; key Menu; sleep 0.6; snap 2-folder-menu; key Escape; menu_pick 13; sleep 6
echo "2 folder menu: $(pgrep -a nexus-terminal | sed 's/^[0-9]* //' | tr '\n' '|')"
pkill -x nexus-terminal; sleep 1.5
# Open with… (2nd entry of a file's menu)
click 600 400; key n; menu_pick 2; sleep 1.5; snap 3-open-with
click 512 505; sleep 0.6; snap 4-app-selected            # "System Information"
click "${DEF_X:-417}" "${DEF_Y:-601}"; sleep 0.4; snap 5-make-default
click 800 639; sleep 5                                    # "Open"
echo "3 open with: $(pgrep -a nexus-sysinfo | sed 's/^[0-9]* //' | tr '\n' '|')"
echo "  mimeapps.list: $(grep -h -A3 'Default Applications' $HOME/.config/mimeapps.list 2>/dev/null | tr '\n' ' ')"
pkill -x nexus-sysinfo; sleep 1.5
# Return opens with the default application chosen above
click 600 400; key n Return; sleep 5
echo "4 Return   : $(pgrep -a nexus-sysinfo | sed 's/^[0-9]* //' | tr '\n' '|')"
pkill -x nexus-sysinfo
app_stop
