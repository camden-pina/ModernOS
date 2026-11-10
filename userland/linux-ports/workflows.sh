#!/bin/sh
# Real upstream programs; every case checks behavior and exit status.
P=/usr/lib/coast/linux-ports
D=/tmp/linux-ports-work
mkdir -p "$D"
passed=0
failed=0
run_case() {
    name=$1
    shift
    "$@" >"$D/case.out" 2>&1
    result=$?
    printf 'LINUX_PORT_CASE %s %s\n' "$name" "$result"
    cat "$D/case.out"
    if test "$result" = 0; then passed=$((passed+1)); else failed=$((failed+1)); fi
}
run_case bash-language "$P/bash/bash" --noprofile --norc -c 'set -e; a=(red blue); declare -A m=([key]=value); f(){ printf "%s:%s:%s" "${a[1]}" "${m[key]}" "$((6*7))"; }; test "$(f)" = blue:value:42; echo BASH_LANGUAGE_PASS'
run_case bash-pipeline-wait "$P/bash/bash" --noprofile --norc -c 'set -o pipefail; test "$(printf "one\ntwo\n" | /usr/lib/coast/linux-ports/sed/sed -n 1p)" = one || exit 1; (exit 7)& p=$!; wait "$p"; test "$?" = 7 || exit 2; echo BASH_PIPE_WAIT_PASS'
run_case grep-recursive /bin/build-sh -c 'set -e; p=/usr/lib/coast/linux-ports/grep/grep; d=/tmp/linux-ports-work/grep; mkdir -p "$d/sub"; printf "alpha 12\nbeta 34\n" >"$d/sub/file"; "$p" -r -n -E "beta [0-9]+" "$d" >"$d/../grep-result"; "$p" -q "sub/file:2:beta 34" "$d/../grep-result"; echo GREP_RECURSIVE_PASS'
run_case grep-nul /bin/build-sh -c 'set -e; p=/usr/lib/coast/linux-ports/grep/grep; printf "red\000blue\000" >/tmp/linux-ports-work/nul; "$p" -z -q "blue" /tmp/linux-ports-work/nul; echo GREP_NUL_PASS'
run_case sed-inplace /bin/build-sh -c 'set -e; p=/usr/lib/coast/linux-ports/sed/sed; d=/tmp/linux-ports-work; printf "before 123\n" >"$d/edit"; "$p" -i.bak "s/before/after/;s/[0-9][0-9]*/42/" "$d/edit"; test "$(cat "$d/edit")" = "after 42"; test "$(cat "$d/edit.bak")" = "before 123"; echo SED_INPLACE_PASS'
run_case sed-multiline /bin/build-sh -c 'set -e; test "$(printf "one\ntwo\n" | /usr/lib/coast/linux-ports/sed/sed "N;s/\\n/:/")" = one:two; echo SED_MULTILINE_PASS'
run_case diff-cmp /bin/build-sh -c 'p=/usr/lib/coast/linux-ports/diffutils; d=/tmp/linux-ports-work; printf "same\n" >"$d/a"; cp "$d/a" "$d/b"; "$p/diff" "$d/a" "$d/b" || exit 1; "$p/cmp" "$d/a" "$d/b" || exit 2; printf "changed\n" >"$d/b"; "$p/diff" -u "$d/a" "$d/b" >"$d/diff"; test "$?" = 1 || exit 3; "$p/cmp" -s "$d/a" "$d/b"; test "$?" = 1 || exit 4; "$p/diff" "$d/a" "$d/missing"; test "$?" = 2 || exit 5; echo DIFF_CMP_PASS'
run_case diff3-sdiff /bin/build-sh -c 'p=/usr/lib/coast/linux-ports/diffutils; d=/tmp/linux-ports-work; export PATH="$p:/usr/bin:/bin"; printf "one\ntwo\n" >"$d/base"; printf "ONE\ntwo\n" >"$d/left"; printf "one\nTWO\n" >"$d/right"; "$p/diff3" -m "$d/left" "$d/base" "$d/right" >"$d/merged"; r=$?; test "$r" = 0 -o "$r" = 1 || exit 1; /usr/lib/coast/linux-ports/grep/grep -q ONE "$d/merged" || exit 2; /usr/lib/coast/linux-ports/grep/grep -q TWO "$d/merged" || exit 3; "$p/sdiff" "$d/left" "$d/right" >"$d/side"; test "$?" = 1 || exit 4; echo DIFF3_SDIFF_PASS'
run_case lua-language "$P/lua/lua" -e 'local t={1,2,3,4}; local sum=0; for _,v in ipairs(t) do sum=sum+v end; assert(sum==10); local c=coroutine.create(function() coroutine.yield(42) return 7 end); local ok,n=coroutine.resume(c); assert(ok and n==42); ok,n=coroutine.resume(c); assert(ok and n==7); print("LUA_LANGUAGE_PASS")'
run_case lua-files-process "$P/lua/lua" -e 'local p="/tmp/linux-ports-work/lua-data"; local f=assert(io.open(p,"wb")); f:write(string.rep("LuaData",10000)); assert(f:close()); f=assert(io.open(p,"rb")); assert(f:seek("end")==70000); assert(f:seek("set",7)==7); assert(f:read(7)=="LuaData"); f:close(); assert(os.rename(p,p..".moved")); local c=assert(io.popen("printf LUA_CHILD")); assert(c:read("a")=="LUA_CHILD"); assert(c:close()); print("LUA_FILES_PROCESS_PASS")'
run_case lua-bytecode /bin/build-sh -c 'set -e; d=/tmp/linux-ports-work; printf "assert(6*7==42);print(\"LUA_BYTECODE_PASS\")\n" >"$d/program.lua"; /usr/lib/coast/linux-ports/lua/luac -o "$d/program.luac" "$d/program.lua"; /usr/lib/coast/linux-ports/lua/lua "$d/program.luac"'
run_case sqlite-memory /bin/build-sh -c 'set -e; /usr/lib/coast/linux-ports/sqlite/sqlite3 -bail :memory: < /usr/share/coast/linux-ports/memory.sql > /tmp/linux-ports-work/memory.out; test "$(cat /tmp/linux-ports-work/memory.out)" = "$(printf "42\n1")"; echo SQLITE_MEMORY_PASS'
run_case sqlite-file /bin/build-sh -c 'set -e; p=/usr/lib/coast/linux-ports/sqlite/sqlite3; d=/tmp/linux-ports-work/data.db; rm -f "$d" "$d-journal"; "$p" -bail "$d" "PRAGMA journal_mode=DELETE; CREATE TABLE t(n INTEGER, s TEXT); BEGIN IMMEDIATE; INSERT INTO t VALUES(42,'"'"'persisted'"'"'); COMMIT; BEGIN;UPDATE t SET n=7;ROLLBACK;"; test "$("$p" -bail "$d" "SELECT n||'"'"':'"'"'||s FROM t;PRAGMA integrity_check;")" = "$(printf "42:persisted\nok")"; echo SQLITE_FILE_PASS'
run_case sqlite-concurrency /bin/build-sh /usr/share/coast/linux-ports/sqlite-concurrency.sh
run_case sqlite-concurrency-api "$P/sqlite-concurrency-check"
run_case library-sdk /bin/build-sh -c 'set -e; p=/usr/lib/coast/linux-ports/sdk; gcc-dynamic -std=c11 -I"$p/include" -I"$p/include/lua5.5" /usr/src/coast/linux-ports/examples/library-check.c -L"$p/lib" -llua -lsqlite3 -lm -ldl -pthread -o /tmp/linux-ports-work/library-check; /tmp/linux-ports-work/library-check'
run_case library-sdk-static /bin/build-sh -c 'set -e; p=/usr/lib/coast/linux-ports/sdk; gcc -std=c11 -I"$p/include" -I"$p/include/lua5.5" /usr/src/coast/linux-ports/examples/library-check.c -L"$p/lib" -llua -lsqlite3 -lm -ldl -pthread -o /tmp/linux-ports-work/library-check-static; /tmp/linux-ports-work/library-check-static'
run_case record-lock-semantics "$P/record-lock-check" --coast
run_case browser-lock-denials /bin/web-capability-check
printf 'LINUX_PORT_RESULT passed=%s failed=%s\n' "$passed" "$failed"
test "$failed" = 0
