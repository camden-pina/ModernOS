"""Literal package search with names and real commands ahead of metadata."""


def search(packages, queries):
    terms = [term.casefold() for query in queries for term in query.split()]
    matches = []
    for package in packages:
        name = package['Name'].casefold()
        commands = [value.casefold() for value in package.get('Commands', [])]
        aliases = [value.split('=', 1)[0].casefold() for value in package.get('Provides', [])]
        description = package.get('Description', '').casefold()
        scores = []
        for term in terms:
            candidates = []
            for values, exact, prefix, substring in [([name], 0, 1, 4),
                                                     (commands, 2, 3, 5),
                                                     (aliases, 6, 7, 8)]:
                for value in values:
                    if term == value:
                        candidates.append((exact, 0))
                    elif value.startswith(term):
                        candidates.append((prefix, len(value)-len(term)))
                    elif len(term) >= 2 and term in value:
                        candidates.append((substring, len(value)-len(term)))
            # A one-letter description search otherwise matches nearly every
            # graphical/build package. Longer descriptive queries still work.
            if len(term) >= 3 and term in description:
                candidates.append((9, 0))
            if not candidates:
                break
            scores.append(min(candidates))
        else:
            key = (sum(score[0] for score in scores), sum(score[1] for score in scores),
                   name, package.get('Repository', 'recipes'))
            matches.append((key, package))
    return [package for _, package in sorted(matches, key=lambda item: item[0])]
