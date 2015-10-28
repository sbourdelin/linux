TMPDIR=$(mktemp -d /tmp/mergetest.XXXXX)
SCRIPT="$(dirname $0)/../merge_config.sh"

writefrag() {
	FRAG=$(mktemp ${TMPDIR}/frag.XXXX)
	cat > "${FRAG}"
	echo $FRAG
}

merge() {
	"${SCRIPT}" -e -O "${TMPDIR}" /dev/null $*
	return $?
}

merge_r() {
	"${SCRIPT}" -e -r -O "${TMPDIR}" /dev/null $*
	return $?
}

check() {
	grep -q "$1" "${TMPDIR}/.config"
	return $?
}
