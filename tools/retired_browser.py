"""Filter retired NetSurf/WebCore applications from inherited initramfs modules."""
from pathlib import Path
import re
import subprocess
import tarfile


def retired(name):
    name = name.removeprefix('./').lstrip('/')
    return (name in {'bin/netsurf', 'bin/web-renderer', 'bin/web-broker', 'bin/webcore-render-check'}
            or name.startswith(('usr/share/netsurf/', 'usr/share/licenses/netsurf/')))


def clean_modules(iso, config, output):
    listing = subprocess.check_output(['xorriso', '-no_rc', '-indev', str(iso), '-find', '/boot',
        '-type', 'f', '-exec', 'report_lba', '--'], stderr=subprocess.DEVNULL, text=True)
    extents = {name: (int(lba) * 2048, int(size)) for lba, size, name in re.findall(
        r"File data lba:\s*0\s*,\s*(\d+)\s*,\s*\d+\s*,\s*(\d+)\s*,\s*'([^']+)'", listing)}
    replacements, removed = {}, []
    output = Path(output)
    output.mkdir(parents=True, exist_ok=True)
    for module in dict.fromkeys(re.findall(r'module_path:\s*boot\(\):(/[^\s]+\.tar)', config)):
        offset, size = extents[module]
        with Path(iso).open('rb') as stream:
            stream.seek(offset)
            with tarfile.open(fileobj=stream, mode='r:') as source:
                members = source.getmembers()
                if not any(retired(m.name) for m in members):
                    continue
                destination = output / Path(module).name
                with tarfile.open(destination, 'w', format=tarfile.USTAR_FORMAT) as target:
                    for member in members:
                        if retired(member.name):
                            removed.append(dict(module=module, path=member.name, bytes=member.size))
                        else:
                            target.addfile(member, source.extractfile(member) if member.isfile() else None)
                replacements[module] = destination
    return replacements, removed
