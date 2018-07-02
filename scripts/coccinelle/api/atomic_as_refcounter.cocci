// Check if refcount_t type and API should be used
// instead of atomic_t type when dealing with refcounters
//
// Copyright (c) 2016-2017, Elena Reshetova, Intel Corporation
//
// Confidence: Moderate
// URL: http://coccinelle.lip6.fr/
// Options: --include-headers --very-quiet

virtual report

@r1 exists@
expression a;
identifier x;
position p1, p2;
identifier fname =~ "free";
identifier fname2 =~ "(?:call_rcu|de(?:l|stroy)|(?:queue|schedule)_work)";
@@

(
(atomic_dec_and_test@p1
|atomic_long_dec_and_test@p1
|atomic64_dec_and_test@p1
|local_dec_and_test@p1
)                           (&(a)->x)
|
(atomic_dec_and_lock@p1
|atomic_long_dec_and_lock@p1
)                           (&(a)->x, ...)
)
...
(
 fname@p2(a, ...);
|
 fname2@p2(...);
)


@script:python depends on report@
p1 << r1.p1;
p2 << r1.p2;
@@
coccilib.report.print_report(p1[0],
                             "atomic_dec_and_test variation before object free at line %s."
                             % (p2[0].line))

@r4 exists@
expression a;
identifier x, y;
position p1, p2;
identifier fname =~ "free";
@@

(
(atomic_dec_and_test@p1
|atomic_long_dec_and_test@p1
|atomic64_dec_and_test@p1
|local_dec_and_test@p1
)                           (&(a)->x)
|
(atomic_dec_and_lock@p1
|atomic_long_dec_and_lock@p1
)                           (&(a)->x, ...)
)
...
y=a
...
fname@p2(y, ...);


@script:python depends on report@
p1 << r4.p1;
p2 << r4.p2;
@@
coccilib.report.print_report(p1[0],
                             "atomic_dec_and_test variation before object free at line %s."
                             % (p2[0].line))

@r2 exists@
expression a;
identifier F =~ "^atomic(?:64|_long)?_add_unless$", x;
position p1;
@@

 F(&(a)->x, -1, 1)@p1

@script:python depends on report@
p1 << r2.p1;
@@
coccilib.report.print_report(p1[0], "atomic_add_unless")

@r3 exists@
expression E;
identifier F =~ "^atomic(?:64|_long)?_add_return$";
position p1;
@@

 E = F@p1(-1, ...);

@script:python depends on report@
p1 << r3.p1;
@@
coccilib.report.print_report(p1[0], "x = atomic_add_return(-1, ...)")
