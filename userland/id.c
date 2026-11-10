#include <libc.h>
static void name(unsigned id) {if(!id)printf("root");else if(id==1000)printf("user");else if(id>1000 && id<1032)printf("profile-%u",id-1000);else printf("%u",id);}
int main(int argc,char **argv) {
 unsigned real=0,kind=0,named=0;
 for(int i=1;i<argc;i++) {
  if(argv[i][0]!='-')return 2;
  for(const char *p=argv[i]+1;*p;p++) {if(*p=='r')real=1;else if(*p=='n')named=1;else if(*p=='u')kind=1;else if(*p=='g')kind=2;else if(*p=='G')kind=3;else {puts("Usage: id [-u|-g|-G] [-r] [-n]");return 2;}}
 }
 unsigned uid=call0(SYS_getuid),euid=call0(SYS_geteuid),gid=call0(SYS_getgid),egid=call0(SYS_getegid);
 if(kind==1 || kind==2) {unsigned v=kind==1?(real?uid:euid):(real?gid:egid);if(named)name(v);else printf("%u",v);puts("");}
 else if(kind==3) {unsigned groups[32];long n=call2(SYS_getgroups,32,groups);if(n<0)return 1;if(named)name(egid);else printf("%u",egid);for(int i=0;i<n;i++)if(groups[i]!=egid){printf(" ");if(named)name(groups[i]);else printf("%u",groups[i]);}puts("");}
 else {printf("uid=%u(",uid);name(uid);printf(") gid=%u(",gid);name(gid);printf(")");if(euid!=uid){printf(" euid=%u(",euid);name(euid);printf(")");}if(egid!=gid){printf(" egid=%u(",egid);name(egid);printf(")");}puts("");}
 return 0;
}
