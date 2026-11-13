#!/usr/bin/perl
#
# hold.pl: run a job as this process's child in a run directory of its own,
# and be the only process that signals it.
#
# Usage: perl hold.pl run [-g] [-P] [-D] [-k SECONDS] DIR COMMAND...
#        perl hold.pl run [-P] [-k SECONDS] DIR -p PID
#        perl hold.pl state DIR
#        perl hold.pl stop DIR SECONDS
#        perl hold.pl clear [-n] ROOT...
#
# DIR is the run's directory, which the caller makes once, by a mkdir that
# fails when it exists, and never makes again; its name is the run's, unique,
# and every file in it is only ever created:
#
# * DIR/live, a lock that the holder takes exclusively before anything else
#   and keeps for its whole life. It is created under another name, locked,
#   and then linked into place, so a DIR/live that exists has been locked. Its
#   descriptor is closed on exec, so no job inherits it.
# * DIR/boot, the ID of the boot the holder runs in, which the holder writes
#   before its job can run: /proc/sys/kernel/random/boot_id on Linux, `sysctl
#   -n kern.bootsessionuuid` on the Mac.
# * DIR/group, with -g, the number of the job's process group, which the
#   holder writes before its job can run, for a person who checks a lost run.
# * DIR/stop, the tombstone, empty: a stopper creates it, and the holder reads
#   it before the job starts and on every turn after.
# * DIR/end, the job's status as sh reports one, written to a file of its own
#   and linked into place, which fails when DIR/end exists, so it is written
#   once: by this holder, or by a looker that has confirmed a lost run's end,
#   as `state` says.
# * DIR/go, the gate of -p, a FIFO that the caller makes.
# * DIR/jobs, the run directories of the jobs that the run's own job starts,
#   each a DIR of its own, nested under the run that started it, as
#   release.sh's job_dir makes them from RACH2_HOLD_DIR.
#
# A run directory on disk is a run not yet proved ended: it is removed only
# once its state, below, reads `ended`, so the directories under a root are
# the registry of what may still run, and outlive every invocation.
#
# run: the holder. It sets RACH2_HOLD_DIR to DIR, made absolute, for COMMAND,
# so that the jobs COMMAND starts nest under DIR/jobs. With COMMAND it forks
# COMMAND, which waits on a pipe before
# its exec: the holder reads DIR/stop first, and on a stop closes the pipe,
# and the child exits 143 without running anything; otherwise it lets the
# child go and waits on a second pipe, closed on exec, until the exec has
# happened or failed. No signal goes to the job before that, so none is lost
# to a child whose dispositions are not yet COMMAND's. With -g the holder
# leads a process group of its own and forks the anchor, a child that leads
# another group, ignores HUP, INT and TERM, and exits once the holder closes
# the pipe to it or dies; COMMAND joins the anchor's group, whose number is
# the anchor's, which no other group can take while the anchor is this
# process's unreaped child, killed or not. The holder is never in the group
# it signals, so no KILL of the group reaches it.
#
# With -p, PID is already the holder's child: release.sh's `held` opens
# DIR/go for reading and writing on descriptor 6 and forks the job, which
# opens DIR/go for reading on a descriptor of its own, which cannot block
# since its descriptor 6 writes to the FIFO, closes its 6, and runs only once
# it reads `go`; and then execs this program in its own place with descriptor
# 6. The FIFO lasts while either end is open, so the job's reader finds what
# the holder writes, however late it opens. The holder checks that waitpid
# finds PID its own and not yet ended, reads DIR/stop, and then writes `go`,
# or on a stop closes descriptor 6, and the job reads the end of the file and
# exits 143 without running anything. A job that is not this process's
# child, which cannot be with the gate, is refused with 127, and nothing is
# signalled.
#
# Once the job runs, one loop, a look each tenth of a second, reaps the job
# and, once DIR/stop exists, the holder has taken HUP, INT or TERM, or with
# -P the process that started the holder has gone, as getppid shows, sends
# TERM, and KILL SECONDS later, 300 unless -k gives another: to the job, its
# unreaped child, whose number stays its own until this loop reaps it, as a
# zombie's does; with -g to the anchor's whole group. After the job's end the
# holder drains what it left: with -g the rest of the anchor's group, as ps
# shows it, and on Linux, where the holder is a child subreaper (prctl
# PR_SET_CHILD_SUBREAPER), so that every process below it that loses its
# parent becomes its child, each child it has. On each turn of a second it
# reaps what has ended, sends TERM to each child and to the group that has
# had none yet, and from the KILL's turn on, which counts on from a stop, KILL
# to each and to the group on every turn, since a child that a KILL ends
# hands its own children to the holder. It reaps the anchor last, once ps
# shows nothing else in its group, and ends the drain only once waitpid finds
# no child at all, or each child left is a detached holder, below: a drain
# that it confirms, since only it reaps them and no number it signals can
# have passed to another process. On the Mac, which has no subreaper, a
# process that leaves the group escapes the holder, and stop.sh lists such a
# process without signalling it.
#
# With -D the run is detached: it is started in the background to outlive the
# job that started it, as release.sh's run on the Mac outlives the release
# that waits for it, and has a stop of its own. A holder whose drain adopts
# such a holder on Linux, once the job above it has ended, leaves it and its
# group alone: it knows one by its command line in /proc, a hold.pl run with
# -D, neither signals it nor waits for it, and reaps it if it ends first. The
# orphans of a detached run go to its own holder, a subreaper too.
#
# The holder then writes DIR/end and exits with the job's status; 143 when a
# stop came before the job ran, and 125 when DIR/live could not be taken, or
# DIR/boot, DIR/group or the anchor could not be made, when no job runs.
#
# state: print the run's state, read from the lock without creating it:
# `running` while DIR/live is held; `starting` when DIR exists and has no
# DIR/live yet; `ended CODE` when the lock is free and DIR/end holds CODE;
# `lost` when the lock is free and there is no DIR/end, as when the holder
# was killed outright: what it held may still run, and nothing here signals
# it; `absent` when there is no DIR. Exit 0, or 1 with `unknown: WHY` when
# the lock cannot be probed. A lost run whose end is proved is ended: state
# then creates DIR/end with 137, as the holder would, and reads it. The one
# proof is DIR/boot naming another boot than this one. A group that ps shows
# empty is no proof: a process of the job may have left the group, by setsid
# or setpgid, and once the holder is gone nothing holds it, since Linux hands
# a dead subreaper's orphans to the next one above or to init. Anything else
# stays lost until a person has stopped what is left and created DIR/end
# with 137. The state covers the run's jobs: a run that has ended reads
# `ended CODE` only once each directory under DIR/jobs reads ended too, and
# otherwise reads the first of `running`, `starting`,
# `unknown: WHY` and `lost` that one of them reads, so that a holder killed
# at its bound, whose jobs' holders still run, reads running, and one whose
# job's holder was killed reads lost.
#
# stop: create DIR/stop, unless DIR is absent, and wait up to SECONDS, in
# turns of a second, while the run is `running` or `starting`; print the
# state then. Exit 0 on `ended` or `absent`, 1 otherwise.
#
# clear: the one decider of what each run directory in each ROOT permits.
# Remove each whose state reads ended, unless -n is given, and print a line
# `DIR STATE` for each that stays, and one for each directory under it, at
# any depth, that reads lost or unknown by itself, as a job's whose holder was
# killed under a run that still runs. Exit 0 when none stays, 1 when only
# running or starting runs stay, and 2 when any line is lost or unknown, or a
# ROOT cannot be read. A ROOT that does not exist holds nothing.
#
# Nothing reads a clock: each wait counts turns of a sleep, which a suspended
# host stretches and never shortens.

use strict;
use warnings;
use Config;
use Errno qw(ENOENT EWOULDBLOCK EINTR ECHILD);
use Fcntl qw(:flock O_CREAT O_EXCL O_RDWR O_WRONLY);
use POSIX qw(WNOHANG);

my $usage = "usage: hold.pl run [-g] [-P] [-D] [-k SECONDS] DIR COMMAND... | run [-P] [-k SECONDS] DIR -p PID | " .
    "state DIR | stop DIR SECONDS | clear [-n] ROOT...\n";

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

# line FILE: return the first line of FILE without its newline, or nothing
# when FILE cannot be read.
sub line
{
    open(my $f, '<', shift) or return;
    my $s = <$f>;
    defined $s or return;
    chomp $s;
    return $s;
}

# boot: return the ID of this boot, as the header's DIR/boot says, or nothing
# when it cannot be read.
sub boot
{
    my $b = line('/proc/sys/kernel/random/boot_id');
    if (!defined $b && $^O eq 'darwin') {
        $b = `/usr/sbin/sysctl -n kern.bootsessionuuid 2> /dev/null`;
        $b = undef if $? != 0;
        chomp $b if defined $b;
    }
    return defined $b && $b ne '' ? $b : undef;
}

# members GROUP [BUT]: return how many processes ps shows in the process
# group GROUP, BUT and ps itself excepted, or nothing when ps fails.
sub members
{
    my ($group, $but) = @_;
    my $ps = open(my $f, '-|', 'ps', '-A', '-o', 'pgid=', '-o', 'pid=') or return;
    my $n = 0;
    while (<$f>) {
        $n++ if /^\s*(\d+)\s+(\d+)/ && $1 == $group && $2 != ($but // 0) && $2 != $ps;
    }
    close($f) or return;
    return $n;
}

# ended DIR: return `ended CODE` from DIR/end, nothing when it does not
# exist, or `unknown: WHY`.
sub ended
{
    my $dir = shift;
    my $e;
    if (!open($e, '<', "$dir/end")) {
        return $! == ENOENT ? undef : "unknown: $dir/end: $!";
    }
    my $code = <$e> // '';
    chomp $code;
    return $code =~ /^\d+$/ ? "ended $code" : "unknown: $dir/end holds no status";
}

# proved DIR: return whether nothing of the lost run in DIR can still run, by
# the one proof that the header's `state` gives, the boot.
sub proved
{
    my $dir = shift;
    my ($then, $now) = (line("$dir/boot"), boot());
    return defined $then && defined $now && $then ne $now;
}

# own DIR: return the state of the run in DIR alone, its jobs aside.
sub own
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
    my $s = ended($dir);
    return $s if defined $s;
    return 'lost' unless proved($dir);
    # The holder is gone, so no other end can come but a looker's like this.
    create("$dir/end", "137\n");
    return ended($dir) // 'lost';
}

# jobs DIR: return the run directories under DIR/jobs.
sub jobs
{
    my $dir = shift;
    opendir(my $d, "$dir/jobs") or return ();
    return grep { -d && !-l } map { "$dir/jobs/$_" } grep { !/^\.\.?$/ } readdir($d);
}

# state DIR: return the run's state, its jobs' included, as the header's
# `state` prints it.
sub state
{
    my $dir = shift;
    my $s = own($dir);
    return $s unless $s =~ /^ended /;
    my %rank = (running => 4, starting => 3, unknown => 2, lost => 1);
    my ($worst, $at) = ('', 0);
    for my $j (jobs($dir)) {
        my $js = state($j);
        my ($word) = $js =~ /^(\w+)/;
        my $r = $rank{$word // ''} // 0;
        ($worst, $at) = ($js, $r) if $r > $at;
    }
    return $at ? $worst : $s;
}

# below DIR: return `DIR STATE` for each directory under DIR/jobs, at any
# depth, whose own state is lost or unknown.
sub below
{
    my $dir = shift;
    my @out;
    for my $j (jobs($dir)) {
        my $s = own($j);
        push(@out, "$j $s") if $s =~ /^(lost|unknown)/;
        push(@out, below($j));
    }
    return @out;
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
if ($mode eq 'clear') {
    my $keep = @ARGV && $ARGV[0] eq '-n' ? shift : '';
    @ARGV or die $usage;
    require File::Path;
    my $rc = 0;
    for my $root (@ARGV) {
        my $d;
        if (!opendir($d, $root)) {
            next if $! == ENOENT;
            print "$root unknown: $root cannot be read: $!\n";
            $rc = 2;
            next;
        }
        for my $n (sort grep { !/^\.\.?$/ } readdir($d)) {
            my $dir = "$root/$n";
            -d $dir && !-l $dir or next;
            my $s = state($dir);
            if ($s =~ /^ended /) {
                File::Path::remove_tree($dir) unless $keep;
                next;
            }
            my @lines = ("$dir $s", below($dir));
            print "$_\n" for @lines;
            for (@lines) {
                $rc = /^\S+ (lost|unknown)/ ? 2 : ($rc || 1);
            }
        }
    }
    exit $rc;
}
$mode eq 'run' or die $usage;

my ($group, $bound, $orphan) = ('', 300, '');
while (@ARGV && $ARGV[0] =~ /^-[gkPD]$/) {
    my $o = shift;
    if ($o eq '-g') {
        $group = 1;
    } elsif ($o eq '-D') {
        # Read only by a holder above, from the command line.
    } elsif ($o eq '-P') {
        $orphan = getppid();
    } else {
        $bound = shift // '';
        $bound =~ /^\d+$/ or die $usage;
    }
}
my ($dir, @command) = @ARGV;
defined $dir && -d $dir && @command or die $usage;
require File::Spec;
$ENV{RACH2_HOLD_DIR} = File::Spec->rel2abs($dir);
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

# The anchor of -g, and the write end of the pipe whose end lets it exit.
my ($anchor, $hold_w) = (0);

# end CODE: reap the anchor, if any, write CODE to DIR/end, once, and exit
# with it.
sub end
{
    my $code = shift;
    if ($anchor) {
        close($hold_w);
        waitpid($anchor, 0);
    }
    create("$dir/end", "$code\n") or print STDERR "hold: $dir/end cannot be written: $!\n";
    exit $code;
}

# The evidence by which a looker proves a lost run's end, written before
# anything can run.
my $boot = boot();
if (!defined $boot || !create("$dir/boot", "$boot\n")) {
    print STDERR "hold: $dir/boot cannot be written: " . (defined $boot ? $! : 'no boot ID') . "\n";
    end(125);
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

if ($group) {
    if (!setpgrp(0, 0)) {
        print STDERR "hold: setpgrp: $!\n";
        end(125);
    }
    pipe(my $hold_r, $hold_w) or do { print STDERR "hold: pipe: $!\n"; end(125) };
    my $child = fork();
    defined $child or do { print STDERR "hold: cannot fork: $!\n"; end(125) };
    if ($child == 0) {
        close($hold_w);
        close($live);
        $SIG{$_} = 'IGNORE' for qw(HUP INT TERM);
        setpgrp(0, 0);
        while (1) {
            my $n = sysread($hold_r, my $byte, 1);
            last if defined $n ? $n == 0 : !$!{EINTR};
        }
        POSIX::_exit(0);
    }
    close($hold_r);
    $anchor = $child;
    if (!setpgrp($anchor, $anchor) || !create("$dir/group", "$anchor\n")) {
        print STDERR "hold: the group $anchor cannot be made or recorded: $!\n";
        end(125);
    }
}

if (!defined $pid) {
    pipe(my $gate_r, my $gate_w) or die "hold: pipe: $!\n";
    pipe(my $exec_r, my $exec_w) or die "hold: pipe: $!\n";
    $pid = fork();
    defined $pid or die "hold: cannot fork: $!\n";
    if ($pid == 0) {
        close($gate_w);
        close($exec_r);
        close($hold_w) if $anchor;
        $SIG{$_} = $start{$_} for keys %start;
        setpgrp(0, $anchor) if $anchor;
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
    # The child joins the group itself too; whichever comes first does it.
    if ($anchor && !setpgrp($pid, $anchor) && getpgrp($pid) != $anchor) {
        print STDERR "hold: the job cannot join the group $anchor: $!\n";
        close($gate_w);
        waitpid($pid, 0);
        end(125);
    }
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
# With -g the target is the anchor's group, which the holder is not in.
my $target = $anchor ? -$anchor : $pid;
my ($code, $sent, $looks) = (undef, 0, 0);
while (1) {
    my $r = waitpid($pid, WNOHANG);
    if ($r == $pid) {
        $code = $? & 127 ? 128 + ($? & 127) : $? >> 8;
        last;
    }
    # The job is not reaped, so PID is still its own.
    if (stopped()) {
        kill('TERM', $target) if !$sent++;
        kill('KILL', $target) if $looks++ == $bound * 10;
    }
    select(undef, undef, undef, 0.1);
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

# detached PID: succeed when the process PID is a detached holder, a
# `hold.pl run` with -D among its options, as /proc/PID/cmdline shows it.
sub detached
{
    open(my $f, '<', "/proc/$_[0]/cmdline") or return 0;
    local $/;
    my @arg = split(/\0/, <$f> // '');
    shift(@arg) while @arg && $arg[0] !~ m{(^|/)hold\.pl$};
    shift(@arg);
    return 0 unless @arg && shift(@arg) eq 'run';
    while (@arg && $arg[0] =~ /^-[gkPD]$/) {
        my $o = shift(@arg);
        return 1 if $o eq '-D';
        shift(@arg) if $o eq '-k';
    }
    return 0;
}

# Once the job has ended, the drain of what it left, as the header says. The
# turns count on from a stop's, so that the KILL comes SECONDS after the
# stop's TERM, not after the job's end.
my $turns = $sent ? int($looks / 10) : 0;
my (%termed, $told);
my $grouped = $sent;
while ($anchor || $reaper) {
    my $kids = $reaper ? children() : [];
    if ($kids && @$kids) {
        # Reap what has ended, but the anchor, which keeps the group's number.
        waitpid($_, WNOHANG) for grep { $_ != $anchor } @$kids;
        $kids = children();
    }
    my @left = grep { $_ != $anchor && !detached($_) } @{$kids // []};
    my $n = $anchor ? members($anchor, $anchor) : 0;
    if (defined $n && $n == 0 && $kids && !@left) {
        if ($anchor) {
            close($hold_w);
            waitpid($anchor, 0);
            $anchor = 0;
            next;
        }
        my $r = waitpid(-1, WNOHANG);
        last if $r < 0 && $! == ECHILD;
        next if $r > 0;
        # Children remain; the drain ends when each is a detached holder.
        my $now = children();
        last if $r == 0 && $now && !grep { !detached($_) } @$now;
    }
    print STDERR "hold: the job left processes behind; stopping them\n" if !$told++;
    # Each number here is an unreaped child, or the anchor's group while the
    # anchor is one, as read since the last reap.
    if ($turns >= $bound) {
        kill('KILL', -$anchor) if $anchor;
        kill('KILL', @left) if @left;
    } else {
        kill('TERM', -$anchor) if $anchor && !$grouped++;
        kill('TERM', grep { !$termed{$_}++ } @left);
    }
    $turns++;
    sleep 1;
}
end($code);
