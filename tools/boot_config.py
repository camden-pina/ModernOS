#!/usr/bin/env python3
"""Read neutral TOML boot policy and render native bootloader templates."""
from dataclasses import dataclass
from pathlib import Path, PurePosixPath
import re
import tomllib

ROOT = Path(__file__).resolve().parents[1]
TEMPLATES = {"limine": ROOT / "scripts/bootloaders/limine.conf.in",
             "grub": ROOT / "scripts/bootloaders/grub.cfg.in",
             "refind": ROOT / "scripts/bootloaders/refind.conf.in"}
TOKEN = re.compile(r"\{\{([^{}\n]+)\}\}")


@dataclass
class Module:
    path: str
    string: str = ""


@dataclass
class BootConfig:
    title: str
    kernel: str
    modules: list[Module]
    timeout: int = 0
    resolution: str = ""
    menu_resolution: str = ""
    cmdline: str = ""
    wallpaper: str = ""


def local_path(value: str) -> str:
    """Only same-volume absolute paths can be used by the firmware filesystem."""
    if value.startswith("boot():"):
        value = value[len("boot():"):]
    if not value.startswith("/") or "\\" in value or ".." in value.split("/"):
        raise ValueError(
            f"Boot path must be an absolute path on the boot volume, without '..': {value!r}")

    if len(value) > 511 or any(ord(c) < 32 or ord(c) > 126 for c in value):
        raise ValueError("Boot paths must contain printable ASCII and fit in 511 characters")

    normalized = str(PurePosixPath(value))
    if normalized == "/":
        raise ValueError("Boot path must name a file")
    return normalized


def resolution(value: str) -> str:
    if value:
        if not re.fullmatch(r"[1-9][0-9]*x[1-9][0-9]*(?:x(?:16|24|32))?", value):
            raise ValueError(f"Unsupported resolution {value!r}; use WIDTHxHEIGHT[xDEPTH]")
        if any(int(number) > 0xffffffff for number in value.split("x")[:2]):
            raise ValueError("Resolution dimensions must fit in 32 bits")
    return value


def table(value, allowed, name):
    if not isinstance(value, dict):
        raise ValueError(f"{name} must be a TOML table")
    for key in value.keys() - allowed:
        raise ValueError(
            f"Unknown setting {name}.{key}; put loader-specific settings in its native template")
    return value


def text(value, name, *, required=False, maximum=None, ascii_only=False):
    if not isinstance(value, str):
        raise ValueError(f"{name} must be a string")
    if required and not value.strip():
        raise ValueError(f"{name} must not be empty")
    if any(ord(c) < 32 or ord(c) == 127 or (ascii_only and ord(c) > 126) for c in value):
        raise ValueError(
            f"{name} must contain printable {'ASCII' if ascii_only else 'characters'} on one line")
    if maximum is not None and len(value.encode()) > maximum:
        raise ValueError(f"{name} must fit in {maximum} bytes")
    return value


def read_config(path: Path) -> BootConfig:
    try:
        data = tomllib.loads(path.read_text())
    except tomllib.TOMLDecodeError as error:
        raise ValueError(f"{path}: invalid TOML boot policy: {error}") from error

    try:
        table(data, {"version", "boot", "menu"}, "boot.conf")
        if type(data.get("version")) is not int or data["version"] != 1:
            raise ValueError("version must be the integer 1")

        boot = table(data.get("boot"), {"title", "kernel",
                     "modules", "resolution", "cmdline"}, "boot")
        menu = table(data.get("menu", {}), {"timeout", "resolution", "wallpaper"}, "menu")
        title = text(boot.get("title"), "boot.title", required=True)
        kernel = local_path(text(boot.get("kernel"), "boot.kernel", required=True))
        cmdline = text(boot.get("cmdline", ""), "boot.cmdline", maximum=511, ascii_only=True)

        modules = boot.get("modules", [])
        if not isinstance(modules, list) or len(modules) > 32:
            raise ValueError("boot.modules must be an array of at most 32 tables")

        parsed = []
        for number, item in enumerate(modules):
            name = f"boot.modules[{number}]"
            table(item, {"path", "string"}, name)
            parsed.append(Module(local_path(text(item.get("path"), name + ".path", required=True)),
                                 text(item.get("string", ""), name + ".string", maximum=511, ascii_only=True)))

        paths = [kernel, *(module.path for module in parsed)]
        if len(paths) != len(set(paths)):
            raise ValueError("Kernel and module paths must be distinct")

        timeout = menu.get("timeout", 0)
        if type(timeout) is not int or not -1 <= timeout <= 9999:
            raise ValueError("menu.timeout must be an integer -1..9999; -1 waits indefinitely")

        boot_mode = resolution(text(boot.get("resolution", ""), "boot.resolution"))
        menu_mode = resolution(text(menu.get("resolution", ""), "menu.resolution"))
        wallpaper = text(menu.get("wallpaper", ""), "menu.wallpaper")

        return BootConfig(title, kernel, parsed, timeout, boot_mode, menu_mode,
                          cmdline, local_path(wallpaper) if wallpaper else "")
    except ValueError as error:
        raise ValueError(f"{path}: {error}") from error


def efi_config(config: BootConfig) -> str:
    lines = ["# Generated from shared boot policy; edit BOOT_CONFIG.",
             f"kernel={config.kernel}", f"resolution={config.resolution}",
             f"cmdline={config.cmdline}"]
    for module in config.modules:
        lines.extend((f"module={module.path}", f"module_string={module.string}"))

    result = "\n".join(lines) + "\n"
    if len(result.encode()) > 65536:
        raise ValueError("Native EFI boot configuration exceeds 64 KiB")
    return result


def grub_quote(value: str) -> str:
    return '"' + re.sub(r'([\\"$`])', r'\\\1', value) + '"'


def refind_quote(value: str) -> str:
    return '"' + value.replace('"', '""') + '"'


def template_values(config: BootConfig, loader: str):
    if loader not in TEMPLATES:
        raise ValueError(f"Unknown bootloader: {loader}")
    mode = config.menu_resolution or config.resolution
    if loader != "limine" and config.resolution.endswith(("x16", "x24")):
        raise ValueError(f"{loader}: boot.resolution requires a 32-bit framebuffer")
    if loader == "refind" and mode.endswith(("x16", "x24")):
        raise ValueError("refind cannot select the depth in menu.resolution; use WIDTHxHEIGHT[x32]")

    if loader == "limine":
        if config.title.startswith(("/", "+")):
            raise ValueError("limine cannot represent a boot.title starting with '/' or '+'")
        entry = [f"/{config.title}", "    protocol: limine", f"    path: boot():{config.kernel}"]
        if config.resolution:
            entry.append(f"    resolution: {config.resolution}")
        if config.cmdline:
            entry.append(f"    cmdline: {config.cmdline}")
        for module in config.modules:
            entry.extend(
                (f"    module_path: boot():{module.path}", f"    module_string: {module.string}"))
        timeout = "no" if config.timeout == -1 else str(config.timeout)
        display = f"interface_resolution: {mode}" if mode else ""
        wallpaper = f"wallpaper: boot():{config.wallpaper}" if config.wallpaper else ""
    elif loader == "grub":
        entry = [f"menuentry {grub_quote(config.title)} {{",
                 "    chainloader /EFI/ModernOS/modernos.efi", "}"]
        timeout = str(config.timeout)
        display = f"set gfxmode={mode},auto\nset gfxpayload=keep" if mode else ""
        wallpaper = f"    background_image {grub_quote(config.wallpaper)}" if config.wallpaper else ""
    else:
        entry = [f"default_selection {refind_quote(config.title)}", "",
                 f"menuentry {refind_quote(config.title)} {{",
                 "    loader /EFI/ModernOS/modernos.efi", "}"]
        timeout = str(0 if config.timeout == -1 else -1 if config.timeout == 0 else config.timeout)
        display = "resolution " + " ".join(mode.split("x")[:2]) if mode else ""
        wallpaper = "banner /boot/refind-background.png" if config.wallpaper else ""

    return {"HEADER": "# Generated from BOOT_CONFIG and the selected native BOOT_TEMPLATE.",
            "TIMEOUT": timeout, "MENU_RESOLUTION": display,
            "WALLPAPER": wallpaper, "BOOT_ENTRY": "\n".join(entry)}


def loader_config(config: BootConfig, loader: str, template: Path | None = None) -> str:
    values = template_values(config, loader)
    path = Path(template) if template is not None else TEMPLATES[loader]
    source = path.read_text()
    tokens = [match.group(1) for match in TOKEN.finditer(source)]

    unknown = set(tokens) - values.keys()
    if unknown:
        raise ValueError(f"{path}: unknown template placeholder(s): {', '.join(sorted(unknown))}")
    if tokens.count("BOOT_ENTRY") != 1:
        raise ValueError(
            f"{path}: template must contain exactly one {{{{BOOT_ENTRY}}}} placeholder")

    required = {"TIMEOUT"}
    if values["MENU_RESOLUTION"]:
        required.add("MENU_RESOLUTION")
    if config.wallpaper:
        required.add("WALLPAPER")

    missing = required - set(tokens)
    if missing:
        raise ValueError(
            f"{path}: cannot represent shared setting(s); missing placeholder(s): {', '.join(sorted(missing))}")

    # Substitute once, so user data containing braces is never interpreted.
    return TOKEN.sub(lambda match: values[match.group(1)], source)


def limine_config(config: BootConfig, template: Path | None = None) -> str:
    return loader_config(config, "limine", template)


def grub_config(config: BootConfig, template: Path | None = None) -> str:
    return loader_config(config, "grub", template)


def refind_config(config: BootConfig, template: Path | None = None) -> str:
    return loader_config(config, "refind", template)
