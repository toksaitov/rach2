#!/usr/bin/perl
#
# bound.pl: run a command for a bounded number of counted seconds, and kill
# it, its own unreaped child, when it runs longer.
#
# Usage: perl bound.pl SECONDS COMMAND...
#
# It forks COMMAND, with this process's standard input, output and errors,
# and waits for it, counting SECONDS turns of one second, each ten looks a
# tenth of a second apart; a command still running after the last turn gets
# KILL. A HUP, INT or TERM that this process takes goes on to the command.
# The handlers only record a signal; one loop reaps the command and sends
# each signal, and sends one only after a look in the same turn has found
# the command not yet reaped, so that no signal reaches a process that took
# its number: until this process reaps it, the command's number stays its
# own, as a zombie's does. No clock is read: a suspended host stretches the
# bound and never shortens it. ssh, which the callers bound, ignores the
# SIGALRM by which an alarm would bound it, and the KILL here does not
# depend on it.
#
# The status is the command's, 128 plus the signal's number when a signal
# ended it, 137 when this process killed it, and 127 when it could not be
# started.

use strict;
use warnings;
use POSIX qw(WNOHANG);

my ($limit, @command) = @ARGV;
defined $limit && $limit =~ /^\d+$/ && @command or die "usage: bound.pl SECONDS COMMAND...\n";
my @pending;
for my $name (qw(HUP INT TERM)) {
    $SIG{$name} = sub { push(@pending, $name) };
}
my $pid = fork();
defined $pid or die "bound: cannot fork: $!\n";
if ($pid == 0) {
    $SIG{$_} = 'DEFAULT' for qw(HUP INT TERM);
    no warnings 'exec';
    exec { $command[0] } @command;
    print STDERR "bound: cannot run $command[0]: $!\n";
    POSIX::_exit(127);
}
my $status;
my $looks = 0;
while (1) {
    if (waitpid($pid, WNOHANG) == $pid) {
        $status = $?;
        last;
    }
    # The command is not reaped, and only this loop reaps it.
    if ($looks >= $limit * 10) {
        kill('KILL', $pid);
        waitpid($pid, 0);
        print STDERR "bound: $command[0] ran longer than $limit seconds and was killed\n";
        exit 137;
    }
    kill(shift(@pending), $pid) while @pending;
    select(undef, undef, undef, 0.1);
    $looks++;
}
exit($status & 127 ? 128 + ($status & 127) : $status >> 8);
