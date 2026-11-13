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
# process is a line `PID USER ARGS`. Kernel threads, which have no command
# line, and this process are never selected.
#
# Each process is read through a pidfd that this process opens first: its
# command line, session, user and directory come from /proc, and a signal 0
# through the pidfd then shows that the process the pidfd names still runs,
# so that what was read was its own, since its number cannot have passed to
# another process meanwhile. With -k the TERM goes through the same pidfd,
# which names that process and no other, so no signal reaches a process
# that took the number. The status is 1 when /proc cannot be read or the
# kernel has no pidfd, and 0 otherwise.

use strict;
use warnings;
use Getopt::Std;
use POSIX ();

# The system calls' numbers, the same on every Linux architecture but alpha.
my ($PIDFD_OPEN, $PIDFD_SEND_SIGNAL) = (434, 424);

my %o;
getopts('kce:v:d:D:u:', \%o) && !@ARGV
    or die "usage: procs.pl [-k] [-c] [-e REGEX] [-v REGEX] [-d DIR] [-D DIR] [-u USER]\n";

# slurp PATH: return the whole of the file PATH, or nothing.
sub slurp
{
    my $path = shift;
    open(my $f, '<', $path) or return;
    local $/;
    my $s = <$f>;
    close($f);
    return $s;
}

# under PATH DIR: whether PATH is DIR or below it.
sub under
{
    my ($path, $dir) = @_;
    return defined $path && ($path eq $dir || index($path, "$dir/") == 0);
}

opendir(my $d, '/proc') or exit 1;
my (%claims, @chosen);
for my $pid (grep { /^\d+$/ && $_ != $$ } readdir($d)) {
    my $fd = syscall($PIDFD_OPEN, $pid + 0, 0);
    if ($fd < 0) {
        exit 1 if $!{ENOSYS};
        next;
    }
    my $cmdline = slurp("/proc/$pid/cmdline");
    my $stat = slurp("/proc/$pid/stat");
    my $status = slurp("/proc/$pid/status");
    my $cwd = readlink("/proc/$pid/cwd");
    # What was read is the process's own only while it still runs.
    if (!$cmdline || !defined $stat || !defined $status ||
        syscall($PIDFD_SEND_SIGNAL, $fd, 0, 0, 0) < 0) {
        POSIX::close($fd);
        next;
    }
    my $args = join(' ', split(/\0/, $cmdline));
    my @words = split(' ', $args);
    $stat =~ s/.*\) //s;
    my $sid = (split(' ', $stat))[3];
    my ($uid) = $status =~ /^Uid:\s+(\d+)/m;
    my $user = defined $uid ? (getpwuid($uid))[0] // $uid : '?';
    if ($pid == $sid && @words >= 3 && $words[0] eq 'perl' && $words[2] eq 'run' &&
        $words[1] =~ m{/build/flow/claims/[^/]+\.(claim|hold)\.[0-9]+\.pl$}) {
        $claims{$sid} = 1;
    }
    if ((!defined $o{e} || $args =~ /$o{e}/) && (!defined $o{v} || $args !~ /$o{v}/) &&
        (!defined $o{d} || under($cwd, $o{d})) && (!defined $o{D} || !under($cwd, $o{D})) &&
        (!defined $o{u} || $user ne $o{u})) {
        push(@chosen, [$pid, $fd, $sid, $user, $args]);
    } else {
        POSIX::close($fd);
    }
}
for my $p (@chosen) {
    my ($pid, $fd, $sid, $user, $args) = @$p;
    if (!$o{c} || !$claims{$sid}) {
        syscall($PIDFD_SEND_SIGNAL, $fd, 15, 0, 0) if $o{k};
        print "$pid $user $args\n";
    }
    POSIX::close($fd);
}
exit 0;
