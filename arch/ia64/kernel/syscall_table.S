/* SPDX-License-Identifier: GPL-2.0 */

	.rodata
	.align 8
	.globl sys_call_table
sys_call_table:
	data8 sys_ni_syscall		//  This must be sys_ni_syscall!  See ivt.S.
	data8 sys_exit				// 1025
	data8 sys_read
	data8 sys_write
	data8 sys_open
	data8 sys_close
	data8 sys_creat				// 1030
	data8 sys_link
	data8 sys_unlink
	data8 ia64_execve
	data8 sys_chdir
	data8 sys_fchdir			// 1035
	data8 sys_utimes
	data8 sys_mknod
	data8 sys_chmod
	data8 sys_chown
	data8 sys_lseek				// 1040
	data8 sys_getpid
	data8 sys_getppid
	data8 sys_mount
	data8 sys_umount
	data8 sys_setuid			// 1045
	data8 sys_getuid
	data8 sys_geteuid
	data8 sys_ptrace
	data8 sys_access
	data8 sys_sync				// 1050
	data8 sys_fsync
	data8 sys_fdatasync
	data8 sys_kill
	data8 sys_rename
	data8 sys_mkdir				// 1055
	data8 sys_rmdir
	data8 sys_dup
	data8 sys_ia64_pipe
	data8 sys_times
	data8 ia64_brk				// 1060
	data8 sys_setgid
	data8 sys_getgid
	data8 sys_getegid
	data8 sys_acct
	data8 sys_ioctl				// 1065
	data8 sys_fcntl
	data8 sys_umask
	data8 sys_chroot
	data8 sys_ustat
	data8 sys_dup2				// 1070
	data8 sys_setreuid
	data8 sys_setregid
	data8 sys_getresuid
	data8 sys_setresuid
	data8 sys_getresgid			// 1075
	data8 sys_setresgid
	data8 sys_getgroups
	data8 sys_setgroups
	data8 sys_getpgid
	data8 sys_setpgid			// 1080
	data8 sys_setsid
	data8 sys_getsid
	data8 sys_sethostname
	data8 sys_setrlimit
	data8 sys_getrlimit			// 1085
	data8 sys_getrusage
	data8 sys_gettimeofday
	data8 sys_settimeofday
	data8 sys_select
	data8 sys_poll				// 1090
	data8 sys_symlink
	data8 sys_readlink
	data8 sys_uselib
	data8 sys_swapon
	data8 sys_swapoff			// 1095
	data8 sys_reboot
	data8 sys_truncate
	data8 sys_ftruncate
	data8 sys_fchmod
	data8 sys_fchown			// 1100
	data8 ia64_getpriority
	data8 sys_setpriority
	data8 sys_statfs
	data8 sys_fstatfs
	data8 sys_gettid			// 1105
	data8 sys_semget
	data8 sys_semop
	data8 sys_semctl
	data8 sys_msgget
	data8 sys_msgsnd			// 1110
	data8 sys_msgrcv
	data8 sys_msgctl
	data8 sys_shmget
	data8 sys_shmat
	data8 sys_shmdt				// 1115
	data8 sys_shmctl
	data8 sys_syslog
	data8 sys_setitimer
	data8 sys_getitimer
	data8 sys_ni_syscall			// 1120		/* was: ia64_oldstat */
	data8 sys_ni_syscall					/* was: ia64_oldlstat */
	data8 sys_ni_syscall					/* was: ia64_oldfstat */
	data8 sys_vhangup
	data8 sys_lchown
	data8 sys_remap_file_pages		// 1125
	data8 sys_wait4
	data8 sys_sysinfo
	data8 sys_clone
	data8 sys_setdomainname
	data8 sys_newuname			// 1130
	data8 sys_adjtimex
	data8 sys_ni_syscall					/* was: ia64_create_module */
	data8 sys_init_module
	data8 sys_delete_module
	data8 sys_ni_syscall			// 1135		/* was: sys_get_kernel_syms */
	data8 sys_ni_syscall					/* was: sys_query_module */
	data8 sys_quotactl
	data8 sys_bdflush
	data8 sys_sysfs
	data8 sys_personality			// 1140
	data8 sys_ni_syscall		// sys_afs_syscall
	data8 sys_setfsuid
	data8 sys_setfsgid
	data8 sys_getdents
	data8 sys_flock				// 1145
	data8 sys_readv
	data8 sys_writev
	data8 sys_pread64
	data8 sys_pwrite64
	data8 sys_sysctl			// 1150
	data8 sys_mmap
	data8 sys_munmap
	data8 sys_mlock
	data8 sys_mlockall
	data8 sys_mprotect			// 1155
	data8 ia64_mremap
	data8 sys_msync
	data8 sys_munlock
	data8 sys_munlockall
	data8 sys_sched_getparam		// 1160
	data8 sys_sched_setparam
	data8 sys_sched_getscheduler
	data8 sys_sched_setscheduler
	data8 sys_sched_yield
	data8 sys_sched_get_priority_max	// 1165
	data8 sys_sched_get_priority_min
	data8 sys_sched_rr_get_interval
	data8 sys_nanosleep
	data8 sys_ni_syscall			// old nfsservctl
	data8 sys_prctl				// 1170
	data8 sys_getpagesize
	data8 sys_mmap2
	data8 sys_pciconfig_read
	data8 sys_pciconfig_write
	data8 sys_perfmonctl			// 1175
	data8 sys_sigaltstack
	data8 sys_rt_sigaction
	data8 sys_rt_sigpending
	data8 sys_rt_sigprocmask
	data8 sys_rt_sigqueueinfo		// 1180
	data8 sys_rt_sigreturn
	data8 sys_rt_sigsuspend
	data8 sys_rt_sigtimedwait
	data8 sys_getcwd
	data8 sys_capget			// 1185
	data8 sys_capset
	data8 sys_sendfile64
	data8 sys_ni_syscall		// sys_getpmsg (STREAMS)
	data8 sys_ni_syscall		// sys_putpmsg (STREAMS)
	data8 sys_socket			// 1190
	data8 sys_bind
	data8 sys_connect
	data8 sys_listen
	data8 sys_accept
	data8 sys_getsockname			// 1195
	data8 sys_getpeername
	data8 sys_socketpair
	data8 sys_send
	data8 sys_sendto
	data8 sys_recv				// 1200
	data8 sys_recvfrom
	data8 sys_shutdown
	data8 sys_setsockopt
	data8 sys_getsockopt
	data8 sys_sendmsg			// 1205
	data8 sys_recvmsg
	data8 sys_pivot_root
	data8 sys_mincore
	data8 sys_madvise
	data8 sys_newstat			// 1210
	data8 sys_newlstat
	data8 sys_newfstat
	data8 sys_clone2
	data8 sys_getdents64
	data8 sys_getunwind			// 1215
	data8 sys_readahead
	data8 sys_setxattr
	data8 sys_lsetxattr
	data8 sys_fsetxattr
	data8 sys_getxattr			// 1220
	data8 sys_lgetxattr
	data8 sys_fgetxattr
	data8 sys_listxattr
	data8 sys_llistxattr
	data8 sys_flistxattr			// 1225
	data8 sys_removexattr
	data8 sys_lremovexattr
	data8 sys_fremovexattr
	data8 sys_tkill
	data8 sys_futex				// 1230
	data8 sys_sched_setaffinity
	data8 sys_sched_getaffinity
	data8 sys_set_tid_address
	data8 sys_fadvise64_64
	data8 sys_tgkill 			// 1235
	data8 sys_exit_group
	data8 sys_lookup_dcookie
	data8 sys_io_setup
	data8 sys_io_destroy
	data8 sys_io_getevents			// 1240
	data8 sys_io_submit
	data8 sys_io_cancel
	data8 sys_epoll_create
	data8 sys_epoll_ctl
	data8 sys_epoll_wait			// 1245
	data8 sys_restart_syscall
	data8 sys_semtimedop
	data8 sys_timer_create
	data8 sys_timer_settime
	data8 sys_timer_gettime			// 1250
	data8 sys_timer_getoverrun
	data8 sys_timer_delete
	data8 sys_clock_settime
	data8 sys_clock_gettime
	data8 sys_clock_getres			// 1255
	data8 sys_clock_nanosleep
	data8 sys_fstatfs64
	data8 sys_statfs64
	data8 sys_mbind
	data8 sys_get_mempolicy			// 1260
	data8 sys_set_mempolicy
	data8 sys_mq_open
	data8 sys_mq_unlink
	data8 sys_mq_timedsend
	data8 sys_mq_timedreceive		// 1265
	data8 sys_mq_notify
	data8 sys_mq_getsetattr
	data8 sys_kexec_load
	data8 sys_ni_syscall			// reserved for vserver
	data8 sys_waitid			// 1270
	data8 sys_add_key
	data8 sys_request_key
	data8 sys_keyctl
	data8 sys_ioprio_set
	data8 sys_ioprio_get			// 1275
	data8 sys_move_pages
	data8 sys_inotify_init
	data8 sys_inotify_add_watch
	data8 sys_inotify_rm_watch
	data8 sys_migrate_pages			// 1280
	data8 sys_openat
	data8 sys_mkdirat
	data8 sys_mknodat
	data8 sys_fchownat
	data8 sys_futimesat			// 1285
	data8 sys_newfstatat
	data8 sys_unlinkat
	data8 sys_renameat
	data8 sys_linkat
	data8 sys_symlinkat			// 1290
	data8 sys_readlinkat
	data8 sys_fchmodat
	data8 sys_faccessat
	data8 sys_pselect6
	data8 sys_ppoll				// 1295
	data8 sys_unshare
	data8 sys_splice
	data8 sys_set_robust_list
	data8 sys_get_robust_list
	data8 sys_sync_file_range		// 1300
	data8 sys_tee
	data8 sys_vmsplice
	data8 sys_fallocate
	data8 sys_getcpu
	data8 sys_epoll_pwait			// 1305
	data8 sys_utimensat
	data8 sys_signalfd
	data8 sys_ni_syscall
	data8 sys_eventfd
	data8 sys_timerfd_create		// 1310
	data8 sys_timerfd_settime
	data8 sys_timerfd_gettime
	data8 sys_signalfd4
	data8 sys_eventfd2
	data8 sys_epoll_create1			// 1315
	data8 sys_dup3
	data8 sys_pipe2
	data8 sys_inotify_init1
	data8 sys_preadv
	data8 sys_pwritev			// 1320
	data8 sys_rt_tgsigqueueinfo
	data8 sys_recvmmsg
	data8 sys_fanotify_init
	data8 sys_fanotify_mark
	data8 sys_prlimit64			// 1325
	data8 sys_name_to_handle_at
	data8 sys_open_by_handle_at
	data8 sys_clock_adjtime
	data8 sys_syncfs
	data8 sys_setns				// 1330
	data8 sys_sendmmsg
	data8 sys_process_vm_readv
	data8 sys_process_vm_writev
	data8 sys_accept4
	data8 sys_finit_module			// 1335
	data8 sys_sched_setattr
	data8 sys_sched_getattr
	data8 sys_renameat2
	data8 sys_getrandom
	data8 sys_memfd_create			// 1340
	data8 sys_bpf
	data8 sys_execveat
	data8 sys_userfaultfd
	data8 sys_membarrier
	data8 sys_kcmp				// 1345
	data8 sys_mlock2
	data8 sys_copy_file_range
	data8 sys_preadv2
	data8 sys_pwritev2

	.org sys_call_table + 8*NR_syscalls	// guard against failures to increase NR_syscalls
