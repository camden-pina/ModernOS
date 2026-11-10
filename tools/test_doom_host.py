#!/usr/bin/env python3
"""Check native Doom ABI declarations and real DMX decoding/mixing edge cases."""
from pathlib import Path
import subprocess
import tempfile
from build_doom import BUILD, PORT, ROOT, source_archive


def main():
    archive, manifest = source_archive(check_only=True)
    source = BUILD / 'source' / manifest['directory'] / 'doomgeneric'
    if not source.exists():
        raise SystemExit('Run make doom first to restore the checked engine headers.')
    binary = ROOT / 'userland/bin/doom'
    help_result = subprocess.run([str(binary), '--help'], capture_output=True, text=True)
    assert help_result.returncode == 0 and 'WASD' in help_result.stdout, help_result
    for arguments in (['-scaling', '2'], ['-SCALING', '2'], ['-ScAlInG', '2'],
                      ['-GFXMODE', 'rgb565'], ['@response-file'], ['--frames'],
                      ['--frames', '0'], ['--frames', 'invalid']):
        result = subprocess.run([str(binary), *arguments], capture_output=True, text=True)
        assert result.returncode == 2, (arguments, result)
    with tempfile.TemporaryDirectory(prefix='coast-doom-host-') as temporary:
        work = Path(temporary)
        abi = work / 'abi.c'
        abi.write_text('''#include <uapi/abi.h>
#include "coast_protocol.h"
_Static_assert(SYS_display == DOOM_SYS_DISPLAY, "display syscall");
_Static_assert(SYS_input == DOOM_SYS_INPUT, "input syscall");
_Static_assert(SYS_audio == DOOM_SYS_AUDIO, "audio syscall");
_Static_assert(DISPLAY_PRESENT_SWAP == DOOM_DISPLAY_PRESENT_SWAP, "present operation");
_Static_assert(DISPLAY_RELEASE == DOOM_DISPLAY_RELEASE, "release operation");
_Static_assert(INPUT_HID_KEY == DOOM_INPUT_HID_KEY, "HID event");
_Static_assert(INPUT_POINTER == DOOM_INPUT_POINTER, "pointer event");
_Static_assert(sizeof(struct os_display_info) == sizeof(struct doom_display_info), "display info");
_Static_assert(sizeof(struct os_input_event) == sizeof(struct doom_input_event), "input event");
_Static_assert(sizeof(struct os_display_batch) == sizeof(struct doom_display_batch), "display batch");
int main(void) { return 0; }
''')
        subprocess.run(['gcc', '-std=gnu11', '-I' + str(ROOT / 'include'), '-I' + str(PORT),
                        str(abi), '-o', str(work / 'abi')], check=True)
        subprocess.run([str(work / 'abi')], check=True)
        harness = PORT / 'tests/sound_harness.c'
        output = work / 'sound-test'
        subprocess.run(['gcc', '-O1', '-g', '-std=gnu11', '-fsanitize=address,undefined',
                        '-fno-omit-frame-pointer', '-I' + str(source), '-I' + str(PORT),
                        '-idirafter', str(ROOT / 'include'), str(harness), str(PORT / 'i_coast_sound.c'), str(PORT / 'coast_midi.c'), '-lm',
                        '-o', str(output)], check=True)
        subprocess.run([str(output)], check=True)
        platform = work / 'platform.o'
        subprocess.run(['gcc', '-O1', '-g', '-std=gnu11', '-fsanitize=address,undefined',
                        '-ffunction-sections', '-fdata-sections', '-Dmain=Doom_GuestMain',
                        '-Dsyscall=Doom_TestSyscall', '-I' + str(source), '-I' + str(PORT),
                        '-c', str(PORT / 'doomgeneric_coast.c'), '-o', str(platform)], check=True)
        input_test = work / 'input-test'
        subprocess.run(['gcc', '-O1', '-g', '-std=gnu11', '-fsanitize=address,undefined',
                        '-Wl,--gc-sections', '-I' + str(source), '-I' + str(PORT),
                        str(PORT / 'tests/input_harness.c'), str(platform), '-o', str(input_test)], check=True)
        subprocess.run([str(input_test)], check=True)
    print('PASS native Doom host: public ABI layout; DMX bounds; 48kHz resampling; stereo panning; '
          'mix clipping; EAGAIN cursor rollback; sound lifetime; bounded input burst delivery; '
          'menu ASCII; alias key releases; kernel overflow recovery (ASan/UBSan).')


if __name__ == '__main__':
    main()
