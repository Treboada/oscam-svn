#include "globals.h"
#include "CAM/videoguard.h"
#include "CAM/common.h"

#include "log.h"

/* CSCRYPT */
#include "cscrypt.h"

#include <stdlib.h>

#define MAX_ATR_LEN 33	// max. ATR length
#define MAX_HIST    15	// max. number of historical characters

extern int reader_serial_need_dummy_char;

//////  ====================================================================================

int aes_active = 0;
AES_KEY dkey, ekey;

static void cam_videoguard_cAES_SetKey(const unsigned char *key)
{
	AES_set_decrypt_key(key, 128, &dkey);
	AES_set_encrypt_key(key, 128, &ekey);
	aes_active = 1;
}

static int cam_videoguard_cAES_Encrypt(const unsigned char *data, int len, unsigned char *crypt)
{
	if (aes_active) {
		len = (len + 15) & (~15);	// pad up to a multiple of 16
		int i;

		for (i = 0; i < len; i += 16)
			AES_encrypt(data + i, crypt + i, (const AES_KEY *) &ekey);
		return len;
	}
	return -1;
}

//////  ====================================================================================

static inline void cam_videoguard_xxor(unsigned char *data, int len, const unsigned char *v1, const unsigned char *v2)
{
	switch (len) {	// looks ugly, but the compiler can optimize it very well ;)
		case 16:
			*((unsigned int *) data + 3) = *((unsigned int *) v1 + 3) ^ *((unsigned int *) v2 + 3);
			*((unsigned int *) data + 2) = *((unsigned int *) v1 + 2) ^ *((unsigned int *) v2 + 2);
		case 8:
			*((unsigned int *) data + 1) = *((unsigned int *) v1 + 1) ^ *((unsigned int *) v2 + 1);
		case 4:
			*((unsigned int *) data + 0) = *((unsigned int *) v1 + 0) ^ *((unsigned int *) v2 + 0);
			break;
		default:
			while (len--)
				*data++ = *v1++ ^ *v2++;
			break;
	}
}

#define xor16(v1,v2,d) cam_videoguard_xxor((d),16,(v1),(v2))
#define val_by2on3(x)  ((0xaaab*(x))>>16)	//fixed point *2/3

unsigned short cardkeys[3][32];
unsigned char stateD3A[16];

static void cam_videoguard_LongMult(unsigned short *pData, unsigned short *pLen, unsigned int mult, unsigned int carry);
static void cam_videoguard_PartialMod(unsigned short val, unsigned int count, unsigned short *outkey, const unsigned short *inkey);
static void cam_videoguard_RotateRightAndHash(unsigned char *p);
static void cam_videoguard_Reorder16A(unsigned char *dest, const unsigned char *src);
static void cam_videoguard_ReorderAndEncrypt(unsigned char *p);
static void cam_videoguard_Process_D0(const unsigned char *ins, unsigned char *data, const unsigned char *status);
static void cam_videoguard_Process_D1(const unsigned char *ins, unsigned char *data, const unsigned char *status);
static void cam_videoguard_Decrypt_D3(unsigned char *ins, unsigned char *data, const unsigned char *status);
static void cam_videoguard_PostProcess_Decrypt(unsigned char *buff, int len, unsigned char *cw1, unsigned char *cw2);
static void cam_videoguard_SetSeed(const unsigned char *Key1, const unsigned char *Key2);
static void cam_videoguard_GetCamKey(unsigned char *buff);

static void cam_videoguard_SetSeed(const unsigned char *Key1, const unsigned char *Key2)
{
	memcpy(cardkeys[1], Key1, sizeof (cardkeys[1]));
	memcpy(cardkeys[2], Key2, sizeof (cardkeys[2]));
}

static void cam_videoguard_GetCamKey(unsigned char *buff)
{
	unsigned short *tb2 = (unsigned short *) buff, c = 1;

	memset(tb2, 0, 64);
	tb2[0] = 1;
	int i;

	for (i = 0; i < 32; i++)
		cam_videoguard_LongMult(tb2, &c, cardkeys[1][i], 0);
}

static void cam_videoguard_PostProcess_Decrypt(unsigned char *buff, int len, unsigned char *cw1, unsigned char *cw2)
{
	switch (buff[0]) {
		case 0xD0:
			cam_videoguard_Process_D0(buff, buff + 5, buff + buff[4] + 5);
			break;
		case 0xD1:
			cam_videoguard_Process_D1(buff, buff + 5, buff + buff[4] + 5);
			break;
		case 0xD3:
			cam_videoguard_Decrypt_D3(buff, buff + 5, buff + buff[4] + 5);
			if (buff[1] == 0x54) {
				memcpy(cw1, buff + 5, 8);
				int ind;

				for (ind = 14; ind < len + 5;) {
					if (buff[ind] == 0x25) {
						memcpy(cw2, buff + 5 + ind + 2, 8);
						break;
					}
					if (buff[ind + 1] == 0)
						break;
					ind += buff[ind + 1];
				}
			}
			break;
	}
}

static void cam_videoguard_Process_D0(const unsigned char *ins, unsigned char *data, const unsigned char *status)
{
	switch (ins[1]) {
		case 0xb4:
			memcpy(cardkeys[0], data, sizeof (cardkeys[0]));
			break;
		case 0xbc:
		{
			unsigned short *idata = (unsigned short *) data;
			const unsigned short *key1 = (const unsigned short *) cardkeys[1];
			unsigned short key2[32];

			memcpy(key2, cardkeys[2], sizeof (key2));
			int count2;

			for (count2 = 0; count2 < 32; count2++) {
				unsigned int rem = 0, div = key1[count2];
				int i;

				for (i = 31; i >= 0; i--) {
					unsigned int x = idata[i] | (rem << 16);

					rem = (x % div) & 0xffff;
				}
				unsigned int carry = 1, t = val_by2on3(div) | 1;

				while (t) {
					if (t & 1)
						carry = ((carry * rem) % div) & 0xffff;
					rem = ((rem * rem) % div) & 0xffff;
					t >>= 1;
				}
				cam_videoguard_PartialMod(carry, count2, key2, key1);
			}
			unsigned short idatacount = 0;
			int i;

			for (i = 31; i >= 0; i--)
				cam_videoguard_LongMult(idata, &idatacount, key1[i], key2[i]);
			unsigned char stateD1[16];

			cam_videoguard_Reorder16A(stateD1, data);
			cam_videoguard_cAES_SetKey(stateD1);
			break;
		}
	}
}

static void cam_videoguard_Process_D1(const unsigned char *ins, unsigned char *data, const unsigned char *status)
{
	unsigned char iter[16], tmp[16];

	memset(iter, 0, sizeof (iter));
	memcpy(iter, ins, 5);
	xor16(iter, stateD3A, iter);
	memcpy(stateD3A, iter, sizeof (iter));

	int datalen = status - data;
	int datalen1 = datalen;

	if (datalen < 0)
		datalen1 += 15;
	int blocklen = datalen1 >> 4;
	int i;
	int iblock;

	for (i = 0, iblock = 0; i < blocklen + 2; i++, iblock += 16) {
		unsigned char in[16];

		int docalc = datalen & 0xf;
		if ((blocklen == i) && docalc) {
			memset(in, 0, sizeof (in));
			memcpy(in, &data[iblock], datalen - (datalen1 & ~0xf));
		} else if (blocklen + 1 == i) {
			memset(in, 0, sizeof (in));
			memcpy(&in[5], status, 2);
		} else
			memcpy(in, &data[iblock], sizeof (in));

		if (docalc) {
			xor16(iter, in, tmp);
			cam_videoguard_ReorderAndEncrypt(tmp);
			xor16(tmp, stateD3A, iter);
		}
	}
	memcpy(stateD3A, tmp, 16);
}

static void cam_videoguard_Decrypt_D3(unsigned char *ins, unsigned char *data, const unsigned char *status)
{
	if (ins[4] > 16)
		ins[4] -= 16;
	if (ins[1] == 0xbe)
		memset(stateD3A, 0, sizeof (stateD3A));

	unsigned char tmp[16];

	memset(tmp, 0, sizeof (tmp));
	memcpy(tmp, ins, 5);
	xor16(tmp, stateD3A, stateD3A);

	int len1 = ins[4];
	int blocklen = len1 >> 4;

	if (ins[1] != 0xbe)
		blocklen++;

	unsigned char iter[16], states[16][16];

	memset(iter, 0, sizeof (iter));
	int blockindex;

	for (blockindex = 0; blockindex < blocklen; blockindex++) {
		iter[0] += blockindex;
		xor16(iter, stateD3A, iter);
		cam_videoguard_ReorderAndEncrypt(iter);
		xor16(iter, &data[blockindex * 16], states[blockindex]);
		if (blockindex == (len1 >> 4)) {
			int c = len1 - (blockindex * 16);

			if (c < 16)
				memset(&states[blockindex][c], 0, 16 - c);
		}
		xor16(states[blockindex], stateD3A, stateD3A);
		cam_videoguard_RotateRightAndHash(stateD3A);
	}
	memset(tmp, 0, sizeof (tmp));
	memcpy(tmp + 5, status, 2);
	xor16(tmp, stateD3A, stateD3A);
	cam_videoguard_ReorderAndEncrypt(stateD3A);

	memcpy(stateD3A, status - 16, sizeof (stateD3A));
	cam_videoguard_ReorderAndEncrypt(stateD3A);

	memcpy(data, states[0], len1);
	if (ins[1] == 0xbe) {
		cam_videoguard_Reorder16A(tmp, states[0]);
		cam_videoguard_cAES_SetKey(tmp);
	}
}

static void cam_videoguard_ReorderAndEncrypt(unsigned char *p)
{
	unsigned char tmp[16];

	cam_videoguard_Reorder16A(tmp, p);
	cam_videoguard_cAES_Encrypt(tmp, 16, tmp);
	cam_videoguard_Reorder16A(p, tmp);
}

// reorder AAAABBBBCCCCDDDD to ABCDABCDABCDABCD

static void cam_videoguard_Reorder16A(unsigned char *dest, const unsigned char *src)
{
	int i;
	int j;
	int k;

	for (i = 0, k = 0; i < 4; i++)
		for (j = i; j < 16; j += 4, k++)
			dest[k] = src[j];
}

static void cam_videoguard_LongMult(unsigned short *pData, unsigned short *pLen, unsigned int mult, unsigned int carry)
{
	int i;

	for (i = 0; i < *pLen; i++) {
		carry += pData[i] * mult;
		pData[i] = (unsigned short) carry;
		carry >>= 16;
	}
	if (carry)
		pData[(*pLen)++] = carry;
}

static void cam_videoguard_PartialMod(unsigned short val, unsigned int count, unsigned short *outkey, const unsigned short *inkey)
{
	if (count) {
		unsigned int mod = inkey[count];
		unsigned short mult = (inkey[count] - outkey[count - 1]) & 0xffff;
		unsigned int i;
		unsigned int ib1;

		for (i = 0, ib1 = count - 2; i < count - 1; i++, ib1--) {
			unsigned int t = (inkey[ib1] * mult) % mod;

			mult = t - outkey[ib1];
			if (mult > t)
				mult += mod;
		}
		mult += val;
		if ((val > mult) || (mod < mult))
			mult -= mod;
		outkey[count] = (outkey[count] * mult) % mod;
	} else
		outkey[0] = val;
}

static const unsigned char table1[256] = {
	0x63, 0x7c, 0x77, 0x7b, 0xf2, 0x6b, 0x6f, 0xc5, 0x30, 0x01, 0x67, 0x2b, 0xfe, 0xd7, 0xab, 0x76,
	0xca, 0x82, 0xc9, 0x7d, 0xfa, 0x59, 0x47, 0xf0, 0xad, 0xd4, 0xa2, 0xaf, 0x9c, 0xa4, 0x72, 0xc0,
	0xb7, 0xfd, 0x93, 0x26, 0x36, 0x3f, 0xf7, 0xcc, 0x34, 0xa5, 0xe5, 0xf1, 0x71, 0xd8, 0x31, 0x15,
	0x04, 0xc7, 0x23, 0xc3, 0x18, 0x96, 0x05, 0x9a, 0x07, 0x12, 0x80, 0xe2, 0xeb, 0x27, 0xb2, 0x75,
	0x09, 0x83, 0x2c, 0x1a, 0x1b, 0x6e, 0x5a, 0xa0, 0x52, 0x3b, 0xd6, 0xb3, 0x29, 0xe3, 0x2f, 0x84,
	0x53, 0xd1, 0x00, 0xed, 0x20, 0xfc, 0xb1, 0x5b, 0x6a, 0xcb, 0xbe, 0x39, 0x4a, 0x4c, 0x58, 0xcf,
	0xd0, 0xef, 0xaa, 0xfb, 0x43, 0x4d, 0x33, 0x85, 0x45, 0xf9, 0x02, 0x7f, 0x50, 0x3c, 0x9f, 0xa8,
	0x51, 0xa3, 0x40, 0x8f, 0x92, 0x9d, 0x38, 0xf5, 0xbc, 0xb6, 0xda, 0x21, 0x10, 0xff, 0xf3, 0xd2,
	0xcd, 0x0c, 0x13, 0xec, 0x5f, 0x97, 0x44, 0x17, 0xc4, 0xa7, 0x7e, 0x3d, 0x64, 0x5d, 0x19, 0x73,
	0x60, 0x81, 0x4f, 0xdc, 0x22, 0x2a, 0x90, 0x88, 0x46, 0xee, 0xb8, 0x14, 0xde, 0x5e, 0x0b, 0xdb,
	0xe0, 0x32, 0x3a, 0x0a, 0x49, 0x06, 0x24, 0x5c, 0xc2, 0xd3, 0xac, 0x62, 0x91, 0x95, 0xe4, 0x79,
	0xe7, 0xc8, 0x37, 0x6d, 0x8d, 0xd5, 0x4e, 0xa9, 0x6c, 0x56, 0xf4, 0xea, 0x65, 0x7a, 0xae, 0x08,
	0xba, 0x78, 0x25, 0x2e, 0x1c, 0xa6, 0xb4, 0xc6, 0xe8, 0xdd, 0x74, 0x1f, 0x4b, 0xbd, 0x8b, 0x8a,
	0x70, 0x3e, 0xb5, 0x66, 0x48, 0x03, 0xf6, 0x0e, 0x61, 0x35, 0x57, 0xb9, 0x86, 0xc1, 0x1d, 0x9e,
	0xe1, 0xf8, 0x98, 0x11, 0x69, 0xd9, 0x8e, 0x94, 0x9b, 0x1e, 0x87, 0xe9, 0xce, 0x55, 0x28, 0xdf,
	0x8c, 0xa1, 0x89, 0x0d, 0xbf, 0xe6, 0x42, 0x68, 0x41, 0x99, 0x2d, 0x0f, 0xb0, 0x54, 0xbb, 0x16,
};

static void cam_videoguard_RotateRightAndHash(unsigned char *p)
{
	unsigned char t1 = p[15];
	int i;

	for (i = 0; i < 16; i++) {
		unsigned char t2 = t1;

		t1 = p[i];
		p[i] = table1[(t1 >> 1) | ((t2 & 1) << 7)];
	}
}

//////  ====================================================================================

static unsigned char CW1[8], CW2[8];

struct CmdTabEntry {
	unsigned char cla;
	unsigned char cmd;
	unsigned char len;
	unsigned char mode;
};

struct CmdTab {
	unsigned char index;
	unsigned char size;
	unsigned char Nentries;
	unsigned char dummy;
	struct CmdTabEntry e[1];
};

struct CmdTab *cmd_table = NULL;
static void cam_videoguard_memorize_cmd_table(const unsigned char *mem, int size)
{
	cmd_table = (struct CmdTab *) malloc(sizeof (unsigned char) * size);
	memcpy(cmd_table, mem, size);
}

static int cam_videoguard_cmd_table_get_info(const unsigned char *cmd, unsigned char *rlen, unsigned char *rmode)
{
	struct CmdTabEntry *pcte = cmd_table->e;
	int i;

	for (i = 0; i < cmd_table->Nentries; i++, pcte++)
		if (cmd[1] == pcte->cmd) {
			*rlen = pcte->len;
			*rmode = pcte->mode;
			return 1;
		}
	return 0;
}

static int cam_videoguard_status_ok(const unsigned char *status)
{
	return (status[0] == 0x90 || status[0] == 0x91)
		&& (status[1] == 0x00 || status[1] == 0x01 || status[1] == 0x20 || status[1] == 0x21 || status[1] == 0x80 || status[1] == 0x81 || status[1] == 0xa0 || status[1] == 0xa1);
}

static int cam_videoguard_read_cmd_len(const unsigned char *cmd)
{
	unsigned char cmd2[5];

	memcpy(cmd2, cmd, 5);
	cmd2[3] = 0x80;
	cmd2[4] = 1;
	uchar result[260];
	ushort result_size;
	if (!cam_common_cmd2card(cmd2, sizeof(cmd2), result, sizeof(result), &result_size) || result[1] != 0x90 || result[2] != 0x00) {
		log_normal("failed to read %02x%02x cmd length (%02x %02x)", cmd[1], cmd[2], result[1], result[2]);
		return -1;
	}
	return result[0];
}

static int cam_videoguard_do_cmd(const unsigned char *ins, const unsigned char *txbuff, unsigned char *rxbuff, uchar *result, ushort result_max_size, ushort *result_size)
{
	unsigned char ins2[5];

	memcpy(ins2, ins, 5);
	unsigned char len = 0, mode = 0;

	if (cam_videoguard_cmd_table_get_info(ins2, &len, &mode)) {
		if (len == 0xFF && mode == 2) {
			if (ins2[4] == 0)
				ins2[4] = len = cam_videoguard_read_cmd_len(ins2);
		} else if (mode != 0)
			ins2[4] = len;
	}
	if (ins2[0] == 0xd3)
		ins2[4] = len + 16;
	len = ins2[4];

	unsigned char tmp[264];

	if (!rxbuff)
		rxbuff = tmp;
	if (mode > 1) {
		if (!cam_common_cmd2card(ins2, sizeof(ins2), result, result_max_size, result_size) || !cam_videoguard_status_ok(result + len))
			return -1;
		memcpy(rxbuff, ins2, 5);
		memcpy(rxbuff + 5, result, len);
		memcpy(rxbuff + 5 + len, result + len, 2);
	} else {
		uchar cmd[272];
		memcpy(cmd, ins2, 5);
		memcpy(cmd + 5, txbuff, ins2[4]);
		if (!cam_common_cmd2card(cmd, 5 + ins2[4], result, sizeof(result), result_size) || !cam_videoguard_status_ok(result))
			return -2;
		memcpy(rxbuff, ins2, 5);
		memcpy(rxbuff + 5, txbuff, len);
		memcpy(rxbuff + 5 + len, result, 2);
	}

	cam_videoguard_PostProcess_Decrypt(rxbuff, len, CW1, CW2);

	return len;
}

#define BASEYEAR 1997
static void cam_videoguard_rev_date_calc(const unsigned char *Date, int *year, int *mon, int *day, int *hh, int *mm, int *ss)
{
	*year = (Date[0] / 12) + BASEYEAR;
	*mon = (Date[0] % 12) + 1;
	*day = Date[1];
	*hh = Date[2] / 8;
	*mm = (0x100 * (Date[2] - *hh * 8) + Date[3]) / 32;
	*ss = (Date[3] - *mm * 32) * 2;
}

static void cam_videoguard_read_tiers()
{
	static const unsigned char ins2a[5] = { 0xd0, 0x2a, 0x00, 0x00, 0x00 };
	int l;

	uchar result[260];
	ushort result_size;
	l = cam_videoguard_do_cmd(ins2a, NULL, NULL, result, sizeof(result), &result_size);
	if (l < 0 || !cam_videoguard_status_ok(result + l))
		return;
	static unsigned char ins76[5] = { 0xd0, 0x76, 0x00, 0x00, 0x00 };
	ins76[3] = 0x7f;
	ins76[4] = 2;
	if (!cam_common_cmd2card(ins76, sizeof(ins76), result, sizeof(result), &result_size) || !cam_videoguard_status_ok(result + 2))
		return;
	ins76[3] = 0;
	ins76[4] = 0;
	int num = result[1];
	int i;

	for (i = 0; i < num; i++) {
		ins76[2] = i;
		l = cam_videoguard_do_cmd(ins76, NULL, NULL, result, sizeof(result), &result_size);
		if (l < 0 || !cam_videoguard_status_ok(result + l))
			return;
		if (result[2] == 0 && result[3] == 0)
			break;
		int y, m, d, H, M, S;

		cam_videoguard_rev_date_calc(&result[4], &y, &m, &d, &H, &M, &S);
		log_normal("Tier: %02x%02x, expiry date: %04d/%02d/%02d-%02d:%02d:%02d", result[2], result[3], y, m, d, H, M, S);
	}
}

static unsigned int cam_videoguard_num_addr(const unsigned char *data)
{
	return ((data[3] & 0x30) >> 4) + 1;
}

static int cam_videoguard_addr_mode(const unsigned char *data)
{
	switch (data[3] & 0xC0) {
		case 0x40:
			return 3;
		case 0x80:
			return 2;
		default:
			return 0;
	}
}

static const unsigned char *cam_videoguard_payload_addr(const unsigned char *data, const unsigned char *a)
{
	int s;
	int l;
	const unsigned char *ptr = NULL;

	switch (cam_videoguard_addr_mode(data)) {
		case 2:
			s = 3;
			break;
		case 3:
			s = 4;
			break;
		default:
			return NULL;
	}

	int position = -1;

	for (l = 0; l < cam_videoguard_num_addr(data); l++) {
		if (!memcmp(&data[l * 4 + 4], a + 4, s)) {
			position = l;
			break;
		}
	}

	/* skip header, the list of address, and the separator (the two 00 00) */
	ptr = data + 4 + 4 * cam_videoguard_num_addr(data) + 2;

	/* skip optional 00 */
	if (*ptr == 0x00)
		ptr++;

	/* skip the 1st bitmap len */
	ptr++;

	/* check */
	if (*ptr != 0x02)
		return NULL;

	/* skip the 1st timestamp 02 00 or 02 06 xx aabbccdd yy */
	ptr += 2 + ptr[1];

	for (l = 0; l < position; l++) {

		/* skip the payload of the previous SA */
		ptr += 1 + ptr[0];

		/* skip optional 00 */
		if (*ptr == 0x00)
			ptr++;

		/* skip the bitmap len */
		ptr++;

		/* check */
		if (*ptr != 0x02)
			return NULL;

		/* skip the timestamp 02 00 or 02 06 xx aabbccdd yy */
		ptr += 2 + ptr[1];
	}

	return ptr;
}

int cam_videoguard_detect(uchar *atr, ushort atr_size)
{
	/* known atrs */
	unsigned char atr_bskyb[] = { 0x3F, 0x7F, 0x13, 0x25, 0x03, 0x33, 0xB0, 0x06, 0x69, 0xFF, 0x4A, 0x50, 0xD0, 0x00, 0x00, 0x53, 0x59, 0x00, 0x00, 0x00 };
	unsigned char atr_bskyb_new[] = { 0x3F, 0xFD, 0x13, 0x25, 0x02, 0x50, 0x00, 0x0F, 0x33, 0xB0, 0x0F, 0x69, 0xFF, 0x4A, 0x50, 0xD0, 0x00, 0x00, 0x53, 0x59, 0x02 };
	unsigned char atr_skyitalia[] = { 0x3F, 0xFF, 0x13, 0x25, 0x03, 0x10, 0x80, 0x33, 0xB0, 0x0E, 0x69, 0xFF, 0x4A, 0x50, 0x70, 0x00, 0x00, 0x49, 0x54, 0x02, 0x00, 0x00 };
	unsigned char atr_premiere[] = { 0x3F, 0xFF, 0x11, 0x25, 0x03, 0x10, 0x80, 0x41, 0xB0, 0x07, 0x69, 0xFF, 0x4A, 0x50, 0x70, 0x00, 0x00, 0x50, 0x31, 0x01, 0x00, 0x11 };
	unsigned char atr_directv[] = { 0x3F, 0x78, 0x13, 0x25, 0x03, 0x40, 0xB0, 0x20, 0xFF, 0xFF, 0x4A, 0x50, 0x00 };
	unsigned char atr_yes[] = { 0x3F, 0xFF, 0x13, 0x25, 0x03, 0x10, 0x80, 0x33, 0xB0, 0x11, 0x69, 0xFF, 0x4A, 0x50, 0x50, 0x00, 0x00, 0x47, 0x54, 0x01, 0x00, 0x00 };
	unsigned char atr_viasat_new[] = { 0x3F, 0x7D, 0x11, 0x25, 0x02, 0x41, 0xB0, 0x03, 0x69, 0xFF, 0x4A, 0x50, 0xF0, 0x80, 0x00, 0x56, 0x54, 0x03};
	unsigned char atr_viasat_old[] = { 0x3F, 0x7F, 0x11, 0x25, 0x03, 0x33, 0xB0, 0x09, 0x69, 0xFF, 0x4A, 0x50, 0x70, 0x00, 0x00, 0x56, 0x54, 0x01, 0x00, 0x00};
	unsigned char atr_viasat_ukraine[] = { 0x3F, 0xFF, 0x14, 0x25, 0x03, 0x10, 0x80, 0x41, 0xB0, 0x01, 0x69, 0xFF, 0x4A, 0x50, 0x70, 0x00, 0x00, 0x5A, 0x4B, 0x01, 0x00, 0x00};

	if ((atr_size == sizeof (atr_bskyb)) && (memcmp(atr, atr_bskyb, atr_size) == 0)) {
		log_normal("Type: Videoguard BSkyB");
		/* BSkyB seems to need one additionnal byte in the serial communication... */
		reader_serial_need_dummy_char = 1;
	} else if ((atr_size == sizeof (atr_bskyb_new)) && (memcmp(atr, atr_bskyb_new, atr_size) == 0)) {
		log_normal("Type: Videoguard BSkyB - New");
	} else if ((atr_size == sizeof (atr_skyitalia)) && (memcmp(atr, atr_skyitalia, atr_size) == 0)) {
		log_normal("Type: Videoguard Sky Italia");
	} else if ((atr_size == sizeof (atr_premiere)) && (memcmp(atr, atr_premiere, atr_size) == 0)) {
		log_normal("Type: Videoguard Premiere NDS");
	} else if ((atr_size == sizeof (atr_directv)) && (memcmp(atr, atr_directv, atr_size) == 0)) {
		log_normal("Type: Videoguard DirecTV");
	} else if ((atr_size == sizeof (atr_yes)) && (memcmp (atr, atr_yes, atr_size) == 0)) {
		log_normal("Type: Videoguard YES DBS Israel");
	} else if ((atr_size == sizeof (atr_viasat_new)) && (memcmp (atr, atr_viasat_new, atr_size) == 0)) {
		log_normal("Type: Videoguard Viasat new (093E)");
	} else if ((atr_size == sizeof (atr_viasat_old)) && (memcmp (atr, atr_viasat_old, atr_size) == 0)) {
		log_normal("Type: Videoguard Viasat old (090F)");
	} else if ((atr_size == sizeof (atr_viasat_ukraine)) && (memcmp (atr, atr_viasat_ukraine, atr_size) == 0)) {
		log_normal("Type: Videoguard Viasat ukraine(0931)");
	} else {
		/* not a known videoguard */
		return 0;
	}

	return 1;
}

int cam_videoguard_load_card()
{
	unsigned char ins7401[5] = { 0xD0, 0x74, 0x01, 0x00, 0x00 };
	int l;

	if ((l = cam_videoguard_read_cmd_len(ins7401)) < 0)
		return 0;
	ins7401[4] = l;
	uchar result[260];
	ushort result_size;
	if (!cam_common_cmd2card(ins7401, sizeof(ins7401), result, sizeof(result), &result_size) || !cam_videoguard_status_ok(result + l)) {
		log_normal("failed to read cmd list");
		return 0;
	}
	cam_videoguard_memorize_cmd_table(result, l);

	unsigned char buff[256];

	unsigned char ins7416[5] = { 0xD0, 0x74, 0x16, 0x00, 0x00 };
	if (cam_videoguard_do_cmd(ins7416, NULL, NULL, result, sizeof(result), &result_size) < 0) {
		log_normal("cmd 7416 failed");
		return 0;
	}

	unsigned char ins36[5] = { 0xD0, 0x36, 0x00, 0x00, 0x00 };
	unsigned char boxID[4];

	if (reader[ridx].boxid > 0) {
		/* the boxid is specified in the config */
		int i;

		for (i = 0; i < 4; i++) {
			boxID[i] = (reader[ridx].boxid >> (8 * (3 - i))) % 0x100;
		}
	} else {
		/* we can try to get the boxid from the card */
		int boxidOK = 0;

		l = cam_videoguard_do_cmd(ins36, NULL, buff, result, sizeof(result), &result_size);
		if (l >= 0) {
			int i;

			for (i = 0; i < l; i++) {
				if (buff[i+1] == 0xF3 && (buff[i] == 0x00 || buff[i] == 0x0A)) {
					memcpy(&boxID, &buff[i + 2], sizeof (boxID));
					boxidOK = 1;
					break;
				}
			}
		}

		if (!boxidOK) {
			log_normal("no boxID available");
			return 0;
		}
	}

	unsigned char ins4C[5] = { 0xD0, 0x4C, 0x00, 0x00, 0x09 };
	unsigned char payload4C[9] = { 0, 0, 0, 0, 3, 0, 0, 0, 4 };
	memcpy(payload4C, boxID, 4);
	uchar cmd[272];
	memcpy(cmd, ins4C, 5);
	memcpy(cmd + 5, payload4C, ins4C[4]);
	if (!cam_common_cmd2card(cmd, 5 + ins4C[4], result, sizeof(result), &result_size) || !cam_videoguard_status_ok(result + l)) {
		log_normal("sending boxid failed");
		return 0;
	}

	unsigned char ins58[5] = { 0xD0, 0x58, 0x00, 0x00, 0x00 };
	l = cam_videoguard_do_cmd(ins58, NULL, buff, result, sizeof(result), &result_size);
	if (l < 0) {
		log_normal("cmd ins58 failed");
		return 0;
	}
	memset(reader[ridx].hexserial, 0, 4);
	memcpy(reader[ridx].hexserial + 4, result + 3, 4);
	reader[ridx].caid[0] = result[24] * 0x100 + result[25];

	/* we have one provider, 0x0000 */
	reader[ridx].nprov = 1;
	memset(reader[ridx].prid, 0x00, sizeof (reader[ridx].prid));

	const unsigned char seed1[] = {
		0xb9, 0xd5, 0xef, 0xd5, 0xf5, 0xd5, 0xfb, 0xd5, 0x31, 0xd6, 0x43, 0xd6, 0x55, 0xd6, 0x61, 0xd6,
		0x85, 0xd6, 0x9d, 0xd6, 0xaf, 0xd6, 0xc7, 0xd6, 0xd9, 0xd6, 0x09, 0xd7, 0x15, 0xd7, 0x21, 0xd7,
		0x27, 0xd7, 0x3f, 0xd7, 0x45, 0xd7, 0xb1, 0xd7, 0xbd, 0xd7, 0xdb, 0xd7, 0x11, 0xd8, 0x23, 0xd8,
		0x29, 0xd8, 0x2f, 0xd8, 0x4d, 0xd8, 0x8f, 0xd8, 0xa1, 0xd8, 0xad, 0xd8, 0xbf, 0xd8, 0xd7, 0xd8
	};
	const unsigned char seed2[] = {
		0x01, 0x00, 0xcf, 0x13, 0xe0, 0x60, 0x54, 0xac, 0xab, 0x99, 0xe6, 0x0c, 0x9f, 0x5b, 0x91, 0xb9,
		0x72, 0x72, 0x4d, 0x5b, 0x5f, 0xd3, 0xb7, 0x5b, 0x01, 0x4d, 0xef, 0x9e, 0x6b, 0x8a, 0xb9, 0xd1,
		0xc9, 0x9f, 0xa1, 0x2a, 0x8d, 0x86, 0xb6, 0xd6, 0x39, 0xb4, 0x64, 0x65, 0x13, 0x77, 0xa1, 0x0a,
		0x0c, 0xcf, 0xb4, 0x2b, 0x3a, 0x2f, 0xd2, 0x09, 0x92, 0x15, 0x40, 0x47, 0x66, 0x5c, 0xda, 0xc9
	};
	cam_videoguard_SetSeed(seed1, seed2);

	unsigned char insB4[5] = { 0xD0, 0xB4, 0x00, 0x00, 0x40 };
	unsigned char tbuff[64];

	cam_videoguard_GetCamKey(tbuff);
	l = cam_videoguard_do_cmd(insB4, tbuff, NULL, result, sizeof(result), &result_size);
	if (l < 0 || !cam_videoguard_status_ok(result)) {
		log_normal("cmd D0B4 failed (%02X%02X)", result[0], result[1]);
		return 0;
	}

	unsigned char insBC[5] = { 0xD0, 0xBC, 0x00, 0x00, 0x00 };
	l = cam_videoguard_do_cmd(insBC, NULL, NULL, result, sizeof(result), &result_size);
	if (l < 0) {
		log_normal("cmd D0BC failed");
		return 0;
	}

	unsigned char insBE[5] = { 0xD3, 0xBE, 0x00, 0x00, 0x00 };
	l = cam_videoguard_do_cmd(insBE, NULL, NULL, result, sizeof(result), &result_size);
	if (l < 0) {
		log_normal("cmd D3BE failed");
		return 0;
	}

	unsigned char ins58a[5] = { 0xD1, 0x58, 0x00, 0x00, 0x00 };
	l = cam_videoguard_do_cmd(ins58a, NULL, NULL, result, sizeof(result), &result_size);
	if (l < 0) {
		log_normal("cmd D158 failed");
		return 0;
	}

	unsigned char ins4Ca[5] = { 0xD1, 0x4C, 0x00, 0x00, 0x00 };
	l = cam_videoguard_do_cmd(ins4Ca, payload4C, NULL, result, sizeof(result), &result_size);
	if (l < 0 || !cam_videoguard_status_ok(result)) {
		log_normal("cmd D14Ca failed");
		return 0;
	}

	log_normal("caid: %04X, serial: %02X%02X%02X%02X, BoxID: %02X%02X%02X%02X", reader[ridx].caid[0], reader[ridx].hexserial[4], reader[ridx].hexserial[5], reader[ridx].hexserial[6], reader[ridx].hexserial[7], boxID[0], boxID[1], boxID[2], boxID[3]);

	cam_videoguard_read_tiers();

	return 1;
}

int cam_videoguard_process_ecm(ECM_REQUEST * er)
{
	static unsigned char ins40[5] = { 0xD1, 0x40, 0x40, 0x80, 0xFF };
	static const unsigned char ins54[5] = { 0xD3, 0x54, 0x00, 0x00, 0x00 };
	int posECMpart2 = er->ecm[6] + 7;
	int lenECMpart2 = er->ecm[posECMpart2] + 1;
	unsigned char tbuff[264];

	tbuff[0] = 0;
	memcpy(&tbuff[1], &(er->ecm[posECMpart2 + 1]), lenECMpart2 - 1);
	ins40[4] = lenECMpart2;
	int l;

	uchar result[260];
	ushort result_size;
	l = cam_videoguard_do_cmd(ins40, tbuff, NULL, result, sizeof(result), &result_size);
	if (l > 0 && cam_videoguard_status_ok(result)) {
		l = cam_videoguard_do_cmd(ins54, NULL, NULL, result, sizeof(result), &result_size);
		if (l > 0 && cam_videoguard_status_ok(result + l)) {
			if (er->ecm[0] & 1) {
				memcpy(er->cw + 8, CW1, 8);
			} else {
				memcpy(er->cw + 0, CW1, 8);
			}

			return 1;
		}
	}

	return 0;
}

int cam_videoguard_process_emm(EMM_PACKET * ep)
{
	unsigned char ins42[5] = { 0xD1, 0x42, 0x00, 0x00, 0xFF };
	uchar result[260];
	ushort result_size;
	int rc = 0;

	const unsigned char *payload = cam_videoguard_payload_addr(ep->emm, reader[ridx].hexserial);

	if (payload) {
		ins42[4] = *payload;
		int l = cam_videoguard_do_cmd(ins42, payload + 1, NULL, result, sizeof(result), &result_size);

		if (l > 0 && cam_videoguard_status_ok(result)) {
			rc = 1;
		}

		log_normal("EMM request return code : %02X%02X", result[0], result[1]);
		if (cam_videoguard_status_ok(result)) {
			cam_videoguard_read_tiers();
		}

	}

	return rc;
}
