#!/bin/sh
# Waits while the phone is hot, when the tests run in the iSH app: an iSH run
# in iSH emulates each guest instruction twice over, and a run of
# tests/regress/run.sh drove an iPhone to "serious" in minutes. The app's
# /proc/ish/thermal says how hot it is: one word, iOS's thermal state
# (nominal, fair, serious or critical). This waits while it says serious or
# critical, and returns once it says less.
#
# Anywhere else it returns at once: on Linux and macOS there is no
# /proc/ish/thermal, and the ish command line tool's is an empty line (run.sh
# run in an iSH that is itself under test).
#
# usage: tests/regress/wait_cool.sh
#   ISH_WAIT_COOL=0  never wait
#
# run.sh calls it before each section (section()), outside the timeout of
# any test: a test's time limit is never spent waiting.
set -u
f=/proc/ish/thermal
[ "${ISH_WAIT_COOL:-1}" = 0 ] && exit 0
[ -r "$f" ] || exit 0
hot() {
    case $1 in
        serious|critical) return 0 ;;
        *) return 1 ;;
    esac
}
state=$(cat "$f")
hot "$state" || exit 0
echo "wait_cool: the phone is $state: waiting till it is fair or nominal (ISH_WAIT_COOL=0: no wait)" >&2
waited=0
while hot "$state"; do
    sleep 30
    waited=$((waited + 30))
    state=$(cat "$f")
    # a line every 5 minutes: the log shows the run is waiting, not stuck
    if hot "$state" && [ $((waited % 300)) -eq 0 ]; then
        echo "wait_cool: still $state after ${waited}s" >&2
    fi
done
echo "wait_cool: $state after ${waited}s" >&2
