#!/usr/bin/perl
#
# claim.pl: run, hold, read and stop a claim of WSL's admission for a Windows
# release run, as release.sh's claim_start, claim_state and claim_stop send it.
#
# Usage: perl claim.pl run LOCKS OUT ID KIND N COMMAND...
#        perl claim.pl hold LOCKS OUT ID KIND N DONE
#        perl claim.pl state LOCKS ID KIND N
#        perl claim.pl stop LOCKS ID KIND [N]
#
# LOCKS is a directory on a file system where flock holds, ext4 in WSL; OUT
# is Windows' output directory as WSL sees it; ID is the Windows run, KIND
# `claim` or `hold`, N the attempt, from 1. Every file of a claim is in
# LOCKS, named ID.KIND.N.WHAT for attempt N, and is only ever created: none
# is removed or rewritten before sweep.sh removes the claims of a closed
# milestone. They are:
#
# * ID.KIND.stop and ID.KIND.N.stop, tombstones, empty: the first ends the
#   claim, the second retires attempt N before a retry. A stop that comes
#   late or twice changes nothing.
# * ID.KIND.N.live, a lock that the runner holds exclusively from before it
#   reads the tombstones until it exits, and that its launcher inherits.
# * ID.KIND.N.go: the runner read no tombstone and OUT/windows.current named
#   ID, and starts the launcher; ID.KIND.N.none: it starts nothing.
# * ID.KIND.N.owner: the launcher's number and its start, from proc(5)'s
#   stat, which the ledger keeps beside its owner; written before its exec,
#   so before it can enter the ledger.
# * ID.KIND.N.granted, a lock that the holder, the command that admission
#   runs, holds exclusively while it holds the claim; its existence says that
#   the attempt was granted.
# * ID.KIND.N.reaped: the runner has reaped the launcher.
# * ID.KIND.N.held: the launcher stayed in the ledger after its end.
# * ID.KIND.N.rc: the launcher's status, written last.
#
# Lookers probe a lock shared and non-blocking, and never create its file: a
# missing file is a free lock. A stopper writes its tombstone first and only
# then probes the locks, so a runner or holder that read no tombstone holds
# a lock that the stopper waits for, and one that locks later reads the
# tombstone; neither side has anything to undo. No lock is shared between
# claims or attempts.
#
# run: the runner, which claim_start starts in a session of its own. It
# creates and locks ID.KIND.N.live, a few tries a second apart; reads both
# tombstones and OUT/windows.current, and starts nothing after a tombstone,
# when the file names another run or when the lock was not got, creating
# ID.KIND.N.none then; creates ID.KIND.N.go; clears
# close-on-exec on the lock and forks COMMAND, the launcher, with
# PR_SET_PDEATHSIG of TERM on x86-64 Linux, so that a runner that dies first
# withdraws the launcher's request. While the launcher runs it reads the
# tombstones each second, and on one sends TERM, and KILL thirty turns
# later, to the launcher, its own unreaped child. Once it has reaped it, it
# creates ID.KIND.N.reaped, has etude status recover the ledger, which drops
# a dead owner, and reads the ledger for the launcher, a minute at most;
# creates ID.KIND.N.held when the launcher stays there, and then
# ID.KIND.N.rc.
#
# hold: the holder, the last word of COMMAND, which admission runs once it
# grants the claim. It has no children. It creates and locks
# ID.KIND.N.granted, reads the tombstones, and holds the lock while OUT/DONE
# is missing, no tombstone exists and the size of OUT/ID.alive, to which the
# part's script on Windows and the release's beat each append a byte a
# minute, changed in the last 120 looks, five seconds apart.
#
# state: print `granted` while ID.KIND.N.granted is held and the launcher is
# not reaped; nothing while a lock is held otherwise, as while admission
# waits or the runner cleans up, or when a lock cannot be probed; once both
# are free, `dead` when the attempt was granted, `refused` when it has a
# status, `unstarted` when it never started its launcher, and `dead`
# otherwise, as a restart of WSL leaves it.
#
# stop: create the claim's tombstone, or attempt N's with N, and wait two
# and a half minutes at most for the locks of each attempt, or of N, to be
# free; then have etude status recover the ledger, a minute at most, and
# look up each attempt's owner there. Exit 0 once recovery succeeded, the
# ledger names none of them and no attempt has a held file; otherwise print
# a line for what stays and exit 1.
#
# Nothing reads a clock: each wait counts turns of a sleep, which a suspended
# host stretches and never shortens. RACH2_CLAIM_LEDGER and RACH2_CLAIM_ETUDE
# name another ledger and etude, which the host case uses.

use strict;
use warnings;
use Errno qw(ENOENT EWOULDBLOCK);
use Fcntl qw(:flock O_CREAT O_RDWR O_WRONLY F_GETFD F_SETFD FD_CLOEXEC);
use File::Path qw(make_path);
use POSIX qw(WNOHANG);

my $ledger = $ENV{RACH2_CLAIM_LEDGER} // '/var/lib/rach2-etude/ledger';
my $etude = $ENV{RACH2_CLAIM_ETUDE} // '/usr/local/libexec/rach2/etude';

# touch FILE: create FILE, empty, unless it exists; succeed once it exists.
sub touch
{
    my ($file) = @_;
    sysopen(my $f, $file, O_WRONLY | O_CREAT, 0644) or return 0;
    return close($f);
}

# create FILE TEXT: create FILE holding TEXT whole, by a link that fails
# when FILE exists; succeed once FILE exists.
sub create
{
    my ($file, $text) = @_;
    my $part = "$file.$$";
    open(my $f, '>', $part) or return 0;
    my $ok = (print $f $text) && close($f) && (link($part, $file) || -e $file);
    unlink($part);
    return $ok;
}

# take FILE: create FILE and take it exclusively, ten tries a second apart,
# since a looker may hold it shared for a moment; return the handle, or
# nothing.
sub take
{
    my ($file) = @_;
    sysopen(my $l, $file, O_RDWR | O_CREAT, 0644) or return;
    for (1 .. 10) {
        return $l if flock($l, LOCK_EX | LOCK_NB);
        sleep 1;
    }
    return;
}

# held FILE: 1 while a process holds the lock FILE, 0 when it is free or
# FILE does not exist, and nothing when it cannot be probed. FILE is never
# created.
sub held
{
    my ($file) = @_;
    my $l;
    open($l, '<', $file) or return $! == ENOENT ? 0 : undef;
    return 0 if flock($l, LOCK_SH | LOCK_NB);
    return $! == EWOULDBLOCK ? 1 : undef;
}

# stopped CLAIM N: succeed when a tombstone ends the claim CLAIM or its
# attempt N.
sub stopped
{
    my ($claim, $n) = @_;
    return -e "$claim.stop" || -e "$claim.$n.stop";
}

# start PID: field 22 of proc(5)'s stat of process PID, the start that the
# ledger keeps beside its owner's number, or nothing where there is no proc.
sub start
{
    my ($p) = @_;
    open(my $s, '<', "/proc/$p/stat") or return '';
    my $line = <$s> // '';
    $line =~ s/.*\) //s;
    return (split(' ', $line))[19] // '';
}

# bounded SECONDS COMMAND...: run COMMAND with its output discarded, and
# kill it, its own unreaped child, after SECONDS counted seconds; return its
# status, or nothing when it was killed or could not start.
sub bounded
{
    my ($limit, @command) = @_;
    my $pid = fork();
    defined $pid or return;
    if ($pid == 0) {
        open(STDOUT, '>', '/dev/null');
        open(STDERR, '>&', \*STDOUT);
        exec { $command[0] } @command or POSIX::_exit(127);
    }
    for (1 .. $limit * 10) {
        return $? if waitpid($pid, WNOHANG) == $pid;
        select(undef, undef, undef, 0.1);
    }
    return $? if waitpid($pid, WNOHANG) == $pid;
    kill('KILL', $pid);
    waitpid($pid, 0);
    return;
}

# listed PID START: 1 when the ledger names the owner PID with START, or
# with an unknown start, 0 when it does not or has no claims, and nothing
# when it cannot be read.
sub listed
{
    my ($pid, $start) = @_;
    $start = "x" if $start eq "";
    my $c;
    open($c, '<', "$ledger/claims") or return $! == ENOENT ? 0 : undef;
    while (<$c>) {
        return 1 if /^owner \Q$pid\E (\Q$start\E |\d+ 0$)/;
    }
    return 0;
}

# claim_run LOCKS OUT ID KIND N COMMAND...: the runner, as the header says.
sub claim_run
{
    my ($locks, $out, $id, $kind, $n, @command) = @_;
    my $claim = "$locks/$id.$kind";
    my $base = "$claim.$n";
    my $live = take("$base.live");
    if (!$live) {
        touch("$base.none");
        print "claim: attempt $n starts nothing: $base.live cannot be locked\n";
        return 1;
    }
    if (stopped($claim, $n)) {
        touch("$base.none");
        print "claim: attempt $n starts nothing: the claim or the attempt is stopped\n";
        return 0;
    }
    my $current = '';
    if (open(my $c, '<', "$out/windows.current")) {
        $current = <$c> // '';
        $current =~ s/\r?\n\z//;
    }
    if ($current ne $id) {
        touch("$base.none");
        print "claim: attempt $n starts nothing: the current run is not $id\n";
        return 0;
    }
    touch("$base.go") or die "claim: cannot create $base.go\n";
    # The launcher holds the lock too, so that a stopper waits for it.
    fcntl($live, F_SETFD, fcntl($live, F_GETFD, 0) & ~FD_CLOEXEC) or die "claim: fcntl: $!\n";
    my $parent = $$;
    my $pid = fork();
    defined $pid or die "claim: cannot fork: $!\n";
    if ($pid == 0) {
        # prctl(PR_SET_PDEATHSIG, SIGTERM) on x86-64 Linux.
        syscall(157, 1, 15) if $^O eq 'linux' && (POSIX::uname())[4] eq 'x86_64';
        POSIX::_exit(1) if getppid() != $parent;
        create("$base.owner", "$$ " . start($$) . "\n") or POSIX::_exit(1);
        exec { $command[0] } @command or POSIX::_exit(127);
    }
    # The child is not reaped before its start is read.
    my $start = start($pid);
    my ($sent, $turns, $status) = (0, 0, 0);
    while (1) {
        if (waitpid($pid, WNOHANG) == $pid) {
            $status = $?;
            touch("$base.reaped");
            last;
        }
        # TERM, then KILL, go only to this unreaped child.
        if (!$sent && stopped($claim, $n)) {
            kill('TERM', $pid);
            $sent = 1;
        } elsif ($sent && ++$turns == 30) {
            kill('KILL', $pid);
        }
        sleep 1;
    }
    my $in = 1;
    for (1 .. 12) {
        bounded(30, $etude, 'status', $ledger);
        $in = listed($pid, $start);
        last if defined $in && !$in;
        sleep 5;
    }
    if (!defined $in) {
        create("$base.held",
               "$ledger/claims cannot be read, so the end of process $pid is not confirmed\n");
    } elsif ($in) {
        create("$base.held",
               "process $pid keeps a claim or request in $ledger/claims; a person checks it and removes it\n");
    }
    create("$base.rc", ($status & 127 ? 128 + ($status & 127) : $status >> 8) . "\n");
    return 0;
}

# claim_hold LOCKS OUT ID KIND N DONE: the holder, as the header says.
sub claim_hold
{
    my ($locks, $out, $id, $kind, $n, $done) = @_;
    my $claim = "$locks/$id.$kind";
    my $granted = take("$claim.$n.granted") or die "claim: cannot lock $claim.$n.granted\n";
    my $alive = "$out/$id.alive";
    my $size = (stat($alive))[7] // -1;
    my $still = 0;
    my $why;
    while (1) {
        if (stopped($claim, $n)) {
            $why = 'the claim was stopped';
        } elsif (-e "$out/$done") {
            $why = "$done exists";
        } else {
            my $now = (stat($alive))[7] // -1;
            if ($now != $size) {
                ($size, $still) = ($now, 0);
            } elsif (++$still >= 120) {
                $why = "$id.alive did not grow in 120 looks";
            }
        }
        last if defined $why;
        sleep 5;
    }
    print "claim: attempt $n ends its hold: $why\n";
    return 0;
}

# claim_state LOCKS ID KIND N: print the state of attempt N, as the header says.
sub claim_state
{
    my ($locks, $id, $kind, $n) = @_;
    my $base = "$locks/$id.$kind.$n";
    my $live = held("$base.live");
    my $grant = held("$base.granted");
    return 0 unless defined $live && defined $grant;
    if ($grant && !-e "$base.reaped") {
        print "granted\n";
    } elsif ($live || $grant) {
        # A wait, or the runner's cleanup after its launcher.
    } elsif (-e "$base.granted") {
        print "dead\n";
    } elsif (-e "$base.rc") {
        print "refused\n";
    } elsif (!-e "$base.go") {
        print "unstarted\n";
    } else {
        print "dead\n";
    }
    return 0;
}

# claim_stop LOCKS ID KIND [N]: end the claim, or its attempt N, as the header
# says.
sub claim_stop
{
    my ($locks, $id, $kind, $n) = @_;
    my $claim = "$locks/$id.$kind";
    my $tomb = defined $n ? "$claim.$n.stop" : "$claim.stop";
    make_path($locks);
    if (!touch($tomb)) {
        print "cannot create $tomb\n";
        return 1;
    }
    my @attempts = defined $n ? ($n) : do {
        my %seen;
        my @numbers = map { /^\Q$claim\E\.(\d+)\./ ? $1 : () } glob("$claim.*");
        grep { !$seen{$_}++ } sort { $a <=> $b } @numbers;
    };
    my $turns = 0;
    for my $t (@attempts) {
        for my $file ("$claim.$t.live", "$claim.$t.granted") {
            while (1) {
                my $h = held($file);
                if (!defined $h) {
                    print "cannot probe $file\n";
                    return 1;
                }
                last unless $h;
                if (++$turns > 150) {
                    print "an attempt of the claim still holds $file after two and a half minutes\n";
                    return 1;
                }
                sleep 1;
            }
        }
    }
    my $rc = bounded(60, $etude, 'status', $ledger);
    if (!defined $rc || $rc != 0) {
        print "etude status could not recover $ledger, so the end of the claim is not confirmed\n";
        return 1;
    }
    for my $t (@attempts) {
        my $o = "$claim.$t.owner";
        next unless -e $o;
        open(my $f, '<', $o) or do { print "cannot read $o\n"; return 1 };
        my ($pid, $start) = split(' ', <$f> // '');
        if (!defined $pid || $pid !~ /^\d+$/) {
            print "cannot read $o\n";
            return 1;
        }
        my $in = listed($pid, $start // 'x');
        if (!defined $in) {
            print "cannot read $ledger/claims\n";
            return 1;
        }
        if ($in) {
            print "process $pid keeps a claim or request in $ledger/claims; ",
                "a person checks it and removes it\n";
            return 1;
        }
    }
    my $kept = 0;
    for my $t (@attempts) {
        open(my $f, '<', "$claim.$t.held") or next;
        print while <$f>;
        $kept = 1;
    }
    return $kept;
}

my %modes = (run => [\&claim_run, 6], hold => [\&claim_hold, 6], state => [\&claim_state, 4], stop => [\&claim_stop, 3]);
my $mode = shift(@ARGV) // '';
my $m = $modes{$mode};
die "usage: claim.pl run|hold|state|stop LOCKS ...\n" unless $m && @ARGV >= $m->[1];
exit $m->[0]->(@ARGV);
