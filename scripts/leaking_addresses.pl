#!/usr/bin/env perl
#
# (c) 2017 Tobin C. Harding <me@tobin.cc>
# Licensed under the terms of the GNU GPL License version 2
#
# leaking_addresses.pl: Scan 64 bit kernel for potential leaking addresses.
#  - Scans dmesg output.
#  - Walks directory tree and parses each file (for each directory in @DIRS).

use warnings;
use strict;
use POSIX;
use File::Basename;
use Cwd 'abs_path';
use Term::ANSIColor qw(:constants);
use Getopt::Long qw(:config no_auto_abbrev);

my $P = $0;
my $V = '0.01';

# Directories to scan.
my @DIRS = ('/proc', '/sys');

# Command line options.
my $help = 0;
my $debug = 0;
my @dont_walk = ();
my @dont_parse = ();

sub help {
	my ($exitcode) = @_;

	print << "EOM";
Usage: $P [OPTIONS]
Version: $V

Options:

      --dont_walk=<dir>      Don't walk <dir> (absolute path).
      --dont_parse=<file>    Don't parse <file> (absolute path).
  -d, --debug		     Display debugging output.
  -h, --help, --version      Display this help and exit.

Example:

    # Just scan dmesg output.
    scripts/leaking_addresses.pl --dont_walk /proc --dont_walk /sys

Scans the running (64 bit) kernel for potential leaking addresses.

EOM
	exit($exitcode);
}

GetOptions(
	'dont_walk=s'		=> \@dont_walk,
	'dont_parse=s'		=> \@dont_parse,
	'd|debug'		=> \$debug,
	'h|help'		=> \$help,
	'version'		=> \$help
) or help(1);

help(0) if ($help);

parse_dmesg();
walk(@DIRS);

exit 0;


sub dprint
{
	printf(STDERR @_) if $debug;
}

# True if argument potentially contains a kernel address.
sub may_leak_address
{
	my ($line) = @_;

        # Ignore false positives.
        if ($line =~ '\b(0x)?(f|F){16}\b' or
            $line =~ '\b(0x)?0{16}\b' or
            $line =~ '\bKEY=[[:xdigit:]]{14} [[:xdigit:]]{16} [[:xdigit:]]{16}\b') {
		return 0;
        }

        # Potential kernel address.
        if ($line =~ '\b(0x)?ffff[[:xdigit:]]{12}\b') {
		return 1;
        }

        return 0;
}

sub parse_dmesg
{
        open my $cmd, '-|', 'dmesg';
        while (<$cmd>) {
                if (may_leak_address($_)) {
                        print 'dmesg: ' . $_;
                }
        }
        close $cmd;
}

# We should skip parsing these files.
sub skip_parse
{
	my ($path) = @_;

	# Skip these absolute path names.
        my @skip_paths = ('/proc/kmsg',
                          '/proc/kcore',
                          '/proc/kallsyms',
                          '/proc/fs/ext4/sdb1/mb_groups',
                          '/proc/1/fd/3',
                          '/sys/kernel/debug/tracing/trace_pipe',
                          '/sys/kernel/security/apparmor/revision');

        # Exclude paths passed in via command line options.
        push(@skip_paths, @dont_parse);

        # Skip these files under any subdirectory.
        my @skip_files = ('0',
                          '1',
                          '2',
                          'pagemap',
                          'events',
                          'access',
                          'registers',
                          'snapshot_raw',
                          'trace_pipe_raw',
                          'ptmx',
                          'trace_pipe');

        foreach(@skip_paths) {
                return 1 if (/^$path$/);
        }

        my($filename, $dirs, $suffix) = fileparse($path);
        foreach(@skip_files) {
		return 1 if (/^$filename$/);
        }

        return 0;
}

sub parse_file
{
	my ($file) = @_;

        if (! -R $file) {
                return;
        }

        if (skip_parse($file)) {
		dprint "skipping file: $file\n";
		return;
        }
        dprint "parsing: $file\n";

        open my $fh, "<", $file or return;
        while( <$fh> )  {
                if (may_leak_address($_)) {
                        print $file . ': ' . $_;
                }
        }
        close $fh;
}

# We should skip walking these directories.
sub skip_dir
{
	my ($path) = @_;

	# skip these directories under any subdirectory.
	my @skip_dirs = ('self',
                         'thread-self',
                         'cwd',
                         'fd',
                         'stderr',
                         'stdin',
                         'stdout');

	my($filename, $dirs, $suffix) = fileparse($path);

        foreach(@skip_dirs) {
                return 1 if (/^$filename$/);
        }

        return 0;
}

# Allow command line options to exclude paths to walk.
sub dont_walk
{
	my ($path) = @_;

	foreach(@dont_walk) {
		return 1 if (/^$path$/);
	}
}

# Recursively walk directory tree.
sub walk
{
        my @dirs = @_;
        my %seen;

        while (my $pwd = shift @dirs) {
		next if (dont_walk($pwd));
                next if (!opendir(DIR, $pwd));
                my @files = readdir(DIR);
                closedir(DIR);

                foreach my $file (@files) {
                        next if ($file eq '.' or $file eq '..');

                        my $path = "$pwd/$file";
                        next if (-l $path);

                        if (-d $path) {
				next if skip_dir($path);
				push @dirs, $path;
                        } else {
				parse_file($path);
                        }
                }
        }
}

