/// Drop static on local variable when the variable is not used before update.
//#
//# Removing unnecessary static on local variables reduces the code
//# size and increases maintainability.
//#
//# On the other hand, even though it is rare, be aware that if a
//# large object is initialized all at once, it might not be wise
//# to remove the static because that would increase the risk of a
//# stack overflow.
///
// Confidence: Moderate
// Copyright: (C) 2017 Julia Lawall, Inria. GPLv2.
// URL: http://coccinelle.lip6.fr/
// Copyright: (C) 2017 Gustavo A. R. Silva. GPLv2.
// Work supported by a grant from
// The Linux Foundation's Core Infrastructure Initiative.
// URL: https://www.coreinfrastructure.org/
// Options: --no-includes --include-headers
// Keywords: static

virtual patch
virtual context
virtual org
virtual report

// <smpl>
@bad exists@
position p;
identifier x;
expression e;
type T;
@@

T x@p;
... when != x = e
x = <+...x...+>

@worse exists@
position p;
identifier x;
type T;
@@

T x@p;
...
 &x

@modify depends on patch && !context && !org && !report@
identifier x;
expression e;
type T;
position p != {bad.p,worse.p};
@@

-static
 T x@p;
 ... when != x
     when strict
?x = e;
// </smpl>


// ----------------------------------------------------------------------------

@modify_context depends on !patch && (context || org || report) forall@
type T;
identifier x;
expression e;
position p != {bad.p,worse.p};
position j0;
@@

* static
 T x@j0@p;
 ... when != x
     when strict
?x = e;

// ----------------------------------------------------------------------------

@script:python modify_org depends on org@
j0 << modify_context.j0;
@@

msg = "Unnecessary static on local variable."
coccilib.org.print_todo(j0[0], msg)

// ----------------------------------------------------------------------------

@script:python modify_report depends on report@
j0 << modify_context.j0;
@@

msg = "Unnecessary static on local variable."
coccilib.report.print_report(j0[0], msg)

