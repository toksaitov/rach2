#!/usr/bin/perl
#
# scratch.pl: remove the scratch that tests left in directories when they
# were killed before their exit traps ran.
#
# Usage: perl scratch.pl -d DIR [-d DIR]... SECONDS STEM...
#
# sweep.sh runs this as root in WSL on the testing host, for /tmp, whose
# tmpfs is the host's memory, and the build/tmp of each account's clone, and
# here for this checkout's build/tmp and that of the Mac's release copy, with
# the stems that `stems` in lib.sh reads from master's tests and tools. A DIR
# that does not exist is passed over. An entry of a DIR is removed when all of
# these hold:
#
# * its name is one of the STEMs that begin with `rach2`, a dot and six to
#   eight letters, digits or underscores, as mktemp(1) makes from a template
#   STEM.XXXXXX and Python's tempfile from a prefix `STEM.`;
# * it is a directory or a regular file, and root does not own it;
# * nothing in it has changed for SECONDS;
# * nothing is mounted in it, and no process has its working directory, its
#   program, an open file or a mapped file in it, as /proc shows, or lsof(8)
#   where there is no /proc, as on macOS; lsof sees only the processes that
#   the user who runs this may read.
#
# It stops itself after nine minutes, within sweep.sh's limit of ten, and
# prints one line for each DIR: how many entries it removed and their size.

use strict;
use warnings;
use Cwd qw(abs_path);
use File::Find;
use File::Path qw(remove_tree);
use Getopt::Long qw(:config no_ignore_case bundling);

alarm 540;
my @dirs;
GetOptions('d=s' => \@dirs) and @dirs
    or die "usage: scratch.pl -d DIR [-d DIR]... SECONDS STEM...\n";
my $age = shift // '';
$age =~ /^\d+$/ or die "scratch: SECONDS must be a number\n";
my @stems = grep { /^rach2/ } @ARGV;
my $stems = join '|', map { quotemeta } @stems;
my $now = time;

my @busy;
if (-d '/proc/self/fd') {
    for my $p (glob '/proc/[0-9]*') {
        for my $l ("$p/cwd", "$p/exe", glob "$p/fd/*") {
            my $t = readlink $l;
            push @busy, $t if defined $t;
        }
        if (open my $m, '<', "$p/maps") {
            push @busy, map { m{^\S+ \S+ \S+ \S+ \S+ +(/.*?)(?: \(deleted\))?$} ? $1 : () } <$m>;
        }
    }
} else {
    push @busy, map { /^n(\/.*)/ ? $1 : () } `lsof -w -n -F n 2> /dev/null`;
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
        if (-d $_ && -f "$_/.etude-retention-root.json") {
            $young = 1;
            $File::Find::prune = 1;
        } elsif (!@s || $now - $s[9] < $age) {
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

# sweep DIR: remove the scratch in DIR and print the line for it.
sub sweep {
    my $dir = abs_path(shift) // return;
    opendir my $d, $dir or return;
    my ($count, $bytes) = (0, 0);
    my @used = grep { index($_, "$dir/") == 0 } @busy;
    for my $e (sort readdir $d) {
        next unless @stems and $e =~ /^(?:$stems)\.[A-Za-z0-9_]{6,8}$/;
        my $p = "$dir/$e";
        my @st = lstat $p or next;
        next if $st[4] == 0 or !(-d _ or -f _);
        next if grep { $_ eq $p or index($_, "$p/") == 0 } @used;
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
}

sweep($_) for grep { -d } @dirs;
