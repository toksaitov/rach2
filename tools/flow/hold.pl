#!/usr/bin/perl
#
# hold.pl: hold a job unreaped and send it TERM once DIR/stop exists, so that
# a stop reaches the job and never a process that took its number.
#
# Usage: perl hold.pl DIR -p PID
#        perl hold.pl [-g] DIR COMMAND...
#
# With -p, the job PID is already this process's child: release.sh's `held`
# starts the job from its subshell and then execs this program in the
# subshell's place. Otherwise this process forks COMMAND as the job, with
# the signal dispositions this process started with. With -g it first leads
# a process group of its own, which COMMAND joins: the group's number is
# this process's, so it passes to no other group while this process runs,
# and this process exits only once ps shows nothing else in the group.
#
# One loop, a look each tenth of a second, reaps the job and sends the stop,
# once, when DIR/stop exists: to the job only after a look in the same turn
# has found it not yet reaped, since only this loop reaps it and until then
# its number stays its own, as a zombie's does; with -g to the whole group,
# which this process leads, at any time. HUP, INT and TERM are ignored here,
# so that a signal meant for the job's group cannot end this process before
# the job, and no other signal is sent.
#
# Once the job is reaped, and with -g the group has no other process, it
# writes the job's status, as sh reports one, to DIR/end, through a new file
# and a rename, and exits with it. When the job PID is not this process's
# child, as when bash reaped a job that ended before the exec, nothing is
# signalled and the status is 127. DIR is the caller's, made before.

use strict;
use warnings;
use POSIX qw(WNOHANG);

my $group = @ARGV && $ARGV[0] eq '-g' ? shift : '';
my ($dir, @command) = @ARGV;
my $usage = "usage: hold.pl DIR -p PID | hold.pl [-g] DIR COMMAND...\n";
defined $dir && -d $dir && @command or die $usage;
my $pid;
if ($command[0] eq '-p') {
    !$group && @command == 2 && $command[1] =~ /^\d+$/ or die $usage;
    $pid = $command[1];
}

# The dispositions COMMAND gets, as this process found them.
my %start = map { ($_ => $SIG{$_} // 'DEFAULT') } qw(HUP INT TERM);
$SIG{$_} = 'IGNORE' for qw(HUP INT TERM);

if (!defined $pid) {
    setpgrp(0, 0) or die "hold: setpgrp: $!\n" if $group;
    $pid = fork();
    defined $pid or die "hold: cannot fork: $!\n";
    if ($pid == 0) {
        $SIG{$_} = $start{$_} for keys %start;
        no warnings 'exec';
        exec { $command[0] } @command;
        print STDERR "hold: cannot run $command[0]: $!\n";
        POSIX::_exit(127);
    }
}

# end CODE: write CODE to DIR/end and exit with it.
sub end
{
    my $code = shift;
    if (open(my $f, '>', "$dir/end.new")) {
        print $f "$code\n";
        close($f) and rename("$dir/end.new", "$dir/end");
    }
    exit $code;
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

my ($sent, $code);
while (!defined $code) {
    my $r = waitpid($pid, WNOHANG);
    if ($r == $pid) {
        $code = $? & 127 ? 128 + ($? & 127) : $? >> 8;
        last;
    }
    if ($r < 0) {
        print STDERR "hold: job $pid is not this process's child\n";
        end(127);
    }
    # The job is not reaped, and only this loop reaps it.
    if (!$sent && -e "$dir/stop") {
        $sent = $group ? kill('TERM', -$$) : kill('TERM', $pid);
    }
    select(undef, undef, undef, 0.1);
}
if ($group) {
    # The group's number is this process's own, so a stop may still go.
    while (1) {
        my $n = others();
        last if defined $n && $n == 0;
        $sent = kill('TERM', -$$) if !$sent && -e "$dir/stop";
        select(undef, undef, undef, 1);
    }
}
end($code);
