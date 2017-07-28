/*
 * linux/include/linux/sunrpc/auth_gss.h
 *
 * Declarations for RPCSEC_GSS
 *
 * Dug Song <dugsong@monkey.org>
 * Andy Adamson <andros@umich.edu>
 * Bruce Fields <bfields@umich.edu>
 * Copyright (c) 2000 The Regents of the University of Michigan
 */

#ifndef _LINUX_SUNRPC_AUTH_GSS_H
#define _LINUX_SUNRPC_AUTH_GSS_H

#ifdef __KERNEL__
#include <linux/sunrpc/auth.h>
#include <linux/sunrpc/svc.h>
#include <linux/sunrpc/gss_api.h>

#define RPC_GSS_VERSION		1
#define RPC_GSS3_VERSION	3

#define MAXSEQ 0x80000000 /* maximum legal sequence number, from rfc 2203 */

enum rpc_gss_proc {
	RPC_GSS_PROC_DATA = 0,
	RPC_GSS_PROC_INIT = 1,
	RPC_GSS_PROC_CONTINUE_INIT = 2,
	RPC_GSS_PROC_DESTROY = 3,
	RPC_GSS_PROC_BIND_CHANNEL = 4,	/* GSS2, not used */
	RPC_GSS_PROC_CREATE = 5,	/* GSS3 */
	RPC_GSS_PROC_LIST = 6		/* GSS3 */
};

enum rpc_gss_svc {
	RPC_GSS_SVC_NONE = 1,
	RPC_GSS_SVC_INTEGRITY = 2,
	RPC_GSS_SVC_PRIVACY = 3
};

/* on-the-wire gss cred: */
struct rpc_gss_wire_cred {
	u32			gc_v;		/* version */
	u32			gc_proc;	/* control procedure */
	u32			gc_seq;		/* sequence number */
	u32			gc_svc;		/* service */
	struct xdr_netobj	gc_ctx;		/* context handle */
};

/* on-the-wire gss verifier: */
struct rpc_gss_wire_verf {
	u32			gv_flavor;
	struct xdr_netobj	gv_verf;
};

/* return from gss NULL PROC init sec context */
struct rpc_gss_init_res {
	struct xdr_netobj	gr_ctx;		/* context handle */
	u32			gr_major;	/* major status */
	u32			gr_minor;	/* minor status */
	u32			gr_win;		/* sequence window */
	struct xdr_netobj	gr_token;	/* token */
};

struct gss3_assert_list {
	struct list_head	assert_list;
	spinlock_t		assert_lock;
};

/* The gss_cl_ctx struct holds all the information the rpcsec_gss client
 * code needs to know about a single security context.  In particular,
 * gc_gss_ctx is the context handle that is used to do gss-api calls, while
 * gc_wire_ctx is the context handle that is used to identify the context on
 * the wire when communicating with a server. */

struct gss_cl_ctx {
	atomic_t		count;
	u32			gc_v;
	enum rpc_gss_proc	gc_proc;
	u32			gc_seq;
	spinlock_t		gc_seq_lock;
	struct gss_ctx		*gc_gss_ctx;
	struct xdr_netobj	gc_wire_ctx;
	struct xdr_netobj	gc_acceptor;
	u32			gc_win;
	unsigned long		gc_expiry;
	struct gss3_assert_list	gc_alist;
	struct rcu_head		gc_rcu;
};

struct gss_upcall_msg;
struct gss_cred {
	struct rpc_cred		gc_base;
	enum rpc_gss_svc	gc_service;
	struct gss_cl_ctx __rcu	*gc_ctx;
	struct gss_upcall_msg	*gc_upcall;
	const char		*gc_principal;
	unsigned long		gc_upcall_timestamp;
};

/** GSS3 */
enum gss3_type {
	GSS3_LABEL = 0,
	GSS3_PRIVS = 1,
};

struct gss3_chan_binding {
	u32	cb_len;
	void	*cb_binding;
};

struct gss3_mp_auth {
	u32	mp_handle_len;
	void	*mp_handle;
	u32	*mp_mic_len;
	void	*mp_mic;	/* header mic */
};

struct gss3_label {
	u32			la_lfs;
	u32			la_pi;
	struct xdr_netobj	la_label;
};

struct gss3_privs {
	struct xdr_netobj	pr_name;
	struct xdr_netobj	pr_data;
};

struct gss3_assertion_u {
	u32	au_type;
	union {
		struct gss3_label	au_label;
		struct gss3_privs	au_privs;
	} u;
};

struct gss3_create_args {
	struct gss3_mp_auth		*ca_mp_auth;
	struct gss3_chan_binding	*ca_chan_bind;
	u32			ca_num;
	struct gss3_assertion_u	*ca_assertions;
};

struct gss3_create_res {
	u32		cr_hlen;
	void		*cr_handle;
	struct gss3_mp_auth		*cr_mp_auth;
	struct gss3_chan_binding	*cr_chan_bind;
	u32			cr_num;
	struct gss3_assertion_u	*cr_assertions;
};

#endif /* __KERNEL__ */
#endif /* _LINUX_SUNRPC_AUTH_GSS_H */

