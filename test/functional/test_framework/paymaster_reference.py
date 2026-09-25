#!/usr/bin/env python3
# Copyright (c) 2026 The DigiByte Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Pinned official reference releases for Paymaster compatibility scenarios."""

import os
import subprocess
from pathlib import Path

from test_framework.test_framework import SkipTest
from test_framework.util import assert_equal, assert_raises_rpc_error


REFERENCES = {
    "v9.26.5": (92605, "V9_26_5_DIGIBYTED"),
    "v9.26.6rc2": (92606, "V9_26_6RC2_DIGIBYTED"),
}


def add_reference_options(parser):
    parser.add_argument("--reference-release", choices=REFERENCES, default="v9.26.5",
                        help="Official non-Paymaster release to compare with this build")
    parser.add_argument("--thaw-day", action="store_true",
                        help="Activate Thaw Day at height 1 on both RC2 and current nodes")


def configure_reference(test):
    if test.options.thaw_day:
        if test.options.reference_release != "v9.26.6rc2":
            raise ValueError("--thaw-day requires --reference-release=v9.26.6rc2")
        for args in test.extra_args:
            args.append("-ddthawdayheight=1")


def locate_reference(test):
    tag = test.options.reference_release
    _, variable = REFERENCES[tag]
    override = os.getenv(variable)
    # Preserve the historical alias only for the historical reference.
    if not override and tag == "v9.26.5":
        variable = "PRE_PAYMASTER_DIGIBYTED"
        override = os.getenv(variable)
    binary = Path(override).expanduser() if override else Path(
        test.options.previous_releases_path, tag, "bin",
        "digibyted" + test.config["environment"]["EXEEXT"])
    if not binary.is_file():
        if override:
            raise AssertionError(f"{variable} does not name a file: {binary}")
        raise SkipTest(f"Official {tag} required at {binary}; set {REFERENCES[tag][1]}")
    version = subprocess.check_output([str(binary), "-version"], text=True, timeout=15).splitlines()[0]
    assert version.endswith("version " + tag), version
    test.log.info("Reference release %s: %s", tag, binary)
    return str(binary)


def assert_reference(test, node):
    tag = test.options.reference_release
    info = node.getnetworkinfo()
    assert_equal(info["version"], REFERENCES[tag][0])
    assert tag[1:].split("rc")[0] in info["subversion"], info["subversion"]
    assert_raises_rpc_error(-32601, "Method not found", node.getpaymasterinfo)


def assert_reference_rules(test):
    """Ensure the RC2 scenarios really exercised the selected consensus rules."""
    if test.options.reference_release != "v9.26.6rc2":
        return
    for node in test.nodes:
        state = node.getdigidollardeploymentinfo()["thaw_day"]
        assert_equal(state["active_at_tip"], test.options.thaw_day)
        assert_equal(state["active_next_block"], test.options.thaw_day)
        if test.options.thaw_day:
            assert_equal(state["height"], 1)
