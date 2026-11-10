#include <libc.h>
int main(void) {unsigned uid=call0(SYS_geteuid);if(!uid)puts("root");else if(uid==1000)puts("user");else if(uid>1000 && uid<1032)printf("profile-%u\n",uid-1000);else printf("%u\n",uid);return 0;}
