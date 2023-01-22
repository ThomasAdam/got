#!/bin/sh
#
# Copyright (c) 2022 Mikhail Pchelin <misha@freebsd.org>
#
# Permission to use, copy, modify, and distribute this software for any
# purpose with or without fee is hereby granted, provided that the above
# copyright notice and this permission notice appear in all copies.
#
# THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
# WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
# MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
# ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
# WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
# ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
# OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.

. ../cmdline/common.sh
. ./common.sh

dummy_commit="aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"

# Non-existent commit
test_request_bad_commit() {
	local testroot=`test_init request_bad_commit`

	echo "0054want $dummy_commit multi_ack side-band-64k ofs-delta" \
		| ssh ${GOTD_DEVUSER}@127.0.0.1 git-upload-pack '/test-repo' \
		> $testroot/stdout 2>$testroot/stderr

	echo -n "0041ERR object $dummy_commit not found" \
		> $testroot/stdout.expected

	echo "gotsh: object $dummy_commit not found" \
		> $testroot/stderr.expected

	cmp -s $testroot/stdout.expected $testroot/stdout 0 112
	ret=$?
	if [ $ret -ne 0 ]; then
		echo "unexpected stdout" >&2
		test_done "$testroot" "1"
		return 1
	fi

	cmp -s $testroot/stderr.expected $testroot/stderr
	ret=$?
	if [ $ret -ne 0 ]; then
		echo "unexpected stderr" >&2
		diff -u $testroot/stderr.expected $testroot/stderr
		test_done "$testroot" "1"
		return 1
	fi
	test_done "$testroot" "$ret"
}

# Zero pkt-len (as flush packet with payload)
test_request_bad_length_zero() {
	local testroot=`test_init test_request_bad_length_zero`

	echo "0000want $dummy_commit multi_ack side-band-64k ofs-delta" \
		| ssh ${GOTD_DEVUSER}@127.0.0.1 git-upload-pack '/test-repo' \
		> $testroot/stdout 2>$testroot/stderr

	echo -n "00000028ERR unexpected flush packet received" \
		> $testroot/stdout.expected

	echo "gotsh: unexpected flush packet received" \
		> $testroot/stderr.expected

	cmp -s $testroot/stdout.expected $testroot/stdout 0 108
	ret=$?
	if [ $ret -ne 0 ]; then
		echo "unexpected stdout" >&2
		test_done "$testroot" "1"
		return 1
	fi

	cmp -s $testroot/stderr $testroot/stderr.expected
	ret=$?
	if [ $ret -ne 0 ]; then
		echo "unexpected stderr" >&2
		diff -u $testroot/stderr.expected $testroot/stderr
		test_done "$testroot" "1"
		return 1
	fi
	test_done "$testroot" "$ret"
}

# 0004 (empty)
test_request_bad_length_empty() {
	local testroot=`test_init test_request_bad_length_empty`

	echo "0004want $dummy_commit multi_ack side-band-64k ofs-delta" \
		| ssh ${GOTD_DEVUSER}@127.0.0.1 git-upload-pack '/test-repo' \
		> $testroot/stdout 2>$testroot/stderr

	echo -n '006c0000000000000000000000000000000000000000 ' \
		> $testroot/stdout.expected
	printf "capabilities^{}\0" >> $testroot/stdout.expected
	echo -n " agent=got/${GOT_VERSION_STR} ofs-delta side-band-64k" \
		>> $testroot/stdout.expected
	echo -n '00000018ERR packet too short' >> $testroot/stdout.expected

	echo "gotsh: packet too short" > $testroot/stderr.expected

	cmp -s $testroot/stdout.expected $testroot/stdout
	ret=$?
	if [ $ret -ne 0 ]; then
		echo "unexpected stdout" >&2
		test_done "$testroot" "1"
		return 1
	fi

	cmp -s $testroot/stderr.expected $testroot/stderr
	ret=$?
	if [ $ret -ne 0 ]; then
		echo "unexpected stderr" >&2
		diff -u $testroot/stderr.expected $testroot/stderr
		test_done "$testroot" "1"
		return 1
	fi
	test_done "$testroot" "$ret"
}

# Pkt-len too small
test_request_bad_length_small() {
	local testroot=`test_init test_request_bad_length_small`

	echo "0002want $dummy_commit multi_ack side-band-64k ofs-delta" \
		| ssh ${GOTD_DEVUSER}@127.0.0.1 git-upload-pack '/test-repo' \
		> $testroot/stdout 2>$testroot/stderr

	echo -n '006c0000000000000000000000000000000000000000 ' \
		> $testroot/stdout.expected
	printf "capabilities^{}\0" >> $testroot/stdout.expected
	echo -n " agent=got/${GOT_VERSION_STR} ofs-delta side-band-64k" \
		>> $testroot/stdout.expected
	echo -n '00000018ERR packet too short' >> $testroot/stdout.expected

	echo "gotsh: packet too short" > $testroot/stderr.expected

	cmp -s $testroot/stdout.expected $testroot/stdout
	ret=$?
	if [ $ret -ne 0 ]; then
		echo "unexpected stdout" >&2
		test_done "$testroot" "1"
		return 1
	fi

	cmp -s $testroot/stderr.expected $testroot/stderr
	ret=$?
	if [ $ret -ne 0 ]; then
		echo "unexpected stderr" >&2
		diff -u $testroot/stderr.expected $testroot/stderr
		test_done "$testroot" "1"
		return 1
	fi
	test_done "$testroot" "$ret"
}

# Pkt-len too large
test_request_bad_length_large() {
	local testroot=`test_init test_request_bad_length_large`

	echo "ffffwant $dummy_commit multi_ack side-band-64k ofs-delta" \
		| ssh ${GOTD_DEVUSER}@127.0.0.1 git-upload-pack '/test-repo' \
		> $testroot/stdout 2>$testroot/stderr

	echo -n '006c0000000000000000000000000000000000000000 ' \
		> $testroot/stdout.expected
	printf "capabilities^{}\0" >> $testroot/stdout.expected
	echo -n " agent=got/${GOT_VERSION_STR} ofs-delta side-band-64k" \
		>> $testroot/stdout.expected
	echo -n '0000001eERR unexpected end of file' \
		>> $testroot/stdout.expected

	echo "gotsh: unexpected end of file" > $testroot/stderr.expected

	cmp -s $testroot/stdout.expected $testroot/stdout
	ret=$?
	if [ $ret -ne 0 ]; then
		echo "unexpected stdout" >&2
		test_done "$testroot" "1"
		return 1
	fi

	cmp -s $testroot/stderr.expected $testroot/stderr
	ret=$?
	if [ $ret -ne 0 ]; then
		echo "unexpected stderr" >&2
		diff -u $testroot/stderr.expected $testroot/stderr
		test_done "$testroot" "1"
		return 1
	fi
	test_done "$testroot" "$ret"
}

# Unknown feature
test_request_bad_capabilities() {
	local testroot=`test_init test_request_bad_capabilities`

	echo "0054want $dummy_commit aaaaaaaaa bbbbbbbbbbbbb ccccccccc" \
		| ssh ${GOTD_DEVUSER}@127.0.0.1 git-upload-pack '/test-repo' \
		> $testroot/stdout 2>$testroot/stderr

	echo -n "00000025ERR unexpected want-line received" \
		> $testroot/stdout.expected

	echo "gotsh: unexpected want-line received" > $testroot/stderr.expected

	cmp -s $testroot/stdout.expected $testroot/stdout 0 108
	ret=$?
	if [ $ret -ne 0 ]; then
		echo "unexpected stdout" >&2
		test_done "$testroot" "1"
		return 1
	fi

	cmp -s $testroot/stderr.expected $testroot/stderr
	ret=$?
	if [ $ret -ne 0 ]; then
		echo "unexpected stderr" >&2
		diff -u $testroot/stderr.expected $testroot/stderr
		test_done "$testroot" "1"
		return 1
	fi
	test_done "$testroot" "$ret"
}

# Unknown repository
test_request_bad_repository() {
	local testroot=`test_init test_request_bad_repository`

	echo "0054want $dummy_commit aaaaaaaaa bbbbbbbbbbbbb ccccccccc" \
		| ssh ${GOTD_DEVUSER}@127.0.0.1 git-upload-pack '/XXXX-XXXX' \
		> $testroot/stdout 2>$testroot/stderr

	echo -n "001fERR no git repository found" > $testroot/stdout.expected

	echo "gotsh: no git repository found" > $testroot/stderr.expected

	cmp -s $testroot/stdout.expected $testroot/stdout
	ret=$?
	if [ $ret -ne 0 ]; then
		echo "unexpected stdout" >&2
		test_done "$testroot" "1"
		return 1
	fi

	cmp -s $testroot/stderr.expected $testroot/stderr
	ret=$?
	if [ $ret -ne 0 ]; then
		echo "unexpected stderr" >&2
		diff -u $testroot/stderr.expected $testroot/stderr
		test_done "$testroot" "1"
		return 1
	fi
	test_done "$testroot" "$ret"

}

# Repository with name of 255 symbols
test_request_bad_large_repo_name() {
	local testroot=`test_init test_request_bad_large_repo_name`

	# build a string of 255 "A": 63 "A" four times plus tree more "A"
	local a=AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA
	local repo_name="AAA$a$a$a$a"

	echo "0054want $dummy_commit aaaaaaaaa bbbbbbbbbbbbb ccccccccc" \
		| ssh ${GOTD_DEVUSER}@127.0.0.1 git-upload-pack "/$repo_name" \
		> $testroot/stdout 2>$testroot/stderr

	echo -n "0018ERR buffer too small" > $testroot/stdout.expected

	echo "gotsh: buffer too small" > $testroot/stderr.expected

	cmp -s $testroot/stdout.expected $testroot/stdout
	ret=$?
	if [ $ret -ne 0 ]; then
		echo "unexpected stdout" >&2
		test_done "$testroot" "1"
		return 1
	fi

	cmp -s $testroot/stderr.expected $testroot/stderr
	ret=$?
	if [ $ret -ne 0 ]; then
		echo "unexpected stderr" >&2
		diff -u $testroot/stderr.expected $testroot/stderr
		test_done "$testroot" "1"
		return 1
	fi
	test_done "$testroot" "$ret"
}

test_request_bad_no_repo() {
	local testroot=`test_init test_request_bad_no_repo`

	for i in `seq 10`; do
		ssh ${GOTD_DEVUSER}@127.0.0.1 git-upload-pack \
			>/dev/null 2>/dev/null </dev/null
	done

	# should still be able to clone; the repo is empty however
	got clone -q ${GOTD_TEST_REPO_URL} $testroot/repo-clone \
		2> $testroot/stderr
	cat <<EOF > $testroot/stderr.expected
got-fetch-pack: could not find any branches to fetch
got: could not find any branches to fetch
EOF

	if ! cmp -s "$testroot/stderr.expected" "$testroot/stderr"; then
		echo "got clone failed for unexpected reason" >&2
		diff -u "$testroot/stderr.expected" "$testroot/stderr"
		test_done "$testroot" 1
		return
	fi

	test_done "$testroot" 0
}

test_parseargs "$@"
run_test test_request_bad_commit
run_test test_request_bad_length_zero
run_test test_request_bad_length_empty
run_test test_request_bad_length_small
run_test test_request_bad_length_large
run_test test_request_bad_capabilities
run_test test_request_bad_repository
run_test test_request_bad_large_repo_name
run_test test_request_bad_no_repo
