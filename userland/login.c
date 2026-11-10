#include <libc.h>
#include <uapi/account.h>
#include <account_secret.h>
int main(int argc,char **argv) {
 if(argc!=2){puts("Usage: login USER_ID (for example, login 1000)");return 2;}
 struct os_account_request q={0};
 if(!strcmp(argv[1],"root"))q.uid=0;
 else if(!strcmp(argv[1],"user"))q.uid=OS_ACCOUNT_FIRST;
 else {const char *s=argv[1];if(!*s)return 2;for(;*s;s++){if(*s<'0' || *s>'9' || q.uid>1031)return 2;q.uid=q.uid*10+*s-'0';}}
 if(q.uid && (q.uid<OS_ACCOUNT_FIRST || q.uid>=OS_ACCOUNT_FIRST+OS_ACCOUNT_COUNT))return 2;
 int e=account_secret(q.password,"Profile password: ");
 if(!e)e=call2(SYS_account,OS_ACCOUNT_CONSOLE_LOGIN,&q);
 account_erase(&q,sizeof(q));puts(e?"login: authentication failed.":"Console account switched.");return !!e;
}
