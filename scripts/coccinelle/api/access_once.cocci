/// Use READ_ONCE or WRITE_ONCE instead of ACCESS_ONCE.
///
// Confidence: High
// Contact: Joe Perches <joe@perches.com>
// Options: --no-includes --include-headers

virtual patch
virtual context
virtual org
virtual report

@written depends on patch && !context && !org && !report@
expression e1;
expression e2;
@@

-       ACCESS_ONCE(e1) = e2
+       WRITE_ONCE(e1, e2)

@read depends on patch && !context && !org && !report@
expression e1;
@@

-       ACCESS_ONCE(e1)
+       READ_ONCE(e1)

// ----------------------------------------------------------------------------

@written_context depends on !patch && (context || org || report)@
expression e1, e2;
position j0;
@@

*        ACCESS_ONCE@j0(e1) = e2

@read_context depends on !patch && (context || org || report)@
expression e1;
position j0;
@@

*        ACCESS_ONCE@j0(e1)

// ----------------------------------------------------------------------------

@script:python written_org depends on org@
j0 << written_context.j0;
@@

msg = "Use WRITE_ONCE for an ACCESS_ONCE that is written."
coccilib.org.print_todo(j0[0], msg, color="ovl-face2")

@script:python read_org depends on org@
j0 << read_context.j0;
@@

msg = "Use READ_ONCE for an ACCESS_ONCE that is read."
coccilib.org.print_todo(j0[0], msg)

// ----------------------------------------------------------------------------

@script:python written_report depends on report@
j0 << written_context.j0;
@@

msg = "Use WRITE_ONCE for an ACCESS_ONCE that is written."
coccilib.report.print_report(j0[0], msg)

@script:python read_report depends on report@
j0 << read_context.j0;
@@

msg = "Use READ_ONCE for an ACCESS_ONCE that is read."
coccilib.report.print_report(j0[0], msg)
