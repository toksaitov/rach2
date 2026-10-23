#!/usr/bin/perl
#
# leftovers.pl: remove what finished work left in home directories: the
# directories of failed tests, loose files, and copies of the repository
# whose work master has.
#
# Usage: (cat leftovers.pl; echo __END__; cat TEXT) |
#            perl - [-n] [-l] [-m SHA] [-r GITDIR]... [-k PATH]...
#                SECONDS HOME...
#
# The program comes on standard input, followed by a line `__END__` and TEXT,
# the titles, bodies and comments of the open issues, which it reads as its
# DATA; without that line it removes nothing and fails. The manager's loop
# runs it before each check, as root in WSL on the testing host for the
# accounts' home directories, with -l, and on the owner's computer for the
# Desktop, which has the owner's checkout `rach2` and its worktrees. SHA is
# master's commit, and each GITDIR a repository that may have it, as each
# HOME/rach2/.git may. Each HOME has three kinds of leftovers:
#
# * failed: an entry of `build/tests/failed` in HOME/rach2 or a copy beside
#   it, which tests/run keeps for a test that failed;
# * copy: a directory HOME/rach2-* or HOME/rach2/.claude/worktrees/* with a
#   `.git` of its own, which is a clone, a copy that tools/dev/testing.sh
#   made or a git worktree;
# * loose: with -l, any other entry of HOME whose name begins neither with a
#   dot nor with `rach2`, such as a log or a script a session left there.
#
# An entry is removed when all of these hold:
#
# * it is a directory or a regular file, and root does not own it;
# * no PATH that -k gives is it;
# * nothing in it has changed for SECONDS;
# * nothing is mounted in it, and no process has its working directory, its
#   program, an open file or a mapped file in it, as /proc shows, or lsof(8)
#   where there is no /proc;
# * TEXT does not have its name as a word, nor, for a failed test, the name
#   of its run, `tests.ARCH.TIME.PID`;
# * a loose entry has nothing in it named as a key, a password, a tarball or
#   the Doom WAD is, and no `.git`;
# * a copy's worktree is not locked, no failed test's directory in it is kept
#   for an open issue or a process, git status finds nothing uncommitted or
#   untracked in it, every file git ignores in it is build output that the
#   build makes again, and a merge of its HEAD into SHA would leave SHA's tree
#   as it is, as `merged` in lib.sh decides for a branch.
#
# Git reads a copy as root, with no hook or file system monitor. It fetches
# the copy's HEAD into a scratch repository that borrows the objects of the
# GITDIR that has SHA, and makes the merge there, since git writing an object
# that a repository has already sets that object's time, and a copy so
# touched would never stay unchanged for SECONDS. A copy's age is read before
# the failed tests in it are removed. A copy that is a git worktree goes with
# its entry in its repository; one that testing.sh made goes with its lock and
# owner files beside it.
#
# With -n it removes nothing and says what it would remove. It prints a line
# for each entry it removes or keeps, but not for a failed test or a loose
# entry kept only because it changed within SECONDS, and a last line with how
# many it removed and their size. It stops itself after nine minutes, within
# the manager's limit of ten.

use strict;
use warnings;
use Cwd qw(abs_path);
use File::Find;
use File::Path qw(remove_tree);
use File::Temp qw(tempdir);
use Getopt::Long qw(:config no_ignore_case bundling);

$SIG{$_} = sub { die "leftovers: stopped by SIG$_[0]\n" } for qw(ALRM HUP INT TERM);
alarm 540;

my ($dry, $loose, $master, @refs, @keep) = (0, 0, '');
GetOptions('n' => \$dry, 'l' => \$loose, 'm=s' => \$master,
    'r=s' => \@refs, 'k=s' => \@keep)
    or die "usage: leftovers.pl [-n] [-l] [-m SHA] [-r GITDIR]... [-k PATH]... SECONDS HOME...\n";
my $age = shift // '';
$age =~ /^\d+$/ or die "leftovers: SECONDS must be a number\n";
$master =~ /^(?:[0-9a-f]{40}|[0-9a-f]{64}|)$/ or die "leftovers: SHA must be a full commit id\n";
my $cited = do {
    no warnings 'unopened';
    defined fileno DATA or die "leftovers: no text of the open issues after __END__\n";
    local $/;
    <DATA> // '';
};
my @homes = map { abs_path($_) } grep { -d } @ARGV;
my %keep = map { (abs_path($_) // $_) => 1 } @keep;
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

# busy PATH: whether a process or a mount uses PATH or anything under it.
sub busy {
    my $p = shift;
    return scalar grep { $_ eq $p or index($_, "$p/") == 0 } @busy;
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

# size PATH: the bytes that the files under PATH take on their filesystem.
sub size {
    my $n = 0;
    find({ no_chdir => 1, wanted => sub {
        my @s = lstat $_;
        $n += $s[12] * 512 if @s && -f _;
    } }, shift);
    return $n;
}

# cites NAME: whether the issues' text has NAME as a word: with no letter,
# digit, `_`, `-` or `.` before it, and none of the first three after it,
# nor a dot that one follows, so that a sentence may end with the name.
sub cites {
    my $n = shift;
    return $cited =~ /(?<![A-Za-z0-9_.-])\Q$n\E(?![A-Za-z0-9_-]|\.[A-Za-z0-9_-])/;
}

# guarded PATH: the first name under PATH that keeps a loose entry, if any:
# a repository, a key or a password, a tarball, or a WAD.
sub guarded {
    my $hit;
    find({ no_chdir => 1, wanted => sub {
        my $b = (split m{/}, $_)[-1];
        if ($b eq '.git' or $b =~ m{\.wad$|^id_|\.(?:pem|key|asc|gpg|p12|pfx|kdbx)$|
                _(?:rsa|dsa|ecdsa|ed25519)(?:\.pub)?$|authorized_keys|known_hosts|^\.netrc$|
                shadow|passw|secret|token|credential|\.(?:tar|tgz|tbz2?|txz|zip|7z)(?:\.|$)}xi) {
            $hit //= $_;
            $File::Find::prune = 1;
        }
    } }, shift);
    return $hit;
}

# git DIR ENV ARG...: run git in DIR with the variables of the hash ENV added,
# no optional lock, hook or file system monitor, and any owner trusted, and
# return its output, or undef when it fails.
sub git {
    my ($dir, $env, @arg) = @_;
    my $pid = open my $fh, '-|';
    return unless defined $pid;
    if (!$pid) {
        open STDERR, '>', '/dev/null';
        delete @ENV{qw(GIT_DIR GIT_WORK_TREE GIT_INDEX_FILE GIT_OBJECT_DIRECTORY
            GIT_ALTERNATE_OBJECT_DIRECTORIES)};
        %ENV = (%ENV, GIT_OPTIONAL_LOCKS => 0, %$env);
        exec 'git', '-C', $dir, '-c', 'safe.directory=*', '-c', 'core.fsmonitor=false',
            '-c', 'core.hooksPath=/dev/null', @arg;
        exit 127;
    }
    my $out = do { local $/; <$fh> } // '';
    close $fh or return;
    return $out;
}

# built PATH: whether PATH, as git ls-files prints an ignored file or
# directory, is output that the build makes again: build/, the compilation
# database and clangd's cache, the vendored projects' trees and stamps but not
# the GNU downloads, the krffs files that make writes from the committed key
# pairs, Python's caches and Finder's .DS_Store files.
sub built {
    my $p = shift;
    return 0 if $p =~ m{^vendor/gnu/downloads};
    return $p =~ m{^(?:build/|\.cache/|compile_commands\.json$|vendor/)|(?:^|/)(?:__pycache__/|\.DS_Store$)|
        ^krffs/(?:authorized_keys|test_authorized_keys|shadow|rach2_(?:demo|test)_password)$}x;
}

my $scratch = abs_path(tempdir('rach2-leftovers.XXXXXX', TMPDIR => 1, CLEANUP => 1));
my $objects;
if ($master ne '') {
    for my $r (@refs, map { "$_/rach2/.git" } @homes) {
        next unless -d "$r/objects" and defined git($r, {}, '--git-dir', $r, 'cat-file', '-e', "$master^{commit}");
        $objects = abs_path("$r/objects");
        last;
    }
}

# unmerged PATH: why the copy at PATH must stay, or nothing when its work is
# all on master.
sub unmerged {
    my $p = shift;
    my $common = git($p, {}, 'rev-parse', '--path-format=absolute', '--git-common-dir');
    my $own = git($p, {}, 'rev-parse', '--absolute-git-dir');
    return 'git cannot read it' unless defined $common and defined $own;
    chomp($common, $own);
    return 'its worktree is locked' if -e "$own/locked";
    my $head = git($p, {}, 'rev-parse', '-q', '--verify', 'HEAD^{commit}');
    return 'it has no commit checked out' unless defined $head;
    chomp $head;
    my $status = git($p, {}, 'status', '--porcelain', '--untracked-files=all');
    return 'git status fails in it' unless defined $status;
    return 'it has uncommitted or untracked files' if $status ne '';
    my $ignored = git($p, {}, 'ls-files', '-z', '--others', '--ignored', '--exclude-standard', '--directory');
    return 'git cannot list its ignored files' unless defined $ignored;
    for (split /\0/, $ignored) {
        return "its ignored $_ is not build output" unless built($_);
    }
    return 'no commit of master was given' if $master eq '';
    return "master's commit is in no repository here" unless defined $objects;
    my $r = "$scratch/repo.git";
    remove_tree($r);
    defined git($scratch, {}, 'init', '-q', '--bare', $r) or return 'git init fails in the scratch';
    open my $alt, '>', "$r/objects/info/alternates" or return "no scratch for git: $!";
    print $alt "$objects\n";
    close $alt or return "no scratch for git: $!";
    defined git($r, {}, '-c', 'gc.auto=0', 'fetch', '-q', '--no-tags', $p, $head)
        or return 'its commit cannot be fetched';
    my $tree = git($r, {}, 'rev-parse', '-q', '--verify', "$master^{tree}");
    my $merge = git($r, {}, 'merge-tree', '--write-tree', '--no-messages', $master, $head);
    return 'it has work that master lacks'
        unless defined $tree and defined $merge and (split /\n/, $merge)[0] eq (split /\n/, $tree)[0];
    return;
}

my ($count, $bytes) = (0, 0);

# sweep KIND PATH WHY: remove PATH when WHY is undefined, or keep it and say
# why when WHY is not empty.
sub sweep {
    my ($kind, $p, $why) = @_;
    if (defined $why) {
        print "leftovers: keeps $kind $p: $why\n" if $why ne '';
        return;
    }
    my $n = size($p);
    if ($dry) {
        printf "leftovers: would remove %s %s: %d KiB\n", $kind, $p, $n / 1024;
    } else {
        my $wt = $kind eq 'copy' && -f "$p/.git"
            ? git($p, {}, 'rev-parse', '--path-format=absolute', '--git-common-dir') : undef;
        if (-d $p) {
            remove_tree($p, { error => \my $err });
            if (@$err) {
                my ($f, $e) = %{ $err->[0] };
                print "leftovers: cannot remove $kind $p: $f: $e\n";
                return;
            }
        } elsif (!unlink $p) {
            print "leftovers: cannot remove $kind $p: $!\n";
            return;
        }
        if (defined $wt) {
            chomp $wt;
            git($wt, {}, '--git-dir', $wt, 'worktree', 'prune');
        }
        for my $f ("$p.sync.lock", "$p.sync.owner") {
            unlink $f if $kind eq 'copy' and -f $f and !-l $f and !busy($f);
        }
        printf "leftovers: removed %s %s: %d KiB\n", $kind, $p, $n / 1024;
    }
    $count++;
    $bytes += $n;
}

# verdict KIND PATH: why PATH must stay whatever its kind, or nothing.
sub verdict {
    my ($kind, $p) = @_;
    my @st = lstat $p or return '';
    return '' unless -d _ or -f _;
    return 'root owns it' if $st[4] == 0;
    return 'it is named to keep' if $keep{$p};
    return 'a process or a mount uses it' if busy($p);
    my $b = (split m{/}, $p)[-1];
    return 'an open issue cites it' if cites($b);
    return 'an open issue cites its run'
        if $kind eq 'failed' and $b =~ /^(tests\.[^.]+\.[^.]+\.[^.]+)\./ and cites($1);
    return;
}

# entries DIR: the names in DIR but . and .., sorted.
sub entries {
    opendir my $d, shift or return;
    return sort grep { $_ ne '.' and $_ ne '..' } readdir $d;
}

for my $h (@homes) {
    my @trees = map { "$h/$_" } grep { /^rach2(?:-|$)/ and -d "$h/$_" and !-l "$h/$_" } entries($h);
    my $wts = "$h/rach2/.claude/worktrees";
    my @copies = grep { $_ ne "$h/rach2" } @trees;
    push @copies, map { "$wts/$_" } entries($wts) if -d $wts and !-l $wts and !-l "$h/rach2/.claude";
    my %why = map { ($_ => scalar verdict('copy', $_)) } grep { -e "$_/.git" and !-l "$_/.git" } @copies;
    for my $c (grep { !defined $why{$_} } keys %why) {
        $why{$c} = "it changed within $age seconds" if fresh($c);
    }
    for my $t (@trees) {
        my $f = "$t/build/tests/failed";
        next if -l $f or !-d $f;
        for my $p (map { "$f/$_" } entries($f)) {
            my $why = verdict('failed', $p);
            $why{$t} //= "its failed test $p is kept" if $why and exists $why{$t};
            $why //= fresh($p) ? '' : undef;
            sweep('failed', $p, $why);
        }
    }
    for my $c (sort keys %why) {
        sweep('copy', $c, $why{$c} // unmerged($c));
    }
    next unless $loose;
    for my $e (grep { !/^(?:\.|rach2)/ } entries($h)) {
        my $p = "$h/$e";
        my $why = verdict('loose', $p);
        if (!defined $why) {
            my $hit = guarded($p);
            $why = defined $hit ? "$hit may be a key, a tarball, a WAD or a repository"
                : fresh($p) ? '' : undef;
        }
        sweep('loose', $p, $why);
    }
}
printf "leftovers: %s %d entries, %d KiB\n", $dry ? 'would remove' : 'removed', $count, $bytes / 1024;
