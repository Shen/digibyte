#!/usr/bin/env python3
# Copyright (c) 2026 The DigiByte Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.

"""Check Paymaster boundaries. Candidate files are data, never executed.

Without --base: working-tree dependency checks (including untracked sources).
With --base/--head: policy is read from the trusted base, sources/records from
the candidate. Bootstrap and policy changes deliberately require owner review.
"""

import argparse
import fnmatch
import json
from pathlib import Path
import posixpath
import re
import subprocess
import sys


POLICY = "test/lint/paymaster-boundaries.json"
RECORD = "doc/design/paymaster-change-record.json"
CYCLES = "contrib/devtools/circular-dependencies.py"
INCLUDE = re.compile(r'^\s*#\s*include\s*([<"])([^>"\r\n]+)[>"]', re.MULTILINE)


def matches(path, patterns):
    return any(fnmatch.fnmatchcase(path.lower(), pattern.lower()) for pattern in patterns)


def dependency_errors(path, source, policy):
    errors = []
    # Check logical preprocessing lines, including deliberately split includes.
    source = re.sub(r'\\\r?\n', '', source)
    # Preserve newlines for diagnostics; comments are not dependencies.
    source = re.sub(r'/\*.*?\*/', lambda match: '\n' * match[0].count('\n'), source, flags=re.DOTALL)
    source = re.sub(r'//[^\n]*', '', source)
    for rule in policy["dependencies"]:
        if not matches(path, rule["sources"]):
            continue
        for directive in re.finditer(r'^\s*#\s*include\b([^\n]*)', source, re.MULTILINE):
            if not INCLUDE.match(directive[0]):
                errors.append(f"{path}: computed include cannot be checked against architecture boundaries")
        for match in INCLUDE.finditer(source):
            name = match[2].replace('\\', '/')
            if name.startswith('/') or ':' in name:
                errors.append(f"{path}: absolute include outside source ownership: {match[2]}")
                continue
            # Project includes are source-root relative; also resolve explicit
            # relative includes so ../rpc/foo.h cannot evade the boundary.
            if name.startswith('.'):
                name = posixpath.normpath(posixpath.join(posixpath.dirname(path), name))
                if not name.startswith('src/'):
                    errors.append(f"{path}: include escapes the source root: {match[2]}")
                    continue
                name = name.removeprefix('src/')
            else:
                name = posixpath.normpath(name)
            if matches(name, rule["deny"]):
                line = source.count('\n', 0, match.start()) + 1
                errors.append(f"{path}:{line}: forbidden dependency {match[2]}")
    return errors


def change_errors(paths, policy, record):
    errors = []
    paths = set(paths)
    control_changes = {p for p in paths if matches(p, policy["controls"])}
    if control_changes:
        errors.append("policy/control changes require separate owner review: " + ', '.join(sorted(control_changes)))
    # Ordinary unrelated Core work is not a Paymaster scope request.
    scoped = control_changes or any(matches(p, policy["owned"]) for p in paths)
    if not scoped:
        return errors
    records = record.get("shared_changes", {})
    for path in sorted(paths):
        if matches(path, policy["forbidden"]):
            errors.append(f"{path}: consensus/activation/PoW/script change outside Paymaster scope")
        elif matches(path, policy["controls"]):
            continue
        elif matches(path, policy["shared"]):
            entry = records.get(path, {})
            for field in ("purpose", "reused_api", "ordinary_callers", "regressions"):
                value = entry.get(field)
                if not isinstance(value, str) or not value.strip():
                    errors.append(f"{path}: missing shared-Core record field {field}")
        elif not matches(path, policy["owned"] + policy["support"]):
            errors.append(f"{path}: outside agreed Paymaster change surface")
    return errors


def git(root, *args):
    return subprocess.check_output(['git', '-C', str(root), *args], stderr=subprocess.PIPE)


def resolve(root, ref):
    oid = git(root, 'rev-parse', '--verify', '--end-of-options', f'{ref}^{{commit}}').decode().strip()
    if not re.fullmatch(r'[0-9a-f]{40,64}', oid):
        raise ValueError('Invalid commit identifier')
    return oid


def read_blob(root, revision, path):
    if revision:
        content = git(root, 'show', f'{revision}:{path}')
    else:
        target = root / path
        if target.is_symlink() or not target.resolve().is_relative_to(root.resolve()):
            raise ValueError(f'{path}: source must remain inside the workspace')
        content = target.read_bytes()
    if len(content) > 8 * 1024 * 1024:
        raise ValueError(f'{path}: oversized source/policy blob')
    return content.decode('utf-8-sig')


def source_includes(root, revision=None, complete_paths=()):
    """Read include lines in one Git scan; untracked local files also count."""
    if revision:
        paths = git(root, 'ls-tree', '-r', '--name-only', revision, '--', 'src').decode().splitlines()
    else:
        paths = git(root, 'ls-files', '--cached', '--others', '--exclude-standard', '--', 'src').decode().splitlines()
    sources = {path.removeprefix('src/'): '' for path in paths if path.endswith(('.cpp', '.h', '.c'))}
    command = ['git', '-C', str(root), 'grep', '-I', '-E', r'^\s*#\s*include\s*[<"]']
    if revision:
        command.append(revision)
    command.extend(['--', 'src'])
    result = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=False)
    if result.returncode not in (0, 1):
        raise ValueError('Cannot scan source dependencies: ' + result.stderr.decode())
    for line in result.stdout.decode('utf-8').splitlines():
        if revision:
            line = line.split(':', 1)[1]
        path, _, include = line.partition(':')
        path = path.removeprefix('src/')
        if path in sources:
            sources[path] += include + '\n'
    if not revision:
        for path in git(root, 'ls-files', '--others', '--exclude-standard', '--', 'src').decode().splitlines():
            if path.removeprefix('src/') in sources:
                sources[path.removeprefix('src/')] = read_blob(root, None, path)
    # Read changed files in full in both snapshots. A multiline directive or
    # comment must not disappear merely because the fast grep selected one line.
    for path in complete_paths:
        if path.removeprefix('src/') in sources:
            sources[path.removeprefix('src/')] = read_blob(root, revision, path)
    return sources


def cycle_errors(root, base=None, head=None):
    # Only the target branch's helper is executed. Candidate source is data.
    namespace = {'__name__': 'trusted_circular_dependencies'}
    exec(compile(read_blob(root, base, CYCLES), CYCLES, 'exec'), namespace)
    baseline = base or resolve(root, 'HEAD')
    revisions = [baseline, head] if head else [baseline]
    changed = set(git(root, 'diff', '--no-renames', '--name-only', *revisions, '--', 'src').decode().splitlines())
    if not head:
        changed.update(git(root, 'ls-files', '--others', '--exclude-standard', '--', 'src').decode().splitlines())
    before = namespace['dependency_graph'](source_includes(root, baseline, changed))
    after = namespace['dependency_graph'](source_includes(root, head, changed))
    return [f'new cyclic dependency: {source} -> {target}'
            for source, target in namespace['added_cycle_edges'](before, after)]


def check(root, base=None, head=None):
    base = resolve(root, base) if base else None
    head = resolve(root, head) if head else None
    try:
        policy = json.loads(read_blob(root, base, POLICY))
    except (subprocess.CalledProcessError, FileNotFoundError) as error:
        raise ValueError('Trusted base has no boundary policy; owner-reviewed bootstrap is required') from error
    if policy.get('version') != 1:
        raise ValueError('Unsupported boundary policy version')
    errors = []
    if head:
        paths = git(root, 'ls-tree', '-r', '--name-only', '-z', head).decode().split('\0')
        # --no-renames includes both sides of a rename and catches moving a
        # protected source into a permitted directory.
        changed = git(root, 'diff', '--no-renames', '--name-only', '-z', base, head, '--').decode().split('\0')
        changed = [path for path in changed if path]
        try:
            record = json.loads(read_blob(root, head, RECORD))
        except (subprocess.CalledProcessError, FileNotFoundError):
            record = {}
        errors += change_errors(changed, policy, record)
    else:
        paths = git(root, 'ls-files', '-z', '--cached', '--others', '--exclude-standard').decode().split('\0')
    for path in sorted(set(paths)):
        guarded = matches(path, policy['owned']) or any(
            matches(path, rule['sources']) for rule in policy['dependencies'])
        if path.endswith(('.cpp', '.h')) and guarded:
            if not head and not (root / path).exists():
                continue
            errors += dependency_errors(path, read_blob(root, head, path), policy)
    errors += cycle_errors(root, base, head)
    return errors


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, default=Path(__file__).resolve().parents[2])
    parser.add_argument('--base')
    parser.add_argument('--head')
    args = parser.parse_args()
    if bool(args.base) != bool(args.head):
        parser.error('--base and --head must be supplied together')
    try:
        errors = check(args.root, args.base, args.head)
    except (ValueError, OSError, subprocess.CalledProcessError) as error:
        errors = [str(error)]
    for error in errors:
        print(error, file=sys.stderr)
    if not errors:
        print('Paymaster boundaries: passed' + (' (trusted base)' if args.base else ' (working-tree dependencies only)'))
    return bool(errors)


if __name__ == '__main__':
    sys.exit(main())
