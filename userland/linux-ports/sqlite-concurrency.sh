#!/bin/sh
set -e
P=/usr/lib/coast/linux-ports/sqlite/sqlite3
D=/tmp/linux-ports-work
rm -f "$D/concurrency.db" "$D/concurrency.db-journal" "$D/sqlite-ready" "$D/sqlite-release"
"$P" -bail "$D/concurrency.db" 'CREATE TABLE concurrency(n);'
"$P" -bail "$D/concurrency.db" < /usr/share/coast/linux-ports/sqlite-writer.sql >"$D/writer.out" 2>&1 &
writer=$!
n=0
while test ! -f "$D/sqlite-ready"; do
    n=$((n+1))
    test "$n" -lt 3000 || exit 1
    /usr/lib/coast/linux-compat/coreutils/sleep .01
done
set +e
"$P" -bail "$D/concurrency.db" 'BEGIN IMMEDIATE;INSERT INTO concurrency VALUES(99);COMMIT;' >"$D/contender.out" 2>&1
contender=$?
read_count=$("$P" -bail "$D/concurrency.db" 'SELECT count(*) FROM concurrency;')
reader=$?
touch "$D/sqlite-release"
wait "$writer"
writer_result=$?
set -e
printf "SQLITE_CLI_CONTENTION contender=%s reader=%s visible_rows=%s writer=%s\n" "$contender" "$reader" "$read_count" "$writer_result"
test "$contender" = 1
test "$reader" = 0
test "$read_count" = 0
test "$writer_result" = 0
/usr/lib/coast/linux-ports/grep/grep -q locked "$D/contender.out"
test "$("$P" -bail "$D/concurrency.db" 'SELECT n FROM concurrency;PRAGMA integrity_check;')" = "$(printf '42\nok')"
echo SQLITE_CONCURRENCY_PASS
