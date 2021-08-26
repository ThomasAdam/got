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

#include <sys/types.h>
#include <sys/queue.h>
#include <sys/uio.h>
#include <sys/time.h>
#include <sys/stat.h>

#include <stdint.h>
#include <errno.h>
#include <imsg.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <sha1.h>
#include <fcntl.h>
#include <unistd.h>
#include <zlib.h>
#include <err.h>

#include "got_error.h"
#include "got_object.h"
#include "got_path.h"
#include "got_version.h"
#include "got_fetch.h"
#include "got_reference.h"

#include "got_lib_sha1.h"
#include "got_lib_delta.h"
#include "got_lib_object.h"
#include "got_lib_object_parse.h"
#include "got_lib_privsep.h"
#include "got_lib_pack.h"

#ifndef nitems
#define nitems(_a)	(sizeof((_a)) / sizeof((_a)[0]))
#endif

struct got_object *indexed;
static int chattygot;

static const struct got_error *
readn(ssize_t *off, int fd, void *buf, size_t n)
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

static const struct got_error *
flushpkt(int fd)
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
static const struct got_error *
read_pkthdr(int *datalen, int fd)
{
	static const struct got_error *err = NULL;
	char lenstr[5];
	long len;
	char *e;
	int n, i;
	ssize_t r;

	*datalen = 0;

	err = readn(&r, fd, lenstr, 4);
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

static const struct got_error *
readpkt(int *outlen, int fd, char *buf, int buflen)
{
	const struct got_error *err = NULL;
	int datalen, i;
	ssize_t n;

	err = read_pkthdr(&datalen, fd);
	if (err)
		return err;

	if (datalen > buflen)
		return got_error(GOT_ERR_NO_SPACE);

	err = readn(&n, fd, buf, datalen);
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

static const struct got_error *
writepkt(int fd, char *buf, int nbuf)
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

static const struct got_error *
tokenize_refline(char **tokens, char *line, int len, int maxtokens)
{
	const struct got_error *err = NULL;
	char *p;
	size_t i, n = 0;

	for (i = 0; i < maxtokens; i++)
		tokens[i] = NULL;

	for (i = 0; n < len && i < maxtokens; i++) {
		while (isspace(*line)) {
			line++;
			n++;
		}
		p = line;
		while (*line != '\0' && n < len &&
		    (!isspace(*line) || i == maxtokens - 1)) {
			line++;
			n++;
		}
		tokens[i] = strndup(p, line - p);
		if (tokens[i] == NULL) {
			err = got_error_from_errno("strndup");
			goto done;
		}
		/* Skip \0 field-delimiter at end of token. */
		while (line[0] == '\0' && n < len) {
			line++;
			n++;
		}
	}
	if (i <= 2)
		err = got_error(GOT_ERR_NOT_REF);
done:
	if (err) {
		int j;
		for (j = 0; j < i; j++) {
			free(tokens[j]);
			tokens[j] = NULL;
		}
	}
	return err;
}

static const struct got_error *
parse_refline(char **id_str, char **refname, char **server_capabilities,
    char *line, int len)
{
	const struct got_error *err = NULL;
	char *tokens[3];

	err = tokenize_refline(tokens, line, len, nitems(tokens));
	if (err)
		return err;

	if (tokens[0])
		*id_str = tokens[0];
	if (tokens[1])
		*refname = tokens[1];
	if (tokens[2]) {
		char *p;
		*server_capabilities = tokens[2];
		p = strrchr(*server_capabilities, '\n');
		if (p)
			*p = '\0';
	}

	return NULL;
}

#define GOT_CAPA_AGENT			"agent"
#define GOT_CAPA_OFS_DELTA		"ofs-delta"
#define GOT_CAPA_SIDE_BAND_64K		"side-band-64k"
#define GOT_CAPA_REPORT_STATUS		"report-status"
#define GOT_CAPA_DELETE_REFS		"delete-refs"

#define GOT_SIDEBAND_PACKFILE_DATA	1
#define GOT_SIDEBAND_PROGRESS_INFO	2
#define GOT_SIDEBAND_ERROR_INFO		3


struct got_capability {
	const char *key;
	const char *value;
};
static const struct got_capability got_capabilities[] = {
	{ GOT_CAPA_AGENT, "got/" GOT_VERSION_STR },
	{ GOT_CAPA_OFS_DELTA, NULL },
#if 0
	{ GOT_CAPA_SIDE_BAND_64K, NULL },
#endif
	{ GOT_CAPA_REPORT_STATUS, NULL },
	{ GOT_CAPA_DELETE_REFS, NULL },
};

static const struct got_error *
match_capability(char **my_capabilities, const char *capa,
    const struct got_capability *mycapa)
{
	char *equalsign;
	char *s;

	equalsign = strchr(capa, '=');
	if (equalsign) {
		if (strncmp(capa, mycapa->key, equalsign - capa) != 0)
			return NULL;
	} else {
		if (strcmp(capa, mycapa->key) != 0)
			return NULL;
	}

	if (asprintf(&s, "%s %s%s%s",
	    *my_capabilities != NULL ? *my_capabilities : "",
	    mycapa->key,
	    mycapa->value != NULL ? "=" : "",
	    mycapa->value != NULL? mycapa->value : "") == -1)
		return got_error_from_errno("asprintf");

	free(*my_capabilities);
	*my_capabilities = s;
	return NULL;
}

static const struct got_error *
match_capabilities(char **my_capabilities, char *server_capabilities)
{
	const struct got_error *err = NULL;
	char *capa;
	size_t i;

	*my_capabilities = NULL;
	do {
		capa = strsep(&server_capabilities, " ");
		for (i = 0; capa != NULL && i < nitems(got_capabilities); i++) {
			err = match_capability(my_capabilities,
			    capa, &got_capabilities[i]);
			if (err)
				goto done;
		}
	} while (capa);

	if (*my_capabilities == NULL) {
		*my_capabilities = strdup("");
		if (*my_capabilities == NULL) {
			err = got_error_from_errno("strdup");
			goto done;
		}
	}

	/*
	 * Workaround for github.
	 *
	 * Github will accept the pack but fail to update the references
	 * if we don't have capabilities advertised. Report-status seems
	 * harmless to add, so we add it.
	 *
	 * Github doesn't advertise any capabilities, so we can't check
	 * for compatibility. We just need to add it blindly.
	 */
	if (strstr(*my_capabilities, GOT_CAPA_REPORT_STATUS) == NULL) {
		char *s;
		if (asprintf(&s, "%s %s", *my_capabilities,
		    GOT_CAPA_REPORT_STATUS) == -1) {
			err = got_error_from_errno("asprintf");
			goto done;
		}
		free(*my_capabilities);
		*my_capabilities = s;
	}
done:
	if (err) {
		free(*my_capabilities);
		*my_capabilities = NULL;
	}
	return err;
}

static const struct got_error *
send_upload_progress(struct imsgbuf *ibuf, off_t bytes)
{
	if (imsg_compose(ibuf, GOT_IMSG_SEND_UPLOAD_PROGRESS, 0, 0, -1,
	    &bytes, sizeof(bytes)) == -1)
		return got_error_from_errno(
		    "imsg_compose SEND_UPLOAD_PROGRESS");

	return got_privsep_flush_imsg(ibuf);
}

static const struct got_error *
send_pack_request(struct imsgbuf *ibuf)
{
	if (imsg_compose(ibuf, GOT_IMSG_SEND_PACK_REQUEST, 0, 0, -1,
	    NULL, 0) == -1)
		return got_error_from_errno("imsg_compose SEND_PACK_REQUEST");
	return got_privsep_flush_imsg(ibuf);
}

static const struct got_error *
send_done(struct imsgbuf *ibuf)
{
	if (imsg_compose(ibuf, GOT_IMSG_SEND_DONE, 0, 0, -1, NULL, 0) == -1)
		return got_error_from_errno("imsg_compose SEND_DONE");
	return got_privsep_flush_imsg(ibuf);
}

static const struct got_error *
recv_packfd(int *packfd, struct imsgbuf *ibuf)
{
	const struct got_error *err;
	struct imsg imsg;

	*packfd = -1;

	err = got_privsep_recv_imsg(&imsg, ibuf, 0);
	if (err)
		return err;
		
	if (imsg.hdr.type == GOT_IMSG_STOP) {
		err = got_error(GOT_ERR_CANCELLED);
		goto done;
	}

	if (imsg.hdr.type != GOT_IMSG_SEND_PACKFD) {
		err = got_error(GOT_ERR_PRIVSEP_MSG);
		goto done;
	}

	if (imsg.hdr.len - IMSG_HEADER_SIZE != 0) {
		err = got_error(GOT_ERR_PRIVSEP_LEN);
		goto done;
	}

	*packfd = imsg.fd;
done:
	imsg_free(&imsg);
	return err;
}

static const struct got_error *
send_pack_file(int sendfd, int packfd, struct imsgbuf *ibuf)
{
	const struct got_error *err;
	unsigned char buf[8192];
	ssize_t r, w;
	off_t wtotal = 0;

	if (lseek(packfd, 0L, SEEK_SET) == -1)
		return got_error_from_errno("lseek");

	for (;;) {
		r = read(packfd, buf, sizeof(buf));
		if (r == -1)
			return got_error_from_errno("read");
		if (r == 0)
			break;
		w = write(sendfd, buf, r);
		if (w == -1)
			return got_error_from_errno("write");
		if (w != r)
			return got_error(GOT_ERR_IO);
		wtotal += w;
		err = send_upload_progress(ibuf, wtotal);
		if (err)
			return err;
	}

	return NULL;
}

static const struct got_error *
send_error(const char *buf, size_t len)
{
	static char msg[1024];
	size_t i;

	for (i = 0; i < len && i < sizeof(msg) - 1; i++) {
		if (!isprint(buf[i]))
			return got_error_msg(GOT_ERR_BAD_PACKET,
			    "non-printable error message received from server");
		msg[i] = buf[i];
	}
	msg[i] = '\0';
	return got_error_msg(GOT_ERR_SEND_FAILED, msg);
}

static const struct got_error *
send_their_ref(struct imsgbuf *ibuf, struct got_object_id *refid,
    const char *refname)
{
	const struct got_error *err = NULL;
	struct ibuf *wbuf;
	size_t len, reflen = strlen(refname);

	len = sizeof(struct got_imsg_send_remote_ref) + reflen;
	if (len >= MAX_IMSGSIZE - IMSG_HEADER_SIZE)
		return got_error(GOT_ERR_NO_SPACE);

	wbuf = imsg_create(ibuf, GOT_IMSG_SEND_REMOTE_REF, 0, 0, len);
	if (wbuf == NULL)
		return got_error_from_errno("imsg_create SEND_REMOTE_REF");

	/* Keep in sync with struct got_imsg_send_remote_ref definition! */
	if (imsg_add(wbuf, refid->sha1, SHA1_DIGEST_LENGTH) == -1) {
		err = got_error_from_errno("imsg_add SEND_REMOTE_REF");
		ibuf_free(wbuf);
		return err;
	}
	if (imsg_add(wbuf, &reflen, sizeof(reflen)) == -1) {
		err = got_error_from_errno("imsg_add SEND_REMOTE_REF");
		ibuf_free(wbuf);
		return err;
	}
	if (imsg_add(wbuf, refname, reflen) == -1) {
		err = got_error_from_errno("imsg_add SEND_REMOTE_REF");
		ibuf_free(wbuf);
		return err;
	}

	wbuf->fd = -1;
	imsg_close(ibuf, wbuf);
	return got_privsep_flush_imsg(ibuf);
}

static const struct got_error *
send_ref_status(struct imsgbuf *ibuf, const char *refname, int success,
    struct got_pathlist_head *refs, struct got_pathlist_head *delete_refs)

{
	const struct got_error *err = NULL;
	struct ibuf *wbuf;
	size_t len, reflen = strlen(refname);
	struct got_pathlist_entry *pe;
	int ref_valid = 0;
	char *eol;

	eol = strchr(refname, '\n');
	if (eol == NULL) {
		return got_error_msg(GOT_ERR_BAD_PACKET,
		    "unexpected message from server");
	}
	*eol = '\0';

	TAILQ_FOREACH(pe, refs, entry) {
		if (strcmp(refname, pe->path) == 0) {
			ref_valid = 1;
			break;
		}
	}
	if (!ref_valid) {
		TAILQ_FOREACH(pe, delete_refs, entry) {
			if (strcmp(refname, pe->path) == 0) {
				ref_valid = 1;
				break;
			}
		}
	}
	if (!ref_valid) {
		return got_error_msg(GOT_ERR_BAD_PACKET,
		    "unexpected message from server");
	}

	len = sizeof(struct got_imsg_send_ref_status) + reflen;
	if (len >= MAX_IMSGSIZE - IMSG_HEADER_SIZE)
		return got_error(GOT_ERR_NO_SPACE);

	wbuf = imsg_create(ibuf, GOT_IMSG_SEND_REF_STATUS,
	    0, 0, len);
	if (wbuf == NULL)
		return got_error_from_errno("imsg_create SEND_REF_STATUS");

	/* Keep in sync with struct got_imsg_send_ref_status definition! */
	if (imsg_add(wbuf, &success, sizeof(success)) == -1) {
		err = got_error_from_errno("imsg_add SEND_REF_STATUS");
		ibuf_free(wbuf);
		return err;
	}
	if (imsg_add(wbuf, &reflen, sizeof(reflen)) == -1) {
		err = got_error_from_errno("imsg_add SEND_REF_STATUS");
		ibuf_free(wbuf);
		return err;
	}
	if (imsg_add(wbuf, refname, reflen) == -1) {
		err = got_error_from_errno("imsg_add SEND_REF_STATUS");
		ibuf_free(wbuf);
		return err;
	}

	wbuf->fd = -1;
	imsg_close(ibuf, wbuf);
	return got_privsep_flush_imsg(ibuf);
}

static const struct got_error *
describe_refchange(int *n, int *sent_my_capabilites,
    const char *my_capabilities, char *buf, size_t bufsize,
    const char *refname, const char *old_hashstr, const char *new_hashstr)
{
	*n = snprintf(buf, bufsize, "%s %s %s",
	    old_hashstr, new_hashstr, refname);
	if (*n >= bufsize)
		return got_error(GOT_ERR_NO_SPACE);

	/*
	 * We must announce our capabilities along with the first
	 * reference. Unfortunately, the protocol requires an embedded
	 * NUL as a separator between reference name and capabilities,
	 * which we have to deal with here.
	 * It also requires a linefeed for terminating packet data.
	 */
	if (!*sent_my_capabilites && my_capabilities != NULL) {
		int m;
		if (*n >= bufsize - 1)
			return got_error(GOT_ERR_NO_SPACE);
		m = snprintf(buf + *n + 1, /* offset after '\0' */
		    bufsize - (*n + 1), "%s\n", my_capabilities);
		if (*n + m >= bufsize)
			return got_error(GOT_ERR_NO_SPACE);
		*n += m;
		*sent_my_capabilites = 1;
	} else {
		*n = strlcat(buf, "\n", bufsize);
		if (*n >= bufsize)
			return got_error(GOT_ERR_NO_SPACE);
	}

	return NULL;
}

static const struct got_error *
send_pack(int fd, struct got_pathlist_head *refs,
    struct got_pathlist_head *delete_refs, struct imsgbuf *ibuf)
{
	const struct got_error *err = NULL;
	char buf[GOT_FETCH_PKTMAX];
	unsigned char zero_id[SHA1_DIGEST_LENGTH] = { 0 };
	char old_hashstr[SHA1_DIGEST_STRING_LENGTH];
	char new_hashstr[SHA1_DIGEST_STRING_LENGTH];
	struct got_pathlist_head their_refs;
	int is_firstpkt = 1;
	int n, nsent = 0;
	int packfd = -1;
	char *id_str = NULL, *refname = NULL;
	struct got_object_id *id = NULL;
	char *server_capabilities = NULL, *my_capabilities = NULL;
	struct got_pathlist_entry *pe;
	int sent_my_capabilites = 0;

	TAILQ_INIT(&their_refs);

	if (TAILQ_EMPTY(refs) && TAILQ_EMPTY(delete_refs))
		return got_error(GOT_ERR_SEND_EMPTY);

	while (1) {
		err = readpkt(&n, fd, buf, sizeof(buf));
		if (err)
			goto done;
		if (n == 0)
			break;
		if (n >= 4 && strncmp(buf, "ERR ", 4) == 0) {
			err = send_error(&buf[4], n - 4);
			goto done;
		}
		err = parse_refline(&id_str, &refname, &server_capabilities,
		    buf, n);
		if (err)
			goto done;
		if (is_firstpkt) {
			if (chattygot && server_capabilities[0] != '\0')
				fprintf(stderr, "%s: server capabilities: %s\n",
				    getprogname(), server_capabilities);
			err = match_capabilities(&my_capabilities,
			    server_capabilities);
			if (err)
				goto done;
			if (chattygot)
				fprintf(stderr, "%s: my capabilities:%s\n",
				    getprogname(), my_capabilities);
			is_firstpkt = 0;
		}
		if (strstr(refname, "^{}")) {
			if (chattygot) {
				fprintf(stderr, "%s: ignoring %s\n",
				    getprogname(), refname);
			}
			continue;
		}

		id = malloc(sizeof(*id));
		if (id == NULL) {
			err = got_error_from_errno("malloc");
			goto done;
		}
		if (!got_parse_sha1_digest(id->sha1, id_str)) {
			err = got_error(GOT_ERR_BAD_OBJ_ID_STR);
			goto done;
		}
		err = send_their_ref(ibuf, id, refname);
		if (err)
			goto done;

		err = got_pathlist_append(&their_refs, refname, id);
		if (chattygot)
			fprintf(stderr, "%s: remote has %s %s\n",
			    getprogname(), refname, id_str);
		free(id_str);
		id_str = NULL;
		refname = NULL; /* do not free; owned by their_refs */
		id = NULL; /* do not free; owned by their_refs */
	}

	if (!TAILQ_EMPTY(delete_refs)) {
		if (my_capabilities == NULL ||
		    strstr(my_capabilities, GOT_CAPA_DELETE_REFS) == NULL) {
			err = got_error(GOT_ERR_CAPA_DELETE_REFS);
			goto done;
		}
	}

	TAILQ_FOREACH(pe, delete_refs, entry) {
		const char *refname = pe->path;
		struct got_pathlist_entry *their_pe;
		struct got_object_id *their_id = NULL;

		TAILQ_FOREACH(their_pe, &their_refs, entry) {
			const char *their_refname = their_pe->path;
			if (got_path_cmp(refname, their_refname,
			    strlen(refname), strlen(their_refname)) == 0) {
				their_id = their_pe->data;
				break;
			}
		}
		if (their_id == NULL) {
			err = got_error_fmt(GOT_ERR_NOT_REF,
			    "%s does not exist in remote repository",
			    refname);
			goto done;
		}

		got_sha1_digest_to_str(their_id->sha1, old_hashstr,
		    sizeof(old_hashstr));
		got_sha1_digest_to_str(zero_id, new_hashstr,
		    sizeof(new_hashstr));
		err = describe_refchange(&n, &sent_my_capabilites,
		    my_capabilities, buf, sizeof(buf), refname,
		    old_hashstr, new_hashstr);
		if (err)
			goto done;
		err = writepkt(fd, buf, n);
		if (err)
			goto done;
		if (chattygot) {
			fprintf(stderr, "%s: deleting %s %s\n",
			    getprogname(), refname, old_hashstr);
		}
		nsent++;
	}

	TAILQ_FOREACH(pe, refs, entry) {
		const char *refname = pe->path;
		struct got_object_id *id = pe->data;
		struct got_object_id *their_id = NULL;
		struct got_pathlist_entry *their_pe;

		TAILQ_FOREACH(their_pe, &their_refs, entry) {
			const char *their_refname = their_pe->path;
			if (got_path_cmp(refname, their_refname,
			    strlen(refname), strlen(their_refname)) == 0) {
				their_id = their_pe->data;
				break;
			}
		}
		if (their_id) {
			if (got_object_id_cmp(id, their_id) == 0) {
				if (chattygot) {
					fprintf(stderr,
					    "%s: no change for %s\n",
					    getprogname(), refname);
				}
				continue;
			}
			got_sha1_digest_to_str(their_id->sha1, old_hashstr,
			    sizeof(old_hashstr));
		} else {
			got_sha1_digest_to_str(zero_id, old_hashstr,
			    sizeof(old_hashstr));
		}
		got_sha1_digest_to_str(id->sha1, new_hashstr,
		    sizeof(new_hashstr));
		err = describe_refchange(&n, &sent_my_capabilites,
		    my_capabilities, buf, sizeof(buf), refname,
		    old_hashstr, new_hashstr);
		if (err)
			goto done;
		err = writepkt(fd, buf, n);
		if (err)
			goto done;
		if (chattygot) {
			if (their_id) {
				fprintf(stderr, "%s: updating %s %s -> %s\n",
				    getprogname(), refname, old_hashstr,
				    new_hashstr);
			} else {
				fprintf(stderr, "%s: creating %s %s\n",
				    getprogname(), refname, new_hashstr);
			}
		}
		nsent++;
	}
	err = flushpkt(fd);
	if (err)
		goto done;

	err = send_pack_request(ibuf);
	if (err)
		goto done;

	err = recv_packfd(&packfd, ibuf);
	if (err)
		goto done;

	err = send_pack_file(fd, packfd, ibuf);
	if (err)
		goto done;

	err = readpkt(&n, fd, buf, sizeof(buf));
	if (err)
		goto done;
	if (n >= 4 && strncmp(buf, "ERR ", 4) == 0) {
		err = send_error(&buf[4], n - 4);
		goto done;
	} else if (n < 10 || strncmp(buf, "unpack ok\n", 10) != 0) {
		err = got_error_msg(GOT_ERR_BAD_PACKET,
		    "unexpected message from server");
		goto done;
	}

	while (nsent > 0) {
		err = readpkt(&n, fd, buf, sizeof(buf));
		if (err)
			goto done;
		if (n < 3) {
			err = got_error_msg(GOT_ERR_BAD_PACKET,
			    "unexpected message from server");
			goto done;
		} else if (strncmp(buf, "ok ", 3) == 0) {
			err = send_ref_status(ibuf, buf + 3, 1,
			   refs, delete_refs);
			if (err)
				goto done;
		} else if (strncmp(buf, "ng ", 3) == 0) {
			err = send_ref_status(ibuf, buf + 3, 0,
			    refs, delete_refs);
			if (err)
				goto done;
		} else {
			err = got_error_msg(GOT_ERR_BAD_PACKET,
			    "unexpected message from server");
			goto done;
		}
		nsent--;
	}

	err = send_done(ibuf);
done:
	TAILQ_FOREACH(pe, &their_refs, entry) {
		free((void *)pe->path);
		free(pe->data);
	}
	got_pathlist_free(&their_refs);
	free(id_str);
	free(id);
	free(refname);
	free(server_capabilities);
	return err;
}

int
main(int argc, char **argv)
{
	const struct got_error *err = NULL;
	int sendfd, i;
	struct imsgbuf ibuf;
	struct imsg imsg;
	struct got_pathlist_head refs;
	struct got_pathlist_head delete_refs;
	struct got_pathlist_entry *pe;
	struct got_imsg_send_request send_req;
	struct got_imsg_send_ref href;
	size_t datalen;
#if 0
	static int attached;
	while (!attached)
		sleep (1);
#endif

	TAILQ_INIT(&refs);
	TAILQ_INIT(&delete_refs);

	imsg_init(&ibuf, GOT_IMSG_FD_CHILD);
#ifndef PROFILE
	/* revoke access to most system calls */
	if (pledge("stdio recvfd", NULL) == -1) {
		err = got_error_from_errno("pledge");
		got_privsep_send_error(&ibuf, err);
		return 1;
	}
#endif
	if ((err = got_privsep_recv_imsg(&imsg, &ibuf, 0)) != 0) {
		if (err->code == GOT_ERR_PRIVSEP_PIPE)
			err = NULL;
		goto done;
	}
	if (imsg.hdr.type == GOT_IMSG_STOP)
		goto done;
	if (imsg.hdr.type != GOT_IMSG_SEND_REQUEST) {
		err = got_error(GOT_ERR_PRIVSEP_MSG);
		goto done;
	}
	datalen = imsg.hdr.len - IMSG_HEADER_SIZE;
	if (datalen < sizeof(send_req)) {
		err = got_error(GOT_ERR_PRIVSEP_LEN);
		goto done;
	}
	memcpy(&send_req, imsg.data, sizeof(send_req));
	sendfd = imsg.fd;
	imsg_free(&imsg);

	if (send_req.verbosity > 0)
		chattygot += send_req.verbosity;

	for (i = 0; i < send_req.nrefs; i++) {
		struct got_object_id *id;
		char *refname;

		if ((err = got_privsep_recv_imsg(&imsg, &ibuf, 0)) != 0) {
			if (err->code == GOT_ERR_PRIVSEP_PIPE)
				err = NULL;
			goto done;
		}
		if (imsg.hdr.type == GOT_IMSG_STOP)
			goto done;
		if (imsg.hdr.type != GOT_IMSG_SEND_REF) {
			err = got_error(GOT_ERR_PRIVSEP_MSG);
			goto done;
		}
		datalen = imsg.hdr.len - IMSG_HEADER_SIZE;
		if (datalen < sizeof(href)) {
			err = got_error(GOT_ERR_PRIVSEP_LEN);
			goto done;
		}
		memcpy(&href, imsg.data, sizeof(href));
		if (datalen - sizeof(href) < href.name_len) {
			err = got_error(GOT_ERR_PRIVSEP_LEN);
			goto done;
		}
		refname = malloc(href.name_len + 1);
		if (refname == NULL) {
			err = got_error_from_errno("malloc");
			goto done;
		}
		memcpy(refname, imsg.data + sizeof(href), href.name_len);
		refname[href.name_len] = '\0';

		/*
		 * Prevent sending of references that won't make any
		 * sense outside the local repository's context.
		 */
		if (strncmp(refname, "refs/got/", 9) == 0 ||
		    strncmp(refname, "refs/remotes/", 13) == 0) {
			err = got_error_fmt(GOT_ERR_SEND_BAD_REF,
			    "%s", refname);
			goto done;
		}

		id = malloc(sizeof(*id));
		if (id == NULL) {
			free(refname);
			err = got_error_from_errno("malloc");
			goto done;
		}
		memcpy(id->sha1, href.id, SHA1_DIGEST_LENGTH);
		if (href.delete)
			err = got_pathlist_append(&delete_refs, refname, id);
		else
			err = got_pathlist_append(&refs, refname, id);
		if (err) {
			free(refname);
			free(id);
			goto done;
		}

		imsg_free(&imsg);
	}

	err = send_pack(sendfd, &refs, &delete_refs, &ibuf);
done:
	TAILQ_FOREACH(pe, &refs, entry) {
		free((char *)pe->path);
		free(pe->data);
	}
	got_pathlist_free(&refs);
	TAILQ_FOREACH(pe, &delete_refs, entry) {
		free((char *)pe->path);
		free(pe->data);
	}
	got_pathlist_free(&delete_refs);
	if (sendfd != -1 && close(sendfd) == -1 && err == NULL)
		err = got_error_from_errno("close");
	if (err != NULL && err->code != GOT_ERR_CANCELLED)  {
		fprintf(stderr, "%s: %s\n", getprogname(), err->msg);
		got_privsep_send_error(&ibuf, err);
	}

	exit(0);
}
