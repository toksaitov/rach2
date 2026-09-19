# busybox-mailutils(1)

NAME: busybox-mailutils - the mail tools: the BusyBox applets of its `mailutils` directory as built for Rach2 (BusyBox 1.36.1, vendor/busybox/rach2.config), one family page with an alias for each applet.

The page: NAME lists every applet with .Nm; SYNOPSIS gives each applet's usage line as BusyBox states it; DESCRIPTION says in a sentence or two per applet what it does, adapted from BusyBox's usage text below (GPL-2.0; say so in a trailing paragraph), and what it does not do on this kernel where that differs from Linux. The aliases are already in man/aliases. Facts on this kernel: `sendmail`, `makemime`, `reformime`, `popmaildir`: mail tools with no mail system here.


The usage text BusyBox carries, per applet:


## makemime

```
"[OPTIONS] [FILE]..."
#define makemime_full_usage "\n\n"
      "Create multipart MIME-encoded message from FILEs\n"
```


## popmaildir

```
"[OPTIONS] MAILDIR [CONN_HELPER ARGS]"
#define popmaildir_full_usage "\n\n"
      "Fetch content of remote mailbox to local maildir\n"
```


## reformime

```
"[OPTIONS]"
#define reformime_full_usage "\n\n"
      "Parse MIME-encoded message on stdin\n"
    "\n	-x PREFIX	Extract content of MIME sections to files"
    "\n	-X PROG ARGS	Filter content of MIME sections through PROG"
    "\n			Must be the last option"
    "\n"
    "\nOther options are silently ignored"
```


## sendmail

```
"[-tv] [-f SENDER] [-amLOGIN 4<user_pass.txt | -auUSER -apPASS]"
    "\n		[-w SECS] [-H 'PROG ARGS' | -S HOST] [RECIPIENT_EMAIL]..."
#define sendmail_full_usage "\n\n"
      "Read email from stdin and send it\n"
    "\nStandard options:"
    "\n	-t		Read additional recipients from message body"
    "\n	-f SENDER	For use in MAIL FROM:<sender>. Can be empty string"
    "\n			Default: -auUSER, or username of current UID"
    "\n	-o OPTIONS	Various options. -oi implied, others are ignored"
    "\n	-i		-oi synonym, implied and ignored"
    "\n"
    "\nBusybox specific options:"
    "\n	-v		Verbose"
    "\n	-w SECS		Network timeout"
    "\n	-H 'PROG ARGS'	Run connection helper. Examples:"
    "\n		openssl s_client -quiet -tls1 -starttls smtp -connect smtp.gmail.com:25"
    "\n		openssl s_client -quiet -tls1 -connect smtp.gmail.com:465"
    "\n			$SMTP_ANTISPAM_DELAY: seconds to wait after helper connect"
    "\n	-S HOST[:PORT]	Server (default $SMTPHOST or 127.0.0.1)"
    "\n	-amLOGIN	Log in using AUTH LOGIN"
    "\n	-amPLAIN	or AUTH PLAIN"
    "\n			(-amCRAM-MD5 not supported)"
    "\n	-auUSER		Username for AUTH"
    "\n	-apPASS 	Password for AUTH"
    "\n"
    "\nIf no -a options are given, authentication is not done."
    "\nIf -amLOGIN is given but no -au/-ap, user/password is read from fd #4."
    "\nOther options are silently ignored; -oi is implied."
IF_MAKEMIME(
    "\nUse makemime to create emails with attachments."
)
```
