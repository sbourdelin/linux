#!/usr/bin/env perl
#
# (c) 2017 Tobin C. Harding <me@tobin.cc>
# (c) 2017 Kaiwan N Billimoria <kaiwan.billimoria@gmail.com> (ix86 stuff)
#
# Licensed under the terms of the GNU GPL License version 2
#
# leaking_addresses.pl: Scan the kernel for potential leaking addresses.
#  - Scans dmesg output.
#  - Walks directory tree and parses each file (for each directory in @DIRS).
#
# Use --debug to output path before parsing, this is useful to find files that
# cause the script to choke.
#
# You may like to set kptr_restrict=2 before running script
# (see Documentation/sysctl/kernel.txt).

use warnings;
use strict;
use POSIX;
use File::Basename;
use File::Spec;
use Cwd 'abs_path';
use Term::ANSIColor qw(:constants);
use Getopt::Long qw(:config no_auto_abbrev);
use Config;
use feature 'state';

my $P = $0;
my $V = '0.01';

# Directories to scan.
my @DIRS = ('/proc', '/sys');

# Timer for parsing each file, in seconds.
my $TIMEOUT = 10;

# Script can only grep for kernel addresses on the following architectures. If
# your architecture is not listed here and has a grep'able kernel address please
# consider submitting a patch.
my @SUPPORTED_ARCHITECTURES = ('x86_64', 'ppc64', 'i[3456]86');

# Command line options.
my $help = 0;
my $debug = 0;
my $raw = 0;			# Show raw output.
my $output_raw = "";		# Write raw results to file.
my $input_raw = "";		# Read raw results from file instead of scanning.
my $suppress_dmesg = 0;		# Don't show dmesg in output.
my $squash_by_path = 0;		# Summary report grouped by absolute path.
my $squash_by_filename = 0;	# Summary report grouped by filename.
my $page_offset_32bit = 0;	# 32-bit: value of CONFIG_PAGE_OFFSET
my $kernel_config_file = "";	# Kernel configuration file.

# Do not parse these files (absolute path).
my @skip_parse_files_abs = ('/proc/kmsg',
			    '/proc/kcore',
			    '/proc/fs/ext4/sdb1/mb_groups',
			    '/proc/1/fd/3',
			    '/sys/firmware/devicetree',
			    '/proc/device-tree',
			    '/sys/kernel/debug/tracing/trace_pipe',
			    '/sys/kernel/security/apparmor/revision');

# Do not parse these files under any subdirectory.
my @skip_parse_files_any = ('0',
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

# Do not walk these directories (absolute path).
my @skip_walk_dirs_abs = ();

# Do not walk these directories under any subdirectory.
my @skip_walk_dirs_any = ('self',
			  'thread-self',
			  'cwd',
			  'fd',
			  'usbmon',
			  'stderr',
			  'stdin',
			  'stdout');

sub help
{
	my ($exitcode) = @_;

	print << "EOM";

Usage: $P [OPTIONS]
Version: $V

Options:

	-o, --output-raw=<file>		Save results for future processing.
	-i, --input-raw=<file>		Read results from file instead of scanning.
	      --raw			  Show raw results (default).
	      --suppress-dmesg            Do not show dmesg results.
	      --squash-by-path            Show one result per unique path.
	      --squash-by-filename        Show one result per unique filename.
	    --page-offset-32bit=<hex>	PAGE_OFFSET value (for 32-bit kernels).
	    --kernel-config-file=<file>	Kernel configuration file (e.g /boot/config)
	-d, --debug			Display debugging output.
	-h, --help, --version		Display this help and exit.

Examples:

	# Scan kernel and dump raw results.
	$0

	# Scan kernel and save results to file.
	$0 --output-raw scan.out

	# View summary report.
	$0 --input-raw scan.out --squash-by-filename

	# Scan kernel on a 32-bit system with a 2GB:2GB virtual address split.
	$0 --page-offset-32bit=0x80000000

Scans the running kernel for potential leaking addresses.

EOM
	exit($exitcode);
}

GetOptions(
	'd|debug'		=> \$debug,
	'h|help'		=> \$help,
	'version'		=> \$help,
	'o|output-raw=s'        => \$output_raw,
	'i|input-raw=s'         => \$input_raw,
	'suppress-dmesg'        => \$suppress_dmesg,
	'squash-by-path'        => \$squash_by_path,
	'squash-by-filename'    => \$squash_by_filename,
	'raw'                   => \$raw,
	'page-offset-32bit=o'   => \$page_offset_32bit,
	'kernel-config-file=s'  => \$kernel_config_file,
) or help(1);

help(0) if ($help);

if ($input_raw) {
	format_output($input_raw);
	exit(0);
}

if (!$input_raw and ($squash_by_path or $squash_by_filename)) {
	printf "\nSummary reporting only available with --input-raw=<file>\n";
	printf "(First run scan with --output-raw=<file>.)\n";
	exit(128);
}

if (is_supported_architecture()) {
	show_detected_architecture() if $debug;
} else {
	printf "\nScript does not support your architecture, sorry.\n";
	printf "\nCurrently we support: \n\n";
	foreach(@SUPPORTED_ARCHITECTURES) {
		printf "\t%s\n", $_;
	}

	my $archname = $Config{archname};
	printf "\n\$ perl -MConfig -e \'print \"\$Config{archname}\\n\"\'\n";
	printf "%s\n", $archname;

	exit(129);
}

if ($output_raw) {
	open my $fh, '>', $output_raw or die "$0: $output_raw: $!\n";
	select $fh;
}

parse_dmesg();
walk(@DIRS);

exit 0;

sub dprint
{
	printf(STDERR @_) if $debug;
}

sub is_supported_architecture
{
	return (is_x86_64() or is_ppc64() or is_ix86_32());
}

sub is_x86_64
{
	my $archname = $Config{archname};

	if ($archname =~ m/x86_64/) {
		return 1;
	}
	return 0;
}

sub is_ppc64
{
	my $archname = $Config{archname};

	if ($archname =~ m/powerpc/ and $archname =~ m/64/) {
		return 1;
	}
	return 0;
}

sub is_ix86_32
{
	my $archname = $Config{archname};

	if ($archname =~ m/i[3456]86-linux/) {
		return 1;
	}
	return 0;
}

sub show_detected_architecture
{
	printf "Detected architecture: ";
	if (is_ix86_32()) {
		printf "32 bit x86\n";
	} elsif (is_x86_64()) {
		printf "x86_64\n";
	} elsif (is_ppc64()) {
		printf "ppc64\n";
	} else {
		printf "failed to detect architecture\n"
	}
}

sub is_false_positive
{
	my ($match) = @_;

	if (is_ix86_32()) {
		return is_false_positive_ix86_32($match);
	}

	# 64 bit architectures

	if ($match =~ '\b(0x)?(f|F){16}\b' or
	    $match =~ '\b(0x)?0{16}\b') {
		return 1;
	}

	if (is_x86_64) {
		# vsyscall memory region, we should probably check against a range here.
		if ($match =~ '\bf{10}600000\b' or
		    $match =~ '\bf{10}601000\b') {
			return 1;
		}
	}

	return 0;
}

sub is_false_positive_ix86_32
{
	my ($match) = @_;
	state $page_offset = get_page_offset(); # only gets called once

	if ($match =~ '\b(0x)?(f|F){8}\b') {
		return 1;
	}

	my $addr32 = eval hex($match);
	if ($addr32 < $page_offset) {
		return 1;
	}

	return 0;
}

sub get_page_offset
{
	my $page_offset;
	my $default_offset = "0xc0000000";
	my @config_files;

	# Allow --page-offset-32bit to over ride.
	if ($page_offset_32bit != 0) {
		return $page_offset_32bit;
	}

	# Allow --kernel-config-file to over ride.
	if ($kernel_config_file != "") {
		@config_files = ($kernel_config_file);
	} else {
		my $config_file = '/boot/config-' . `uname -r`;
		@config_files = ($config_file, '/boot/config');
	}

	if (-R "/proc/config.gz") {
		my $tmp_file = "/tmp/tmpkconf";
		if (system("gunzip < /proc/config.gz > $tmp_file")) {
			dprint " parse_kernel_config: system(gunzip...) failed\n";
		} else {
			$page_offset = parse_kernel_config_file($tmp_file);
			if ($page_offset ne "") {
				return $page_offset;
			}
		}
		system("rm -f $tmp_file");
	}

	foreach my $config_file (@config_files) {
		$page_offset = parse_kernel_config($config_file);
		if ($page_offset ne "") {
			return $page_offset;
		}
	}

	printf STDERR "Failed to parse kernel config files\n";
	printf STDERR "Falling back to %s\n", $default_offset;
	return $default_offset;
}

sub parse_kernel_config_file
{
	my ($file) = @_;
	my $config = 'CONFIG_PAGE_OFFSET';
	my $val = "";

	open(my $fh, "<", $file) or return "";
	while (my $line = <$fh> ) {
		if ($line =~ /^$config/) {
			my ($str, $val) = split /=/, $line;
			chomp($val);
			last;
		}
	}

	close $fh;
	return $val;
}


# True if argument potentially contains a kernel address.
sub may_leak_address
{
	my ($line) = @_;
	my $address_re;

	# Signal masks.
	if ($line =~ '^SigBlk:' or
	    $line =~ '^SigIgn:' or
	    $line =~ '^SigCgt:') {
		return 0;
	}

	if (is_x86_64() or is_ppc64()) {
		if ($line =~ '\bKEY=[[:xdigit:]]{14} [[:xdigit:]]{16} [[:xdigit:]]{16}\b' or
		    $line =~ '\b[[:xdigit:]]{14} [[:xdigit:]]{16} [[:xdigit:]]{16}\b') {
			return 0;
		}
	}

	# One of these is guaranteed to be true.
	if (is_x86_64()) {
		$address_re = '\b(0x)?ffff[[:xdigit:]]{12}\b';
	} elsif (is_ppc64()) {
		$address_re = '\b(0x)?[89abcdef]00[[:xdigit:]]{13}\b';
	} elsif (is_ix86_32()) {
		$address_re = '\b(0x)?[[:xdigit:]]{8}\b';
	}

	while (/($address_re)/g) {
		if (!is_false_positive($1)) {
			return 1;
		}
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

# True if we should skip this path.
sub skip
{
	my ($path, $paths_abs, $paths_any) = @_;

	foreach (@$paths_abs) {
		return 1 if (/^$path$/);
	}

	my($filename, $dirs, $suffix) = fileparse($path);
	foreach (@$paths_any) {
		return 1 if (/^$filename$/);
	}

	return 0;
}

sub skip_parse
{
	my ($path) = @_;
	return skip($path, \@skip_parse_files_abs, \@skip_parse_files_any);
}

sub timed_parse_file
{
	my ($file) = @_;

	eval {
		local $SIG{ALRM} = sub { die "alarm\n" }; # NB: \n required.
		alarm $TIMEOUT;
		parse_file($file);
		alarm 0;
	};

	if ($@) {
		die unless $@ eq "alarm\n";	# Propagate unexpected errors.
		printf STDERR "timed out parsing: %s\n", $file;
	}
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
	while ( <$fh> ) {
		if (may_leak_address($_)) {
			print $file . ': ' . $_;
		}
	}
	close $fh;
}


# True if we should skip walking this directory.
sub skip_walk
{
	my ($path) = @_;
	return skip($path, \@skip_walk_dirs_abs, \@skip_walk_dirs_any)
}

# Recursively walk directory tree.
sub walk
{
	my @dirs = @_;

	while (my $pwd = shift @dirs) {
		next if (skip_walk($pwd));
		next if (!opendir(DIR, $pwd));
		my @files = readdir(DIR);
		closedir(DIR);

		foreach my $file (@files) {
			next if ($file eq '.' or $file eq '..');

			my $path = "$pwd/$file";
			next if (-l $path);

			if (-d $path) {
				push @dirs, $path;
			} else {
				timed_parse_file($path);
			}
		}
	}
}

sub format_output
{
	my ($file) = @_;

	# Default is to show raw results.
	if ($raw or (!$squash_by_path and !$squash_by_filename)) {
		dump_raw_output($file);
		return;
	}

	my ($total, $dmesg, $paths, $files) = parse_raw_file($file);

	printf "\nTotal number of results from scan (incl dmesg): %d\n", $total;

	if (!$suppress_dmesg) {
		print_dmesg($dmesg);
	}

	if ($squash_by_filename) {
		squash_by($files, 'filename');
	}

	if ($squash_by_path) {
		squash_by($paths, 'path');
	}
}

sub dump_raw_output
{
	my ($file) = @_;

	open (my $fh, '<', $file) or die "$0: $file: $!\n";
	while (<$fh>) {
		if ($suppress_dmesg) {
			if ("dmesg:" eq substr($_, 0, 6)) {
				next;
			}
		}
		print $_;
	}
	close $fh;
}

sub parse_raw_file
{
	my ($file) = @_;

	my $total = 0;          # Total number of lines parsed.
	my @dmesg;              # dmesg output.
	my %files;              # Unique filenames containing leaks.
	my %paths;              # Unique paths containing leaks.

	open (my $fh, '<', $file) or die "$0: $file: $!\n";
	while (my $line = <$fh>) {
		$total++;

		if ("dmesg:" eq substr($line, 0, 6)) {
			push @dmesg, $line;
			next;
		}

		cache_path(\%paths, $line);
		cache_filename(\%files, $line);
	}

	return $total, \@dmesg, \%paths, \%files;
}

sub print_dmesg
{
	my ($dmesg) = @_;

	print "\ndmesg output:\n";

	if (@$dmesg == 0) {
		print "<no results>\n";
		return;
	}

	foreach(@$dmesg) {
		my $index = index($_, ': ');
		$index += 2;    # skid ': '
		print substr($_, $index);
	}
}

sub squash_by
{
	my ($ref, $desc) = @_;

	print "\nResults squashed by $desc (excl dmesg). ";
	print "Displaying [<number of results> <$desc>], <example result>\n";

	if (keys %$ref == 0) {
		print "<no results>\n";
		return;
	}

	foreach(keys %$ref) {
		my $lines = $ref->{$_};
		my $length = @$lines;
		printf "[%d %s] %s", $length, $_, @$lines[0];
	}
}

sub cache_path
{
	my ($paths, $line) = @_;

	my $index = index($line, ': ');
	my $path = substr($line, 0, $index);

	$index += 2;            # skip ': '
	add_to_cache($paths, $path, substr($line, $index));
}

sub cache_filename
{
	my ($files, $line) = @_;

	my $index = index($line, ': ');
	my $path = substr($line, 0, $index);
	my $filename = basename($path);

	$index += 2;            # skip ': '
	add_to_cache($files, $filename, substr($line, $index));
}

sub add_to_cache
{
	my ($cache, $key, $value) = @_;

	if (!$cache->{$key}) {
		$cache->{$key} = ();
	}
	push @{$cache->{$key}}, $value;
}
