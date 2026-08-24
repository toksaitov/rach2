# What a clone omits

`git clone` produces a buildable tree, but deliberately omits secrets, game data that cannot be redistributed, and machine-specific credentials. This page lists each omission, the default behavior, and the reason to transfer it.

## The credentials

`krffs/.gitignore` excludes eleven credential files in three groups.

When all eleven files are absent, the first `make` creates a working set. A PARTIAL set requires care because each generator stops when it finds a sentinel instead of filling gaps. `gen_demo_key.sh` does nothing once the aggregate `authorized_keys` exists, `gen_demo_password.sh` does nothing once `shadow` exists, and neither generator creates the two optional `.extra` files. Transfer every file in a group or none of them. A build does not repair an incomplete group.

`krffs/gen_demo_key.sh` creates the root SSH files:

* `krffs/rach2_demo_ed25519` and `.pub`, the key pair used by `ssh -i` to log in as root.
* `krffs/authorized_keys`, staged into the image as root's `~/.ssh/authorized_keys`.
* `krffs/authorized_keys.extra`, an optional hand-written file whose public keys are staged beside the generated key, allowing a workstation key to be added.

The test account has the corresponding SSH files:

* `krffs/rach2_test_ed25519` and `.pub`, and `krffs/test_authorized_keys`.
* `krffs/test_authorized_keys.extra`, the same optional hand-written addition as the root file.

`krffs/gen_demo_password.sh` creates the password files:

* `krffs/rach2_demo_password` and `krffs/rach2_test_password`, retained in plaintext so a person can read them.
* `krffs/shadow`, staged as the image's `/etc/shadow`, containing their SHA-512 crypt hashes.

Without a transfer, the next build creates a new set. The new machine then has independent credentials, and the old credentials cannot access it. Transfer the files outside Git only when existing keys must continue to reach an image or running installation. Credentials for a sealed machine were created once for that machine and exist nowhere else; losing them requires reflashing it.

## The machine's own identity

The machine's identity is not a repository file. The SSH server creates and retains a host key on its first boot. The fingerprint printed during that boot is the only authentication available to the first client. It must be read from the machine's screen and written in the session record. A netconsole capture contains the same line, but its unauthenticated broadcast provides no proof. A clone cannot include a key that does not exist until the machine boots.

Recovery uses a RAM image as its root, so every recovery boot creates a new host key and causes `ssh` to report a changed identity. `dev-log/25-sealed/checklist.md` documents the expected response.

## The game data

Git ignores `ports/doom/doom1.wad`, about 4 MB, because it is game content. Save either the id Software shareware `doom1.wad` (sha256 `1d7d43be501e67d927e415e0b8f3e29c3bf33075e859721816f652a526cac771`) or `freedoom1.wad` from https://freedoom.github.io under that name, then run `./ports/doom/fetch_wad.sh` to check it. The image builds without the optional WAD; `doom` then starts and exits with "IWAD not found".

## The build tree

Git ignores the regenerable `build/` directory, which occupies hundreds of megabytes. Prefer a clone to a copied working tree. When copying is necessary, exclude `build/` and first run `make distclean` or delete the vendor stamps (`vendor/*/.stamp`). A stamp without its corresponding output causes `make full` to skip that capstone, leaving a missing binary that is reported only later.

## GitHub

Remote authentication belongs to the machine. Before it can push, a new machine needs its own credential, either an SSH key registered with the account or `gh auth login`. The tree neither includes nor should include that credential.
