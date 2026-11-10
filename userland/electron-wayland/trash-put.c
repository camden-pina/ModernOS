/* Electron's supported ELECTRON_TRASH=trash-cli backend. Use the genuine
   deployed GIO implementation for the freedesktop trash and restore metadata.
   These public, stable opaque APIs avoid introducing newer host GLib headers
   into a binary linked against the pinned guest GIO/glibc distribution. */
#include <stdio.h>
#include <string.h>
typedef struct _GFile GFile;
typedef struct {unsigned domain;int code;char *message;} GError;
extern GFile *g_file_new_for_path(const char *);
extern int g_file_trash(GFile *,void *,GError **);
extern void g_object_unref(void *);
extern void g_error_free(GError *);
int main(int argc,char **argv) {
    int first=1;
    if(argc>1 && !strcmp(argv[1],"--"))first++;
    if(first==argc || argc-first>32) {
        fputs("Usage: trash-put [--] PATH... (up to 32 items)\n",stderr);return 2;
    }
    for(int i=first;i<argc;i++)if(!*argv[i] || (first==1 && argv[i][0]=='-')) {
        fputs("trash-put: use -- before a pathname beginning with -\n",stderr);return 2;
    }
    int failed=0;
    for(int i=first;i<argc;i++) {
        GFile *file=g_file_new_for_path(argv[i]);GError *error=NULL;
        if(!file || !g_file_trash(file,NULL,&error)) {
            fprintf(stderr,"trash-put: %s: %s\n",argv[i],error&&error->message?error->message:"unable to move item to trash");failed=1;
        }
        if(error)g_error_free(error);
        if(file)g_object_unref(file);
    }
    return failed;
}
