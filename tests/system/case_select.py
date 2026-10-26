#!/usr/bin/env python3
"""Choose the cases of a selection of suites: RACH2_CASES and the agents' rows.

Usage: case_select.py [-c IDENTITIES] [-r SUITE ROWS]... ARCH CASES OUT SUITE...

IDENTITIES are case identities separated by blanks, such as
`guest.default.vforkmusl machine.ext2.cost`; each suite of the run that
they name runs the cases listed, in its manifest's order, and with -c every
suite of the run must be named. ROWS are the last parts of identities of
SUITE's cases, run in the order given; a SUITE that is not a suite of the
run is passed over. CASES is the directory of the manifests, the manifest
of a suite being the first `*.SUITE.json` in it, and OUT the directory that
gets SUITE.json for each suite chosen from. ARCH is the run's architecture,
or its two separated by a blank, and a case chosen must run on each.

The program prints the number of cases chosen, or nothing when no suite
was chosen from, and exits 0. A choice it refuses, an identity or row that
names no case of a suite of the run, one given twice, a suite left with no
case, or a suite that both -c and -r choose from, writes nothing, is said
on stderr and exits 2.
"""
import argparse
import glob
import json
import os
import sys


class Refused(Exception):
    """A choice of cases that the run refuses."""


def manifest_of(cases, suite):
    """Read the manifest of SUITE's family under CASES."""
    paths = sorted(glob.glob(os.path.join(glob.escape(cases), "*." + suite + ".json")))
    if not paths:
        raise Refused(f"no family of cases is the suite {suite}")
    with open(paths[0], encoding="utf-8") as stream:
        return json.load(stream)


def runs_on(case, arch):
    """Say whether CASE runs on ARCH, as `etude run suites` decides it."""
    archs = case.get("architectures")
    return archs == "none" or (isinstance(archs, list) and arch in archs)


def choose(manifest, wanted, arch, ordered):
    """Give the cases of MANIFEST whose identities are WANTED, in WANTED's
    order when ORDERED and in the manifest's otherwise."""
    cases = {case["case_id"]: case for case in manifest["cases"]}
    for name in wanted:
        for one in arch.split():
            if not runs_on(cases[name], one):
                raise Refused(f"{name} does not run on {one}")
    if ordered:
        return [cases[name] for name in wanted]
    return [case for case in manifest["cases"] if case["case_id"] in wanted]


def select(identities, rows, arch, cases, suites):
    """Give each suite chosen from with the manifest of its chosen cases."""
    manifests = {suite: manifest_of(cases, suite) for suite in suites}
    families = {m["family_id"]: suite for suite, m in manifests.items()}
    chosen = {}
    if identities is not None:
        wanted = identities.split()
        if not wanted:
            raise Refused("RACH2_CASES names no case")
        per_suite = {}
        for identity in wanted:
            family, _, _ = identity.rpartition(".")
            suite = families.get(family)
            if suite is None or identity not in {c["case_id"] for c in manifests[suite]["cases"]}:
                raise Refused(f"{identity} is no case of a suite of the run")
            if identity in per_suite.get(suite, []):
                raise Refused(f"{identity} is given twice")
            per_suite.setdefault(suite, []).append(identity)
        for suite in suites:
            if suite not in per_suite and suite not in rows:
                raise Refused(f"the selection leaves the suite {suite} with no case")
        for suite, names in per_suite.items():
            if suite in rows:
                raise Refused(f"the suite {suite} is chosen from twice")
            chosen[suite] = choose(manifests[suite], names, arch, False)
    for suite, names in rows.items():
        if suite not in manifests:
            continue
        family = manifests[suite]["family_id"]
        wanted = [family + "." + name for name in names.split()]
        ids = {c["case_id"] for c in manifests[suite]["cases"]}
        if not wanted or len(set(wanted)) != len(wanted) or any(n not in ids for n in wanted):
            raise Refused(f"the rows of {suite} have an unknown or repeated name")
        chosen[suite] = choose(manifests[suite], wanted, arch, True)
    return {suite: dict(manifests[suite], cases=picked) for suite, picked in chosen.items()}


def main():
    """Write the manifests of the chosen cases and print their number."""
    parser = argparse.ArgumentParser(prog="case-select")
    parser.add_argument("-c", dest="identities")
    parser.add_argument("-r", dest="rows", nargs=2, action="append", default=[])
    parser.add_argument("arch")
    parser.add_argument("cases")
    parser.add_argument("out")
    parser.add_argument("suites", nargs="+")
    args = parser.parse_args()
    try:
        chosen = select(args.identities, dict(args.rows), args.arch, args.cases, args.suites)
        os.makedirs(args.out, exist_ok=True)
        for suite, manifest in chosen.items():
            with open(os.path.join(args.out, suite + ".json"), "w", encoding="utf-8") as stream:
                json.dump(manifest, stream)
                stream.write("\n")
    except (OSError, KeyError, TypeError, ValueError, Refused) as error:
        print(f"case-select: {error}", file=sys.stderr)
        return 2
    if chosen:
        print(sum(len(m["cases"]) for m in chosen.values()))
    return 0


if __name__ == "__main__":
    sys.exit(main())
