/// Check for double conversion of timeouts values
//
// for constants this actually makes little sense - other than
// for readability - in cases where non-constants are involved there is
// a call to jiffies_to_msecs that is saved here.
//
// Confidence: Medium
// Copyright: (C) 2015 Nicholas Mc Guire <hofrat@osadl.org>, OSADL, GPL v2
// URL: http://coccinelle.lip6.fr
// Options: --no-includes --include-headers

virtual org
virtual patch
virtual report

@depends on patch@
constant C;
expression E;
@@

(
- msleep_interruptible(jiffies_to_msecs(C))
+ schedule_timeout_interruptible(C)
|
- msleep_interruptible(jiffies_to_msecs(E))
+ schedule_timeout_interruptible(E)
|
- msleep(jiffies_to_msecs(C))
+ schedule_timeout(C)
|
- msleep(jiffies_to_msecs(E))
+ schedule_timeout(E)
)

@cc depends on !patch && (org || report)@
constant C;
expression E;
position p;
@@

(
* msleep_interruptible@p(jiffies_to_msecs(C))
|
* msleep_interruptible@p(jiffies_to_msecs(E))
|
* msleep@p(jiffies_to_msecs(C))
|
* msleep@p(jiffies_to_msecs(E))
)

@script:python depends on org@
p << cc.p;
@@

msg = "INFO: schedule_timeout*() preferred if timeout in jiffies"
coccilib.org.print_safe_todo(p[0], msg)

@script:python depends on report@
p << cc.p;
@@

msg = "INFO: schedule_timeout*() preferred if timeout in jiffies"
coccilib.report.print_report(p[0], msg)
