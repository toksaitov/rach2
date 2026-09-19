# busybox-loginutils(8)

NAME: busybox-loginutils - the account tools: the BusyBox applets of its `loginutils` directory as built for Rach2 (BusyBox 1.36.1, vendor/busybox/rach2.config), one family page with an alias for each applet.

The page: NAME lists every applet with .Nm; SYNOPSIS gives each applet's usage line as BusyBox states it; DESCRIPTION says in a sentence or two per applet what it does, adapted from BusyBox's usage text below (GPL-2.0; say so in a trailing paragraph), and what it does not do on this kernel where that differs from Linux. The aliases are already in man/aliases. Facts on this kernel: On os-real `login`, `su` and `passwd` are set-id BusyBox applets from the second build; `adduser`, `addgroup`, `deluser`, `delgroup`, `chpasswd` are the account tools (CLAUDE.md's standing decision: the account tools are BusyBox's). The credential files are passwd(5). No PAM, no `login.defs`.


The usage text BusyBox carries, per applet:


## add-shell

```
"SHELL..."
#define add_shell_full_usage "\n\n"
      "Add SHELLs to /etc/shells"
```


## addgroup

```
"[-g GID] [-S] " IF_FEATURE_ADDUSER_TO_GROUP("[USER] ") "GROUP"
#define addgroup_full_usage "\n\n"
      "Add a group" IF_FEATURE_ADDUSER_TO_GROUP(" or add a user to a group") "\n"
    "\n	-g GID	Group id"
    "\n	-S	Create a system group"
```


## adduser

```
"[OPTIONS] USER [GROUP]"
#define adduser_full_usage "\n\n"
      "Create new user, or add USER to GROUP\n"
    "\n	-h DIR		Home directory"
    "\n	-g GECOS	GECOS field"
    "\n	-s SHELL	Login shell"
    "\n	-G GRP		Group"
    "\n	-S		Create a system user"
    "\n	-D		Don't assign a password"
    "\n	-H		Don't create home directory"
    "\n	-u UID		User id"
    "\n	-k SKEL		Skeleton directory (/etc/skel)"
```


## chpasswd

```
"[-me] [-c ALG] [-R DIR]"
#define chpasswd_full_usage "\n\n"
      "Read user:password from stdin and update /etc/passwd\n"
    "\n	-e	Supplied passwords are in encrypted form"
    "\n	-m	Encrypt using md5, not des"
    "\n	-c ALG	"CRYPT_METHODS_HELP_STR
    "\n	-R DIR	Directory to chroot into"
```


## cryptpw

```
"[-P FD] [-m TYPE] [-S SALT] [PASSWORD] [SALT]"
```


## delgroup

```
IF_FEATURE_DEL_USER_FROM_GROUP("[USER] ")"GROUP"
#define delgroup_full_usage "\n\n"
      "Delete group GROUP from the system"
IF_FEATURE_DEL_USER_FROM_GROUP(" or user USER from group GROUP")
```


## deluser

```
IF_LONG_OPTS("[--remove-home] ") "USER"
#define deluser_full_usage "\n\n"
      "Delete USER from the system"
```


## mkpasswd

```
(no usage text; state the applet by its BusyBox documentation)
```


## remove-shell

```
"SHELL..."
#define remove_shell_full_usage "\n\n"
      "Remove SHELLs from /etc/shells"
```
