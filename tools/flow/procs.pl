#!/usr/bin/perl
#
# procs.pl: list the processes of a Linux machine that a selection names,
# from /proc, and with -k send each TERM through a pidfd.
#
# Usage: perl procs.pl [-k] [-c] [-e REGEX] [-v REGEX] [-d DIR] [-D DIR]
#                      [-u USER]
#
# A process is selected when its command line, its arguments joined by spaces,
# matches the Perl REGEX of -e and not that of -v; its working directory is
# DIR of -d or below it, and not DIR of -D or below it; and its user is not
# USER of -u; each option that is not given selects every process. With -c a
# process whose session's leader is a perl that runs a claim's script,
# LOCKS/ID.KIND.N.pl with `run`, as release.sh's claim_start starts one, the
# first three words of its command line, is never selected. Each selected
# process is a line `PID START USER ARGS`, START being field 22 of its
# proc(5) stat, its start in clock ticks after the boot, which the admission
# ledger keeps beside an owner's number, so that a caller matches the ledger
# by both and never by a number alone. Kernel threads, which have no command
# line, and this process are never selected.
#
# Each process is read through a pidfd that this process opens first: its
# command line, session, user and directory come from /proc, and a signal 0
# through the pidfd then shows that the process the pidfd names still runs,
# by its success or, for another user's process, its EPERM,
# so that what was read was its own, since its number cannot have passed to
# another process meanwhile. With -k the TERM goes through the same pidfd,
# which names that process and no other, so no signal reaches a process
# that took the number. The scan is strict: a process that ended while it
# was read, which the pidfd's open or signal 0 tells by ESRCH, is skipped,
# and every other error, a pidfd that cannot be opened, a file of a live
# process that cannot be read, a directory that cannot be read when -d or -D
# selects by it, or a TERM that fails but with ESRCH, ends the scan with a
# line `procs: WHY` on stderr and status 1, since a partial list would read
# as nothing left. The status is 0 once every process was read and every
# TERM sent.

use strict;
use warnings;
use Getopt::Std;
use POSIX ();

# The system calls' numbers, the same on every Linux architecture but alpha.
my ($PIDFD_OPEN, $PIDFD_SEND_SIGNAL) = (434, 424);

my %o;
getopts('kce:v:d:D:u:', \%o) && !@ARGV
    or die "usage: procs.pl [-k] [-c] [-e REGEX] [-v REGEX] [-d DIR] [-D DIR] [-u USER]\n";

# slurp PATH: return the whole of the file PATH, or nothing with $! set.
sub slurp
{
    my $path = shift;
    open(my $f, '<', $path) or return;
    local $/;
    my $s = <$f>;
    defined $s or return;
    close($f);
    return $s;
}

# fail WHY: end the scan with WHY, as the header says.
sub fail
{
    print STDERR "procs: $_[0]\n";
    exit 1;
}

# gone FD: succeed when the process the pidfd FD names has ended; fail the
# scan when that cannot be read. A signal 0 refused with EPERM names another
# user's process that runs, since the kernel checks the permission only on a
# process that has not ended.
sub gone
{
    my $fd = shift;
    return 0 if syscall($PIDFD_SEND_SIGNAL, $fd, 0, 0, 0) == 0 || $!{EPERM};
    return 1 if $!{ESRCH};
    fail("a pidfd cannot be signalled: $!");
}

# under PATH DIR: whether PATH is DIR or below it.
sub under
{
    my ($path, $dir) = @_;
    return defined $path && ($path eq $dir || index($path, "$dir/") == 0);
}

opendir(my $d, '/proc') or fail("/proc cannot be read: $!");
my (%claims, @chosen);
for my $pid (grep { /^\d+$/ && $_ != $$ } readdir($d)) {
    my $fd = syscall($PIDFD_OPEN, $pid + 0, 0);
    if ($fd < 0) {
        next if $!{ESRCH};
        fail("pidfd_open of $pid: $!");
    }
    my $cmdline = slurp("/proc/$pid/cmdline");
    my $stat = slurp("/proc/$pid/stat");
    my $status = slurp("/proc/$pid/status");
    my $cwd = defined $o{d} || defined $o{D} ? readlink("/proc/$pid/cwd") : '';
    # What was read is the process's own only while it still runs; a read
    # that failed for a process that still runs fails the scan.
    if (gone($fd)) {
        POSIX::close($fd);
        next;
    }
    defined $cmdline && defined $stat && defined $status or
        fail("process $pid cannot be read whole: $!");
    # A kernel thread, and a zombie, have no command line.
    if ($cmdline eq '') {
        POSIX::close($fd);
        next;
    }
    defined $cwd or fail("the directory of process $pid cannot be read: $!");
    my $args = join(' ', split(/\0/, $cmdline));
    my @words = split(' ', $args);
    $stat =~ s/.*\) //s;
    my @f = split(' ', $stat);
    my ($sid, $start) = @f[3, 19];
    my ($uid) = $status =~ /^Uid:\s+(\d+)/m;
    my $user = defined $uid ? (getpwuid($uid))[0] // $uid : '?';
    if ($pid == $sid && @words >= 3 && $words[0] eq 'perl' && $words[2] eq 'run' &&
        $words[1] =~ m{/build/flow/claims/[^/]+\.(claim|hold)\.[0-9]+\.pl$}) {
        $claims{$sid} = 1;
    }
    if ((!defined $o{e} || $args =~ /$o{e}/) && (!defined $o{v} || $args !~ /$o{v}/) &&
        (!defined $o{d} || under($cwd, $o{d})) && (!defined $o{D} || !under($cwd, $o{D})) &&
        (!defined $o{u} || $user ne $o{u})) {
        push(@chosen, [$pid, $fd, $sid, $start, $user, $args]);
    } else {
        POSIX::close($fd);
    }
}
closedir($d);
for my $p (@chosen) {
    my ($pid, $fd, $sid, $start, $user, $args) = @$p;
    if (!$o{c} || !$claims{$sid}) {
        if ($o{k} && syscall($PIDFD_SEND_SIGNAL, $fd, 15, 0, 0) < 0 && !$!{ESRCH}) {
            fail("TERM to process $pid: $!");
        }
        print "$pid $start $user $args\n";
    }
    POSIX::close($fd);
}
exit 0;
