# Terminals

`sys/tty/` is the kernel's terminal layer: the N_TTY line discipline, which edits, echoes and recognizes signal characters in what is typed; the console terminal, which the keyboard and COM1 feed and the screen and COM1 show; and the pseudo-terminal pairs a network login gives a remote user. This document also follows a keystroke, a mouse movement and a serial byte from the hardware to a reader, through `sys/input.c`, `sys/kbd.c`, `sys/mouse.c` and `sys/arch/x86/serial.c`. The screen the console draws on is described in [the display Readme](../display/Readme.md).

## Interfaces

Programs see the console, `/dev/tty`, `/dev/ptmx` and `/dev/pts/N` as [console(4)](../../../man/man4/console.4) describes them, with the terminal requests of [ioctl(2)](../../../man/man2/ioctl.2) and the reads of [read(2)](../../../man/man2/read.2). The mouse is the path `/mouse` ([open(2)](../../../man/man2/open.2)), and the counters of the console's receive path are `/proc/console` ([proc(5)](../../../man/man5/proc.5)).

Inside the kernel, `tty.h` is the terminal. `tty_input_char` gives a terminal one input byte from any source, with interrupts off or from an interrupt handler; `tty_read`, `tty_write`, `tty_poll`, `tty_inq`, `tty_set_termios`, `tty_flush_input` and `tty_hangup` serve the descriptors; `tty_set_session` and `tty_make_sole_ctty` (in `pty.c`) set a terminal's session and foreground group. `input.h` takes a set-1 scancode (`input_key`) or a mouse report (`input_mouse`) from any driver. The debug shell in `rach2/kernel.c` defines the three functions `input.c` calls: `shell_reads_input`, `shell_input_char` and `shell_interrupt`.

## Design

### The line discipline

`n_tty.c` is pure logic over a `struct n_tty` and makes no kernel calls, so the kernel and a host test compile the same file. Echo and output leave through a sink function the caller supplies: the console's sink writes the screen and COM1, and a pseudo-terminal slave's writes its master's ring.

The state is the termios (`struct ktermios`, Linux's 36-byte kernel layout with 19 control characters, the same on both architectures), the line under edit (`N_TTY_LINE_MAX`, 256 bytes), and the read queue, a ring of `N_TTY_READ_MAX` (4096) bytes beside an array that marks the last byte of each committed line.

In canonical mode a byte edits the line. ERASE, WERASE and KILL remove bytes, and with ECHOE each removed byte is rubbed out on the screen as `\b \b`. An ordinary byte joins the line while the line has room for it and a terminator, so a line has at most 255 bytes before its newline; a byte past that is dropped and counted. A newline or VEOL commits the line with its terminator. VEOF commits a nonempty line without one, and on an empty line records end of file at the queue's present position. A canonical read returns at most one line, stopping at a newline or at a marked byte, so a line committed by VEOF keeps its boundary, and a later change of VEOL leaves the queued boundaries as they were. A read that reaches the end-of-file position returns 0, once.

In raw mode each byte is readable at once, and a read waits for VMIN bytes. When a program turns ICANON off, a partial line joins the queue behind the lines committed before it, so it is neither lost nor reordered; if the queue has no room for it, it stays in the line buffer and a raw read takes it after the queue.

With ISIG set, VINTR, VQUIT and VSUSP are echoed, discard all unread input, as Linux does without NOFLSH, and are returned to the caller, which posts SIGINT, SIGQUIT or SIGTSTP to the foreground process group. A control character of 0 is disabled. The input flags ISTRIP, IGNCR, ICRNL and INLCR and the output flags OPOST, ONLCR and OCRNL are the ones implemented.

`n_tty_has_room` is true while the discipline can take a whole line more: the queue and the line together use no more than 4096 less 256 bytes. The producers that can wait, the serial pumps and a master's write, feed the discipline only while it is true, so a burst waits upstream and no byte of it is dropped.

### The terminal object

`tty.c` puts the discipline behind a wait queue. `struct tty` has the discipline, `input_wq` for readers and pollers, `space_wq` for a producer waiting for room, the window size, the sink, an optional `pump` that drains a polled input source, and the session and foreground group (`sid` and `fg_pgid`, 0 for none).

`tty_input_char` runs one byte through the discipline. It wakes `space_wq` with `WQ_ROOM` when the byte took bytes out of the discipline (an erase, or a signal character's flush), posts a recognized signal to every live user process of the foreground group, and wakes `input_wq` with `WQ_DATA` when a read would no longer block.

`tty_read` waits on `input_wq` with `tty_ready_cond` as the predicate, which runs the pump before it tests the discipline; the wait calls it with interrupts off, so no interrupt comes between a drain and the test after it. `tty_poll` and `tty_inq`, behind poll(2) and FIONREAD, also run the pump with interrupts off, and so does the path that records the USB latency stamps. After a wake the test and the copy run in one interrupts-off step, and a reader whose line another reader took waits again, since a read of the empty queue would return 0, end of file. One read copies at most `N_TTY_READ_CHUNK`, 1024 bytes.

`tty_write` passes each byte through output processing to the sink. On the console it runs between `console_write_begin` and `console_write_end`, so a kernel log record raised during the write waits for its end, and it then gives the screen's answers to the queries in the output (`k_take_answer` in `sys/kio.c`) to the console's input, where the program reads them as typed bytes.

`tty_hangup` records end of file after the queued input and wakes every reader. A canonical read consumes that mark once, so an endpoint that hangs up keeps its own state for the reads after it, as the pseudo-terminal slave does with `master_open`.

`tty_set_session` writes `sid` and `fg_pgid` with interrupts off, because the console's interrupt-side drain reads both. `tty_make_sole_ctty` makes a terminal the only controlling terminal of a session, dropping the console and every slave the session had, in one interrupts-off step. A session leader claims the console, when no session owns it, by reading it, and an unowned slave by reading it or by opening it without O_NOCTTY while its session has no controlling terminal; TIOCSCTTY claims either. When a session leader exits, `exit_release_session` in `sys/syscall_proc.c` releases the console and the slaves its session controls. Background reads and writes are as console(4) says.

### The console terminal

`tty_console_init` sets up `console_tty` at boot. Its sink, `console_sink`, writes each byte to the screen through `k_putc` and to COM1 through `serial_out`; its pump, `console_pump`, drains COM1's receive ring; its window size is the screen's cells (`k_console_size`). The keyboard and COM1 feed it.

### Keystrokes

Two drivers produce keystrokes, and both give `input_key` set-1 scancodes, the code the 8042 delivers at port 0x60. The PS/2 keyboard's handler (`sys/arch/x86/ps2kbd.c`, IRQ 1, vector 33) reads the port when the status register says a byte is waiting. A USB boot keyboard's reports become set-1 make and break codes in `sys/usb/hid.c`, as [the USB Readme](../usb/Readme.md) describes. Either way `input_key` runs in interrupt context, and nothing after it depends on which keyboard sent the code.

`input_key` first ends a suspend to idle (`suspend_wake`) and puts the code in the raw scancode ring of `sys/kbd.c`, 64 slots, which takes every code, releases and modifiers included, and drops a code that finds it full. The process that owns `/fb` reads that ring through the console ([read(2)](../../../man/man2/read.2)). `input_key` then keeps one modifier state for every keyboard: Shift (0x2A and 0x36, ignored after an 0xE0 prefix, where the keyboard sends it around a cursor key), Ctrl (0x1D) and Alt (0x38), each for either side. A release does nothing more. A press is tested in this order:

* Ctrl+C, when no reader is parked on the console and no session owns it, is `shell_interrupt`, the debug shell's own interrupt (`kill_foreground_process` in `rach2/kernel.c`).
* While the debug shell reads input (`shell_reads_input`), the extended Page Up, Page Down, Home and End scroll the screen.
* With a reader parked on the console or a session owning it, the key goes to the discipline through `session_key`. Shift with Page Up or Page Down scrolls the screen back, and any other key first returns it to the live screen. The arrows, Home and End become xterm's escape sequences and Insert, Delete, Page Up and Page Down VT220's (`ESC [ 2 ~` and so on), as the `rach2` terminfo entry lists them; Ctrl with a letter becomes its control byte; Backspace becomes DEL, the erase character; Alt puts ESC before the key's byte. Ctrl+C here is VINTR like any other byte, and a key nobody reads waits in the discipline as type-ahead.
* Otherwise a character goes to the debug shell's command buffer (`shell_input_char`) while the shell reads, and is dropped when nothing reads.

The keymap is US English (`EN_us` and `EN_us_shift` in `kbd.c`).

### Mice

The PS/2 mouse (`sys/arch/x86/ps2mouse.c`, IRQ 12, vector 44) assembles three-byte packets, finding the first byte by its bit 3, always set, and drops a packet with an overflow bit set; `rach2.nomouse` on the kernel command line skips it. A USB boot mouse's reports come from `hid.c`. Both put packets in the one queue of `sys/mouse.c`: 64 slots, each a button byte (left, right and middle in bits 0 to 2) and two signed byte movements clamped to -128 through 127. A packet that finds the queue full is dropped, and nothing in the queue says which mouse moved. `/mouse` fails with ENODEV until a mouse of either kind has been found (`input_mouse_found`); an open empties the queue and restarts the PS/2 packet framing.

### COM1

COM1 (`sys/arch/x86/serial.c`, port 0x3F8, IRQ 4, vector 36) runs at 115200 baud, 8N1, with its FIFO on. `serial_init` finds it by its scratch register, and on a machine without one every serial call does nothing. Received bytes go to a ring of 4096 bytes by two services: the port's interrupt takes at most 64 bytes a time, and `tty_console_poll`, which the timer tick calls every 10 ms (`TICK_HZ` is 100) and the interrupt calls when its service leaves bytes in the ring, takes at most 64 more, so a port whose interrupt is not enabled is still read. A full ring stops the service and masks the receive interrupt until a take leaves the ring less than half full. The bytes stay in the UART, whose FIFO then fills, so QEMU's sender waits and a real sender overruns, which the line status reports.

After the service, what `tty_console_poll` does depends on who reads the console:

* A reader or poller parked on the console is woken and drains the ring itself, in `console_pump`, in its own context with interrupts off, until the ring is empty or the discipline has no room for a line. It echoes what it takes when the ring had at most 64 bytes as it began, and otherwise takes them without echo, since each echoed byte waits on the transmitter, about 87 microseconds at 115200 baud, with interrupts off.
* With nobody parked and a session owning the console, as when a shell waits for its foreground job, `console_pump_irq` moves at most 64 bytes into the discipline without echo, so an interrupt character reaches the foreground group and the rest waits as type-ahead.
* With no session, the bytes stay in the ring for the debug shell, which polls it (`serial_try_getc`) and releases the console whenever it regains control.

Both pumps stop at a full discipline and leave the rest in the ring. A signal character still gets through: it would discard the input before it anyway, so `console_signal_through` discards the bytes ahead of it in the ring and feeds it. A flush of the console's input, by TCSETSF or TCFLSH, empties the ring and the UART's receive FIFO as well.

`/proc/console` has the counters of this path, in the order a byte meets them, which proc(5) lists. `uart-overruns` counts the overruns any read of the line status saw; `tick-drains` counts the runs of `console_pump_irq`, from the tick and from the interrupt, and `reader-drains` those of `console_pump`, from a read, a poll or FIONREAD; `shell-bytes` counts the bytes the debug shell and the boot diagnostic took; `flush-bytes` counts what signal characters, TCSETSF and TCFLSH discarded from the discipline, and the bytes discarded ahead of a signal character.

### Pseudo-terminals

Opening `/dev/ptmx` allocates a `struct pty` from the heap at the lowest index no live pair has, locked until TIOCSPTLCK clears the lock (`pty.c`). The pair is freed when the master and every slave description have closed. The slave is a full `struct tty` with no pump, and its sink, `pty_slave_sink`, writes the master's ring, `m_buf`, of `PTY_BUF_SIZE` (4096) bytes.

* A master write feeds the slave's discipline a byte at a time through `tty_input_char` while the discipline has room for a line. A write that fills it returns short; one that has fed nothing waits, or fails with EAGAIN when nonblocking. A signal character among the bytes that wait is fed at once, and the bytes before it are discarded and counted, as on the console.
* A master read drains `m_buf`, and in packet mode (TIOCPKT) puts the status byte 0, TIOCPKT_DATA, before the bytes. It returns 0 once every slave description has closed and the ring is empty.
* A slave write is output processing into `m_buf`. The writer marks the pair busy for the whole write (`writing` in `struct pty`), so another writer waits until it ends, or fails with EAGAIN when nonblocking, as Linux's `atomic_write_lock` keeps two writes apart. It copies a piece at a time, reserving eight bytes of ring for each byte while OPOST is set and one otherwise (`pts_growth`), and while the ring has no room it waits on the slave's queue, which a master read wakes, so no output is lost. Echo does not wait, and is dropped when the ring is full.
* TIOCSWINSZ on the master posts SIGWINCH to the slave's foreground group.

The master has one wait queue, `m_read_wq`, for its readers, writers and pollers, and the slave's `space_wq` points at it. Each wake gives its kind of change, which an edge-triggered epoll registration reports. A slave read, an input flush, a termios change, and master input that took bytes out of the slave's discipline (an erase, or a signal character's flush) wake it with `WQ_ROOM`. Slave output and echo wake it with `WQ_DATA` when they put bytes in `m_buf`, and with no kind when they put none, as input written with echo off, or echo the full ring dropped. These wakes are made even when a later user copy of the same write fails. Input written to the master wakes the slave's `input_wq` with `WQ_DATA`.

Closing the master posts SIGHUP to the slave's foreground group and hangs the slave up. Slave readers take what is queued, and then every read returns 0, nonblocking ones included: `pts_read` tests `master_open` when the discipline has nothing, and its wait predicate tests it beside the discipline's readiness. A slave write after the close fails with EIO, and poll reports POLLHUP. When the last slave description closes, master readers are woken to read end of file, and the master's poll reports POLLHUP.

## Limits

* VTIME is stored and ignored: a raw read with VMIN above 0 waits for VMIN bytes with no timeout.
* The keyboard feeds the console's discipline without testing for room, so a key typed into a full queue or a full canonical line is dropped and counted (`queue-drops`, `line-drops`).
* One modifier state serves every keyboard: two keyboards share Shift, Ctrl and Alt, and a release on either releases the modifier for both.
* The keymap is US English. Caps Lock, Num Lock, the function keys and the keypad's digit keys produce no byte.
* While a program owns `/fb`, each key also reaches the console's discipline when a session owns the console, and waits there as type-ahead for the next reader.
* The two mice give opposite signs of vertical movement. `hid.c` turns a USB report's Y, positive downward, into positive upward; `ps2mouse.c` turns the PS/2 wire's Y, positive upward, into positive downward.
* TIOCSWINSZ on the console or a slave stores the size and posts no SIGWINCH; only the master's posts it.
* Packet mode sends only TIOCPKT_DATA, never the status bytes for a flush or a stop of output.

## Tests

The host tests run with `tests/run host`:

* `tests/host/n_tty_hosttest.c`: the discipline's editing, its commit rules, VMIN, ISIG and output mapping.
* `tests/host/tty_hosttest.c`: `tty.c` and `pty.c` under a model scheduler that runs a producer only after the consumer is blocked: each wait and wake of a pair with its kind, packet mode, and TCFLSH on the master.
* `tests/host/hid_hosttest.c`: USB keyboard reports turned into the scancodes `input_key` takes.

The suites, each run with `tests/run NAME`:

* `console`: stdintest's line reads, and ttyprobe's cooked line with erases, its raw byte, the screen's answers to its queries, and Alt with x read as ESC and x.
* `conload`: 360 numbered, checksummed lines typed at COM1 while the machine is loaded, with the reader parked, as type-ahead, after an interrupt character to a foreground child, with that character behind 200 lines nobody read, and across an exec; every line must arrive.
* `default`: among its probes, ptyprobe, a pair driven as a network login drives it, and jobprobe, sessions, groups and job control.
* `ssh`: dropbear's logins on a pseudo-terminal, with its window size and `stty`.
* `usb`: keystrokes and mouse movement from USB devices on machines with no 8042, read through the debug shell's buffer, the terminal and the mouse queue.
