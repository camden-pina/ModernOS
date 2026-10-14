#!/usr/bin/env python3
"""Package one selected boot manager and the shared ModernOS boot payload."""
import argparse
import os
from pathlib import Path, PurePosixPath
import shutil
import subprocess
import sys

from boot_config import read_config, efi_config, loader_config, local_path, TEMPLATES


def run(command):
    subprocess.run([str(argument) for argument in command], check=True)


def require_tool(command, hint=""):
    if not command or not shutil.which(command):
        raise ValueError(f"Required host tool {command!r} was not found. {hint}")


def copy_file(source: Path, root: Path, guest: str):
    if not source.is_file():
        raise ValueError(f"Missing boot file: {source}")
    target = root / local_path(guest).lstrip("/")
    target.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(source, target)
    return target


def add_destination(occupied, guest, label):
    # UEFI FAT filesystems are case insensitive; also reject file/directory
    # collisions before replacing an existing staging tree.
    path = PurePosixPath(guest.casefold())
    if any(path == other or path in other.parents or other in path.parents
           for other in occupied):
        raise ValueError(f"{label} destination conflicts with a boot file: {guest}")

    directories = {str(parent).casefold(): str(parent)
                   for previous in occupied.values() for parent in previous.parents}
    original = PurePosixPath(guest)
    for parent in original.parents:
        existing = directories.get(str(parent).casefold())
        if existing is not None and existing != str(parent):
            raise ValueError(f"{label} directory casing conflicts with {existing}: {guest}")
    occupied[path] = original


def stage_payload(args, config):
    root = args.root.resolve()
    source_root = getattr(args, "source_root", Path(__file__).resolve().parents[1])

    # O=. is supported; the staging directory must still be a separate child.
    if root == Path.cwd().resolve() or root in Path.cwd().resolve().parents:
        raise ValueError(f"Refusing to clear unsafe ISO staging directory: {root}")
    inputs = [args.kernel, args.initramfs, args.config, args.efi_image,
              getattr(args, "template", None) or TEMPLATES[args.bootloader]]
    if any(path and path.resolve().is_relative_to(root) for path in inputs):
        raise ValueError("Boot inputs must be outside the disposable ISO staging directory")
    for path in [args.kernel, args.initramfs]:
        if not path.is_file():
            raise ValueError(
                f"Missing {path}; build it first, or use 'make iso' to build the initramfs")

    module_sources = {}
    for mapping in args.module_file:
        guest, separator, host = mapping.partition("=")
        if not separator:
            raise ValueError("--module-file requires GUEST_PATH=HOST_PATH")
        module_sources[local_path(guest)] = Path(host)

    initramfs_modules = [module for module in config.modules if module.string == "initramfs"]
    if not initramfs_modules:
        initramfs_modules = [module for module in config.modules if Path(
            module.path).name == "initramfs.tar"]
    if not initramfs_modules and len(config.modules) == 1:
        initramfs_modules = config.modules

    if len(initramfs_modules) > 1:
        raise ValueError("Only one configured module may identify the initramfs")
    initramfs_path = initramfs_modules[0].path if initramfs_modules else None
    modules = []
    for module in config.modules:
        source = module_sources.pop(module.path, None)
        if source is None:
            source = args.initramfs if module.path == initramfs_path else source_root / module.path.lstrip("/")
        if not source.is_file():
            raise ValueError(
                f"Missing module {module.path}: supply BOOT_MODULES='{module.path}=/host/file'")
        modules.append((source, module.path))

    if module_sources:
        raise ValueError(
            f"Module override is absent from {args.config}: {next(iter(module_sources))}")

    destinations = [config.kernel, *(guest for _, guest in modules)]
    if len(destinations) != len(set(destinations)):
        raise ValueError("Kernel and module paths must be distinct")
    wallpaper_source = source_root / config.wallpaper.lstrip("/") if config.wallpaper else None

    # The shared configuration stores its wallpaper beside boot.conf.
    if wallpaper_source and not wallpaper_source.is_file():
        wallpaper_source = args.config.parent / Path(config.wallpaper).name
    if wallpaper_source and not wallpaper_source.is_file():
        wallpaper_source = source_root / "arch/x86/boot" / Path(config.wallpaper).name

    if wallpaper_source and not wallpaper_source.is_file():
        raise ValueError(f"Missing configured wallpaper: {wallpaper_source}")
    reserved = {"/boot/modernos.conf", "/boot/limine/limine.conf",
                "/boot/limine/limine-bios.sys", "/boot/limine/limine-bios-cd.bin",
                "/boot/limine/limine-uefi-cd.bin", "/boot/grub/grub.cfg",
                "/EFI/BOOT/BOOTX64.EFI", "/EFI/BOOT/refind.conf",
                "/EFI/ModernOS/modernos.efi", "/boot/refind-background.png", "/efiboot.img"}
    occupied = {PurePosixPath(name.casefold()): PurePosixPath(name) for name in reserved}
    for guest in destinations:
        add_destination(occupied, guest, "Payload")
    if config.wallpaper:
        add_destination(occupied, config.wallpaper, "Wallpaper")
    assets = []
    for mapping in getattr(args, "asset_file", []):
        guest, separator, host = mapping.partition("=")
        if not separator:
            raise ValueError("--asset-file requires GUEST_PATH=HOST_PATH")
        guest, source = local_path(guest), Path(host)
        if guest.casefold() == "/efi/boot/icons":
            raise ValueError(f"Asset destination conflicts with a boot file: {guest}")
        add_destination(occupied, guest, "Asset")

        if not source.is_file():
            raise ValueError(f"Missing boot asset: {source}")
        assets.append((source, guest))

    sources = [source for source, _ in modules + assets]
    if wallpaper_source:
        sources.append(wallpaper_source)

    if any(source.resolve().is_relative_to(root) for source in sources):
        raise ValueError("Boot inputs must be outside the disposable ISO staging directory")
    if args.bootloader != "limine" and not args.efi_image.is_file():
        raise ValueError(f"Missing native EFI image: {args.efi_image}")

    if args.bootloader != "limine":
        if config.resolution.endswith(("x16", "x24")):
            raise ValueError("Native EFI boot requires a 32-bit framebuffer; use WIDTHxHEIGHTx32")
        for source in [args.kernel, *(source for source, _ in modules)]:
            if source.stat().st_size > 1024**3:
                raise ValueError(f"Native EFI boot file exceeds its 1 GiB per-file limit: {source}")

    if root.exists():
        shutil.rmtree(root)
    root.mkdir(parents=True)
    copy_file(args.kernel, root, config.kernel)
    for source, guest in modules:
        copy_file(source, root, guest)
    if wallpaper_source:
        copy_file(wallpaper_source, root, config.wallpaper)
    for source, guest in assets:
        copy_file(source, root, guest)
    boot = root / "boot"
    boot.mkdir(exist_ok=True)
    (boot / "modernos.conf").write_text(efi_config(config))
    return root


def install_limine(args, root, config, rendered=None):
    directory = args.limine_dir
    for name in ("limine-bios.sys", "limine-bios-cd.bin", "limine-uefi-cd.bin"):
        copy_file(directory / name, root, "/boot/limine/" + name)
    configuration = root / "boot/limine/limine.conf"
    configuration.write_text(rendered if rendered is not None else
                             loader_config(config, "limine", getattr(args, "template", None)))
    copy_file(directory / "BOOTX64.EFI", root, "/EFI/BOOT/BOOTX64.EFI")
    return ["-b", "boot/limine/limine-bios-cd.bin", "-no-emul-boot",
            "-boot-load-size", "4", "-boot-info-table", "--efi-boot",
            "boot/limine/limine-uefi-cd.bin", "-efi-boot-part", "--efi-boot-image",
            "--protective-msdos-label"]


def install_grub(args, root, config, rendered=None):
    cfg = root / "boot/grub/grub.cfg"
    cfg.parent.mkdir(parents=True, exist_ok=True)
    cfg.write_text(rendered if rendered is not None else
                   loader_config(config, "grub", getattr(args, "template", None)))
    target = root / "EFI/BOOT/BOOTX64.EFI"
    target.parent.mkdir(parents=True, exist_ok=True)
    command = [args.grub_mkstandalone, "-O", "x86_64-efi", "--locales=", "--fonts=",
               "--modules=part_gpt part_msdos fat search search_label chain normal configfile "
               "all_video efi_gop gfxterm gfxterm_background jpeg png reboot halt efifwsetup",
               "-o", target]
    if args.grub_efi_dir:
        command.extend(("-d", args.grub_efi_dir))
    command.append(f"boot/grub/grub.cfg={cfg}")
    if args.grub_font and args.grub_font.is_file():
        command.append(f"boot/grub/fonts/unicode.pf2={args.grub_font}")
    run(command)


def install_refind(args, root, config, rendered=None):
    copy_file(args.refind_efi, root, "/EFI/BOOT/BOOTX64.EFI")
    (root / "EFI/BOOT/refind.conf").write_text(rendered if rendered is not None else
                                               loader_config(config, "refind", getattr(args, "template", None)))
    icons = args.refind_efi.parent / "icons"
    if icons.is_dir():
        existing = {path.relative_to(root).as_posix().casefold()
                    for path in root.rglob("*") if path.is_file()}
        for source in icons.rglob("*"):
            if source.is_file():
                guest = "/EFI/BOOT/icons/" + source.relative_to(icons).as_posix()
                if guest.lstrip("/").casefold() not in existing:
                    copy_file(source, root, guest)

    if config.wallpaper:
        try:
            from PIL import Image
        except ImportError as error:
            raise ValueError(
                "rEFInd wallpaper conversion requires Pillow (python-pillow / python3-pil)") from error
        with Image.open(root / config.wallpaper.lstrip("/")) as image:
            image.convert("RGB").save(root / "boot/refind-background.png")


def make_efi_volume(args, root):
    files = [path for path in root.rglob("*") if path.is_file()]
    if any(path.stat().st_size >= 2**32 for path in files):
        raise ValueError("A boot file exceeds FAT's 4 GiB file size limit")

    # FAT32 overhead, directory entries and growth room; round to 16 MiB.
    chunk = 16 * 1024 * 1024
    size = max(64 * 1024 * 1024,
               ((sum(path.stat().st_size for path in files) + 2 * chunk - 1) // chunk) * chunk)
    volume = root / "efiboot.img"
    with volume.open("wb") as output:
        output.truncate(size)
    run([args.mformat, "-i", volume, "-F", "-v", "MODERNOS", "::"])
    for source in sorted(root.iterdir()):
        if source != volume:
            run([args.mcopy, "-i", volume, "-s", source, "::"])
    return ["-e", "efiboot.img", "-no-emul-boot", "-efi-boot-part", "--efi-boot-image",
            "--protective-msdos-label"]


def build(args):
    config = read_config(args.config)
    efi_config(config)  # Validate handoff values before clearing old staging.
    rendered = loader_config(config, args.bootloader, getattr(args, "template", None))
    require_tool(args.xorriso, "Install xorriso.")
    if args.bootloader != "limine":
        require_tool(args.mformat, "Install mtools.")
        require_tool(args.mcopy, "Install mtools.")
    if args.bootloader == "grub":
        require_tool(args.grub_mkstandalone,
                     "Install GRUB's host tools and x86_64-efi modules, or set GRUB_MKSTANDALONE and GRUB_EFI_DIR.")
    root = stage_payload(args, config)
    if args.bootloader == "limine":
        options = install_limine(args, root, config, rendered)
    else:
        copy_file(args.efi_image, root, "/EFI/ModernOS/modernos.efi")
        if args.bootloader == "grub":
            install_grub(args, root, config, rendered)
        else:
            install_refind(args, root, config, rendered)
        options = make_efi_volume(args, root)

    args.output.parent.mkdir(parents=True, exist_ok=True)
    temporary = args.output.with_name(args.output.name + ".tmp")
    try:
        run([args.xorriso, "-as", "mkisofs", "-R", "-J", "-V", "MODERNOS_ISO",
             *options, root, "-o", temporary])
        if args.bootloader == "limine":
            run([args.limine_dir / "limine", "bios-install", temporary])
        os.replace(temporary, args.output)
    finally:
        temporary.unlink(missing_ok=True)

    print(f"  ISO     {args.output} ({args.bootloader})")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bootloader", choices=("limine", "grub", "refind"), required=True)
    for name in ("config", "kernel", "initramfs", "root", "output"):
        parser.add_argument("--" + name, type=Path, required=True)
    parser.add_argument("--efi-image", type=Path, default=Path("modernos.efi"))
    parser.add_argument("--source-root", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("--template", type=Path, help="native template for the selected bootloader")
    parser.add_argument("--limine-dir", type=Path)
    parser.add_argument("--grub-mkstandalone", default="grub-mkstandalone")
    parser.add_argument("--grub-efi-dir", default="")
    parser.add_argument("--grub-font", type=Path)
    parser.add_argument("--refind-efi", type=Path)
    parser.add_argument("--module-file", action="append", default=[])
    parser.add_argument("--asset-file", action="append", default=[])
    parser.add_argument("--xorriso", default="xorriso")
    parser.add_argument("--mformat", default="mformat")
    parser.add_argument("--mcopy", default="mcopy")
    args = parser.parse_args()

    try:
        build(args)
    except (ValueError, OSError, subprocess.CalledProcessError) as error:
        print(f"ISO build failed: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
