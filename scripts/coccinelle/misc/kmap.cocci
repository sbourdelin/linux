/// kmap never fails; remove NULL test case.
///
// Copyright: (C) 2017 Fabian Frederick.  GPLv2.
// Comments: -
// Options: --no-includes --include-headers

virtual patch
virtual org
virtual report
virtual context

@r2 depends on patch@
expression E;
position p;
@@

E = kmap(...);
- if (!E) {
- ...
- }

@r depends on context || report || org @
expression E;
position p;
@@

* E = kmap(...);
* if (@p!E) {
* ...
* }

@script:python depends on org@
p << r.p;
@@

cocci.print_main("kmap() can't fail, NULL check and special process is not needed", p)

@script:python depends on report@
p << r.p;
@@

msg = "WARNING: NULL check on kmap() result is not needed."
coccilib.report.print_report(p[0], msg)
