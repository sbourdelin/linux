/// report bad/problematic usleep_range usage
//
// This is a checker for the documented intended use of usleep_range
// see: Documentation/timers/timers-howto.txt and
// Link: http://lkml.org/lkml/2016/11/29/54 for some notes on
//       when mdelay might not be a suitable replacement
//
// Limitations:
//  * The numeric limits are only checked when numeric constants are in
//    use (as of 4.9.0 thats 90.29% of the calls) no constant folding
//    is done - so this can miss some out-of-range cases - but in 4.9.0
//    it was catching 74 of the 103 bad cases (71.8%) and 50 of 52
//    (96.1%) of the critical cases (min < 10 and min==max - there
//  * There may be RT use-cases where both min < 10 and min==max)
//    justified (e.g. high-throughput drivers on a shielded core)
//
// 1) warn if min == max
//
//  The problem is that usleep_range is calculating the delay by
//      exp = ktime_add_us(ktime_get(), min)
//      delta = (u64)(max - min) * NSEC_PER_USEC
//  so delta is set to 0 if min==max
//  and then calls
//      schedule_hrtimeout_range(exp, 0,...)
//  effectively this means that the clock subsystem has no room to
//  optimize. usleep_range() is in non-atomic context so a 0 range
//  makes very little sense as the task can be preempted anyway so
//  there is no guarantee that the 0 range would be adding much
//  precision - it just removes optimization potential, so it probably
//  never really makes sense.
//
// 2) warn if min < 10 or min > 20ms
//
//  it makes little sense to use a non-atomic call for very short
//  delays because the scheduling jitter will most likely exceed
//  this limit - udelay() makes more sense in that case. For very
//  large delays using hrtimers is useless as preemption becomes
//  quite likely resulting in high inaccuracy anyway - so use
//  jiffies based msleep and don't burden the hrtimer subsystem.
//
// 3) warn if max < min
//
//  Joe Perches <joe@perches.com> added a check for this case
//  that is definitely wrong.
//
// Confidence: Moderate
// Copyright: (C) 2016 Nicholas Mc Guire, OSADL.  GPLv2.
// Comments:
// Options: --no-includes --include-headers

virtual org
virtual report
virtual context

@nullrangectx depends on context@
expression E1,E2;
position p;
@@

* usleep_range@p(E1,E2)


@nullrange@
expression E1,E2;
position p;
@@

  usleep_range@p(E1,E2)

@script:python depends on !context@
p << nullrange.p;
min << nullrange.E1;
max << nullrange.E2;
@@

if(min == max):
   msg = "WARNING: usleep_range min == max (%s) - consider delta " % (min)
   coccilib.report.print_report(p[0], msg)
if str.isdigit(min):
   if(int(min) < 10):
      msg = "ERROR: usleep_range min (%s) less than 10us - consider using udelay()" % (min)
      coccilib.report.print_report(p[0], msg)
   if(20000 < int(min)):
      msg = "ERROR: usleep_range min (%s) exceed 20m - consider using mslee()" % (min)
      coccilib.report.print_report(p[0], msg)
   if(int(max) < int(min)):
      msg = "ERROR: usleep_range max (%s) less than min (%s)" % (max,min)
      coccilib.report.print_report(p[0], msg)
