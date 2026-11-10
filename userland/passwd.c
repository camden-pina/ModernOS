#include <libc.h>
#include <uapi/account.h>
#include <account_secret.h>
int main(int argc,char **argv) {
 unsigned uid=call0(SYS_getuid);struct os_account_request q={.uid=uid,.actor=uid};
 if(argc==2 && !uid){
  if(!strcmp(argv[1],"root"))q.uid=0;
  else if(!strcmp(argv[1],"user"))q.uid=1000;
  else {const char *p=argv[1];unsigned n=0;if(!*p)return 2;for(;*p;p++){if(*p<'0' || *p>'9' || n>1031)return 2;n=n*10+*p-'0';}if(n && (n<1000 || n>1031))return 2;q.uid=n;}
  q.actor=0;
 }
 else if(argc!=1){puts("Usage: passwd [USER_ID (root only)]");return 2;}
 int e=call2(SYS_account,OS_ACCOUNT_QUERY,&q);q.actor=uid;
 if(e){puts("passwd: account unavailable.");return 1;}
 if(uid && (q.flags&OS_ACCOUNT_PASSWORD))e=account_secret(q.password,"Current password: ");
 if(!e)e=account_secret(q.new_password,"New password: ");
 char confirm[128]={0};if(!e)e=account_secret(confirm,"Confirm new password: ");
 if(!e && (strlen(q.new_password)<8 || strcmp(q.new_password,confirm)))e=-E_INVAL;
 q.changes=OS_ACCOUNT_SET_PASSWORD;q.flags&=OS_ACCOUNT_ADMIN;
 if(!e)e=call2(SYS_account,OS_ACCOUNT_UPDATE,&q);
 account_erase(&q,sizeof(q));account_erase(confirm,sizeof(confirm));
 puts(e?"passwd: password was not changed (check current password and use at least 8 matching characters).":"Password updated for this live boot.");return !!e;
}
