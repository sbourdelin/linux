/// Correct error handling for devm_ioremap_resource
///
// Confidence: High
// Copyright: (C) 2016 Amitoj Kaur Chawla
// Keywords: devm,devm_ioremap_resource

virtual context
virtual org
virtual report

// ----------------------------------------------------------------------------

@err depends on context || org || report@
statement S;
expression e;
position j0;
@@

  e = devm_ioremap_resource(...);
* if (!e@j0) S
// ----------------------------------------------------------------------------

@script:python err_org depends on org@
j0 << err.j0;
@@

msg = "Incorrect error handling."
coccilib.org.print_todo(j0[0], msg)

// ----------------------------------------------------------------------------

@script:python err_report depends on report@
j0 << err.j0;
@@

msg = "Incorrect error handling."
coccilib.report.print_report(j0[0], msg)
