#!/usr/bin/env python3
import pathlib,subprocess,tempfile
ROOT=pathlib.Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix='coast-game-scale-') as temporary:
 output=pathlib.Path(temporary)/'check'
 subprocess.run(['gcc','-O1','-g','-std=gnu11','-Wall','-Wextra','-Werror','-Wno-misleading-indentation','-ffreestanding','-fno-builtin','-ffunction-sections','-fdata-sections','-fsanitize=address,undefined','-fno-omit-frame-pointer','-I'+str(ROOT/'userland/include'),'-I'+str(ROOT/'include'),str(ROOT/'tools/tests/game_scale.c'),'-Wl,--gc-sections','-o',str(output)],check=True)
 subprocess.run([str(output)],check=True)
