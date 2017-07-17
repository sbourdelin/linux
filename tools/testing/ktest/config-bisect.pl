#!/usr/bin/perl -w
#
# Copyright 2010 - Steven Rostedt <srostedt@redhat.com>, Red Hat Inc.
# Licensed under the terms of the GNU GPL License version 2
#

use strict;
use List::Util qw(shuffle);

my $outputdir;

sub make_oldconfig {
	if (system "\$MAKE O=$outputdir olddefconfig") {
		# Perhaps olddefconfig doesn't exist in this version of
		# the kernel; try oldnoconfig
		print "olddefconfig failed, trying make oldnoconfig\n";
		if (system "\$MAKE O=$outputdir oldnoconfig") {
			print "oldnoconfig failed, trying yes '' | make oldconfig\n";
			# try a yes '' | oldconfig
			system "yes '' | \$MAKE O=$outputdir oldconfig" == 0 or
				die "failed make config oldconfig";
		}
	}
}

sub assign_configs {
	my ($hash, $config) = @_;

	print "Reading configs from $config\n";

	open (IN, $config)
		or die "Failed to read $config";

	while (<IN>) {
		chomp;
		if (/^((CONFIG\S*)=.*)/) {
			${$hash}{$2} = $1;
		} elsif (/^(# (CONFIG\S*) is not set)/) {
			${$hash}{$2} = $1;
		}
	}

	close(IN);
}

sub save_config {
	my ($pc, $file) = @_;

	my %configs = %{$pc};

	print "Saving configs into $file\n";

	open(OUT, ">$file") or die "Can not write to $file";

	foreach my $config (keys %configs) {
		print OUT "$configs{$config}\n";
	}
	close(OUT);

	make_oldconfig;
}

# compare two config hashes, and return configs with different vals.
sub diff_config_vals {
	my ($pa, $pb) = @_;

	# crappy Perl way to pass in hashes.
	my %a = %{$pa};
	my %b = %{$pb};

	my @ret;

	foreach my $item (keys %a) {
		if (defined($b{$item}) && $b{$item} ne $a{$item}) {
			push @ret, $item;
		}
	}

	return @ret;
}

# return if two configs are equal or not
# 0 is equal +1 b has something a does not
# +1 if a and b have a different item.
# -1 if a has something b does not
sub compare_configs {
	my ($pa, $pb) = @_;

	my %ret;

	# crappy Perl way to pass in hashes.
	my %a = %{$pa};
	my %b = %{$pb};

	foreach my $item (keys %b) {
		if (!defined($a{$item})) {
			return 1;
		}
		if ($a{$item} ne $b{$item}) {
			return 1;
		}
	}

	foreach my $item (keys %a) {
		if (!defined($b{$item})) {
			return -1;
		}
	}

	return 0;
}


sub process_new_config {
	my ($tc, $nc, $gc, $bc, $ofile) = @_;

	my %tmp_config = %{$tc};
	my %good_configs = %{$gc};
	my %bad_configs = %{$bc};

	my %new_configs;

	my $runtest = 1;
	my $ret;

	save_config \%tmp_config, $ofile;
	assign_configs \%new_configs, $ofile;

	$ret = compare_configs \%new_configs, \%bad_configs;
	if (!$ret) {
		print "New config equals bad config, try next test\n";
		$runtest = 0;
	}

	if ($runtest) {
		$ret = compare_configs \%new_configs, \%good_configs;
		if (!$ret) {
			print "New config equals good config, try next test\n";
			$runtest = 0;
		}
	}

	%{$nc} = %new_configs;

	return $runtest;
}

sub run_config_bisect {
	my ($outfile, $pgood, $pbad) = @_;

	my %good_configs = %{$pgood};
	my %bad_configs = %{$pbad};

	my @diff_arr = diff_config_vals \%good_configs, \%bad_configs;
	my $len_diff = $#diff_arr + 1;

	my $rand_tries = 10;
	my $runtest = 0;
	my %new_configs;
	my $ret;
	my $count = 0;

	print "d=$len_diff\n";

	if ($len_diff <= 1) {
		print "$0: No more bisecting possible\n";
		exit 2;
	}

	while ($runtest == 0 && ++$count <= $rand_tries) {
		my %tmp_config = %bad_configs;

		# If we fail to generate a new config (due to the top half
		# configs being unchangeable without the bottom half configs)
		# then try a few random permutations, and if those fail try
		# each option one at a time.

		if ($count <= $rand_tries) {
			@diff_arr = shuffle(@diff_arr);
		}

		my $half = int($#diff_arr / 2);
		if ($count > 2) {
			$half = int(rand($#diff_arr));
		}

		my @tophalf = @diff_arr[0 .. $half];

		foreach my $item (@tophalf) {
			$tmp_config{$item} = $good_configs{$item};
		}

		$runtest = process_new_config \%tmp_config, \%new_configs,
				\%good_configs, \%bad_configs, $outfile;
	}

	if ($runtest == 0) {
		print "$0: No more bisecting possible\n";
		exit 2;
	}
}

sub cb_by_file {
	my ($outfile, $goodfile, $badfile) = @_;
	my (%good, %bad);

	assign_configs \%good, $goodfile;
	assign_configs \%bad, $badfile;

	run_config_bisect $outfile, \%good, \%bad;
}

if (!defined($ARGV[2])) {
	print "Usage: $0 <outputdir> <good> <bad>\n";
	exit 1;
}

if (!defined($ENV{MAKE})) {
	$ENV{MAKE} = "make";
}

$outputdir = $ARGV[0];

cb_by_file("${outputdir}/.config", $ARGV[1], $ARGV[2]);
exit 0;
