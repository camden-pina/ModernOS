#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>
#include <sqlite3.h>
int main(void) {
 int fd=open("/tmp/linux-ports-diag",O_CREAT|O_RDWR|O_TRUNC,0600);
 struct flock lock={.l_type=F_WRLCK,.l_whence=SEEK_SET,.l_start=0,.l_len=1};
 errno=0;int r=fcntl(fd,F_GETLK,&lock);printf("DIAG F_GETLK r=%d errno=%d type=%d\n",r,errno,lock.l_type);
 lock.l_type=F_WRLCK;errno=0;r=fcntl(fd,F_SETLK,&lock);printf("DIAG F_SETLK r=%d errno=%d\n",r,errno);close(fd);
 sqlite3 *db=NULL;char *error=NULL;r=sqlite3_open("/tmp/linux-ports-diag.db",&db);
 printf("DIAG sqlite open=%d extended=%d errno=%d\n",r,sqlite3_extended_errcode(db),sqlite3_system_errno(db));
 if(!r) {r=sqlite3_exec(db,"CREATE TABLE t(n); BEGIN IMMEDIATE; INSERT INTO t VALUES(42);COMMIT",NULL,NULL,&error);printf("DIAG sqlite exec=%d extended=%d errno=%d message=%s\n",r,sqlite3_extended_errcode(db),sqlite3_system_errno(db),error?error:"none");}
 sqlite3_free(error);sqlite3_close(db);return 0;
}
