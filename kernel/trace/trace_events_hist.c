/*
 * trace_events_hist - trace event hist triggers
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * Copyright (C) 2015 Tom Zanussi <tom.zanussi@linux.intel.com>
 */

#include <linux/module.h>
#include <linux/kallsyms.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/stacktrace.h>
#include <linux/tracefs.h>

#include "tracing_map.h"
#include "trace.h"

#define SYNTHETIC_EVENT_SYSTEM "synthetic"

struct hist_field;

typedef u64 (*hist_field_fn_t) (struct hist_field *field,
				struct tracing_map_elt *elt,
				struct ring_buffer_event *rbe,
				void *event);

#define HIST_FIELD_OPERANDS_MAX	2
#define HIST_ASSIGNMENT_MAX	4
#define HIST_ACTIONS_MAX	8

enum field_op_id {
	FIELD_OP_NONE,
	FIELD_OP_PLUS,
	FIELD_OP_MINUS,
	FIELD_OP_UNARY_MINUS,
};

struct hist_var_ref {
	struct hist_trigger_data	*hist_data;
	unsigned int			idx;
	bool				pending;
	char				*pending_system;
	char				*pending_event_name;
	char				*pending_var_name;
};

struct hist_field {
	struct ftrace_event_field	*field;
	unsigned long			flags;
	hist_field_fn_t			fn;
	unsigned int			size;
	unsigned int			offset;
	unsigned int			is_signed;
	struct hist_var_ref		var_ref;
	unsigned int			var_ref_idx;
	char				*var_name;
	struct hist_field		*operands[HIST_FIELD_OPERANDS_MAX];
	enum field_op_id		operator;
	char				*name;
	u64				var_val;
	unsigned int			var_idx;
	bool                            read_once;
};

static u64 hist_field_none(struct hist_field *field,
			   struct tracing_map_elt *elt,
			   struct ring_buffer_event *rbe,
			   void *event)
{
	return 0;
}

static u64 hist_field_counter(struct hist_field *field,
			      struct tracing_map_elt *elt,
			      struct ring_buffer_event *rbe,
			      void *event)
{
	return 1;
}

static u64 hist_field_string(struct hist_field *hist_field,
			     struct tracing_map_elt *elt,
			     struct ring_buffer_event *rbe,
			     void *event)
{
	char *addr = (char *)(event + hist_field->field->offset);

	return (u64)(unsigned long)addr;
}

static u64 hist_field_dynstring(struct hist_field *hist_field,
				struct tracing_map_elt *elt,
				struct ring_buffer_event *rbe,
				void *event)
{
	u32 str_item = *(u32 *)(event + hist_field->field->offset);
	int str_loc = str_item & 0xffff;
	char *addr = (char *)(event + str_loc);

	return (u64)(unsigned long)addr;
}

static u64 hist_field_pstring(struct hist_field *hist_field,
			      struct tracing_map_elt *elt,
			      struct ring_buffer_event *rbe,
			      void *event)
{
	char **addr = (char **)(event + hist_field->field->offset);

	return (u64)(unsigned long)*addr;
}

static u64 hist_field_plus(struct hist_field *hist_field,
			   struct tracing_map_elt *elt,
			   struct ring_buffer_event *rbe,
			   void *event)
{
	struct hist_field *operand1 = hist_field->operands[0];
	struct hist_field *operand2 = hist_field->operands[1];

	u64 val1 = operand1->fn(operand1, elt, rbe, event);
	u64 val2 = operand2->fn(operand2, elt, rbe, event);

	return val1 + val2;
}

static u64 hist_field_minus(struct hist_field *hist_field,
			    struct tracing_map_elt *elt,
			    struct ring_buffer_event *rbe,
			    void *event)
{
	struct hist_field *operand1 = hist_field->operands[0];
	struct hist_field *operand2 = hist_field->operands[1];

	u64 val1 = operand1->fn(operand1, elt, rbe, event);
	u64 val2 = operand2->fn(operand2, elt, rbe, event);

	return val1 - val2;
}

static u64 hist_field_unary_minus(struct hist_field *hist_field,
				  struct tracing_map_elt *elt,
				  struct ring_buffer_event *rbe,
				  void *event)
{
	struct hist_field *operand = hist_field->operands[0];

	s64 sval = (s64)operand->fn(operand, elt, rbe, event);
	u64 val = (u64)-sval;

	return val;
}

static u64 hist_field_log2(struct hist_field *hist_field,
			   struct tracing_map_elt *elt,
			   struct ring_buffer_event *rbe,
			   void *event)
{
	struct hist_field *operand = hist_field->operands[0];

	u64 val = operand->fn(operand, elt, rbe, event);

	return (u64) ilog2(roundup_pow_of_two(val));
}

#define DEFINE_HIST_FIELD_FN(type)					\
	static u64 hist_field_##type(struct hist_field *hist_field,	\
				     struct tracing_map_elt *elt,	\
				     struct ring_buffer_event *rbe,	\
				     void *event)			\
{									\
	type *addr = (type *)(event + hist_field->field->offset);	\
									\
	return (u64)(unsigned long)*addr;				\
}

DEFINE_HIST_FIELD_FN(s64);
DEFINE_HIST_FIELD_FN(u64);
DEFINE_HIST_FIELD_FN(s32);
DEFINE_HIST_FIELD_FN(u32);
DEFINE_HIST_FIELD_FN(s16);
DEFINE_HIST_FIELD_FN(u16);
DEFINE_HIST_FIELD_FN(s8);
DEFINE_HIST_FIELD_FN(u8);

#define for_each_hist_field(i, hist_data)	\
	for ((i) = 0; (i) < (hist_data)->n_fields; (i)++)

#define for_each_hist_val_field(i, hist_data)	\
	for ((i) = 0; (i) < (hist_data)->n_vals; (i)++)

#define for_each_hist_key_field(i, hist_data)	\
	for ((i) = (hist_data)->n_vals; (i) < (hist_data)->n_fields; (i)++)

#define HIST_STACKTRACE_DEPTH	16
#define HIST_STACKTRACE_SIZE	(HIST_STACKTRACE_DEPTH * sizeof(unsigned long))
#define HIST_STACKTRACE_SKIP	5

#define HITCOUNT_IDX		0
#define HIST_KEY_SIZE_MAX	(MAX_FILTER_STR_VAL + HIST_STACKTRACE_SIZE)

enum hist_field_flags {
	HIST_FIELD_FL_HITCOUNT		= 1,
	HIST_FIELD_FL_KEY		= 2,
	HIST_FIELD_FL_STRING		= 4,
	HIST_FIELD_FL_HEX		= 8,
	HIST_FIELD_FL_SYM		= 16,
	HIST_FIELD_FL_SYM_OFFSET	= 32,
	HIST_FIELD_FL_EXECNAME		= 64,
	HIST_FIELD_FL_SYSCALL		= 128,
	HIST_FIELD_FL_STACKTRACE	= 256,
	HIST_FIELD_FL_LOG2		= 512,
	HIST_FIELD_FL_VAR		= 1024,
	HIST_FIELD_FL_VAR_ONLY		= 2048,
	HIST_FIELD_FL_VAR_REF		= 4096,
	HIST_FIELD_FL_EXPR		= 8192,
	HIST_FIELD_FL_TIMESTAMP		= 16384,
	HIST_FIELD_FL_TIMESTAMP_USECS	= 32768,
};

struct hist_trigger_attrs {
	char		*keys_str;
	char		*vals_str;
	char		*assignment_str[HIST_ASSIGNMENT_MAX];
	unsigned int	n_assignments;
	char		*sort_key_str;
	char		*name;
	bool		pause;
	bool		cont;
	bool		clear;
	bool		ts_in_usecs;
	unsigned int	map_bits;
	char		*action_str[HIST_ACTIONS_MAX];
	unsigned int	n_actions;
};

struct hist_trigger_data {
	struct hist_field               *fields[TRACING_MAP_FIELDS_MAX];
	struct hist_field               *var_refs[TRACING_MAP_VARS_MAX];
	unsigned int			n_vals;
	unsigned int			n_keys;
	unsigned int			n_fields;
	unsigned int			n_vars;
	unsigned int			n_var_refs;
	unsigned int			key_size;
	struct tracing_map_sort_key	sort_keys[TRACING_MAP_SORT_KEYS_MAX];
	unsigned int			n_sort_keys;
	struct trace_event_file		*event_file;
	struct hist_trigger_attrs	*attrs;
	struct tracing_map		*map;
	bool				enable_timestamps;
	struct action_data		*actions[HIST_ACTIONS_MAX];
	unsigned int			n_actions;
};

static u64 hist_field_timestamp(struct hist_field *hist_field,
				struct tracing_map_elt *elt,
				struct ring_buffer_event *rbe,
				void *event)
{
	struct hist_trigger_data *hist_data = elt->map->private_data;
	struct trace_array *tr = hist_data->event_file->tr;

	u64 ts = ring_buffer_event_time_stamp(rbe);

	if (hist_data->attrs->ts_in_usecs && trace_clock_in_ns(tr))
		ts = ns2usecs(ts);

	return ts;
}

static LIST_HEAD(hist_var_list);

struct hist_var_data {
	struct list_head list;
	struct hist_trigger_data *hist_data;
};

static struct hist_field *check_var_ref(struct hist_field *hist_field,
					struct hist_trigger_data *var_data,
					unsigned int var_idx)
{
	struct hist_field *found = NULL;

	if (hist_field && hist_field->flags & HIST_FIELD_FL_VAR_REF) {
		if (hist_field->var_ref.idx == var_idx &&
		    hist_field->var_ref.hist_data == var_data) {
			found = hist_field;
		}
	}

	return found;
}

static struct hist_field *find_var_ref(struct hist_trigger_data *hist_data,
				       struct hist_trigger_data *var_data,
				       unsigned int var_idx)
{
	struct hist_field *hist_field, *found = NULL;
	unsigned int i, j;

	for_each_hist_field(i, hist_data) {
		hist_field = hist_data->fields[i];
		found = check_var_ref(hist_field, var_data, var_idx);
		if (found)
			return found;

		for (j = 0; j < HIST_FIELD_OPERANDS_MAX; j++) {
			struct hist_field *operand;
			operand = hist_field->operands[j];
			found = check_var_ref(operand, var_data, var_idx);
			if (found)
				return found;
		}
	}

	return found;
}

static struct hist_field *find_any_var_ref(struct hist_trigger_data *hist_data,
					   unsigned int var_idx)
{
	struct hist_field *found = NULL;
	struct hist_var_data *var_data;

	list_for_each_entry(var_data, &hist_var_list, list) {
		found = find_var_ref(var_data->hist_data, hist_data, var_idx);
		if (found)
			break;
	}

	return found;
}

static bool check_var_refs(struct hist_trigger_data *hist_data)
{
	struct hist_field *field;
	bool found = false;
	int i;

	for_each_hist_field(i, hist_data) {
		field = hist_data->fields[i];
		if (field && field->flags & HIST_FIELD_FL_VAR) {
			if (find_any_var_ref(hist_data, field->var_ref.idx)) {
				found = true;
				break;
			}
		}
	}

	return found;
}

static struct hist_var_data *find_hist_vars(struct hist_trigger_data *hist_data)
{
	struct hist_var_data *var_data, *found = NULL;

	list_for_each_entry(var_data, &hist_var_list, list) {
		if (var_data->hist_data == hist_data) {
			found = var_data;
			break;
		}
	}

	return found;
}

static bool has_hist_vars(struct hist_trigger_data *hist_data)
{
	struct hist_field *hist_field;
	bool found = false;
	int i;

	for_each_hist_field(i, hist_data) {
		hist_field = hist_data->fields[i];
		if (hist_field && hist_field->flags & HIST_FIELD_FL_VAR) {
			found = true;
			break;
		}
	}

	return found;
}

static int save_hist_vars(struct hist_trigger_data *hist_data)
{
	struct hist_var_data *var_data;

	var_data = find_hist_vars(hist_data);
	if (var_data)
		return 0;

	var_data = kzalloc(sizeof(*var_data), GFP_KERNEL);
	if (!var_data)
		return -ENOMEM;

	var_data->hist_data = hist_data;
	list_add(&var_data->list, &hist_var_list);

	return 0;
}

static int remove_hist_vars(struct hist_trigger_data *hist_data)
{
	struct hist_var_data *var_data;

	var_data = find_hist_vars(hist_data);
	if (!var_data)
		return -EINVAL;

	if (check_var_refs(hist_data))
		return -EINVAL;

	list_del(&var_data->list);

	return 0;
}

static struct hist_field *find_var_field(struct hist_trigger_data *hist_data,
					 const char *system,
					 const char *event_name,
					 const char *var_name)
{
	struct hist_field *hist_field, *found = NULL;
	int i;

	for_each_hist_field(i, hist_data) {
		hist_field = hist_data->fields[i];
		if (hist_field && hist_field->flags & HIST_FIELD_FL_VAR) {
			if (strcmp(hist_field->var_name, var_name) == 0) {
				found = hist_field;
				break;
			}
		}
	}

	return found;
}

struct hist_elt_data {
	char *comm;
	u64 *var_ref_vals;
};

static u64 hist_field_var_ref(struct hist_field *hist_field,
			      struct tracing_map_elt *elt,
			      struct ring_buffer_event *rbe,
			      void *event)
{
	struct hist_elt_data *elt_data;
	u64 var_val = 0;

	elt_data = elt->private_data;
	var_val = elt_data->var_ref_vals[hist_field->var_ref_idx];

	return var_val;
}

static bool resolve_var_refs(struct hist_trigger_data *hist_data,
			     void *key,
			     u64 *var_ref_vals, bool self)
{
	struct hist_trigger_data *var_data;
	struct tracing_map_elt *var_elt;
	struct hist_field *hist_field;
	bool resolved = true;
	unsigned int i, var_idx;
	u64 var_val = 0;

	for (i = 0; i < hist_data->n_var_refs; i++) {
		hist_field = hist_data->var_refs[i];
		var_idx = hist_field->var_ref.idx;
		var_data = hist_field->var_ref.hist_data;

		if ((self && var_data != hist_data) ||
		    (!self && var_data == hist_data))
			continue;

		var_elt = tracing_map_lookup(var_data->map, key);
		if (!var_elt) {
			resolved = false;
			break;
		}

		if (!tracing_map_var_set(var_elt, var_idx)) {
			resolved = false;
			break;
		}

		if (self || !hist_field->read_once)
			var_val = tracing_map_read_var(var_elt, var_idx);
		else
			var_val = tracing_map_read_var_once(var_elt, var_idx);

		var_ref_vals[i] = var_val;
	}

	return resolved;
}

static const char *hist_field_name(struct hist_field *field)
{
	const char *field_name = NULL;

	if (field->field)
		field_name = field->field->name;
	else if (field->flags & HIST_FIELD_FL_LOG2)
		field_name = hist_field_name(field->operands[0]);
	else if (field->flags & HIST_FIELD_FL_TIMESTAMP)
		field_name = "common_timestamp";
	else if (field->flags & HIST_FIELD_FL_EXPR ||
		 field->flags & HIST_FIELD_FL_VAR_REF)
		field_name = field->name;

	return field_name;
}

static hist_field_fn_t select_value_fn(int field_size, int field_is_signed)
{
	hist_field_fn_t fn = NULL;

	switch (field_size) {
	case 8:
		if (field_is_signed)
			fn = hist_field_s64;
		else
			fn = hist_field_u64;
		break;
	case 4:
		if (field_is_signed)
			fn = hist_field_s32;
		else
			fn = hist_field_u32;
		break;
	case 2:
		if (field_is_signed)
			fn = hist_field_s16;
		else
			fn = hist_field_u16;
		break;
	case 1:
		if (field_is_signed)
			fn = hist_field_s8;
		else
			fn = hist_field_u8;
		break;
	}

	return fn;
}

static int parse_map_size(char *str)
{
	unsigned long size, map_bits;
	int ret;

	strsep(&str, "=");
	if (!str) {
		ret = -EINVAL;
		goto out;
	}

	ret = kstrtoul(str, 0, &size);
	if (ret)
		goto out;

	map_bits = ilog2(roundup_pow_of_two(size));
	if (map_bits < TRACING_MAP_BITS_MIN ||
	    map_bits > TRACING_MAP_BITS_MAX)
		ret = -EINVAL;
	else
		ret = map_bits;
 out:
	return ret;
}

static void destroy_hist_trigger_attrs(struct hist_trigger_attrs *attrs)
{
	unsigned int i;

	if (!attrs)
		return;

	for (i = 0; i < attrs->n_assignments; i++)
		kfree(attrs->assignment_str[i]);

	for (i = 0; i < attrs->n_actions; i++)
		kfree(attrs->action_str[i]);

	kfree(attrs->name);
	kfree(attrs->sort_key_str);
	kfree(attrs->keys_str);
	kfree(attrs->vals_str);
	kfree(attrs);
}

static int parse_action(char *str, struct hist_trigger_attrs *attrs)
{
	int ret = 0;

	if (attrs->n_actions == HIST_ACTIONS_MAX)
		return -EINVAL;

	return ret;
}

static int parse_assignment(char *str, struct hist_trigger_attrs *attrs)
{
	int ret = 0;

	if ((strncmp(str, "key=", strlen("key=")) == 0) ||
	    (strncmp(str, "keys=", strlen("keys=")) == 0))
		attrs->keys_str = kstrdup(str, GFP_KERNEL);
	else if (((strncmp(str, "val=", strlen("val=")) == 0) ||
		  (strncmp(str, "vals=", strlen("vals=")) == 0) ||
		  (strncmp(str, "values=", strlen("values=")) == 0)))
		attrs->vals_str = kstrdup(str, GFP_KERNEL);
	else if (strncmp(str, "sort=", strlen("sort=")) == 0)
		attrs->sort_key_str = kstrdup(str, GFP_KERNEL);
	else if (strncmp(str, "name=", strlen("name=")) == 0)
		attrs->name = kstrdup(str, GFP_KERNEL);
	else if (strncmp(str, "size=", strlen("size=")) == 0) {
		int map_bits = parse_map_size(str);

		if (map_bits < 0) {
			ret = map_bits;
			goto out;
		}
		attrs->map_bits = map_bits;
	} else {
		char *assignment;

		if (attrs->n_assignments == HIST_ASSIGNMENT_MAX) {
			ret = -EINVAL;
			goto out;
		}

		assignment = kstrdup(str, GFP_KERNEL);
		if (!assignment) {
			ret = -ENOMEM;
			goto out;
		}

		attrs->assignment_str[attrs->n_assignments++] = assignment;
	}
 out:
	return ret;
}

static struct hist_trigger_attrs *parse_hist_trigger_attrs(char *trigger_str)
{
	struct hist_trigger_attrs *attrs;
	int ret = 0;

	attrs = kzalloc(sizeof(*attrs), GFP_KERNEL);
	if (!attrs)
		return ERR_PTR(-ENOMEM);

	while (trigger_str) {
		char *str = strsep(&trigger_str, ":");
		if (strchr(str, '=')) {
			ret = parse_assignment(str, attrs);
			if (ret)
				goto free;
		} else if (strcmp(str, "pause") == 0)
			attrs->pause = true;
		else if ((strcmp(str, "cont") == 0) ||
			 (strcmp(str, "continue") == 0))
			attrs->cont = true;
		else if (strcmp(str, "clear") == 0)
			attrs->clear = true;
		else {
			ret = parse_action(str, attrs);
			if (ret < 0)
				goto free;
			if (!ret) {
				ret = -EINVAL;
				goto free;
			}
		}
	}

	if (!attrs->keys_str) {
		ret = -EINVAL;
		goto free;
	}

	return attrs;
 free:
	destroy_hist_trigger_attrs(attrs);

	return ERR_PTR(ret);
}

static inline void save_comm(char *comm, struct task_struct *task)
{
	if (!task->pid) {
		strcpy(comm, "<idle>");
		return;
	}

	if (WARN_ON_ONCE(task->pid < 0)) {
		strcpy(comm, "<XXX>");
		return;
	}

	memcpy(comm, task->comm, TASK_COMM_LEN);
}

static void hist_trigger_elt_data_free(struct tracing_map_elt *elt)
{
	struct hist_elt_data *private_data = elt->private_data;

	kfree(private_data->comm);
	kfree(private_data);
}

static int hist_trigger_elt_data_alloc(struct tracing_map_elt *elt)
{
	struct hist_trigger_data *hist_data = elt->map->private_data;
	unsigned int size = TASK_COMM_LEN + 1;
	struct hist_elt_data *elt_data;
	struct hist_field *key_field;
	unsigned int i;

	elt->private_data = elt_data = kzalloc(sizeof(*elt_data), GFP_KERNEL);
	if (!elt_data)
		return -ENOMEM;

	for_each_hist_key_field(i, hist_data) {
		key_field = hist_data->fields[i];

		if (key_field->flags & HIST_FIELD_FL_EXECNAME) {
			elt_data->comm = kzalloc(size, GFP_KERNEL);
			if (!elt_data->comm) {
				kfree(elt_data);
				elt->private_data = NULL;
				return -ENOMEM;
			}
			break;
		}
	}

	return 0;
}

static void hist_trigger_elt_data_copy(struct tracing_map_elt *to,
				       struct tracing_map_elt *from)
{
	struct hist_elt_data *from_data = from->private_data;
	struct hist_elt_data *to_data = to->private_data;

	memcpy(to_data, from_data, sizeof(*to));

	if (from_data->comm)
		memcpy(to_data->comm, from_data->comm, TASK_COMM_LEN + 1);
}

static void hist_trigger_elt_data_init(struct tracing_map_elt *elt)
{
	struct hist_elt_data *private_data = elt->private_data;

	if (private_data->comm)
		save_comm(private_data->comm, current);
}

static const struct tracing_map_ops hist_trigger_elt_data_ops = {
	.elt_alloc	= hist_trigger_elt_data_alloc,
	.elt_copy	= hist_trigger_elt_data_copy,
	.elt_free	= hist_trigger_elt_data_free,
	.elt_init	= hist_trigger_elt_data_init,
};

static char *expr_str(struct hist_field *field)
{
	char *expr = kzalloc(MAX_FILTER_STR_VAL, GFP_KERNEL);

	if (!expr)
		return NULL;

	if (field->operator == FIELD_OP_UNARY_MINUS) {
		char *subexpr;

		strcat(expr, "-(");
		subexpr = expr_str(field->operands[0]);
		if (!subexpr) {
			kfree(expr);
			return NULL;
		}
		strcat(expr, subexpr);
		strcat(expr, ")");

		return expr;
	}

	strcat(expr, hist_field_name(field->operands[0]));

	switch (field->operator) {
	case FIELD_OP_MINUS:
		strcat(expr, "-");
		break;
	case FIELD_OP_PLUS:
		strcat(expr, "+");
		break;
	default:
		kfree(expr);
		return NULL;
	}

	strcat(expr, hist_field_name(field->operands[1]));

	return expr;
}

static int contains_operator(char *str)
{
	enum field_op_id field_op = FIELD_OP_NONE;
	char *op;

	op = strpbrk(str, "+-");
	if (!op)
		return FIELD_OP_NONE;

	switch (*op) {
	case '-':
		if (*str == '-')
			field_op = FIELD_OP_UNARY_MINUS;
		else
			field_op = FIELD_OP_MINUS;
		break;
	case '+':
		field_op = FIELD_OP_PLUS;
		break;
	default:
		break;
	}

	return field_op;
}

static void destroy_hist_field(struct hist_field *hist_field)
{
	unsigned int i;

	if (!hist_field)
		return;

	for (i = 0; i < HIST_FIELD_OPERANDS_MAX; i++)
		destroy_hist_field(hist_field->operands[i]);

	kfree(hist_field->var_name);
	kfree(hist_field->name);

	kfree(hist_field);
}

static struct hist_field *create_hist_field(struct ftrace_event_field *field,
					    unsigned long flags,
					    char *var_name)
{
	struct hist_field *hist_field;

	if (field && is_function_field(field))
		return NULL;

	hist_field = kzalloc(sizeof(struct hist_field), GFP_KERNEL);
	if (!hist_field)
		return NULL;
	hist_field->is_signed = false;

	if (flags & HIST_FIELD_FL_EXPR)
		goto out; /* caller will populate */

	if (flags & HIST_FIELD_FL_VAR_REF) {
		hist_field->fn = hist_field_var_ref;
		goto out;
	}

	if (flags & HIST_FIELD_FL_HITCOUNT) {
		hist_field->fn = hist_field_counter;
		goto out;
	}

	if (flags & HIST_FIELD_FL_STACKTRACE) {
		hist_field->fn = hist_field_none;
		goto out;
	}

	if (flags & HIST_FIELD_FL_LOG2) {
		unsigned long fl = flags & ~HIST_FIELD_FL_LOG2;
		hist_field->fn = hist_field_log2;
		hist_field->operands[0] = create_hist_field(field, fl, NULL);
		hist_field->size = hist_field->operands[0]->size;
		goto out;
	}

	if (flags & HIST_FIELD_FL_TIMESTAMP) {
		hist_field->fn = hist_field_timestamp;
		hist_field->size = sizeof(u64);
		goto out;
	}

	if (WARN_ON_ONCE(!field))
		goto out;

	if (is_string_field(field)) {
		flags |= HIST_FIELD_FL_STRING;
		hist_field->size = MAX_FILTER_STR_VAL;

		if (field->filter_type == FILTER_STATIC_STRING)
			hist_field->fn = hist_field_string;
		else if (field->filter_type == FILTER_DYN_STRING)
			hist_field->fn = hist_field_dynstring;
		else
			hist_field->fn = hist_field_pstring;
	} else {
		hist_field->size = field->size;
		hist_field->fn = select_value_fn(field->size,
						 field->is_signed);
		if (!hist_field->fn) {
			destroy_hist_field(hist_field);
			return NULL;
		}
	}
 out:
	hist_field->field = field;
	hist_field->flags = flags;
	if (var_name)
		hist_field->var_name = kstrdup(var_name, GFP_KERNEL);

	return hist_field;
}

static void destroy_hist_fields(struct hist_trigger_data *hist_data)
{
	unsigned int i;

	for (i = 0; i < TRACING_MAP_FIELDS_MAX; i++) {
		if (hist_data->fields[i]) {
			destroy_hist_field(hist_data->fields[i]);
			hist_data->fields[i] = NULL;
		}
	}
}

struct synthetic_event_field {
	char *name;
	struct hist_field *var_ref;
};

struct synthetic_event {
	struct list_head		list;
	char				*name;
	struct synthetic_event_field	*fields;
	unsigned int			n_fields;
	u64				*var_ref_vals;
	struct trace_event_class	class;
	struct trace_event_call		call;
	struct tracepoint		*tp;
};

struct action_data;

typedef void (*action_fn_t) (struct hist_trigger_data *hist_data,
			     struct tracing_map_elt *elt, void *rec,
			     struct ring_buffer_event *rbe,
			     struct action_data *data, u64 *var_ref_vals);

struct action_data {
	action_fn_t	fn;
	unsigned int	var_ref_idx;
	struct synthetic_event *synthetic_event;
};

static LIST_HEAD(synthetic_events_list);
static DEFINE_MUTEX(synthetic_event_mutex);

static void free_synthetic_tracepoint(struct tracepoint *tp)
{
	if (!tp)
		return;

	kfree(tp->name);
	kfree(tp);
}

static struct tracepoint *alloc_synthetic_tracepoint(char *name)
{
	struct tracepoint *tp;
	int ret = 0;

	tp = kzalloc(sizeof(*tp), GFP_KERNEL);
	if (!tp) {
		ret = -ENOMEM;
		goto free;
	}

	tp->name = kstrdup(name, GFP_KERNEL);
	if (!tp->name) {
		ret = -ENOMEM;
		goto free;
	}

	return tp;
 free:
	free_synthetic_tracepoint(tp);

	return ERR_PTR(ret);
}

static inline void trace_synthetic(struct synthetic_event *event,
				   u64 *var_ref_vals,
				   unsigned int var_ref_idx)
{
	struct tracepoint *tp = event->tp;

	if (unlikely(atomic_read(&tp->key.enabled) > 0)) {
		struct tracepoint_func *it_func_ptr;
		void *it_func;
		void *__data;

		if (!(cpu_online(raw_smp_processor_id())))
			return;

		it_func_ptr = rcu_dereference_sched((tp)->funcs);
		if (it_func_ptr) {
			do {
				it_func = (it_func_ptr)->func;
				__data = (it_func_ptr)->data;
				((void(*)(void *__data, u64* var_ref_vals, unsigned int var_ref_idx))(it_func))(__data, var_ref_vals, var_ref_idx);
			} while ((++it_func_ptr)->func);
		}
	}
}

static struct synthetic_event *find_synthetic_event(char *name);

static void reset_pending_var_refs(struct hist_trigger_data *hist_data,
				   struct synthetic_event *event)
{
	const char *system, *event_name, *pending_system, *pending_event_name;
	struct synthetic_event_field *se_field;
	struct trace_event_call *call;
	struct hist_field *ref_field;
	unsigned int i;

	call = hist_data->event_file->event_call;
	system = call->class->system;
	event_name = trace_event_name(call);

	for (i = 0; i < event->n_fields; i++) {
		se_field = &event->fields[i];
		ref_field = se_field->var_ref;

		pending_system = ref_field->var_ref.pending_system;
		if ((pending_system) && (strcmp(system, pending_system) != 0))
			continue;

		pending_event_name = ref_field->var_ref.pending_event_name;
		if (pending_event_name &&
		    (strcmp(event_name, pending_event_name) == 0))
			ref_field->var_ref.pending = true;
	}
}

static void unresolve_pending_var_refs(struct hist_trigger_data *hist_data)
{
	struct synthetic_event *event;

	mutex_lock(&synthetic_event_mutex);
	list_for_each_entry(event, &synthetic_events_list, list)
		reset_pending_var_refs(hist_data, event);
	mutex_unlock(&synthetic_event_mutex);
}

static bool resolve_pending_var_refs(struct synthetic_event *event)
{
	struct hist_var_data *var_data;
	struct hist_field *var_field = NULL, *ref_field = NULL;
	struct synthetic_event_field *se_field;
	char *system, *event_name, *var_name;
	bool pending = false;
	unsigned int i;

	for (i = 0; i < event->n_fields; i++) {
		se_field = &event->fields[i];
		ref_field = se_field->var_ref;
		if (!ref_field->var_ref.pending)
			continue;

		pending = true;

		system = ref_field->var_ref.pending_system;
		event_name = ref_field->var_ref.pending_event_name;
		var_name = ref_field->var_ref.pending_var_name;

		list_for_each_entry(var_data, &hist_var_list, list) {
			var_field = find_var_field(var_data->hist_data, system,
						   event_name, var_name);
			if (!var_field)
				continue;

			ref_field->var_ref.idx = var_field->var_ref.idx;
			ref_field->var_ref.hist_data = var_data->hist_data;
			if (!ref_field->name)
				ref_field->name = kstrdup(var_field->var_name, GFP_KERNEL);
			ref_field->var_ref.pending = false;
			pending = false;
		}
	}

	return !pending;
}

static struct hist_field *parse_var_ref(char *system, char *event_name,
					char *var_name, bool defer)
{
	struct hist_var_data *var_data;
	struct hist_field *var_field = NULL, *ref_field = NULL;

	list_for_each_entry(var_data, &hist_var_list, list) {
		var_field = find_var_field(var_data->hist_data, system,
					   event_name, var_name);
		if (var_field)
			break;
	}

	if (var_field) {
		unsigned long flags = HIST_FIELD_FL_VAR_REF;

		ref_field = create_hist_field(NULL, flags, NULL);
		if (ref_field) {
			ref_field->var_ref.idx = var_field->var_ref.idx;
			ref_field->var_ref.hist_data = var_data->hist_data;
			ref_field->name = kstrdup(var_field->var_name, GFP_KERNEL);
		}
	} else if (defer) {
		unsigned long flags = HIST_FIELD_FL_VAR_REF;

		ref_field = create_hist_field(NULL, flags, NULL);
		if (ref_field) {
			char *str;

			ref_field->var_ref.pending = true;
			if (system) {
				str = kstrdup(system, GFP_KERNEL);
				ref_field->var_ref.pending_system = str;
			}
			if (event_name) {
				str = kstrdup(event_name, GFP_KERNEL);
				ref_field->var_ref.pending_event_name = str;
			}
			str = kstrdup(var_name, GFP_KERNEL);
			ref_field->var_ref.pending_var_name = str;
		}
	}

	return ref_field;
}

static struct ftrace_event_field *
parse_field(struct hist_trigger_data *hist_data, struct trace_event_file *file,
	    char *field_str, unsigned long *flags)
{
	struct ftrace_event_field *field = NULL;
	char *field_name;

	field_name = strsep(&field_str, ".");
	if (field_str) {
		if (strcmp(field_str, "hex") == 0)
			*flags |= HIST_FIELD_FL_HEX;
		else if (strcmp(field_str, "sym") == 0)
			*flags |= HIST_FIELD_FL_SYM;
		else if (strcmp(field_str, "sym-offset") == 0)
			*flags |= HIST_FIELD_FL_SYM_OFFSET;
		else if ((strcmp(field_str, "execname") == 0) &&
			 (strcmp(field_name, "common_pid") == 0))
			*flags |= HIST_FIELD_FL_EXECNAME;
		else if (strcmp(field_str, "syscall") == 0)
			*flags |= HIST_FIELD_FL_SYSCALL;
		else if (strcmp(field_str, "log2") == 0)
			*flags |= HIST_FIELD_FL_LOG2;
		else if (strcmp(field_str, "usecs") == 0)
			*flags |= HIST_FIELD_FL_TIMESTAMP_USECS;
		else
			return ERR_PTR(-EINVAL);
	}

	if (strcmp(field_name, "common_timestamp") == 0) {
		*flags |= HIST_FIELD_FL_TIMESTAMP;
		hist_data->enable_timestamps = true;
		if (*flags & HIST_FIELD_FL_TIMESTAMP_USECS)
			hist_data->attrs->ts_in_usecs = true;
	} else {
		field = trace_find_event_field(file->event_call, field_name);
		if (!field)
			return ERR_PTR(-EINVAL);
	}

	return field;
}

struct hist_field *parse_atom(struct hist_trigger_data *hist_data,
			      struct trace_event_file *file, char *str,
			      unsigned long *flags, char *var_name)
{
	struct ftrace_event_field *field = NULL;
	struct hist_field *hist_field = NULL;
	int ret = 0;

	hist_field = parse_var_ref(NULL, NULL, str, false);
	if (hist_field) {
		hist_data->var_refs[hist_data->n_var_refs] = hist_field;
		hist_field->var_ref_idx = hist_data->n_var_refs++;
		return hist_field;
	}

	field = parse_field(hist_data, file, str, flags);
	if (IS_ERR(field)) {
		ret = PTR_ERR(field);
		goto out;
	}

	hist_field = create_hist_field(field, *flags, var_name);
	if (!hist_field) {
		ret = -ENOMEM;
		goto out;
	}

	return hist_field;
 out:
	return ERR_PTR(ret);
}

static struct hist_field *parse_expr(struct hist_trigger_data *hist_data,
				     struct trace_event_file *file,
				     char *str, unsigned long flags,
				     char *var_name);

static struct hist_field *parse_unary(struct hist_trigger_data *hist_data,
				      struct trace_event_file *file,
				      char *str, unsigned long flags,
				      char *var_name)
{
	struct hist_field *operand1, *expr = NULL;
	unsigned long operand_flags;
	char *operand1_str;
	int ret = 0;
	char *s;

	// we support only -(xxx) i.e. explicit parens required

	str++; // skip leading '-'

	s = strchr(str, '(');
	if (s)
		str++;
	else {
		ret = -EINVAL;
		goto free;
	}

	s = strchr(str, ')');
	if (s)
		*s = '\0';
	else {
		ret = -EINVAL; // no closing ')'
		goto free;
	}

	operand1_str = strsep(&str, "(");
	if (!operand1_str)
		goto free;

	flags |= HIST_FIELD_FL_EXPR;
	expr = create_hist_field(NULL, flags, var_name);
	if (!expr) {
		ret = -ENOMEM;
		goto free;
	}

	operand_flags = 0;
	operand1 = parse_expr(hist_data, file, str, operand_flags, NULL);
	if (IS_ERR(operand1)) {
		ret = PTR_ERR(operand1);
		goto free;
	}

	if (operand1 == NULL) {
		operand_flags = 0;
		operand1 = parse_atom(hist_data, file, operand1_str,
				      &operand_flags, NULL);
		if (IS_ERR(operand1)) {
			ret = PTR_ERR(operand1);
			goto free;
		}
	}

	expr->fn = hist_field_unary_minus;
	expr->operands[0] = operand1;
	expr->operator = FIELD_OP_UNARY_MINUS;
	expr->name = expr_str(expr);

	return expr;
 free:
	return ERR_PTR(ret);
}

static struct hist_field *parse_expr(struct hist_trigger_data *hist_data,
				     struct trace_event_file *file,
				     char *str, unsigned long flags,
				     char *var_name)
{
	struct hist_field *operand1 = NULL, *operand2 = NULL, *expr = NULL;
	unsigned long operand_flags;
	int field_op, ret = -EINVAL;
	char *sep, *operand1_str;

	field_op = contains_operator(str);
	if (field_op == FIELD_OP_NONE)
		return NULL;

	if (field_op == FIELD_OP_UNARY_MINUS)
		return parse_unary(hist_data, file, str, flags, var_name);

	switch (field_op) {
	case FIELD_OP_MINUS:
		sep = "-";
		break;
	case FIELD_OP_PLUS:
		sep = "+";
		break;
	default:
		goto free;
	}

	operand1_str = strsep(&str, sep);
	if (!operand1_str || !str)
		goto free;

	operand_flags = 0;
	operand1 = parse_atom(hist_data, file, operand1_str,
			      &operand_flags, NULL);
	if (IS_ERR(operand1)) {
		ret = PTR_ERR(operand1);
		operand1 = NULL;
		goto free;
	}

	// rest of string could be another expression e.g. b+c in a+b+c
	operand_flags = 0;
	operand2 = parse_expr(hist_data, file, str, operand_flags, NULL);
	if (IS_ERR(operand2)) {
		ret = PTR_ERR(operand2);
		operand2 = NULL;
		goto free;
	}
	if (!operand2) {
		operand_flags = 0;
		operand2 = parse_atom(hist_data, file, str,
				      &operand_flags, NULL);
		if (IS_ERR(operand2)) {
			ret = PTR_ERR(operand2);
			operand2 = NULL;
			goto free;
		}
	}

	flags |= HIST_FIELD_FL_EXPR;
	expr = create_hist_field(NULL, flags, var_name);
	if (!expr) {
		ret = -ENOMEM;
		goto free;
	}

	operand1->read_once = true;
	operand2->read_once = true;

	expr->operands[0] = operand1;
	expr->operands[1] = operand2;
	expr->operator = field_op;
	expr->name = expr_str(expr);

	switch (field_op) {
	case FIELD_OP_MINUS:
		expr->fn = hist_field_minus;
		break;
	case FIELD_OP_PLUS:
		expr->fn = hist_field_plus;
		break;
	default:
		goto free;
	}

	return expr;
 free:
	destroy_hist_field(operand1);
	destroy_hist_field(operand2);
	destroy_hist_field(expr);

	return ERR_PTR(ret);
}

static int create_hitcount_val(struct hist_trigger_data *hist_data)
{
	hist_data->fields[HITCOUNT_IDX] =
		create_hist_field(NULL, HIST_FIELD_FL_HITCOUNT, NULL);
	if (!hist_data->fields[HITCOUNT_IDX])
		return -ENOMEM;

	hist_data->n_vals++;

	if (WARN_ON(hist_data->n_vals > TRACING_MAP_VALS_MAX))
		return -EINVAL;

	return 0;
}

static int create_val_field(struct hist_trigger_data *hist_data,
			    unsigned int val_idx,
			    struct trace_event_file *file,
			    char *field_str, char *var_name)
{
	struct hist_field *hist_field;
	unsigned long flags = 0;
	char *token;
	int ret = 0;

	if (WARN_ON(val_idx >= TRACING_MAP_VALS_MAX))
		return -EINVAL;

	if (var_name)
		flags |= HIST_FIELD_FL_VAR | HIST_FIELD_FL_VAR_ONLY;

	token = strsep(&field_str, "=");
	if (field_str) {
		if (var_name) {
			ret = -EINVAL;
			goto out;
		}
		var_name = token;
		flags |= HIST_FIELD_FL_VAR;
	} else
		field_str = token;

	hist_field = parse_expr(hist_data, file, field_str, flags, var_name);
	if (IS_ERR(hist_field)) {
		ret = PTR_ERR(hist_field);
		goto out;
	}

	if (!hist_field) {
		hist_field = parse_atom(hist_data, file, field_str,
					&flags, var_name);
		if (IS_ERR(hist_field)) {
			ret = PTR_ERR(hist_field);
			goto out;
		}
	}

	hist_data->fields[val_idx] = hist_field;

	++hist_data->n_vals;

	if (WARN_ON(hist_data->n_vals > TRACING_MAP_VALS_MAX))
		ret = -EINVAL;
 out:
	return ret;
}

static int create_val_fields(struct hist_trigger_data *hist_data,
			     struct trace_event_file *file)
{
	char *fields_str, *field_str;
	unsigned int i, j = 1;
	int ret;

	ret = create_hitcount_val(hist_data);
	if (ret)
		goto out;

	fields_str = hist_data->attrs->vals_str;
	if (!fields_str)
		goto out;

	for (i = 0, j = 1; i < TRACING_MAP_VALS_MAX &&
		     j < TRACING_MAP_VALS_MAX; i++) {
		field_str = strsep(&fields_str, ",");
		if (!field_str)
			break;

		if (strcmp(field_str, "hitcount") == 0)
			continue;

		ret = create_val_field(hist_data, j++, file, field_str, NULL);
		if (ret)
			goto out;
	}

	if (fields_str && (strcmp(fields_str, "hitcount") != 0))
		ret = -EINVAL;
 out:
	return ret;
}

static int create_key_field(struct hist_trigger_data *hist_data,
			    unsigned int key_idx,
			    unsigned int key_offset,
			    struct trace_event_file *file,
			    char *field_str)
{
	struct hist_field *hist_field;
	unsigned long flags = 0;
	unsigned int key_size;
	char *var_name;
	int ret = 0;

	if (WARN_ON(key_idx >= TRACING_MAP_FIELDS_MAX))
		return -EINVAL;

	flags |= HIST_FIELD_FL_KEY;

	var_name = strsep(&field_str, "=");
	if (field_str)
		flags |= HIST_FIELD_FL_VAR;
	else {
		field_str = var_name;
		var_name = NULL;
	}

	if (strcmp(field_str, "stacktrace") == 0) {
		flags |= HIST_FIELD_FL_STACKTRACE;
		key_size = sizeof(unsigned long) * HIST_STACKTRACE_DEPTH;
		hist_field = create_hist_field(NULL, flags, var_name);
	} else {
		hist_field = parse_expr(hist_data, file, field_str, flags,
					var_name);
		if (IS_ERR(hist_field)) {
			ret = PTR_ERR(hist_field);
			goto out;
		}

		if (!hist_field) {
			hist_field = parse_atom(hist_data, file, field_str,
						&flags, var_name);
			if (IS_ERR(hist_field)) {
				ret = PTR_ERR(hist_field);
				goto out;
			}
		}

		if (hist_field->flags & HIST_FIELD_FL_VAR_REF) {
			destroy_hist_field(hist_field);
			ret = -EINVAL;
			goto out;
		}

		key_size = hist_field->size;
	}

	hist_data->fields[key_idx] = hist_field;

	key_size = ALIGN(key_size, sizeof(u64));
	hist_data->fields[key_idx]->size = key_size;
	hist_data->fields[key_idx]->offset = key_offset;
	hist_data->key_size += key_size;
	if (hist_data->key_size > HIST_KEY_SIZE_MAX) {
		ret = -EINVAL;
		goto out;
	}

	hist_data->n_keys++;

	if (WARN_ON(hist_data->n_keys > TRACING_MAP_KEYS_MAX))
		return -EINVAL;

	ret = key_size;
 out:
	return ret;
}

static int create_key_fields(struct hist_trigger_data *hist_data,
			     struct trace_event_file *file)
{
	unsigned int i, key_offset = 0, n_vals = hist_data->n_vals;
	char *fields_str, *field_str;
	int ret = -EINVAL;

	fields_str = hist_data->attrs->keys_str;
	if (!fields_str)
		goto out;

	strsep(&fields_str, "=");
	if (!fields_str)
		goto out;

	for (i = n_vals; i < n_vals + TRACING_MAP_KEYS_MAX; i++) {
		field_str = strsep(&fields_str, ",");
		if (!field_str)
			break;

		ret = create_key_field(hist_data, i, key_offset,
				       file, field_str);
		if (ret < 0)
			goto out;

		key_offset += ret;
	}

	if (fields_str) {
		ret = -EINVAL;
		goto out;
	}

	ret = 0;
 out:
	return ret;
}

static int create_var_fields(struct hist_trigger_data *hist_data,
			     struct trace_event_file *file)
{
	unsigned int i, j = hist_data->n_vals;
	char *str, *var;
	int ret = 0;

	for (i = 0; i < hist_data->attrs->n_assignments; i++) {

		str = hist_data->attrs->assignment_str[i];

		var = strsep(&str, "=");
		if (!str) {
			ret = -EINVAL;
			goto out;
		}

		ret = create_val_field(hist_data, j++, file, str, var);
		if (ret)
			goto out;
	}
 out:
	return ret;
}

static int create_hist_fields(struct hist_trigger_data *hist_data,
			      struct trace_event_file *file)
{
	int ret;

	ret = create_val_fields(hist_data, file);
	if (ret)
		goto out;

	ret = create_var_fields(hist_data, file);
	if (ret)
		goto out;

	ret = create_key_fields(hist_data, file);
	if (ret)
		goto out;

	hist_data->n_fields = hist_data->n_vals + hist_data->n_keys;
 out:
	return ret;
}

static int is_descending(const char *str)
{
	if (!str)
		return 0;

	if (strcmp(str, "descending") == 0)
		return 1;

	if (strcmp(str, "ascending") == 0)
		return 0;

	return -EINVAL;
}

static int create_sort_keys(struct hist_trigger_data *hist_data)
{
	char *fields_str = hist_data->attrs->sort_key_str;
	struct tracing_map_sort_key *sort_key;
	int descending, ret = 0;
	unsigned int i, j;

	hist_data->n_sort_keys = 1; /* we always have at least one, hitcount */

	if (!fields_str)
		goto out;

	strsep(&fields_str, "=");
	if (!fields_str) {
		ret = -EINVAL;
		goto out;
	}

	for (i = 0; i < TRACING_MAP_SORT_KEYS_MAX; i++) {
		struct hist_field *hist_field;
		char *field_str, *field_name;
		const char *test_name;

		sort_key = &hist_data->sort_keys[i];

		field_str = strsep(&fields_str, ",");
		if (!field_str) {
			if (i == 0)
				ret = -EINVAL;
			break;
		}

		if ((i == TRACING_MAP_SORT_KEYS_MAX - 1) && fields_str) {
			ret = -EINVAL;
			break;
		}

		field_name = strsep(&field_str, ".");
		if (!field_name) {
			ret = -EINVAL;
			break;
		}

		if ((strcmp(field_name, "hitcount") == 0)) {
			descending = is_descending(field_str);
			if (descending < 0) {
				ret = descending;
				break;
			}
			sort_key->descending = descending;
			continue;
		}

		for (j = 1; j < hist_data->n_fields; j++) {
			hist_field = hist_data->fields[j];
			test_name = hist_field_name(hist_field);
			if (strcmp(field_name, test_name) == 0) {
				sort_key->field_idx = j;
				descending = is_descending(field_str);
				if (descending < 0) {
					ret = descending;
					goto out;
				}
				sort_key->descending = descending;
				break;
			}
		}
		if (j == hist_data->n_fields) {
			ret = -EINVAL;
			break;
		}
	}
	hist_data->n_sort_keys = i;
 out:
	return ret;
}

static int create_tracing_map_fields(struct hist_trigger_data *hist_data)
{
	struct tracing_map *map = hist_data->map;
	struct ftrace_event_field *field;
	struct hist_field *hist_field;
	int i, idx;

	for_each_hist_field(i, hist_data) {
		hist_field = hist_data->fields[i];
		if (hist_field->flags & HIST_FIELD_FL_KEY) {
			tracing_map_cmp_fn_t cmp_fn;

			field = hist_field->field;

			if (hist_field->flags & HIST_FIELD_FL_STACKTRACE)
				cmp_fn = tracing_map_cmp_none;
			else if (!field)
				cmp_fn = tracing_map_cmp_num(hist_field->size,
							     hist_field->is_signed);
			else if (is_string_field(field))
				cmp_fn = tracing_map_cmp_string;
			else
				cmp_fn = tracing_map_cmp_num(field->size,
							     field->is_signed);
			idx = tracing_map_add_key_field(map,
							hist_field->offset,
							cmp_fn);
		} else if (!(hist_field->flags & HIST_FIELD_FL_VAR))
			idx = tracing_map_add_sum_field(map);

		if (idx < 0)
			return idx;

		if (hist_field->flags & HIST_FIELD_FL_VAR) {
			idx = tracing_map_add_var(map);
			if (idx < 0)
				return idx;
			hist_field->var_ref.idx = idx;
			hist_data->n_vars++;
		}
	}

	return 0;
}

static int add_synthetic_var_refs(struct hist_trigger_data *hist_data,
				  struct synthetic_event *event)
{
	unsigned int i, var_ref_idx = hist_data->n_var_refs;

	for (i = 0; i < event->n_fields; i++) {
		struct hist_field *var_ref = event->fields[i].var_ref;

		hist_data->var_refs[hist_data->n_var_refs++] = var_ref;
	}

	return var_ref_idx;
}

static void destroy_actions(struct hist_trigger_data *hist_data)
{
	unsigned int i;

	for (i = 0; i < hist_data->n_actions; i++) {
		struct action_data *data = hist_data->actions[i];

		kfree(data);
	}
}

static int create_actions(struct hist_trigger_data *hist_data)
{
	unsigned int i;
	int ret = 0;
	char *str;

	for (i = 0; i < hist_data->attrs->n_actions; i++) {
		str = hist_data->attrs->action_str[i];
	}

	return ret;
}

static void print_actions(struct seq_file *m,
			  struct hist_trigger_data *hist_data,
			  struct tracing_map_elt *elt)
{
	unsigned int i;

	for (i = 0; i < hist_data->n_actions; i++) {
		struct action_data *data = hist_data->actions[i];
	}
}

static void print_actions_spec(struct seq_file *m,
			       struct hist_trigger_data *hist_data)
{
	unsigned int i;

	for (i = 0; i < hist_data->n_actions; i++) {
		struct action_data *data = hist_data->actions[i];
	}
}

static void destroy_hist_data(struct hist_trigger_data *hist_data)
{
	if (!hist_data)
		return;

	destroy_hist_trigger_attrs(hist_data->attrs);
	destroy_hist_fields(hist_data);
	tracing_map_destroy(hist_data->map);
	destroy_actions(hist_data);
	kfree(hist_data);
}

static struct hist_trigger_data *
create_hist_data(unsigned int map_bits,
		 struct hist_trigger_attrs *attrs,
		 struct trace_event_file *file)
{
	const struct tracing_map_ops *map_ops = NULL;
	struct hist_trigger_data *hist_data;
	int ret = 0;

	hist_data = kzalloc(sizeof(*hist_data), GFP_KERNEL);
	if (!hist_data)
		return ERR_PTR(-ENOMEM);

	hist_data->attrs = attrs;

	ret = create_hist_fields(hist_data, file);
	if (ret)
		goto free;

	ret = create_sort_keys(hist_data);
	if (ret)
		goto free;

	map_ops = &hist_trigger_elt_data_ops;

	hist_data->map = tracing_map_create(map_bits, hist_data->key_size,
					    map_ops, hist_data);
	if (IS_ERR(hist_data->map)) {
		ret = PTR_ERR(hist_data->map);
		hist_data->map = NULL;
		goto free;
	}

	ret = create_tracing_map_fields(hist_data);
	if (ret)
		goto free;

	hist_data->event_file = file;
 out:
	return hist_data;
 free:
	hist_data->attrs = NULL;

	destroy_hist_data(hist_data);

	hist_data = ERR_PTR(ret);

	goto out;
}

static void hist_trigger_elt_update(struct hist_trigger_data *hist_data,
				    struct tracing_map_elt *elt, void *rec,
				    struct ring_buffer_event *rbe,
				    u64 *var_ref_vals)
{
	struct hist_field *hist_field;
	unsigned int i, var_idx;
	u64 hist_val;
	struct hist_elt_data *elt_data;

	elt_data = elt->private_data;
	elt_data->var_ref_vals = var_ref_vals;

	for_each_hist_val_field(i, hist_data) {
		hist_field = hist_data->fields[i];
		hist_val = hist_field->fn(hist_field, elt, rbe, rec);
		if (hist_field->flags & HIST_FIELD_FL_VAR) {
			var_idx = hist_field->var_ref.idx;
			tracing_map_set_var(elt, var_idx, hist_val);
			if (hist_field->flags & HIST_FIELD_FL_VAR_ONLY)
				continue;
		}
		tracing_map_update_sum(elt, i, hist_val);
	}

	for_each_hist_key_field(i, hist_data) {
		hist_field = hist_data->fields[i];
		if (hist_field->flags & HIST_FIELD_FL_VAR) {
			hist_val = hist_field->fn(hist_field, elt, rbe, rec);
			var_idx = hist_field->var_ref.idx;
			tracing_map_set_var(elt, var_idx, hist_val);
		}
	}
}

static inline void add_to_key(char *compound_key, void *key,
			      struct hist_field *key_field, void *rec)
{
	size_t size = key_field->size;

	if (key_field->flags & HIST_FIELD_FL_STRING) {
		struct ftrace_event_field *field;

		field = key_field->field;
		if (field->filter_type == FILTER_DYN_STRING)
			size = *(u32 *)(rec + field->offset) >> 16;
		else if (field->filter_type == FILTER_PTR_STRING)
			size = strlen(key);
		else if (field->filter_type == FILTER_STATIC_STRING)
			size = field->size;

		/* ensure NULL-termination */
		if (size > key_field->size - 1)
			size = key_field->size - 1;
	}

	memcpy(compound_key + key_field->offset, key, size);
}

static void
hist_trigger_actions(struct hist_trigger_data *hist_data,
		     struct tracing_map_elt *elt, void *rec,
		     struct ring_buffer_event *rbe, u64 *var_ref_vals)
{
	struct action_data *data;
	unsigned int i;

	for (i = 0; i < hist_data->n_actions; i++) {
		data = hist_data->actions[i];
		data->fn(hist_data, elt, rec, rbe, data, var_ref_vals);
	}
}

static void event_hist_trigger(struct event_trigger_data *data, void *rec,
			       struct ring_buffer_event *rbe)
{
	struct hist_trigger_data *hist_data = data->private_data;
	bool use_compound_key = (hist_data->n_keys > 1);
	unsigned long entries[HIST_STACKTRACE_DEPTH];
	u64 var_ref_vals[TRACING_MAP_VARS_MAX];
	char compound_key[HIST_KEY_SIZE_MAX];
	struct stack_trace stacktrace;
	struct hist_field *key_field;
	struct tracing_map_elt *elt = NULL;
	u64 field_contents;
	void *key = NULL;
	unsigned int i;

	memset(compound_key, 0, hist_data->key_size);

	for_each_hist_key_field(i, hist_data) {
		key_field = hist_data->fields[i];

		if (key_field->flags & HIST_FIELD_FL_STACKTRACE) {
			stacktrace.max_entries = HIST_STACKTRACE_DEPTH;
			stacktrace.entries = entries;
			stacktrace.nr_entries = 0;
			stacktrace.skip = HIST_STACKTRACE_SKIP;

			memset(stacktrace.entries, 0, HIST_STACKTRACE_SIZE);
			save_stack_trace(&stacktrace);

			key = entries;
		} else {
			field_contents = key_field->fn(key_field, elt, rbe, rec);
			if (key_field->flags & HIST_FIELD_FL_STRING) {
				key = (void *)(unsigned long)field_contents;
				use_compound_key = true;
			} else
				key = (void *)&field_contents;
		}
		if (use_compound_key)
			add_to_key(compound_key, key, key_field, rec);
	}

	if (use_compound_key)
		key = compound_key;

	if (hist_data->n_var_refs &&
	    !resolve_var_refs(hist_data, key, var_ref_vals, false))
		return;

	elt = tracing_map_insert(hist_data->map, key);
	if (!elt)
		return;

	hist_trigger_elt_update(hist_data, elt, rec, rbe, var_ref_vals);

	if (resolve_var_refs(hist_data, key, var_ref_vals, true))
		hist_trigger_actions(hist_data, elt, rec, rbe, var_ref_vals);
}

static void hist_trigger_stacktrace_print(struct seq_file *m,
					  unsigned long *stacktrace_entries,
					  unsigned int max_entries)
{
	char str[KSYM_SYMBOL_LEN];
	unsigned int spaces = 8;
	unsigned int i;

	for (i = 0; i < max_entries; i++) {
		if (stacktrace_entries[i] == ULONG_MAX)
			return;

		seq_printf(m, "%*c", 1 + spaces, ' ');
		sprint_symbol(str, stacktrace_entries[i]);
		seq_printf(m, "%s\n", str);
	}
}

static void
hist_trigger_entry_print(struct seq_file *m,
			 struct hist_trigger_data *hist_data, void *key,
			 struct tracing_map_elt *elt)
{
	struct hist_field *key_field;
	char str[KSYM_SYMBOL_LEN];
	bool multiline = false;
	const char *field_name;
	unsigned int i;
	u64 uval;

	seq_puts(m, "{ ");

	for_each_hist_key_field(i, hist_data) {
		key_field = hist_data->fields[i];

		if (i > hist_data->n_vals)
			seq_puts(m, ", ");

		field_name = hist_field_name(key_field);

		if (key_field->flags & HIST_FIELD_FL_HEX) {
			uval = *(u64 *)(key + key_field->offset);
			seq_printf(m, "%s: %llx", field_name, uval);
		} else if (key_field->flags & HIST_FIELD_FL_SYM) {
			uval = *(u64 *)(key + key_field->offset);
			sprint_symbol_no_offset(str, uval);
			seq_printf(m, "%s: [%llx] %-45s", field_name,
				   uval, str);
		} else if (key_field->flags & HIST_FIELD_FL_SYM_OFFSET) {
			uval = *(u64 *)(key + key_field->offset);
			sprint_symbol(str, uval);
			seq_printf(m, "%s: [%llx] %-55s", field_name,
				   uval, str);
		} else if (key_field->flags & HIST_FIELD_FL_EXECNAME) {
			char *comm = elt->private_data;

			uval = *(u64 *)(key + key_field->offset);
			seq_printf(m, "%s: %-16s[%10llu]", field_name,
				   comm, uval);
		} else if (key_field->flags & HIST_FIELD_FL_SYSCALL) {
			const char *syscall_name;

			uval = *(u64 *)(key + key_field->offset);
			syscall_name = get_syscall_name(uval);
			if (!syscall_name)
				syscall_name = "unknown_syscall";

			seq_printf(m, "%s: %-30s[%3llu]", field_name,
				   syscall_name, uval);
		} else if (key_field->flags & HIST_FIELD_FL_STACKTRACE) {
			seq_puts(m, "stacktrace:\n");
			hist_trigger_stacktrace_print(m,
						      key + key_field->offset,
						      HIST_STACKTRACE_DEPTH);
			multiline = true;
		} else if (key_field->flags & HIST_FIELD_FL_LOG2) {
			seq_printf(m, "%s: ~ 2^%-2llu", field_name,
				   *(u64 *)(key + key_field->offset));
		} else if (key_field->flags & HIST_FIELD_FL_STRING) {
			seq_printf(m, "%s: %-50s", field_name,
				   (char *)(key + key_field->offset));
		} else {
			uval = *(u64 *)(key + key_field->offset);
			seq_printf(m, "%s: %10llu", field_name, uval);
		}
	}

	if (!multiline)
		seq_puts(m, " ");

	seq_puts(m, "}");

	seq_printf(m, " hitcount: %10llu",
		   tracing_map_read_sum(elt, HITCOUNT_IDX));

	for (i = 1; i < hist_data->n_vals; i++) {
		field_name = hist_field_name(hist_data->fields[i]);

		if (hist_data->fields[i]->flags & HIST_FIELD_FL_HEX) {
			seq_printf(m, "  %s: %10llx", field_name,
				   tracing_map_read_sum(elt, i));
		} else {
			seq_printf(m, "  %s: %10llu", field_name,
				   tracing_map_read_sum(elt, i));
		}
	}

	seq_puts(m, "\n");
}

static int print_entries(struct seq_file *m,
			 struct hist_trigger_data *hist_data)
{
	struct tracing_map_sort_entry **sort_entries = NULL;
	struct tracing_map *map = hist_data->map;
	int i, n_entries;

	n_entries = tracing_map_sort_entries(map, hist_data->sort_keys,
					     hist_data->n_sort_keys,
					     &sort_entries);
	if (n_entries < 0)
		return n_entries;

	for (i = 0; i < n_entries; i++)
		hist_trigger_entry_print(m, hist_data,
					 sort_entries[i]->key,
					 sort_entries[i]->elt);

	tracing_map_destroy_sort_entries(sort_entries, n_entries);

	return n_entries;
}

static void hist_trigger_show(struct seq_file *m,
			      struct event_trigger_data *data, int n)
{
	struct hist_trigger_data *hist_data;
	int n_entries, ret = 0;

	if (n > 0)
		seq_puts(m, "\n\n");

	seq_puts(m, "# event histogram\n#\n# trigger info: ");
	data->ops->print(m, data->ops, data);
	seq_puts(m, "#\n\n");

	hist_data = data->private_data;
	n_entries = print_entries(m, hist_data);
	if (n_entries < 0) {
		ret = n_entries;
		n_entries = 0;
	}

	seq_printf(m, "\nTotals:\n    Hits: %llu\n    Entries: %u\n    Dropped: %llu\n",
		   (u64)atomic64_read(&hist_data->map->hits),
		   n_entries, (u64)atomic64_read(&hist_data->map->drops));
}

static int hist_show(struct seq_file *m, void *v)
{
	struct event_trigger_data *data;
	struct trace_event_file *event_file;
	int n = 0, ret = 0;

	mutex_lock(&event_mutex);

	event_file = event_file_data(m->private);
	if (unlikely(!event_file)) {
		ret = -ENODEV;
		goto out_unlock;
	}

	list_for_each_entry_rcu(data, &event_file->triggers, list) {
		if (data->cmd_ops->trigger_type == ETT_EVENT_HIST)
			hist_trigger_show(m, data, n++);
	}

 out_unlock:
	mutex_unlock(&event_mutex);

	return ret;
}

static int event_hist_open(struct inode *inode, struct file *file)
{
	return single_open(file, hist_show, file);
}

const struct file_operations event_hist_fops = {
	.open = event_hist_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = single_release,
};

static const char *get_hist_field_flags(struct hist_field *hist_field)
{
	const char *flags_str = NULL;

	if (hist_field->flags & HIST_FIELD_FL_HEX)
		flags_str = "hex";
	else if (hist_field->flags & HIST_FIELD_FL_SYM)
		flags_str = "sym";
	else if (hist_field->flags & HIST_FIELD_FL_SYM_OFFSET)
		flags_str = "sym-offset";
	else if (hist_field->flags & HIST_FIELD_FL_EXECNAME)
		flags_str = "execname";
	else if (hist_field->flags & HIST_FIELD_FL_SYSCALL)
		flags_str = "syscall";
	else if (hist_field->flags & HIST_FIELD_FL_LOG2)
		flags_str = "log2";
	else if (hist_field->flags & HIST_FIELD_FL_TIMESTAMP_USECS)
		flags_str = "usecs";

	return flags_str;
}

static void hist_field_print(struct seq_file *m, struct hist_field *hist_field)
{
	const char *field_name = hist_field_name(hist_field);

	if (hist_field->var_name)
		seq_printf(m, "%s=", hist_field->var_name);

	if (field_name)
		seq_printf(m, "%s", field_name);

	if (hist_field->flags) {
		const char *flags_str = get_hist_field_flags(hist_field);

		if (flags_str)
			seq_printf(m, ".%s", flags_str);
	}
}

static bool var_only(struct hist_trigger_data *hist_data)
{
	unsigned int i;

	for_each_hist_val_field(i, hist_data)
		if (hist_data->fields[i]->flags & HIST_FIELD_FL_VAR_ONLY)
			return true;
	return false;
}

static int event_hist_trigger_print(struct seq_file *m,
				    struct event_trigger_ops *ops,
				    struct event_trigger_data *data)
{
	struct hist_trigger_data *hist_data = data->private_data;
	struct hist_field *key_field;
	unsigned int i;

	seq_puts(m, "hist:");

	if (data->name)
		seq_printf(m, "%s:", data->name);

	seq_puts(m, "keys=");

	for_each_hist_key_field(i, hist_data) {
		key_field = hist_data->fields[i];

		if (i > hist_data->n_vals)
			seq_puts(m, ",");

		if (key_field->flags & HIST_FIELD_FL_STACKTRACE)
			seq_puts(m, "stacktrace");
		else if (key_field->flags & HIST_FIELD_FL_TIMESTAMP)
			seq_puts(m, "common_timestamp");
		else
			hist_field_print(m, key_field);
	}

	if (!var_only(hist_data))
		seq_puts(m, ":vals=");
	else
		seq_puts(m, ":");

	for_each_hist_val_field(i, hist_data) {
		if (i == HITCOUNT_IDX && !var_only(hist_data))
			seq_puts(m, "hitcount");
		else if (hist_data->fields[i]->flags & HIST_FIELD_FL_TIMESTAMP)
			seq_puts(m, "common_timestamp");
		else {
			if (!var_only(hist_data))
				seq_puts(m, ",");
			hist_field_print(m, hist_data->fields[i]);
		}
	}

	seq_puts(m, ":sort=");

	for (i = 0; i < hist_data->n_sort_keys; i++) {
		struct tracing_map_sort_key *sort_key;
		unsigned int idx;

		sort_key = &hist_data->sort_keys[i];
		idx = sort_key->field_idx;

		if (WARN_ON(idx >= TRACING_MAP_FIELDS_MAX))
			return -EINVAL;

		if (i > 0)
			seq_puts(m, ",");

		if (idx == HITCOUNT_IDX)
			seq_puts(m, "hitcount");
		else if (hist_data->fields[idx]->flags & HIST_FIELD_FL_TIMESTAMP)
			seq_puts(m, "common_timestamp");
		else
			hist_field_print(m, hist_data->fields[idx]);

		if (sort_key->descending)
			seq_puts(m, ".descending");
	}
	seq_printf(m, ":size=%u", (1 << hist_data->map->map_bits));

	print_actions_spec(m, hist_data);

	if (data->filter_str)
		seq_printf(m, " if %s", data->filter_str);

	if (data->paused)
		seq_puts(m, " [paused]");
	else
		seq_puts(m, " [active]");

	seq_putc(m, '\n');

	return 0;
}

static int event_hist_trigger_init(struct event_trigger_ops *ops,
				   struct event_trigger_data *data)
{
	struct hist_trigger_data *hist_data = data->private_data;

	if (!data->ref && hist_data->attrs->name)
		save_named_trigger(hist_data->attrs->name, data);

	data->ref++;

	return 0;
}

static void event_hist_trigger_free(struct event_trigger_ops *ops,
				    struct event_trigger_data *data)
{
	struct hist_trigger_data *hist_data = data->private_data;

	if (WARN_ON_ONCE(data->ref <= 0))
		return;

	data->ref--;
	if (!data->ref) {
		if (data->name)
			del_named_trigger(data);

		trigger_data_free(data);

		unresolve_pending_var_refs(hist_data);

		if (remove_hist_vars(hist_data))
			return;

		destroy_hist_data(hist_data);
	}
}

static struct event_trigger_ops event_hist_trigger_ops = {
	.func			= event_hist_trigger,
	.print			= event_hist_trigger_print,
	.init			= event_hist_trigger_init,
	.free			= event_hist_trigger_free,
};

static int event_hist_trigger_named_init(struct event_trigger_ops *ops,
					 struct event_trigger_data *data)
{
	data->ref++;

	save_named_trigger(data->named_data->name, data);

	event_hist_trigger_init(ops, data->named_data);

	return 0;
}

static void event_hist_trigger_named_free(struct event_trigger_ops *ops,
					  struct event_trigger_data *data)
{
	if (WARN_ON_ONCE(data->ref <= 0))
		return;

	event_hist_trigger_free(ops, data->named_data);

	data->ref--;
	if (!data->ref) {
		del_named_trigger(data);
		trigger_data_free(data);
	}
}

static struct event_trigger_ops event_hist_trigger_named_ops = {
	.func			= event_hist_trigger,
	.print			= event_hist_trigger_print,
	.init			= event_hist_trigger_named_init,
	.free			= event_hist_trigger_named_free,
};

static struct event_trigger_ops *event_hist_get_trigger_ops(char *cmd,
							    char *param)
{
	return &event_hist_trigger_ops;
}

static void hist_clear(struct event_trigger_data *data)
{
	struct hist_trigger_data *hist_data = data->private_data;

	if (data->name)
		pause_named_trigger(data);

	synchronize_sched();

	tracing_map_clear(hist_data->map);

	if (data->name)
		unpause_named_trigger(data);
}

static bool compatible_field(struct ftrace_event_field *field,
			     struct ftrace_event_field *test_field)
{
	if (field == test_field)
		return true;
	if (field == NULL || test_field == NULL)
		return false;
	if (strcmp(field->name, test_field->name) != 0)
		return false;
	if (strcmp(field->type, test_field->type) != 0)
		return false;
	if (field->size != test_field->size)
		return false;
	if (field->is_signed != test_field->is_signed)
		return false;

	return true;
}

static bool hist_trigger_match(struct event_trigger_data *data,
			       struct event_trigger_data *data_test,
			       struct event_trigger_data *named_data,
			       bool ignore_filter)
{
	struct tracing_map_sort_key *sort_key, *sort_key_test;
	struct hist_trigger_data *hist_data, *hist_data_test;
	struct hist_field *key_field, *key_field_test;
	unsigned int i;

	if (named_data && (named_data != data_test) &&
	    (named_data != data_test->named_data))
		return false;

	if (!named_data && is_named_trigger(data_test))
		return false;

	hist_data = data->private_data;
	hist_data_test = data_test->private_data;

	if (hist_data->n_vals != hist_data_test->n_vals ||
	    hist_data->n_fields != hist_data_test->n_fields ||
	    hist_data->n_sort_keys != hist_data_test->n_sort_keys)
		return false;

	if (!ignore_filter) {
		if ((data->filter_str && !data_test->filter_str) ||
		   (!data->filter_str && data_test->filter_str))
			return false;
	}

	for_each_hist_field(i, hist_data) {
		key_field = hist_data->fields[i];
		key_field_test = hist_data_test->fields[i];

		if (key_field->flags != key_field_test->flags)
			return false;
		if (!compatible_field(key_field->field, key_field_test->field))
			return false;
		if (key_field->offset != key_field_test->offset)
			return false;
		if (key_field->size != key_field_test->size)
			return false;
		if (key_field->is_signed != key_field_test->is_signed)
			return false;
		if ((key_field->var_name && !key_field_test->var_name) ||
		    (!key_field->var_name && key_field_test->var_name) ||
		    strcmp(key_field->var_name, key_field_test->var_name) != 0)
			return false;
	}

	for (i = 0; i < hist_data->n_sort_keys; i++) {
		sort_key = &hist_data->sort_keys[i];
		sort_key_test = &hist_data_test->sort_keys[i];

		if (sort_key->field_idx != sort_key_test->field_idx ||
		    sort_key->descending != sort_key_test->descending)
			return false;
	}

	if (!ignore_filter && data->filter_str &&
	    (strcmp(data->filter_str, data_test->filter_str) != 0))
		return false;

	return true;
}

static int hist_register_trigger(char *glob, struct event_trigger_ops *ops,
				 struct event_trigger_data *data,
				 struct trace_event_file *file)
{
	struct hist_trigger_data *hist_data = data->private_data;
	struct event_trigger_data *test, *named_data = NULL;
	int ret = 0;

	if (hist_data->attrs->name) {
		named_data = find_named_trigger(hist_data->attrs->name);
		if (named_data) {
			if (!hist_trigger_match(data, named_data, named_data,
						true)) {
				ret = -EINVAL;
				goto out;
			}
		}
	}

	if (hist_data->attrs->name && !named_data)
		goto new;

	list_for_each_entry_rcu(test, &file->triggers, list) {
		if (test->cmd_ops->trigger_type == ETT_EVENT_HIST) {
			if (!hist_trigger_match(data, test, named_data, false))
				continue;
			if (hist_data->attrs->pause)
				test->paused = true;
			else if (hist_data->attrs->cont)
				test->paused = false;
			else if (hist_data->attrs->clear)
				hist_clear(test);
			else
				ret = -EEXIST;
			goto out;
		}
	}
 new:
	if (hist_data->attrs->cont || hist_data->attrs->clear) {
		ret = -ENOENT;
		goto out;
	}

	if (hist_data->attrs->pause)
		data->paused = true;

	if (named_data) {
		destroy_hist_data(data->private_data);
		data->private_data = named_data->private_data;
		set_named_trigger_data(data, named_data);
		data->ops = &event_hist_trigger_named_ops;
	}

	if (data->ops->init) {
		ret = data->ops->init(data->ops, data);
		if (ret < 0)
			goto out;
	}

	list_add_rcu(&data->list, &file->triggers);
	ret++;

	update_cond_flag(file);

	tracing_set_time_stamp_abs(file->tr, true);
 out:
	return ret;
}

static int hist_trigger_enable(struct event_trigger_data *data,
			       struct trace_event_file *file)
{
	int ret = 0;

	if (trace_event_trigger_enable_disable(file, 1) < 0) {
		list_del_rcu(&data->list);
		update_cond_flag(file);
		ret--;
	}

	return ret;
}

static bool hist_trigger_check_refs(struct event_trigger_data *data,
				    struct trace_event_file *file)
{
	struct hist_trigger_data *hist_data = data->private_data;
	struct event_trigger_data *test, *named_data = NULL;

	if (hist_data->attrs->name)
		named_data = find_named_trigger(hist_data->attrs->name);

	list_for_each_entry_rcu(test, &file->triggers, list) {
		if (test->cmd_ops->trigger_type == ETT_EVENT_HIST) {
			if (!hist_trigger_match(data, test, named_data, false))
				continue;
			hist_data = test->private_data;
			if (check_var_refs(hist_data))
				return true;
			break;
		}
	}

	return false;
}

static void hist_unregister_trigger(char *glob, struct event_trigger_ops *ops,
				    struct event_trigger_data *data,
				    struct trace_event_file *file)
{
	struct hist_trigger_data *hist_data = data->private_data;
	struct event_trigger_data *test, *named_data = NULL;
	bool unregistered = false;

	if (hist_data->attrs->name)
		named_data = find_named_trigger(hist_data->attrs->name);
	list_for_each_entry_rcu(test, &file->triggers, list) {
		if (test->cmd_ops->trigger_type == ETT_EVENT_HIST) {
			if (!hist_trigger_match(data, test, named_data, false))
				continue;
			unregistered = true;
			list_del_rcu(&test->list);
			trace_event_trigger_enable_disable(file, 0);
			update_cond_flag(file);
			break;
		}
	}

	if (unregistered && test->ops->free)
		test->ops->free(test->ops, test);
}

static void hist_unreg_all(struct trace_event_file *file)
{
	struct event_trigger_data *test, *n;

	list_for_each_entry_safe(test, n, &file->triggers, list) {
		if (test->cmd_ops->trigger_type == ETT_EVENT_HIST) {
			list_del_rcu(&test->list);
			trace_event_trigger_enable_disable(file, 0);
			update_cond_flag(file);
			if (test->ops->free)
				test->ops->free(test->ops, test);
		}
	}
}

static int event_hist_trigger_func(struct event_command *cmd_ops,
				   struct trace_event_file *file,
				   char *glob, char *cmd, char *param)
{
	unsigned int hist_trigger_bits = TRACING_MAP_BITS_DEFAULT;
	struct event_trigger_data *trigger_data;
	struct hist_trigger_attrs *attrs;
	struct event_trigger_ops *trigger_ops;
	struct hist_trigger_data *hist_data;
	bool unreg_self = false;
	char *trigger;
	int ret = 0;

	if (!param)
		return -EINVAL;

	/* separate the trigger from the filter (k:v [if filter]) */
	trigger = strsep(&param, " \t");
	if (!trigger)
		return -EINVAL;

	attrs = parse_hist_trigger_attrs(trigger);
	if (IS_ERR(attrs))
		return PTR_ERR(attrs);

	if (attrs->map_bits)
		hist_trigger_bits = attrs->map_bits;

	hist_data = create_hist_data(hist_trigger_bits, attrs, file);
	if (IS_ERR(hist_data)) {
		destroy_hist_trigger_attrs(attrs);
		return PTR_ERR(hist_data);
	}

	trigger_ops = cmd_ops->get_trigger_ops(cmd, trigger);

	ret = -ENOMEM;
	trigger_data = kzalloc(sizeof(*trigger_data), GFP_KERNEL);
	if (!trigger_data)
		goto out_free;

	trigger_data->count = -1;
	trigger_data->ops = trigger_ops;
	trigger_data->cmd_ops = cmd_ops;

	INIT_LIST_HEAD(&trigger_data->list);
	RCU_INIT_POINTER(trigger_data->filter, NULL);

	trigger_data->private_data = hist_data;

	/* if param is non-empty, it's supposed to be a filter */
	if (param && cmd_ops->set_filter) {
		ret = cmd_ops->set_filter(param, trigger_data, file);
		if (ret < 0)
			goto out_free;
	}

	if (hist_trigger_check_refs(trigger_data, file)) {
		ret = -EINVAL;
		goto out_free;
	}

	if (glob[0] == '!') {
		cmd_ops->unreg(glob+1, trigger_ops, trigger_data, file);
		ret = 0;
		goto out_free;
	}

	ret = cmd_ops->reg(glob, trigger_ops, trigger_data, file);

	/*
	 * The above returns on success the # of triggers registered,
	 * but if it didn't register any it returns zero.  Consider no
	 * triggers registered a failure too.
	 */
	if (!ret) {
		if (!(attrs->pause || attrs->cont || attrs->clear))
			ret = -ENOENT;
		goto out_free;
	} else if (ret < 0)
		goto out_free;

	if (has_hist_vars(hist_data))
		save_hist_vars(hist_data);

	ret = create_actions(hist_data);
	if (ret)
		goto out_unreg;

	ret = tracing_map_init(hist_data->map);
	if (ret)
		goto out_unreg;

	ret = hist_trigger_enable(trigger_data, file);
	if (ret)
		goto out_unreg;
	/* Just return zero, not the number of registered triggers */
	ret = 0;
 out:
	return ret;
 out_unreg:
	cmd_ops->unreg(glob+1, trigger_ops, trigger_data, file);
	unreg_self = true;
 out_free:
	if (cmd_ops->set_filter)
		cmd_ops->set_filter(NULL, trigger_data, NULL);
	if (!unreg_self) {
		kfree(trigger_data);
		destroy_hist_data(hist_data);
	}
	goto out;
}

static struct event_command trigger_hist_cmd = {
	.name			= "hist",
	.trigger_type		= ETT_EVENT_HIST,
	.flags			= EVENT_CMD_FL_NEEDS_REC,
	.func			= event_hist_trigger_func,
	.reg			= hist_register_trigger,
	.unreg			= hist_unregister_trigger,
	.unreg_all		= hist_unreg_all,
	.get_trigger_ops	= event_hist_get_trigger_ops,
	.set_filter		= set_trigger_filter,
};

__init int register_trigger_hist_cmd(void)
{
	int ret;

	ret = register_event_command(&trigger_hist_cmd);
	WARN_ON(ret < 0);

	return ret;
}

static void
hist_enable_trigger(struct event_trigger_data *data, void *rec,
		    struct ring_buffer_event *event)
{
	struct enable_trigger_data *enable_data = data->private_data;
	struct event_trigger_data *test;

	list_for_each_entry_rcu(test, &enable_data->file->triggers, list) {
		if (test->cmd_ops->trigger_type == ETT_EVENT_HIST) {
			if (enable_data->enable)
				test->paused = false;
			else
				test->paused = true;
		}
	}
}

static void
hist_enable_count_trigger(struct event_trigger_data *data, void *rec,
			  struct ring_buffer_event *event)
{
	if (!data->count)
		return;

	if (data->count != -1)
		(data->count)--;

	hist_enable_trigger(data, rec, event);
}

static struct event_trigger_ops hist_enable_trigger_ops = {
	.func			= hist_enable_trigger,
	.print			= event_enable_trigger_print,
	.init			= event_trigger_init,
	.free			= event_enable_trigger_free,
};

static struct event_trigger_ops hist_enable_count_trigger_ops = {
	.func			= hist_enable_count_trigger,
	.print			= event_enable_trigger_print,
	.init			= event_trigger_init,
	.free			= event_enable_trigger_free,
};

static struct event_trigger_ops hist_disable_trigger_ops = {
	.func			= hist_enable_trigger,
	.print			= event_enable_trigger_print,
	.init			= event_trigger_init,
	.free			= event_enable_trigger_free,
};

static struct event_trigger_ops hist_disable_count_trigger_ops = {
	.func			= hist_enable_count_trigger,
	.print			= event_enable_trigger_print,
	.init			= event_trigger_init,
	.free			= event_enable_trigger_free,
};

static struct event_trigger_ops *
hist_enable_get_trigger_ops(char *cmd, char *param)
{
	struct event_trigger_ops *ops;
	bool enable;

	enable = (strcmp(cmd, ENABLE_HIST_STR) == 0);

	if (enable)
		ops = param ? &hist_enable_count_trigger_ops :
			&hist_enable_trigger_ops;
	else
		ops = param ? &hist_disable_count_trigger_ops :
			&hist_disable_trigger_ops;

	return ops;
}

static void hist_enable_unreg_all(struct trace_event_file *file)
{
	struct event_trigger_data *test, *n;

	list_for_each_entry_safe(test, n, &file->triggers, list) {
		if (test->cmd_ops->trigger_type == ETT_HIST_ENABLE) {
			list_del_rcu(&test->list);
			update_cond_flag(file);
			trace_event_trigger_enable_disable(file, 0);
			if (test->ops->free)
				test->ops->free(test->ops, test);
		}
	}
}

static struct event_command trigger_hist_enable_cmd = {
	.name			= ENABLE_HIST_STR,
	.trigger_type		= ETT_HIST_ENABLE,
	.func			= event_enable_trigger_func,
	.reg			= event_enable_register_trigger,
	.unreg			= event_enable_unregister_trigger,
	.unreg_all		= hist_enable_unreg_all,
	.get_trigger_ops	= hist_enable_get_trigger_ops,
	.set_filter		= set_trigger_filter,
};

static struct event_command trigger_hist_disable_cmd = {
	.name			= DISABLE_HIST_STR,
	.trigger_type		= ETT_HIST_ENABLE,
	.func			= event_enable_trigger_func,
	.reg			= event_enable_register_trigger,
	.unreg			= event_enable_unregister_trigger,
	.unreg_all		= hist_enable_unreg_all,
	.get_trigger_ops	= hist_enable_get_trigger_ops,
	.set_filter		= set_trigger_filter,
};

static __init void unregister_trigger_hist_enable_disable_cmds(void)
{
	unregister_event_command(&trigger_hist_enable_cmd);
	unregister_event_command(&trigger_hist_disable_cmd);
}

__init int register_trigger_hist_enable_disable_cmds(void)
{
	int ret;

	ret = register_event_command(&trigger_hist_enable_cmd);
	if (WARN_ON(ret < 0))
		return ret;
	ret = register_event_command(&trigger_hist_disable_cmd);
	if (WARN_ON(ret < 0))
		unregister_trigger_hist_enable_disable_cmds();

	return ret;
}

static void free_synthetic_event_field(struct synthetic_event_field *field)
{
	if (field->var_ref->var_ref.pending)
		destroy_hist_field(field->var_ref);
	kfree(field->name);
}

static void free_synthetic_event_print_fmt(struct trace_event_call *call)
{
	kfree(call->print_fmt);
}

static void free_synthetic_event(struct synthetic_event *event)
{
	unsigned int i;

	if (!event)
		return;

	for (i = 0; i < event->n_fields; i++)
		free_synthetic_event_field(&event->fields[i]);

	kfree(event->fields);
	kfree(event->name);

	kfree(event->class.system);
	free_synthetic_tracepoint(event->tp);
	free_synthetic_event_print_fmt(&event->call);

	kfree(event);
}

static struct synthetic_event *alloc_synthetic_event(char *event_name,
						     int n_fields)
{
	struct synthetic_event *event;

	event = kzalloc(sizeof(*event), GFP_KERNEL);
	if (!event) {
		event = ERR_PTR(-ENOMEM);
		goto out;
	}

	event->name = kstrdup(event_name, GFP_KERNEL);
	if (!event->name) {
		kfree(event);
		event = ERR_PTR(-ENOMEM);
		goto out;
	}

	event->n_fields = n_fields;
	event->fields = kcalloc(n_fields, sizeof(*event->fields), GFP_KERNEL);
	if (!event->fields)
		goto err;
 out:
	return event;
 err:
	free_synthetic_event(event);
	event = NULL;
	goto out;
}

static struct synthetic_event *find_synthetic_event(char *name)
{
	struct synthetic_event *event, *found = NULL;

	mutex_lock(&synthetic_event_mutex);

	list_for_each_entry(event, &synthetic_events_list, list) {
		if (strcmp(event->name, name) == 0) {
			found = event;
			goto out;
		}
	}
 out:
	mutex_unlock(&synthetic_event_mutex);

	return found;
}

struct synthetic_trace_event {
	struct trace_entry	ent;
	int			n_fields;
	u64			fields[];
};

static int synthetic_event_define_fields(struct trace_event_call *call)
{
	struct synthetic_event *event = call->data;
	struct synthetic_trace_event trace;
	unsigned int i;
	int ret = 0;
	int offset = offsetof(typeof(trace), fields);

	for (i = 0; i < event->n_fields; i++) {
		ret = trace_define_field(call, "u64", event->fields[i].name,
					 offset, sizeof(u64), 0, FILTER_OTHER);
		offset += sizeof(u64);
	}

	return ret;
}

static enum print_line_t
print_synthetic_event(struct trace_iterator *iter, int flags,
		      struct trace_event *event)
{
	struct trace_array *tr = iter->tr;
	struct trace_seq *s = &iter->seq;
	struct synthetic_trace_event *entry;
	struct synthetic_event *se;
	unsigned int i;

	entry = (struct synthetic_trace_event *)iter->ent;
	se = container_of(event, struct synthetic_event, call.event);

	trace_seq_printf(s, "%s: ", se->name);

	for (i = 0; i < entry->n_fields; i++) {
		if (trace_seq_has_overflowed(s))
			goto end;

		/* parameter types */
		if (tr->trace_flags & TRACE_ITER_VERBOSE)
			trace_seq_printf(s, "%s ", "u64");

		/* parameter values */
		trace_seq_printf(s, "%s=%llu%s", se->fields[i].name,
				 entry->fields[i],
				 i == entry->n_fields - 1 ? "" : ", ");
	}
end:
	trace_seq_putc(s, '\n');

	return trace_handle_return(s);
}

static struct trace_event_functions synthetic_event_funcs = {
	.trace		= print_synthetic_event
};

static notrace void
trace_event_raw_event_synthetic(void *__data,
				u64 *var_ref_vals,
				unsigned int var_ref_idx)
{
	struct trace_event_file *trace_file = __data;
	struct synthetic_trace_event *entry;
	struct trace_event_buffer fbuffer;
	int fields_size;
	unsigned int i;

	struct synthetic_event *event;

	event = trace_file->event_call->data;

	if (trace_trigger_soft_disabled(trace_file))
		return;

	fields_size = event->n_fields * sizeof(u64);

	entry = trace_event_buffer_reserve(&fbuffer, trace_file,
					   sizeof(*entry) + fields_size);
	if (!entry)
		return;

	entry->n_fields = event->n_fields;

	for (i = 0; i < event->n_fields; i++)
		entry->fields[i] = var_ref_vals[var_ref_idx + i];

	trace_event_buffer_commit(&fbuffer);
}

static int __set_synthetic_event_print_fmt(struct synthetic_event *event,
					   char *buf, int len)
{
	int pos = 0;
	int i;

	/* When len=0, we just calculate the needed length */
#define LEN_OR_ZERO (len ? len - pos : 0)

	pos += snprintf(buf + pos, LEN_OR_ZERO, "\"");
	for (i = 0; i < event->n_fields; i++) {
		pos += snprintf(buf + pos, LEN_OR_ZERO, "%s: 0x%%0%zulx%s",
				event->fields[i].name, sizeof(u64),
				i == event->n_fields - 1 ? "" : ", ");
	}
	pos += snprintf(buf + pos, LEN_OR_ZERO, "\"");

	for (i = 0; i < event->n_fields; i++) {
		pos += snprintf(buf + pos, LEN_OR_ZERO,
				", ((u64)(REC->%s))", event->fields[i].name);
	}

#undef LEN_OR_ZERO

	/* return the length of print_fmt */
	return pos;
}

static int set_synthetic_event_print_fmt(struct trace_event_call *call)
{
	struct synthetic_event *event = call->data;
	char *print_fmt;
	int len;

	/* First: called with 0 length to calculate the needed length */
	len = __set_synthetic_event_print_fmt(event, NULL, 0);

	print_fmt = kmalloc(len + 1, GFP_KERNEL);
	if (!print_fmt)
		return -ENOMEM;

	/* Second: actually write the @print_fmt */
	__set_synthetic_event_print_fmt(event, print_fmt, len + 1);
	call->print_fmt = print_fmt;

	return 0;
}

int dynamic_trace_event_reg(struct trace_event_call *call,
			    enum trace_reg type, void *data)
{
	struct trace_event_file *file = data;

	WARN_ON(!(call->flags & TRACE_EVENT_FL_TRACEPOINT));
	switch (type) {
	case TRACE_REG_REGISTER:
		return dynamic_tracepoint_probe_register(call->tp,
							 call->class->probe,
							 file);
	case TRACE_REG_UNREGISTER:
		tracepoint_probe_unregister(call->tp,
					    call->class->probe,
					    file, true);
		return 0;

#ifdef CONFIG_PERF_EVENTS
	case TRACE_REG_PERF_REGISTER:
		return dynamic_tracepoint_probe_register(call->tp,
							 call->class->perf_probe,
							 call);
	case TRACE_REG_PERF_UNREGISTER:
		tracepoint_probe_unregister(call->tp,
					    call->class->perf_probe,
					    call, true);
		return 0;
	case TRACE_REG_PERF_OPEN:
	case TRACE_REG_PERF_CLOSE:
	case TRACE_REG_PERF_ADD:
	case TRACE_REG_PERF_DEL:
		return 0;
#endif
	}
	return 0;
}

static int register_synthetic_event(struct synthetic_event *event)
{
	struct trace_event_call *call = &event->call;
	int ret = 0;

	event->call.class = &event->class;
	event->class.system = kstrdup(SYNTHETIC_EVENT_SYSTEM, GFP_KERNEL);
	if (!event->class.system) {
		ret = -ENOMEM;
		goto out;
	}

	event->tp = alloc_synthetic_tracepoint(event->name);
	if (IS_ERR(event->tp)) {
		ret = PTR_ERR(event->tp);
		event->tp = NULL;
		goto out;
	}

	INIT_LIST_HEAD(&call->class->fields);
	call->event.funcs = &synthetic_event_funcs;
	call->class->define_fields = synthetic_event_define_fields;

	ret = register_trace_event(&call->event);
	if (!ret) {
		ret = -ENODEV;
		goto out;
	}
	call->flags = TRACE_EVENT_FL_TRACEPOINT;
	call->class->reg = dynamic_trace_event_reg;
	call->class->probe = trace_event_raw_event_synthetic;
	call->data = event;
	call->tp = event->tp;
	ret = trace_add_event_call(call);
	if (ret) {
		pr_warn("Failed to register synthetic event: %s\n",
			trace_event_name(call));
		goto err;
	}

	ret = set_synthetic_event_print_fmt(call);
	if (ret < 0) {
		trace_remove_event_call(call);
		goto err;
	}
 out:
	return ret;
 err:
	unregister_trace_event(&call->event);
	goto out;
}

static int unregister_synthetic_event(struct synthetic_event *event)
{
	struct trace_event_call *call = &event->call;
	int ret;

	ret = trace_remove_event_call(call);
	if (ret) {
		pr_warn("Failed to remove synthetic event: %s\n",
			trace_event_name(call));
		free_synthetic_event_print_fmt(call);
		unregister_trace_event(&call->event);
	}

	return ret;
}

static int add_synthetic_event(struct synthetic_event *event)
{
	int ret;

	mutex_lock(&synthetic_event_mutex);

	ret = register_synthetic_event(event);
	if (ret)
		goto out;
	list_add(&event->list, &synthetic_events_list);
out:
	mutex_unlock(&synthetic_event_mutex);

	return ret;
}

static void remove_synthetic_event(struct synthetic_event *event)
{
	mutex_lock(&synthetic_event_mutex);

	unregister_synthetic_event(event);
	list_del(&event->list);

	mutex_unlock(&synthetic_event_mutex);
}

static int parse_synthetic_field(struct synthetic_event *event,
				 char *str, int i)
{
	char *field_name, *system, *event_name, *var_name;
	struct hist_field *var_ref;
	int ret = 0;

	field_name = strsep(&str, "=");
	if (!str || !field_name) {
		ret = -EINVAL;
		goto out;
	}

	event->fields[i].name = kstrdup(field_name, GFP_KERNEL);
	if (!event->fields[i].name)
		ret = -ENOMEM;

	system = strsep(&str, ":");
	if (!system || !str) {
		ret = -EINVAL;
		goto out;
	}

	event_name = strsep(&str, ":");
	if (!str) {
		var_name = event_name;
		event_name = system;
		system = NULL;
	} else
		var_name = str;

	var_ref = parse_var_ref(system, event_name, var_name, true);
	if (!var_ref) {
		ret = -EINVAL;
		goto out;
	}

	event->fields[i].var_ref = var_ref;
 out:
	return ret;
}

static int create_synthetic_event(int argc, char **argv)
{
	struct synthetic_event *event = NULL;
	bool delete_event = false;
	int i, ret = 0;
	char *token;

	/*
	 * Argument syntax:
	 *  - Add synthetic event: hist:<event_name> [EVENT:]VAR ...
	 *  - Remove synthetic event: !hist:<event_name> [EVENT:]VAR ...
	 * EVENT can be sys:event_name or event_name or nothing if VAR unique
	 */
	if (argc < 1) {
		ret = -EINVAL;
		goto err;
	}

	token = argv[0];
	if (token[0] == '!') {
		delete_event = true;
		token++;
	}

	event = find_synthetic_event(token);
	if (event) {
		if (delete_event) {
			remove_synthetic_event(event);
			goto err;
		} else
			ret = -EEXIST;
		goto out;
	} else if (delete_event) {
		ret = -EINVAL;
		goto out;
	}

	if (argc < 2) {
		ret = -EINVAL;
		goto err;
	}

	event = alloc_synthetic_event(token, argc - 1);
	if (IS_ERR(event)) {
		ret = PTR_ERR(event);
		event = NULL;
		goto err;
	}

	for (i = 1; i < argc; i++) {
		ret = parse_synthetic_field(event, argv[i], i - 1);
		if (ret)
			goto err;
	}

	ret = add_synthetic_event(event);
	if (ret)
		goto err;
 out:
	return ret;
 err:
	free_synthetic_event(event);

	goto out;
}

static int release_all_synthetic_events(void)
{
	struct synthetic_event *event, *e;

	mutex_lock(&synthetic_event_mutex);

	list_for_each_entry_safe(event, e, &synthetic_events_list, list) {
		remove_synthetic_event(event);
		free_synthetic_event(event);
	}

	mutex_unlock(&synthetic_event_mutex);

	return 0;
}


static void *synthetic_events_seq_start(struct seq_file *m, loff_t *pos)
{
	mutex_lock(&synthetic_event_mutex);

	return seq_list_start(&synthetic_events_list, *pos);
}

static void *synthetic_events_seq_next(struct seq_file *m, void *v, loff_t *pos)
{
	return seq_list_next(v, &synthetic_events_list, pos);
}

static void synthetic_events_seq_stop(struct seq_file *m, void *v)
{
	mutex_unlock(&synthetic_event_mutex);
}

static int synthetic_events_seq_show(struct seq_file *m, void *v)
{
	struct synthetic_event_field *se_field;
	const char *var_name, *system, *event_name;
	struct hist_trigger_data *hist_data;
	struct synthetic_event *event = v;
	struct trace_event_call *call;
	struct hist_field *ref_field;
	bool pending;
	unsigned int i;

	seq_printf(m, "%s ", event->name);

	for (i = 0; i < event->n_fields; i++) {
		se_field = &event->fields[i];
		ref_field = se_field->var_ref;
		pending = ref_field->var_ref.pending;
		if (!pending) {
			hist_data = ref_field->var_ref.hist_data;
			call = hist_data->event_file->event_call;
			system = call->class->system;
			event_name = trace_event_name(call);
		} else {
			system = ref_field->var_ref.pending_system;
			event_name = ref_field->var_ref.pending_event_name;
		}

		var_name = ref_field->var_ref.pending_var_name;

		/* parameter values */
		seq_printf(m, "%s=%s%s%s:%s%s%s", event->fields[i].name,
			   system ? system : "", system ? ":" : "",
			   event_name, var_name, pending ? "*" : "",
			   i == event->n_fields - 1 ? "" : ", ");
	}

	seq_putc(m, '\n');

	return 0;
}

static const struct seq_operations synthetic_events_seq_op = {
	.start  = synthetic_events_seq_start,
	.next   = synthetic_events_seq_next,
	.stop   = synthetic_events_seq_stop,
	.show   = synthetic_events_seq_show
};

static int synthetic_events_open(struct inode *inode, struct file *file)
{
	int ret;

	if ((file->f_mode & FMODE_WRITE) && (file->f_flags & O_TRUNC)) {
		ret = release_all_synthetic_events();
		if (ret < 0)
			return ret;
	}

	return seq_open(file, &synthetic_events_seq_op);
}

static ssize_t synthetic_events_write(struct file *file,
				      const char __user *buffer,
				      size_t count, loff_t *ppos)
{
	return trace_parse_run_command(file, buffer, count, ppos,
				       create_synthetic_event);
}

static const struct file_operations synthetic_events_fops = {
	.open           = synthetic_events_open,
	.write		= synthetic_events_write,
	.read           = seq_read,
	.llseek         = seq_lseek,
	.release        = seq_release,
};

static __init int trace_events_hist_init(void)
{
	struct dentry *entry = NULL;
	struct trace_array *tr;
	struct dentry *d_tracer;
	int err = 0;

	tr = top_trace_array();
	if (!tr) {
		err = -ENODEV;
		goto err;
	}

	d_tracer = tracing_init_dentry();
	if (IS_ERR(d_tracer)) {
		err = PTR_ERR(d_tracer);
		goto err;
	}

	entry = tracefs_create_file("synthetic_events", 0644, d_tracer,
				    tr, &synthetic_events_fops);
	if (!entry) {
		err = -ENODEV;
		goto err;
	}

	return err;
 err:
	pr_warn("Could not create tracefs 'synthetic_events' entry\n");

	return err;
}

fs_initcall(trace_events_hist_init);
