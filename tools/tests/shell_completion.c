#include "../../userland/include/shell_completion.h"
#include <libc.h>
extern long syscall(long, ...);
extern int *__errno_location(void);
extern int fflush(void *);
long syscall6(long n, long a, long b, long c, long d, long e, long f) {
  long r = syscall(n, a, b, c, d, e, f);
  return r < 0 ? -*__errno_location() : r;
}
#define CHECK(x)                                                               \
  do {                                                                         \
    if (!(x)) {                                                                \
      printf("COMPLETION FAIL line=%u: %s\n", __LINE__, #x);                   \
      fflush(NULL);                                                            \
      __builtin_trap();                                                        \
    }                                                                          \
  } while (0)
static struct sc_result result;
struct item {
  const char *path;
  unsigned kind;
};
static const struct item items[] = {
    {"/work/alpha.txt", 1},         {"/work/alpine.txt", 1},
    {"/work/a space.txt", 1},       {"/work/quote'one.txt", 1},
    {"/work/cash$dollar", 1},       {"/work/αbeta", 1},
    {"/work/αcharlie", 1},          {"/work/dir one", 2},
    {"/work/dir one/child.txt", 1}, {"/work/.hidden", 1},
    {"/bin/sc-command", 1},         {"/usr/bin/sc-command", 1},
    {"/usr/bin/sc-companion", 1},   {"/bin/notexec", 0}};
static int kind(void *unused, const char *p, bool executable) {
  (void)unused;
  (void)executable;
  for (unsigned i = 0; i < sizeof(items) / sizeof(items[0]); i++)
    if (!strcmp(p, items[i].path))
      return items[i].kind;
  return 0;
}
static int scan(void *unused, const char *dir, sc_visit visit, void *data) {
  (void)unused;
  size_t d = strlen(dir);
  while (d > 1 && dir[d - 1] == '/')
    d--;
  for (unsigned i = 0; i < sizeof(items) / sizeof(items[0]); i++) {
    const char *p = items[i].path;
    if (strncmp(p, dir, d) || p[d] != '/')
      continue;
    p += d + 1;
    bool child = false;
    for (const char *q = p; *q; q++)
      if (*q == '/')
        child = true;
    if (!child)
      visit(data, p, items[i].kind);
  }
  return 0;
}
static const struct sc_fs fs = {scan, kind};
static int crowded(void *unused, const char *dir, sc_visit visit, void *data) {
  (void)unused;(void)dir;
  char name[24];
  for(unsigned i=0;i<SC_MATCHES+5;i++) {
    strcpy(name,"item000");name[4]+=i/100;name[5]+=(i/10)%10;name[6]+=i%10;
    visit(data,name,1);
  }
  return 0;
}
static int any_file(void *unused,const char *path,bool executable) {
  (void)unused;(void)path;(void)executable;return 1;
}

static void complete(const char *source, int cursor, const char *expected,
                     unsigned count) {
  char line[512];
  strcpy(line, source);
  int position = cursor < 0 ? (int)strlen(line) : cursor;
  int n = shell_complete(&result, line, sizeof(line), &position, "/work",
                         "/bin:/usr/bin", &fs, NULL);
  CHECK(n >= 0);
  if (strcmp(line, expected)) {
    printf("source=%s actual=%s expected=%s\n", source, line, expected);
    CHECK(0);
  }
  CHECK(result.count == count);
  CHECK(position >= 0 && (size_t)position <= strlen(line));
}
int main(int argc, char **argv) {
  complete("sc-", -1, "sc-com", 2);
  complete("sc-comm", -1, "sc-command ", 1);
  complete("cat al", -1, "cat alp", 2);
  complete("cat a\\ sp", -1, "cat a\\ space.txt ", 1);
  complete("cat 'a sp", -1, "cat 'a space.txt' ", 1);
  complete("cat \"a sp", -1, "cat \"a space.txt\" ", 1);
  complete("cat quote", -1, "cat quote\\'one.txt ", 1);
  complete("cat cash\\$", -1, "cat cash\\$dollar ", 1);
  complete("cat α", -1, "cat α", 2);
  complete("cd dir", -1, "cd dir\\ one/", 1);
  complete("cat dir\\ one/ch", -1, "cat dir\\ one/child.txt ", 1);
  complete("cat ./missing", -1, "cat ./missing", 0);
  complete("cat alpha.txt", 6, "cat alpha.txt", 1);
  complete("cat al.txt", 6, "cat alp.txt", 2);
  complete("cat al | sc-comm", -1, "cat al | sc-command ", 1);
  complete("cat > al", -1, "cat > alp", 2);
  complete("run sc-comm", -1, "run sc-command ", 1);
  complete("cat $(bad", -1, "cat $(bad", 0);
  complete("cat .h", -1, "cat .hidden ", 1);
  complete("cd al", -1, "cd al", 0);
  complete("cat \xce", -1, "cat \xce", 0);
  char shortline[8] = "cat al";
  int pos = 6;
  CHECK(shell_complete(&result, shortline, sizeof(shortline), &pos, "/work",
                       "/bin", &fs, NULL) >= 0);
  CHECK(!strcmp(shortline, "cat alp"));
  complete("cat 'quote", -1, "cat 'quote'\\''one.txt' ", 1);
  complete("cat \"cash\\$", -1, "cat \"cash\\$dollar\" ", 1);
  char limited[7]="cat al";pos=6;
  CHECK(shell_complete(&result,limited,sizeof(limited),&pos,"/work","/bin",&fs,NULL)==-E_2BIG);
  CHECK(!strcmp(limited,"cat al") && pos==6);
  char crowded_line[512]="cat item";pos=strlen(crowded_line);
  const struct sc_fs many={crowded,any_file};
  CHECK(shell_complete(&result,crowded_line,sizeof(crowded_line),&pos,"/work","",&many,NULL)==0);
  CHECK(result.count==SC_MATCHES && result.truncated && !strcmp(crowded_line,"cat item"));
  if (argc == 2) {
    char line[512] = "./native-com";
    pos = strlen(line);
    CHECK(shell_complete_native(&result, line, sizeof(line), &pos, argv[1],
                                "") >= 0);
    CHECK(!strcmp(line, "./native-command "));
  }
  puts("PASS shared completion: PATH/builtin deduplication, prefixes/lists, "
       "directories, single/double/escaped quotes, UTF8, middle cursor/suffix, "
       "pipelines/redirection, no expansion/execution and no-match");
  return 0;
}
