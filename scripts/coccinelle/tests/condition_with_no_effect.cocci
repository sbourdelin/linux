///Find conditions where if and else branch match
// There can be false positives in cases where the positional 
// information is used (as with lockdep) or where the identity
// is a placeholder for not yet handled cases.
//
// In the Linux kernel though it did not seem to actually report
// false positives except for those that that were documented as 
// being intentional.
// the two known cases are:
//   arch/sh/kernel/traps_64.c:read_opcode()
//        } else if ((pc & 1) == 0) {
//              /* SHcompact */
//              /* TODO : provide handling for this.  We don't really support
//                 user-mode SHcompact yet, and for a kernel fault, this would
//                 have to come from a module built for SHcompact.  */
//              return -EFAULT;
//      } else {
//              /* misaligned */
//              return -EFAULT;
//      }
//   fs/kernfs/file.c:kernfs_fop_open()
//       * Both paths of the branch look the same.  They're supposed to
//       * look that way and give @of->mutex different static lockdep keys.
//       */
//      if (has_mmap)
//              mutex_init(&of->mutex);
//      else
//              mutex_init(&of->mutex);
//
// All other cases look like bugs or at least lack of documentation
//
// Confidence: Moderate
// Copyright: (C) 2016 Nicholas Mc Guire, OSADL.  GPLv2.
// Comments:
// Options: --no-includes --include-headers

virtual context
virtual org
virtual report

@cond@
statement S1;
position p;
@@

<+...
* if@p (...) S1 else S1
...+>

@script:python depends on org@
p << cond.p;
@@

cocci.print_main("WARNING: possible condition with no effect (if == else)",p)

@script:python depends on report@
p << cond.p;
@@

coccilib.report.print_report(p[0],"WARNING: possible condition with no effect (if == else)")
