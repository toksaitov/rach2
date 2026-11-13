#!/usr/bin/perl
#
# hold.pl: run a job as this process's child in a run directory of its own,
# and be the only process that signals it.
#
# Usage: perl hold.pl run [-g] [-P] [-k SECONDS] DIR COMMAND...
#        perl hold.pl run [-P] [-k SECONDS] DIR -p PID
#        perl hold.pl state DIR
#        perl hold.pl stop DIR SECONDS
#
# DIR is the run's directory, which the caller makes once, by a mkdir that
# fails when it exists, and never makes again; its name is the run's, unique,
# and every file in it is only ever created:
#
# * DIR/live, a lock that the holder takes exclusively before anything else
#   and keeps for its whole life. It is created under another name, locked,
#   and then linked into place, so a DIR/live that exists has been locked. Its
#   descriptor is closed on exec, so no job inherits it.
# * DIR/stop, the tombstone, empty: a stopper creates it, and the holder reads
#   it before the job starts and on every turn after.
# * DIR/end, the job's status as sh reports one, written to a file of its own
#   and linked into place, which fails when DIR/end exists, so it is written
#   once and by this holder only.
# * DIR/go, the gate of -p, a FIFO that the caller makes.
#
# run: the holder. With COMMAND it forks COMMAND, which waits on a pipe before
# its exec: the holder reads DIR/stop first, and on a stop closes the pipe,
# and the child exits 143 without running anything; otherwise it lets the
# child go and waits on a second pipe, closed on exec, until the exec has
# happened or failed. No signal goes to the job before that, so none is lost
# to a child whose dispositions are not yet COMMAND's. With -g the holder
# first leads a process group of its own, which COMMAND joins; the group's
# number is the holder's, which no other group can take while it runs.
#
# With -p, PID is already the holder's child: release.sh's `held` forks the
# job, which opens DIR/go for reading and runs only once it reads `go`, and
# then execs this program in its own place with the write end of DIR/go on
# descriptor 6. The holder checks that waitpid finds PID its own and not yet
# ended, reads DIR/stop, and then writes `go`, or on a stop closes descriptor
# 6, and the job reads the end of the file and exits 143 without running
# anything. A job that is not this process's child, which cannot be with the
# gate, is refused with 127, and nothing is signalled.
#
# Once the job runs, one loop, a look each tenth of a second, reaps the job
# and, once DIR/stop exists, the holder has taken HUP, INT or TERM, or with
# -P the process that started the holder has gone, as getppid shows, sends
# TERM, and KILL SECONDS later, 300 unless -k gives another: to the job, its
# unreaped child, whose number stays its own until this loop reaps it, as a
# zombie's does; with -g to the whole group, which the holder leads. With -g
# the holder waits after the job's end until ps shows nothing else in its
# group, sending the same stop to the group. On Linux the holder is a child
# subreaper (prctl PR_SET_CHILD_SUBREAPER), so every process below it that
# loses its parent becomes its child; after the job's end it stops each
# child it then has, TERM and KILL SECONDS later, and reaps each, until
# waitpid finds none: a drain that it confirms, since only it reaps them and
# no number it signals can have passed to another process. On the Mac, which
# has no subreaper, a process that leaves the group escapes the holder, and
# stop.sh lists such a process without signalling it.
#
# The holder then writes DIR/end and exits with the job's status; 143 when a
# stop came before the job ran, and 125 when DIR/live could not be taken.
#
# state: print the run's state, read from the lock without creating it:
# `running` while DIR/live is held; `starting` when DIR exists and has no
# DIR/live yet; `ended CODE` when the lock is free and DIR/end holds CODE;
# `lost` when the lock is free and there is no DIR/end, as when the holder
# was killed outright: what it held may still run, and nothing here signals
# it; `absent` when there is no DIR. Exit 0, or 1 with `unknown: WHY` when
# the lock cannot be probed.
#
# stop: create DIR/stop, unless DIR is absent, and wait up to SECONDS, in
# turns of a second, while the run is `running` or `starting`; print the
# state then. Exit 0 on `ended` or `absent`, 1 otherwise.
#
# Nothing reads a clock: each wait counts turns of a sleep, which a suspended
# host stretches and never shortens.

use strict;
use warnings;
use Config;
use Errno qw(ENOENT EWOULDBLOCK EINTR ECHILD EEXIST);
use Fcntl qw(:flock O_CREAT O_EXCL O_RDWR O_WRONLY);
use POSIX qw(WNOHANG);

my $usage = "usage: hold.pl run [-g] [-P] [-k SECONDS] DIR COMMAND... | run [-P] [-k SECONDS] DIR -p PID | " .
    "state DIR | stop DIR SECONDS\n";

# create FILE TEXT: create FILE holding TEXT whole, by a link that fails when
# FILE exists; return whether this call created it.
sub create
{
    my ($file, $text) = @_;
    my $part = "$file.$$";
    open(my $f, '>', $part) or return 0;
    my $ok = (print $f $text) && close($f) && link($part, $file);
    unlink($part);
    return $ok;
}

# state DIR: return the run's state as the header's `state` prints it.
sub state
{
    my $dir = shift;
    return 'absent' if !-e $dir;
    my $l;
    if (!open($l, '<', "$dir/live")) {
        return $! == ENOENT ? 'starting' : "unknown: $dir/live: $!";
    }
    if (!flock($l, LOCK_SH | LOCK_NB)) {
        return $! == EWOULDBLOCK ? 'running' : "unknown: $dir/live: $!";
    }
    close($l);
    if (open(my $e, '<', "$dir/end")) {
        my $code = <$e> // '';
        chomp $code;
        return $code =~ /^\d+$/ ? "ended $code" : "unknown: $dir/end holds no status";
    }
    return $! == ENOENT ? 'lost' : "unknown: $dir/end: $!";
}

my $mode = shift // '';
if ($mode eq 'state') {
    @ARGV == 1 or die $usage;
    my $s = state($ARGV[0]);
    print "$s\n";
    exit($s =~ /^unknown/ ? 1 : 0);
}
if ($mode eq 'stop') {
    @ARGV == 2 && $ARGV[1] =~ /^\d+$/ or die $usage;
    my ($dir, $bound) = @ARGV;
    if (-e $dir && !sysopen(my $t, "$dir/stop", O_WRONLY | O_CREAT, 0644)) {
        print "unknown: $dir/stop cannot be made: $!\n";
        exit 1;
    }
    my $s = state($dir);
    for (my $turn = 0; $turn < $bound && ($s eq 'running' || $s eq 'starting'); $turn++) {
        sleep 1;
        $s = state($dir);
    }
    print "$s\n";
    exit($s =~ /^(ended|absent)/ ? 0 : 1);
}
$mode eq 'run' or die $usage;

my ($group, $bound, $orphan) = ('', 300, '');
while (@ARGV && $ARGV[0] =~ /^-[gkP]$/) {
    my $o = shift;
    if ($o eq '-g') {
        $group = 1;
    } elsif ($o eq '-P') {
        $orphan = getppid();
    } else {
        $bound = shift // '';
        $bound =~ /^\d+$/ or die $usage;
    }
}
my ($dir, @command) = @ARGV;
defined $dir && -d $dir && @command or die $usage;
my $pid;
if ($command[0] eq '-p') {
    !$group && @command == 2 && $command[1] =~ /^\d+$/ or die $usage;
    $pid = $command[1];
}

# The lock, taken before anything else; Perl closes it on exec.
my $live;
{
    my $part = "$dir/live.$$";
    if (!sysopen($live, $part, O_RDWR | O_CREAT | O_EXCL, 0644) ||
        !flock($live, LOCK_EX | LOCK_NB) || !link($part, "$dir/live")) {
        print STDERR "hold: $dir/live cannot be taken: $!\n";
        unlink($part);
        exit 125;
    }
    unlink($part);
}

# end CODE: write CODE to DIR/end, once, and exit with it.
sub end
{
    my $code = shift;
    create("$dir/end", "$code\n") or print STDERR "hold: $dir/end cannot be written: $!\n";
    exit $code;
}

# A signal to the holder counts as a stop; the handlers only record it.
my $asked;
my %start = map { ($_ => $SIG{$_} // 'DEFAULT') } qw(HUP INT TERM);
$SIG{$_} = sub { $asked = 1 } for qw(HUP INT TERM);
sub stopped { return $asked || -e "$dir/stop" || ($orphan && getppid() != $orphan) }

# PR_SET_CHILD_SUBREAPER, by prctl's number on each architecture the testing
# host may run.
my $reaper = 0;
if ($^O eq 'linux') {
    my %prctl = (x86_64 => 157, i686 => 172, i386 => 172, aarch64 => 167);
    my ($arch) = $Config{archname} =~ /^([^-]+)/;
    my $n = $prctl{$arch // ''};
    $reaper = defined $n && syscall($n, 36, 1, 0, 0, 0) == 0;
}

if (!defined $pid) {
    setpgrp(0, 0) or die "hold: setpgrp: $!\n" if $group;
    pipe(my $gate_r, my $gate_w) or die "hold: pipe: $!\n";
    pipe(my $exec_r, my $exec_w) or die "hold: pipe: $!\n";
    $pid = fork();
    defined $pid or die "hold: cannot fork: $!\n";
    if ($pid == 0) {
        close($gate_w);
        close($exec_r);
        $SIG{$_} = $start{$_} for keys %start;
        my $n;
        do { $n = sysread($gate_r, my $go, 1) } while (!defined $n && $! == EINTR);
        POSIX::_exit(143) unless $n;
        no warnings 'exec';
        exec { $command[0] } @command;
        syswrite($exec_w, "$!");
        POSIX::_exit(127);
    }
    close($gate_r);
    close($exec_w);
    if (stopped()) {
        close($gate_w);
        waitpid($pid, 0);
        end(143);
    }
    syswrite($gate_w, 'x', 1);
    close($gate_w);
    my ($n, $why) = (0, '');
    do { $n = sysread($exec_r, my $b, 512); $why .= $b if $n } while ($n || (!defined $n && $! == EINTR));
    print STDERR "hold: cannot run $command[0]: $why\n" if $why ne '';
} else {
    open(my $go, '>&=', 6) or do { print STDERR "hold: no gate on descriptor 6: $!\n"; end(127) };
    if (waitpid($pid, WNOHANG) != 0) {
        print STDERR "hold: job $pid is not this process's child, or ended at its gate\n";
        close($go);
        end(127);
    }
    syswrite($go, "go\n") unless stopped();
    close($go);
}

# The job runs; only this loop reaps it, and every signal goes from here.
my ($code, $sent, $looks) = (undef, 0, 0);
sub signal
{
    my ($name, @to) = @_;
    kill($name, @to);
}
while (1) {
    my $r = waitpid($pid, WNOHANG);
    if ($r == $pid) {
        $code = $? & 127 ? 128 + ($? & 127) : $? >> 8;
        last;
    }
    # The job is not reaped, so PID is still its own.
    if (stopped()) {
        signal('TERM', $group ? -$$ : $pid) if !$sent++;
        signal('KILL', $group ? -$$ : $pid) if $looks++ == $bound * 10;
    }
    select(undef, undef, undef, 0.1);
}

# others: return how many processes but this one ps shows in this process's
# group, ps's own excepted, or nothing when ps cannot be read.
sub others
{
    my $ps = open(my $f, '-|', 'ps', '-A', '-o', 'pgid=', '-o', 'pid=') or return;
    my $n = 0;
    while (<$f>) {
        $n++ if /^\s*(\d+)\s+(\d+)/ && $1 == $$ && $2 != $$ && $2 != $ps;
    }
    close($f) or return;
    return $n;
}

# children: return the processes whose parent is this one, from /proc, each
# an unreaped child of this process, or nothing when /proc cannot be read.
sub children
{
    opendir(my $d, '/proc') or return;
    my @kids;
    for my $p (grep { /^\d+$/ } readdir($d)) {
        open(my $f, '<', "/proc/$p/stat") or next;
        my $s = <$f> // '';
        $s =~ s/.*\) //s;
        push(@kids, $p) if (split(' ', $s))[1] == $$;
    }
    return \@kids;
}

# Once the job has ended, what it left: with -g the rest of the group, and
# on Linux every child this process adopted. Each is stopped as the job is:
# TERM at once, since the job is gone, and KILL SECONDS later.
my $turns = 0;
while ($group || $reaper) {
    my $n = $group ? others() : 0;
    1 while waitpid(-1, WNOHANG) > 0;
    my $kids = $reaper ? children() : [];
    last if defined $n && $n == 0 && defined $kids && !@$kids && (!$reaper || waitpid(-1, WNOHANG) < 0);
    if ($turns == 0) {
        print STDERR "hold: the job left processes behind; stopping them\n";
    }
    my $sig = $turns >= $bound ? 'KILL' : 'TERM';
    if ($turns == 0 || $turns == $bound) {
        signal($sig, -$$) if $group;
        signal($sig, @$kids) if $kids && @$kids;
    }
    $turns++;
    sleep 1;
}
end($code);
