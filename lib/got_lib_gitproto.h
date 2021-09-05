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

struct got_pathlist_head;

const struct got_error *got_gitproto_parse_refline(char **id_str,
    char **refname, char **server_capabilities, char *line, int len);
const struct got_error *got_gitproto_match_capabilities(
    char **common_capabilities,
    struct got_pathlist_head *symrefs, char *server_capabilities,
    const struct got_capability my_capabilities[], size_t ncapa);
