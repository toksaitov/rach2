# The agents layer

This directory adds to the x86-64 [GNU system](../Readme.md) the programs a person uses to work with a coding agent over SSH: each vendor's published build, fetched for the person who builds the image, checked against the digest its vendor publishes, and installed unmodified under its own license. Nothing of a package is committed but its recipe. The layer has one package so far, GitHub's `gh`. A package is installed only when the builder names it.

## Use

`GNU_AGENTS` lists the packages the GNU root gets, by the names of their recipes in [`packages/`](packages), or is `all` for every package; it is empty by default. The packages are x86-64 programs, so the variable needs `ARCH=x86_64`:

    make ARCH=x86_64 GNU_AGENTS=gh os-real-gnu-ext2
    make ARCH=x86_64 GNU_AGENTS=all machine

`make ARCH=x86_64 GNU_AGENTS=gh gnu-agents` fetches and stages the packages without building the root. A change to the list, to a recipe or to `fetch_agents.sh` builds the root again; [`mk/gnu-agents.mk`](../../../mk/gnu-agents.mk) has the rules. The release to the installed machine builds with `GNU_AGENTS=all` (`tools/flow/release.sh`), and so does the root `tests/run agents` boots, `os-real-gnu-agents.ext2`.

On the machine each package is a directory of `/opt`, as its vendor published it, with a link to its program in `/usr/bin` and `/bin`. Its page in the manual says how to sign in over SSH and where the credentials are kept: [gh(1)](../../../man/man1/gh.1).

## Sources and license

| Package | Version | License | From | Pinned by |
| --- | --- | --- | --- | --- |
| [`gh`](packages/gh.sh) | 2.102.0 | MIT | `gh_2.102.0_linux_amd64.tar.gz` of GitHub's release `v2.102.0` of `cli/cli` | the SHA-256 the release's `gh_2.102.0_checksums.txt` lists |

A package keeps its vendor's license, which the recipe names in `LICENSE` and installs beside the program; `NOTICES` lists those files, and the stage fails without them. The recipes and `fetch_agents.sh` are in-house.

## Design

### Recipes

A recipe, `packages/NAME.sh`, is a shell fragment that [`fetch_agents.sh`](fetch_agents.sh) reads in a shell of its own. Its header comment says what the program is, where it comes from and how it is pinned. It sets:

* `VERSION`, the version the vendor published.
* `LICENSE`, the license's short name.
* `FILES`, one line for each download: the name it is kept under, its URL, and its digest as the vendor publishes it, `sha256:HEX` or `sha512:HEX`.
* `NOTICES`, the license and notice files, as paths in the stage.

It defines `package_stage DIR`, which lays the package out under `DIR` as the root has it, taking each download through `fetched NAME`. It may define `package_verify`, which checks the vendor's signature on a download, and `package_check DIR`, which runs the staged program on the build host with a home of its own, `SCRATCH`.

### Fetching and staging

`fetch_agents.sh NAME...` downloads each file of `FILES` once into `../downloads/agents/NAME/`, which Git ignores, and checks it against its digest each time `fetched` names it, so a download is checked again at every use; a file with another digest is removed and refused. It then runs `package_verify`, stages the package from nothing into `agents/NAME/` of the layer's build tree, `build/x86_64/vendor/gnu/agents/NAME/`, checks that each of `NOTICES` is there, and runs `package_check`. Make runs it behind a stamp for each package, `agents/NAME.stamp`.

### Installing

[`install_root.sh`](../install_root.sh) copies each stage that `GNU_AGENTS` names into the root after the Chromium packages, and refuses a file the root already has, as it does for every part of the layer. Its later steps give each program of `/usr/bin` its name in `/bin`.

### gh

`gh` is a static Go executable and needs no library of the root. Under `/opt/gh` it has `bin/gh`, its manual pages in `share/man/man1` and `LICENSE`. Measured on Linux, it starts no other process for `--version`, `auth status` or `api`. It writes its settings to `~/.config/gh`, and a device identifier and the state of its update check to `~/.local/state/gh`. Before a request it asks the Secret Service of the D-Bus session bus for a stored token, at the address `DBUS_SESSION_BUS_ADDRESS` gives or at `/run/user/UID/bus`, and reads the token from `~/.config/gh/hosts.yml` where there is none. It talks to `api.github.com` and `github.com`, or to the host `GH_HOST` names, whose API it reaches at `https://HOST/api/v3/`, and trusts the certificate authorities of `/etc/ssl/certs`, with those of the file `SSL_CERT_FILE` names. It does not update itself: on a terminal it checks GitHub for a newer release once a day and prints a notice, which `GH_NO_UPDATE_NOTIFIER` turns off. Its telemetry, on by default, is turned off by `gh config set telemetry disabled` or `GH_TELEMETRY=false`.

## Building

The layer builds where the rest of the GNU layer does, on an x86-64 Linux host, with access to each vendor's download site. Besides the tools the [GNU layer](../Readme.md#building) lists it needs `sha512sum`, or `shasum`, for a package pinned by SHA-512.

## Updating

To update a package, take the new version's digest from the list its vendor publishes, verifying the list's signature where the vendor signs it, and set `VERSION` and the digest in the recipe. Then update the package's row in the table above, its page in the manual and what its row in `tests/system/agents/` expects, and run `ARCH=x86_64 tests/run agents`.

A new package is a recipe in `packages/`, a row in `tests/system/agents/`, a page in [`man/`](../../../man/Readme.md), a line in the table above and a section of Design that records what the program needs.

## Limits

* The packages are x86-64 programs, and the layer installs them only in the x86-64 GNU root.
* GitHub signs no file of `gh`'s release. Its build provenance attestations are checked by `gh attestation verify`, which needs a sign-in, so the recipe pins the digest the checksums file lists.

## Tests

`fetch_agents.sh` runs each recipe's `package_check` on the build host when it stages the package. `ARCH=x86_64 tests/run agents` boots `os-real-gnu-agents.ext2` with 4 GiB of memory and no route out of the emulator's network, and runs the rows of [`tests/system/agents/`](../../../tests/system/agents) over SSH as the test account; [`tests/system/suites/agents.sh`](../../../tests/system/suites/agents.sh) describes a row. The `gh` row checks the pinned version, the program the path finds and its license, `gh auth status` with no account signed in, and one HTTPS request from `gh api` with no account to openssl's test server on the machine, which `gh` refuses until `SSL_CERT_FILE` names the server's certificate.
