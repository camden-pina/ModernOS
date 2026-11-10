"""One dependency graph for app selection, incremental builds and cleanup."""
from pathlib import Path

APPS = ('chromium', 'codeoss', 'bash', 'gcc', 'python', 'packages', 'makepkg', 'qt', 'qemu', 'qemu-viewer')
APP_REQUIRES = {
    'codeoss': {'bash'}, 'packages': {'python', 'makepkg'},
    'makepkg': {'gcc', 'python', 'bash'}, 'qemu-viewer': {'qemu', 'python', 'chromium'},
}
# These are build dependencies, distinct from the applications installed in an ISO.
DEPENDENCIES = {
    'sources': set(), 'electron': {'sources'}, 'glibc': set(),
    'libgcc': {'glibc'}, 'cxx': {'glibc', 'libgcc'}, 'gui-sources': set(),
    'gui-runtime': {'glibc', 'libgcc', 'cxx', 'gui-sources'},
    'application': {'sources'}, 'sdk': {'glibc', 'libgcc', 'cxx', 'gui-runtime'},
    'addons': {'application', 'sdk', 'sources'}, 'rust': {'application', 'sdk'},
    'kernel': set(), 'native': {'kernel'}, 'compiler': {'native'},
    'packages': {'native'}, 'cli': {'native'}, 'bridge': {'native', 'sdk'},
    'assets': set(), 'program': {'compiler', 'packages'},
    'makepkg': {'compiler', 'packages', 'program'}, 'frontend': {'compiler', 'program', 'makepkg'},
    'qt': {'compiler'}, 'qemu': {'compiler'}, 'viewer': {'qemu', 'program', 'bridge'},
    'image': {'native', 'assets'}, 'qualification': {'image'},
}
APP_COMPONENTS = {
    'chromium': {'electron', 'gui-runtime', 'bridge'},
    'codeoss': {'electron', 'gui-runtime', 'bridge', 'application', 'addons', 'rust'},
    'bash': {'cli'}, 'gcc': {'compiler'}, 'python': {'program'},
    'packages': {'packages', 'frontend'}, 'makepkg': {'makepkg'},
    'qt': {'qt'}, 'qemu': {'qemu'}, 'qemu-viewer': {'viewer'},
}
RECEIPTS = {name: 'BUILD.json' for name in DEPENDENCIES}
RECEIPTS.update(sources='SOURCE.json', kernel='KERNEL.json', application='PACKAGED.json',
                sdk='APPLICATION-SDK.json', image='MANIFEST.json', qualification='QUALIFICATION.json')
RESTARTABLE = {'gui-runtime', 'sdk', 'addons', 'frontend', 'viewer', 'image', 'qualification'}
TOOLCHAINS = {'electron', 'compiler', 'sdk', 'glibc', 'libgcc', 'cxx', 'gui-sources', 'gui-runtime'}


def words(value):
    return set(value.replace(',', ' ').split()) if isinstance(value, str) else set(value)


def app_selection(requested='all', without='', add='', previous=None):
    selected = set(APPS) if requested in (None, 'all') else set() if requested in ('', 'none') else words(requested)
    if add:
        selected = set(previous) if previous is not None else selected
        selected |= words(add)
    excluded = words(without)
    if add:
        added = words(add)
        while True:
            expanded = added | set().union(*(APP_REQUIRES.get(app, set()) for app in added))
            if expanded == added:
                break
            added = expanded
        excluded -= added
    unknown = (selected | excluded) - set(APPS)
    if unknown:
        raise ValueError('Unknown apps: ' + ', '.join(sorted(unknown)) + '. Choose: ' + ', '.join(APPS))
    reasons = {}
    while True:
        before = selected.copy()
        for app in list(selected):
            blocked = APP_REQUIRES.get(app, set()) & excluded
            if app in excluded or blocked:
                selected.remove(app)
                excluded.add(app)
                reasons[app] = 'explicitly omitted' if not blocked else 'requires ' + ', '.join(sorted(blocked))
            else:
                selected |= APP_REQUIRES.get(app, set())
        if before == selected:
            break
    return [app for app in APPS if app in selected], {app: reasons.get(app, 'not selected') for app in APPS if app not in selected}


def closure(names, graph=None):
    graph = DEPENDENCIES if graph is None else graph
    result = set(names)
    while True:
        expanded = result | set().union(*(graph[name] for name in result))
        if expanded == result:
            return result
        result = expanded


def dependents(names, graph=None):
    graph = graph_for(APPS) if graph is None else graph
    result = set(names)
    while True:
        expanded = result | {name for name, dependencies in graph.items() if dependencies & result}
        if expanded == result:
            return result
        result = expanded


def graph_for(apps):
    graph = {name: set(dependencies) for name, dependencies in DEPENDENCIES.items()}
    selected = set().union(*(APP_COMPONENTS[app] for app in apps)) if apps else set()
    if {'chromium', 'codeoss'} & set(apps):
        graph['kernel'].add('electron')
    graph['image'] |= selected
    return graph


def components_for(apps, targets=None):
    graph = graph_for(apps)
    if targets:
        selected = set()
        for name in words(targets):
            if name in APP_COMPONENTS:
                runtime_apps, _ = app_selection(name)
                selected |= set().union(*(APP_COMPONENTS[app] for app in runtime_apps))
            elif name in graph:
                selected.add(name)
            else:
                raise ValueError('Unknown component: ' + name)
    else:
        selected = {'image'}
    if targets:
        graph['kernel'].discard('electron')
        initial = closure(selected, graph)
        if 'electron' in initial:
            graph['kernel'].add('electron')
    return closure(selected, graph)


def receipt_path(output, name):
    directory = Path(output) / ('electron' if name == 'sources' else name)
    return directory / RECEIPTS[name]
