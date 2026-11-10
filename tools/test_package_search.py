#!/usr/bin/env python3
"""Regress the reported search order against real package metadata."""
import argparse
import importlib.util
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('brine_search', ROOT/'userland/packages/search.py')
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--repository', type=Path, required=True)
    parser.add_argument('--output-root', type=Path, required=True)
    args = parser.parse_args()
    output = args.output_root.resolve()
    output.mkdir(parents=True, exist_ok=False)
    data = json.loads(args.repository.read_text())
    packages = []
    for name, record in data['packages'].items():
        commands = sorted({Path(value).name for value in [*record['files'], *record['aliases']]
                           if str(Path(value).parent) in ('bin', 'usr/bin')})
        packages.append(dict(Name=name, Description=record['description'], Provides=record['provides'],
                             Commands=commands, Repository='coast'))
    for record in data['recipes']:
        packages.append(dict(record, Repository='recipes'))
    checks = []

    def names(query):
        return [p['Name'] for p in module.search(packages, [query])]

    def check(name, value):
        assert value, name
        checks.append(name)
        print('PASS '+name)

    g = names('g')
    check('GCC is the first actual repository result for g', g[0] == 'gcc')
    check('Qt does not match a single letter in descriptive metadata', 'qt6-demo' not in g)
    check('real compiler is ahead of build metapackages', all(g.index('gcc') < g.index(n) for n in ['coast-build', 'coast-base'] if n in g))
    check('Git and Gawk remain discoverable by prefix', {'git', 'gawk'} <= set(g))
    check('G++ is literal and finds the owning compiler package', names('g++') == ['gcc'])
    check('search is case-insensitive', names('G') == g and names('G++') == names('g++'))
    check('long description search still finds calculators', 'bc' in names('calculator'))
    check('multiple terms must all match', 'gcc' in names('g++ compiler') and not names('g++ impossible-nonexistent'))
    check('regex syntax is not interpreted', not names('.*'))
    check('name substrings still work for useful partial names', 'gcc' in names('cc'))
    check('empty query returns every package and recipe', len(module.search(packages, [])) == len(packages))
    (output/'report.json').write_text(json.dumps(dict(state='passed', checks=checks, g_results=g), indent=2)+'\n')


if __name__ == '__main__':
    main()
