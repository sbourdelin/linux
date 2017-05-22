======================================
Per-task module auto-load restrictions
======================================


Introduction
============

Usually a request to a kernel feature that is implemented by a module
that is not loaded may trigger automatic module loading feature, allowing
to transparently satisfy userspace, and provide numerous other features
as they are needed. In this case an implicit kernel module load
operation happens.

In most cases to load or unload a kernel module, an explicit operation
happens where programs are required to have ``CAP_SYS_MODULE`` capability
to perform so. However, with implicit module loading, no capabilities are
required, or only ``CAP_NET_ADMIN`` in rare cases where the module has the
'netdev-%s' alias. Historically this was always the case as automatic
module loading is one of the most important and transparent operations
of Linux, users expect that their programs just work, yet, recent cases
showed that this can be abused by unprivileged users or attackers to load
modules that were not updated, or modules that contain bugs and
vulnerabilities.

Currently most of Linux code is in a form of modules, hence, allowing to
control automatic module loading in some cases is as important as the
operation itself, especially in the context where Linux is used in
different appliances.

Restricting automatic module loading allows administratros to have the
appropriate time to update or deny module autoloading in advance. In a
container or sandbox world where apps can be moved from one context to
another, the ability to restrict some containers or apps to load extra
kernel modules will prevent exposing some kernel interfaces that may not
receive the same care as some other parts of the core. The DCCP vulnerability
CVE-2017-6074 that can be triggered by unprivileged, or CVE-2017-7184
in the XFRM framework are some real examples where users or programs are
able to expose such kernel interfaces and escape their sandbox.

The per-task ``modules_autoload_mode`` allow to restrict automatic module
loading per task, preventing the kernel from exposing more of its
interface. This is particularly useful for containers and sandboxes as
noted above, they are restricted from affecting the rest of the system
without affecting its functionality, automatic module loading is still
available for others.


Usage
=====

When the kernel is compiled with modules support ``CONFIG_MODULES``, then:

``PR_SET_MODULES_AUTOLOAD_MODE``:
        Set the current task ``modules_autoload_mode``. When a module
        auto-load request is triggered by current task, then the
        operation has first to satisfy the per-task access mode before
        attempting to implicitly load the module. As an example,
        automatic loading of modules that contain bugs or vulnerabilities
        can be restricted and unprivileged users can no longer abuse such
        interfaces. Once set, this setting is inherited across ``fork(2)``,
        ``clone(2)`` and ``execve(2)``.

        Prior to use, the task must call ``prctl(PR_SET_NO_NEW_PRIVS, 1)``
        or run with ``CAP_SYS_ADMIN`` privileges in its namespace.  If
        these are not true, ``-EACCES`` will be returned.  This requirement
        ensures that unprivileged programs cannot affect the behaviour or
        surprise privileged children.

        Usage:
                ``prctl(PR_SET_MODULES_AUTOLOAD_MODE, mode, 0, 0, 0);``

        The 'mode' argument supports the following values:
        0       There are no restrictions, usually the default unless set
                by parent.
        1       The task must have ``CAP_SYS_MODULE`` to be able to trigger a
                module auto-load operation, or ``CAP_NET_ADMIN`` for modules
                with a 'netdev-%s' alias.
        2       Automatic modules loading is disabled for the current task.

        The mode may only be increased, never decreased, thus ensuring
        that once applied, processes can never relax their setting.


        Returned values:
        0               On success.
        ``-EINVAL``     If 'mode' is not valid, or the operation is not
                        supported.
        ``-EACCES``     If task does not have ``CAP_SYS_ADMIN`` in its namespace
                        or is not running with ``no_new_privs``.
        ``-EPERM``      If 'mode' is less strict than current task
                        ``modules_autoload_mode``.


        Note that even if the per-task ``modules_autoload_mode`` allows to
        auto-load the corresponding modules, automatic module loading
        may still fail due to the global sysctl ``modules_autoload_mode``.
        For more details please see Documentation/sysctl/kernel.txt,
        section "modules_autoload_mode".


        When a request to a kernel module is denied, the module name with the
        corresponding process name and its pid are logged. Administrators can
        use such information to explicitly load the appropriate modules.


``PR_GET_MODULES_AUTOLOAD_MODE``:
        Return the current task ``modules_autoload_mode``.

        Usage:
                ``prctl(PR_GET_MODULES_AUTOLOAD_MODE, 0, 0, 0, 0);``

        Returned values:
        mode            The task's ``modules_autoload_mode``
        ``-ENOSYS``     If the kernel was compiled without ``CONFIG_MODULES``.
