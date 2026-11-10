"""Compare browser acceptance evidence with the same binaries on Debian."""
import json
from pathlib import Path
import re
import subprocess
import sys


def read(path):
    """Read one required evidence object; an absent reference is an error."""
    return json.loads(path.read_text())


def sandbox(result, browser):
    """Keep sandbox policy fields, excluding process IDs and repeated renderer rows."""
    if browser == "firefox":
        text = result["about:support"].strip()
        assert text and "Sandbox" in text, text
        return text
    rows = sorted({json.dumps({k: row[k] for k in ("role", "seccomp", "namespaces")}, sort_keys=True)
                   for row in result["processes"]})
    roles = {row["role"] for row in result["processes"]}
    assert roles == {"renderer", "network", "storage", "gpu"}, roles
    for row in result["processes"]:
        assert row["seccomp"] == (2 if row["role"] in ("renderer", "storage") else 0), row
        assert row["namespaces"]["user"] == (row["role"] in ("renderer", "storage")), row
        assert row["namespaces"]["pid"] == (row["role"] in ("renderer", "storage")), row
    gpu = re.search(r"Sandboxed\s*:?\s*(true|false)", result["chrome://gpu"], re.I)
    assert gpu and gpu[1].lower() == "false", "missing GPU sandbox diagnostic"
    assert "Seccomp" in result["chrome://sandbox"], "missing Chromium sandbox diagnostic"
    return {"processes": rows, "gpu_sandboxed": gpu[1].lower(),
            "sandbox": result["chrome://sandbox"].strip()}


def canonical(result, mode, browser):
    """Return only the behavior a reference comparison judges."""
    if mode == "sandbox":
        return sandbox(result, browser)
    if mode == "recovery":
        assert result["old_pid"] != result["new_pid"] and result["losses"] >= 1, result
        assert result["before"] == result["after"], result
        return result["after"]
    if mode == "webgpu":
        return {"readback": result["readback"],
                "cts": {name: row["status"] for name, row in result["cts"]["results"]},
                "samples": result["samples"]}
    if mode == "sound":
        return result["tabs"]
    return result


def main():
    """Require complete evidence and compare the selected case with Debian's."""
    browser, mode, ref_name, got_name = sys.argv[1:]
    ref, got = Path(ref_name), Path(got_name)
    graphics = Path(__file__).resolve().parent.parent
    if mode == "webgl":
        name = "%s-2.0.1-00.pages" % browser
        base = graphics / "ref/debian/virtio-vga" / ("webgl-" + browser + "-2.0.1")
        args = ["webgl", str(base / name), str(got / name)]
    elif mode.startswith("video-"):
        clip = mode[6:] + (".mp4" if mode.startswith("video-h264-") else ".webm")
        name = browser + "-" + clip
        base = graphics / "ref/debian/virtio-vga" / ("video-" + browser)
        args = ["video", str(base / (name + ".plays")), str(got / (name + ".plays")),
                str(base / (name + ".decoders")), str(got / (name + ".decoders"))]
    else:
        base = ref / (browser + "-" + mode)
        expected, actual = read(base / "result.json"), read(got / "result.json")
        assert canonical(expected, mode, browser) == canonical(actual, mode, browser), (expected, actual)
        if mode == "sound":
            assert read(got / "sound.json")["mixed_windows"] >= 10
        expected_switches, actual_switches = read(base / "switches.json"), read(got / "switches.json")
        assert expected_switches["version"] == actual_switches["version"], "different browser binaries"
        print("browser-judge: result: pass %s %s" % (browser, mode))
        return
    versions = [next((line for line in (path / "record").read_text().splitlines()
                      if line.startswith("browser ")), None) for path in (base, got)]
    assert versions[0] and versions[0] == versions[1], ("different browser binaries", versions)
    subprocess.run([sys.executable, "-I", str(graphics / "runners/judge.py")] + args, check=True)
    print("browser-judge: result: pass %s %s" % (browser, mode))


if __name__ == "__main__":
    main()
