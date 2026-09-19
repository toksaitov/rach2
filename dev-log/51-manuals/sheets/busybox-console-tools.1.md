# busybox-console-tools(1)

NAME: busybox-console-tools - the console tools: the BusyBox applets of its `console-tools` directory as built for Rach2 (BusyBox 1.36.1, vendor/busybox/rach2.config), one family page with an alias for each applet.

The page: NAME lists every applet with .Nm; SYNOPSIS gives each applet's usage line as BusyBox states it; DESCRIPTION says in a sentence or two per applet what it does, adapted from BusyBox's usage text below (GPL-2.0; say so in a trailing paragraph), and what it does not do on this kernel where that differs from Linux. The aliases are already in man/aliases. Facts on this kernel: `clear`, `reset`, `resize`, `setlogcons` on the console: what the kernel console does with the escape sequences they emit (the VGA text driver takes the sequences `clear` writes; say what is honoured). Found at the writing: `resize` cannot resize the VGA screen, since the console does not answer the cursor position query (rach2/sys/kio.c); `setlogcons` needs TIOCLINUX, which this kernel does not have.


The usage text BusyBox carries, per applet:


## clear

```
""
#define clear_full_usage "\n\n"
      "Clear screen"
```


## reset

```
""
#define reset_full_usage "\n\n"
      "Reset terminal (ESC codes) and termios (signals, buffering, echo)"
```


## resize

```
""
#define resize_full_usage "\n\n"
      "Resize the screen"
```


## setlogcons

```
"[N]"
#define setlogcons_full_usage "\n\n"
      "Pin kernel output to VT console N. Default:0 (do not pin)"
```
