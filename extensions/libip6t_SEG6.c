/*
 * Shared library add-on to ip6tables to add SEG6 target support
 *
 * Author:
 *       Ahmed Abdelsalam <amsalam20@gmail.com>
 */

#include <stdio.h>
#include <string.h>
#include <xtables.h>
#include <linux/netfilter_ipv6/ip6t_SEG6.h>

struct seg6_names {
	const char *name;
	enum ip6t_seg6_action action;
	const char *desc;
};

/* SEG6 target command-line options */
enum {
	O_SEG6_ACTION,
	O_SEG6_BSID,
	O_SEG6_TABLE,
};

static const struct seg6_names seg6_table[] = {
	{"go-next", IP6T_SEG6_GO_NEXT, "SEG6 go next"},
	{"skip-next", IP6T_SEG6_SKIP_NEXT, "SEG6 skip next"},
	{"go-last", IP6T_SEG6_GO_LAST, "SEG6 go last"},
	{"bind-sid", IP6T_SEG6_BSID, "SRv6 bind SID"},
};

static void print_seg6_action(void)
{
	unsigned int i;

	printf("Valid SEG6 action:\n");
	for (i = 0; i < ARRAY_SIZE(seg6_table); ++i) {
		printf("\t %s", seg6_table[i].name);
		if (seg6_table[i].action == IP6T_SEG6_BSID)
			printf(" --bsid <ip6addr> --bsid-tbl <table_number> ");
		else
			printf("  \t\t\t\t\t");
		printf("  \t%s", seg6_table[i].desc);
		printf("\n");
	}
	printf("\n");
}

static void SEG6_help(void)
{
	printf(
"SEG6 target options:\n"
"--seg6-action action	perform SR-specific action on SRv6 packets\n");
	print_seg6_action();
}

#define s struct ip6t_seg6_info
static const struct xt_option_entry SEG6_opts[] = {
	{.name = "seg6-action", .id = O_SEG6_ACTION, .type = XTTYPE_STRING,
	  .flags = XTOPT_MAND },
	{.name = "bsid", .id = O_SEG6_BSID, .type = XTTYPE_HOST},
	{.name = "bsid-tbl", .id = O_SEG6_TABLE, .type = XTTYPE_UINT32,
	.flags = XTOPT_PUT, XTOPT_POINTER(s, tbl)},
	{}
};
#undef s

static void SEG6_init(struct xt_entry_target *t)
{
	struct ip6t_seg6_info *seg6 = (struct ip6t_seg6_info *)t->data;

	memset(&seg6->bsid, 0, sizeof(struct in6_addr));
	seg6->tbl = 0;
}

static void SEG6_parse(struct xt_option_call *cb)
{
	struct ip6t_seg6_info *seg6 = cb->data;
	unsigned int i;

	xtables_option_parse(cb);
	switch (cb->entry->id) {
	case O_SEG6_ACTION:
		for (i = 0; i < ARRAY_SIZE(seg6_table); ++i)
			if (strncasecmp(seg6_table[i].name, cb->arg, strlen(cb->arg)) == 0) {
				seg6->action = seg6_table[i].action;
				return;
			}
		xtables_error(PARAMETER_PROBLEM, "unknown SEG6 target action \"%s\"", cb->arg);
	case O_SEG6_BSID:
		if (seg6->action != IP6T_SEG6_BSID)
			xtables_error(PARAMETER_PROBLEM,
				      "bsid can be used only with \"bind-sid\" action");
		seg6->bsid = cb->val.haddr.in6;
		break;
	case O_SEG6_TABLE:
		if (seg6->action != IP6T_SEG6_BSID)
			xtables_error(PARAMETER_PROBLEM,
				      "bsid-tbl can be only used with \"bind-sid\" action");
		break;
	}
}

static void SEG6_print(const void *ip, const struct xt_entry_target *target,
			int numeric)
{
	const struct ip6t_seg6_info *seg6 = (const struct ip6t_seg6_info *)target->data;
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(seg6_table); ++i)
		if (seg6_table[i].action == seg6->action)
			break;
	printf(" seg6-action %s", seg6_table[i].name);
	if (seg6->action == IP6T_SEG6_BSID) {
		printf(" bsid %s", xtables_ip6addr_to_numeric(&seg6->bsid));
		printf(" bsid-tbl %d", seg6->tbl);
	}
}

static void SEG6_save(const void *ip, const struct xt_entry_target *target)
{

	const struct ip6t_seg6_info *seg6 = (const struct ip6t_seg6_info *)target->data;
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(seg6_table); ++i)
		if (seg6_table[i].action == seg6->action)
			break;
	printf(" --seg6-action %s", seg6_table[i].name);
	if (seg6->action == IP6T_SEG6_BSID) {
		printf(" --bsid %s", xtables_ip6addr_to_numeric(&seg6->bsid));
		printf(" --bsid-tbl %d", seg6->tbl);
	}
}

static struct xtables_target seg6_tg6_reg = {
	.name		= "SEG6",
	.version        = XTABLES_VERSION,
	.family         = NFPROTO_IPV6,
	.size           = XT_ALIGN(sizeof(struct ip6t_seg6_info)),
	.userspacesize  = XT_ALIGN(sizeof(struct ip6t_seg6_info)),
	.help           = SEG6_help,
	.init           = SEG6_init,
	.print          = SEG6_print,
	.save           = SEG6_save,
	.x6_parse       = SEG6_parse,
	.x6_options     = SEG6_opts,
};

void _init(void)
{
	xtables_register_target(&seg6_tg6_reg);
}
