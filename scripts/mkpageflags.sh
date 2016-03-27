#!/bin/sh -efu

fatal() {
	echo "$@" >&2
	exit 1
}

any() {
	echo "(__p)"
}

head() {
	echo "compound_head(__p)"
}

no_tail() {
	local enforce="${1:+VM_BUG_ON_PGFLAGS(PageTail(__p), __p);}"

	echo "({$enforce compound_head(__p);})"
}

no_compound() {
	local enforce="${1:+VM_BUG_ON_PGFLAGS(PageCompound(__p), __p);}"

	echo "({$enforce __p;})"
}

generate_test() {
	local op="$1"; shift
	local uname="$1"; shift
	local lname="$1"; shift
	local page="$1"; shift

	cat <<EOF
#define $uname(__p) ({								\\
	int ret;								\\
	if (__builtin_types_compatible_p(typeof(*(__p)), struct page))		\\
		ret = $op(PG_$lname, &$page->flags);				\\
	else									\\
		BUILD_BUG();							\\
	ret;									\\
})

EOF
}

generate_mod() {
	local op="$1"; shift
	local uname="$1"; shift
	local lname="$1"; shift
	local page="$1"; shift

	cat <<EOF
#define $uname(__p) do {							\\
	if (__builtin_types_compatible_p(typeof(*(__p)), struct page))		\\
		$op(PG_$lname, &$page->flags);					\\
	else									\\
		BUILD_BUG();							\\
} while (0)

EOF
}

generate_false() {
	local uname="$1"; shift

	cat <<EOF
#define $uname(__p) 0

EOF
}

generate_noop() {
	local uname="$1"; shift

	cat <<EOF
#define $uname(__p) do { } while (0)

EOF
}

generate_helper() {
	local helper="$1"; shift
	local uname="$1"; shift
	local lname="$1"; shift
	local policy="$1"; shift

	case "$helper" in
		test)
			generate_test 'test_bit' "Page$uname" "$lname" "$($policy)"
			;;
		set)
			generate_mod 'set_bit' "SetPage$uname" "$lname" "$($policy)"
			;;
		clear)
			generate_mod 'clear_bit' "ClearPage$uname" "$lname" "$($policy)"
			;;
		testset)
			generate_test 'test_and_set_bit' "TestSetPage$uname" "$lname" "$($policy 1)"
			;;
		testclear)
			generate_test 'test_and_clear_bit' "TestClearPage$uname" "$lname" "$($policy 1)"
			;;
		__set)
			generate_mod '__set_bit' "__SetPage$uname" "$lname" "$($policy)"
			;;
		__clear)
			generate_mod '__clear_bit' "__ClearPage$uname" "$lname" "$($policy)"
			;;
		testfalse)
			generate_false "Page$uname"
			;;
		setnoop)
			generate_noop "SetPage$uname"
			;;
		clearnoop)
			generate_noop "ClearPage$uname"
			;;
		testsetfalse)
			generate_false "TestSetPage$uname"
			;;
		testclearfalse)
			generate_false "TestClearPage$uname"
			;;
		__clearnoop)
			generate_noop "__ClearPage$uname"
			;;
		*)
			fatal "$helper: unknown helper"
			;;
	esac
}

generate_helpers() {
	while read uname lname policy helpers; do
		for helper in $helpers; do
			generate_helper "$helper" "$uname" "$lname" "$policy"
		done
	done
}

while read l; do
	case "$l" in
		[A-Z]*)
			echo "$l" | generate_helpers
			;;
		*)
			echo "$l"
			;;
	esac
done
