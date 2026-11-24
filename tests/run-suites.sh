#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Runs the test suites that need no build, no chroot and no image: unit tests
# with stand-ins, static checks, and the suites that send real packets in
# private network namespaces. Prints one PASS, SKIP or FAIL line per suite and
# a summary; exits 0 only if no suite failed. The CI job "tests"
# (.github/workflows/build.yml) runs it twice: as the runner's user with
# --user, then with sudo as "--root --no-skip".
#
#     tests/run-suites.sh [--user | --root] [--no-skip] [--list] [--log-dir DIR]
#
#   --user      the suites that run as any user; parts of them that need root
#               skip themselves (the default when not run as root)
#   --root      the same suites as root, so that their root-only tests run too,
#               and the suites that need root: they work in private network,
#               mount and pid namespaces with stand-ins and never change this
#               computer's network or files (the default as root; refused
#               without root)
#   --no-skip   a suite that skipped itself counts as a failure. Use it where
#               every tool and privilege is there (the CI root step): there a
#               skip is lost coverage.
#   --list      print the suites that would run, one "name<TAB>command" per
#               line, and exit
#   --log-dir   keep each suite's output in DIR/<name>.log (default: a
#               temporary directory, removed at the end; the last lines of a
#               failing suite are printed either way)
#
# A suite passes with exit status 0 and skips itself with 77 (needs root, a
# program is missing), as in Automake's test harness. A unittest suite whose
# every test was skipped counts as skipped; single skipped tests are shown
# next to PASS.
#
# Two root suites need a Debian root file system and run only when given one:
#   SHADOW_TEST_ROOTFS=DIR             tests/build/in-chroot-resolver-test.sh
#   + SHADOW_TEST_RESOLVED_DEB=FILE    tests/network/resolved-lab-test.sh
#                                      (DIR must be trixie, FILE trixie's
#                                      systemd-resolved .deb)
#
# Not run here: the Control Center and Network Center back-end tests and the
# shell's status selftest (they need the Qt builds of those programs,
# applications/*/tests, desktop/horizon-shell/modules/status/tests), and the
# QEMU tests (tests/boot, tests/installation/test_install.py, tests/desktop).
set -u
REPO=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)

PASSED=0 SKIPPED=0 FAILED=0
FAILED_NAMES=()
NO_SKIP=0
LIST=0
LOG_DIR=

# suite NAME COMMAND...: run one suite from the top of the repository
suite() {
    local name=$1; shift
    if [ "$LIST" = 1 ]; then printf '%s\t%s\n' "$name" "$(printf '%s' "$*" | tr -s '\n ' '  ')"; return 0; fi
    local log="$LOG_DIR/$name.log" start=$SECONDS rc status note=
    (cd "$REPO" && PYTHONDONTWRITEBYTECODE=1 "$@") >"$log" 2>&1 </dev/null
    rc=$?
    # unittest's last lines: "Ran N tests in ...", then "OK (skipped=M)".
    local ran skipped
    ran=$(sed -n 's/^Ran \([0-9]*\) tests\{0,1\} in .*/\1/p' "$log" | tail -n 1)
    skipped=$(sed -n 's/^OK (.*skipped=\([0-9]*\).*)$/\1/p' "$log" | tail -n 1)
    if [ "$rc" = 0 ] && [ -n "$skipped" ] && [ "$skipped" -ge "${ran:-0}" ]; then
        rc=77 note="all $skipped tests skipped"
    elif [ "$rc" = 0 ] && [ -n "$skipped" ]; then
        note="$skipped of $ran tests skipped"
    fi
    if [ "$rc" = 0 ]; then
        status=PASS PASSED=$((PASSED + 1))
    elif [ "$rc" = 77 ] && [ "$NO_SKIP" = 0 ]; then
        status=SKIP SKIPPED=$((SKIPPED + 1))
        [ -n "$note" ] || note=$(grep -m 1 -i 'skip' "$log" | cut -c1-100)
    else
        status=FAIL FAILED=$((FAILED + 1)) FAILED_NAMES+=("$name")
        if [ "$rc" = 77 ]; then note="skipped, and --no-skip is set: ${note:-$(grep -m 1 -i 'skip' "$log" | cut -c1-100)}"
        else note="exit status $rc"; fi
    fi
    printf '%-4s  %-40s %4ss%s\n' "$status" "$name" $((SECONDS - start)) "${note:+  ($note)}"
    if [ "$status" = FAIL ]; then tail -n 20 "$log" | sed 's/^/      | /'; fi
    return 0
}

# have PROGRAM...: true if every program is installed
have() { local p; for p in "$@"; do command -v "$p" >/dev/null 2>&1 || return 1; done; }

# shellcheck disable=SC2016  # the bash -c programs expand when they run
user_suites() {
    local t
    for t in "$REPO"/tests/security/test_*.py "$REPO"/tests/build/test_*.py; do
        [ -e "$t" ] || continue
        suite "$(basename "$(dirname "$t")")-$(basename "$t" .py)" python3 -B "${t#"$REPO"/}"
    done
    suite installation-plan   python3 -B tests/installation/test_plan.py
    suite arsenal-validate    python3 -B arsenal/tools/validate.py --quiet
    suite fix-network         bash tests/network/fix-network-test.sh
    suite shellcheck          bash -c 'command -v shellcheck >/dev/null || { echo "SKIP: shellcheck is not installed"; exit 77; }
        shellcheck -S warning tests/run-suites.sh scripts/fix-network.sh distro/base/hooks/60-anon-setup.sh \
            build/lib/common.sh build/iso/stage-rootfs.sh security/firewall/NetworkManager/50-shadow-killswitch \
            tests/network/*.sh tests/build/*.sh applications/control-center/tests/real-tool-documents.sh &&
        shellcheck -S warning -s bash desktop/horizon-shell/modules/status/tests/session-test-killswitch.sh'
    suite py-compile          python3 -B -c 'import ast, sys
for path in sys.argv[1:]:
    with open(path, encoding="utf-8") as fh:
        ast.parse(fh.read(), path)' security/firewall/shadow-killswitch security/firewall/shadow-panic \
        security/hardening/shadow-netprivacy security/audit/shadow-security-status distro/base/rootfs/usr/bin/shadow-anon
    suite polkit-xml          bash -c 'command -v xmllint >/dev/null || { echo "SKIP: xmllint is not installed"; exit 77; }
        for f in security/permissions/*.policy; do xmllint --noout "$f" || exit 1; done'
    # systemd-analyze verify of the units phase 1 added; the filter drops what
    # only says that the programs and the other units are not installed here.
    suite units-verify        bash -c 'command -v systemd-analyze >/dev/null || { echo "SKIP: systemd-analyze is not installed"; exit 77; }
        d=$(mktemp -d); trap "rm -rf \"\$d\"" EXIT
        cp security/firewall/systemd/*.service security/firewall/systemd/*.timer security/hardening/systemd/*.service \
           distro/base/rootfs/usr/lib/systemd/system/shadow-anon-*.service \
           distro/base/rootfs/usr/lib/systemd/system/shadow-anon-*.timer "$d/"
        cd "$d" && out=$(SYSTEMD_LOG_LEVEL=err systemd-analyze verify --man=no ./*.service ./*.timer 2>&1 |
            grep -vE "Command .* is not executable|No such file or directory|Unit .*\.service not found|network-pre.target|is not a valid|nftables.service")
        [ -z "$out" ] || { printf "%s\n" "$out"; exit 1; }'
}

root_suites() {
    local t
    # Real packets: each re-executes itself in a private network namespace.
    for t in "$REPO"/tests/network/test_*.py; do
        suite "network-$(basename "$t" .py)" python3 -B "${t#"$REPO"/}"
    done
    if [ -n "${SHADOW_TEST_ROOTFS:-}" ]; then
        suite in-chroot-resolver bash tests/build/in-chroot-resolver-test.sh "$SHADOW_TEST_ROOTFS"
        if [ -n "${SHADOW_TEST_RESOLVED_DEB:-}" ]; then
            suite resolved-lab bash tests/network/resolved-lab-test.sh "$SHADOW_TEST_ROOTFS" "$SHADOW_TEST_RESOLVED_DEB"
        fi
    fi
}

summary() {
    [ "$LIST" = 1 ] && return 0
    echo "----"
    echo "passed: $PASSED  skipped: $SKIPPED  failed: $FAILED${FAILED_NAMES[*]:+ (${FAILED_NAMES[*]})}"
    [ "$FAILED" = 0 ]
}

main() {
    local mode=user
    [ "$(id -u)" = 0 ] && mode=root
    while [ $# -gt 0 ]; do
        case $1 in
            --user) mode=user ;;
            --root) mode=root ;;
            --no-skip) NO_SKIP=1 ;;
            --list) LIST=1 ;;
            --log-dir) LOG_DIR=${2:?--log-dir needs a directory}; shift ;;
            -h|--help) sed -n '3,/^set -u/{/^set -u/d;s/^# \{0,1\}//;p}' "${BASH_SOURCE[0]}"; return 0 ;;
            *) echo "run-suites: unknown argument: $1" >&2; return 2 ;;
        esac
        shift
    done
    if [ "$mode" = root ] && [ "$(id -u)" != 0 ] && [ "$LIST" = 0 ]; then
        echo "run-suites: --root needs root (sudo tests/run-suites.sh --root)" >&2
        return 2
    fi
    local cleanup=
    if [ "$LIST" = 0 ]; then
        if [ -z "$LOG_DIR" ]; then LOG_DIR=$(mktemp -d) cleanup=1; fi
        mkdir -p "$LOG_DIR" || return 2
    fi
    user_suites
    [ "$mode" = root ] && root_suites
    [ -n "$cleanup" ] && rm -rf "$LOG_DIR"
    summary
}

# Sourced (tests/build/test_ci_suites.py) only defines the functions.
if [ "${BASH_SOURCE[0]}" = "$0" ]; then
    main "$@"
fi
