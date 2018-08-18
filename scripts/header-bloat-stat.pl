#!/usr/bin/perl

use strict;
use warnings;

use Getopt::Long;
use File::Find;
use Statistics::Descriptive;

sub help {
    printf "%s [-c] [-m] [-n <name>] [<dirs>]\n", $0;
    printf "  -c  output a single line with data in columns\n";
    printf "  -m  include min/max statistics\n";
    printf "  -n  optional name (e.g. git revision) to use as first datum\n";
    exit(0);
}

my $name;
my $minmax = 0;
my $column = 0;

GetOptions("c|column" => \$column,
	   "m|minmax" => \$minmax,
	   "n|name=s" => \$name,
	   "h|help"   => \&help)
    or die "Bad option";

my @stats =
    (
     ['mean',   sub {$_[0]->mean()}],
     ['min',    sub {$_[0]->min()}],
     ['q25',    sub {$_[0]->quantile(1)}],
     ['median', sub {$_[0]->quantile(2)}],
     ['q75',    sub {$_[0]->quantile(3)}],
     ['max',    sub {$_[0]->max()}],
    );

my @scalars = ('hcount', 'csize', 'tsize', 'ratio');
my %data;
my @out;

find({wanted => \&process_cmd_file, no_chdir => 1}, @ARGV ? @ARGV : '.');

add_output('name', $name) if $name;
add_output('#TUs', $data{ntu});
for my $s (@scalars) {
    my $vals = Statistics::Descriptive::Full->new();
    $vals->add_data(@{$data{$s}});
    $vals->sort_data();
    for my $stat (@stats) {
	next if $s eq 'ratio' && $stat->[0] eq 'mean';
	next if $stat->[0] =~ m/^(min|max)$/ && !$minmax;
	my $val = $stat->[1]->($vals);
	add_output($s . "_" . $stat->[0], $val);
    }
}

if ($column) {
    print join("\t", map {$_->[1]} @out), "\n";
} else {
    printf "%s\t%s\n", @$_ for @out;
}

sub add_output {
    push @out, [@_];
}

sub process_cmd_file {
    # Remove leading ./ components
    s|^(\./)*||;
    # Stuff that includes userspace/host headers is not interesting.
    if (m/^(scripts|tools)/) {
	$File::Find::prune = 1;
	return;
    }
    return unless m/\.o\.cmd$/;

    open(my $fh, '<', $_)
	or die "failed to open $_: $!";
    while (<$fh>) {
	chomp;
	if (m/^source_/) {
	    # Only process stuff built from .S or .c
	    return unless m/\.[Sc]$/;
	}
	if (m/^# header-stats: ([0-9]+) ([0-9]+) ([0-9]+)/) {
	    push @{$data{hcount}}, $1;
	    push @{$data{csize}}, $2;
	    push @{$data{tsize}}, $2 + $3;
	    push @{$data{ratio}}, $2 ? ($2 + $3)/$2 : 1.0;
	    $data{ntu}++;
	}
    }
    close($fh);
}
