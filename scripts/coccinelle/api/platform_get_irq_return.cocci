/// Propagate the return value of platform_get_irq.
//# Sometimes the return value of platform_get_irq is tested using <= 0, but 0
//# might not be an appropriate return value in an error case.
///
// Confidence: Moderate
// Copyright: (C) 2015 Julia Lawall, Inria. GPLv2.
// URL: http://coccinelle.lip6.fr/
// Options: --no-includes --include-headers

virtual context
virtual org
virtual report

// ----------------------------------------------------------------------------

@r depends on context || org || report@
constant C;
statement S;
expression e, ret;
position j0, j1;
@@

* e@j0 = platform_get_irq(...);
(
if@j1 (...) {
  ...
  return -C;
} else S
|
if@j1 (...) {
  ...
  ret = -C;
  ...
  return ret;
} else S
)

// ----------------------------------------------------------------------------

@script:python r_org depends on org@
j0 << r.j0;
j1 << r.j1;
@@

msg = "Propagate return value of platform_get_irq."
coccilib.org.print_todo(j0[0], msg)
coccilib.org.print_link(j1[0], "")

// ----------------------------------------------------------------------------

@script:python r_report depends on report@
j0 << r.j0;
j1 << r.j1;
@@

msg = "Propagate return value of platform_get_irq around line %s." % (j1[0].line)
coccilib.report.print_report(j0[0], msg)

