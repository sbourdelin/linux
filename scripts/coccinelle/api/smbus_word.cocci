/// i2c_smbus_read_word_data() returns native endianness for little-endian
/// bus (it basically has builtin le16_to_cpu). Calling le16_to_cpu on the
/// result breaks support on big endian machines by converting it back.
///
// Confidence: Moderate
// Copyright: (C) 2017 Julia Lawall, Inria. GPLv2.
// URL: http://coccinelle.lip6.fr/
// Options: --no-includes --include-headers
// Keywords: i2c_smbus_read_word_data, le16_to_cpu

virtual context
virtual org
virtual report

// ----------------------------------------------------------------------------

@r depends on context || org || report exists@
expression e, x;
position j0, j1;
@@

* x@j0 = i2c_smbus_read_word_data(...)
... when != x = e
* le16_to_cpu@j1(x)

// ----------------------------------------------------------------------------

@script:python r_org depends on org@
j0 << r.j0;
j1 << r.j1;
@@

msg = "le16_to_cpu not needed on i2c_smbus_read_word_data result."
coccilib.org.print_todo(j0[0], msg)
coccilib.org.print_link(j1[0], "")

// ----------------------------------------------------------------------------

@script:python r_report depends on report@
j0 << r.j0;
j1 << r.j1;
@@

msg = "le16_to_cpu not needed on i2c_smbus_read_word_data result around line %s." % (j1[0].line)
coccilib.report.print_report(j0[0], msg)
