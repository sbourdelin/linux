/// bad uslee_range - warn if min == max
//
//The problem is that usleep_range is calculating the delay by
//      exp = ktime_add_us(ktime_get(), min)
//      delta = (u64)(max - min) * NSEC_PER_USEC
//so delta is set to 0 if min==max
//and then calls
//      schedule_hrtimeout_range(exp, 0,...)
//effectively this means that the clock subsystem has no room to
//optimize. usleep_range() is in non-atomic context so a 0 range
//makes very little sense as the task can be preempted anyway so
//there is no guarantee that the 0 range would be adding much
//precision - it just removes optimization potential, so it probably
//never really makes sense for any non-RT systems.
//
//see: Documentation/timers/timers-howto.txt and
//Link: http://lkml.org/lkml/2016/11/29/54 for some notes on
//      when mdelay might not be a suitable replacement
//
// Confidence: Moderate
// Copyright: (C) 2016 Nicholas Mc Guire, OSADL.  GPLv2.
// Comments:
// Options: --no-includes --include-headers

virtual org
virtual report

@nullrange@
expression E;
constant C;
position p;
@@

<+...
(
  usleep_range@p(C,C)
|
  usleep_range@p(E,E)
)
...+>


@script:python depends on org@
p << nullrange.p;
range << nullrange.C;
@@

cocci.print_main("WARNING: inefficient usleep_range with range 0 (min==max)",p)

@script:python depends on report@
p << nullrange.p;
@@

coccilib.report.print_report(p[0],"WARNING: inefficient usleep_range with range 0 (min==max)")

