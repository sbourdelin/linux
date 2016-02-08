/// move assignments out of if conditions
///
//# This script is designed to correct code where assignments exist in if
//# conditions. It is only capable of handling a subset of such problems.
//# Ideally it would handle all checkpatch errors of the following type:
//#	ERROR: do not use assignment in if condition
//#
//# For example:
//#	if(result = myfun())
//#
//# would become:
//#	result = myfun();
//#	if(result)
//
// Confidence: Moderate
// Copyright: (C) 2015 Kris Borer.  GPLv2.
// URL: http://coccinelle.lip6.fr/
// Comments:
// Options: --no-includes --include-headers

virtual patch


// if ( (ret = call()) )
// if ( (ret = call()) < 0 )
@if1@
expression i;
expression E, E2;
statement S1, S2;
binary operator b;
@@

+ i = E;
  if (
(
- (i = E)
+ i
|
- (i = E)
+ i
  b ...
|
- (i = E),
  E2
)
  ) S1 else S2


// if ( ptr->fun && (ret = ptr->fun()) )
@if2@
expression i2;
expression E1, E2;
@@

+ if( E1 ) {
+       i2 = E2;
+       if (i2) {
- if( E1 && (i2 = E2) ) {
  ...
- }
+       }
+ }


// if ( ptr->fun && (ret = ptr->fun()) < 0 )
@if3@
expression i2;
expression E1, E2;
constant c;
binary operator b;
@@

+ if( E1 ) {
+       i2 = E2;
+       if (i2 b c) {
- if( E1 && ((i2 = E2) b c) ) {
  ...
- }
+       }
+ }


// if ( (ret = call()) && ret != -1 )
// if ( (ret = call()) < 0 && ret != -1 )
@if4@
expression i;
expression E, E2;
statement S1, S2;
binary operator b;
@@

+ i = E;
  if (
(
- (i = E)
+ i
|
  (
- (i = E)
+ i
  b
  ...)
)
  && E2 ) S1 else S2


// if ( (ret = call()) && ret != -1 && ret != -2 )
// if ( (ret = call()) < 0 && ret != -1 && ret != -2 )
@if5@
expression i;
expression E, E2, E3;
statement S1, S2;
binary operator b;
@@

+ i = E;
  if (
(
- (i = E)
+ i
|
  (
- (i = E)
+ i
  b
  ...)
)
  && E2 && E3 ) S1 else S2
