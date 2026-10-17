# The agents layer

This directory adds to the x86-64 [GNU system](../Readme.md) the programs a person uses to work with a coding agent over SSH: each vendor's published build, fetched for the person who builds the image, checked against the digest its vendor publishes, and installed unmodified under its own license. Nothing of a package is committed but its recipe. The layer has three packages, GitHub's `gh`, Anthropic's Claude Code, `claude`, and Meta's Muse Code, `muse`. A package is installed only when the builder names it.

## Use

`GNU_AGENTS` lists the packages the GNU root gets, by the names of their recipes in [`packages/`](packages), or is `all` for every package; it is empty by default. The packages are x86-64 programs, so the variable needs `ARCH=x86_64`:

    make ARCH=x86_64 GNU_AGENTS=gh os-real-gnu-ext2
    make ARCH=x86_64 GNU_AGENTS=all machine

`make ARCH=x86_64 GNU_AGENTS=gh gnu-agents` fetches and stages the packages without building the root. A change to the list, to a recipe or to `fetch_agents.sh` builds the root again; [`mk/gnu-agents.mk`](../../../mk/gnu-agents.mk) has the rules. The release to the installed machine builds with `GNU_AGENTS=all` (`tools/flow/release.sh`), and so does the root `tests/run agents` boots, `os-real-gnu-agents.ext2`.

On the machine each package is a directory of `/opt`, as its vendor published it, with a link to its program in `/usr/bin` and `/bin`. Its page in the manual says how to sign in over SSH and where the credentials are kept: [gh(1)](../../../man/man1/gh.1), [claude(1)](../../../man/man1/claude.1) and [muse(1)](../../../man/man1/muse.1).

## Sources and license

| Package | Version | License | From | Pinned by |
| --- | --- | --- | --- | --- |
| [`gh`](packages/gh.sh) | 2.102.0 | MIT | `gh_2.102.0_linux_amd64.tar.gz` of GitHub's release `v2.102.0` of `cli/cli` | the SHA-256 the release's `gh_2.102.0_checksums.txt` lists |
| [`claude`](packages/claude.sh) | 2.1.296 | Proprietary | `linux-x64/claude` of Anthropic's release `2.1.296` at `downloads.claude.ai/claude-code-releases`, and `LICENSE.md` of the npm package `@anthropic-ai/claude-code` 2.1.296 | the SHA-256 the release's `manifest.json` lists, the manifest's signature by the key with fingerprint `31DDDE24DDFAB679F42D7BD2BAA929FF1A7ECACE`, the digests of the manifest, its signature and the key, and the SHA-512 the npm registry lists for the package |
| [`muse`](packages/muse.sh) | 1.4.10-R6427.1 | [Meta Model API Terms of Service](https://dev.meta.ai/legal/terms-of-service) | `muse-x86-linux` of Meta's release `1.4.10-R6427.1` | the SHA-256 that the release's `manifest.json` lists for `x86_linux` |

A package keeps its vendor's license, which the recipe names in `LICENSE` and installs beside the program; `NOTICES` lists those files, and the stage fails without them. Meta publishes no license file for Muse Code, so its recipe writes `/opt/muse/TERMS`, which names the terms and gives their address. Claude Code is proprietary: the recipe fetches it for the person who builds the image, and nothing of it is redistributed. The recipes and `fetch_agents.sh` are in-house.

## Design

### Recipes

A recipe, `packages/NAME.sh`, is a shell fragment that [`fetch_agents.sh`](fetch_agents.sh) reads in a shell of its own. Its header comment says what the program is, where it comes from and how it is pinned. It sets:

* `VERSION`, the version the vendor published.
* `LICENSE`, the license's short name.
* `FILES`, one line for each download: the name it is kept under, its URL, and its digest as the vendor publishes it, `sha256:HEX` or `sha512:HEX`.
* `NOTICES`, the license and notice files, as paths in the stage.

It defines `package_stage DIR`, which lays the package out under `DIR` as the root has it, taking each download through `fetched NAME`. It may define `package_verify`, which checks the vendor's signature on a download, and `package_check DIR`, which runs the staged program on the build host. Both may write in `SCRATCH`, an empty directory of the package's own, which the program may take as its home.

### Fetching and staging

`fetch_agents.sh NAME...` downloads each file of `FILES` once into `../downloads/agents/NAME/`, which Git ignores, and checks it against its digest each time `fetched` names it, so a download is checked again at every use; a file with another digest is removed and refused. It then makes `SCRATCH`, runs `package_verify`, stages the package from nothing into `agents/NAME/` of the layer's build tree, `build/x86_64/vendor/gnu/agents/NAME/`, checks that each of `NOTICES` is there, runs `package_check`, and removes `SCRATCH`. Make runs it behind a stamp for each package, `agents/NAME.stamp`.

### Installing

[`install_root.sh`](../install_root.sh) copies each stage that `GNU_AGENTS` names into the root after the Chromium packages, and refuses a file the root already has, as it does for every part of the layer. Its later steps give each program of `/usr/bin` its name in `/bin`.

### gh

`gh` is a static Go executable and needs no library of the root. Under `/opt/gh` it has `bin/gh`, its manual pages in `share/man/man1` and `LICENSE`. Measured on Linux, it starts no other process for `--version`, `auth status` or `api`. It writes its settings to `~/.config/gh`, and a device identifier and the state of its update check to `~/.local/state/gh`. Before a request it asks the Secret Service of the D-Bus session bus for a stored token, at the address `DBUS_SESSION_BUS_ADDRESS` gives or at `/run/user/UID/bus`, and reads the token from `~/.config/gh/hosts.yml` where there is none. It talks to `api.github.com` and `github.com`, or to the host `GH_HOST` names, whose API it reaches at `https://HOST/api/v3/`, and trusts the certificate authorities of `/etc/ssl/certs`, with those of the file `SSL_CERT_FILE` names. It does not update itself: on a terminal it checks GitHub for a newer release once a day and prints a notice, which `GH_NO_UPDATE_NOTIFIER` turns off. Its telemetry, on by default, is turned off by `gh config set telemetry disabled` or `GH_TELEMETRY=false`.

### claude

Claude Code is one executable of 257 MB, a program for Bun, a JavaScript runtime, with the runtime and its scripts in the file. Under `/opt/claude` it has `bin/claude` and `LICENSE.md`, and the recipe adds the managed settings, `/etc/claude-code/managed-settings.json`, which come before every other setting. Anthropic's setup page describes the signed manifest that `package_verify` checks: the key is published armored, `gpgv` reads it as a binary keyring, which is the armor's base64 body, and the fingerprint of the key that made the signature is the last field of `gpgv`'s `VALIDSIG` status line. The binary must then have the SHA-256 that the manifest lists under `platforms.linux-x64.checksum`.

What follows was measured with 2.1.296 on the testing host, Ubuntu under WSL 2, under `strace` and behind a proxy that logged each request, in a fresh home. Under WSL every command also runs Windows' `reg.exe` to read Anthropic's policy keys in the registry, which it does not do where there is no WSL.

* Libraries: it links against glibc 2.26 or later, `libc.so.6`, `libm.so.6`, `libpthread.so.0`, `libdl.so.2`, `librt.so.1` and `ld-linux-x86-64.so.2`, which the GNU root has. It reads the certificate authorities of `/etc/ssl/cert.pem` and `/etc/ssl/certs`, with those of the file `NODE_EXTRA_CA_CERTS` names.
* Processes: `--version`, `--help` and `auth status` start none, nor does `update` with the managed settings. A session runs `/bin/bash -c -l` once to save the login shell's environment, and each command of its Bash tool as `/bin/bash -c` with that saved environment; it runs `git` on the repository, its own file under the name `rg` for ripgrep, which it has built in, `uname`, and `gh auth token` and a `gh issue list` of the user's closed issues on `anthropics/claude-code` when the path finds `gh`. `claude doctor`, and `claude update` without the managed settings, run `npm -g config get prefix` where the path finds `npm`.
* Files: `--version` and `--help` write nothing. The other commands, `auth status` among them, write `~/.claude.json`, its settings and the state of each project, through a temporary file and a rename under the lock `~/.claude.json.lock`, and `~/.claude/`: `sessions/`, `backups/`, `cache/`, `projects/` with the transcripts, `plugins/` with Anthropic's catalog of plugins, and `.credentials.json`, mode 0600, after a sign-in. `CLAUDE_CONFIG_DIR` names another directory for `~/.claude`. A session also makes `claude-UID/` and `cc-socks/`, with a socket for each session, under `TMPDIR`, or `/tmp`.
* Hosts: the model's requests go to `api.anthropic.com`, or to `ANTHROPIC_BASE_URL`. A session with no sign-in and the settings as the vendor ships them also reached `api.anthropic.com` for its start-up settings, the list of MCP servers, feature flags and usage events; `http-intake.logs.us5.datadoghq.com` for its logs; `downloads.claude.ai` for its update, `/claude-code-releases/latest` and then the new version's `manifest.json` and binary, and for the catalog of plugins; `raw.githubusercontent.com` for the change log of `anthropics/claude-code`; and, at the first start, `platform.claude.com`. `claude auth login` gives an address at `claude.com/cai/oauth/authorize` to open in a browser. It takes a proxy from `HTTPS_PROXY`.
* Updates: as shipped, a session fetches `latest` from `downloads.claude.ai` at its start and installs a newer version into `~/.local/share/claude/versions/` for the next start, and `claude update` and `claude install` do the same on demand. With `DISABLE_UPDATES` set to 1 in the managed settings, as the recipe writes them, a session fetched no release, and `claude update` and `claude install` printed `Updates are disabled by your administrator` and exited with status 0 without fetching a release, at once with no network as well; `claude doctor` reports `Auto-updates: disabled (set by env: DISABLE_UPDATES)`. `DISABLE_AUTOUPDATER` alone stops only the background update.
* Telemetry: usage events go to `api.anthropic.com/api/event_logging/`, logs to Datadog, and feature flags come from `api.anthropic.com/api/eval/`; `DISABLE_TELEMETRY=1` stopped all three. `CLAUDE_CODE_DISABLE_NONESSENTIAL_TRAFFIC=1` stopped every request of the session but the download of the catalog of plugins, which `CLAUDE_CODE_DISABLE_OFFICIAL_MARKETPLACE_AUTOINSTALL=1` stopped too. The recipe sets neither; [claude(1)](../../../man/man1/claude.1) gives the settings to the person who uses it.
* Sandbox: the managed settings set `sandbox.enabled` to false, Claude Code's default, since its sandbox needs bubblewrap, which the GNU root does not have.

### muse

Muse Code is one static executable of 376 MB, which needs no library of the root. Meta's installer, `https://dev.meta.ai/install.sh`, puts a launcher script at `~/.local/bin/muse`. The launcher reads the channel manifest, `https://api.meta.ai/muse-code/channels/muse-stable`, for the current version and its release manifest, downloads the file that manifest lists for the platform, checks its size and SHA-256, keeps it beside itself as `muse-bin-VERSION` and runs it. Each time it starts it looks for a newer launcher and a newer version, at most once an hour, and installs them in the background; `MUSE_NO_AUTO_UPDATE=1` turns that off. The program has no update code of its own, so the layer installs it without the launcher, and it stays at the version the recipe pins.

Measured on Linux, `--version` and `--help` start no other process and write no file. Its screen asks whether to trust the directory it starts in, and writes the answer to `~/.config/muse/trust.json`. It makes `~/.local/share/muse`, with a SQLite index of its sessions, the sessions, its bundled skills and plugins and a log of each start, and makes and removes scratch directories under `TMPDIR` or `/tmp`. To learn whether its sandbox works it runs `/usr/bin/bwrap`, where there is one, and itself as `muse __tbh_internal_bwrap`, its own copy of bubblewrap; it also reads `/etc/ssh/ssh_config`. With no credential it offers two sign-ins: by browser, a device code that `auth.meta.com` confirms, for which it runs `xdg-open` on the address and also prints it, or by a Model API key typed in. A key in `META_API_KEY` signs it in without either and takes priority over a stored credential, which it keeps in `~/.config/muse/auth.json`. Its settings are `~/.config/muse/settings.json`, where `"telemetry": {"enabled": false}` turns off its telemetry. It talks to `api.meta.ai` and `auth.meta.com`, or to the server `--base-url` names, and trusts as well the certificate authority whose file the setting `endpoint_transport.ca_bundle` names.

## Building

The layer builds where the rest of the GNU layer does, on an x86-64 Linux host, with access to each vendor's download site. Besides the tools the [GNU layer](../Readme.md#building) lists it needs `sha512sum`, or `shasum`, for a package pinned by SHA-512, and `gpgv` and `base64` for Claude Code's signature.

## Updating

To update a package, take the new version's digest from the list its vendor publishes, verifying the list's signature where the vendor signs it, and set `VERSION` and the digest in the recipe. For Claude Code, fetch the new release's `manifest.json` and `manifest.json.sig`, verify the signature by [Anthropic's procedure](https://code.claude.com/docs/en/setup#verify-the-manifest-signature) with the key whose fingerprint is `FINGERPRINT`, and set the binary's digest from the manifest's `linux-x64` entry, the digests of the manifest and its signature, and the npm package's `dist.integrity` from `https://registry.npmjs.org/@anthropic-ai/claude-code/VERSION`, decoded from base64 to hex. A new key changes `FINGERPRINT` only after its fingerprint is checked against the setup page. Then update the package's row in the table above, its page in the manual and what its row in `tests/system/agents/` expects, and run `ARCH=x86_64 tests/run agents`.

A new package is a recipe in `packages/`, a row in `tests/system/agents/`, with a script for the fake server where the row talks to the vendor's service, a page in [`man/`](../../../man/Readme.md), a line in the table above and a section of Design that records what the program needs.

## Limits

* The packages are x86-64 programs, and the layer installs them only in the x86-64 GNU root.
* Claude Code's sandbox is off, and its shell commands run with the user's own rights, until the GNU root has bubblewrap and the kernel what bubblewrap needs.
* GitHub signs no file of `gh`'s release. Its build provenance attestations are checked by `gh attestation verify`, which needs a sign-in, so the recipe pins the digest the checksums file lists.
* Meta signs neither manifest of Muse Code's release, so the recipe pins the digest the release manifest lists. Meta's terms let the person who fetched the program install and use it, and forbid giving it to anyone else: a root built with `muse` is not to be given away.
* Muse Code's sandbox, its own copy of bubblewrap, needs parts of Linux that Rach2 does not have yet, among them `pivot_root` and `/proc/sys/kernel/overflowuid`; on Rach2 it is turned off with `--disable-sandbox` or `--yolo`.

## Tests

`fetch_agents.sh` runs each recipe's `package_check` on the build host when it stages the package. `ARCH=x86_64 tests/run agents` boots `os-real-gnu-agents.ext2` with 4 GiB of memory on a network whose only route out is to the fake API server on the host ([`tests/system/fakeapi/fakeapi.py`](../../../tests/system/fakeapi/fakeapi.py)), and runs the rows of [`tests/system/agents/`](../../../tests/system/agents) over SSH as the test account; [`tests/system/suites/agents.sh`](../../../tests/system/suites/agents.sh) describes a row and its script for the server. The `gh` row checks the pinned version, the program the path finds and its license, `gh auth status` with no account signed in, one HTTPS request from `gh api` with no account to openssl's test server on the machine, which `gh` refuses until `SSL_CERT_FILE` names the server's certificate, and `GH_TOKEN=invalid gh api user` against the fake server as `api.github.com`, which answers 401 as GitHub does. The `muse` row checks the pinned version, the program the path finds and the terms beside it, `muse --help`, and its screen in tmux, in a home of its own: the question whether to trust the directory, the answer recorded in `trust.json`, the two sign-ins it offers with no credential, and its end, not signed in, at Esc. The `claude` row checks the pinned version, the program the path finds and Anthropic's notice, the first line of `claude --help`, `claude update`, which the managed settings refuse without fetching a release, and the version after it, and `claude auth status` with no account signed in; it needs no network. The rows `board_test`, `limited_test`, `release_resume_test`, `tidy_test` and `worker_park_test` run the tests of the project's loops, `tests/system/NAME.sh --mutate`, in a git repository of the tree's tests and tools; [`tools/Readme.md`](../../../tools/Readme.md) says what those tests check. `sh tests/system/agents_mutate.sh` proves the `gh` and `muse` rows against a fault in each recipe, the `claude` row against managed settings that set `DISABLE_AUTOUPDATER` in place of `DISABLE_UPDATES`, and the `limited_test` row against a `limited` in `tools/flow/lib.sh` that takes a usage limit from any line.

The `muse` row then runs `muse exec --yolo`, and `muse-tmux` runs the same script in the interactive screen, in a fresh home and a clone of the shared fixture: read `notes.txt`, run `check.sh`, edit `greeting.txt` and commit. The server checks each tool result before the next turn. The host checks the returned file, committed tree, author, two commits and clean work tree. It also checks the binary's digest before and after each session, with an empty environment except the test settings. `sh tests/system/muse_session_mutate.sh` requires both modes to reject a commit with the wrong author and to pass before and after that fault.

The test sets `endpoint_transport.base_url`, `endpoint_transport.auth` to `"bearer"` and `endpoint_transport.ca_bundle` in the settings file; the interactive program requires the first two together before sending a key to a custom server. Headless mode makes six requests. Interactive mode fetches the model catalog twice and probes `/muse-code/config?channel=public`, then makes the same five model requests. The catalog replies use the public Model API's list of models. The config path has no published protocol, so the test answers it with an empty HTTP 404; the session continues without that service. The shared [Responses script](../../../tests/system/fakeapi/muse.json) supplies both modes; the interactive row's script filter adds only these startup exchanges.

Measured on the testing host in a 4096 MiB x86-64 guest, with TCG and two processors, on Muse Code's pinned version above. The digest check caches the binary's file before the session. Free frames are sampled from `/proc/vmstat` once a second; a frame is 4096 bytes. The fall is from the first sample to the smallest. These measurements set no pass criterion.

| Mode | Session (s) | Minimum sampled free frames | Fall from start (KiB) |
| --- | ---: | ---: | ---: |
| `exec` | 43 | 894906 | 117420 |
| `tmux` | 52 | 889516 | 233460 |

The host samples request arrival every 0.1 s. Each interval below starts at the request that selects the tool and ends at the request containing its result, including the HTTPS exchange and tool execution.

| Mode | Read (s) | First check (s) | Edit (s) | Check and commit (s) |
| --- | ---: | ---: | ---: | ---: |
| `exec` | 4.1 | 7.1 | 4.5 | 6.7 |
| `tmux` | 5.0 | 7.5 | 6.7 | 5.9 |
