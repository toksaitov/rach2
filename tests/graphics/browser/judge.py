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


def crocus(result, browser, fake):
    """Require the GPU profile's evidence to name Crocus, and its pixel unless on the fake chip.

    The fake chip completes every batch without drawing, so that a readback
    there is whatever the buffer held; `fake` leaves the pixel unjudged.
    """
    if browser == "firefox":
        graphics = result["graphics"]
        assert graphics.get("Compositing") == "WebRender", graphics
        assert "crocus" in graphics.get("Driver Vendor", ""), graphics
        assert (graphics.get("Vendor ID"), graphics.get("Device ID")) == ("0x8086", "0x0a16"), graphics
    else:
        assert "HD Graphics 4400" in (result["gl_renderer"] or ""), result["gl_renderer"]
        assert result["webgl_status"] == "Hardware accelerated", result["webgl_status"]
        assert result["gpu0"] == ["0x8086", "0x0a16"], result["gpu0"]
    assert "llvmpipe" not in result["webgl"]["renderer"].lower(), result["webgl"]
    if not fake:
        assert result["webgl"]["pixel"] == [255, 128, 0, 255], result["webgl"]


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
    if mode in ("webgpu-venus", "webgpu-venus-native"):
        assert browser == "chromium" and result["platform"] == "wayland", result
        assert set(result["requests"]) == {"default", "fallback"}, result
        passes = 0
        for row in result["requests"].values():
            adapter, readback = row["adapter"], row["readback"]
            if adapter is not None:
                assert set(adapter) == {"vendor", "architecture", "description", "device", "fallback"}
                assert isinstance(adapter["fallback"], bool), adapter
            if adapter and re.search(r"\bVenus\b", adapter["description"], re.I) and not adapter["fallback"]:
                assert readback and readback["adapter"] == adapter, row
                assert readback["words"] == 1024 and readback["errors"] == 0, row
                assert readback["pixel"] == [255, 128, 0, 255], row
                passes += 1
            else:
                assert readback is None, "a non-Venus adapter counted as a WebGPU pass"
        assert result["venus_passes"] == passes, result
        return result
    if mode == "sound":
        return result["tabs"]
    return result


def main():
    """Require complete evidence and compare the selected case with Debian's.

    The arguments are the browser, the mode, the references, the evidence
    and, for the GPU profile on the fake chip, `fake`. The GPU profile's
    sandboxes are compared with Debian's sandbox case on the same binaries.
    """
    browser, mode, ref_name, got_name = sys.argv[1:5]
    fake = sys.argv[5:] == ["fake"]
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
                str(base / (name + ".decoders")), str(got / (name + ".decoders")),
                str(graphics / "runners/video.limits")]
    elif mode == "gpu":
        base = ref / (browser + "-sandbox")
        expected, actual = read(base / "result.json"), read(got / "result.json")
        assert sandbox(expected, browser) == sandbox(actual, browser), (expected, actual)
        crocus(actual, browser, fake)
        expected_switches, actual_switches = read(base / "switches.json"), read(got / "switches.json")
        assert expected_switches["version"] == actual_switches["version"], "different browser binaries"
        print("browser-judge: result: pass %s %s%s" % (browser, mode, " fake" if fake else ""))
        return
    else:
        base = ref / (browser + "-" + mode)
        expected, actual = read(base / "result.json"), read(got / "result.json")
        assert canonical(expected, mode, browser) == canonical(actual, mode, browser), (expected, actual)
        if mode == "sound":
            assert read(got / "sound.json")["mixed_windows"] >= 10
        expected_switches, actual_switches = read(base / "switches.json"), read(got / "switches.json")
        assert expected_switches["version"] == actual_switches["version"], "different browser binaries"
        if mode in ("webgpu-venus", "webgpu-venus-native"):
            switches = []
            devices = []
            for path, record in ((base, expected_switches), (got, actual_switches)):
                args = [s for s in record["argv"] if not s.startswith("--user-data-dir=")]
                assert "--ozone-platform=wayland" in args and "--enable-unsafe-webgpu" in args
                assert not any(s.startswith("--use-webgpu-adapter=") for s in args), args
                assert ("--use-vulkan=native" in args) == (mode == "webgpu-venus-native"), args
                assert record["environment"]["VK_DRIVER_FILES"] == "/usr/share/vulkan/icd.d/venus_icd.x86_64.json"
                vulkan = (path / "vulkaninfo.out").read_text()
                assert "Virtio-GPU Venus" in vulkan
                fields = ("apiVersion", "driverVersion", "vendorID", "deviceID", "deviceType",
                          "deviceName", "driverID", "driverName", "driverInfo", "conformanceVersion")
                device = [line.strip() for line in vulkan.splitlines()
                          if line.split("=", 1)[0].strip() in fields]
                assert len(device) == len(fields), "missing or multiple Vulkan devices"
                devices.append(device)
                diagnostic = (path / "gpu.txt").read_text()
                assert "Vulkan backend" in diagnostic, "missing Dawn adapter list"
                if mode == "webgpu-venus-native":
                    assert "Virtio-GPU Venus" in diagnostic, "Dawn did not enumerate Venus"
                switches.append(args)
            assert switches[0] == switches[1], "different browser switches"
            assert devices[0] == devices[1], "different Venus drivers or host devices"
            print("browser-judge: Venus readback passes: %d" % actual["venus_passes"])
        print("browser-judge: result: pass %s %s" % (browser, mode))
        return
    versions = [next((line for line in (path / "record").read_text().splitlines()
                      if line.startswith("browser ")), None) for path in (base, got)]
    assert versions[0] and versions[0] == versions[1], ("different browser binaries", versions)
    subprocess.run([sys.executable, "-I", str(graphics / "runners/judge.py")] + args, check=True)
    print("browser-judge: result: pass %s %s" % (browser, mode))


if __name__ == "__main__":
    main()
