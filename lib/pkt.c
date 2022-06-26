/*
 * Copyright (c) 2019 Ori Bernstein <ori@openbsd.org>
 * Copyright (c) 2021 Stefan Sperling <stsp@openbsd.org>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "got_error.h"
#include "got_lib_pkt.h"

const struct got_error *
got_pkt_readn(ssize_t *off, int fd, void *buf, size_t n)
{
	ssize_t r;

	*off = 0;
	while (*off != n) {
		r = read(fd, buf + *off, n - *off);
		if (r == -1)
			return got_error_from_errno("read");
		if (r == 0)
			return NULL;
		*off += r;
	}
	return NULL;
}

const struct got_error *
got_pkt_flushpkt(int fd, int chattygot)
{
	ssize_t w;

	if (chattygot > 1)
		fprintf(stderr, "%s: writepkt: 0000\n", getprogname());

	w = write(fd, "0000", 4);
	if (w == -1)
		return got_error_from_errno("write");
	if (w != 4)
		return got_error(GOT_ERR_IO);
	return NULL;
}

/*
 * Packet header contains a 4-byte hexstring which specifies the length
 * of data which follows.
 */
const struct got_error *
got_pkt_readhdr(int *datalen, int fd, int chattygot)
{
	static const struct got_error *err = NULL;
	char lenstr[5];
	long len;
	char *e;
	int n, i;
	ssize_t r;

	*datalen = 0;

	err = got_pkt_readn(&r, fd, lenstr, 4);
	if (err)
		return err;
	if (r == 0) {
		/* implicit "0000" */
		if (chattygot > 1)
			fprintf(stderr, "%s: readpkt: 0000\n", getprogname());
		return NULL;
	}
	if (r != 4)
		return got_error_msg(GOT_ERR_BAD_PACKET,
		    "wrong packet header length");

	lenstr[4] = '\0';
	for (i = 0; i < 4; i++) {
		if (!isprint((unsigned char)lenstr[i]))
			return got_error_msg(GOT_ERR_BAD_PACKET,
			    "unprintable character in packet length field");
	}
	for (i = 0; i < 4; i++) {
		if (!isxdigit((unsigned char)lenstr[i])) {
			if (chattygot)
				fprintf(stderr, "%s: bad length: '%s'\n",
				    getprogname(), lenstr);
			return got_error_msg(GOT_ERR_BAD_PACKET,
			    "packet length not specified in hex");
		}
	}
	errno = 0;
	len = strtol(lenstr, &e, 16);
	if (lenstr[0] == '\0' || *e != '\0')
		return got_error(GOT_ERR_BAD_PACKET);
	if (errno == ERANGE && (len == LONG_MAX || len == LONG_MIN))
		return got_error_msg(GOT_ERR_BAD_PACKET, "bad packet length");
	if (len > INT_MAX || len < INT_MIN)
		return got_error_msg(GOT_ERR_BAD_PACKET, "bad packet length");
	n = len;
	if (n == 0)
		return NULL;
	if (n <= 4)
		return got_error_msg(GOT_ERR_BAD_PACKET, "packet too short");
	n  -= 4;

	*datalen = n;
	return NULL;
}

const struct got_error *
got_pkt_readpkt(int *outlen, int fd, char *buf, int buflen, int chattygot)
{
	const struct got_error *err = NULL;
	int datalen, i;
	ssize_t n;

	err = got_pkt_readhdr(&datalen, fd, chattygot);
	if (err)
		return err;

	if (datalen > buflen)
		return got_error(GOT_ERR_NO_SPACE);

	err = got_pkt_readn(&n, fd, buf, datalen);
	if (err)
		return err;
	if (n != datalen)
		return got_error_msg(GOT_ERR_BAD_PACKET, "short packet");

	if (chattygot > 1) {
		fprintf(stderr, "%s: readpkt: %zd:\t", getprogname(), n);
		for (i = 0; i < n; i++) {
			if (isprint(buf[i]))
				fputc(buf[i], stderr);
			else
				fprintf(stderr, "[0x%.2x]", buf[i]);
		}
		fputc('\n', stderr);
	}

	*outlen = n;
	return NULL;
}

const struct got_error *
got_pkt_writepkt(int fd, char *buf, int nbuf, int chattygot)
{
	char len[5];
	int i;
	ssize_t w;

	if (snprintf(len, sizeof(len), "%04x", nbuf + 4) >= sizeof(len))
		return got_error(GOT_ERR_NO_SPACE);
	w = write(fd, len, 4);
	if (w == -1)
		return got_error_from_errno("write");
	if (w != 4)
		return got_error(GOT_ERR_IO);
	w = write(fd, buf, nbuf);
	if (w == -1)
		return got_error_from_errno("write");
	if (w != nbuf)
		return got_error(GOT_ERR_IO);
	if (chattygot > 1) {
		fprintf(stderr, "%s: writepkt: %s:\t", getprogname(), len);
		for (i = 0; i < nbuf; i++) {
			if (isprint(buf[i]))
				fputc(buf[i], stderr);
			else
				fprintf(stderr, "[0x%.2x]", buf[i]);
		}
		fputc('\n', stderr);
	}
	return NULL;
}
