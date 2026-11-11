# Corrections to the first run's records

The files beside this one are the run's raw output and are kept as written. Two of them, or what 78-11b.md read from them, are wrong:

* `environment.log`: the shader cache was not empty at the start. Weston was given the same `MESA_SHADER_CACHE_DIR` as the measured clients and was writing it while `guest-env.sh` removed it, so `rm -rf` failed on seven of its directories, "Directory not empty", and the cache kept 9 files, as the log's `MESA_SHADER_CACHE_FILES=9` says. `guest-env.sh` did not pass the failure on, so the runner went on. vkcube's first timed run therefore began with the compositor's 9 files in its cache, not an empty one. `hasvk.py` now gives Weston a cache of its own, `guest-env.sh` fails when it cannot empty the cache, and the runner requires `MESA_SHADER_CACHE_FILES=0`.
* `webgpu/record`: its `size 1280x800` is the runners' default for a headless Weston, recorded before this mode took the caller's DRM Weston. The page drew at the panel's 1366x768, as `webgpu/result.json` and `webgpu/panel.png` show. `browser/run.sh` now sets the size for this mode before it records it.
