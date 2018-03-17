/*
 * Copyright (c) 2018 Stefan Sperling <stsp@openbsd.org>
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

/* A pack file segment mapped with mmap(2). */
struct got_pack_mapping {
	TAILQ_ENTRY(got_pack_mapping) entry;
	int fd;
	uint8_t *addr;
	off_t offset;
	size_t len;
};

#define GOT_PACK_MAX_OPEN_MAPPINGS	512
#define GOT_PACK_MAPPING_MIN_SIZE	8192
#define GOT_PACK_MAPPING_MAX_SIZE	(2048 * GOT_PACK_MAPPING_MIN_SIZE)

/* An open pack file. */
struct got_pack {
	char *path_packfile;
	FILE *packfile;
	size_t filesize;
	int nmappings;

	TAILQ_HEAD(, got_pack_mapping) mappings;
};

const struct got_error *got_pack_close(struct got_pack *);

/* See Documentation/technical/pack-format.txt in Git. */

struct got_packidx_trailer {
	u_int8_t	packfile_sha1[SHA1_DIGEST_LENGTH];
	u_int8_t	packidx_sha1[SHA1_DIGEST_LENGTH];
} __attribute__((__packed__));

/* Ignore pack index version 1 which is no longer written by Git. */
#define GOT_PACKIDX_VERSION 2

struct got_packidx_v2_hdr {
	uint32_t	magic;		/* big endian */
#define GOT_PACKIDX_V2_MAGIC 0xff744f63	/* "\377t0c" */
	uint32_t	version;

	/* 
	 * Each entry N in the fanout table contains the number of objects in
	 * the packfile whose SHA1 begins with a byte less than or equal to N.
	 * The last entry (index 255) contains the number of objects in the
	 * pack file whose first SHA1 byte is <= 0xff, and thus records the
	 * total number of objects in the pack file. All pointer variables
	 * below point to tables with a corresponding number of entries.
	 */
	uint32_t	fanout_table[0xff + 1];	/* values are big endian */

	/* Sorted SHA1 checksums for each object in the pack file. */
	struct got_object_id *sorted_ids;

	/* CRC32 of the packed representation of each object. */
	uint32_t	*crc32;

	/* Offset into the pack file for each object. */
	uint32_t	*offsets;		/* values are big endian */
#define GOT_PACKIDX_OFFSET_VAL_MASK		0x7fffffff
#define GOT_PACKIDX_OFFSET_VAL_IS_LARGE_IDX	0x80000000

	/* Large offsets table is empty for pack files < 2 GB. */
	uint64_t	*large_offsets;		/* values are big endian */

	struct got_packidx_trailer trailer;
};

struct got_packfile_hdr {
	uint32_t	signature;
#define GOT_PACKFILE_SIGNATURE	0x5041434b	/* 'P' 'A' 'C' 'K' */
	uint32_t	version;	/* big endian */
#define GOT_PACKFILE_VERSION 2
	uint32_t	nobjects;	/* big endian */
};

struct got_packfile_obj_hdr {
	/* 
	 * The object size field uses a variable length encoding:
	 * size0...sizeN form a 4+7+7+...+7 bit integer, where size0 is the
	 * least significant part and sizeN is the most significant part.
	 * If the MSB of a size byte is set, an additional size byte follows.
	 * Of the 7 remaining bits of size0, the first 3 bits indicate the
	 * object's type, and the remaining 4 bits contribute to the size.
	 */
	uint8_t *size;		/* variable length */
#define GOT_PACK_OBJ_SIZE_MORE		0x80
#define GOT_PACK_OBJ_SIZE0_TYPE_MASK	0x70 /* See struct got_object->type */
#define GOT_PACK_OBJ_SIZE0_TYPE_MASK_SHIFT	4
#define GOT_PACK_OBJ_SIZE0_VAL_MASK	0x0f
#define GOT_PACK_OBJ_SIZE_VAL_MASK	0x7f
};

/* If object is not a DELTA type. */
struct got_packfile_object_data {
	uint8_t *data;	/* compressed */
};

/* If object is of type	GOT_OBJ_TYPE_REF_DELTA. */
struct got_packfile_object_data_ref_delta {
	uint8_t sha1[SHA1_DIGEST_LENGTH];
	uint8_t *delta_data;		/* compressed */
};

/* If object is of type GOT_OBJ_TYPE_OFFSET_DELTA. */
struct got_packfile_object_data_offset_delta {
	/* 
	 * This offset is interpreted as a negative offset from
	 * the got_packfile_obj_hdr corresponding to this object.
	 * The size provided in the header specifies the amount
	 * of compressed delta data that follows.
	 *
	 * This field uses a variable length encoding of N bytes,
	 * where the MSB is always set except for the last byte.
	 * The value is encoded as a series of N 7 bit integers,
	 * which are concatenated, and if N > 1 the value 2^7 +
	 * 2^14 + ... + 2^(7 * (n-1)) is added to the result.
	 */
	uint8_t *offset;	/* variable length */
#define GOT_PACK_OBJ_DELTA_OFF_MORE		0x80
#define GOT_PACK_OBJ_DELTA_OFF_VAL_MASK		0x7f
	uint8_t *delta_data;		/* compressed */
};

struct got_packfile_obj_data {
	union {
		struct got_packfile_object_data data;
		struct got_packfile_object_data_ref_delta ref_delta;
		struct got_packfile_object_data_offset_delta offset_delta;
	} __attribute__((__packed__));
} __attribute__((__packed__));

const struct got_error *got_packidx_open(struct got_packidx_v2_hdr **,
    const char *);
void got_packidx_close(struct got_packidx_v2_hdr *);

const struct got_error *got_packfile_open_object(struct got_object **,
    struct got_object_id *, struct got_repository *);
const struct got_error *got_packfile_extract_object(FILE **,
    struct got_object *, struct got_repository *);
const struct got_error *got_packfile_extract_object_to_mem(uint8_t **, size_t *,
    struct got_object *, struct got_repository *);
