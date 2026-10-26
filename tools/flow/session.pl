# session.pl: run a loop's session in a process group of its own, record the
# group before the session starts, and stop the group at its time limit or
# when the supervisor is stopped.
#
# Usage: perl session.pl RECORD LOG LIMIT COMMAND...
#
# The supervisor, this process, leaves the loop's process group first, so a
# hangup that ends the loop reaches neither it nor the session. It forks the
# session, puts it in a group of its own whose id is the session's pid, and
# writes that id to RECORD. Only then, with the limit set, does it let the
# session exec COMMAND: a session runs only once its group is recorded and
# its limit is set, and when the record cannot be written, or the
# supervisor dies first, the child exits without running COMMAND.
#
# A session still running after LIMIT seconds gets TERM, its whole group KILL
# half a minute later if any of it is left, so that a child that outlives the
# TERM, such as an emulator, does not outlive the session, and the supervisor
# exits 124. A TERM or INT sent to the supervisor stops the session's group
# the same way, and the supervisor exits 128 plus the signal's number only
# once the group is gone, so that a stop of either group stops the session
# before the loop can start another. A session that ends by itself gives its
# status, as sh reports one, and the processes it left in its group are logged
# to LOG and stopped the same way: a session should leave none, and one it
# leaves, such as a `tests/run` that holds the clone's run lock, would keep
# the next session's suite from running. The supervisor removes RECORD once
# the group is gone. It exits 125 when it could not start the session.

use strict;
use warnings;
use POSIX qw(strftime);

my ($record, $log, $limit, @command) = @ARGV;

# Append TEXT to LOG with the time in UTC, as the loop's `log` does.
sub note
{
    my $text = shift;
    open(my $f, '>>', $log) or return;
    print $f strftime('%F %T', gmtime), " $text\n";
    close($f);
}

# Wait up to SECONDS for the group GROUP to have no process left; return
# whether it has none.
sub drained
{
    my ($group, $seconds) = @_;
    for (1 .. $seconds) {
        return 1 unless kill 0, -$group;
        sleep 1;
    }
    return !kill 0, -$group;
}

# Write GROUP to RECORD through a new file and a rename, so that a reader
# finds the old record or the whole new one; return whether all of it worked.
sub save
{
    my $group = shift;
    open(my $f, '>', "$record.new") or return 0;
    print $f "$group\n" or return 0;
    close($f) or return 0;
    return rename("$record.new", $record);
}

setpgrp(0, 0) or die "session: setpgrp: $!\n";
pipe(my $go_r, my $go_w) or die "session: pipe: $!\n";
my $pid = fork;
defined $pid or die "session: fork: $!\n";
if ($pid == 0) {
    close($go_w);
    setpgrp(0, 0);
    sysread($go_r, my $go, 1) == 1 or POSIX::_exit(125);
    close($go_r);
    exec { $command[0] } @command or die "session: exec $command[0]: $!\n";
}
close($go_r);
if (!setpgrp($pid, $pid) || !save($pid)) {
    my $why = $!;
    note("session group $pid cannot be recorded in $record: $why; " .
        "not starting it");
    unlink("$record.new");
    kill 'KILL', $pid;
    waitpid($pid, 0);
    exit 125;
}

my $expired = 0;
my $signal;
my $deadline;

# Send TERM to the session's group and arrange for KILL half a minute later;
# a second call does nothing, since the first set the deadline.
sub stop
{
    return if defined $deadline;
    $deadline = time + 30;
    kill 'TERM', -$pid;
    alarm(30);
}

$SIG{ALRM} = sub {
    if (defined $deadline) {
        kill 'KILL', -$pid;
    } else {
        $expired = 1;
        stop();
    }
};
for my $name ('TERM', 'INT') {
    $SIG{$name} = sub {
        $signal = $name unless defined $signal;
        stop();
    };
}
alarm($limit);
syswrite($go_w, 'x', 1);
close($go_w);
my $status;
while (1) {
    my $ended = waitpid($pid, 0);
    if ($ended == $pid) {
        $status = $?;
        last;
    }
    die "session: waitpid: $!\n" unless $!{EINTR};
}
alarm(0) unless defined $deadline;

if (!defined $deadline && kill 0, -$pid) {
    for (`ps -A -o pgid= -o pid= -o args=`) {
        note("session group $pid left $2; stopping it")
            if /^\s*(\d+)\s+(.*)$/ && $1 == $pid;
    }
    stop();
}
if (defined $deadline) {
    if (!drained($pid, $deadline - time)) {
        note("session group $pid outlived the TERM; killing it");
        kill 'KILL', -$pid;
    }
    alarm(0);
}
unlink($record);
if (defined $signal) {
    note("session group $pid stopped on SIG$signal");
    exit(128 + ($signal eq 'TERM' ? POSIX::SIGTERM : POSIX::SIGINT));
}
exit 124 if $expired;
exit($status & 127 ? 128 + ($status & 127) : $status >> 8);
