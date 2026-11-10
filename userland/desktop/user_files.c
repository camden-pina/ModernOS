#include "desktop.h"

/* The trusted desktop creates personal data for the active account.
   Bind the parent descriptor before creating, and retain ordinary ownership
   on system directories and mounted filesystems. */
static int parent_directory(const char *path, char *name, struct os_stat *st) {
  char parent[OS_PATH_MAX];
  int e=normalize_path(parent,path,"..");
  if (e) return e;
  const char *leaf=basename(path);
  if (!*leaf || !strcmp(leaf,".") || !strcmp(leaf,"..")) return -E_INVAL;
  copy_text(name,OS_PATH_MAX,leaf);
  int fd=open(parent,O_RDONLY|O_DIRECTORY|O_CLOEXEC,0);
  if (fd<0) return fd;
  e=fstat(fd,st);
  if (e) { close(fd); return e; }
  return fd;
}

int desktop_create_file(const char *path,unsigned mode) {
  struct os_stat parent;
  char name[OS_PATH_MAX];
  int dir=parent_directory(path,name,&parent);
  if (dir<0) return dir;
  int fd=openat(dir,name,O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC,mode);
  int e=fd<0 ? fd : 0;
  if (!e && parent.uid==1000 && parent.gid==1000)
    e=call3(SYS_fchown,fd,1000,1000);
  if (e && fd>=0) { close(fd); call3(SYS_unlinkat,dir,name,0); }
  close(dir);
  return e ? e : fd;
}

int desktop_create_directory(const char *path,unsigned mode) {
  struct os_stat parent;
  char name[OS_PATH_MAX];
  int dir=parent_directory(path,name,&parent);
  if (dir<0) return dir;
  int e=call3(SYS_mkdirat,dir,name,mode);
  if (!e && parent.uid==1000 && parent.gid==1000) {
    int fd=openat(dir,name,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC,0);
    e=fd<0 ? fd : call3(SYS_fchown,fd,1000,1000);
    if (fd>=0) close(fd);
    if (e) call3(SYS_unlinkat,dir,name,AT_REMOVEDIR);
  }
  close(dir);
  return e;
}

unsigned desktop_file_scope(unsigned uid) {
  struct os_account_request q={.uid=uid};
  long old=call2(SYS_account,OS_ACCOUNT_FILE_SCOPE,&q);
  return old<0?0:(unsigned)old;
}
