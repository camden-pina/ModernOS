/* Validate the pinned target libc for the gnulib cross-configure cache.
 * Compare every byte pair to the same tolower contract gnulib replaces.
 * This deliberately makes no promise of ISO-8859-1 locale support. */
#include <ctype.h>
#include <locale.h>
#include <stdio.h>
#include <strings.h>
static int sign(int n) { return (n > 0) - (n < 0); }
int main(void) {
 const char *locales[] = {"C", "C.UTF-8", "fr_FR.ISO-8859-1"};
 for (unsigned l=0;l<sizeof(locales)/sizeof(locales[0]);l++) {
  const char *name=setlocale(LC_ALL,locales[l]);
  if (!name) { printf("locale %s unavailable\n",locales[l]); continue; }
  for (unsigned a=0;a<256;a++) for (unsigned b=0;b<256;b++) {
   char x[3]={(char)a,'x',0}, y[3]={(char)b,'X',0};
   int expected=sign(tolower(a)-tolower(b));
   if (sign(strcasecmp(x,y))!=expected || sign(strncasecmp(x,y,1))!=expected || strncasecmp(x,y,0)!=0) return 1;
  }
  if (strcasecmp("AbCdEf", "aBcDeF") || strcasecmp("ABC", "ABCD")>=0 || strcasecmp("ABCD", "ABC")<=0) return 2;
  printf("locale %s: exhaustive case comparison passed\n",name);
 }
 puts("CONFIGURE_STRCASECMP_PASS"); return 0;
}
