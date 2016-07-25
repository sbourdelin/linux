/// Correct error handling for devm_ioremap_resource
///
// Confidence: High
// Copyright: (C) 2016 Amitoj Kaur Chawla
// Keywords: devm, devm_ioremap_resource

virtual patch
virtual context
virtual org
virtual report

@err depends on patch && !context && !org && !report@
expression e,e1;
@@

  e = devm_ioremap_resource(...);
  if(
-    e == NULL
+    IS_ERR(e)
    )
    {
  ...
  return
- e1
+ PTR_ERR(e)
  ;
  }

// ----------------------------------------------------------------------------

@err_context depends on !patch && (context || org || report)@
expression e, e1;
position j0, j1;
@@

  e@j0 = devm_ioremap_resource(...);
  if(
*     e == NULL
    )
    {
  ...
  return
*  e1@j1
  ;
  }

// ----------------------------------------------------------------------------

@script:python err_org depends on org@
j0 << err_context.j0;
j1 << err_context.j1;
@@

msg = "Incorrect error handling."
coccilib.org.print_todo(j0[0], msg)
coccilib.org.print_link(j1[0], "")

// ----------------------------------------------------------------------------

@script:python err_report depends on report@
j0 << err_context.j0;
j1 << err_context.j1;
@@

msg = "Incorrect error handling around line %s." % (j1[0].line)
coccilib.report.print_report(j0[0], msg)
