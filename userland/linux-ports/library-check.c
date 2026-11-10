#include <stdio.h>
#include <stdlib.h>
#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>
#include <sqlite3.h>
int main(int argc, char **argv) {
    puts("SDK entering Lua"); fflush(stdout);
    lua_State *lua = luaL_newstate();
    if (!lua) return 10;
    luaL_openlibs(lua);
    if (luaL_dostring(lua, "return 6*7") || lua_tointeger(lua, -1) != 42) return 11;
    lua_close(lua);
    puts("SDK Lua passed; entering SQLite"); fflush(stdout);
    sqlite3 *db = NULL;
    if (sqlite3_open(argc > 1 ? argv[1] : "/tmp/linux-ports-work/library.db", &db)) return 20;
    puts("SDK SQLite opened"); fflush(stdout);
    char *error = NULL;
    if (sqlite3_exec(db, "CREATE TABLE IF NOT EXISTS t(n);DELETE FROM t;INSERT INTO t VALUES(42)", NULL, NULL, &error)) {
        fprintf(stderr, "library database: %s\n", error ? error : "unknown"); return 21;
    }
    sqlite3_stmt *query = NULL;
    if (sqlite3_prepare_v2(db, "SELECT n FROM t", -1, &query, NULL) || sqlite3_step(query) != SQLITE_ROW || sqlite3_column_int(query, 0) != 42) return 22;
    sqlite3_finalize(query);
    if (sqlite3_close(db)) return 23;
    puts("LINUX_PORT_LIBRARY_SDK_PASS Lua/SQLite PIC archives with native compiler");
    return 0;
}
