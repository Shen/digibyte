#!/usr/bin/env python3
# Copyright (c) 2018-2020 The DigiByte Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.

import sys
import re
import posixpath
from typing import Dict, List, Set

MAPPING = {
    'core_read.cpp': 'core_io.cpp',
    'core_write.cpp': 'core_io.cpp',
}

# Directories with header-based modules, where the assumption that .cpp files
# define functions and variables declared in corresponding .h files is
# incorrect.
HEADER_MODULE_PATHS = [
    'interfaces/'
]

def module_name(path):
    if path in MAPPING:
        path = MAPPING[path]
    if any(path.startswith(dirpath) for dirpath in HEADER_MODULE_PATHS):
        return path
    if path.endswith(".h"):
        return path[:-2]
    if path.endswith(".c"):
        return path[:-2]
    if path.endswith(".cpp"):
        return path[:-4]
    return None

def dependency_graph(sources):
    """Use the same module identities as the CLI for source snapshots."""
    files = {path: module_name(path) for path in sources}
    deps = {module: set() for module in files.values() if module is not None}
    for path, source in sources.items():
        module = files[path]
        if module is None:
            continue
        source = re.sub(r'\\\r?\n', '', source)
        source = re.sub(r'/\*.*?\*/', '', source, flags=re.DOTALL)
        source = re.sub(r'//[^\n]*', '', source)
        for quote, include in re.findall(r'^\s*#\s*include\s*([<"])([^>"\n]+)[>"]', source, re.MULTILINE):
            include = include.replace('\\', '/')
            local = posixpath.normpath(posixpath.join(posixpath.dirname(path), include))
            include = local if quote == '"' and local in files else posixpath.normpath(include)
            target = module_name(include)
            if target in deps and target != module:
                deps[module].add(target)
    return deps


def added_cycle_edges(before, after):
    """Report newly introduced edges that can reach their own source.

    Existing cyclic edges are tolerated, never new edges (including edges
    extending an existing cycle). This avoids a maintained cycle allow-list.
    """
    result = []
    for source, targets in sorted(after.items()):
        for target in sorted(targets - before.get(source, set())):
            pending = [target]
            seen = set()
            while pending:
                node = pending.pop()
                if node == source:
                    result.append((source, target))
                    break
                if node in seen:
                    continue
                seen.add(node)
                pending.extend(after.get(node, set()) - seen)
    return result


def main():
    files = dict()
    deps: Dict[str, Set[str]] = dict()

    RE = re.compile("^#include <(.*)>")

    # Iterate over files, and create list of modules
    for arg in sys.argv[1:]:
        module = module_name(arg)
        if module is None:
            print("Ignoring file %s (does not constitute module)\n" % arg)
        else:
            files[arg] = module
            deps[module] = set()

    # Iterate again, and build list of direct dependencies for each module
    # TODO: implement support for multiple include directories
    for arg in sorted(files.keys()):
        module = files[arg]
        with open(arg, 'r', encoding="utf8") as f:
            for line in f:
                match = RE.match(line)
                if match:
                    include = match.group(1)
                    included_module = module_name(include)
                    if included_module is not None and included_module in deps and included_module != module:
                        deps[module].add(included_module)

    # Loop to find the shortest (remaining) circular dependency
    have_cycle: bool = False
    while True:
        shortest_cycle = None
        for module in sorted(deps.keys()):
            # Build the transitive closure of dependencies of module
            closure: Dict[str, List[str]] = dict()
            for dep in deps[module]:
                closure[dep] = []
            while True:
                old_size = len(closure)
                old_closure_keys = sorted(closure.keys())
                for src in old_closure_keys:
                    for dep in deps[src]:
                        if dep not in closure:
                            closure[dep] = closure[src] + [src]
                if len(closure) == old_size:
                    break
            # If module is in its own transitive closure, it's a circular dependency; check if it is the shortest
            if module in closure and (shortest_cycle is None or len(closure[module]) + 1 < len(shortest_cycle)):
                shortest_cycle = [module] + closure[module]
        if shortest_cycle is None:
            break
        # We have the shortest circular dependency; report it
        module = shortest_cycle[0]
        print("Circular dependency: %s" % (" -> ".join(shortest_cycle + [module])))
        # And then break the dependency to avoid repeating in other cycles
        deps[shortest_cycle[-1]] = deps[shortest_cycle[-1]] - set([module])
        have_cycle = True

    sys.exit(1 if have_cycle else 0)


if __name__ == '__main__':
    main()
