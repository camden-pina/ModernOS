#!/bin/sh
n=0
while test ! -f /tmp/linux-ports-work/sqlite-release; do
    n=$((n+1))
    test "$n" -lt 3000 || exit 1
    /usr/lib/coast/linux-compat/coreutils/sleep .01 || exit 2
done
