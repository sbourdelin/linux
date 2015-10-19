/*
 * Stack tracing support
 *
 * Copyright (C) 2012 ARM Ltd.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */
#include <linux/kernel.h>
#include <linux/export.h>
#include <linux/ftrace.h>
#include <linux/sched.h>
#include <linux/stacktrace.h>

#include <asm/insn.h>
#include <asm/stacktrace.h>

#ifdef CONFIG_STACK_TRACER
/*
 * This function parses a function prologue of a traced function and
 * determines its stack size.
 * A return value indicates a location of @pc in a function prologue.
 * @return value:
 * <case 1>                       <case 1'>
 * 1:
 *     sub sp, sp, #XX            sub sp, sp, #XX
 * 2:
 *     stp x29, x30, [sp, #YY]    stp x29, x30, [sp, #--ZZ]!
 * 3:
 *     add x29, sp, #YY           mov x29, sp
 * 0:
 *
 * <case 2>
 * 1:
 *     stp x29, x30, [sp, #-XX]!
 * 3:
 *     mov x29, sp
 * 0:
 *
 * @size: sp offset from calller's sp (XX or XX + ZZ)
 * @size2: fp offset from new sp (YY or 0)
 */
static int analyze_function_prologue(unsigned long pc,
		unsigned long *size, unsigned long *size2)
{
	unsigned long offset;
	u32 *addr, insn;
	int pos = -1;
	enum aarch64_insn_register src, dst, reg1, reg2, base;
	int imm;
	enum aarch64_insn_variant variant;
	enum aarch64_insn_adsb_type adsb_type;
	enum aarch64_insn_ldst_type ldst_type;

	*size = *size2 = 0;

	if (!pc)
		goto out;

	if (unlikely(!kallsyms_lookup_size_offset(pc, NULL, &offset)))
		goto out;

	addr = (u32 *)(pc - offset);
#ifdef CONFIG_DYNAMIC_FTRACE
	if (addr == (u32 *)ftrace_call)
		addr = (u32 *)ftrace_caller;
#ifdef CONFIG_FUNCTION_GRAPH_TRACER
	else if (addr == (u32 *)ftrace_graph_caller)
#ifdef CONFIG_DYNAMIC_FTRACE
		addr = (u32 *)ftrace_caller;
#else
		addr = (u32 *)_mcount;
#endif
#endif
#endif

	insn = *addr;
	pos = 1;

	/* analyze a function prologue */
	while ((unsigned long)addr < pc) {
		if (aarch64_insn_is_branch_imm(insn) ||
		    aarch64_insn_is_br(insn) ||
		    aarch64_insn_is_blr(insn) ||
		    aarch64_insn_is_ret(insn) ||
		    aarch64_insn_is_eret(insn))
			/* exiting a basic block */
			goto out;

		if (aarch64_insn_decode_add_sub_imm(insn, &dst, &src,
					&imm, &variant, &adsb_type)) {
			if ((adsb_type == AARCH64_INSN_ADSB_SUB) &&
				(dst == AARCH64_INSN_REG_SP) &&
				(src == AARCH64_INSN_REG_SP)) {
				/*
				 * Starting the following sequence:
				 *   sub sp, sp, #xx
				 *   stp x29, x30, [sp, #yy]
				 *   add x29, sp, #yy
				 */
				WARN_ON(pos != 1);
				pos = 2;
				*size += imm;
			} else if ((adsb_type == AARCH64_INSN_ADSB_ADD) &&
				(dst == AARCH64_INSN_REG_29) &&
				(src == AARCH64_INSN_REG_SP)) {
				/*
				 *   add x29, sp, #yy
				 * or
				 *   mov x29, sp
				 */
				WARN_ON(pos != 3);
				pos = 0;
				*size2 = imm;

				break;
			}
		} else if (aarch64_insn_decode_load_store_pair(insn,
					&reg1, &reg2, &base, &imm,
					&variant, &ldst_type)) {
			if ((ldst_type ==
				AARCH64_INSN_LDST_STORE_PAIR_PRE_INDEX) &&
			    (reg1 == AARCH64_INSN_REG_29) &&
			    (reg2 == AARCH64_INSN_REG_30) &&
			    (base == AARCH64_INSN_REG_SP)) {
				/*
				 * Starting the following sequence:
				 *   stp x29, x30, [sp, #-xx]!
				 *   mov x29, sp
				 */
				WARN_ON(!((pos == 1) || (pos == 2)));
				pos = 3;
				*size += -imm;
			} else if ((ldst_type ==
				AARCH64_INSN_LDST_STORE_PAIR) &&
			    (reg1 == AARCH64_INSN_REG_29) &&
			    (reg2 == AARCH64_INSN_REG_30) &&
			    (base == AARCH64_INSN_REG_SP)) {
				/*
				 *   stp x29, x30, [sp, #yy]
				 */
				WARN_ON(pos != 2);
				pos = 3;
			}
		}

		addr++;
		insn = *addr;
	}

out:
	return pos;
}
#endif

/*
 * AArch64 PCS assigns the frame pointer to x29.
 *
 * A simple function prologue looks like this:
 * 	sub	sp, sp, #0x10
 *   	stp	x29, x30, [sp]
 *	mov	x29, sp
 *
 * A simple function epilogue looks like this:
 *	mov	sp, x29
 *	ldp	x29, x30, [sp]
 *	add	sp, sp, #0x10
 */
int notrace unwind_frame(struct stackframe *frame)
{
	unsigned long high, low;
	unsigned long fp = frame->fp;

	low  = frame->sp;
	high = ALIGN(low, THREAD_SIZE);

	if (fp < low || fp > high - 0x18 || fp & 0xf)
		return -EINVAL;

	frame->sp = fp + 0x10;
	frame->fp = *(unsigned long *)(fp);
	/*
	 * decrement PC by AARCH64_INSN_SIZE here because we care about
	 * the PC at time of bl, not where the return will go.
	 */
	frame->pc = *(unsigned long *)(fp + 8) - AARCH64_INSN_SIZE;

	return 0;
}

void notrace walk_stackframe(struct stackframe *frame,
		     int (*fn)(struct stackframe *, void *), void *data)
{
	while (1) {
		int ret;

		if (fn(frame, data))
			break;
		ret = unwind_frame(frame);
		if (ret < 0)
			break;
	}
}
EXPORT_SYMBOL(walk_stackframe);

#ifdef CONFIG_STACKTRACE
struct stack_trace_data {
	struct stack_trace *trace;
	unsigned int no_sched_functions;
	unsigned int skip;
#ifdef CONFIG_FUNCTION_GRAPH_TRACER
	unsigned int ret_stack_index;
#endif
#ifdef CONFIG_STACK_TRACER
	unsigned long *sp;
#endif
};

static int save_trace(struct stackframe *frame, void *d)
{
	struct stack_trace_data *data = d;
	struct stack_trace *trace = data->trace;
	unsigned long addr = frame->pc;

#ifdef CONFIG_FUNCTION_GRAPH_TRACER
	if (addr == (unsigned long)return_to_handler - AARCH64_INSN_SIZE) {
		/*
		 * This is a case where function graph tracer has
		 * modified a return address (LR) in a stack frame
		 * to hook a function return.
		 * So replace it to an original value.
		 */
		frame->pc = addr =
			current->ret_stack[data->ret_stack_index--].ret
							- AARCH64_INSN_SIZE;
	}
#endif /* CONFIG_FUNCTION_GRAPH_TRACER */

	if (data->no_sched_functions && in_sched_functions(addr))
		return 0;
	if (data->skip) {
		data->skip--;
		return 0;
	}

#ifdef CONFIG_STACK_TRACER
	if (data->sp) {
		if (trace->nr_entries) {
			unsigned long child_pc, sp_off, fp_off;
			int pos;

			child_pc = trace->entries[trace->nr_entries - 1];
			pos = analyze_function_prologue(child_pc,
					&sp_off, &fp_off);
			/*
			 * frame->sp - 0x10 is actually a child's fp.
			 * See above.
			 */
			data->sp[trace->nr_entries] = (pos < 0 ? frame->sp :
					(frame->sp - 0x10) + sp_off - fp_off);
		} else {
			data->sp[0] = frame->sp;
		}
	}
#endif
	trace->entries[trace->nr_entries++] = addr;

	return trace->nr_entries >= trace->max_entries;
}

static void __save_stack_trace_tsk(struct task_struct *tsk,
		struct stack_trace *trace, unsigned long *stack_dump_sp)
{
	struct stack_trace_data data;
	struct stackframe frame;

	data.trace = trace;
	data.skip = trace->skip;
#ifdef CONFIG_FUNCTION_GRAPH_TRACER
	data.ret_stack_index = current->curr_ret_stack;
#endif
#ifdef CONFIG_STACK_TRACER
	data.sp = stack_dump_sp;
#endif

	if (tsk != current) {
		data.no_sched_functions = 1;
		frame.fp = thread_saved_fp(tsk);
		frame.sp = thread_saved_sp(tsk);
		frame.pc = thread_saved_pc(tsk);
	} else {
		data.no_sched_functions = 0;
		frame.fp = (unsigned long)__builtin_frame_address(0);
		frame.sp = current_stack_pointer;
		asm("1:");
		asm("ldr %0, =1b" : "=r" (frame.pc));
	}

	walk_stackframe(&frame, save_trace, &data);
	if (trace->nr_entries < trace->max_entries)
		trace->entries[trace->nr_entries++] = ULONG_MAX;
}

void save_stack_trace_tsk(struct task_struct *tsk, struct stack_trace *trace)
{
	__save_stack_trace_tsk(tsk, trace, NULL);
}

void save_stack_trace(struct stack_trace *trace)
{
	__save_stack_trace_tsk(current, trace, NULL);
}
EXPORT_SYMBOL_GPL(save_stack_trace);

#ifdef CONFIG_STACK_TRACER
void save_stack_trace_sp(struct stack_trace *trace,
					unsigned long *stack_dump_sp)
{
	__save_stack_trace_tsk(current, trace, stack_dump_sp);
}
#endif /* CONFIG_STACK_TRACER */
#endif
