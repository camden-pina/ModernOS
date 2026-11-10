"""Build x86 BIOS, VGA BIOS and boot option ROMs from QEMU's bundled sources."""
from pathlib import Path
import shutil


def build(source, output, run, cross, python, jobs, native=False):
    source, output = Path(source), Path(output)
    output.mkdir(parents=True, exist_ok=True)
    firmware = {}
    variants = [('seabios-128k', 'bios.bin'), ('seabios-256k', 'bios-256k.bin'),
                ('seabios-microvm', 'bios-microvm.bin')]
    variants += [('vga-' + name, 'vgabios' + ('' if name == 'isavga' else '-' + name) + '.bin')
                 for name in ('stdvga', 'cirrus', 'vmware', 'qxl', 'isavga', 'virtio',
                              'bochs-display', 'ramfb', 'ati')]
    for variant, name in variants:
        directory = output / variant
        directory.mkdir(exist_ok=True)
        config = directory / '.config'
        if not config.exists():
            shutil.copyfile(source / 'roms' / ('config.' + variant), config)
        options = ['make', '-j' + str(jobs), 'CROSS_PREFIX=' + str(cross),
                   'HOSTCC=/usr/bin/gcc' + (' -static -no-pie' if native else ''),
                   'HOSTCFLAGS=-O2 -fno-pie', 'HOSTLDFLAGS=-static -no-pie' if native else 'HOSTLDFLAGS=-no-pie',
                   'PYTHON=' + python, 'EXTRAVERSION=-Coast', 'OUT=' + str(directory) + '/',
                   'KCONFIG_CONFIG=' + str(config)]
        run('firmware-' + variant + '-configure', [*options, 'oldnoconfig'], source / 'roms/seabios')
        run('firmware-' + variant + '-build', [*options, 'all'], source / 'roms/seabios')
        path = directory / ('bios.bin' if variant.startswith('seabios') else 'vgabios.bin')
        if not path.is_file():
            raise RuntimeError('Missing source-built firmware: ' + str(path))
        firmware[name] = path
    option = output / 'optionrom'
    option.mkdir(exist_ok=True)
    shutil.copyfile(source / 'pc-bios/optionrom/Makefile', option / 'Makefile')
    (option / 'config.mak').write_text('TOPSRC_DIR=' + str(source) + '\nCC=' + str(cross) +
        'gcc\nOBJCOPY=' + str(cross) + 'objcopy\nPYTHON=' + python + '\n')
    run('firmware-optionrom-build', ['make', '-j' + str(jobs)], option)
    for name in ('multiboot.bin', 'multiboot_dma.bin', 'linuxboot.bin', 'linuxboot_dma.bin',
                 'kvmvapic.bin', 'pvh.bin'):
        firmware[name] = option / name
        if not firmware[name].is_file():
            raise RuntimeError('Missing source-built option ROM: ' + name)
    return firmware
