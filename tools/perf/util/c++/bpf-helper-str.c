#include "clang-bpf-includes.h"
const char clang_builtin_bpf_helper_str[] =
"#ifndef BPF_HELPER_DEFINED\n"
"#define BPF_HELPER_DEFINED\n"
"struct bpf_map_def {\n"
"	unsigned int type;\n"
"	unsigned int key_size;\n"
"	unsigned int value_size;\n"
"	unsigned int max_entries;\n"
"};\n"
"#define SEC(NAME) __attribute__((section(NAME), used))\n"
"#endif"
;
