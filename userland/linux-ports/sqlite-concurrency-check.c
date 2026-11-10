#define _GNU_SOURCE
#include <sqlite3.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#define CHECK(x) do {if(!(x)){fprintf(stderr,"FAIL SQLite concurrency:%d %s errno=%d\n",__LINE__,#x,errno);exit(1);}} while(0)
static int scalar(sqlite3 *db,const char *sql) {
 sqlite3_stmt *q=NULL;CHECK(sqlite3_prepare_v2(db,sql,-1,&q,NULL)==SQLITE_OK);CHECK(sqlite3_step(q)==SQLITE_ROW);int v=sqlite3_column_int(q,0);CHECK(sqlite3_finalize(q)==SQLITE_OK);return v;
}
static void child(const char *self,const char *mode,const char *path) {
 pid_t p=fork();CHECK(p>=0);if(!p){execl(self,self,mode,path,(char*)NULL);_exit(120);}int status;CHECK(waitpid(p,&status,0)==p && WIFEXITED(status) && WEXITSTATUS(status)==0);
}
int main(int argc,char **argv) {
 sqlite3 *db=NULL;
 if(argc==3){
  CHECK(sqlite3_open(argv[2],&db)==SQLITE_OK);CHECK(sqlite3_busy_timeout(db,0)==SQLITE_OK);
  if(!strcmp(argv[1],"--contender")){
   int r=sqlite3_exec(db,"BEGIN IMMEDIATE;INSERT INTO t VALUES(99);COMMIT",NULL,NULL,NULL);
   CHECK(r==SQLITE_BUSY && sqlite3_extended_errcode(db)==SQLITE_BUSY);CHECK(scalar(db,"SELECT count(*) FROM t")==0);
   puts("SQLITE_API_BUSY_5_AND_CONCURRENT_READER_PASS");
  }else{CHECK(!strcmp(argv[1],"--reader"));CHECK(scalar(db,"SELECT n FROM t")==42);CHECK(scalar(db,"SELECT count(*) FROM t")==1);}
  CHECK(sqlite3_close(db)==SQLITE_OK);return 0;
 }
 char dir[]="/tmp/sqlite-concurrency-XXXXXX",path[256];CHECK(mkdtemp(dir));snprintf(path,sizeof(path),"%s/data.db",dir);
 CHECK(sqlite3_open(path,&db)==SQLITE_OK);CHECK(sqlite3_exec(db,"PRAGMA journal_mode=DELETE;CREATE TABLE t(n);BEGIN IMMEDIATE;INSERT INTO t VALUES(42)",NULL,NULL,NULL)==SQLITE_OK);
 child(argv[0],"--contender",path);CHECK(sqlite3_exec(db,"COMMIT",NULL,NULL,NULL)==SQLITE_OK);CHECK(sqlite3_close(db)==SQLITE_OK);
 child(argv[0],"--reader",path);CHECK(!unlink(path) && !rmdir(dir));puts("SQLITE_CONCURRENCY_API_CHECK_PASS");return 0;
}
