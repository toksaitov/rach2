#!/usr/bin/env python3
"""Check focused agent selections, including their order and refusal paths."""
import json
from pathlib import Path
import subprocess
import sys
import tempfile


def main():
    """Run the selector on a private manifest; a refused selection writes nothing."""
    selector = sys.argv[1] if len(sys.argv) > 1 else "tests/cases/machine/agent_select.py"
    with tempfile.TemporaryDirectory(prefix="rach2-agent-select.") as scratch:
        source = Path(scratch) / "source.json"
        dest = Path(scratch) / "selected.json"
        cases = [{"case_id": "machine.agents." + n} for n in ["a", "b", "c"]]
        source.write_text(json.dumps({"family_id": "machine.agents", "cases": cases}))
        for names, wanted in [("c a", [cases[2], cases[0]]), ("b", [cases[1]])]:
            p = subprocess.run([sys.executable, selector, str(source), names, str(dest)],
                               capture_output=True, text=True, check=False)
            if p.returncode or json.loads(dest.read_text())["cases"] != wanted:
                print("agent-select-test: the selection differs from the requested order")
                return 1
        dest.unlink()
        for names in ["", "a a", "unknown", "b unknown"]:
            p = subprocess.run([sys.executable, selector, str(source), names, str(dest)],
                               capture_output=True, text=True, check=False)
            if p.returncode != 2 or dest.exists() or "agent-select:" not in p.stderr:
                print("agent-select-test: a refused selection wrote a manifest or gave no reason")
                return 1
    print("agent-select-test: pass")
    return 0


if __name__ == "__main__":
    sys.exit(main())
