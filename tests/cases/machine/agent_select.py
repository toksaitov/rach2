#!/usr/bin/env python3
"""Select agent cases for a focused proof, in the caller's requested order."""
import json
import sys


def select(source, names, destination):
    """Write a manifest containing exactly NAMES; refuse unknown or repeated rows."""
    with open(source, encoding="utf-8") as stream:
        manifest = json.load(stream)
    cases = {case["case_id"].rsplit(".", 1)[1]: case for case in manifest["cases"]}
    wanted = names.split()
    if not wanted or len(set(wanted)) != len(wanted) or any(n not in cases for n in wanted):
        raise ValueError("the selection has an unknown or repeated agent row")
    manifest["cases"] = [cases[n] for n in wanted]
    with open(destination, "w", encoding="utf-8") as stream:
        json.dump(manifest, stream)
        stream.write("\n")


if __name__ == "__main__":
    try:
        select(*sys.argv[1:])
    except (OSError, TypeError, ValueError) as error:
        print(f"agent-select: {error}", file=sys.stderr)
        sys.exit(2)
