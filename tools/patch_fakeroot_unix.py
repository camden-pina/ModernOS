"""Use private Unix sockets for upstream fakeroot's existing stream transport.

No fake credentials enter the kernel. This replaces TCP/SysV IPC, which the
ordinary Coast account ABI does not expose. The upstream metadata protocol,
preload wrappers, package functions and daemon logic are retained.
"""
from pathlib import Path


def patch_musl(source):
    path = Path(source) / 'configure.ac'
    text = path.read_text()
    original = text
    if 'AC_EGREP_CPP([time64],[' in text:
        start = text.index('AC_EGREP_CPP([time64],[')
        end = text.index('\n:>fakerootconfig.h.tmp', start)
        text = text[:start] + '''dnl Compile-only test: musl has native 64-bit time, without glibc aliases.
AC_COMPILE_IFELSE([AC_LANG_PROGRAM([[
#include <features.h>
#ifndef __GLIBC__
#error not glibc
#else
#include <bits/wordsize.h>
#if __WORDSIZE != 32
#error native 64-bit ABI
#endif
#endif
]], [[]])],
 [AC_MSG_RESULT([yes]); AC_DEFINE(TIME64_HACK)
time64_hack=yes], [AC_MSG_RESULT([no])])
''' + text[end:]
    marker = 'Coast: absent glibc aliases need no extra wrapper'
    if marker in text:
        path.write_text(text)
        return text != original
    old = '  done\ndone\n\nif test "x$time64_hack" = "xyes"; then'
    assert text.count(old) == 1
    text = text.replace(old, '''  done
  dnl Coast: absent glibc aliases need no extra wrapper.
  if test -z "$FOUND"; then
    PF=[`echo ${PRE}${FUNC}| tr '[a-z]' '[A-Z]'`]
    AC_DEFINE_UNQUOTED(NO_WRAP_${PF}_SYMBOL, 1)
  fi
done

if test "x$time64_hack" = "xyes"; then''')
    path.write_text(text)
    return True


def patch(source):
    source = Path(source)
    path = source / 'communicate.c'
    text = path.read_text()
    if '#include <sys/un.h>' in text:
        return
    text = text.replace('#include "communicate.h"', '#include "communicate.h"\n#include <sys/un.h>')
    start = text.index('  static struct sockaddr_in addr = { 0, 0, { 0 } };')
    end = text.index('\n  return (struct sockaddr *) &addr;', start)
    text = text[:start] + '''  static struct sockaddr_un addr;
  if (!addr.sun_path[0]) {
    const char *key = env_var_set(FAKEROOTKEY_ENV);
    if (!key || key[0] != '/' || strlen(key) >= sizeof(addr.sun_path)) {
      errno = EINVAL;
      fail("invalid private socket in FAKEROOTKEY");
    }
    addr.sun_family = AF_UNIX;
    strcpy(addr.sun_path, key);
  }
''' + text[end:]
    text = text.replace('comm_sd = socket(PF_INET, SOCK_STREAM, 0);', 'comm_sd = socket(AF_UNIX, SOCK_STREAM, 0);')
    text = text.replace('''  int val = 1;
  if (setsockopt(comm_sd, IPPROTO_TCP, TCP_NODELAY, &val, sizeof (val)) < 0)
      fail("setsockopt(TCP_NODELAY)");
''', '')
    text = text.replace('connect(comm_sd, get_addr(), sizeof (struct sockaddr_in))',
                        'connect(comm_sd, get_addr(), sizeof (struct sockaddr_un))')
    path.write_text(text)
    path = source / 'faked.c'
    text = path.read_text()
    text = text.replace('#include <sys/stat.h>', '#include <sys/stat.h>\n#include <sys/un.h>\nstatic char private_dir[64], private_socket[108];')
    # The listener does not use the peer address for protocol decisions.
    text = text.replace('struct sockaddr_in addr;', 'struct sockaddr_un addr;')
    start = text.index('  sd = socket(PF_INET, SOCK_STREAM, 0);')
    end = text.index('  sa_detach.sa_handler=detach;', start)
    text = text[:start] + '''  sd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (sd < 0) fail("socket");
  if (port) { errno = EINVAL; fail("numeric TCP ports are unavailable in this port"); }
  strcpy(private_dir, "/tmp/fakeroot-XXXXXX");
  if (!mkdtemp(private_dir)) fail("mkdtemp");
  snprintf(private_socket, sizeof(private_socket), "%s/socket", private_dir);
  memset(&addr, 0, sizeof(addr));
  addr.sun_family = AF_UNIX;
  strcpy(addr.sun_path, private_socket);
  if (bind(sd, (struct sockaddr *)&addr, sizeof(addr)) < 0) fail("bind");
  if (listen(sd, SOMAXCONN) < 0) fail("listen");

''' + text[end:]
    text = text.replace('      if (debug) {\n\tchar host[256];', '''      struct ucred peer;
      socklen_t peer_size = sizeof(peer);
      if (getsockopt(sd, SOL_SOCKET, SO_PEERCRED, &peer, &peer_size) || peer.uid != getuid()) {
        close(sd);
        continue;
      }

      if (debug) {
\tchar host[256];''')
    text = text.replace('  save(0);\n\n  if(g!=-1)', '''  save(0);
#ifdef FAKEROOT_FAKENET
  if (private_socket[0]) unlink(private_socket);
  if (private_dir[0]) rmdir(private_dir);
#endif

  if(g!=-1)''')
    text = text.replace('      printf("%li:%i\\n",(long)FAKE_KEY,pid);', '''#ifdef FAKEROOT_FAKENET
      printf("%s:%i\\n",private_socket,pid);
#else
      printf("%li:%i\\n",(long)FAKE_KEY,pid);
#endif''')
    text = text.replace('    printf("%li:%i\\n",(long)FAKE_KEY,getpid());', '''#ifdef FAKEROOT_FAKENET
    printf("%s:%i\\n",private_socket,getpid());
#else
    printf("%li:%i\\n",(long)FAKE_KEY,getpid());
#endif''')
    path.write_text(text)
