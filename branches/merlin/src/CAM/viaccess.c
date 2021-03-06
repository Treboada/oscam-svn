#include "globals.h"
#include "CAM/viaccess.h"
#include "CAM/common.h"

#include "simples.h"
#include "log.h"

/* CSCRYPT */
#include "cscrypt.h"

#include <stdio.h>

struct geo_cache {
	ulong provid;
	uchar geo[256];
	uchar geo_len;
};

static struct geo_cache last_geo;

struct via_date {
	ushort day_s:5;
	ushort month_s:4;
	ushort year_s:7;

	ushort day_e:5;
	ushort month_e:4;
	ushort year_e:7;
};

static AES_KEY cam_viaccess_AES_key;
  
static void cam_viaccess_parse_via_date(const uchar * buf, struct via_date *vd, int fend)
{
	ushort date;

	date = (buf[0] << 8) | buf[1];
	vd->day_s = date & 0x1f;
	vd->month_s = (date >> 5) & 0x0f;
	vd->year_s = (date >> 9) & 0x7f;

	if (fend) {
		date = (buf[2] << 8) | buf[3];
		vd->day_e = date & 0x1f;
		vd->month_e = (date >> 5) & 0x0f;
		vd->year_e = (date >> 9) & 0x7f;
	}
}

static void cam_viaccess_show_class(const char *p, const uchar * b, int l)
{
	int i, j;

	// b -> via date (4 bytes)
	b += 4;
	l -= 4;

	j = l - 1;
	for (; j >= 0; j--)
		for (i = 0; i < 8; i++)
			if (b[j] & (1 << (i & 7))) {
				uchar cls;
				struct via_date vd;

				cam_viaccess_parse_via_date(b - 4, &vd, 1);
				cls = (l - (j + 1)) * 8 + i;
				if (p)
					log_normal("%sclass: %02X, expiry date: %04d/%02d/%02d - %04d/%02d/%02d", p, cls, vd.year_s + 1980, vd.month_s, vd.day_s, vd.year_e + 1980, vd.month_e, vd.day_e);
				else
					log_normal("class: %02X, expiry date: %04d/%02d/%02d - %04d/%02d/%02d", cls, vd.year_s + 1980, vd.month_s, vd.day_s, vd.year_e + 1980, vd.month_e, vd.day_e);
			}

/*
	int i, j, byts;
	const uchar *oemm;

	oemm = emm;
	byts = emm[1]-4;
	emm+=6;

	j=byts-1;
	for (; j>=0; j--) {
		for (i = 0; i < 8; i++) {
			if (emm[j] & (1 << (i&7))) {
				uchar cls;
				struct via_date vd;
				cam_viaccess_parse_via_date(emm-4, &vd, 1);
				cls = (byts-(j+1))*8+i;
				log_normal("%sclass %02X: expiry date: %02d/%02d/%04d - %02d/%02d/%04d", fnano?"nano A9: ":"", cls, vd.day_s, vd.month_s, vd.year_s+1980, vd.day_e, vd.month_e, vd.year_e+1980);
			}
		}
	}
*/
}

static void cam_viaccess_show_subs(const uchar * emm)
{
	// emm -> A9, A6, B6

	switch (emm[0]) {
		case 0xA9:
			cam_viaccess_show_class("nano A9: ", emm + 2, emm[1]);
			break;
		case 0xA6:
		{
			char szGeo[256];

			memset(szGeo, 0, 256);
			strncpy(szGeo, (char *) emm + 2, emm[1]);
			log_normal("nano A6: geo %s", szGeo);
			break;
		}
		case 0xB6:
		{
			uchar m;	// modexp
			struct via_date vd;

			m = emm[emm[1] + 1];
			cam_viaccess_parse_via_date(emm + 2, &vd, 0);
			log_normal("nano B6: modexp %d%d%d%d%d%d: %02d/%02d/%04d", (m & 0x20) ? 1 : 0, (m & 0x10) ? 1 : 0, (m & 0x08) ? 1 : 0, (m & 0x04) ? 1 : 0, (m & 0x02) ? 1 : 0, (m & 0x01) ? 1 : 0, vd.day_s, vd.month_s, vd.year_s + 1980);
			break;
		}
	}
}

static int cam_viaccess_chk_prov(uchar * id, uchar keynr)
{
	int i, j, rc;

	for (rc = i = 0; (!rc) && (i < reader[ridx].nprov); i++)
		if (!memcmp(&reader[ridx].prid[i][1], id, 3))
			for (j = 0; (!rc) && (j < 16); j++)
				if (reader[ridx].availkeys[i][j] == keynr)
					rc = 1;
	return (rc);
}

static int cam_viaccess_card_send_ins(const uchar *cmd, const uchar *data, uchar *result, ushort result_max_size, ushort *result_size)
{
	uchar buf[256];
	memcpy(buf, cmd, 5);
	memcpy(buf + 5, data, cmd[4]);
	return cam_common_cmd2card(buf, 5 + cmd[4], result, result_max_size, result_size);
}

int cam_viaccess_detect(uchar *atr, ushort atr_size)
{
	if (atr[0] == 0x3f && atr[1] == 0x77 && atr[2] == 0x18 && atr[9] == 0x68) {
		return 1;
	}

	return 0;
}

int cam_viaccess_load_card()
{
	int i, l, scls, show_cls;
	static uchar insac[] = { 0xca, 0xac, 0x00, 0x00, 0x00 };	// select data
	static uchar insb8[] = { 0xca, 0xb8, 0x00, 0x00, 0x00 };	// read selected data
	static uchar insa4[] = { 0xca, 0xa4, 0x00, 0x00, 0x00 };	// select issuer
	static uchar insc0[] = { 0xca, 0xc0, 0x00, 0x00, 0x00 };	// read data item
	static uchar ins24[] = { 0xca, 0x24, 0x00, 0x00, 0x09 };	// set pin

	static uchar cls[] = { 0x00, 0x21, 0xff, 0x9f };
	static uchar pin[] = { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x04 };

	static uchar insFAC[] = { 0x87, 0x02, 0x00, 0x00, 0x03 };	// init FAC
	static uchar FacDat[] = { 0x00, 0x00, 0x28 };

	uchar buf[256];
	uchar result[260];
	ushort result_size;

	cam_viaccess_card_send_ins(insFAC, FacDat, result, sizeof(result), &result_size);
	if (!(result[result_size - 2] == 0x90 && result[result_size - 1] == 0))
		return (0);

//	switch((atr[atr_size-4]<<8)|atr[atr_size-3]) {
//		case 0x6268: ver="2.3"; break;
//		case 0x6668: ver="2.4(?)"; break;
//		case 0xa268:
//		default: ver="unknown"; break;
//	}

	reader[ridx].caid[0] = 0x500;
	memset(reader[ridx].prid, 0xff, sizeof (reader[ridx].prid));
	insac[2] = 0xa4;
	cam_common_cmd2card(insac, sizeof(insac), result, sizeof(result), &result_size);	// request unique id
	insb8[4] = 0x07;
	cam_common_cmd2card(insb8, sizeof(insb8), result, sizeof(result), &result_size);	// read unique id
	memcpy(reader[ridx].hexserial, result + 2, 5);
	log_normal("type: viaccess(%sstandard atr), caid: %04X, serial: %llu", reader[ridx].card_atr[9] == 0x68 ? "" : "non-", reader[ridx].caid[0], b2ll(5, result + 2));

	i = 0;
	insa4[2] = 0x00;
	cam_common_cmd2card(insa4, sizeof(insa4), result, sizeof(result), &result_size);	// select issuer 0
	buf[0] = 0;
	while ((result[result_size - 2] == 0x90) && (result[result_size - 1] == 0)) {
		insc0[4] = 0x1a;
		cam_common_cmd2card(insc0, sizeof(insc0), result, sizeof(result), &result_size);	// show provider properties
		result[2] &= 0xF0;
		reader[ridx].prid[i][0] = 0;
		memcpy(&reader[ridx].prid[i][1], result, 3);
		memcpy(&reader[ridx].availkeys[i][0], result + 10, 16);
		sprintf((char *) buf + strlen((char *) buf), ",%06lX", b2i(3, &reader[ridx].prid[i][1]));

		insac[2] = 0xa5;
		cam_common_cmd2card(insac, sizeof(insac), result, sizeof(result), &result_size);	// request sa
		insb8[4] = 0x06;
		cam_common_cmd2card(insb8, sizeof(insb8), result, sizeof(result), &result_size);	// read sa
		memcpy(&reader[ridx].sa[i][0], result + 2, 4);

/*
		insac[2]=0xa7; cam_common_cmd2card(insac, sizeof(insac), result, sizeof(result), &result_size); // request name
		insb8[4]=0x02; cam_common_cmd2card(insb8, sizeof(insb8), result, sizeof(result), &result_size); // read name nano + len
		l = result[1];
		insb8[4] = l; cam_common_cmd2card(insb8, sizeof(insb8), result, sizeof(result), &result_size); // read name
		result[l] = 0;
		log_normal("name: %s", result);
*/

		insa4[2] = 0x02;
		cam_common_cmd2card(insa4, sizeof(insa4), result, sizeof(result), &result_size);	// select next issuer
		i++;
	}
	reader[ridx].nprov = i;
	log_normal("providers: %d (%s)", reader[ridx].nprov, buf + 1);

	/* init the maybe existing aes key */
	AES_set_decrypt_key((const unsigned char *) reader[ridx].aes_key, 128, &cam_viaccess_AES_key);

	/* disabling parental lock. assuming pin "0000" */
	if (cfg->ulparent) {
		static uchar inDPL[] = { 0xca, 0x24, 0x02, 0x00, 0x09 };
		static uchar cmDPL[] = { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0F };
		cam_viaccess_card_send_ins(inDPL, cmDPL, result, sizeof(result), &result_size);
		if (!(result[result_size - 2] == 0x90 && result[result_size - 1] == 0))
			log_normal("Can't disable parental lock. Wrong PIN? I assumed 0000!");
		else
			log_normal("Parental lock disabled");
	}

	show_cls = reader[ridx].show_cls;
	memset(&last_geo, 0, sizeof (last_geo));

	// set pin
	cam_viaccess_card_send_ins(ins24, pin, result, sizeof(result), &result_size);

	insac[2] = 0xa4;
	cam_common_cmd2card(insac, sizeof(insac), result, sizeof(result), &result_size);	// request unique id
	insb8[4] = 0x07;
	cam_common_cmd2card(insb8, sizeof(insb8), result, sizeof(result), &result_size);	// read unique id
	log_normal("serial: %llu", b2ll(5, result + 2));

	scls = 0;
	insa4[2] = 0x00;
	cam_common_cmd2card(insa4, sizeof(insa4), result, sizeof(result), &result_size);	// select issuer 0
	for (i = 1; (result[result_size - 2] == 0x90) && (result[result_size - 1] == 0); i++) {
		ulong l_provid, l_sa;
		uchar l_name[64];

		insc0[4] = 0x1a;
		cam_common_cmd2card(insc0, sizeof(insc0), result, sizeof(result), &result_size);	// show provider properties
		result[2] &= 0xF0;
		l_provid = b2i(3, result);

		insac[2] = 0xa5;
		cam_common_cmd2card(insac, sizeof(insac), result, sizeof(result), &result_size);	// request sa
		insb8[4] = 0x06;
		cam_common_cmd2card(insb8, sizeof(insb8), result, sizeof(result), &result_size);	// read sa
		l_sa = b2i(4, result + 2);

		insac[2] = 0xa7;
		cam_common_cmd2card(insac, sizeof(insac), result, sizeof(result), &result_size);	// request name
		insb8[4] = 0x02;
		cam_common_cmd2card(insb8, sizeof(insb8), result, sizeof(result), &result_size);	// read name nano + len
		l = result[1];
		insb8[4] = l;
		cam_common_cmd2card(insb8, sizeof(insb8), result, sizeof(result), &result_size);	// read name
		result[l] = 0;
		trim((char *) result);
		if (result[0])
			snprintf((char *) l_name, sizeof (l_name), ", name: %s", result);
		else
			l_name[0] = 0;

		// read GEO
		insac[2] = 0xa6;
		cam_common_cmd2card(insac, sizeof(insac), result, sizeof(result), &result_size);	// request GEO
		insb8[4] = 0x02;
		cam_common_cmd2card(insb8, sizeof(insb8), result, sizeof(result), &result_size);	// read GEO nano + len
		l = result[1];
		insb8[4] = l;
		cam_common_cmd2card(insb8, sizeof(insb8), result, sizeof(result), &result_size);	// read geo
		log_normal("provider: %d, id: %06X%s, sa: %08X, geo: %s", i, l_provid, l_name, l_sa, (l < 4) ? "empty" : cs_hexdump(1, result, l));

		// read classes subscription
		insac[2] = 0xa9;
		insac[4] = 4;
		cam_viaccess_card_send_ins(insac, cls, result, sizeof(result), &result_size);	// request class subs
		scls = 0;
		while ((result[result_size - 2] == 0x90) && (result[result_size - 1] == 0)) {
			insb8[4] = 0x02;
			cam_common_cmd2card(insb8, sizeof(insb8), result, sizeof(result), &result_size);	// read class subs nano + len
			if ((result[result_size - 2] == 0x90) && (result[result_size - 1] == 0)) {
				int fshow;

				l = result[1];
				//fshow = (client[cs_idx].dbglvl==D_DUMP)?1:(scls < show_cls) ? 1 : 0;
				fshow = (scls < show_cls);
				insb8[4] = l;
				cam_common_cmd2card(insb8, sizeof(insb8), result, sizeof(result), &result_size);	// read class subs
				if ((result[result_size - 2] == 0x90) && (fshow) && (result[result_size - 1] == 0x00 || result[result_size - 1] == 0x08)) {
					cam_viaccess_show_class(NULL, result, result_size - 2);
					scls++;
				}
			}
		}

		insac[4] = 0;
		insa4[2] = 0x02;
		cam_common_cmd2card(insa4, sizeof(insa4), result, sizeof(result), &result_size);	// select next provider
	}

	return 1;
}

int cam_viaccess_process_ecm(ECM_REQUEST * er)
{
	static unsigned char insa4[] = { 0xca, 0xa4, 0x04, 0x00, 0x03 };	// set provider id
	static unsigned char ins88[] = { 0xca, 0x88, 0x00, 0x00, 0x00 };	// set ecm
	static unsigned char insf8[] = { 0xca, 0xf8, 0x00, 0x00, 0x00 };	// set geographic info 
	static unsigned char insc0[] = { 0xca, 0xc0, 0x00, 0x00, 0x12 };	// read dcw
	uchar result[260];
	ushort result_size;

	const uchar *ecm88Data = er->ecm + 4;	//XXX what is the 4th byte for ??
	int ecm88Len = SCT_LEN(er->ecm) - 4;
	ulong provid;
	int rc = 0;
	int hasD2 = 0;
	uchar DE04[256];

	if (ecm88Data[0] == 0xd2) {
		// FIXME: use the d2 arguments
		int len = ecm88Data[1] + 2;

		ecm88Data += len;
		ecm88Len -= len;
		hasD2 = 1;
	}

	if ((ecm88Data[0] == 0x90 || ecm88Data[0] == 0x40) && ecm88Data[1] == 0x03) {
		uchar ident[3], keynr;

		uchar *ecmf8Data = 0;
		int ecmf8Len = 0;

		memcpy(ident, &ecm88Data[2], sizeof (ident));
		provid = b2i(3, ident);
		ident[2] &= 0xF0;
		keynr = ecm88Data[4] & 0x0F;
		if (!cam_viaccess_chk_prov(ident, keynr)) {
			log_debug("smartcardviaccess ecm: provider or key not found on card");
			return 0;
		}
		ecm88Data += 5;
		ecm88Len -= 5;

		// DE04
		if (ecm88Data[0] == 0xDE && ecm88Data[1] == 0x04) {
			memcpy(DE04, &ecm88Data[0], 6);
			ecm88Data += 6;
		}
		//

		if (last_geo.provid != provid) {
			last_geo.provid = provid;
			last_geo.geo_len = 0;
			last_geo.geo[0] = 0;
			cam_viaccess_card_send_ins(insa4, ident, result, sizeof(result), &result_size);	// set provider
		}

		while (ecm88Len > 0 && ecm88Data[0] < 0xA0) {
			int nanoLen = ecm88Data[1] + 2;

			if (!ecmf8Data)
				ecmf8Data = (uchar *) ecm88Data;
			ecmf8Len += nanoLen;
			ecm88Len -= nanoLen;
			ecm88Data += nanoLen;
		}
		if (ecmf8Len) {
			if (last_geo.geo_len != ecmf8Len || memcmp(last_geo.geo, ecmf8Data, last_geo.geo_len)) {
				memcpy(last_geo.geo, ecmf8Data, ecmf8Len);
				last_geo.geo_len = ecmf8Len;
				insf8[3] = keynr;
				insf8[4] = ecmf8Len;
				cam_viaccess_card_send_ins(insf8, ecmf8Data, result, sizeof(result), &result_size);
			}
		}
		ins88[2] = ecmf8Len ? 1 : 0;
		ins88[3] = keynr;
		ins88[4] = ecm88Len;

		// DE04
		if (DE04[0] == 0xDE) {
			memcpy(DE04 + 6, (uchar *) ecm88Data, ecm88Len - 6);
			cam_viaccess_card_send_ins(ins88, DE04, result, sizeof(result), &result_size);	// request dcw
		} else {
			cam_viaccess_card_send_ins(ins88, (uchar *) ecm88Data, result, sizeof(result), &result_size);	// request dcw
		}
		//

		cam_common_cmd2card(insc0, sizeof(insc0), result, sizeof(result), &result_size);	// read dcw
		switch (result[0]) {
			case 0xe8:	// even
				if (result[1] == 8) {
					memcpy(er->cw, result + 2, 8);
					rc = 1;
				}
				break;
			case 0xe9:	// odd
				if (result[1] == 8) {
					memcpy(er->cw + 8, result + 2, 8);
					rc = 1;
				}
				break;
			case 0xea:	// complete
				if (result[1] == 16) {
					memcpy(er->cw, result + 2, 16);
					rc = 1;
				}
				break;
		}
	}

	if (hasD2) {
		AES_decrypt(er->cw, er->cw, &cam_viaccess_AES_key);
	}

	return rc;
}

int cam_viaccess_process_emm(EMM_PACKET * ep)
{
	static unsigned char insa4[] = { 0xca, 0xa4, 0x04, 0x00, 0x03 };		// set provider id
	static unsigned char insf0[] = { 0xca, 0xf0, 0x00, 0x01, 0x22 };		// set adf
	static unsigned char insf4[] = { 0xca, 0xf4, 0x00, 0x01, 0x00 };		// set adf, encrypted
	static unsigned char ins18[] = { 0xca, 0x18, 0x01, 0x01, 0x00 };		// set subscription
	static unsigned char ins1c[] = { 0xca, 0x1c, 0x01, 0x01, 0x00 };		// set subscription, encrypted
	static unsigned char insc8[] = { 0xca, 0xc8, 0x00, 0x00, 0x02, 0x00, 0x00 };	// read extended status

	uchar result[260];
	ushort result_size;

	int emmLen = SCT_LEN(ep->emm) - 7;
	int rc = 0;

	int emmUpToEnd;
	uchar *emmParsed = ep->emm + 7;
	int provider_ok = 0;
	uchar keynr = 0;
	int ins18Len = 0;
	uchar ins18Data[512];
	uchar insData[512];
	uchar *nano81Data = 0;
	uchar *nano91Data = 0;
	uchar *nano92Data = 0;
	uchar *nano9EData = 0;
	uchar *nanoF0Data = 0;

	for (emmUpToEnd = emmLen; (emmParsed[1] != 0) && (emmUpToEnd > 0); emmUpToEnd -= (2 + emmParsed[1]), emmParsed += (2 + emmParsed[1])) {
		///log_dump (emmParsed, emmParsed[1] + 2, "NANO");

		if (emmParsed[0] == 0x90 && emmParsed[1] == 0x03) {
			/* identification of the service operator */

			uchar soid[3], ident[3], i;

			for (i = 0; i < 3; i++) {
				soid[i] = ident[i] = emmParsed[2 + i];
			}
			ident[2] &= 0xF0;
			keynr = soid[2] & 0x0F;
			if (cam_viaccess_chk_prov(ident, keynr)) {
				provider_ok = 1;
			} else {
				log_debug("smartcardviaccess emm: provider or key not found on card (%x, %x)", ident, keynr);
				return 0;
			}

			// set provider
			cam_viaccess_card_send_ins(insa4, soid, result, sizeof(result), &result_size);
			if (result[result_size - 2] != 0x90 || result[result_size - 1] != 0x00) {
				log_dump(insa4, 5, "set provider cmd:");
				log_dump(soid, 3, "set provider data:");
				log_normal("update error: %02X %02X", result[result_size - 2], result[result_size - 1]);
				return 0;
			}
		} else if (emmParsed[0] == 0x9e && emmParsed[1] == 0x20) {
			/* adf */

			if (!nano91Data) {
				/* adf is not crypted, so test it */

				uchar custwp;
				uchar *afd;

				custwp = reader[ridx].sa[0][3];
				afd = (uchar *) emmParsed + 2;

				if (afd[31 - custwp / 8] & (1 << (custwp & 7)))
					log_debug("emm for our card %08X", b2i(4, &reader[ridx].sa[0][0]));
				else
					return 2;	// skipped
			}
			// memorize
			nano9EData = emmParsed;

		} else if (emmParsed[0] == 0x81) {
			nano81Data = emmParsed;
		} else if (emmParsed[0] == 0x91 && emmParsed[1] == 0x08) {
			nano91Data = emmParsed;
		} else if (emmParsed[0] == 0x92 && emmParsed[1] == 0x08) {
			nano92Data = emmParsed;
		} else if (emmParsed[0] == 0xF0 && emmParsed[1] == 0x08) {
			nanoF0Data = emmParsed;
		} else if (emmParsed[0] == 0x1D && emmParsed[0] == 0x01 && emmParsed[0] == 0x01) {
			/* from cccam... skip it... */
		} else {
			/* other nanos */
			cam_viaccess_show_subs(emmParsed);

			memcpy(ins18Data + ins18Len, emmParsed, emmParsed[1] + 2);
			ins18Len += emmParsed[1] + 2;
		}
	}

	if (!provider_ok) {
		log_debug("viaccess: provider not found in emm... continue anyway...");
		// force key to 1...
		keynr = 1;
	}

	if (!nanoF0Data) {
		log_dump(ep->emm, ep->l, "can't find 0xf0 in emm...");
		return 0;	// error
	}

	if (nano9EData) {
		if (!nano91Data) {
			// set adf
			insf0[3] = keynr;	// key
			cam_viaccess_card_send_ins(insf0, nano9EData, result, sizeof(result), &result_size);
			if (result[result_size - 2] != 0x90 || result[result_size - 1] != 0x00) {
				log_dump(insf0, 5, "set adf cmd:");
				log_dump(nano9EData, 0x22, "set adf data:");
				log_normal("update error: %02X %02X", result[result_size - 2], result[result_size - 1]);
				return 0;
			}
		} else {
			// set adf crypte
			insf4[3] = keynr;	// key
			insf4[4] = nano91Data[1] + 2 + nano9EData[1] + 2;
			memcpy(insData, nano91Data, nano91Data[1] + 2);
			memcpy(insData + nano91Data[1] + 2, nano9EData, nano9EData[1] + 2);
			cam_viaccess_card_send_ins(insf4, insData, result, sizeof(result), &result_size);
			if ((result[result_size - 2] != 0x90 && result[result_size - 2] != 0x91) || result[result_size - 1] != 0x00) {
				log_dump(insf4, 5, "set adf encrypted cmd:");
				log_dump(insData, insf4[4], "set adf encrypted data:");
				log_normal("update error: %02X %02X", result[result_size - 2], result[result_size - 1]);
				return 0;
			}
		}
	}

	if (!nano92Data) {
		// send subscription
		ins18[4] = ins18Len + nanoF0Data[1] + 2;
		memcpy(insData, ins18Data, ins18Len);
		memcpy(insData + ins18Len, nanoF0Data, nanoF0Data[1] + 2);
		cam_viaccess_card_send_ins(ins18, insData, result, sizeof(result), &result_size);
		if (result[result_size - 2] == 0x90 && result[result_size - 1] == 0x00) {
			log_debug("update successfully written");
			rc = 1;	// written
		} else {
			log_dump(ins18, 5, "set subscription cmd:");
			log_dump(insData, ins18[4], "set subscription data:");
			log_normal("update error: %02X %02X", result[result_size - 2], result[result_size - 1]);
		}

	} else {
		// send subscription encrypted

		if (!nano81Data) {
			log_dump(ep->emm, ep->l, "0x92 found, but can't find 0x81 in emm...");
			return 0;	// error
		}

		ins1c[3] = keynr;	// key
		ins1c[4] = nano92Data[1] + 2 + nano81Data[1] + 2 + nanoF0Data[1] + 2;
		memcpy(insData, nano92Data, nano92Data[1] + 2);
		memcpy(insData + nano92Data[1] + 2, nano81Data, nano81Data[1] + 2);
		memcpy(insData + nano92Data[1] + 2 + nano81Data[1] + 2, nanoF0Data, nanoF0Data[1] + 2);
		cam_viaccess_card_send_ins(ins1c, insData, result, sizeof(result), &result_size);
		if (result[result_size - 2] != 0x90 || result[result_size - 1] != 0x00) {
			/* maybe a 2nd level status, so read it */
			//log_dump(ins1c, 5, "set subscription encrypted cmd:");
			//log_dump(insData, ins1c[4], "set subscription encrypted data:");
			//log_normal("update error: %02X %02X", result[result_size-2], result[result_size-1]);

			cam_common_cmd2card(insc8, sizeof(insc8), result, sizeof(result), &result_size);
			if (result[0] != 0x00 || result[1] != 00 || result[result_size - 2] != 0x90 || result[result_size - 1] != 0x00) {
				//log_dump(result, result_size, "extended status error:");
				return 0;
			} else {
				log_debug("update successfully written (with extended status OK)");
				rc = 1;	// written
			}
		} else {
			log_debug("update successfully written");
			rc = 1;	// written
		}
	}

	memset(&last_geo, 0, sizeof (last_geo));

	/*
	   Sub Main()
	   Sc.Write("CA A4 04 00 03")
	   RX
	   Sc.Write("02 07 11")
	   RX
	   Sc.Write("CA F0 00 01 22")
	   RX
	   Sc.Write("9E 20")
	   Sc.Write("10 10 08 8A 80 00 04 00 10 10 26 E8 54 80 1E 80")
	   Sc.Write("00 01 00 00 00 00 00 50 00 00 80 02 22 00 08 50")
	   RX
	   Sc.Write("CA 18 01 01 11")
	   RX
	   Sc.Write("A9 05 34 DE 34 FF 80")
	   Sc.Write("F0 08 1A 3E AF B5 2B EE E3 3B")
	   RX

	   End Sub
	 */

	return rc;
}
