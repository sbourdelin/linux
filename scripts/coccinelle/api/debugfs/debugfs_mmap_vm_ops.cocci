/// Don't set vma->vm_ops from a debugfs file's ->mmap() implementation.
///
//# Rationale: While a debugfs file's struct file_operations is
//# protected against file removal races through a proxy wrapper
//# automatically provided by the debugfs core, anything installed at
//# vma->vm_ops from ->mmap() isn't: the mm subsystem may and will
//# invoke its members at any time.
//
// Copyright (C): 2016 Nicolai Stange
// Options: --no-includes
//

virtual context
virtual report
virtual org

@unsupp_mmap_impl@
identifier mmap_impl;
identifier filp, vma;
expression e;
position p;
@@

int mmap_impl(struct file *filp, struct vm_area_struct *vma)
{
  ...
  vma->vm_ops@p = e
  ...
}

@unsupp_fops@
identifier fops;
identifier unsupp_mmap_impl.mmap_impl;
@@
struct file_operations fops = {
 .mmap = mmap_impl,
};

@unsupp_fops_usage@
expression name, mode, parent, data;
identifier unsupp_fops.fops;
@@
debugfs_create_file(name, mode, parent, data, &fops)


@context_unsupp_mmap_impl depends on context && unsupp_fops_usage@
identifier unsupp_mmap_impl.mmap_impl;
identifier unsupp_mmap_impl.filp, unsupp_mmap_impl.vma;
expression unsupp_mmap_impl.e;
@@
int mmap_impl(struct file *filp, struct vm_area_struct *vma)
{
  ...
* vma->vm_ops = e
  ...
}

@script:python depends on org && unsupp_fops_usage@
p << unsupp_mmap_impl.p;
@@
coccilib.org.print_todo(p[0], "a debugfs file's ->mmap() must not set ->vm_ops")

@script:python depends on report && unsupp_fops_usage@
p << unsupp_mmap_impl.p;
@@
coccilib.report.print_report(p[0], "a debugfs file's ->mmap() must not set ->vm_ops")
