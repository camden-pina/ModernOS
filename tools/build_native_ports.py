#!/usr/bin/env python3
"""Reproducible offline ports of Vim, BusyBox, and GNU make to Coast.

Build prerequisites (static musl/ncurses) are provided by build_nano.py.
No Linux service or simulated filesystem is required at runtime. BusyBox's
shell uses bounded foreground process groups; suspended jobs remain unsupported.
"""
import hashlib
import os
from pathlib import Path
import shutil
import subprocess
import sys

from source_archive import unpack

ROOT = Path(__file__).resolve().parents[1]
VENDOR = ROOT / 'userland/vendor'
BUILD = Path(os.environ.get('COAST_PORTS_BUILD', ROOT / 'build/vendor')).resolve()
MUSL = ROOT / 'build/native-musl'
LIBS = ROOT / 'build/native-libs'
PACKAGES = {
    'vim-9.2.0000.tar.gz': '875875fb5988af3db0726bef9b048a559a9563aa0ecca8240e82057e8e5941c3',
    'busybox-1.37.0.tar.bz2': '3311dff32e746499f4df0d5df04d7eb396382d7e108bb9250e7b519b837043a4',
    'make-4.4.1.tar.gz': 'dd16fb1d67bfab79a72f5e8390735c49e3e8e70b4945a15ab1f81ddb78658fb3',
}


def run(where, log, *command, env=None, input=None):
    with (BUILD / log).open('wb') as output:
        result = subprocess.run(command, cwd=where, env=env, input=input,
                                stdout=output, stderr=subprocess.STDOUT)
    if result.returncode:
        raise SystemExit(f'{command[0]} failed; see {BUILD / log}')


BUILD.mkdir(parents=True, exist_ok=True)

only_busybox = sys.argv[1:] == ['--busybox-only']
if sys.argv[1:] and not only_busybox:
    raise SystemExit('Usage: build_native_ports.py [--busybox-only]')
compiler = os.environ.get('COAST_PORTS_CC', str(MUSL / 'bin/musl-gcc'))
if not shutil.which(compiler) or (not only_busybox and not (LIBS / 'lib/libncursesw.a').exists()):
    raise SystemExit('Build bin/nano first, or select COAST_PORTS_CC for a BusyBox-only build.')
config = VENDOR / 'busybox-coast.config'
key = hashlib.sha256(Path(__file__).read_bytes() + config.read_bytes() + compiler.encode() +
                     str(ROOT).encode() + ''.join(PACKAGES.values()).encode()).hexdigest()
stamp = BUILD / ('coast-busybox.stamp' if only_busybox else 'coast-ports.stamp')
vim = BUILD / 'vim-9.2.0000'
busybox = BUILD / 'busybox-1.37.0'
make = BUILD / 'make-4.4.1'
pairs = ((busybox / 'busybox', 'busybox'),) if only_busybox else ((vim / 'src/vim','vim'), (busybox / 'busybox','busybox'), (make / 'make','make'))
binaries = tuple(source for source, name in pairs)

for filename, digest in PACKAGES.items():
    if only_busybox and not filename.startswith('busybox-'):
        continue
    if hashlib.sha256((VENDOR / filename).read_bytes()).hexdigest() != digest:
        raise SystemExit(f'Upstream source checksum mismatch: {VENDOR / filename}')

if not stamp.exists() or stamp.read_text() != key or not all(p.exists() for p in binaries):
    for filename in PACKAGES:
        if only_busybox and not filename.startswith('busybox-'):
            continue
        target = BUILD / filename.split('.tar.')[0]
        if target.exists():
            shutil.rmtree(target)
        unpack(VENDOR / filename, BUILD)

    jobs = str(max(1, int(os.environ.get('COAST_BUILD_JOBS', '1'))))
    env = os.environ.copy()
    for name in ('CFLAGS', 'CPPFLAGS', 'LDFLAGS', 'LIBS', 'CONFIG_SITE', 'CROSS_COMPILE'):
        env.pop(name, None)
    env.update(CC=compiler,
               CFLAGS='-O2 -std=gnu11 -fno-pie -fno-stack-protector -mno-avx -mno-avx2',
               CPPFLAGS=f'-I{LIBS}/include -I{LIBS}/include/ncursesw',
               LDFLAGS=f'-static -no-pie -L{LIBS}/lib')
    if only_busybox:
        env.update(CPPFLAGS='', LDFLAGS='-static -no-pie')
    if not only_busybox:
        vim_env = env.copy()

        # The upstream timer_create branch uses SIGEV_THREAD/pthreads. Select its
        # unmodified SIGALRM/setitimer implementation, matching the target ABI.
        vim_env['vim_cv_timer_create_works'] = 'no'
        run(vim, 'vim-configure.log', './configure', '--prefix=/usr', '--with-features=normal',
            '--enable-gui=no', '--without-x', '--without-wayland', '--disable-xsmp',
            '--disable-channel', '--disable-terminal', '--disable-netbeans', '--disable-gpm',
            '--disable-selinux', '--disable-smack', '--disable-xattr', '--disable-acl',
            '--disable-nls', '--with-tlib=ncursesw', env=vim_env)
        run(vim, 'vim-build.log', 'make', '-j' + jobs, env=env)

    host_flags = ['HOSTCC=gcc -static -no-pie', 'HOSTCFLAGS=-O2 -std=gnu11 -fno-pie', 'HOSTLDFLAGS=-static -no-pie']
    run(busybox, 'busybox-configure.log', 'make', *host_flags, 'allnoconfig')
    configuration = busybox / '.config'
    lines = configuration.read_text().splitlines()
    options = dict(line.split('=', 1) for line in config.read_text().splitlines()
                   if line.startswith('CONFIG_'))
    for i, line in enumerate(lines):
        name = line.split('=', 1)[0] if line.startswith('CONFIG_') else \
            line[2:-11] if line.startswith('# CONFIG_') and line.endswith(' is not set') else ''
        if name in options:
            lines[i] = name + '=' + options.pop(name)

    if options:
        raise SystemExit('Unknown BusyBox configuration options: ' + ', '.join(options))
    configuration.write_text('\n'.join(lines) + '\n')
    run(busybox, 'busybox-oldconfig.log', 'make', *host_flags, 'oldconfig', input=b'\n' * 1024)

    # Upstream patch ignores rename's result. Report replacement failure on
    # filesystems lacking that operation instead of claiming a patch succeeded.
    patch_source = busybox / 'editors/patch.c'
    patch_text = patch_source.read_text()
    assert patch_text.count('rename(TT.tempname, temp);') == 1
    patch_source.write_text(patch_text.replace('rename(TT.tempname, temp);',
                                               'if (rename(TT.tempname, temp))\n'
                                               '\t\t\t\tbb_perror_msg_and_die("cannot replace %s; edited data is in %s", temp, TT.tempname);'))
    # Native PTY sessions have inherited descriptors, not reopenable
    # /dev/pts names. Prefer their readable terminal streams so less never
    # opens the global console while running in the desktop Terminal.
    less_source = busybox / 'miscutils/less.c'
    less_text = less_source.read_text()
    tty_lookup = '\ttty_name = xmalloc_ttyname(STDOUT_FILENO);'
    tty_ready = '\tG.kbd_fd_orig_flags = ndelay_on(tty_fd);'
    assert less_text.count(tty_lookup) == less_text.count(tty_ready) == 1
    less_text = less_text.replace(tty_lookup,
        '\tif (isatty(STDIN_FILENO)) {\n'
        '\t\ttty_fd = dup(STDIN_FILENO);\n'
        '\t\tif (tty_fd >= 0) goto coast_tty_ready;\n'
        '\t}\n'
        '\tif ((fcntl(STDOUT_FILENO, F_GETFL) & O_ACCMODE) == O_RDWR) {\n'
        '\t\ttty_fd = dup(STDOUT_FILENO);\n'
        '\t\tif (tty_fd >= 0) goto coast_tty_ready;\n'
        '\t}\n' + tty_lookup)
    less_text = less_text.replace(tty_ready, ' coast_tty_ready:\n' + tty_ready)
    # A dup shares file-status flags with stdout. Render with blocking
    # output so a large screen cannot discard bytes when the PTY queue fills;
    # nonblocking mode is needed only for the actual keyboard read.
    replacements = (
        (tty_ready,
         '\tG.kbd_fd_orig_flags = fcntl(tty_fd, F_GETFL);\n'
         '\tif (G.kbd_fd_orig_flags < 0) bb_simple_perror_msg_and_die("fcntl");\n'
         '\tndelay_off(tty_fd);'),
        ('\tkey64 = safe_read_key(kbd_fd, kbd_input, /*timeout off:*/ -2);',
         '\tndelay_on(kbd_fd);\n'
         '\tkey64 = safe_read_key(kbd_fd, kbd_input, /*timeout off:*/ -2);\n'
         '\tint saved_errno = errno;\n'
         '\tndelay_off(kbd_fd);\n'
         '\terrno = saved_errno;'),
        ('\tif (!(G.kbd_fd_orig_flags & O_NONBLOCK))\n\t\tndelay_off(kbd_fd);',
         '\tif (G.kbd_fd_orig_flags & O_NONBLOCK)\n\t\tndelay_on(kbd_fd);\n'
         '\telse\n\t\tndelay_off(kbd_fd);'),
        ('\tif (width < 20 || max_displayed_line < 3)\n\t\treturn bb_cat(argv);',
         '\tif (width < 20 || max_displayed_line < 3) {\n'
         '\t\tfcntl(tty_fd, F_SETFL, G.kbd_fd_orig_flags);\n'
         '\t\treturn bb_cat(argv);\n\t}'),
    )
    # The console releases foreground ownership when fd 0 stops being a
    # terminal. Keep file data separate from the inherited keyboard stream.
    # Its canonical/raw transition also flushes typeahead, so hold raw mode
    # throughout the interactive session, with the existing restoration on
    # normal/error exit and fatal signals.
    replacements += (
        ('\tint kbd_fd;', '\tint kbd_fd;\n\tint input_fd;'),
        ('safe_read(STDIN_FILENO, readbuf, COMMON_BUFSIZE)',
         'safe_read(G.input_fd, readbuf, COMMON_BUFSIZE)'),
        ('\t\txmove_fd(xopen(filename, O_RDONLY), STDIN_FILENO);',
         '\t\tif (G.input_fd != STDIN_FILENO || !isatty(STDIN_FILENO))\n'
         '\t\t\tclose(G.input_fd);\n'
         '\t\tG.input_fd = xopen(filename, O_RDONLY);'),
        ('\tpfd[0].fd = STDIN_FILENO;', '\tpfd[0].fd = G.input_fd;'),
        ('\tset_tty_cooked();\n\treturn key64;', '\treturn key64;'),
        ('\trestore_tty();\n\texit_SUCCESS();', '\texit_SUCCESS();'),
        ('\tbuffer = xmalloc((max_displayed_line+1) * sizeof(char *));\n\treinitialize();',
         '\tif (atexit(restore_tty)) {\n'
         '\t\trestore_tty();\n'
         '\t\tbb_simple_error_msg_and_die("cannot register terminal cleanup");\n'
         '\t}\n'
         '\tif (tcsetattr(tty_fd, TCSANOW, &term_less))\n'
         '\t\tbb_simple_perror_msg_and_die("cannot configure terminal");\n'
         '\tbuffer = xmalloc((max_displayed_line+1) * sizeof(char *));\n'
         '\treinitialize();'),
    )
    for before, after in replacements:
        assert less_text.count(before) == 1
        less_text = less_text.replace(before, after)
    less_source.write_text(less_text)

    # Native foreground jobs without enabling upstream stopped-job machinery.
    ash_source = busybox / 'shell/ash.c'
    ash_text = ash_source.read_text()
    ash_replacements = (
        ('\trootpid = getpid();', '\trootpid = getpid();\n\tcoast_jobs_init();'),
        ('\tunsigned nprocs;        /* number of processes */',
         '\tunsigned nprocs;        /* number of processes */\n'
         '\tpid_t coast_group;\n\tint coast_foreground;'),
        ('static struct job *makejob(/*union node *,*/ int);', r'''
/* A native shell is a controller only at its own inherited session root.
 * Nested shells retain their foreground job's group. No SIGTSTP/fg/bg claim. */
static pid_t coast_controller;
static int coast_member;
static int coast_terminal = -1;
static int coast_attempted;
static void coast_jobs_init(void)
{
    int fd;
    pid_t self = getpid();
    if (coast_attempted)
        return;
    coast_attempted = 1;
    int controller = getpgrp() == self && getsid(0) == self;
    for (fd = 0; fd < 3; fd++) {
        int copy;
        if (!isatty(fd) || tcgetpgrp(fd) != getpgrp())
            continue;
        copy = fcntl(fd, F_DUPFD_CLOEXEC, 10);
        if (copy < 0)
            continue;
        if (!controller || setpgid(0, 0) == 0) {
            coast_terminal = copy;
            coast_member = 1;
            coast_controller = controller ? self : 0;
            return;
        }
        close(copy);
    }
}
static struct job *makejob(/*union node *,*/ int);
'''),
        ('\tpid = fork();\n\tif (pid < 0) {',
         '\tcoast_jobs_init();\n'
         '\tint managed = jp && mode != FORK_NOJOB && (coast_controller == getpid() || (coast_member && mode == FORK_BG));\n'
         '\tpid_t group = jp && jp->nprocs ? jp->ps[0].ps_pid : 0;\n'
         '\tpid = fork();\n\tif (pid < 0) {'),
        ('\t\tCLEAR_RANDOM_T(&random_gen); /* or else $RANDOM repeats in child */', r'''
        if (managed) {
            unsigned attempt;
            pid_t wanted = group ? group : getpid();
            if (setpgid(0, wanted))
                bb_perror_msg_and_die("foreground process group");
            if (mode == FORK_FG) {
                /* Parent publishes the complete pipeline before handing off.
                 * Never let less race TCSETS before foreground publication. */
                for (attempt = 0; attempt < 10000; attempt++) {
                    if (tcgetpgrp(coast_terminal) == wanted)
                        break;
                    usleep(1000);
                }
                if (attempt == 10000)
                    bb_error_msg_and_die("foreground handoff timed out");
            }
        }
        /* A foreground subshell retains its job group, but must still put
         * its own asynchronous children into separate background groups. */
        if (mode == FORK_BG && coast_terminal >= 0) {
            close(coast_terminal);
            coast_terminal = -1;
        }
        coast_member = coast_terminal >= 0;
        coast_controller = 0;
        CLEAR_RANDOM_T(&random_gen); /* or else $RANDOM repeats in child */
'''),
        ('\t\tforkparent(jp, n, mode, pid);', r'''
        if (managed) {
            pid_t wanted = group ? group : pid;
            if (setpgid(pid, wanted) && errno != ESRCH) {
                kill(pid, SIGKILL);
                ash_msg_and_raise_perror("foreground process group");
            }
            jp->coast_group = wanted;
            jp->coast_foreground = mode == FORK_FG;
        }
        forkparent(jp, n, mode, pid);
'''),
        ('\tint st;\n\n\tTRACE(("waitforjob',
         '\tint st;\n\tint handed = 0;\n\tstruct termios saved_terminal;\n'
         '\tif (jp && jp->coast_foreground && coast_controller == getpid()) {\n'
         '\t\tif (tcgetattr(coast_terminal, &saved_terminal) ||\n'
         '\t\t    tcsetpgrp(coast_terminal, jp->coast_group)) {\n'
         '\t\t\tkill(-jp->coast_group, SIGKILL);\n'
         '\t\t\tash_msg_and_raise_perror("foreground terminal handoff");\n'
         '\t\t}\n\t\thanded = 1;\n\t}\n\n\tTRACE(("waitforjob'),
        ('\tst = getstatus(jp);\n#if JOBS',
         '\tst = getstatus(jp);\n'
         '\tif (handed) {\n'
         '\t\tif (tcsetpgrp(coast_terminal, coast_controller) ||\n'
         '\t\t    tcsetattr(coast_terminal, TCSANOW, &saved_terminal))\n'
         '\t\t\tash_msg_and_raise_perror("restore foreground terminal");\n'
         '\t\tif (st == 128 + SIGINT)\n\t\t\traise(SIGINT);\n\t}\n#if JOBS'),
    )
    for before, after in ash_replacements:
        assert ash_text.count(before) == 1, (before, ash_text.count(before))
        ash_text = ash_text.replace(before, after)
    ash_source.write_text(ash_text)

    # The desktop command field intentionally leaves canonical ECHO off.
    # Interactive ash still owns a complete raw editor while it is foreground;
    # upstream's stty-noecho fallback would leave Tab in the GUI field instead.
    # The editor restores the original termios, including its ECHO state.
    lineedit_source = busybox / 'libbb/lineedit.c'
    lineedit_text = lineedit_source.read_text()
    fallback = 'if (n != 0 || (initial_settings.c_lflag & (ECHO|ICANON)) == ICANON) {'
    assert lineedit_text.count(fallback) == 1
    lineedit_source.write_text(lineedit_text.replace(fallback, 'if (n != 0) {'))

    run(busybox, 'busybox-build.log', 'make', '-j' + jobs, f'CC={env["CC"]}', *host_flags,
        'CFLAGS=-O2 -std=gnu11 -fno-pie -fno-stack-protector -mno-avx -mno-avx2',
        'EXTRA_LDFLAGS=-static -no-pie')

    if not only_busybox:
        # Coast retains its interactive PID-1 shell. Recipes need the real POSIX
        # shell. This is the only upstream source change in the GNU make port.
        job = make / 'src/job.c'
        text = job.read_text()
        assert text.count('const char *default_shell = "/bin/sh";') == 2
        job.write_text(text.replace('const char *default_shell = "/bin/sh";',
                                    'const char *default_shell = "/bin/ash";'))
        make_env = env.copy()

        # Named filesystem FIFOs are absent. Upstream pipe jobserver is supported.
        make_env['ac_cv_func_mkfifo'] = 'no'

        # Recipes receive private address spaces and admission-time CPU balancing.
        # The kernel retains vfork for musl spawn/GCC; make uses its fork fallback.
        make_env['ac_cv_func_vfork'] = 'no'
        run(make, 'make-configure.log', './configure', '--prefix=/usr', '--disable-nls',
            '--disable-posix-spawn', '--without-guile', env=make_env)
        run(make, 'make-build.log', 'make', '-j' + jobs, env=make_env)
    stamp.write_text(key)

bin_dir = Path(os.environ.get('COAST_PORTS_BIN', ROOT / 'userland/bin')).resolve()
bin_dir.mkdir(parents=True, exist_ok=True)
for source, name in pairs:
    shutil.copyfile(source, bin_dir / name)
    (bin_dir / name).chmod(source.stat().st_mode & 0o777)
if not only_busybox:
    runtime = ROOT / 'userland/runtime/usr/share/vim/vim92'
    if runtime.exists():
        shutil.rmtree(runtime)
    shutil.copytree(vim / 'runtime', runtime,
                    ignore=shutil.ignore_patterns('.git*', 'Makefile', '*.info'))
    shutil.copy2(ROOT / 'userland/config/vimrc', runtime.parent / 'vimrc')
licenses = Path(os.environ.get('COAST_PORTS_LICENSES', VENDOR / 'licenses')).resolve()
licenses.mkdir(exist_ok=True)

for source, name in ((vim / 'LICENSE', 'vim-LICENSE.txt'),
                     (vim / 'runtime/doc/uganda.txt', 'vim-uganda.txt'),
                     (busybox / 'LICENSE', 'busybox-LICENSE.txt'),
                     (make / 'COPYING', 'make-COPYING.txt')):
    if not only_busybox or name == 'busybox-LICENSE.txt':
        shutil.copyfile(source, licenses / name)

print('Built native BusyBox 1.37.0 (static musl, interactive completion).' if only_busybox else 'Built native Vim 9.2.0000, BusyBox 1.37.0 and GNU make 4.4.1 (static musl).')
