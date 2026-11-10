// Exercise production save/recovery code with real files and syscall failures.
#include "../userland/codeoss/file-service.h"
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdarg>
#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <vector>
#include <functional>
static std::string root;
static unsigned cases;
static int fault, writes, renames;
extern "C" ssize_t __real_write(int,const void*,size_t);
extern "C" int __real_fsync(int);
extern "C" int __real_close(int);
extern "C" long __real_syscall(long,...);
extern "C" ssize_t __wrap_write(int fd,const void*bytes,size_t n) {
 if(fault==1 && writes++==1){errno=ENOSPC;return -1;}
 if(fault==1 && n>2)n=2;
 return __real_write(fd,bytes,n);
}
extern "C" int __wrap_fsync(int fd) {if(fault==2){errno=EIO;return -1;}return __real_fsync(fd);}
extern "C" int __wrap_close(int fd) {bool fail=fault==3 && (fcntl(fd,F_GETFL)&O_ACCMODE)==O_WRONLY;int e=__real_close(fd);if(fail){fault=0;errno=EIO;return -1;}return e;}
extern "C" long __wrap_syscall(long number,...) {
 if(number!=SYS_renameat2){errno=ENOSYS;return -1;}
 va_list args;va_start(args,number);int fromfd=va_arg(args,int);const char*from=va_arg(args,const char*);int tofd=va_arg(args,int);const char*to=va_arg(args,const char*);int flags=va_arg(args,int);va_end(args);
 renames++;
 if((fault==4 && renames==1)||(fault==5 && renames==2)){errno=ENOSPC;return -1;}
 if(fault==6 && renames==2) {
  int fd=open(to,O_WRONLY|O_CREAT|O_EXCL,0600);if(fd<0)abort();
  const char bytes[]="intervening writer";if(__real_write(fd,bytes,sizeof(bytes)-1)!=(ssize_t)sizeof(bytes)-1)abort();__real_close(fd);
 }
 return __real_syscall(number,fromfd,from,tofd,to,flags);
}
#define CHECK(x) do {if(!(x)){fprintf(stderr,"FAIL file service line=%d case=%u fault=%d: %s\n",__LINE__,cases,fault,#x);exit(1);}}while(0)
static void put(const std::string &path,const std::string &text) {
 int fd=open(path.c_str(),O_WRONLY|O_CREAT|O_TRUNC,0600);CHECK(fd>=0);CHECK(__real_write(fd,text.data(),text.size())==(ssize_t)text.size());CHECK(__real_close(fd)==0);
}
static CoreFS::Failure expect(int number,const std::function<void()> &action) {
 try{action();}catch(const CoreFS::Failure &e){CHECK(e.number==number);cases++;return e;}
 CHECK(false);abort();
}
static unsigned temporaries() {DIR*d=opendir(root.c_str());CHECK(d);unsigned n=0;while(auto*e=readdir(d))if(!strncmp(e->d_name,".codeoss-tmp-",13))n++;closedir(d);return n;}
int main(int argc,char**argv) {
 CHECK(argc==2);root=argv[1];std::string p=root+"/source.txt",original="BOM\xef\xbb\xbf caf\xc3\xa9 \xf0\x9f\x98\x80\r\nsecond\n";
 CHECK(CoreFS::encode(CoreFS::decode(original))==original);cases++;
 const std::vector<std::string> invalid={std::string("a\0b",3),"\x80","\xc0\x80","\xe0\x80\x80","\xed\xa0\x80","\xf4\x90\x80\x80","\xf0\x9f","\xc2x"};
 for(const auto &s:invalid)expect(EILSEQ,[&]{CoreFS::decode(s);});
 expect(EILSEQ,[]{CoreFS::encode(std::u16string(1,0xd800));});expect(EILSEQ,[]{CoreFS::encode(std::u16string(1,0xdc00));});
 expect(EINVAL,[&]{CoreFS::read("relative");});expect(EINVAL,[&]{CoreFS::read(std::string("/x\0y",4));});expect(ENAMETOOLONG,[&]{CoreFS::read("/"+std::string(511,'x'));});
 expect(EISDIR,[&]{CoreFS::read(root);});expect(ENOTSUP,[]{CoreFS::read("/dev/null");});
 put(p,original);auto first=CoreFS::read(p);CHECK(first.bytes==original);CHECK(chmod(p.c_str(),0640)==0);auto saved=CoreFS::save(p,"edited\n",&original);CHECK(!saved.backup.empty());CHECK(CoreFS::read(saved.backup).bytes==original);CHECK(CoreFS::read(p).bytes=="edited\n");CHECK((CoreFS::read(p).mode&0777)==0640);cases++;
 expect(EAGAIN,[&]{CoreFS::save(p,"stale",&original);});CHECK(CoreFS::read(p).bytes=="edited\n");
 expect(EEXIST,[&]{CoreFS::save(p,"clobber",nullptr);});CHECK(CoreFS::read(p).bytes=="edited\n");
 CHECK(symlink(p.c_str(),(root+"/link").c_str())==0);expect(ELOOP,[&]{CoreFS::read(root+"/link");});expect(EEXIST,[&]{CoreFS::save(root+"/link","bad",nullptr);});
 CHECK(mkfifo((root+"/pipe").c_str(),0600)==0);expect(ENOTSUP,[&]{CoreFS::read(root+"/pipe");});
 CHECK(chmod(p.c_str(),0444)==0);std::string edited="edited\n";expect(EACCES,[&]{CoreFS::save(p,"bad",&edited);});CHECK(chmod(p.c_str(),0600)==0);
 put(root+"/large",std::string(CoreFS::MaxBytes+1,'x'));expect(EFBIG,[&]{CoreFS::read(root+"/large");});expect(EFBIG,[&]{CoreFS::save(root+"/large-new",std::string(CoreFS::MaxBytes+1,'x'),nullptr);});
 for(int failure=1;failure<=5;failure++) {
  put(p,original);fault=failure;writes=renames=0;expect(failure==1||failure==4||failure==5?ENOSPC:EIO,[&]{CoreFS::save(p,"safe replacement",&original);});fault=0;
  CHECK(CoreFS::read(p).bytes==original);CHECK(!temporaries());
 }
 put(p,original);fault=6;writes=renames=0;auto race=expect(EEXIST,[&]{CoreFS::save(p,"unsaved edit",&original);});fault=0;
 CHECK(CoreFS::read(p).bytes=="intervening writer");CHECK(!race.recovery.empty());CHECK(CoreFS::read(race.recovery).bytes==original);CHECK(!temporaries());cases++;
 auto created=CoreFS::save(root+"/new.txt","new\n",nullptr);CHECK(created.backup.empty());CHECK(CoreFS::read(root+"/new.txt").bytes=="new\n");cases++;
 printf("CODEOSS_HOST_FILE_SERVICE_PASS cases=%u Unicode, checked reads, read-only/symlink/FIFO rejection, no-clobber, retained originals, partial write/fsync/close/rename failure and concurrent-destination recovery\n",cases);return 0;
}
