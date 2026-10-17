#!/usr/bin/perl
#
# scratch.pl: remove the scratch that tests left in a directory when they were
# killed before their exit traps ran.
#
# Usage: perl scratch.pl [DIR [SECONDS]]
#
# The manager loop runs this before each check, as root in WSL on the testing
# host, for /tmp, whose tmpfs is the host's memory. An entry of DIR, /tmp
# unless given, is removed when all of these hold:
#
# * its name has the form that mktemp(1) and mkdtemp(3) give, a name, a dot
#   and six or more letters, digits or underscores, which the lock files
#   rach2-ports.N, rach2-host.lock and rach2-host.gate do not have;
# * it is a directory or a regular file, and root does not own it;
# * nothing in it has changed for SECONDS, a day unless given;
# * nothing is mounted in it, and no process has its working directory or an
#   open file in it, as /proc shows where it exists.
#
# It prints one line: how many entries it removed and their size.

use strict;
use warnings;
use File::Find;
use File::Path qw(remove_tree);

my $dir = shift // '/tmp';
my $age = shift // 86400;
my $now = time;

my @busy;
for my $p (glob '/proc/[0-9]*') {
    for my $l ("$p/cwd", glob "$p/fd/*") {
        my $t = readlink $l;
        push @busy, $t if defined $t;
    }
}
if (open my $m, '<', '/proc/self/mounts') {
    push @busy, map { (split ' ')[1] } <$m>;
} else {
    push @busy, map { /^\S+ on (.+?) (?:type |\()/ ? $1 : () } `mount`;
}

# fresh PATH: whether PATH or anything under it changed within $age seconds.
sub fresh {
    my $young = 0;
    find({ no_chdir => 1, wanted => sub {
        my @s = lstat $_;
        if (!@s || $now - $s[9] < $age) {
            $young = 1;
            $File::Find::prune = 1;
        }
    } }, shift);
    return $young;
}

# size PATH: the bytes that the files under PATH take on their filesystem,
# which for a sparse disk image is less than its length.
sub size {
    my $n = 0;
    find({ no_chdir => 1, wanted => sub {
        my @s = lstat $_;
        $n += $s[12] * 512 if @s && -f _;
    } }, shift);
    return $n;
}

opendir my $d, $dir or die "scratch: $dir: $!\n";
my ($count, $bytes) = (0, 0);
for my $e (sort readdir $d) {
    next unless $e =~ /^[A-Za-z0-9][A-Za-z0-9_.-]*\.[A-Za-z0-9_]{6,}$/;
    my $p = "$dir/$e";
    my @st = lstat $p or next;
    next if $st[4] == 0 or !(-d _ or -f _);
    next if grep { $_ eq $p or index($_, "$p/") == 0 } @busy;
    next if fresh($p);
    my $n = size($p);
    if (-d $p) {
        remove_tree($p, { error => \my $err });
        next if @$err;
    } else {
        unlink $p or next;
    }
    $count++;
    $bytes += $n;
}
printf "scratch: removed %d entries of %s, %d KiB\n", $count, $dir, $bytes / 1024;
