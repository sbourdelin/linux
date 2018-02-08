/// < and > between constants is typically meant to be a shift operation
//# When the left argument is a #define constant, the operation can be
//# meaningful.
///
// Confidence: Moderate
// Copyright: (C) 2018 Julia Lawall, Inria, GPLv2.
// URL: http://coccinelle.lip6.fr/
// Options: --include-headers --include-headers-for-types

virtual patch
virtual context
virtual org
virtual report

@ok@
position p;
expression x,y;
binary operator op = {<,>};
typedef bool;
bool b;
@@

(
x op@p y || ...
|
x op@p y && ...
|
b = x op@p y
|
WARN_ON(x op@p y)
|
WARN_ON_ONCE(x op@p y)
|
BUG_ON(x op@p y)
|
BUILD_BUG_ON(x op@p y)
|
BUILD_BUG_ON_MSG(x op@p y,...)
)

// ----------------------------------------------------------------------------

@depends on patch && !context && !org && !report@
type T;
binary operator op = {<,>};
position p != ok.p;
constant int x, y;
@@

(
 (T)x
- <@p
+ <<
 y
|
 (T)x
- >@p
+ >>
 y
)

@r depends on !patch && (context || org || report)@
type T;
binary operator op = {<,>};
position p != ok.p;
constant int x, y;
position j0 : script:python() { j0[0].file != "" }; // "" in ifdefs
@@

*(T)x op@p@j0 y

// ----------------------------------------------------------------------------

@script:python r_org depends on org@
j0 << r.j0;
@@

msg = "WARNING: Shift rather than comparison expected."
coccilib.org.print_todo(j0[0], msg)

// ----------------------------------------------------------------------------

@script:python r_report depends on report@
j0 << r.j0;
@@

msg = "WARNING: Shift rather than comparison expected."
coccilib.report.print_report(j0[0], msg)

