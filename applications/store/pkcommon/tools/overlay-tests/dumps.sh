#!/bin/bash
O=/tmp/store-pktest/out; mkdir -p $O
t() { local name=$1; shift; timeout 400 "$@" > $O/$name.json 2> $O/$name.err; echo "$name rc=$? bytes=$(stat -c %s $O/$name.json) err=$(grep -v -c '^$' $O/$name.err)"; }
t st-updates nexus-store --dump updates
t st-repos nexus-store --dump repos
t st-search nexus-store --dump search editor
t st-comp-kate nexus-store --dump component kate
t st-comp-bash nexus-store --dump component bash
t st-comp-none nexus-store --dump component nexus-no-such-pkg
t st-installed nexus-store --dump installed
t up-status nexus-updater --dump status
t up-updates nexus-updater --dump updates
t up-history nexus-updater --dump history
t up-repos nexus-updater --dump repos
