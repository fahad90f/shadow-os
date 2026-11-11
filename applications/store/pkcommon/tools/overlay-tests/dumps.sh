#!/bin/bash
O=/tmp/store-pktest/out; mkdir -p $O
t() { local name=$1; shift; timeout 400 "$@" > $O/$name.json 2> $O/$name.err; echo "$name rc=$? bytes=$(stat -c %s $O/$name.json) err=$(grep -v -c '^$' $O/$name.err)"; }
t st-updates shadow-store --dump updates
t st-repos shadow-store --dump repos
t st-search shadow-store --dump search editor
t st-comp-kate shadow-store --dump component kate
t st-comp-bash shadow-store --dump component bash
t st-comp-none shadow-store --dump component shadow-no-such-pkg
t st-installed shadow-store --dump installed
t up-status shadow-updater --dump status
t up-updates shadow-updater --dump updates
t up-history shadow-updater --dump history
t up-repos shadow-updater --dump repos
