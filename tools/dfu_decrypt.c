/*
 * dfu_decrypt.c -- decrypt an Apple .im4p with GID0 AES on the DEVICE,
 * using the checkm8 payload's exec channel (gaster semantics).
 *
 * Why: A9 DFU images (iBSS/iBEC) are AES-256-CBC encrypted with the GID0
 * key fused in the SoC. The key never leaves the chip, so decryption must
 * run on-device. Our E19 payload (gaster's handle_checkm8_request) exposes
 * the exec channel:
 *     request : DFU_DNLOAD (0x21/1), 72-byte exec header + payload bytes
 *     response: GET (0xA1/2, wValue=0xFFFF), DONE_MAGIC + ret + data
 * (source: gaster.c gaster_aes/gaster_command, 0x7ff/gaster master,
 *  fetched 2026-11-01; layout verified against our payload tail in
 *  src/dfu/checkm8_payload.c -- same EXEC_MAGIC/DONE_MAGIC).
 *
 * For CPID 0x8000 (SRTG iBoot-2234.0.0.3.3) gaster uses:
 *     func = aes_crypto_cmd = 0x10000DAA0 (matches our chip DB,
 *     include/device/chip_db_table.h)
 *     dst = insecure_memory_base + 9*8 = 0x180380048
 *     src = insecure_memory_base + 2*8 = 0x180380010
 *     cmd = AES_CMD_CBC|AES_CMD_DEC = 17
 *     options = AES_KEY_SZ_256|AES_KEY_TYPE_GID0 = 0x20000200
 * Input = kbag (IV||KEY, 48 bytes); the GID0 AES decrypts it in place.
 *
 * After decryption host-side we check the compressed-image header
 * ("comp"/"lzss", big-endian sizes, 0x16C pad) and LZSS-decompress
 * (decompressor ported verbatim from gaster.c).
 *
 * Usage: dfu_decrypt <in.im4p> <out.bin>
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <CommonCrypto/CommonCryptor.h>
#include <libirecovery.h>

#define sleep_ms(ms) usleep((ms) * 1000)

#define EXEC_MAGIC      0x6578656365786563ULL   /* "execexec" */
#define DONE_MAGIC      0x646F6E65646F6E65ULL   /* "donedone" */

#define AES_CMD_DEC     1U
#define AES_CMD_CBC     16U
#define AES_KEY_SZ_256  0x20000000U
#define AES_KEY_TYPE_GID0 0x200U

#define CPID_8000_AES_CRYPTO_CMD      0x10000DAA0ULL
#define CPID_8000_INSECURE_MEM_BASE   0x180380000ULL

#define KBAG_SZ         48
#define DFU_FILE_SUFFIX_LEN 16
#define COMP_HDR_PAD_SZ 0x16C
#define LZSS_F          18
#define LZSS_N          4096
#define LZSS_THRESHOLD  2

static int ctrl(irecv_client_t client, uint8_t rt, uint8_t req,
                uint16_t wv, uint16_t wi, unsigned char *data,
                uint16_t len)
{
	return irecv_usb_control_transfer(client, rt, req, wv, wi, data,
	                                  len, 5000);
}

/* ---- minimal DER walker for IM4P --------------------------------- */

struct tlv { const uint8_t *buf; size_t len; };

static int der_read_tlv(const uint8_t *p, size_t n, size_t *pos,
                        uint8_t *tag, struct tlv *out)
{
	size_t i = *pos;
	if (i + 2 > n)
		return -1;
	*tag = p[i++];
	size_t len = p[i++];
	if (len & 0x80) {
		size_t nlen = len & 0x7F;
		if (nlen == 0 || nlen > 4 || i + nlen > n)
			return -1;
		len = 0;
		while (nlen--)
			len = (len << 8) | p[i++];
	}
	if (i + len > n)
		return -1;
	out->buf = p + i;
	out->len = len;
	*pos = i + len;
	return 0;
}

/* Find the largest OCTET STRING (payload data) and the KBAG blob. */
static int im4p_parse(const uint8_t *p, size_t n, struct tlv *data,
                      struct tlv *kbag)
{
	size_t pos = 0;
	uint8_t tag;
	struct tlv v;

	memset(data, 0, sizeof(*data));
	memset(kbag, 0, sizeof(*kbag));

	if (der_read_tlv(p, n, &pos, &tag, &v) != 0 || tag != 0x30)
		return -1;

	const uint8_t *seq = v.buf;
	size_t seq_n = v.len;
	pos = 0;
	size_t best = 0;
	while (der_read_tlv(seq, seq_n, &pos, &tag, &v) == 0) {
		if (tag == 0x04) {           /* OCTET STRING */
			if (v.len > best) {  /* payload data = largest */
				best = v.len;
				*data = v;
			}
			/* KBAG = small OCTET wrapping SEQ{SEQ{INT 1,
			 * IV, KEY}} -- size cap excludes the (large,
			 * encrypted) data blob. */
			if (v.len <= 256 && v.len > 4 &&
			    v.buf[0] == 0x30)
				*kbag = v;
		}
	}
	return (data->len && kbag->len) ? 0 : -1;
}

static int get_status(irecv_client_t client, unsigned char st[6])
{
	return ctrl(client, 0xA1, 3, 0, 0, st, 6);
}

/* ---- on-device GID0 AES ------------------------------------------ */

static int device_decrypt_kbag(irecv_client_t client,
                               uint8_t kbag[KBAG_SZ])
{
	/* gaster exec_cmd layout (gaster.c gaster_aes): magic, func,
	 * x[0]=cmd, x[1]=dst, x[2]=src, x[3]=len, x[4]=options,
	 * x[5]=x[6]=0 -- the memcpy covers 72 bytes (x[7] excluded),
	 * then the kbag bytes follow.  Total = 72 + 48 = 120 bytes. */
	struct {
		uint64_t magic, func;
		uint64_t x[7];      /* cmd, dst, src, len, options, 0, 0 */
	} exec;
	uint8_t req[72 + KBAG_SZ];
	uint8_t rsp[2 * 8 + KBAG_SZ];
	unsigned char zeros[DFU_FILE_SUFFIX_LEN] = { 0 };
	int r;

	memset(&exec, 0, sizeof(exec));
	exec.magic   = EXEC_MAGIC;
	exec.func    = CPID_8000_AES_CRYPTO_CMD;
	exec.x[0]    = AES_CMD_CBC | AES_CMD_DEC;           /* cmd     */
	exec.x[1]    = CPID_8000_INSECURE_MEM_BASE + 9 * 8; /* dst     */
	exec.x[2]    = CPID_8000_INSECURE_MEM_BASE + 2 * 8; /* src     */
	exec.x[3]    = KBAG_SZ;                             /* len     */
	exec.x[4]    = AES_KEY_SZ_256 | AES_KEY_TYPE_GID0;  /* options */

	memcpy(req, &exec, sizeof(exec));   /* 72 bytes: x[7] excluded */
	memcpy(req + sizeof(exec), kbag, KBAG_SZ);

	/* gaster_command sequence (gaster.c): dfu_send_data (DNLOAD
	 * chunks + 16-byte zero suffix + ZLP + status walk to state 8
	 * MANIFEST_WAIT_RESET) -- NO reset -- then the 0xA1/2 exec GET
	 * on the SAME handle.  In state 8 the hooked dfu_handle_request
	 * intercepts wValue=0xFFFF and executes the command. */
	r = ctrl(client, 0x21, 1, 0, 0, req, (uint16_t)sizeof(req));
	if (r < 0) {
		fprintf(stderr, "exec DNLOAD failed r=%d\n", r);
		return -1;
	}
	r = ctrl(client, 0x21, 1, 0, 0, zeros, DFU_FILE_SUFFIX_LEN);
	if (r < 0) {
		fprintf(stderr, "suffix DNLOAD failed r=%d\n", r);
		return -1;
	}
	r = ctrl(client, 0x21, 1, 0, 0, NULL, 0);
	if (r < 0) {
		fprintf(stderr, "ZLP DNLOAD failed r=%d\n", r);
		return -1;
	}
	for (int i = 0; i < 3; i++) {
		unsigned char st[6] = { 0 };
		if (get_status(client, st) < 0) {
			fprintf(stderr, "status walk failed at %d\n", i);
			return -1;
		}
		if (st[4] != 6 + i) {
			fprintf(stderr, "unexpected DFU state %u "
			        "(want %d)\n", st[4], 6 + i);
			return -1;
		}
	}

	memset(rsp, 0, sizeof(rsp));
	r = ctrl(client, 0xA1, 2, 0xFFFF, 0, rsp, (uint16_t)sizeof(rsp));
	if (r < 0) {
		fprintf(stderr, "exec GET failed r=%d\n", r);
		return -1;
	}

	uint64_t magic, ret;
	memcpy(&magic, rsp, 8);
	memcpy(&ret, rsp + 8, 8);
	if (magic != DONE_MAGIC) {
		fprintf(stderr, "bad DONE magic %#llx\n",
		        (unsigned long long)magic);
		irecv_close(client);
		return -1;
	}
	if (ret != 0) {
		fprintf(stderr, "device AES failed ret=%lld\n",
		        (long long)ret);
		irecv_close(client);
		return -1;
	}
	memcpy(kbag, rsp + 16, KBAG_SZ);
	irecv_close(client);
	return 0;
}

/* ---- LZSS (verbatim port from gaster.c) -------------------------- */

static size_t decompress_lzss(const uint8_t *src, size_t src_len,
                              uint8_t *dst, size_t dst_len)
{
	const uint8_t *src_end = src + src_len;
	uint8_t *dst_start = dst, *dst_end = dst + dst_len;
	uint16_t i, r = LZSS_N - LZSS_F, flags = 0;
	uint8_t text_buf[LZSS_N + LZSS_F - 1], j;

	memset(text_buf, ' ', r);
	while (src != src_end && dst != dst_end) {
		if (((flags >>= 1) & 0x100) == 0) {
			flags = *src++ | 0xFF00U;
			if (src == src_end)
				break;
		}
		if ((flags & 1) != 0) {
			text_buf[r++] = *dst++ = *src++;
			r &= LZSS_N - 1;
		} else {
			i = *src++;
			if (src == src_end)
				break;
			j = *src++;
			i |= (j & 0xF0U) << 4;
			j = (j & 0xFU) + LZSS_THRESHOLD;
			do {
				*dst++ = text_buf[r++] =
				    text_buf[i++ & (LZSS_N - 1)];
				r &= LZSS_N - 1;
			} while (j-- != 0 && dst != dst_end);
		}
	}
	return (size_t)(dst - dst_start);
}

int main(int argc, char **argv)
{
	if (argc != 3) {
		fprintf(stderr, "usage: %s <in.im4p> <out.bin>\n", argv[0]);
		return 2;
	}

	/* read input */
	FILE *f = fopen(argv[1], "rb");
	if (!f) { perror("fopen"); return 2; }
	fseek(f, 0, SEEK_END);
	long fsz = ftell(f);
	fseek(f, 0, SEEK_SET);
	uint8_t *blob = malloc((size_t)fsz);
	if (!blob || fread(blob, 1, (size_t)fsz, f) != (size_t)fsz) {
		fprintf(stderr, "read failed\n");
		return 2;
	}
	fclose(f);
	fprintf(stderr, "input: %s (%ld bytes)\n", argv[1], fsz);

	/* parse IM4P */
	struct tlv data, kbag_tlv;
	if (im4p_parse(blob, (size_t)fsz, &data, &kbag_tlv) != 0) {
		fprintf(stderr, "IM4P parse failed (need data+KBAG)\n");
		return 1;
	}
	fprintf(stderr, "data: %zu bytes, kbag: %zu bytes\n",
	        data.len, kbag_tlv.len);

	/* extract iv||key from KBAG.  Real layout (dumped from
	 * iBSS.n69u 19H422): OCTET{ SEQ{ SEQ{INT 1, IV(16), KEY(32)},
	 * [second entry for dev builds] } } -- first entry is the
	 * production KBAG (matches gaster img4_get_kbag, which decodes
	 * SEQ then SEQ then INT==1 then the two OCTETs). */
	uint8_t kbag[KBAG_SZ];
	{
		const uint8_t *k = kbag_tlv.buf;
		size_t kn = kbag_tlv.len, pos = 0;
		uint8_t tag;
		struct tlv v, inner;
		int found = 0;
		while (!found &&
		       der_read_tlv(k, kn, &pos, &tag, &v) == 0) {
			if (tag != 0x30)
				continue;
			size_t ipos = 0;
			while (!found &&
			       der_read_tlv(v.buf, v.len, &ipos, &tag,
			                    &inner) == 0) {
				if (tag != 0x30)
					continue;
				size_t jpos = 0;
				struct tlv idx, iv, key;
				if (der_read_tlv(inner.buf, inner.len,
				                 &jpos, &tag,
				                 &idx) != 0 ||
				    tag != 0x02)
					continue;
				if (der_read_tlv(inner.buf, inner.len,
				                 &jpos, &tag,
				                 &iv) != 0 ||
				    tag != 0x04 || iv.len != 16)
					continue;
				if (der_read_tlv(inner.buf, inner.len,
				                 &jpos, &tag,
				                 &key) != 0 ||
				    tag != 0x04 || key.len != 32)
					continue;
				memcpy(kbag, iv.buf, 16);
				memcpy(kbag + 16, key.buf, 32);
				found = 1;
			}
		}
		if (!found) {
			fprintf(stderr, "no IV/KEY in KBAG\n");
			return 1;
		}
	}

	/* device-side decrypt */
	irecv_client_t client = NULL;
	if (irecv_open_with_ecid(&client, 0) != IRECV_E_SUCCESS) {
		fprintf(stderr, "no DFU device\n");
		return 1;
	}
	if (device_decrypt_kbag(client, kbag) != 0) {
		irecv_close(client);
		return 1;
	}
	irecv_close(client);

	fprintf(stderr, "kbag iv: ");
	for (int i = 0; i < 16; i++) fprintf(stderr, "%02X", kbag[i]);
	fprintf(stderr, "\nkbag key: ");
	for (int i = 16; i < 48; i++) fprintf(stderr, "%02X", kbag[i]);
	fprintf(stderr, "\n");

	/* host-side AES-256-CBC decrypt of the payload */
	uint8_t *dec = malloc(data.len);
	size_t moved = 0;
	if (CCCrypt(kCCDecrypt, kCCAlgorithmAES, 0,
	            kbag + 16, kCCKeySizeAES256, kbag,
	            data.buf, data.len, dec, data.len, &moved) != kCCSuccess) {
		fprintf(stderr, "CCCrypt failed\n");
		return 1;
	}

	/* compressed-image header? ("comp"/"lzss" big-endian) */
	uint32_t magic = (uint32_t)dec[0] << 24 | (uint32_t)dec[1] << 16 |
	                 (uint32_t)dec[2] << 8 | dec[3];
	if (magic == 0x636F6D70) {                       /* "comp" */
		uint32_t type = (uint32_t)dec[4] << 24 | dec[5] << 16 |
		                dec[6] << 8 | dec[7];
		uint32_t uncomp = (uint32_t)dec[12] << 24 | dec[13] << 16 |
		                  dec[14] << 8 | dec[15];
		uint32_t comp_sz = (uint32_t)dec[16] << 24 | dec[17] << 16 |
		                   dec[18] << 8 | dec[19];
		fprintf(stderr, "comp type=%08x uncomp=%u comp=%u\n",
		        type, uncomp, comp_sz);
		if (type != 0x6C7A7373) {                    /* "lzss" */
			fprintf(stderr, "unsupported compression\n");
			return 1;
		}
		uint8_t *out = malloc(uncomp);
		size_t got = decompress_lzss(dec + 20 + COMP_HDR_PAD_SZ,
		                             comp_sz, out, uncomp);
		if (got != uncomp) {
			fprintf(stderr, "lzss short (%zu != %u)\n",
			        got, uncomp);
			return 1;
		}
		free(dec);
		dec = out;
		data.len = uncomp;
	} else {
		fprintf(stderr, "no comp header, using raw decrypted data\n");
	}

	FILE *o = fopen(argv[2], "wb");
	if (!o) { perror("fopen out"); return 2; }
	fwrite(dec, 1, data.len, o);
	fclose(o);
	fprintf(stderr, "wrote %s (%zu bytes)\n", argv[2], data.len);
	free(dec);
	return 0;
}
