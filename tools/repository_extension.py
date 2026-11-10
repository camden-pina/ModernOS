#!/usr/bin/env python3
"""Publish additional signed packages/recipes as a new public Coast snapshot."""
import gzip
import io
import json
import os
from pathlib import Path, PurePosixPath
import shutil
import subprocess
import tarfile

from export_repository import export
from serve_repository import load_repository
from source_tree import digest as sha, EPOCH


def mtree_path(path):
    return ''.join('\\%03o' % ord(c) if c in ' \\#' else c for c in path)


def extend(repository, output, keys, repo_add, packages, recipes=(), distfiles=(),
           replace_packages=(), replace_recipes=()):
    output, keys, repo_add = map(lambda p: Path(p).resolve(), (output, keys, repo_add))
    if not keys.is_dir() or keys.stat().st_mode & 0o077 or keys.is_relative_to(output):
        raise ValueError('Keep the existing private signing directory outside the public output, mode 0700')
    export(repository, output)
    manifest = output / 'repository.json'
    snapshot = json.loads(manifest.read_text())
    replacements, recipe_replacements = dict(replace_packages), dict(replace_recipes)
    incoming = {p['name']: p for p in packages}
    if len(incoming) != len(packages) or replacements.keys() - incoming.keys():
        raise ValueError('Every replacement must have exactly one new package')
    for name, version in replacements.items():
        previous = snapshot['packages'].get(name)
        if not previous or previous['version'] != version or incoming[name]['version'] == version:
            raise ValueError('Package replacement version mismatch: ' + name)
        archive = f'/repo/x86_64/{name}-{version}-x86_64.pkg.tar.gz'
        for route in (archive, archive + '.sig'):
            asset = snapshot['host_files'].pop(route, None)
            if asset:
                (output / asset['path']).unlink()
        del snapshot['packages'][name]
    incoming_recipes = {r['name']: r for r in recipes}
    if len(incoming_recipes) != len(recipes) or recipe_replacements.keys() - incoming_recipes.keys():
        raise ValueError('Every recipe replacement must include its new source')
    recipe_ids = {}
    for name, version in recipe_replacements.items():
        previous = [r for r in snapshot['recipes'] if r['Name'] == name]
        if len(previous) != 1 or previous[0]['Version'] != version or incoming_recipes[name]['version'] == version:
            raise ValueError('Recipe replacement version mismatch: ' + name)
        recipe_ids[name] = previous[0]['ID']
        snapshot['recipes'].remove(previous[0])
        for route in list(snapshot['host_files']):
            if route.startswith(('/recipes/' + name + '.git/', '/recipes/' + name + '/files/')):
                del snapshot['host_files'][route]
        for path in (output / 'public/recipes' / (name + '.git'), output / 'public/recipes' / name):
            if path.exists():
                shutil.rmtree(path)
    fingerprint = snapshot['public_signing_fingerprint']
    generated = output / 'generated'
    generated.mkdir()
    environment = dict(PATH='/usr/bin:/bin', HOME='/tmp', TMPDIR='/tmp', LANG='C.UTF-8',
        GNUPGHOME=str(keys), SOURCE_DATE_EPOCH=str(EPOCH),
        MAKEPKG_LIBRARY=str(repo_add.parents[1] / 'share/makepkg'),
        GIT_CONFIG_NOSYSTEM='1', GIT_CONFIG_GLOBAL='/dev/null',
        GIT_AUTHOR_NAME='Coast local ports', GIT_AUTHOR_EMAIL='ports@coast.invalid',
        GIT_COMMITTER_NAME='Coast local ports', GIT_COMMITTER_EMAIL='ports@coast.invalid',
        GIT_AUTHOR_DATE=str(EPOCH) + ' +0000', GIT_COMMITTER_DATE=str(EPOCH) + ' +0000')
    conf = ['bwrap', '--unshare-all', '--die-with-parent', '--new-session', '--ro-bind', '/', '/',
            '--proc', '/proc', '--dev', '/dev', '--tmpfs', '/tmp',
            '--bind', str(output), str(output), '--bind', str(keys), str(keys)]

    def host(command):
        result = subprocess.run([*conf, '--chdir', str(output), '--', *map(str, command)],
                                env=environment, stdin=subprocess.DEVNULL, capture_output=True, text=True)
        if result.returncode:
            raise RuntimeError(result.stderr[-3000:] + result.stdout[-1500:])
        return result.stdout.strip()

    def publish(path, route):
        snapshot['host_files'][route] = dict(path=str(path.relative_to(output)), sha256=sha(path))

    def sign(path):
        host(['gpg', '--batch', '--yes', '--pinentry-mode', 'loopback', '--passphrase', '',
              '--local-user', fingerprint, '--detach-sign', path])
        host(['gpg', '--batch', '--verify', str(path) + '.sig', path])

    owned = {p: name for name, package in snapshot['packages'].items()
             for p in [*package['files'], *package['aliases']]}
    archives = []
    directory = output / 'public/repo/x86_64'
    directory.mkdir(parents=True, exist_ok=True)
    for package in packages:
        name = package['name']
        if name in snapshot['packages']:
            raise ValueError('Package already exists: ' + name)
        entries = {}
        for path, source in package.get('files', {}).items():
            relative = PurePosixPath(path)
            if relative.is_absolute() or '..' in relative.parts or any(ord(c) < 32 or ord(c) == 127 for c in path):
                raise ValueError('Unsafe package path: ' + path)
            if path in owned:
                raise ValueError('Package ownership conflict: ' + path)
            source = Path(source)
            entries[path] = dict(path=source, sha256=sha(source), bytes=source.stat().st_size,
                                 mode=0o755 if source.stat().st_mode & 0o111 else 0o644)
            owned[path] = name
        directories = sorted({str(parent) for path in entries for parent in PurePosixPath(path).parents
                              if str(parent) != '.'})
        info = (f'pkgname = {name}\npkgbase = {name}\npkgver = {package["version"]}\n'
                f'pkgdesc = {package["description"]}\n'
                f'builddate = {EPOCH}\npackager = Coast local ports\n'
                f'size = {sum(a["bytes"] for a in entries.values())}\narch = x86_64\n'
                f'license = {package["license"]}\n')
        info += ''.join('depend = ' + d + '\n' for d in package.get('depends', []))
        info += ''.join('provides = ' + d + '\n' for d in package.get('provides', []))
        mtree = ['#mtree', f'. type=dir uid=0 gid=0 mode=755 time={EPOCH}.0']
        mtree += [f'./{mtree_path(p)} type=dir uid=0 gid=0 mode=755 time={EPOCH}.0' for p in directories]
        mtree += [f'./{mtree_path(p)} type=file uid=0 gid=0 mode={a["mode"]:o} size={a["bytes"]} time={EPOCH}.0 sha256digest={a["sha256"]}'
                  for p, a in sorted(entries.items())]
        archive = directory / f'{name}-{package["version"]}-x86_64.pkg.tar.gz'
        with archive.open('wb') as raw, gzip.GzipFile(fileobj=raw, mode='wb', mtime=EPOCH, filename='') as compressed, \
                tarfile.open(fileobj=compressed, mode='w|', format=tarfile.PAX_FORMAT) as bundle:
            def put(path, data=b'', mode=0o644, kind=tarfile.REGTYPE):
                item = tarfile.TarInfo(path)
                item.uid = item.gid = 0
                item.mtime, item.mode, item.type, item.size = EPOCH, mode, kind, len(data)
                bundle.addfile(item, io.BytesIO(data) if data else None)
            put('.PKGINFO', info.encode())
            put('.MTREE', gzip.compress(('\n'.join(mtree) + '\n').encode(), mtime=EPOCH))
            for path in directories:
                put(path + '/', mode=0o755, kind=tarfile.DIRTYPE)
            for path, asset in sorted(entries.items()):
                put(path, asset['path'].read_bytes(), asset['mode'])
        sign(archive)
        archives.append(archive)
        snapshot['packages'][name] = {**{k: v for k, v in package.items() if k != 'files'},
                                      'files': sorted(entries), 'aliases': [], 'provides': package.get('provides', [])}
        publish(archive, '/repo/x86_64/' + archive.name)
        publish(Path(str(archive) + '.sig'), '/repo/x86_64/' + archive.name + '.sig')
    if archives:
        host(['/usr/bin/bash', repo_add, '--sign', '--key', fingerprint, directory / 'coast.db.tar.gz', *archives])
    for path in directory.iterdir():
        if path.name.endswith(('.old', '.old.sig')):
            path.unlink()
        else:
            publish(path, '/repo/x86_64/' + path.name)
    for recipe in recipes:
        name = recipe['name']
        if any(r['Name'] == name for r in snapshot['recipes']):
            raise ValueError('Recipe already exists: ' + name)
        work = generated / name
        work.mkdir()
        for path in Path(recipe['directory']).iterdir():
            if path.is_file():
                shutil.copy2(path, work / path.name)
        host(['git', 'init', '--initial-branch=master', work])
        host(['git', '-C', work, 'add', '.'])
        host(['git', '-C', work, '-c', 'commit.gpgsign=false', 'commit', '-m', 'Pinned Coast source recipe'])
        commit = host(['git', '-C', work, 'rev-parse', 'HEAD'])
        bare = output / 'public/recipes' / (name + '.git')
        bare.parent.mkdir(parents=True, exist_ok=True)
        host(['git', 'clone', '--bare', '--no-hardlinks', work, bare])
        host(['git', '--git-dir=' + str(bare), 'update-server-info'])
        for path in bare.rglob('*'):
            if path.is_file():
                publish(path, '/recipes/' + name + '.git/' + str(path.relative_to(bare)))
        index = recipe_ids.get(name, max((r['ID'] for r in snapshot['recipes']), default=0) + 1)
        snapshot['recipes'].append(dict(ID=index, PackageBaseID=index, Name=name, PackageBase=name,
            Version=recipe['version'], Description=recipe['description'], Maintainer='coast-local',
            Depends=recipe.get('depends', ['musl']), MakeDepends=recipe.get('makedepends', ['gcc', 'make']),
            License=[recipe['license']], NumVotes=0, Popularity=0.0, OutOfDate=None,
            FirstSubmitted=EPOCH, LastModified=EPOCH, Commit=commit, URLPath=f'/recipes/{name}.git',
            Files={p.name: sha(p) for p in work.iterdir() if p.is_file()}))
        for source in work.iterdir():
            if source.is_file():
                path = output / 'public/recipes' / name / 'files' / source.name
                path.parent.mkdir(parents=True, exist_ok=True)
                shutil.copyfile(source, path)
                publish(path, '/recipes/' + name + '/files/' + source.name)
    if recipes:
        catalog = output / 'public/recipes/index.json'
        catalog.write_text(json.dumps(dict(version=1, name='Coast Packages', results=snapshot['recipes']), indent=2) + '\n')
        sign(catalog)
        publish(catalog, '/recipes/index.json')
        publish(Path(str(catalog) + '.sig'), '/recipes/index.json.sig')
    for source in distfiles:
        source = Path(source)
        path = output / 'public/distfiles' / source.name
        path.parent.mkdir(parents=True, exist_ok=True)
        if path.exists() and sha(path) != sha(source):
            raise ValueError('Distfile name conflict: ' + source.name)
        shutil.copyfile(source, path)
        publish(path, '/distfiles/' + path.name)
    manifest.write_text(json.dumps(snapshot, indent=2) + '\n')
    load_repository(manifest)
    shutil.rmtree(generated)
    return manifest
