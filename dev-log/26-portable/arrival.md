# What a clone does not carry

`git clone` gives a tree that builds. It does not give a tree that is identical to the one it came from, and the difference is deliberate in every case: these files are either secrets, or game data nobody may redistribute, or credentials that would be worth less if they travelled. This page says what is missing, what happens if you do nothing, and what carrying it would buy.

## The credentials

Ten files, git-ignored by `krffs/.gitignore`, in three groups. `make` mints whatever is absent, so a clone that builds an image gets working credentials of its own on the first run.

The root SSH key, minted by `krffs/gen_demo_key.sh`:

* `krffs/rach2_demo_ed25519` and `.pub`, the keypair `ssh -i` uses to log in as root.
* `krffs/authorized_keys`, staged into the image as root's `~/.ssh/authorized_keys`.
* `krffs/authorized_keys.extra`, optional and hand-made: any public key put here is staged alongside the generated one, which is how a person's own workstation key gets in.

The test account's SSH key, the same shape:

* `krffs/rach2_test_ed25519` and `.pub`, and `krffs/test_authorized_keys`.

The passwords, minted by `krffs/gen_demo_password.sh`:

* `krffs/rach2_demo_password` and `krffs/rach2_test_password`, the plaintext, kept so a person can read them.
* `krffs/shadow`, staged as the image's `/etc/shadow`, holding their SHA-512 crypt hashes.

Do nothing and the next build mints a new set, which is usually what you want: the new machine gets its own credentials and the old ones open nothing on it. Carry them, out of band, only to keep an existing image or a running installation reachable with the keys you already have. A sealed machine is the case that matters: its credentials were minted once for that machine and exist nowhere else, so losing them means reflashing it.

## The machine's own identity

Not a file here at all. The SSH server mints a host key on its first boot and keeps it, and the fingerprint it prints on that boot is the only thing that authenticates the machine to its first client. It is read off the machine's own screen and written into the session record; the netconsole capture shows the same line but is unauthenticated broadcast, so it proves nothing. A clone cannot carry this because it does not exist until the machine boots.

Recovery is different: its root is a RAM image, so it mints a new host key on every recovery boot and `ssh` will say the identity changed. That is expected, and `dev-log/25-sealed/checklist.md` says what to do about it.

## The game data

`ports/doom/doom1.wad`, about 4 MB, git-ignored because it is game content. Put either the id Software shareware `doom1.wad` (sha256 `1d7d43be501e67d927e415e0b8f3e29c3bf33075e859721816f652a526cac771`) or `freedoom1.wad` from https://freedoom.github.io saved under that name, and run `./ports/doom/fetch_wad.sh` to check it is in place. Without it the image still builds: the WAD is staged as an optional file, and `doom` starts and exits with "IWAD not found".

## The build tree

`build/` is git-ignored and regenerable, and it is hundreds of megabytes. Clone rather than copy. If you must copy a working tree, exclude `build/` and either run `make distclean` or delete the vendored stamps (`vendor/*/.stamp`) first: a stamp left behind without the output it stands for makes `make full` skip rebuilding that capstone, and the missing binary shows up much later as a puzzle.

## GitHub

The remote's authentication is the machine's, not the repository's. A new machine needs its own credential (an SSH key on the account, or `gh auth login`) before it can push. Nothing in the tree carries it and nothing should.
