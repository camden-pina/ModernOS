#!/usr/bin/env python3
"""Publish qualified Coast-built GNU generators and complete offline source kits."""
import argparse
import gzip
import io
import json
from pathlib import Path
import tarfile

from repository_extension import extend
from source_tree import digest

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--repository', type=Path, required=True)
    parser.add_argument('--bison-qualification', type=Path, required=True)
    parser.add_argument('--grep-qualification', type=Path, required=True)
    parser.add_argument('--output-root', type=Path, required=True)
    parser.add_argument('--staging-root', type=Path, required=True)
    parser.add_argument('--signing-home', type=Path, default=ROOT / 'build/coast-signing-keys')
    parser.add_argument('--repo-add', type=Path, default=ROOT / 'build/coast-security-ports-r3/install/usr/bin/repo-add')
    args = parser.parse_args()
    stage = args.staging_root.resolve()
    stage.mkdir(parents=True, exist_ok=False)
    baseline = json.loads(args.repository.read_text())
    packages, recipes, distfiles, replacements, evidence = [], [], [], {}, {}
    for name in ['bison', 'grep']:
        proof = getattr(args, name + '_qualification').resolve(strict=True)
        report = json.loads((proof / 'report.json').read_text())
        if (report['state'] != 'passed' or report['build_host'] != 'Coast' or
                report['guest_execution_on_linux'] is not False or not report['owned_vm_stopped']):
            raise ValueError('A successful stopped native qualification is required: ' + name)
        required = {'native-BUILD.json', 'native-' + name + '-runtime.tar.gz'}
        if not required.issubset(report['exports']):
            raise ValueError('Native build artifacts are missing: ' + name)
        for artifact, expected in report['exports'].items():
            if digest(proof / artifact) != expected:
                raise ValueError('Qualified export changed: ' + artifact)
        for source, expected in report['source_files'].items():
            if digest(ROOT / source) != expected:
                raise ValueError('Source changed after qualification: ' + source)
        build = json.loads((proof / 'native-BUILD.json').read_text())
        if build['build_mode'] != 'native-coast' or build['target_host_executed'] is not True:
            raise ValueError('Source compilation inside Coast is required: ' + name)
        runtime = stage / (name + '-runtime')
        runtime.mkdir()
        with tarfile.open(proof / ('native-' + name + '-runtime.tar.gz')) as bundle:
            bundle.extractall(runtime, filter='data')
        for path, artifact in build['files'].items():
            if digest(runtime / path) != artifact['sha256']:
                raise ValueError('Native runtime differs from its build receipt: ' + path)
        version = build['version']
        archive_name = build['source']['archive']
        upstream = ROOT / ('build/gnu-runtime/vendor' if name == 'bison' else 'userland/vendor/linux-ports') / archive_name
        if digest(upstream) != build['source']['sha256']:
            raise ValueError('Upstream source changed: ' + archive_name)
        kit = stage / ('coast-' + name + '-' + version + '.tar.gz')
        kit_root = 'coast-' + name + '-' + version
        kit_files = {path: ROOT / path for path in report['source_files']}
        kit_files[('build/gnu-runtime/vendor/' if name == 'bison' else 'userland/vendor/linux-ports/') + archive_name] = upstream
        with kit.open('wb') as raw, gzip.GzipFile(fileobj=raw, mode='wb', mtime=0, filename='') as compressed, \
                tarfile.open(fileobj=compressed, mode='w|', format=tarfile.PAX_FORMAT) as bundle:
            directories = set()
            for relative, source in sorted(kit_files.items()):
                target = kit_root + '/' + relative
                for parent in reversed(Path(target).parents):
                    if str(parent) != '.' and str(parent) not in directories:
                        info = tarfile.TarInfo(str(parent))
                        info.type, info.mode = tarfile.DIRTYPE, 0o755
                        bundle.addfile(info)
                        directories.add(str(parent))
                info = tarfile.TarInfo(target)
                info.mode, info.size = 0o644, source.stat().st_size
                with source.open('rb') as stream:
                    bundle.addfile(info, stream)
        distfiles.append(kit)
        if name == 'bison':
            files = {p: runtime / p for p in build['files'] if p != 'usr/share/info/dir'}
            release = '1'
        else:
            # Preserve the installed GNU grep locations and ownership while
            # replacing the two previous aliases with the same native ELF.
            binary = runtime / 'usr/bin/grep'
            files = {p: binary for p in ['usr/bin/grep', 'usr/bin/linux-grep', 'usr/lib/coast/linux-ports/grep/grep']}
            files['usr/share/licenses/linux-ports/grep/COPYING'] = runtime / 'usr/share/licenses/grep/COPYING'
            files['usr/src/coast/package-tools/' + archive_name] = upstream
            files['usr/src/coast/package-tools/' + kit.name] = kit
            replacements[name] = baseline['packages'][name]['version']
            release = '2'
        packages.append(dict(name=name, version=version + '-' + release,
            description='GNU ' + name + ' compiled and qualified inside Coast', license='GPL-3.0-or-later',
            depends=['musl'] + (['m4'] if name == 'bison' else []), files=files))
        packages.append(dict(name=name + '-build', version=version + '-1',
            description='Complete offline source rebuild kit for GNU ' + name, license='LicenseRef-Coast-Mixed',
            depends=['gcc', 'binutils', 'make', 'python', 'xz', 'm4', 'patch'] + (['coast-linux-uapi'] if name == 'grep' else []),
            files={'usr/src/coast/' + name + '-build/' + kit.name: kit}))
        recipe = stage / (name + '-recipe')
        recipe.mkdir()
        if name == 'bison':
            packaging = '  cp -a build/bison-native/runtime/usr "$pkgdir/"\n  rm -f "$pkgdir/usr/share/info/dir"\n'
        else:
            packaging = ''.join('  install -Dm755 build/grep-native/runtime/usr/bin/grep "$pkgdir/' + p + '"\n'
                for p in ['usr/bin/grep', 'usr/bin/linux-grep', 'usr/lib/coast/linux-ports/grep/grep'])
            packaging += '  install -Dm644 build/grep-native/runtime/usr/share/licenses/grep/COPYING "$pkgdir/usr/share/licenses/linux-ports/grep/COPYING"\n'
        (recipe / 'PKGBUILD').write_text('pkgname=' + name + '\npkgver=' + version + '\npkgrel=' + release + '\n'
            'pkgdesc="Native GNU ' + name + ' for Coast"\narch=(x86_64)\nlicense=(GPL-3.0-or-later)\n'
            'depends=(musl' + (' m4' if name == 'bison' else '') + ')\n'
            'makedepends=(' + name + '-build)\n'
            'source=("${COAST_REPOSITORY_URL:-http://10.0.2.2:8765}/distfiles/' + kit.name + '")\n'
            "sha256sums=('" + digest(kit) + "')\n"
            'build() {\n  cd "$srcdir/' + kit_root + '"\n  python3 -B tools/build_' + name + '.py --native --jobs 4\n}\n'
            'check() {\n  cd "$srcdir/' + kit_root + '"\n  python3 ports/gnu-runtime/' + name + '_check.py --runtime build/' + name + '-native/runtime\n}\n'
            'package() {\n  cd "$srcdir/' + kit_root + '"\n' + packaging + '}\n')
        (recipe / '.SRCINFO').write_text('pkgbase = ' + name + '\n\tpkgdesc = Native GNU ' + name + '\n'
            '\tpkgver = ' + version + '\n\tpkgrel = ' + release + '\n\tarch = x86_64\n'
            '\tlicense = GPL-3.0-or-later\n\tdepends = musl\n' + ('\tdepends = m4\n' if name == 'bison' else '') +
            '\tmakedepends = ' + name + '-build\n\tsource = ' + kit.name + '\n\tsha256sums = ' + digest(kit) + '\n\npkgname = ' + name + '\n')
        recipes.append(dict(name=name, version=version + '-' + release, description='Build GNU ' + name + ' inside Coast',
            license='GPL-3.0-or-later', directory=recipe, depends=['musl'] + (['m4'] if name == 'bison' else []), makedepends=[name + '-build']))
        evidence[name] = dict(qualification_sha256=digest(proof / 'report.json'),
            native_build_sha256=digest(proof / 'native-BUILD.json'), source_kit_sha256=digest(kit))
    manifest = extend(args.repository, args.output_root, args.signing_home, args.repo_add,
        packages, recipes=recipes, replace_packages=replacements, distfiles=distfiles)
    (args.output_root / 'NATIVE-GENERATORS.json').write_text(json.dumps(dict(repository=str(manifest),
        qualifications=evidence, source_only_kits=True, signing_secret_included=False), indent=2) + '\n')
    print('Qualified native GNU tools published:', manifest)


if __name__ == '__main__':
    main()
