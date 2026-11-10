/* Sanitize production profile identifiers/names before any filesystem lookup. */
#include "../userland/desktop/profiles.c"
#define CHECK(x) do { if(!(x)) { const char message[]="FAIL profile format\n";write(2,message,sizeof(message)-1);exit(1); } } while(0)
int main(void) {
  CHECK(valid_id("default"));CHECK(valid_id("profile-1"));CHECK(valid_id("profile-31"));
  const char *bad[]={"","../escape","profile-0","profile-01","profile-32","profile-1/../../escape","default/extra","profile-9999999999999999999999999","profile-","profile--1","profile-1\n"};
  for(unsigned i=0;i<sizeof(bad)/sizeof(*bad);i++)CHECK(!valid_id(bad[i]));
  CHECK(valid_name("Default"));CHECK(valid_name("Studio 2"));CHECK(valid_name("../Display name only"));
  CHECK(!valid_name(""));CHECK(!valid_name(" trailing"));CHECK(!valid_name("trailing "));CHECK(!valid_name("line\nbreak"));CHECK(!valid_name("tab\tname"));
  char text[42];memset(text,'x',40);text[40]=0;CHECK(valid_name(text));text[40]='x';text[41]=0;CHECK(!valid_name(text));
  const char message[]="PASS production profile path IDs, printable display-name bounds, traversal/overflow/control rejection (ASan/UBSan)\n";write(1,message,sizeof(message)-1);return 0;
}
