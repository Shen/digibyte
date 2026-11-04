#!/usr/bin/env python3
# Copyright (c) 2026 The DigiByte Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.

"""Negative and positive fixtures for the trusted Paymaster boundary checker."""

import importlib.util
import json
from pathlib import Path
import subprocess
import tempfile
import unittest


HERE = Path(__file__).resolve().parent
SPEC = importlib.util.spec_from_file_location('boundaries', HERE / 'lint-paymaster-boundaries.py')
BOUNDARIES = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(BOUNDARIES)
POLICY = json.loads((HERE / 'paymaster-boundaries.json').read_text(encoding='utf-8'))


class BoundaryTests(unittest.TestCase):
    def test_dependency_layers(self):
        for path, header in [('src/paymaster/x.cpp', 'wallet/wallet.h'),
                             ('src/paymaster/x.h', '../rpc/server.h'),
                             ('src/wallet/paymasterservice.cpp', 'wallet/rpc/paymaster.h'),
                             ('src/wallet/paymasterservice.cpp', 'qt/walletmodel.h'),
                             ('src/qt/walletasync.cpp', 'qt/paymasterasync.h'),
                             ('src/qt/walletasync.h', 'paymaster/error.h'),
                             ('src/qt/paymasterpage.cpp', 'wallet/wallet.h')]:
            with self.subTest(path=path, header=header):
                self.assertTrue(BOUNDARIES.dependency_errors(path, f'#include "{header}"\n', POLICY))
        self.assertFalse(BOUNDARIES.dependency_errors('src/paymaster/x.cpp', '#include <primitives/transaction.h>\n', POLICY))
        self.assertFalse(BOUNDARIES.dependency_errors('src/qt/paymasterpage.cpp', '#include <interfaces/wallet.h>\n', POLICY))
        self.assertFalse(BOUNDARIES.dependency_errors('src/paymaster/x.cpp', '/*\n#include <wallet/wallet.h>\n*/', POLICY))
        for source in ['#define HEADER "wallet/wallet.h"\n#include HEADER',
                       '#include <C:/repo/src/wallet/wallet.h>',
                       '#include "../../../wallet.h"',
                       '#include \\\n<rpc/server.h>']:
            self.assertTrue(BOUNDARIES.dependency_errors('src/paymaster/x.cpp', source, POLICY))

    def test_scope_and_shared_record(self):
        own = 'src/paymaster/client.cpp'
        for path in ['src/consensus/params.h', 'src/script/interpreter.cpp', 'src/net.cpp', 'src/validation.cpp']:
            self.assertTrue(BOUNDARIES.change_errors([own, path], POLICY, {}))
        shared = 'src/wallet/wallet.cpp'
        self.assertTrue(BOUNDARIES.change_errors([own, shared], POLICY, {}))
        record = {'shared_changes': {shared: dict.fromkeys(
            ['purpose', 'reused_api', 'ordinary_callers', 'regressions'], 'Documented fixture')}}
        self.assertFalse(BOUNDARIES.change_errors([own, shared], POLICY, record))
        self.assertFalse(BOUNDARIES.change_errors([own], POLICY, {}))
        self.assertFalse(BOUNDARIES.change_errors(['src/validation.cpp'], POLICY, {}))

    def test_new_cycles_and_existing_cycles(self):
        namespace = {'__name__': 'cycle_fixture'}
        exec((HERE.parents[1] / BOUNDARIES.CYCLES).read_text(encoding='utf-8'), namespace)
        graph = namespace['dependency_graph']
        added = namespace['added_cycle_edges']
        before = graph({'a.h': '#include <b.h>', 'b.h': '', 'interfaces/node.h': ''})
        after = graph({'a.h': '#include <b.h>', 'b.h': '#include "a.h"', 'interfaces/node.h': ''})
        self.assertEqual(added(before, after), [('b', 'a')])
        self.assertEqual(added(after, after), [])
        extended = graph({'a.h': '#include <b.h>\n#include <c.h>', 'b.h': '#include <a.h>', 'c.h': '#include <a.h>'})
        self.assertEqual(set(added(after, extended)), {('a', 'c'), ('c', 'a')})
        self.assertIn('interfaces/node.h', before)
        relative = graph({'a/a.h': '#include "../b/b.h"', 'b/b.h': '#include "../a/a.h"'})
        self.assertEqual(set(added({}, relative)), {('a/a', 'b/b'), ('b/b', 'a/a')})

    def test_candidate_cannot_replace_policy(self):
        with tempfile.TemporaryDirectory(prefix='paymaster-boundaries-') as directory:
            root = Path(directory)

            def git(*args):
                return subprocess.check_output(['git', '-C', directory, *args], stderr=subprocess.PIPE).decode().strip()

            git('init')
            git('config', 'user.name', 'Boundary fixture')
            git('config', 'user.email', 'boundary@example.invalid')
            target = root / BOUNDARIES.POLICY
            target.parent.mkdir(parents=True)
            target.write_text(json.dumps(POLICY), encoding='utf-8')
            helper = root / BOUNDARIES.CYCLES
            helper.parent.mkdir(parents=True)
            helper.write_text((HERE.parents[1] / BOUNDARIES.CYCLES).read_text(encoding='utf-8'), encoding='utf-8')
            git('add', '.')
            git('commit', '-m', 'trusted policy')
            base = git('rev-parse', 'HEAD')
            self.assertFalse(BOUNDARIES.check(root, base, base))
            candidate = json.loads(json.dumps(POLICY))
            candidate['dependencies'] = []
            candidate['forbidden'] = []
            target.write_text(json.dumps(candidate), encoding='utf-8')
            source = root / 'src/paymaster/bad.cpp'
            source.parent.mkdir(parents=True)
            source.write_text('#include <wallet/wallet.h>\n', encoding='utf-8')
            (source.parent / 'a.h').write_text('#include \\\n"b.h"\n', encoding='utf-8')
            (source.parent / 'b.h').write_text('#include "a.h"\n', encoding='utf-8')
            helper.write_text('raise RuntimeError("Candidate helper must never execute")\n', encoding='utf-8')
            git('add', '.')
            git('commit', '-m', 'attempt to weaken policy')
            errors = BOUNDARIES.check(root, base, git('rev-parse', 'HEAD'))
            self.assertTrue(any('forbidden dependency' in error for error in errors))
            self.assertTrue(any('owner review' in error for error in errors))
            self.assertTrue(any('new cyclic dependency' in error for error in errors))

    def test_shared_dependencies_are_scanned(self):
        with tempfile.TemporaryDirectory(prefix='paymaster-shared-') as directory:
            root = Path(directory)
            def git(*args):
                return subprocess.check_output(['git', '-C', directory, *args], stderr=subprocess.PIPE).decode().strip()
            git('init')
            git('config', 'user.name', 'Boundary fixture')
            git('config', 'user.email', 'boundary@example.invalid')
            for name in [BOUNDARIES.POLICY, BOUNDARIES.CYCLES]:
                target = root / name
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_text((HERE.parents[1] / name).read_text(encoding='utf-8'), encoding='utf-8')
            git('add', '.')
            git('commit', '-m', 'trusted policy')
            source = root / 'src/qt/walletasync.cpp'
            source.parent.mkdir(parents=True)
            source.write_text('#include <qt/paymasterasync.h>\n', encoding='utf-8')
            self.assertTrue(any('forbidden dependency' in error for error in BOUNDARIES.check(root)))
            source.write_text('#include <interfaces/wallet.h>\n', encoding='utf-8')
            self.assertFalse(BOUNDARIES.check(root))


if __name__ == '__main__':
    unittest.main()
