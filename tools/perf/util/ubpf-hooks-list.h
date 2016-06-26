UBPF_HOOK(perf_record_start,
	  __UBPF_HOOK_PROTO(
		  __proto(int dummy)
		  ),

	  __UBPF_HOOK_ARGS(dummy),

	  __UBPF_HOOK_STRUCT__entry(
		  __field(int,	dummy)
		  ),

	  __UBPF_HOOK_ASSIGN(
		  __entry.dummy = dummy;
		  )
	);

UBPF_HOOK(perf_record_end,
	  __UBPF_HOOK_PROTO(
		  __proto(int samples),
		  __proto(int dummy)
		  ),

	  __UBPF_HOOK_ARGS(samples, dummy),

	  __UBPF_HOOK_STRUCT__entry(
		  __field(int,	samples)
		  __field(int,	dummy)
		  ),

	  __UBPF_HOOK_ASSIGN(
		  __entry.samples = samples;
		  __entry.dummy = dummy;
		  )
	);
