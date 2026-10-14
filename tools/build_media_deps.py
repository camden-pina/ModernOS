#!/usr/bin/env python3
"""Build the pinned CPU media decoders against the native static musl SDK."""
import argparse
import hashlib
import os
import shutil
import tarfile
from build_webcore_deps import ROOT, BUILD, PREFIX, CROSS, FLAGS, environment, run, stage, verify

ARCHIVE = 'ffmpeg-9.0.2.tar.xz'

# Official release signature was checked with FFmpeg's published release key,
# FCF986EA15E6E293A5644F10B4322F04D67658D8. Builds are subsequently offline.
DIGEST = '8c3850283eb25fa026482078a04051e0be17347b09ef81a0849bec15a96e002e'
NASM_ARCHIVE = 'nasm-2.16.03.tar.xz'
NASM_DIGEST = '1412a1c760bbd05db026b6c0d1657affd6631cd0a63cddb6f73cc6d4aa616148'


def host_assembler(jobs):
    archive = ROOT / 'userland/vendor' / NASM_ARCHIVE
    verify(archive, NASM_DIGEST)
    source = BUILD / 'host-tools/nasm-2.16.03'
    source.parent.mkdir(parents=True, exist_ok=True)
    if not source.exists():
        with tarfile.open(archive) as contents:
            contents.extractall(source.parent, filter='data')

    # NASM's out-of-tree Makefile expects matching object subdirectories.
    directory = BUILD / 'host-nasm-2.16.03'
    for subdirectory in source.rglob('*'):
        if subdirectory.is_dir():
            (directory / subdirectory.relative_to(source)).mkdir(parents=True, exist_ok=True)
    host_env = dict(os.environ, CC='cc', CFLAGS='-O2')
    stage('host-nasm-2.16.03', [source / 'configure'],
          [['make', f'-j{jobs}', 'nasm']], host_env, NASM_DIGEST,
          outputs=[directory / 'nasm'])
    assembler = BUILD / 'host-tools/nasm'
    shutil.copy2(BUILD / 'host-nasm-2.16.03/nasm', assembler)
    notices = ROOT / 'userland/vendor/licenses/nasm-NOTICES.txt'
    notices.parent.mkdir(parents=True, exist_ok=True)
    notices.write_text(
        'NASM 2.16.03, host build tool only.\n'
        f'Official source: https://www.nasm.us/pub/nasm/releasebuilds/2.16.03/{NASM_ARCHIVE}\n'
        f'Bundled source: userland/vendor/{NASM_ARCHIVE}\nSHA-256: {NASM_DIGEST}\n\n'
        + (source / 'LICENSE').read_text())
    return assembler


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--jobs', type=int, default=8)
    args = parser.parse_args()

    if not 1 <= args.jobs <= 32:
        parser.error('--jobs must be 1..32')
    BUILD.mkdir(parents=True, exist_ok=True)
    archive = ROOT / 'userland/vendor' / ARCHIVE
    verify(archive, DIGEST)
    source = BUILD / 'ffmpeg-9.0.2'
    if not source.exists():
        with tarfile.open(archive) as contents:
            contents.extractall(BUILD, filter='data')
    env = environment()
    assembler = host_assembler(args.jobs)
    options = [source / 'configure', f'--prefix={PREFIX}', '--enable-cross-compile',
               '--target-os=linux', '--arch=x86_64', f'--cc={CROSS}gcc', f'--ar={CROSS}ar',
               f'--ranlib={CROSS}ranlib', '--disable-shared', '--enable-static',
               '--disable-autodetect', '--disable-programs', '--disable-doc',
               '--disable-network', '--disable-avdevice', '--disable-avfilter',
               '--disable-everything', f'--x86asmexe={assembler}', '--disable-avx', '--disable-avx2',
               '--disable-avx512', '--disable-avx512icl', '--disable-xop', '--disable-fma3', '--disable-fma4',
               '--enable-pthreads', '--enable-swscale', '--enable-swresample',
               '--enable-decoder=h264,vp8,vp9,aac,mp3,opus,vorbis,pcm_s16le',
               '--enable-parser=h264,vp8,vp9,aac,mpegaudio,opus,vorbis',
               '--enable-demuxer=mov,matroska,ogg,mp3,wav,aac',
               '--enable-bsf=aac_adtstoasc,h264_mp4toannexb', f'--extra-cflags={FLAGS}',
               '--extra-ldflags=-static -no-pie']
    stage('ffmpeg-9.0.2-target', options,
          [['make', f'-j{args.jobs}'], ['make', 'install']], env, DIGEST,
          outputs=[PREFIX / ('lib/lib' + name + '.a')
                   for name in ('avcodec', 'avformat', 'avutil', 'swscale', 'swresample')]
          + [PREFIX / 'lib/pkgconfig/libavcodec.pc'])
    notices = ROOT / 'userland/vendor/licenses/ffmpeg-NOTICES.txt'
    notices.write_text(f'FFmpeg 9.0.2 CPU demuxers/decoders (LGPL 2.1 or later).\n'
                       f'Official source: https://ffmpeg.org/releases/{ARCHIVE}\n'
                       f'Bundled source: userland/vendor/{ARCHIVE}\nSHA-256: {DIGEST}\n'
                       'No GPL/nonfree components, network protocols, device access or external plugins.\n'
                       'Build recipe: tools/build_media_deps.py\n\n' + (source / 'COPYING.LGPLv2.1').read_text())
    shutil.copy2(source / 'LICENSE.md', ROOT / 'userland/vendor/licenses/ffmpeg-LICENSE.md')

    print('Built native FFmpeg CPU media libraries.', flush=True)


if __name__ == '__main__':
    main()
