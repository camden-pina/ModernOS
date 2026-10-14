#!/usr/bin/env python3
"""Host checks for shared boot policy, selector errors and fresh ISO staging."""
import argparse
from dataclasses import replace
from pathlib import Path
import subprocess
import shutil
import tempfile
import unittest
from unittest.mock import patch

from boot_config import read_config, efi_config, limine_config, grub_config, refind_config, TEMPLATES
from build_boot_iso import build, stage_payload, install_limine, install_refind, make_efi_volume

ROOT = Path(__file__).resolve().parents[1]
POLICY = 'version = 1\n[boot]\ntitle = "OS"\nkernel = "/kernel"\n'


class BootConfigTests(unittest.TestCase):
    def test_repository_policy(self):
        config = read_config(ROOT / "arch/x86/boot/boot.conf")
        self.assertEqual(config.title, "ModernOS")
        self.assertEqual(config.kernel, "/boot/kernel.elf")
        self.assertEqual([(m.path, m.string) for m in config.modules],
                         [("/boot/initramfs.tar", "initramfs")])
        self.assertEqual(config.resolution, "2560x1600x32")
        self.assertIn("set timeout=0\n", grub_config(config))
        self.assertIn("timeout -1\n", refind_config(config))
        self.assertIn("resolution 2560 1600\n", refind_config(config))
        self.assertIn("module=/boot/initramfs.tar\nmodule_string=initramfs\n", efi_config(config))
        self.assertIn("chainloader /EFI/ModernOS/modernos.efi", grub_config(config))
        self.assertIn("loader /EFI/ModernOS/modernos.efi", refind_config(config))
        for command in ("reboot", "halt", "fwsetup"):
            self.assertIn(f"    {command}\n", grub_config(config))
        self.assertIn("showtools reboot,shutdown,firmware", refind_config(config))

    def parse(self, text):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "boot.conf"
            path.write_text(text)
            return read_config(path)

    def test_custom_paths_modules_and_command_line(self):
        config = self.parse('''version = 1
[boot]
title = "Custom OS"
kernel = "/images/os.elf"
cmdline = "debug=1 root=ram"
[[boot.modules]]
path = "/images/root.tar"
string = "initramfs"
[[boot.modules]]
path = "/images/font.psf"
string = "font"
[menu]
timeout = 8
resolution = "1280x720"
''')
        self.assertEqual(config.kernel, "/images/os.elf")
        self.assertEqual(len(config.modules), 2)
        self.assertIn("cmdline=debug=1 root=ram", efi_config(config))
        self.assertIn("module=/images/font.psf\nmodule_string=font", efi_config(config))
        self.assertIn('menuentry "Custom OS"', grub_config(config))
        self.assertIn("timeout 8\n", refind_config(config))
        self.assertIn("cmdline: debug=1 root=ram", limine_config(config))
        self.assertIn("module_path: boot():/images/font.psf", limine_config(config))

    def test_rejects_ambiguous_or_unloadable_policy(self):
        cases = (
            POLICY.replace('/kernel', '/../kernel'),
            POLICY.replace('/kernel', 'tftp():/kernel'),
            POLICY + '[[boot.modules]]\nstring = "orphan"\n',
            POLICY + '[menu]\ntimeout = "forever"\n',
            POLICY + 'resolution = "0x768"\n',
            POLICY + '[[boot.modules]]\npath = "/kernel"\n',
            POLICY + 'kernel = "/other"\n',
            POLICY.replace('version = 1', 'version = 2'),
            POLICY.replace('version = 1', 'version = true'),
            POLICY + '[menu]\ntimeout = true\n',
            POLICY + 'cmdline = "line\\ncommand"\n',
            POLICY + 'cmdline = "' + 'x' * 512 + '"\n',
            POLICY + 'resolution = "4294967296x720"\n',
        )
        for text in cases:
            with self.subTest(text=text), self.assertRaises(ValueError):
                self.parse(text)

    def test_unknown_shared_settings_are_rejected(self):
        for source, setting in ((POLICY + 'verbose = true\n', 'boot.verbose'),
                                (POLICY + '[menu]\ntimeuot = 2\n', 'menu.timeuot'),
                                (POLICY + '[grub]\ntimeout_style = "hidden"\n', 'boot.conf.grub'),
                                (POLICY + '[[boot.modules]]\npath = "/root"\nstrng = "initramfs"\n', 'strng')):
            with self.subTest(setting=setting), self.assertRaisesRegex(ValueError, setting):
                self.parse(source)

    def test_timeout_behavior_is_portable(self):
        for timeout, limine, grub, refind in ((-1, 'no', '-1', '0'), (0, '0', '0', '-1'), (8, '8', '8', '8')):
            config = self.parse(POLICY + f'[menu]\ntimeout = {timeout}\n')
            self.assertIn(f'timeout: {limine}\n', limine_config(config))
            self.assertIn(f'set timeout={grub}\n', grub_config(config))
            self.assertIn(f'timeout {refind}\n', refind_config(config))

    def test_native_settings_stay_in_the_selected_template(self):
        config = self.parse(POLICY)
        renderers = {'limine': limine_config, 'grub': grub_config, 'refind': refind_config}
        additions = {'limine': 'verbose: yes',
                     'grub': 'set timeout_style=hidden', 'refind': 'big_icon_size 128'}
        with tempfile.TemporaryDirectory() as temporary:
            for loader, renderer in renderers.items():
                template = Path(temporary) / loader
                template.write_text(TEMPLATES[loader].read_text().replace('{{BOOT_ENTRY}}',
                                    additions[loader] + '\n\n{{BOOT_ENTRY}}'))
                self.assertIn(additions[loader], renderer(config, template))
                for other, other_renderer in renderers.items():
                    if other != loader:
                        self.assertNotIn(additions[loader], other_renderer(config))

            # A string in user data is not a second template program.
            self.assertIn('/Title {{TIMEOUT}}',
                          limine_config(replace(config, title='Title {{TIMEOUT}}')))

    def test_invalid_templates_preserve_previous_staging(self):
        config_file = ROOT / 'arch/x86/boot/boot.conf'
        with tempfile.TemporaryDirectory() as temporary:
            base = Path(temporary)
            root = base / 'stage'
            root.mkdir()
            marker = root / 'previous-image'
            marker.write_bytes(b'preserve this image')
            template = base / 'grub.cfg.in'
            source = TEMPLATES['grub'].read_text()
            cases = (source + '\n{{UNKNOWN}}\n', source.replace('{{WALLPAPER}}', ''),
                     source.replace('{{MENU_RESOLUTION}}', ''), source.replace('{{TIMEOUT}}', '0'),
                     source.replace('{{BOOT_ENTRY}}', ''), source + '\n{{BOOT_ENTRY}}\n')
            for content in cases:
                template.write_text(content)
                args = argparse.Namespace(config=config_file, root=root,
                                          bootloader='grub', template=template)
                with self.subTest(content=content), self.assertRaises(ValueError):
                    build(args)
                self.assertEqual(marker.read_bytes(), b'preserve this image')

    def test_unsupported_depth_is_reported(self):
        config = self.parse(POLICY + 'resolution = "1280x720x16"\n')
        self.assertIn('resolution: 1280x720x16', limine_config(config))
        for renderer in (grub_config, refind_config):
            with self.assertRaisesRegex(ValueError, '32-bit framebuffer'):
                renderer(config)
        config = replace(config, resolution='1280x720x32', menu_resolution='1280x720x16')
        with self.assertRaisesRegex(ValueError, 'menu.resolution'):
            refind_config(config)

    def test_invalid_make_selector(self):
        for selector in ("unknown", "limine grub", ""):
            result = subprocess.run(["make", "--no-print-directory", "-s", "help", f"BOOTLOADER={selector}"],
                                    cwd=ROOT, capture_output=True, text=True)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("BOOTLOADER", result.stderr)

    def test_in_tree_clean_preserves_efi_sources(self):
        for target in ("clean", "efi-clean"):
            result = subprocess.run(["make", "--no-print-directory", "-n", "O=.", target],
                                    cwd=ROOT, capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            source = str(ROOT / "arch/x86/boot/efi")
            self.assertNotIn(f'rm -rf "{source}', result.stdout)
            self.assertNotIn(f"rm -rf {source}", result.stdout)
            self.assertIn(source + "/modernos.efi", result.stdout)

    def test_switching_loaders_replaces_staging(self):
        with tempfile.TemporaryDirectory() as temporary:
            base = Path(temporary)
            config_file = base / "arch/x86/boot/boot.conf"
            config_file.parent.mkdir(parents=True)
            config_file.write_text(POLICY.replace('/kernel', '/images/kernel.elf') +
                                   '[[boot.modules]]\npath = "/images/root.tar"\nstring = "initramfs"\n')
            kernel, initramfs, efi, refind = [
                base / name for name in ("vmlinux", "root.tar", "os.efi", "refind.efi")]
            for path in (kernel, initramfs, efi, refind):
                path.write_bytes(path.name.encode())
            limine = base / "limine"
            limine.mkdir()
            for name in ("limine-bios.sys", "limine-bios-cd.bin", "limine-uefi-cd.bin", "BOOTX64.EFI"):
                (limine / name).write_bytes(b"limine:" + name.encode())
            args = argparse.Namespace(root=base / "stage", config=config_file, kernel=kernel,
                                      initramfs=initramfs, efi_image=efi, module_file=[],
                                      bootloader="refind", refind_efi=refind, limine_dir=limine)
            config = read_config(config_file)
            root = stage_payload(args, config)
            install_refind(args, root, config)

            self.assertEqual((root / "EFI/BOOT/BOOTX64.EFI").read_bytes(), b"refind.efi")
            self.assertTrue((root / "EFI/BOOT/refind.conf").is_file())
            (root / "stale-file").write_text("must disappear")
            args.bootloader = "limine"
            root = stage_payload(args, config)
            install_limine(args, root, config)

            self.assertIn('protocol: limine', (root / 'boot/limine/limine.conf').read_text())
            self.assertNotIn('version = 1', (root / 'boot/limine/limine.conf').read_text())
            self.assertFalse((root / "EFI/BOOT/refind.conf").exists())
            self.assertFalse((root / "stale-file").exists())
            self.assertEqual((root / "images/kernel.elf").read_bytes(), kernel.read_bytes())
            self.assertEqual((root / "images/root.tar").read_bytes(), initramfs.read_bytes())

            args.bootloader = "grub"
            root = stage_payload(args, config)
            self.assertFalse((root / "boot/limine").exists())
            self.assertFalse((root / "EFI/BOOT/BOOTX64.EFI").exists())

    def test_nonfirst_initramfs_and_module_overrides(self):
        with tempfile.TemporaryDirectory() as temporary:
            base = Path(temporary)
            config_file = base / "boot.conf"
            config_file.write_text(POLICY.replace('/kernel', '/custom/kernel.elf') +
                                   '[[boot.modules]]\npath = "/custom/font.bin"\nstring = "font"\n'
                                   '[[boot.modules]]\npath = "/custom/ramdisk.tar"\nstring = "initramfs"\n'
                                   '[[boot.modules]]\npath = "/custom/extra.bin"\nstring = "data"\n')
            kernel, initramfs, efi, font = [
                base / name for name in ("vmlinux", "root.tar", "os.efi", "font.bin")]
            for path in (kernel, initramfs, efi, font):
                path.write_bytes(path.name.encode())
            extra = base / "source/custom/extra.bin"
            extra.parent.mkdir(parents=True)
            extra.write_bytes(b"extra source")
            args = argparse.Namespace(root=base / "stage", config=config_file, kernel=kernel,
                                      initramfs=initramfs, efi_image=efi,
                                      module_file=[f"/custom/font.bin={font}"],
                                      source_root=base / "source", bootloader="grub")
            root = stage_payload(args, read_config(config_file))
            self.assertEqual((root / "custom/kernel.elf").read_bytes(), kernel.read_bytes())
            self.assertEqual((root / "custom/ramdisk.tar").read_bytes(), initramfs.read_bytes())
            self.assertEqual((root / "custom/font.bin").read_bytes(), font.read_bytes())
            self.assertEqual((root / "custom/extra.bin").read_bytes(), b"extra source")

    def test_extra_assets_and_conflict_detection(self):
        with tempfile.TemporaryDirectory() as temporary:
            base = Path(temporary)
            config_file = base / 'boot.conf'
            config_file.write_text(POLICY)
            kernel, initramfs, efi, asset = [
                base / name for name in ('vmlinux', 'root.tar', 'os.efi', 'theme.txt')]
            for path in (kernel, initramfs, efi, asset):
                path.write_bytes(path.name.encode())
            args = argparse.Namespace(root=base / 'stage', config=config_file, kernel=kernel,
                                      initramfs=initramfs, efi_image=efi, module_file=[], bootloader='grub',
                                      asset_file=[f'/boot/grub/themes/custom/theme.txt={asset}'])
            config = read_config(config_file)
            root = stage_payload(args, config)
            staged = root / 'boot/grub/themes/custom/theme.txt'
            self.assertEqual(staged.read_bytes(), asset.read_bytes())

            for destination in ('/kernel', '/boot', '/boot/grub/grub.cfg', '/efi/boot/bootx64.efi',
                                '/efi/boot/icons/custom.png'):
                args.asset_file = [f'{destination}={asset}']
                with self.subTest(destination=destination), self.assertRaisesRegex(ValueError, 'conflicts'):
                    stage_payload(args, config)
                self.assertEqual(staged.read_bytes(), asset.read_bytes())

            args.asset_file = [f'/extra.txt={staged}']
            with self.assertRaisesRegex(ValueError, 'outside the disposable'):
                stage_payload(args, config)

            self.assertEqual(staged.read_bytes(), asset.read_bytes())
            args.asset_file = []
            template = root / 'grub.cfg.in'
            template.write_text(TEMPLATES['grub'].read_text())
            with patch.dict(TEMPLATES, grub=template):
                with self.assertRaisesRegex(ValueError, 'outside the disposable'):
                    stage_payload(args, config)

            self.assertEqual(staged.read_bytes(), asset.read_bytes())

    def test_refind_assets_override_bundled_icons(self):
        with tempfile.TemporaryDirectory() as temporary:
            base = Path(temporary)
            config_file = base / 'boot.conf'
            config_file.write_text(POLICY)
            kernel, initramfs, efi, icon = [
                base / name for name in ('vmlinux', 'root.tar', 'refind.efi', 'icon.png')]
            for path in (kernel, initramfs, efi, icon):
                path.write_bytes(path.name.encode())
            (base / 'icons').mkdir()
            (base / 'icons/custom.png').write_bytes(b'default icon')
            (base / 'icons/other.png').write_bytes(b'other bundled icon')
            args = argparse.Namespace(root=base / 'stage', config=config_file, kernel=kernel,
                                      initramfs=initramfs, efi_image=efi, refind_efi=efi, module_file=[],
                                      bootloader='refind', asset_file=[f'/EFI/BOOT/icons/custom.png={icon}'])
            config = read_config(config_file)
            root = stage_payload(args, config)
            install_refind(args, root, config)

            self.assertEqual((root / 'EFI/BOOT/icons/custom.png').read_bytes(), icon.read_bytes())
            self.assertEqual((root / 'EFI/BOOT/icons/other.png').read_bytes(),
                             b'other bundled icon')

    @unittest.skipUnless(shutil.which("mformat") and shutil.which("mcopy"), "FAT check requires mtools")

    def test_fat_volume_contains_arbitrary_guest_paths(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary) / "stage"
            for name, payload in (("images/kernel.elf", b"kernel"),
                                  ("modules/root.tar", b"initramfs"),
                                  ("standalone.bin", b"standalone"),
                                  ("EFI/BOOT/BOOTX64.EFI", b"boot manager"),
                                  ("boot/modernos.conf", b"kernel=/images/kernel.elf\n")):
                target = root / name
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_bytes(payload)

            make_efi_volume(argparse.Namespace(mformat="mformat", mcopy="mcopy"), root)

            for guest in ("images/kernel.elf", "modules/root.tar", "standalone.bin"):
                extracted = Path(temporary) / "extracted"
                subprocess.run(["mcopy", "-i", str(root / "efiboot.img"),
                                "::/" + guest, str(extracted)], check=True)
                self.assertEqual(extracted.read_bytes(), (root / guest).read_bytes())
                extracted.unlink()

            result = subprocess.run(["mcopy", "-i", str(root / "efiboot.img"),
                                     "::/efiboot.img", str(Path(temporary) / "recursive")],
                                    capture_output=True)
            self.assertNotEqual(result.returncode, 0)


if __name__ == "__main__":
    unittest.main()
