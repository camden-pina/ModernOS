#include "desktop.h"
#include <uapi/clipboard.h>

/* Desktop personalization registry; privilege policy is kernel-owned. */
#define PROFILE_LIMIT 8
static struct {
  char id[16], name[41];
  unsigned avatar;
} profiles[PROFILE_LIMIT];
static unsigned count, active;
static char base[OS_PATH_MAX], legacy[OS_PATH_MAX], registry[OS_PATH_MAX],
    status[160];
static bool invalid_registry;
static bool valid_name(const char *name) {
  size_t n = strlen(name);
  if (!n || n > 40 || name[0] == ' ' || name[n - 1] == ' ')
    return false;
  for (size_t i = 0; i < n; i++)
    if ((unsigned char)name[i] < 32 || (unsigned char)name[i] > 126)
      return false;
  return true;
}
static bool valid_id(const char *id) {
  if (!strcmp(id, "default"))
    return true;
  size_t length = strlen(id);
  if (length < 9 || length > 10 || id[8] == '0')
    return false;
  if (strncmp(id, "profile-", 8))
    return false;
  unsigned value = 0;
  for (const char *p = id + 8; *p; p++) {
    if (*p < '0' || *p > '9')
      return false;
    value = value * 10 + *p - '0';
    if (value > 31)
      return false;
  }
  return value > 0;
}
static int read_small(const char *path, char *buffer, size_t size) {
  int fd = open(path, O_RDONLY, 0);
  if (fd < 0)
    return fd;
  size_t used = 0;
  long n;
  while ((n = read(fd, buffer + used, size - used - 1)) > 0) {
    used += n;
    if (used == size - 1) {
      char extra;
      n = read(fd, &extra, 1);
      if (n > 0)
        n = -E_FBIG;
      break;
    }
  }
  close(fd);
  for (size_t i = 0; i < used; i++)
    if (!buffer[i])
      n = -E_INVAL;
  buffer[used] = 0;
  return n < 0 ? n : 0;
}
static int path_for(char *out, unsigned at) {
  char directory[OS_PATH_MAX], root[OS_PATH_MAX];
  int e = join_path(root, base, "profiles");
  if (!e)
    e = join_path(directory, root, profiles[at].id);
  return e ? e : join_path(out, directory, "desktop.ini");
}
void profiles_load(const char *old_config) {
  copy_text(legacy, sizeof(legacy), old_config);
  normalize_path(base, legacy, "..");
  join_path(registry, base, "profiles.ini");
  memset(profiles, 0, sizeof(profiles));
  count = 1;
  active = 0;
  invalid_registry = false;
  status[0] = 0;
  copy_text(profiles[0].id, sizeof(profiles[0].id), "default");
  copy_text(profiles[0].name, sizeof(profiles[0].name), "Default");
  char content[1024], chosen[16] = "default";
  int e = read_small(registry, content, sizeof(content));
  if (e && e != -E_NOENT)
    invalid_registry = true;
  if (!e) {
    bool version = false, default_seen = false, active_seen = false;
    char *p = content;
    while (*p && !invalid_registry) {
      char *key = p;
      while (*p && *p != '=' && *p != '\n')
        p++;
      if (*p != '=') {
        invalid_registry = true;
        break;
      }
      *p++ = 0;
      char *value = p;
      while (*p && *p != '\n')
        p++;
      if (*p)
        *p++ = 0;
      if (!strcmp(key, "version")) {
        if (version || strcmp(value, "1"))
          invalid_registry = true;
        version = true;
      } else if (!strcmp(key, "active")) {
        if (active_seen || !valid_id(value))
          invalid_registry = true;
        active_seen = true;
        copy_text(chosen, sizeof(chosen), value);
      } else {
        if (!valid_id(key) || !valid_name(value)) {
          invalid_registry = true;
          break;
        }
        if (!strcmp(key, "default")) {
          if (default_seen)
            invalid_registry = true;
          default_seen = true;
          copy_text(profiles[0].name, sizeof(profiles[0].name), value);
          continue;
        }
        for (unsigned i = 0; i < count; i++)
          if (!strcmp(profiles[i].id, key))
            invalid_registry = true;
        if (invalid_registry || count == PROFILE_LIMIT) {
          invalid_registry = true;
          break;
        }
        copy_text(profiles[count].id, sizeof(profiles[count].id), key);
        copy_text(profiles[count++].name, sizeof(profiles[0].name), value);
      }
    }
    if (!version || !active_seen || !default_seen)
      invalid_registry = true;
    bool found = false;
    for (unsigned i = 0; i < count; i++)
      if (!strcmp(profiles[i].id, chosen)) {
        active = i;
        found = true;
      }
    if (!found)
      invalid_registry = true;
  }
  if (invalid_registry) {
    count = 1;
    active = 0;
    copy_text(profiles[0].name, sizeof(profiles[0].name), "Default");
    copy_text(status, sizeof(status),
              "Profile registry could not be read. Original preserved; profile "
              "writes are disabled.");
  }
  for (unsigned i = 0; i < count; i++) {
    char path[OS_PATH_MAX], text[OS_PATH_MAX + 1024];
    path_for(path, i);
    if (!read_small(path, text, sizeof(text))) {
      const char *line = text;
      while (*line) {
        if (!strncmp(line, "profile_avatar=", 15) && line[15] >= '0' &&
            line[15] <= '5' && (line[16] == '\n' || !line[16]))
          profiles[i].avatar = line[15] - '0';
        while (*line && *line != '\n')
          line++;
        if (*line)
          line++;
      }
    }
  }
  for(unsigned i=0;i<count;i++) {
    struct os_account_request q={.uid=profiles_uid(i)};
    int error=call2(SYS_account,OS_ACCOUNT_ENSURE,&q);
    if(error && error!=-E_ACCES && !status[0])copy_text(status,sizeof(status),"Account policy unavailable; terminal sessions fail closed.");
  }
  struct os_account_request current={0};
  if(!call2(SYS_account,OS_ACCOUNT_CURRENT,&current)) {
    bool found=false;
    for(unsigned i=0;i<count;i++)if(profiles_uid(i)==current.uid){active=i;found=true;break;}
    if(!found) {
      unsigned at=count<PROFILE_LIMIT?count++:PROFILE_LIMIT-1;
      char n[16];format_uint(n,current.uid-OS_ACCOUNT_FIRST);
      copy_text(profiles[at].id,sizeof(profiles[at].id),"profile-");append_text(profiles[at].id,sizeof(profiles[at].id),n);
      copy_text(profiles[at].name,sizeof(profiles[at].name),"Profile ");append_text(profiles[at].name,sizeof(profiles[at].name),n);
      active=at;invalid_registry=true;
      copy_text(status,sizeof(status),"Using the signed-in account; the invalid registry was preserved.");
    }
  }
  path_for(desk.config, active);
  if (desk.trace)
    printf("DESKTOP PROFILE_ACTIVE id=%s name=%s config=%s count=%u\n",
           profiles[active].id, profiles[active].name, desk.config, count);
}
unsigned profiles_count(void) { return count; }
unsigned profiles_active(void) { return active; }
const char *profiles_name(unsigned at) {
  return at < count ? profiles[at].name : "";
}
const char *profiles_id(unsigned at) {
  return at < count ? profiles[at].id : "";
}
const char *profiles_status(void) { return status; }
const char *profiles_legacy(void) { return active == 0 ? legacy : ""; }
static int make_directory(const char *path) {
  int e = desktop_create_directory(path, 0700);
  return e == -E_EXIST ? 0 : e;
}
static int profiles_prepare_root(void) {
  if (invalid_registry)
    return -E_INVAL;
  int e = make_directory(base);
  char root[OS_PATH_MAX], dir[OS_PATH_MAX];
  if (!e)
    e = join_path(root, base, "profiles");
  if (!e)
    e = make_directory(root);
  if (!e)
    e = join_path(dir, root, profiles[active].id);
  if (!e)
    e = make_directory(dir);
  return e;
}
int profiles_save_registry(void) {
  if (invalid_registry)
    return -E_INVAL;
  char text[1024];
  copy_text(text, sizeof(text), "version=1\nactive=");
  append_text(text, sizeof(text), profiles[active].id);
  append_text(text, sizeof(text), "\n");
  for (unsigned i = 0; i < count; i++) {
    append_text(text, sizeof(text), profiles[i].id);
    append_text(text, sizeof(text), "=");
    append_text(text, sizeof(text), profiles[i].name);
    append_text(text, sizeof(text), "\n");
  }
  return preferences_write(registry, text);
}
static bool profiles_close_apps(void) {
  for(unsigned i=0;i<MAX_WINDOWS;i++) {
    Window *w=&desk.windows[i];
    if(w->alive && w->app!=APP_SETTINGS) {wm_close(w);if(w->alive)return false;}
  }
  return true;
}
static int profiles_commit_login(unsigned old) {
  struct os_account_request q={.uid=profiles_uid(active)};
  int e=call2(SYS_account,OS_ACCOUNT_LOGIN,&q);
  if(e) {active=old;profiles_save_registry();return e;}
  call3(SYS_clipboard,OS_CLIPBOARD_CLEAR,0,0);
  settings_reload_profile();files_profile_reset();return 0;
}
static int profiles_switch_root(unsigned target,const char *password) {
  if (target >= count || invalid_registry)
    return -E_INVAL;
  if (target == active)
    return 0;
  struct os_account_request auth={.uid=profiles_uid(target)};
  copy_text(auth.password,sizeof(auth.password),password?password:"");
  int checked=call2(SYS_account,OS_ACCOUNT_AUTH,&auth);
  volatile char *secret=auth.password;for(unsigned i=0;i<sizeof(auth.password);i++)secret[i]=0;
  if(checked)return checked;
  char path[OS_PATH_MAX];
  path_for(path, target);
  struct os_stat st;
  int e = stat(path, &st);
  if (e)
    return e;
  if ((st.mode & 0170000) != S_IFREG)
    return -E_INVAL;
  e = config_save();
  if (e)
    return e;
  if(!profiles_close_apps())return -E_BUSY;
  unsigned old = active;
  active = target;
  e = profiles_save_registry();
  if (e) {
    active = old;
    return e;
  }
  e=profiles_commit_login(old);if(e)return e;
  trace("PROFILE_SWITCH", NULL, profiles[active].id);
  return 0;
}
static int profiles_create_root(const char *name) {
  if (!valid_name(name) || invalid_registry)
    return -E_INVAL;
  if (count == PROFILE_LIMIT)
    return -E_NOSPC;
  for (unsigned i = 0; i < count; i++)
    if (!strcmp(name, profiles[i].name))
      return -E_EXIST;
  int e = config_save();
  if (e)
    return e;
  unsigned old = active, next = count;
  char path[OS_PATH_MAX], root[OS_PATH_MAX], candidate[OS_PATH_MAX];
  join_path(root, base, "profiles");
  bool free_id = false;
  for (unsigned n = 1; n <= 31; n++) {
    char value[16];
    format_uint(value, n);
    copy_text(profiles[next].id, sizeof(profiles[next].id), "profile-");
    append_text(profiles[next].id, sizeof(profiles[next].id), value);
    bool used = false;
    for (unsigned i = 0; i < count; i++)
      used |= !strcmp(profiles[i].id, profiles[next].id);
    join_path(candidate, root, profiles[next].id);
    struct os_stat st;
    int exists = stat(candidate, &st);
    if (!used && exists == -E_NOENT) {
      free_id = true;
      break;
    }
  }
  if (!free_id)
    return -E_EXIST;
  copy_text(profiles[next].name, sizeof(profiles[next].name), name);
  struct os_account_request account={.uid=profiles_uid(next)};
  e=call2(SYS_account,OS_ACCOUNT_ENSURE,&account);
  if(e)return e;
  if(!profiles_close_apps())return -E_BUSY;
  e=call2(SYS_account,OS_ACCOUNT_AUTH,&account);if(e)return e;
  count++;
  active = next;
  path_for(path, active);
  copy_text(desk.config, sizeof(desk.config), path);
  e = config_save();
  if (!e)
    e = profiles_save_registry();
  if (e) {
    count--;
    active = old;
    for (unsigned i = 0; i < count; i++) {
      char path[OS_PATH_MAX], text[OS_PATH_MAX + 1024];
      path_for(path, i);
      if (!read_small(path, text, sizeof(text))) {
        const char *line = text;
        while (*line) {
          if (!strncmp(line, "profile_avatar=", 15) && line[15] >= '0' &&
              line[15] <= '5' && (line[16] == '\n' || !line[16]))
            profiles[i].avatar = line[15] - '0';
          while (*line && *line != '\n')
            line++;
          if (*line)
            line++;
        }
      }
    }
    path_for(desk.config, active);
    return e;
  }
  e=profiles_commit_login(old);if(e)return e;
  trace("PROFILE_CREATE", NULL, profiles[active].id);
  return 0;
}
int profiles_rename(unsigned at, const char *name) {
  if (at >= count || !valid_name(name) || invalid_registry)
    return -E_INVAL;
  for (unsigned i = 0; i < count; i++)
    if (i != at && !strcmp(name, profiles[i].name))
      return -E_EXIST;
  char previous[41];
  copy_text(previous, sizeof(previous), profiles[at].name);
  copy_text(profiles[at].name, sizeof(profiles[at].name), name);
  int e = profiles_save_registry();
  if (e)
    copy_text(profiles[at].name, sizeof(profiles[at].name), previous);
  else
    trace("PROFILE_RENAME", NULL, profiles[at].id);
  return e;
}

unsigned profiles_avatar(unsigned at) {
  return at < count ? profiles[at].avatar : 0;
}
void profiles_avatar_changed(void) {
  if (active < count)
    profiles[active].avatar = desk.profile_avatar;
}

unsigned profiles_uid(unsigned at) {
  return OS_ACCOUNT_FIRST+(!strcmp(profiles[at].id,"default")?0:(unsigned)number(profiles[at].id+8,10));
}
int profiles_account(unsigned at,struct os_account_request *q) {
  memset(q,0,sizeof(*q));q->uid=profiles_uid(at);
  return call2(SYS_account,OS_ACCOUNT_QUERY,q);
}
void profiles_home_init(void) {
  if(active==0)copy_text(desk.home,sizeof(desk.home),"/home/user");
  else {copy_text(desk.home,sizeof(desk.home),"/home/");append_text(desk.home,sizeof(desk.home),profiles[active].id);}
  const char *folders[]={"","Desktop","Documents","Downloads","Projects","Pictures",".Trash",".config"};
  for(unsigned i=0;i<sizeof(folders)/sizeof(*folders);i++) {
    char path[OS_PATH_MAX];copy_text(path,sizeof(path),desk.home);
    if(i)join_path(path,desk.home,folders[i]);
    int e=call2(SYS_mkdir,path,0700);if(e && e!=-E_EXIST)break;
    int fd=open(path,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC,0);struct os_stat st;
    e=fd<0?fd:fstat(fd,&st);
    unsigned uid=profiles_uid(active);
    if(!e && st.uid && st.uid!=uid)e=-E_PERM;
    if(!e)e=call3(SYS_fchown,fd,uid,uid);
    if(!e && !i)e=call2(SYS_fchmod,fd,0700);
    if(fd>=0)close(fd);
    if(e)break;
  }
}

int profiles_prepare(void) {unsigned old=desktop_file_scope(0);int e=profiles_prepare_root();desktop_file_scope(old);return e;}
int profiles_create(const char *name) {desktop_file_scope(0);int e=profiles_create_root(name);desktop_file_scope(profiles_uid(profiles_active()));return e;}
int profiles_switch(unsigned at,const char *password) {desktop_file_scope(0);int e=profiles_switch_root(at,password);desktop_file_scope(profiles_uid(profiles_active()));return e;}
