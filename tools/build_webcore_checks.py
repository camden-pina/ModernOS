#!/usr/bin/env python3
"""Link new genuine WebCore-dependency checks against the preserved target SDK."""
import shlex
import subprocess
import argparse
from build_webcore_deps import ROOT, BUILD, PREFIX, FLAGS, environment, run


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--only', nargs='+',
                        choices=['webcore-deps-check', 'web-soup-check', 'web-broker'])
    args = parser.parse_args()

    BUILD.mkdir(parents=True, exist_ok=True)
    (ROOT / 'userland/bin').mkdir(parents=True, exist_ok=True)
    env = environment()
    base = ['-Wall', '-Wextra', '-Werror', '-pthread', '-static', '-no-pie']
    for name, packages, extra in (
        ('webcore-deps-check', ['harfbuzz', 'fontconfig', 'freetype2', 'libwebp', 'libjpeg',
                                'libxml-2.0', 'sqlite3', 'libgcrypt', 'libpsl'], []),
        ('web-soup-check', ['libsoup-3.0', 'gio-unix-2.0', 'gthread-2.0', 'openssl'],
         [str(PREFIX / 'lib/gio/modules/libgioopenssl.a'),
          str(BUILD / 'glib-networking-2.80.1-target/tls/base/libtlsbase.a')]),
        ('web-broker', ['libsoup-3.0', 'gio-unix-2.0', 'gthread-2.0', 'openssl', 'sqlite3'],
         [str(PREFIX / 'lib/gio/modules/libgioopenssl.a'),
          str(BUILD / 'glib-networking-2.80.1-target/tls/base/libtlsbase.a')]),
    ):
        if args.only and name not in args.only:
            continue
        cflags = shlex.split(subprocess.check_output(
            ['pkg-config', '--cflags', *packages], env=env, text=True))
        libs = shlex.split(subprocess.check_output(
            ['pkg-config', '--static', '--libs', *packages], env=env, text=True))
        run(ROOT, name + '-build', [env['CC'], *FLAGS.split(), *base, *cflags,
            ROOT / 'userland/modern-browser' / (name + '.c'), '-Wl,--start-group',
            *extra, *libs, f'-L{PREFIX}/lib64', '-lstdc++', '-lm', '-pthread', '-Wl,--end-group',
            '-o', ROOT / 'userland/bin' / name], env)


if __name__ == '__main__':
    main()
