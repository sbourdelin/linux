/// Recopying from the same user buffer frequently indicates a pattern of
/// Reading a size header, allocating, and then re-reading an entire
/// structure. If the structure's size is not re-validated, this can lead
/// to structure or data size confusions.
///
// Confidence: Moderate
// Copyright: (C) 2016 Kees Cook, Google. License: GPLv2.
// URL: http://coccinelle.lip6.fr/
// Comments:
// Options: -no_includes -include_headers

virtual report
virtual org

@cfu_twice@
position p;
identifier src;
expression dest1, dest2, size1, size2, offset;
@@

*copy_from_user(dest1, src, size1)
 ... when != src = offset
     when != src += offset
*copy_from_user@p(dest2, src, size2)

@script:python depends on org@
p << cfu_twice.p;
@@

cocci.print_main("potentially dangerous second copy_from_user()",p)

@script:python depends on report@
p << cfu_twice.p;
@@

coccilib.report.print_report(p[0],"potentially dangerous second copy_from_user()")
