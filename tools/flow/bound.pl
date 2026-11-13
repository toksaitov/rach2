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
# Both signals go to the command only while waitpid has not returned it, so
# that they cannot reach a process that took its number. No clock is read: a
# suspended host stretches the bound and never shortens it. ssh, which the
# callers bound, ignores the SIGALRM by which an alarm would bound it, and
# the KILL here does not depend on it.
#
# The status is the command's, 128 plus the signal's number when a signal
# ended it, 137 when this process killed it, and 127 when it could not be
# started.

use strict;
use warnings;
use POSIX qw(WNOHANG);

my ($limit, @command) = @ARGV;
defined $limit && $limit =~ /^\d+$/ && @command or die "usage: bound.pl SECONDS COMMAND...\n";
my $pid = fork();
defined $pid or die "bound: cannot fork: $!\n";
if ($pid == 0) {
    exec { $command[0] } @command or die "bound: cannot run $command[0]: $!\n";
}
my $reaped = 0;
my $status;
# reap: return whether the command has ended, collecting its status once.
sub reap
{
    return 1 if $reaped;
    if (waitpid($pid, WNOHANG) == $pid) {
        $status = $?;
        $reaped = 1;
    }
    return $reaped;
}
for my $name (qw(HUP INT TERM)) {
    $SIG{$name} = sub { kill($name, $pid) unless reap() };
}
for (1 .. $limit * 10) {
    last if reap();
    select(undef, undef, undef, 0.1);
}
unless (reap()) {
    kill('KILL', $pid);
    waitpid($pid, 0);
    print STDERR "bound: $command[0] ran longer than $limit seconds and was killed\n";
    exit 137;
}
exit($status & 127 ? 128 + ($status & 127) : $status >> 8);
