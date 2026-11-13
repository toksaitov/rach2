# session.pl: run a loop's session in a process group of its own, record the
# group before the session starts, and stop the group at its time limit or
# when the supervisor is stopped.
#
# Usage: perl session.pl RECORD LOG LIMIT COMMAND...
#
# The supervisor, this process, first takes RECORD.live, a lock that it holds
# exclusively for its whole life and that no child keeps, so that a looker
# tells a live supervisor from a RECORD that a killed one left by probing the
# lock, never by a number; a supervisor that cannot take it, since another
# holds it, exits 125 having started nothing. A RECORD that exists once it
# holds the lock was left by a supervisor that was killed, whose session may
# still run: it removes RECORD only once the boot RECORD names is not this
# one, or a ps that succeeds shows no process in the group RECORD names, and
# otherwise exits 125 having started nothing, as the loop's short-session
# wait then tries again. It then leaves the loop's process group, so a hangup
# that ends the loop reaches neither it nor the session. It forks the group's
# leader, the anchor: a child that leads a group of its own, whose id is the
# anchor's pid, ignores HUP, INT and TERM, and exits once the supervisor
# closes the pipe to it or dies. It then forks the session into that group
# and writes `GROUP BOOT` to RECORD, the group's id and the ID of this boot,
# /proc/sys/kernel/random/boot_id on Linux and `sysctl -n
# kern.bootsessionuuid` on the Mac, or `-` when it cannot be read. Only then,
# with the limit set, does it let the session exec COMMAND: a session runs
# only once its group is recorded and its limit is set, and when the record
# cannot be written, or the supervisor dies first, the child exits without
# running COMMAND, as it does when a HUP, INT, TERM or RECORD.stop came
# before its go; the supervisor then exits as for a stop, below.
#
# Every signal to the group goes while the anchor is an unreaped child of
# the supervisor, so the group's number is still the anchor's and has passed
# to no other group: the supervisor reaps the anchor only once ps shows no
# other process in the group, and signals the group no more after that. Its
# handlers only record a signal; one loop, a turn each second, reaps the
# session and sends every signal.
#
# A session still running after LIMIT seconds gets TERM, its whole group, and
# KILL thirty turns later if any of it is left, so that a child that outlives
# the TERM, such as an emulator, does not outlive the session, and the
# supervisor exits 124. A HUP, INT or TERM sent to the supervisor stops the
# session's group the same way, and the supervisor exits 128 plus the signal's
# number only once the group is gone, so that a stop of either group stops the
# session before the loop can start another; only a KILL can end the
# supervisor first, which leaves RECORD. So does RECORD.stop, which stop.sh
# leaves so that no signal of its own goes to the supervisor, whose number
# only the loop holds; the supervisor then exits 143, and the loop removes the
# file before it starts the next session. A session that ends by itself gives
# its status, as sh reports one, and the processes it left in its group are
# logged to LOG and stopped the same way: a session should leave none, and one
# it leaves, such as a `tests/run` that holds the clone's run lock, would keep
# the next session's suite from running. A process that outlives the KILL
# keeps the supervisor, the anchor and RECORD until it is gone, as LOG notes.
# The supervisor removes RECORD once the group is gone. It exits 125 when it
# could not start the session.

use strict;
use warnings;
use Fcntl qw(:flock O_CREAT O_RDWR);
use POSIX qw(strftime WNOHANG);

my ($record, $log, $limit, @command) = @ARGV;

# Append TEXT to LOG with the time in UTC, as the loop's `log` does.
sub note
{
    my $text = shift;
    open(my $f, '>>', $log) or return;
    print $f strftime('%F %T', gmtime), " $text\n";
    close($f);
}

# boot: return the ID of this boot, as the header says, or `-`.
sub boot
{
    my $b;
    if (open(my $f, '<', '/proc/sys/kernel/random/boot_id')) {
        $b = <$f>;
    } elsif ($^O eq 'darwin') {
        $b = `/usr/sbin/sysctl -n kern.bootsessionuuid 2> /dev/null`;
        $b = undef if $? != 0;
    }
    chomp $b if defined $b;
    return defined $b && $b =~ /^\S+$/ ? $b : '-';
}

# Write `GROUP BOOT` to RECORD through a new file and a rename, so that a
# reader finds the old record or the whole new one; return whether all of it
# worked.
sub save
{
    my $group = shift;
    open(my $f, '>', "$record.new") or return 0;
    print $f "$group ", boot(), "\n" or return 0;
    close($f) or return 0;
    return rename("$record.new", $record);
}

# gone GROUP BOOT: return whether nothing of the session that a killed
# supervisor recorded as GROUP BOOT can still run, as the header says.
sub gone
{
    my ($group, $then) = @_;
    my $now = boot();
    return 1 if defined $then && $then ne '-' && $now ne '-' && $then ne $now;
    return 0 unless defined $group && $group =~ /^\d+$/;
    my @ps = `ps -A -o pgid=`;
    return 0 if $? != 0;
    return !grep { /^\s*(\d+)\s*$/ && $1 == $group } @ps;
}

my ($signal, $expired);
for my $name ('HUP', 'TERM', 'INT') {
    $SIG{$name} = sub { $signal //= $name };
}
$SIG{ALRM} = sub { $expired = 1 };
my %number = (HUP => POSIX::SIGHUP, INT => POSIX::SIGINT, TERM => POSIX::SIGTERM);

# The lock, which Perl closes on exec and the anchor closes at its start.
my $live;
if (!sysopen($live, "$record.live", O_RDWR | O_CREAT, 0644) || !flock($live, LOCK_EX | LOCK_NB)) {
    note("$record.live cannot be taken: $!; another supervisor may run; not starting the session");
    exit 125;
}

# A record that a killed supervisor left.
if (-e $record) {
    my ($group, $then);
    if (open(my $f, '<', $record)) {
        ($group, $then) = split(' ', <$f> // '');
    }
    if (!gone($group, $then)) {
        note("$record names group " . ($group // '?') . " of a supervisor that was killed, which " .
            "may still run; not starting the session");
        exit 125;
    }
    note("the group " . ($group // '?') . " that $record names is gone; removing it");
    if (!unlink($record)) {
        note("$record cannot be removed: $!; not starting the session");
        exit 125;
    }
}

setpgrp(0, 0) or die "session: setpgrp: $!\n";
pipe(my $hold_r, my $hold_w) or die "session: pipe: $!\n";
my $group = fork;
defined $group or die "session: fork: $!\n";
if ($group == 0) {
    # The anchor: it leads the group and waits for the end of the pipe.
    close($hold_w);
    close($live);
    $SIG{$_} = 'IGNORE' for qw(HUP INT TERM ALRM);
    setpgrp(0, 0);
    while (1) {
        my $n = sysread($hold_r, my $byte, 1);
        last if defined $n ? $n == 0 : !$!{EINTR};
    }
    POSIX::_exit(0);
}
close($hold_r);

# end STATUS: close the anchor's pipe, reap the anchor and exit with STATUS.
sub end
{
    close($hold_w);
    waitpid($group, 0);
    exit shift;
}

if (!setpgrp($group, $group)) {
    note("session group $group cannot be made: $!; not starting it");
    end(125);
}
pipe(my $go_r, my $go_w) or die "session: pipe: $!\n";
my $pid = fork;
if (!defined $pid) {
    note("session cannot fork: $!; not starting it");
    end(125);
}
if ($pid == 0) {
    close($go_w);
    close($hold_w);
    $SIG{$_} = 'DEFAULT' for qw(HUP TERM INT ALRM);
    setpgrp(0, $group);
    sysread($go_r, my $go, 1) == 1 or POSIX::_exit(125);
    close($go_r);
    exec { $command[0] } @command or die "session: exec $command[0]: $!\n";
}
close($go_r);
if (!setpgrp($pid, $group) || !save($group)) {
    my $why = $!;
    note("session group $group cannot be recorded in $record: $why; " .
        "not starting it");
    unlink("$record.new");
    # Without its go the session exits 125 and runs nothing.
    close($go_w);
    waitpid($pid, 0);
    end(125);
}

# members: return a reference to the list of `PID ARGS` of each process in
# the group but the anchor, or nothing when ps cannot be read.
sub members
{
    my @ps = `ps -A -o pgid= -o pid= -o args=`;
    return if $? != 0;
    return [map { /^\s*(\d+)\s+(\d+)\s+(.*)$/ && $1 == $group && $2 != $group ? "$2 $3" : () } @ps];
}

# A stop that came before the go starts nothing.
if (defined $signal || -e "$record.stop") {
    close($go_w);
    waitpid($pid, 0);
    unlink($record);
    note("session group $group stopped before it started");
    end(128 + $number{$signal // 'TERM'});
}
alarm($limit);
syswrite($go_w, 'x', 1);
close($go_w);
my ($status, $stopping, $left, $asked);
my $turns = 0;
while (1) {
    if (!defined $status && waitpid($pid, WNOHANG) == $pid) {
        $status = $?;
        alarm(0) unless $stopping || $expired;
    }
    $asked = 1 if !$stopping && -e "$record.stop";
    if (!$stopping && (defined $signal || $expired || $asked)) {
        $stopping = 1;
    } elsif (!$stopping && defined $status) {
        $left = members();
        last if $left && !@$left;
        note("session group $group left $_; stopping it") for @{$left || []};
        $stopping = 1;
    }
    if ($stopping) {
        # The anchor is an unreaped child, so -$group is still its group.
        if ($turns == 0) {
            kill('TERM', -$group);
        } else {
            $left = members();
            last if $left && !@$left;
        }
        if ($turns == 30) {
            note("session group $group outlived the TERM; killing it");
            kill('KILL', -$group);
        } elsif ($turns == 60) {
            note("session group $group outlived its KILL; waiting for it to end");
        }
        $turns++;
    }
    select(undef, undef, undef, 1);
}
alarm(0);
unlink($record);
if (defined $signal) {
    note("session group $group stopped on SIG$signal");
    end(128 + $number{$signal});
}
end(124) if $expired;
if ($asked) {
    note("session group $group stopped on $record.stop");
    end(128 + POSIX::SIGTERM);
}
end($status & 127 ? 128 + ($status & 127) : $status >> 8);
