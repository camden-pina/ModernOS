#include <libc.h>
#include <uapi/account.h>
#include "include/shell_completion.h"
#include <stdbool.h>

static char cwd[OS_PATH_MAX], previous_cwd[OS_PATH_MAX];
static char history[32][512];
static unsigned history_count;
static int background_request;
struct command_help {
  const char *name, *group, *usage, *summary, *details, *example;
  int min, max, external;
};

/* external: 0=builtin, 1=program, 2=foreground, 3=upstream help,
   4=upstream foreground, 5=upstream shell (scripts may run in background). */
static const struct command_help commands[] = {
#define SHELL_COMMAND(...) {__VA_ARGS__},
#include "shell_commands.inc"
#undef SHELL_COMMAND
};

static const struct command_help *lookup_command(const char *name) {
  for (unsigned i = 0; i < sizeof(commands) / sizeof(commands[0]); i++)
    if (!strcmp(name, commands[i].name))
      return &commands[i];
  return NULL;
}

static void help_page(const struct command_help *entry) {
  printf("Usage: %s\n%s\n%s\nExample: %s\n", entry->usage, entry->summary,
         entry->details, entry->example);
}

static int usage_error(const struct command_help *entry, const char *reason) {
  printf("%s: %s\n", entry->name, reason);
  help_page(entry);
  return 2;
}

static int help(const char *topic) {
  if (topic) {
    const struct command_help *entry = lookup_command(topic);
    if (entry) {
      help_page(entry);
      return 0;
    }
    int found = 0;
    for (unsigned i = 0; i < sizeof(commands) / sizeof(commands[0]); i++)
      if (!strcmp(topic, commands[i].group)) {
        printf("  %s - %s\n", commands[i].usage, commands[i].summary);
        found = 1;
      }
    if (!found) {
      printf("help: unknown command or category '%s'.\n", topic);
      puts("Categories: shell files text disks system programs");
      return 2;
    }
    puts("Use help COMMAND for options and examples.");
    return 0;
  }

  puts(
      "Coast command guide: help COMMAND, help CATEGORY, or COMMAND --help");
  const char *groups[] = {"shell", "files",  "text",
                          "disks", "system", "programs"};
  for (unsigned g = 0; g < 6; g++) {
    printf("%s:", groups[g]);
    for (unsigned i = 0; i < sizeof(commands) / sizeof(commands[0]); i++)
      if (!strcmp(groups[g], commands[i].group))
        printf(" %s", commands[i].name);
    puts("");
  }

  puts("Paths: quote spaces; / and /tmp live in RAM; writable FAT32 mounts "
       "persist.");
  puts("Programs: NAME searches /bin; run PATH ARGS launches an ELF/script; & "
       "starts a job.");
  puts("Terminal: Backspace edits a line; Ctrl-C cancels; edit FILE opens the "
       "editor.");
  puts("Storage: lsblk -> mounts -> ls /mnt/DEVICE. Use sync before stopping.");
  puts("Example: edit /mnt/nvme1n1p1/notes.txt");
  puts("For scripts, pipelines and redirection, start build-sh. Use gcc/g++ to "
       "compile programs.");
  return 0;
}

static struct job {
  int pid, status, finished;
} jobs[128];

static void remember_job(int pid) {
  for (unsigned i = 0; i < 128; i++)
    if (!jobs[i].pid) {
      jobs[i] = (struct job){pid, 0, 0};
      return;
    }
}

static void reap_jobs(void) {
  int status, pid;
  while ((pid = waitpid(-1, &status, 1)) > 0) {
    for (unsigned i = 0; i < 128; i++)
      if (jobs[i].pid == pid) {
        jobs[i].finished = 1;
        jobs[i].status = status;
        break;
      }
  }
}

static void error(const char *name, long code) {
  printf("%s: %s (%ld)\n", name, error_text(code), code);
}

static int tokenize(char *line, char **argv) {
  char *readp = line, *writep = line;
  int argc = 0;
  background_request = 0;
  while (*readp) {
    while (*readp == ' ' || *readp == '\t' || *readp == '\n')
      readp++;
    if (!*readp)
      break;
    if (argc == OS_ARG_MAX)
      return -E_2BIG;
    argv[argc++] = writep;
    int literal = 0;
    char quote = 0;

    while (*readp &&
           (quote || (*readp != ' ' && *readp != '\t' && *readp != '\n'))) {
      char c = *readp++;
      if (!quote && (c == '|' || c == '<' || c == '>' || c == ';'))
        return -E_NOTSUP;
      if (c == '\\' && quote != '\'') {
        if (!*readp)
          return -E_INVAL;
        literal = 1;
        *writep++ = *readp++;
        continue;
      }

      if (c == '\'' || c == '"') {
        literal = 1;
        if (quote == c) {
          quote = 0;
          continue;
        }
        if (!quote) {
          quote = c;
          continue;
        }
      }

      *writep++ = c;
    }

    if (quote)
      return -E_INVAL;
    if (*readp)
      readp++;
    *writep++ = 0;
    if (!literal && !strcmp(argv[argc - 1], "&")) {
      while (*readp == ' ' || *readp == '\t')
        readp++;
      if (*readp || argc == 1)
        return -E_NOTSUP;
      background_request = 1;
    }
  }

  argv[argc] = NULL;
  return argc;
}

static int ls(const char *path, int hidden, int detailed) {
  int fd = open(path, O_RDONLY | O_DIRECTORY, 0);
  if (fd < 0) {
    error(path, fd);
    return 1;
  }
  uint8_t buffer[4096];
  long n;
  int failed = 0;
  while ((n = call3(SYS_getdents64, fd, buffer, sizeof(buffer))) > 0) {
    for (long off = 0; off < n;) {
      struct os_dirent *d = (void *)(buffer + off);
      if (d->reclen < 24 || d->reclen > n - off) {
        puts("ls: invalid directory response");
        close(fd);
        return 1;
      }
      if (hidden || d->name[0] != '.') {
        if (detailed) {
          struct os_stat st;
          long e =
              syscall6(SYS_newfstatat, fd, (long)d->name, (long)&st, 0, 0, 0);
          if (e < 0) {
            error(d->name, e);
            failed = 1;
          } else
            printf("%o %ld ", st.mode & 0777, (long)st.size);
        }

        printf("%s%s\n", d->name,
               d->type == 4    ? "/"
               : d->type == 10 ? "@"
                               : "");
      }

      off += d->reclen;
    }
  }

  if (n < 0) {
    error(path, n);
    failed = 1;
  }
  close(fd);
  return failed;
}

static int cat(const char *path) {
  int fd = open(path, O_RDONLY, 0);
  if (fd < 0) {
    error(path, fd);
    return 1;
  }
  char buffer[4096];
  long n;
  while ((n = read(fd, buffer, sizeof(buffer))) > 0) {
    long written = 0;
    while (written < n) {
      long part = write(1, buffer + written, n - written);
      if (part <= 0) {
        error("cat", part ? part : -E_IO);
        close(fd);
        return 1;
      }
      written += part;
    }
  }

  if (n < 0)
    error(path, n);
  close(fd);
  return n < 0 ? 1 : 0;
}

static int destination(const char *source, const char *target, char *out) {
  struct os_stat st;
  if (!stat(target, &st) && (st.mode & 0170000) == S_IFDIR) {
    const char *base = source;
    for (const char *p = source; *p; p++)
      if (*p == '/')
        base = p + 1;
    size_t n = strlen(target), b = strlen(base);
    if (!b || n + b + 2 > OS_PATH_MAX)
      return -E_NAMETOOLONG;
    strcpy(out, target);
    out[n++] = '/';
    strcpy(out + n, base);
  } else {
    if (strlen(target) >= OS_PATH_MAX)
      return -E_NAMETOOLONG;
    strcpy(out, target);
  }
  return 0;
}

static int copy_file(const char *source, const char *target) {
  char path[OS_PATH_MAX];
  int e = destination(source, target, path);
  if (e)

    return e;

  int input = open(source, O_RDONLY, 0);
  if (input < 0)
    return input;

  struct os_stat src, dst;
  e = fstat(input, &src);
  if (e || (src.mode & 0170000) != S_IFREG) {
    close(input);
    return e ? e : -E_ISDIR;
  }
  if (!stat(path, &dst) && dst.dev == src.dev && dst.ino == src.ino) {
    close(input);
    return -E_INVAL;
  }

  int output = open(path, O_CREAT | O_WRONLY | O_TRUNC, src.mode & 0777);
  if (output < 0) {
    close(input);
    return output;
  }

  char buffer[4096];
  long n, total = 0;
  while ((n = read(input, buffer, sizeof(buffer))) > 0) {
    long written = 0;
    while (written < n) {
      long part = write(output, buffer + written, n - written);
      if (part <= 0) {
        e = part ? part : -E_NOSPC;
        break;
      }
      written += part;
    }

    if (e)
      break;
    total += n;
  }

  if (!e && n < 0)
    e = n;
  if (!e && total != src.size)
    e = -E_IO;

  close(output);
  close(input);
  if (!e) {
    long mode = call2(SYS_chmod, path, src.mode & 0777);
    if (mode < 0 && mode != -E_NOTSUP)
      e = mode;
  }

  if (!e)
    e = call0(SYS_sync);
  return e;
}

static int info(unsigned kind) {
  char buffer[8192];
  long n = call3(SYS_sysinfo, kind, buffer, sizeof(buffer));
  if (n < 0) {
    error("sysinfo", n);
    return 1;
  }
  long e = write_all(1, buffer, n);
  if (e) {
    error("sysinfo output", e);
    return 1;
  }
  return 0;
}

static int mkdir_parents(const char *path) {
  char copy[OS_PATH_MAX];
  if (strlen(path) >= sizeof(copy))
    return -E_NAMETOOLONG;
  strcpy(copy, path);
  for (char *p = copy + 1;; p++)
    if (*p == '/' || !*p) {
      char saved = *p;
      *p = 0;
      if (*copy) {
        long e = call2(SYS_mkdir, copy, 0755);
        if (e && e != -E_EXIST)
          return e;
        struct os_stat st;
        e = stat(copy, &st);
        if (e)
          return e;
        if ((st.mode & 0170000) != S_IFDIR)
          return -E_NOTDIR;
      }

      *p = saved;
      if (!saved)
        break;
    }
  return 0;
}

static int program_path(const char *program, char *path) {
  size_t n = strlen(program);
  if (!n)
    return -E_NOENT;
  if (n >= OS_PATH_MAX)
    return -E_NAMETOOLONG;
  for (const char *p = program; *p; p++)
    if (*p == '/') {
      strcpy(path, program);
      return 0;
    }
  const char *search = shell_search_path();
  char denied[OS_PATH_MAX] = "";
  for (unsigned component = 0; component < 64; component++) {
    size_t d = 0;
    while (search[d] && search[d] != ':')
      d++;
    bool separator = d && search[d - 1] != '/';
    if (d + separator + n < OS_PATH_MAX) {
      memcpy(path, search, d);
      if (separator)
        path[d] = '/';
      memcpy(path + d + separator, program, n + 1);
      struct os_stat st;
      if (!stat(path, &st) && (st.mode & 0170000) == S_IFREG) {
        if (st.mode & 0111)
          return 0;
        if (!denied[0])
          strcpy(denied, path);
      }
    }
    search += d;
    if (!*search)
      break;
    search++;
  }
  if (denied[0]) {
    strcpy(path, denied);
    return 0;
  }
  return -E_NOENT;
}

static void suggest(const char *name) {
  /* Bounded edit distance for short command names. */
  size_t n = strlen(name);
  if (n > 32)
    return;
  const char *best = NULL;
  unsigned score = 3;
  for (unsigned c = 0; c < sizeof(commands) / sizeof(commands[0]); c++) {
    const char *candidate = commands[c].name;
    size_t m = strlen(candidate);
    unsigned row[33], next[33];
    if (m > 32)
      continue;
    for (unsigned j = 0; j <= m; j++)
      row[j] = j;
    for (unsigned i = 1; i <= n; i++) {
      next[0] = i;
      for (unsigned j = 1; j <= m; j++) {
        unsigned a = row[j] + 1, b = next[j - 1] + 1,
                 d = row[j - 1] + (name[i - 1] != candidate[j - 1]);
        next[j] = a < b ? a : b;
        if (d < next[j])
          next[j] = d;
      }

      memcpy(row, next, (m + 1) * sizeof(row[0]));
    }

    if (row[m] < score) {
      score = row[m];
      best = candidate;
    }
  }

  if (best)
    printf("Did you mean '%s'? See help %s.\n", best, best);
}

/* Match the shell's invocation mode without granting it terminal ownership.
   A script/-c command can run in the background; -i and stdin scripts still
   need the foreground terminal. The kernel enforces access for descendants. */
static bool shell_has_script(int argc, char **argv) {
  bool interactive = false, from_stdin = false;
  for (int i = 1; i < argc; i++) {
    const char *option = argv[i];
    if (!strcmp(option, "--")) {
      return !interactive && !from_stdin && i + 1 < argc &&
             strcmp(argv[i + 1], "-");
    }
    if ((option[0] != '-' && option[0] != '+') || !option[1]) {
      return !interactive && !from_stdin && strcmp(option, "-");
    }
    if (option[1] == '-') {
      return false;
    }
    bool enable = option[0] == '-', command = false, named_option = false;
    for (const char *flag = option + 1; *flag; flag++) {
      if (*flag == 'i')
        interactive = enable;
      if (*flag == 's')
        from_stdin = enable;
      if (*flag == 'c' && enable)
        command = true;
      if (*flag == 'o')
        named_option = true;
    }

    if (command) {
      return !interactive && i + 1 < argc;
    }
    if (named_option && i + 1 < argc) {
      if (!strcmp(argv[i + 1], "interactive"))
        interactive = enable;
      i++;
    }
  }

  return false;
}

static int command(int argc, char **argv) {
  if (!argc)
    return 0;
  const char *name = argv[0];
  long e = 0;
  const struct command_help *entry = lookup_command(name);
  int background = background_request;
  if (background)
    argv[--argc] = NULL;
  if (entry) {
    if (entry->external < 3 && argc == 2 &&
        (!strcmp(argv[1], "--help") || !strcmp(argv[1], "-h"))) {
      help_page(entry);
      return 0;
    }
    if (background && entry->external != 1 && entry->external != 3 &&
        !(entry->external == 5 && shell_has_script(argc, argv)))
      return usage_error(entry,
                         "this command requires the foreground terminal");
    if (argc - 1 < entry->min || argc - 1 > entry->max)
      return usage_error(entry, "incorrect number of arguments");
  }

  if (!strcmp(name, "help"))
    return help(argc > 1 ? argv[1] : NULL);
  if (!strcmp(name, "pwd")) {
    e = call2(SYS_getcwd, cwd, sizeof(cwd));

    if (e < 0) {
      error(name, e);
      return 1;
    }
    puts(cwd);
    return 0;
  }

  if (!strcmp(name, "cd")) {
    int previous = argc > 1 && !strcmp(argv[1], "-");
    if (previous && !previous_cwd[0]) {
      puts("cd: no previous directory; change directory first.");
      return 1;
    }
    char before[OS_PATH_MAX];
    e = call2(SYS_getcwd, before, sizeof(before));
    if (e >= 0) {
      e = call1(SYS_chdir, previous ? previous_cwd : argc > 1 ? argv[1] : "/");
      if (!e) {
        strcpy(previous_cwd, before);
        if (previous) {
          e = call2(SYS_getcwd, cwd, sizeof(cwd));
          if (e >= 0)
            puts(cwd);
        }
      }
    }
  } else if (!strcmp(name, "ls")) {
    int hidden = 0, detailed = 0, first = 1;
    for (; first < argc && argv[first][0] == '-'; first++) {
      if (!strcmp(argv[first], "--")) {
        first++;
        break;
      }
      for (char *p = argv[first] + 1; *p; p++) {
        if (*p == 'a')
          hidden = 1;
        else if (*p == 'l')
          detailed = 1;
        else
          return usage_error(entry,
                             "unknown option; supported options are -a and -l");
      }
    }

    int failed = 0;
    if (first == argc)
      failed = ls(".", hidden, detailed);
    for (int i = first; i < argc; i++) {
      if (argc - first > 1)
        printf("%s:\n", argv[i]);
      failed |= ls(argv[i], hidden, detailed);
    }
    return failed;
  } else if (!strcmp(name, "cat")) {
    int failed = 0;
    for (int i = 1; i < argc; i++)
      failed |= cat(argv[i]);
    return failed;
  } else if (!strcmp(name, "cp")) {
    if (argv[1][0] == '-')
      return usage_error(entry, "options are not supported; prefix a filename "
                                "beginning with - with ./");
    e = copy_file(argv[1], argv[2]);
  } else if (!strcmp(name, "mv")) {
    char path[OS_PATH_MAX];
    e = destination(argv[1], argv[2], path);
    if (!e)
      e = rename_noreplace(argv[1], path);
    if (e == -E_XDEV) {
      e = copy_file(argv[1], path);
      if (!e)
        e = call1(SYS_unlink, argv[1]);
    }
  } else if (!strcmp(name, "mkdir") || !strcmp(name, "rmdir") ||
             !strcmp(name, "rm") || !strcmp(name, "touch")) {
    int first = 1, parents = !strcmp(name, "mkdir") && !strcmp(argv[1], "-p"),
        failed = 0;
    if (parents || !strcmp(argv[1], "--"))
      first++;
    else if (argv[1][0] == '-')
      return usage_error(
          entry,
          "unsupported option; use -- before filenames beginning with -");
    if (first == argc)
      return usage_error(entry, "missing path");
    for (int i = first; i < argc; i++) {
      if (!strcmp(name, "mkdir"))
        e = parents ? mkdir_parents(argv[i]) : call2(SYS_mkdir, argv[i], 0755);
      else if (!strcmp(name, "rmdir"))
        e = call1(SYS_rmdir, argv[i]);
      else if (!strcmp(name, "rm"))
        e = call1(SYS_unlink, argv[i]);
      else {
        int fd = open(argv[i], O_CREAT | O_WRONLY, 0644);
        e = fd < 0 ? fd : close(fd);
      }
      if (e < 0) {
        error(argv[i], e);
        failed = 1;
      }
    }

    return failed;
  } else if (!strcmp(name, "write") || !strcmp(name, "append")) {
    int fd = open(argv[1],
                  O_CREAT | O_WRONLY |
                      (!strcmp(name, "append") ? O_APPEND : O_TRUNC),
                  0644);
    if (fd < 0)
      e = fd;
    else {
      for (int i = 2; i < argc && !e; i++) {
        if (i > 2)
          e = write_all(fd, " ", 1);
        if (!e)
          e = write_all(fd, argv[i], strlen(argv[i]));
      }

      if (!e)
        e = write_all(fd, "\n", 1);
      if (!e)
        e = fsync(fd);
      close(fd);
    }
  } else if (!strcmp(name, "chmod")) {
    long mode = number(argv[1], 8);
    if (mode < 0 || mode > 07777)
      return usage_error(
          entry, "mode must be octal digits in the range 0 through 7777");
    e = call2(SYS_chmod, argv[2], mode);
  } else if (!strcmp(name, "stat")) {
    struct os_stat st;
    e = stat(argv[1], &st);
    if (!e)
      printf("%s type=%s size=%ld mode=%o uid=%u gid=%u device=%lu inode=%lu\n", argv[1],
             (st.mode & 0170000) == S_IFDIR   ? "directory"
             : (st.mode & 0170000) == S_IFCHR ? "device"
                                              : "file",
             (long)st.size, st.mode & 07777, st.uid, st.gid, (unsigned long)st.dev,
             (unsigned long)st.ino);
  } else if (!strcmp(name, "mount")) {
    if (argc == 1) {
      return info(INFO_MOUNTS);
    }
    int i = 1;
    unsigned flags = 0;
    const char *type = "auto";
    while (i < argc && argv[i][0] == '-') {
      if (!strcmp(argv[i], "-t") && i + 1 < argc) {
        type = argv[i + 1];
        i += 2;
      } else if (!strcmp(argv[i], "-o") && i + 1 < argc) {
        if (!strcmp(argv[i + 1], "ro"))
          flags = MS_RDONLY;
        else if (!strcmp(argv[i + 1], "rw"))
          flags = 0;
        else
          return usage_error(entry, "mount options must be ro or rw");
        i += 2;
      } else
        return usage_error(entry, "unknown option or missing option value");
    }

    if (argc - i != 2)
      return usage_error(
          entry, "expected a device name and an existing empty directory");
    e = syscall6(SYS_mount, (long)argv[i], (long)argv[i + 1], (long)type, flags,
                 0, 0);
    if (!e) {
      puts("Mounted filesystem:");
      return info(INFO_MOUNTS);
    }
  } else if (!strcmp(name, "umount"))
    e = call2(SYS_umount2, argv[1], 0);
  else if (!strcmp(name, "ps")) {
    return info(INFO_PROCESSES);
  } else if (!strcmp(name, "acpi") || !strcmp(name, "battery")) {
    return info(INFO_ACPI);
  } else if (!strcmp(name, "power") && argc == 1) {
    return info(INFO_POWER);
  } else if (!strcmp(name, "poweroff") || !strcmp(name, "reboot") ||
             !strcmp(name, "power")) {
    unsigned action = !strcmp(name, "poweroff") ? OS_POWER_OFF : OS_POWER_RESET;
    if (!strcmp(name, "power")) {
      if (strcmp(argv[1], "cancel"))
        return usage_error(entry, "only cancel is supported");
      action = OS_POWER_CANCEL;
    }
    e = call2(SYS_power, action, 0);
    if (!e) {
      puts(action == OS_POWER_CANCEL
               ? "Cancellation accepted. Check power for the resulting state."
               : "Power request accepted (or already pending). Completion is "
                 "asynchronous; check power for progress or failure.");
    }
  } else if (!strcmp(name, "cpus")) {
    return info(INFO_CPUS);
  } else if (!strcmp(name, "disks")) {
    return info(INFO_DISKS);
  } else if (!strcmp(name, "lsblk")) {
    return info(INFO_LSBLK);
  } else if (!strcmp(name, "netinfo")) {
    return info(INFO_NETWORK);
  } else if (!strcmp(name, "mounts")) {
    return info(INFO_MOUNTS);
  } else if (!strcmp(name, "sync"))
    e = call0(SYS_sync);
  else if (!strcmp(name, "uname")) {
    if (argc > 1 && strcmp(argv[1], "-a"))
      return usage_error(entry, "only -a is supported");
    struct os_utsname os;
    e = call1(SYS_uname, &os);
    if (!e) {
      if (argc > 1)
        printf("%s %s %s %s %s\n", os.sysname, os.nodename, os.release,
               os.version, os.machine);
      else
        puts(os.sysname);
    }
  } else if (!strcmp(name, "uptime")) {
    struct os_timespec ts;
    e = call2(SYS_clock_gettime, 1, &ts);
    if (!e)
      printf("up %ld days %ld hours %ld minutes %ld seconds\n",
             (long)ts.sec / 86400, (long)ts.sec / 3600 % 24,
             (long)ts.sec / 60 % 60, (long)ts.sec % 60);
  } else if (!strcmp(name, "which")) {
    int failed = 0;
    for (int i = 1; i < argc; i++) {
      char path[OS_PATH_MAX];
      e = program_path(argv[i], path);
      if (!e)
        e = call2(SYS_access, path, 1);
      if (e) {
        error(argv[i], e);
        failed = 1;
      } else
        puts(path);
    }

    return failed;
  } else if (!strcmp(name, "kill")) {
    long signal = 9, pid;
    if (argc == 3) {
      if (argv[1][0] != '-')
        return usage_error(
            entry, "signal must have the form -NUMBER (for example -15)");
      signal = number(argv[1] + 1, 10);
    }
    pid = number(argv[argc - 1], 10);
    if (pid <= 1 || pid > INT32_MAX || signal < 0 || signal > 64)
      return usage_error(entry, "PID must be greater than 1; signal must be an "
                                "integer from 0 through 64");
    e = call2(SYS_kill, pid, signal);
  } else if (!strcmp(name, "jobs")) {
    reap_jobs();
    int found = 0;
    for (unsigned i = 0; i < 128; i++)
      if (jobs[i].pid) {
        found = 1;
        printf("pid %d %s status=%d\n", jobs[i].pid,
               jobs[i].finished ? "done" : "running", jobs[i].status);
      }
    if (!found)
      puts("No background jobs.");
    return 0;
  } else if (!strcmp(name, "wait")) {
    int status = 0;
    long pid = argc > 1 ? number(argv[1], 10) : -1;
    if (argc > 1 && (pid <= 1 || pid > INT32_MAX))
      return usage_error(entry, "PID must be a decimal integer greater than 1");
    struct job *job = NULL;
    for (unsigned i = 0; i < 128; i++)
      if (jobs[i].pid && (pid == -1 || jobs[i].pid == pid)) {
        job = &jobs[i];
        break;
      }
    if (job && job->finished) {
      e = job->pid;
      status = job->status;
    } else
      e = waitpid(job ? job->pid : pid, &status, 0);
    if (e > 0) {
      printf("pid %ld status=%d\n", e, status);
      if (job)
        memset(job, 0, sizeof(*job));
      e = 0;
    }
  } else if (!strcmp(name, "sleep")) {
    long seconds = number(argv[1], 10);
    if (seconds < 0 || seconds > 86400)
      return usage_error(
          entry, "SECONDS must be a decimal integer from 0 through 86400");
    struct os_timespec ts = {seconds, 0};
    e = call2(SYS_nanosleep, &ts, 0);
  } else if (!strcmp(name, "echo")) {
    for (int i = 1; i < argc; i++)
      printf("%s%s", i == 1 ? "" : " ", argv[i]);
    puts("");
    return 0;
  } else if (!strcmp(name, "clear")) {
    write_all(1, "\033[2J\033[H", 7);
    return 0;
  } else if (!strcmp(name, "history")) {
    unsigned start = history_count > 32 ? history_count - 32 : 0;
    for (unsigned i = start; i < history_count; i++)
      printf("%u %s\n", i + 1, history[i % 32]);
    return 0;
  } else if (!strcmp(name, "exit")) {
    if (call0(SYS_getpid) != 1)
      exit(0);
    puts("The system shell remains active. Use the emulator controls to stop.");
    return 0;
  } else {
    int first = !strcmp(name, "run") ? 1 : 0;
    if (argc <= first)
      return usage_error(lookup_command("run"), "missing program name");
    char path[OS_PATH_MAX];
    const char *program = argv[first];
    e = program_path(program, path);
    if (e) {
      if (e == -E_NOENT && !entry) {
        printf("%s: command not found. Type help or which NAME.\n", program);
        suggest(program);
      } else {
        error(program, e);
      }
      return 127;
    }
    int pid = spawn(path, argv + first, background);
    if (pid < 0) {
      if (pid == -E_NOENT && !entry) {
        printf("%s: command not found. Type help or which NAME.\n", program);
        suggest(program);
      } else {
        error(program, pid);
        if (pid == -E_NOEXEC)
          puts("Programs must be supported x86-64 ELF files or executable "
               "scripts with an available interpreter.");
      }

      return 127;
    }

    if (background) {
      remember_job(pid);
      printf("started pid %d\n", pid);
    } else {
      int status;
      int result = waitpid(pid, &status, 0);
      if (result < 0) {
        error("wait", result);
        return 1;
      }
      if (status & 127) {
        printf("pid %d terminated by signal %d\n", pid, status & 127);
        return 128 + (status & 127);
      }
      if (status >> 8) {
        printf("pid %d exited %d\n", pid, status >> 8);
        return status >> 8;
      }
    }

    return 0;
  }

  if (e < 0) {
    error(name, e);
    if (e == -E_ROFS)
      puts("Choose a writable FAT32 mount from mounts, or /tmp for temporary "
           "files.");
    if (e == -E_BUSY && !strcmp(name, "umount"))
      puts("Change directory outside this mount and close processes or files "
           "using it.");
    if (e == -E_NOENT && !strcmp(name, "mount"))
      puts("Use lsblk for device names; create an empty target directory with "
           "mkdir.");
    if ((e == -E_INVAL || e == -E_NOTSUP) && entry)
      help_page(entry);
    return 1;
  }

  return 0;
}

static int parse_error(int code) {
  if (code == -E_2BIG)
    puts("parse: input exceeds the 511-byte console line limit or the argument "
         "limit. Use build-sh SCRIPT for long build commands.");
  else
    puts("parse: unmatched quote, unfinished escape, or unsupported shell "
         "operator. Close quotes and escape literal operators.");
  if (code == -E_NOTSUP)
    puts(
        "For pipes, redirection and command lists, use build-sh -c 'COMMAND'.");
  return 2;
}

static void terminal_interrupt(int signal) { (void)signal; }
__attribute__((naked)) static void terminal_signal_return(void) {
  __asm__ volatile("mov $15,%rax\nsyscall\nud2");
}
int main(int argc, char **argv) {
  bool login_console=call0(SYS_getpid)==1;
  if(login_console && call0(SYS_getuid)==0) {
    struct os_account_request q={.uid=OS_ACCOUNT_FIRST};
    int e=call2(SYS_account,OS_ACCOUNT_ENSURE,&q);
    if(!e) {e=call2(SYS_mkdir,"/home",0755);if(e==-E_EXIST)e=0;}
    if(!e) {e=call2(SYS_mkdir,"/home/user",0700);if(e==-E_EXIST)e=0;}
    int home=e?-1:open("/home/user",O_RDONLY|O_DIRECTORY|O_NOFOLLOW,0);
    if(!e)e=home<0?home:call3(SYS_fchown,home,OS_ACCOUNT_FIRST,OS_ACCOUNT_FIRST);
    if(home>=0)close(home);
    if(!e)e=call2(SYS_account,OS_ACCOUNT_ENTER,&q);
    if(e) {puts("Secure console setup failed; no root shell is exposed.");for(;;)call0(SYS_sched_yield);}
    call1(SYS_chdir,"/home/user");
    puts("Console uses the owner account (UID 1000), with administrator access through sudo.");
    char *gui_args[] = {"gui", NULL};
    puts("Starting the Coast desktop. Ctrl+Alt+G switches to the console.");
    command(1, gui_args);
  }
  int terminal = argc == 2 && !strcmp(argv[1], "--terminal");
  if (terminal) {
    struct os_sigaction action = {.handler = (uintptr_t)terminal_interrupt,
                                  .flags = SA_RESTORER,
                                  .restorer = (uintptr_t)terminal_signal_return,
                                  .mask = 0};
    long error = syscall6(SYS_rt_sigaction, 2, (long)&action, 0, 8, 0, 0);
    if (error)
      printf("Terminal SIGINT handler unavailable: %s\n", error_text(error));
  }
  if (argc > 1 && !terminal) {
    if (argc != 3 || strcmp(argv[1], "-c")) {
      help_page(lookup_command("coast-sh"));
      return 2;
    }
    char *words[OS_ARG_MAX + 1];
    int n = tokenize(argv[2], words);
    return n < 0 ? parse_error(n) : command(n, words);
  }

  puts("Coast userland shell. Type help, help text, or help COMMAND.");

  for (;;) {
    if(login_console) {
      unsigned previous_uid=call0(SYS_getuid);struct os_account_request q={0};
      int e=call2(SYS_account,OS_ACCOUNT_CONSOLE_SYNC,&q);
      unsigned uid=call0(SYS_getuid);
      if(e) {puts("Console account unavailable.");for(;;)call0(SYS_sched_yield);}
      if(uid!=previous_uid) {
        memset(history,0,sizeof(history));history_count=0;previous_cwd[0]=0;
        char home[64]="/home/user";
        if(!uid)strcpy(home,"/root");
        else if(uid!=OS_ACCOUNT_FIRST) {strcpy(home,"/home/profile-");char digits[16];unsigned n=0,v=uid-OS_ACCOUNT_FIRST;do{digits[n++]='0'+v%10;v/=10;}while(v);char *p=home+strlen(home);while(n)*p++=digits[--n];*p=0;}
        call1(SYS_chdir,"/");call1(SYS_chdir,home);
      }
    }
    reap_jobs();
    long e = call2(SYS_getcwd, cwd, sizeof(cwd));
    if (e < 0) {
      error("pwd", e);
      strcpy(cwd, "?");
    }

    char line[512], *words[OS_ARG_MAX + 1];
    int length;
    if (terminal) {
      length =
          shell_read_terminal(line, sizeof(line), cwd, history, history_count);
    } else {
      length = shell_read_interactive(line, sizeof(line), cwd, history,
                                      history_count);
    }
    if (login_console && length == SHELL_GUI_HOTKEY) {
      char *gui_args[] = {"gui", NULL};
      command(1, gui_args);
      continue;
    }
    if (length < 0) {
      parse_error(length);
      continue;
    }
    if (!length && terminal)
      return 0;
    if (!length || !line[0])
      continue;

    strcpy(history[history_count++ % 32], line);
    int count = tokenize(line, words);
    if (count < 0)
      parse_error(count);
    else
      command(count, words);
  }
}
