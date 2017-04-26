/// Use offset_in_page instead of duplicating its implementation
///
// Confidence: High
// Copyright: (C) 2017 Vaishali Thakkar, Oracle. GPLv2.
// Options: --no-includes --include-headers
// Keywords: offset_in_page

virtual patch
virtual context
virtual org
virtual report

@r_patch depends on patch@
expression e;
identifier i;
@@
- unsigned long i = (unsigned long)e & ~PAGE_MASK;
...
- i
+ offset_in_page(e)

@r1_patch depends on patch@
expression e1;
@@

(
- (unsigned long)e1 & ~PAGE_MASK
+ offset_in_page(e1)
|
- (unsigned long)e1 % PAGE_SIZE
+ offset_in_page(e1)
)

@r_context depends on !patch@
expression e;
identifier i;
position p;
@@

* unsigned long i = (unsigned long)e@p & ~PAGE_MASK;
...
* i

@r1_context depends on !patch@
expression e1;
position p1;
@@

(
* (unsigned long)e1@p1 & ~PAGE_MASK
|
* (unsigned long)e1@p1 % PAGE_SIZE
)

@script:python depends on org@
p << r_context.p;
@@

coccilib.org.print_todo(p[0], "WARNING opportunity for offset_in_page")

@script:python depends on org@
p << r1_context.p1;
@@

coccilib.org.print_todo(p[0], "WARNING opportunity for offset_in_page")

@script:python depends on report@
p << r_context.p;
@@

coccilib.report.print_report(p[0], "WARNING opportunity for offset_in_page")

@script:python depends on report@
p << r1_context.p1;
@@

coccilib.report.print_report(p[0], "WARNING opportunity for offset_in_page")
