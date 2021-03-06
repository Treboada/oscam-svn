#include "globals.h"
#include "sharing/newcamd.h"

#include "oscam.h"
#include "card.h"
#include "chk.h"
#include "simples.h"
#include "log.h"
#include "network.h"

/* CSCRYPT */
#include "cscrypt.h"

#include <stdlib.h>
#include <time.h>
#include <unistd.h>

#define CWS_NETMSGSIZE 272

#define CWS_FIRSTCMDNO 0xe0

typedef enum {
	MSG_CLIENT_2_SERVER_LOGIN = CWS_FIRSTCMDNO,
	MSG_CLIENT_2_SERVER_LOGIN_ACK,
	MSG_CLIENT_2_SERVER_LOGIN_NAK,
	MSG_CARD_DATA_REQ,
	MSG_CARD_DATA,
	MSG_SERVER_2_CLIENT_NAME,
	MSG_SERVER_2_CLIENT_NAME_ACK,
	MSG_SERVER_2_CLIENT_NAME_NAK,
	MSG_SERVER_2_CLIENT_LOGIN,
	MSG_SERVER_2_CLIENT_LOGIN_ACK,
	MSG_SERVER_2_CLIENT_LOGIN_NAK,
	MSG_ADMIN,
	MSG_ADMIN_ACK,
	MSG_ADMIN_LOGIN,
	MSG_ADMIN_LOGIN_ACK,
	MSG_ADMIN_LOGIN_NAK,
	MSG_ADMIN_COMMAND,
	MSG_ADMIN_COMMAND_ACK,
	MSG_ADMIN_COMMAND_NAK,
	MSG_KEEPALIVE = CWS_FIRSTCMDNO + 0x1d
} net_msg_type_t;

typedef enum {
	COMMTYPE_CLIENT,
	COMMTYPE_SERVER
} comm_type_t;

#define REQ_SIZE	2
static uchar *req = 0;
static int ncd_proto = NCD_AUTO;

static int sharing_newcamd_message_send(int handle, ushort * netMsgId, uchar * buffer, int len, uchar * deskey, comm_type_t commType, ushort sid)
{
	uchar netbuf[CWS_NETMSGSIZE];
	int head_size;

	head_size = (ncd_proto == NCD_524) ? 8 : 12;

	if (len < 3 || len + head_size > CWS_NETMSGSIZE || handle < 0)
		return -1;
	buffer[1] = (buffer[1] & 0xf0) | (((len - 3) >> 8) & 0x0f);
	buffer[2] = (len - 3) & 0xff;
	memcpy(netbuf + head_size, buffer, len);
	len += head_size;
	if (netMsgId) {
		switch (commType) {
			case COMMTYPE_CLIENT:
				(*netMsgId)++;
				break;
			case COMMTYPE_SERVER:
				if (*netMsgId == 0xFFFE)
					*netMsgId = 0;	// ??? 0xFFFF ?
				break;
		}
		netbuf[2] = (*netMsgId) >> 8;
		netbuf[3] = (*netMsgId) & 0xff;
	} else
		netbuf[2] = netbuf[3] = 0;
	memset(netbuf + 4, 0, (ncd_proto == NCD_524) ? 4 : 8);
	if (sid) {
		netbuf[(ncd_proto == NCD_524) ? 6 : 4] = (uchar) (sid >> 8);	//sid
		netbuf[(ncd_proto == NCD_524) ? 7 : 5] = (uchar) (sid);
	}
	netbuf[0] = (len - 2) >> 8;
	netbuf[1] = (len - 2) & 0xff;
	log_ddump(netbuf, len, "send %d bytes to %s", len, (is_server ? "client" : "remote server"));
	if ((len = des_encrypt(netbuf, len, deskey)) < 0)
		return -1;
	netbuf[0] = (len - 2) >> 8;
	netbuf[1] = (len - 2) & 0xff;
	return send(handle, netbuf, len, 0);
}

static int sharing_newcamd_message_receive(int handle, ushort * netMsgId, uchar * buffer, uchar * deskey, comm_type_t commType)
{
	int len, ncd_off, msgid;
	uchar netbuf[CWS_NETMSGSIZE];
	int returnLen;

	if (!buffer || handle < 0)
		return -1;
	len = recv(handle, netbuf, 2, 0);
	log_debug("nmr(): len=%d, errno=%d", len, (len == -1) ? errno : 0);
	if (!len) {
		log_debug("nmr: 1 return 0");
		network_tcp_connection_close(handle);
		return 0;
	}
	if (len != 2) {
		log_debug("nmr: len!=2");
		network_tcp_connection_close(handle);
		return -1;
	}
	if (((netbuf[0] << 8) | netbuf[1]) > CWS_NETMSGSIZE - 2) {
		log_debug("nmr: 1 return -1");
		return -1;
	}

	len = recv(handle, netbuf + 2, (netbuf[0] << 8) | netbuf[1], 0);
	if (!len) {
		log_debug("nmr: 2 return 0");
		return 0;
	}
	if (len != ((netbuf[0] << 8) | netbuf[1])) {
		log_debug("nmr: 2 return -1");
		return -1;
	}
	len += 2;
	if ((len = des_decrypt(netbuf, len, deskey)) < 11) {	// 15(newcamd525) or 11 ???
		log_debug("nmr: can't decrypt, invalid des key?");
		sleep(2);
		return -1;
	}
	//log_ddump(netbuf, len, "nmr: decrypted data, len=%d", len);
	msgid = (netbuf[2] << 8) | netbuf[3];

	if (ncd_proto == NCD_AUTO) {
		// auto detect
		int l5 = (((netbuf[13] & 0x0f) << 8) | netbuf[14]) + 3;
		int l4 = (((netbuf[9] & 0x0f) << 8) | netbuf[10]) + 3;

		if ((l5 <= len - 12) && ((netbuf[12] & 0xF0) == 0xE0 || (netbuf[12] & 0xF0) == 0x80))
			ncd_proto = NCD_525;
		else if ((l4 <= len - 8) && ((netbuf[8] & 0xF0) == 0xE0 || (netbuf[9] & 0xF0) == 0x80))
			ncd_proto = NCD_524;
		else {
			log_debug("nmr: 4 return -1");
			return -1;
		}

		log_debug("nmr: autodetect: newcamd52%d used", (ncd_proto == NCD_525) ? 5 : 4);
	}

	ncd_off = (ncd_proto == NCD_525) ? 4 : 0;

	returnLen = (((netbuf[9 + ncd_off] & 0x0f) << 8) | netbuf[10 + ncd_off]) + 3;
	if (returnLen > (len - (8 + ncd_off))) {
		log_debug("nmr: 4 return -1");
		return -1;
	}
//	log_ddump(netbuf, len, "nmr: decrypted data");
	if (netMsgId) {
		switch (commType) {
			case COMMTYPE_SERVER:
				*netMsgId = msgid;
				break;

			case COMMTYPE_CLIENT:
				//if (*netMsgId != ((netbuf[2] << 8) | netbuf[3])) {
				log_debug("nmr: netMsgId=%d, from server=%d, ", *netMsgId, msgid);
				//return -2;
				//}
				break;

			default:
				log_debug("nmr: 5 return -1");
				return -1;
				break;
		}
	}
	switch (commType) {
		case COMMTYPE_SERVER:
			buffer[0] = (ncd_proto == NCD_525) ? netbuf[4] : netbuf[6];	// sid
			buffer[1] = (ncd_proto == NCD_525) ? netbuf[5] : netbuf[7];
			break;
		case COMMTYPE_CLIENT:
			buffer[0] = netbuf[2];	// msgid
			buffer[1] = netbuf[3];
			break;
	}

	memcpy(buffer + 2, netbuf + (8 + ncd_off), returnLen);
	return returnLen + 2;
}

static void sharing_newcamd_cmd_no_data_send(int handle, ushort * netMsgId, net_msg_type_t cmd, uchar * deskey, comm_type_t commType)
{
	uchar buffer[CWS_NETMSGSIZE];

	buffer[0] = cmd;
	buffer[1] = 0;
	sharing_newcamd_message_send(handle, netMsgId, buffer, 3, deskey, commType, 0);
}

static int sharing_newcamd_cmd_no_data_receive(int handle, ushort * netMsgId, uchar * deskey, comm_type_t commType)
{
	uchar buffer[CWS_NETMSGSIZE];

	if (sharing_newcamd_message_receive(handle, netMsgId, buffer, deskey, commType) != 3 + 2)
		return -1;
	return buffer[2];
}

static void sharing_newcamd_reply_ka()
{
	if (!client[cs_idx].udp_fd) {
		log_debug("invalid client fd=%d", client[cs_idx].udp_fd);
		return;
	}
	log_debug("send keep_alive");
	sharing_newcamd_cmd_no_data_send(client[cs_idx].udp_fd, &client[cs_idx].ncd_msgid, MSG_KEEPALIVE, client[cs_idx].ncd_skey, COMMTYPE_SERVER);
}

static int sharing_newcamd_connect_server()
{
	uint i;
	uchar buf[CWS_NETMSGSIZE];
	uchar keymod[14];
	uchar *key;
	int handle = 0;

	uint index;
	uchar *passwdcrypt;
	uchar login_answer;
	int bytes_received;

	if (reader[ridx].device[0] == 0 || reader[ridx].r_pwd[0] == 0 || reader[ridx].r_usr[0] == 0 || reader[ridx].r_port == 0)
		return -5;

	// 1. Connect
	//
	handle = network_tcp_connection_open(reader[ridx].device, reader[ridx].r_port);
	if (handle < 0)
		return -1;

	// 2. Get init sequence
	//
	reader[ridx].ncd_msgid = 0;
	if (read(handle, keymod, sizeof (keymod)) != sizeof (keymod)) {
		log_normal("server does not return 14 bytes");
		network_tcp_connection_close(handle);
		return -2;
	}
	log_ddump(keymod, 14, "server init sequence:");
	key = des_login_key_get(keymod, reader[ridx].ncd_key, 14);

	// 3. Send login info
	//
	index = 3;
	buf[0] = MSG_CLIENT_2_SERVER_LOGIN;
	buf[1] = 0;
	strcpy((char *) buf + index, reader[ridx].r_usr);
	passwdcrypt = (uchar *) __md5_crypt(reader[ridx].r_pwd, "$1$abcdefgh$");
	index += strlen(reader[ridx].r_usr) + 1;
	strcpy((char *) buf + index, (const char *) passwdcrypt);

	//log_debug("login to server %s:%d user=%s, pwd=%s, len=%d", reader[ridx].device,
	//          reader[ridx].r_port, reader[ridx].r_usr, reader[ridx].r_pwd,
	//          index+strlen(passwdcrypt)+1);
	sharing_newcamd_message_send(handle, 0, buf, index + strlen((char *) passwdcrypt) + 1, key, COMMTYPE_CLIENT, 0x8888);

	// 3.1 Get login answer
	//
	login_answer = sharing_newcamd_cmd_no_data_receive(handle, &reader[ridx].ncd_msgid, key, COMMTYPE_CLIENT);
	if (login_answer == MSG_CLIENT_2_SERVER_LOGIN_NAK) {
		log_normal("login failed for user '%s'", reader[ridx].r_usr);
		network_tcp_connection_close(handle);
		return -3;
	}
	if (login_answer != MSG_CLIENT_2_SERVER_LOGIN_ACK) {
		log_normal("expected MSG_CLIENT_2_SERVER_LOGIN_ACK (%02X), received %02X", MSG_CLIENT_2_SERVER_LOGIN_ACK, login_answer);
		network_tcp_connection_close(handle);
		return -3;
	}
	// 4. Send MSG_CARD_DATE_REQ
	//
	key = des_login_key_get(reader[ridx].ncd_key, passwdcrypt, strlen((char *) passwdcrypt));

	sharing_newcamd_cmd_no_data_send(handle, &reader[ridx].ncd_msgid, MSG_CARD_DATA_REQ, key, COMMTYPE_CLIENT);
	bytes_received = sharing_newcamd_message_receive(handle, &reader[ridx].ncd_msgid, buf, key, COMMTYPE_CLIENT);
	if (bytes_received < 16 || buf[2] != MSG_CARD_DATA) {
		log_normal("expected MSG_CARD_DATA (%02X), received %02X", MSG_CARD_DATA, buf[2]);
		network_tcp_connection_close(handle);
		return -4;
	}
	// 5. Parse CAID and PROVID(s)
	//
	reader[ridx].caid[0] = (ushort) ((buf[4 + 2] << 8) | buf[5 + 2]);
	memcpy(&reader[ridx].hexserial, buf + 6 + 2, 8);
	log_normal("Newcamd Server: %s:%d - UserID: %i", reader[ridx].device, reader[ridx].r_port, buf[3 + 2]);
	log_normal("CAID: %04X - UA: %02X%02X%02X%02X%02X%02X%02X%02X - Provider # %i", reader[ridx].caid[0], reader[ridx].hexserial[0], reader[ridx].hexserial[1], reader[ridx].hexserial[2], reader[ridx].hexserial[3], reader[ridx].hexserial[4], reader[ridx].hexserial[5], reader[ridx].hexserial[6],
	       reader[ridx].hexserial[7], buf[14 + 2]);
	reader[ridx].nprov = buf[14 + 2];
	memset(reader[ridx].prid, 0xff, sizeof (reader[ridx].prid));
	for (i = 0; i < reader[ridx].nprov; i++) {
		reader[ridx].availkeys[i][0] = 1;
		reader[ridx].prid[i][0] = buf[15 + 2 + 11 * i];
		reader[ridx].prid[i][1] = buf[16 + 2 + 11 * i];
		reader[ridx].prid[i][2] = buf[17 + 2 + 11 * i];
		memcpy(&reader[ridx].sa[i], buf + 22 + 2 + 11 * i, 4);	// the 4 first bytes are not read
		log_normal("Provider ID: %02X%02X%02X - SA: %02X%02X%02X%02X", reader[ridx].prid[i][1], reader[ridx].prid[i][2], reader[ridx].prid[i][3], reader[ridx].sa[i][0], reader[ridx].sa[i][1], reader[ridx].sa[i][2], reader[ridx].sa[i][3]);
	}
	memcpy(reader[ridx].ncd_skey, key, 16);

	// 6. Set connected info
	//
	reader[ridx].tcp_connected = 1;
	reader[ridx].last_g = reader[ridx].last_s = time((time_t *) 0);

//	log_normal("last_s=%d, last_g=%d", reader[ridx].last_s, reader[ridx].last_g);

	// !!! Only after connect() on client[cs_idx].udp_fd (Linux)
	pfd = client[cs_idx].udp_fd;

	return 0;
}

static int sharing_newcamd_connect()
{
	if (!reader[ridx].tcp_connected && sharing_newcamd_connect_server() < 0)
		return 0;
	if (!client[cs_idx].udp_fd)
		return 0;

	return 1;
}


static int sharing_newcamd_send(uchar * buf, int ml, ushort sid)
{
	if (!sharing_newcamd_connect())
		return (-1);

	//log_ddump(buf, ml, "send %d bytes to %s", ml, (is_server ? "client" : "remote server"));
	return (sharing_newcamd_message_send(client[cs_idx].udp_fd, &reader[ridx].ncd_msgid, buf, ml, reader[ridx].ncd_skey, COMMTYPE_CLIENT, sid));
}

static int sharing_newcamd_recv(uchar * buf, int l)
{
	int rc, rs;

	if (is_server) {
		rs = sharing_newcamd_message_receive(client[cs_idx].udp_fd, &client[cs_idx].ncd_msgid, buf, client[cs_idx].ncd_skey, COMMTYPE_SERVER);
	} else {
		if (!client[cs_idx].udp_fd)
			return (-1);
		rs = sharing_newcamd_message_receive(client[cs_idx].udp_fd, &reader[ridx].ncd_msgid, buf, reader[ridx].ncd_skey, COMMTYPE_CLIENT);
	}
	if (rs < 5) {
		rc = (-1);
	} else {
		rc = rs;
	}
	log_ddump(buf, rs, "received %d bytes from %s", rs, (is_server ? "client" : "remote server"));
	client[cs_idx].last = time((time_t *) 0);
	if (rc == -1) {
		if (rs > 0) {
			log_normal("packet to small (%d bytes)", rs);
		} else {
			log_normal("Connection closed to %s", (is_server ? "client" : "remote server"));
		}
	}

	return rc;
}

static unsigned int seed;
static uchar sharing_newcamd_fast_rnd()
{
	unsigned int offset = 12923;
	unsigned int multiplier = 4079;

	seed = seed * multiplier + offset;
	return (uchar) (seed % 0xFF);
}

static FILTER sharing_newcamd_mk_user_au_ftab(int au)
{
	int i, j, found;
	FILTER filt;
	FILTER *pufilt;

	filt.caid = reader[au].caid[0];
	if (filt.caid == 0)
		filt.caid = client[cs_idx].ftab.filts[0].caid;
	filt.nprids = 0;
	memset(&filt.prids, 0, sizeof (filt.prids));
	pufilt = &client[cs_idx].ftab.filts[0];

	for (i = 0; i < reader[au].nprov; i++)
		filt.prids[filt.nprids++] = b2i(3, &reader[au].prid[i][1]);

	for (i = 0; i < pufilt->nprids; i++) {
		for (j = found = 0; (!found) && (j < filt.nprids); j++)
			if (pufilt->prids[i] == filt.prids[j])
				found = 1;
		if (!found)
			filt.prids[filt.nprids++] = pufilt->prids[i];
	}

	return filt;
}

static FILTER sharing_newcamd_mk_user_ftab()
{
	FILTER *psfilt = 0;
	FILTER filt;
	int port_idx, i, j, k, c;

	filt.caid = 0;
	filt.nprids = 0;
	memset(&filt.prids, 0, sizeof (filt.prids));

	port_idx = client[cs_idx].port_idx;
	psfilt = &cfg->ncd_ptab.ports[port_idx].ftab.filts[0];

	// 1. CAID
	// search server CAID in client CAID
	for (c = i = 0; i < CS_MAXCAIDTAB; i++) {
		int ctab_caid;

		ctab_caid = client[cs_idx].ctab.caid[i] & client[cs_idx].ctab.mask[i];
		if (ctab_caid)
			c++;
		if (psfilt->caid == ctab_caid) {
			filt.caid = ctab_caid;
			break;
		}
	}
	if (c && !filt.caid) {
		log_normal("no valid CAID found in CAID for user '%s'", client[cs_idx].usr);
		return filt;
	}
	// search CAID in client IDENT
	log_debug("client[%d].%s nfilts=%d, filt.caid=%04X", cs_idx, client[cs_idx].usr, client[cs_idx].ftab.nfilts, filt.caid);

	if (!filt.caid && client[cs_idx].ftab.nfilts) {
		int fcaids;

		for (i = fcaids = 0; i < client[cs_idx].ftab.nfilts; i++) {
			ushort ucaid = client[cs_idx].ftab.filts[i].caid;

			if (ucaid)
				fcaids++;
			if (ucaid && psfilt->caid == ucaid) {
				filt.caid = ucaid;
				break;
			}
		}
		if (fcaids == client[cs_idx].ftab.nfilts && !filt.caid) {
			log_normal("no valid CAID found in IDENT for user '%s'", client[cs_idx].usr);
			//cs_disconnect_client();
			return filt;
		}
	}
	// empty client CAID - use server CAID
	if (!filt.caid)
		filt.caid = psfilt->caid;

	// 2. PROVID
	if (!client[cs_idx].ftab.nfilts) {
		int r, add;

		for (i = 0; i < psfilt->nprids; i++) {
			// use server PROVID(s) (and only those which are in user's groups)
			add = 0;
			for (r = 0; !add && r < CS_MAXREADER; r++) {
				if (reader[r].grp & client[cs_idx].grp) {
					if (!reader[r].ftab.nfilts) {
						if (reader[r].type & R_IS_NETWORK)
							add = 1;
						for (j = 0; !add && j < reader[r].nprov; j++)
							if (b2i(3, &reader[r].prid[j][1]) == psfilt->prids[i])
								add = 1;
					} else {
						for (j = 0; !add && j < reader[r].ftab.nfilts; j++) {
							ulong rcaid = reader[r].ftab.filts[j].caid;

							if (!rcaid || rcaid == filt.caid) {
								for (k = 0; !add && k < reader[r].ftab.filts[j].nprids; k++)
									if (reader[r].ftab.filts[j].prids[k] == psfilt->prids[i])
										add = 1;
							}
						}
					}
				}
			}
			if (add)
				filt.prids[filt.nprids++] = psfilt->prids[i];
		}
		return filt;
	}
	// search in client IDENT
	for (j = 0; j < client[cs_idx].ftab.nfilts; j++) {
		ulong ucaid = client[cs_idx].ftab.filts[j].caid;

		log_debug("client caid #%d: %04X", j, ucaid);
		if (!ucaid || ucaid == filt.caid) {
			for (i = 0; i < psfilt->nprids; i++) {
				log_debug("search server provid #%d: %06X", i, psfilt->prids[i]);
				if (client[cs_idx].ftab.filts[j].nprids) {
					for (k = 0; k < client[cs_idx].ftab.filts[j].nprids; k++)
						if (client[cs_idx].ftab.filts[j].prids[k] == psfilt->prids[i])
							filt.prids[filt.nprids++] = client[cs_idx].ftab.filts[j].prids[k];
				} else {
					filt.prids[filt.nprids++] = psfilt->prids[i];
					// allow server PROVID(s) if no PROVID(s) specified in IDENT
				}
			}
		}
	}

	if (!filt.nprids) {
		log_normal("no valid PROVID(s) found in CAID for user '%s'", client[cs_idx].usr);
		//cs_disconnect_client();
	}

	return filt;
}

static void sharing_newcamd_auth_client(in_addr_t ip)
{
	int i, ok = 0;
	uchar *usr = NULL, *pwd = NULL;
	struct s_auth *account;
	uchar buf[14];
	uchar *key = 0;
	uchar *passwdcrypt = NULL;
	int au = -1;

	// make random 14 bytes
	seed = (unsigned int) time((time_t *) 0);
	for (i = 0; i < 14; i++)
		buf[i] = sharing_newcamd_fast_rnd();

	// send init sequence
	send(client[cs_idx].udp_fd, buf, 14, 0);
	key = des_login_key_get(buf, cfg->ncd_key, 14);
	memcpy(client[cs_idx].ncd_skey, key, 16);
	client[cs_idx].ncd_msgid = 0;

	i = oscam_process_input(mbuf, sizeof (mbuf), cfg->cmaxidle);
	if (i > 0) {
		if (mbuf[2] != MSG_CLIENT_2_SERVER_LOGIN) {
			log_debug("expected MSG_CLIENT_2_SERVER_LOGIN (%02X), received %02X", MSG_CLIENT_2_SERVER_LOGIN, mbuf[2]);
			if (req) {
				free(req);
				req = 0;
			}
			oscam_exit(0);
		}
		usr = mbuf + 5;
		pwd = usr + strlen((char *) usr) + 1;
		//log_debug("usr=%s,pwd=%s", usr, pwd);
	} else {
		log_debug("bad client login request");
		if (req) {
			free(req);
			req = 0;
		}
		oscam_exit(0);
	}

	for (ok = 0, account = cfg->account; (usr) && (account) && (!ok); account = account->next) {
		log_debug("account->usr=%s", account->usr);
		if (strcmp((char *) usr, account->usr) == 0) {
			passwdcrypt = (uchar *) __md5_crypt(account->pwd, "$1$abcdefgh$");
			log_debug("account->pwd=%s", passwdcrypt);
			if (strcmp((char *) pwd, (const char *) passwdcrypt) == 0) {
				int auth_reject;
				client[cs_idx].crypted = 1;
				auth_reject = oscam_auth_client(account, NULL);
				if (!auth_reject) {
					log_normal("user %s authenticated successfully (using client %02X%02X)", usr, mbuf[0], mbuf[1]);
					ok = 1;
				} else {
					log_normal("user %s did not authenticate (using client %02X%02X) (error: %d)", usr, mbuf[0], mbuf[1], auth_reject);
				}

				break;
			} else {
				log_normal("user %s is providing a wrong password (using client %02X%02X)", usr, mbuf[0], mbuf[1]);
			}
		}
	}

	if (!ok && !account) {
		log_normal("user %s is trying to connect but doesnt exist ! (using client %02X%02X)", usr, mbuf[0], mbuf[1]);
		usr = 0;
	}

	if (ok) {
		au = client[cs_idx].au;
		if (au != -1) {
			if (cfg->ncd_ptab.ports[client[cs_idx].port_idx].ftab.filts[0].caid != reader[au].caid[0]
			    && cfg->ncd_ptab.ports[client[cs_idx].port_idx].ftab.filts[0].caid != reader[au].ftab.filts[0].caid) {
				log_normal("AU wont be used on this port -> disable AU");
				au = -1;
			} else if (reader[au].card_system <= 0 && !(reader[au].type & R_IS_CASCADING)) {
				// Init for AU enabled card not finished, reject Client
				ok = 0;
				au = -2;	// Flag zur Logausgabe
			} else {
				log_normal("AU flag %d for user %s", au, usr);
			}
		} else {
			log_normal("AU disabled for user %s", usr);
		}
	}

	sharing_newcamd_cmd_no_data_send(client[cs_idx].udp_fd, &client[cs_idx].ncd_msgid, (ok) ? MSG_CLIENT_2_SERVER_LOGIN_ACK : MSG_CLIENT_2_SERVER_LOGIN_NAK, client[cs_idx].ncd_skey, COMMTYPE_SERVER);

	// we need to add a test to make sure all card reader are ready before allowing more interaction with the user.
	// cfg->ncd_ptab.ports[client[cs_idx].port_idx].ftab.filts[0].caid old the CAID for this port
	// we need to check all ready for a match and check if they are ready
	// if they arent ready, disconnect the user.
	// it will reconnect in a few second.

	if (ok) {
		FILTER pufilt_noau = { 0, 0, { 0 } };
		FILTER *pufilt = 0;

		key = des_login_key_get(cfg->ncd_key, passwdcrypt, strlen((char *) passwdcrypt));
		memcpy(client[cs_idx].ncd_skey, key, 16);

		i = oscam_process_input(mbuf, sizeof (mbuf), cfg->cmaxidle);
		if (i > 0) {
			int j, len = 15;

			if (mbuf[2] != MSG_CARD_DATA_REQ) {
				log_debug("expected MSG_CARD_DATA_REQ (%02X), received %02X", MSG_CARD_DATA_REQ, mbuf[2]);
				if (req) {
					free(req);
					req = 0;
				}
				oscam_exit(0);
			}

			client[cs_idx].ftab.filts[0] = sharing_newcamd_mk_user_ftab();
			pufilt = &client[cs_idx].ftab.filts[0];

			if (au != -1) {
				unsigned char equal = 1;

				// remember user filter
				memcpy(&pufilt_noau, pufilt, sizeof (FILTER));

				client[cs_idx].ftab.filts[0] = sharing_newcamd_mk_user_au_ftab(au);
				pufilt = &client[cs_idx].ftab.filts[0];

				// check if user filter CAID and PROVID is the same as CAID and PROVID of the AU reader

				if ((pufilt->caid != pufilt_noau.caid)) {
					// log_normal("CAID server: %04X, CAID card: %04X, not equal, AU disabled",pufilt_noau.caid,pufilt->caid);
					// equal = 0;
				}

				for (j = 0; equal && j < pufilt_noau.nprids && j < pufilt->nprids; j++) {
					if (pufilt->prids[j] != pufilt_noau.prids[j]) {
						// log_normal("PROVID%d server: %04X, PROVID%d card: %04X, not equal, AU disabled",j,pufilt_noau.prids[j],j,pufilt->prids[j]);
						//weird// equal = 0;
					}
				}

				if (!equal) {
					// Not equal -> AU must set to disabled -> set back to user filter
					memcpy(pufilt, &pufilt_noau, sizeof (FILTER));
					au = -1;
				}
			}

			client[cs_idx].ftab.nfilts = 1;
			mbuf[0] = MSG_CARD_DATA;
			mbuf[1] = 0x00;
			mbuf[2] = 0x00;

			// AU always set to true because some clients cannot handle "non-AU"
			// For security reason don't send the real hexserial (see below)
			// if a non-AU-client sends an EMM-request it will be thrown away
			// (see function "sharing_newcamd_process_emm")
			//mbuf[3] = 1;
			if (au != -1)
				mbuf[3] = 1;
			else
				mbuf[3] = cs_idx + 10;	// Unique user number

			mbuf[4] = (uchar) (pufilt->caid >> 8);
			mbuf[5] = (uchar) (pufilt->caid);
			mbuf[6] = 0x00;
			mbuf[7] = 0x00;

			if (au != -1) {
				// TODO: cleanup this code... We should not have to check the caid here...
				if (((pufilt->caid >> 8) == 0x17) || ((pufilt->caid >> 8) == 0x06))	// Betacrypt or Irdeto
				{
					// only 4 Bytes Hexserial for newcamd clients (Hex Base + Hex Serial)
					// first 2 Byte always 00
					mbuf[8] = 0x00;	//serial only 4 bytes
					mbuf[9] = 0x00;	//serial only 4 bytes
					// 1 Byte Hex Base (se irdeto.c how this is stored in "reader[au].hexserial")
					mbuf[10] = reader[au].hexserial[3];
					// 3 Bytes Hex Serial (see irdeto.c how this is stored in "reader[au].hexserial")
					mbuf[11] = reader[au].hexserial[0];
					mbuf[12] = reader[au].hexserial[1];
					mbuf[13] = reader[au].hexserial[2];
				} else if (((pufilt->caid >> 8) == 0x05) || ((pufilt->caid >> 8) == 0x0D)) {
					mbuf[8] = 0x00;
					mbuf[9] = reader[au].hexserial[0];
					mbuf[10] = reader[au].hexserial[1];
					mbuf[11] = reader[au].hexserial[2];
					mbuf[12] = reader[au].hexserial[3];
					mbuf[13] = reader[au].hexserial[4];
				} else if ((pufilt->caid >> 8) == 0x09) {
					mbuf[8] = 0x00;
					mbuf[9] = 0x00;
					mbuf[10] = reader[au].hexserial[4];
					mbuf[11] = reader[au].hexserial[5];
					mbuf[12] = reader[au].hexserial[6];
					mbuf[13] = reader[au].hexserial[7];
				} else {
					mbuf[8] = reader[au].hexserial[0];
					mbuf[9] = reader[au].hexserial[1];
					mbuf[10] = reader[au].hexserial[2];
					mbuf[11] = reader[au].hexserial[3];
					mbuf[12] = reader[au].hexserial[4];
					mbuf[13] = reader[au].hexserial[5];
				}
			} else {
				client[cs_idx].au = -1;
				mbuf[8] = 0x00;
				mbuf[9] = 0x00;
				mbuf[10] = 0x00;
				mbuf[11] = 0x00;
				mbuf[12] = 0x00;
				mbuf[13] = 0x00;
				// send "faked" Hexserial to client
				/*
				   if (((pufilt->caid >= 0x1700) && (pufilt->caid <= 0x1799))  || // Betacrypt
				   ((pufilt->caid >= 0x0600) && (pufilt->caid <= 0x0699)))    // Irdeto 
				   {
				   mbuf[6] = 0x00;
				   mbuf[7] = 0x00;
				   mbuf[8] = 0x00;
				   mbuf[9] = 0x00;
				   mbuf[10] = sharing_newcamd_fast_rnd();
				   mbuf[11] = sharing_newcamd_fast_rnd();
				   mbuf[12] = sharing_newcamd_fast_rnd();
				   mbuf[13] = sharing_newcamd_fast_rnd();
				   }
				   else
				   {
				   mbuf[6] = sharing_newcamd_fast_rnd();
				   mbuf[7] = sharing_newcamd_fast_rnd();
				   mbuf[8] = sharing_newcamd_fast_rnd();
				   mbuf[9] = sharing_newcamd_fast_rnd();
				   mbuf[10] = sharing_newcamd_fast_rnd();
				   mbuf[11] = sharing_newcamd_fast_rnd();
				   mbuf[12] = sharing_newcamd_fast_rnd();
				   mbuf[13] = sharing_newcamd_fast_rnd();
				   }
				 */
			}

			mbuf[14] = pufilt->nprids;
			for (j = 0; j < pufilt->nprids; j++) {
				if ((pufilt->caid >= 0x0600) && (pufilt->caid <= 0x0699))	// Irdeto
				{
					mbuf[15 + 11 * j] = 0;
					mbuf[16 + 11 * j] = 0;
					mbuf[17 + 11 * j] = j;
				} else {
					mbuf[15 + 11 * j] = (uchar) (pufilt->prids[j] >> 16);
					mbuf[16 + 11 * j] = (uchar) (pufilt->prids[j] >> 8);
					mbuf[17 + 11 * j] = (uchar) (pufilt->prids[j]);
				}
				mbuf[18 + 11 * j] = 0x00;
				mbuf[19 + 11 * j] = 0x00;
				mbuf[20 + 11 * j] = 0x00;
				mbuf[21 + 11 * j] = 0x00;
				if (au != -1) {	// check if user provid from IDENT exists on card
					int k, found;
					ulong rprid;

					found = 0;
					if (pufilt->caid == reader[au].caid[0]) {
						for (k = 0; (k < reader[au].nprov); k++) {
							rprid = b2i(3, &reader[au].prid[k][1]);
							if (rprid == pufilt->prids[j]) {
								if ((pufilt->caid >= 0x0600) && (pufilt->caid <= 0x0699))	// Irdeto
								{
									mbuf[22 + 11 * j] = reader[au].prid[k][0];
									mbuf[23 + 11 * j] = reader[au].prid[k][1];
									mbuf[24 + 11 * j] = reader[au].prid[k][2];
									mbuf[25 + 11 * j] = reader[au].prid[k][3];
								} else {
									mbuf[22 + 11 * j] = reader[au].sa[k][0];
									mbuf[23 + 11 * j] = reader[au].sa[k][1];
									mbuf[24 + 11 * j] = reader[au].sa[k][2];
									mbuf[25 + 11 * j] = reader[au].sa[k][3];
								}
								found = 1;
								break;
							}
						}
					}
					if (!found) {
						mbuf[22 + 11 * j] = 0x00;
						mbuf[23 + 11 * j] = 0x00;
						mbuf[24 + 11 * j] = 0x00;
						mbuf[25 + 11 * j] = 0x00;
					}
				} else {
					if ((pufilt->caid >= 0x0600) && (pufilt->caid <= 0x0699))	// Irdeto
					{
						mbuf[22 + 11 * j] = 0x00;
						mbuf[23 + 11 * j] = (uchar) (pufilt->prids[j] >> 16);
						mbuf[24 + 11 * j] = (uchar) (pufilt->prids[j] >> 8);
						mbuf[25 + 11 * j] = (uchar) (pufilt->prids[j]);
					} else {
						mbuf[22 + 11 * j] = 0x00;
						mbuf[23 + 11 * j] = 0x00;
						mbuf[24 + 11 * j] = 0x00;
						mbuf[25 + 11 * j] = 0x00;
					}
				}
				len += 11;
			}

			if (sharing_newcamd_message_send(client[cs_idx].udp_fd, &client[cs_idx].ncd_msgid, mbuf, len, key, COMMTYPE_SERVER, 0) < 0) {
				if (req) {
					free(req);
					req = 0;
				}
				oscam_exit(0);
			}
		}
	} else {
		if (au == -2)
			oscam_auth_client(0, "Init for AU enabled card not finished");
		else
			oscam_auth_client(0, usr ? "login failure" : "no such user");
		if (req) {
			free(req);
			req = 0;
		}
		oscam_exit(0);
	}
}

static void sharing_newcamd_send_dcw(ECM_REQUEST * er)
{
	int len;
	ushort cl_msgid;

	if (!client[cs_idx].udp_fd) {
		log_debug("ncd_send_dcw: error: client[cs_idx].udp_fd=%d", client[cs_idx].udp_fd);
		return;
	}
	memcpy(&cl_msgid, req + (er->cpti * REQ_SIZE), 2);	// get client ncd_msgid + 0x8x
	mbuf[0] = er->ecm[0];
	if (client[cs_idx].ftab.filts[0].nprids == 0 || er->rc > 3 /*not found */ ) {
		len = 3;
		mbuf[1] = mbuf[2] = 0x00;
	} else {
		len = 19;
		mbuf[1] = mbuf[2] = 0x10;
		memcpy(mbuf + 3, er->cw, 16);
	}

	log_debug("ncd_send_dcw: er->cpti=%d, cl_msgid=%d, %02X", er->cpti, cl_msgid, mbuf[0]);

	sharing_newcamd_message_send(client[cs_idx].udp_fd, &cl_msgid, mbuf, len, client[cs_idx].ncd_skey, COMMTYPE_SERVER, 0);
}

static void sharing_newcamd_process_ecm(uchar * buf, int l)
{
	int pi;
	ECM_REQUEST *er;

	if (!(er = oscam_get_ecmtask())) {
		return;
	}
	// save client ncd_msgid
	memcpy(req + (er->cpti * REQ_SIZE), &client[cs_idx].ncd_msgid, 2);
	log_debug("ncd_process_ecm: er->cpti=%d, cl_msgid=%d, %02X", er->cpti, client[cs_idx].ncd_msgid, buf[2]);
	er->l = buf[4] + 3;
	er->srvid = (buf[0] << 8) | buf[1];
	er->caid = 0;
	pi = client[cs_idx].port_idx;
	if (cfg->ncd_ptab.nports && cfg->ncd_ptab.nports >= pi)
		er->caid = cfg->ncd_ptab.ports[pi].ftab.filts[0].caid;
	memcpy(er->ecm, buf + 2, er->l);
	oscam_process_ecm(er);
}

static void sharing_newcamd_process_emm(uchar * buf, int l)
{
	int au, ok = 1;
	ushort caid;

	memset(&epg, 0, sizeof (epg));
	au = client[cs_idx].au;

	// if client is not allowed to do AU just send back the OK-answer to
	// the client and do nothing else with the received data
	if ((au >= 0) && (au <= CS_MAXREADER)) {
		epg.l = buf[2] + 3;
		caid = client[cs_idx].ftab.filts[0].caid;
		epg.caid[0] = (uchar) (caid >> 8);
		epg.caid[1] = (uchar) (caid);

/*  if (caid == 0x0500)
  {
    ushort emm_head;

    emm_head = (buf[0]<<8) | buf[1];
    switch( emm_head )
    {
      case 0x8e70:  // EMM-S
        memcpy(epg.hexserial+1, buf+3, 4);
        epg.hexserial[4]=reader[au].hexserial[4];
        break;
      case 0x8870:  // EMM-U
      case 0x8c70:  // confidential ?
      default:
        log_normal("unsupported emm type: %04X", emm_head);
        ok=0;
    }
    if( !ok ) log_normal("only EMM-S supported");
  }
  else*/
		memcpy(epg.hexserial, reader[au].hexserial, 8);	// dummy

		memcpy(epg.emm, buf, epg.l);
		if (ok) {
			oscam_process_emm(&epg);
		}
	}
	// Should always send an answer to client (also if au is disabled),
	// some clients will disconnect if they get no answer
	buf[1] = 0x10;
	buf[2] = 0x00;
	sharing_newcamd_message_send(client[cs_idx].udp_fd, &client[cs_idx].ncd_msgid, buf, 3, client[cs_idx].ncd_skey, COMMTYPE_SERVER, 0);
}

static void sharing_newcamd_server()
{
	int n;

	req = (uchar *) malloc(CS_MAXPENDING * REQ_SIZE);
	if (!req) {
		log_normal("Cannot allocate memory (errno=%d)", errno);
		oscam_exit(1);
	}
	memset(req, 0, CS_MAXPENDING * REQ_SIZE);

	client[cs_idx].ncd_server = 1;
	log_debug("client connected to %d port", cfg->ncd_ptab.ports[client[cs_idx].port_idx].s_port);
	sharing_newcamd_auth_client(client[cs_idx].ip);

	n = -9;
	while (n == -9) {
		while ((n = oscam_process_input(mbuf, sizeof (mbuf), cfg->cmaxidle)) > 0) {
			switch (mbuf[2]) {
				case 0x80:
				case 0x81:
					sharing_newcamd_process_ecm(mbuf, n);
					break;
				case MSG_KEEPALIVE:
					sharing_newcamd_reply_ka();
					break;
				default:
					if (mbuf[2] > 0x81 && mbuf[2] < 0x90)
						sharing_newcamd_process_emm(mbuf + 2, n - 2);
					else
						log_debug("unknown command !");
			}
		}
		if (n == -9) {
			sharing_newcamd_reply_ka();
		}
	}

	if (req) {
		free(req);
		req = 0;
	}

	oscam_disconnect_client();
}

/*
*	client functions
*/

static int sharing_newcamd_client_init()
{
	static struct sockaddr_in loc_sa;
	struct protoent *ptrp;
	int p_proto;
	char ptxt[16];

	pfd = 0;
	if (reader[ridx].r_port <= 0) {
		log_normal("invalid port %d for server %s", reader[ridx].r_port, reader[ridx].device);
		return (1);
	}
	if ((ptrp = getprotobyname("tcp")))
		p_proto = ptrp->p_proto;
	else
		p_proto = 6;

	client[cs_idx].ip = 0;
	memset((char *) &loc_sa, 0, sizeof (loc_sa));
	loc_sa.sin_family = AF_INET;
	if (cfg->serverip[0])
		loc_sa.sin_addr.s_addr = network_inet_addr(cfg->serverip);
	else
		loc_sa.sin_addr.s_addr = INADDR_ANY;
	loc_sa.sin_port = htons(reader[ridx].l_port);

	if ((client[cs_idx].udp_fd = socket(PF_INET, SOCK_STREAM, p_proto)) < 0) {
		log_normal("Socket creation failed (errno=%d)", errno);
		oscam_exit(1);
	}
#ifdef SO_PRIORITY
	if (cfg->netprio)
		setsockopt(client[cs_idx].udp_fd, SOL_SOCKET, SO_PRIORITY, (void *) &cfg->netprio, sizeof (ulong));
#endif
	if (!reader[ridx].tcp_ito) {
		ulong keep_alive = reader[ridx].tcp_ito ? 1 : 0;

		setsockopt(client[cs_idx].udp_fd, SOL_SOCKET, SO_KEEPALIVE, (void *) &keep_alive, sizeof (ulong));
	}

	if (reader[ridx].l_port > 0) {
		if (bind(client[cs_idx].udp_fd, (struct sockaddr *) &loc_sa, sizeof (loc_sa)) < 0) {
			log_normal("bind failed (errno=%d)", errno);
			close(client[cs_idx].udp_fd);
			return (1);
		}
		sprintf(ptxt, ", port=%d", reader[ridx].l_port);
	} else
		ptxt[0] = '\0';

	memset((char *) &client[cs_idx].udp_sa, 0, sizeof (client[cs_idx].udp_sa));
	client[cs_idx].udp_sa.sin_family = AF_INET;
	client[cs_idx].udp_sa.sin_port = htons((u_short) reader[ridx].r_port);

	ncd_proto = reader[ridx].ncd_proto;

	log_normal("proxy %s:%d newcamd52%d (fd=%d%s)", reader[ridx].device, reader[ridx].r_port, (ncd_proto == NCD_525) ? 5 : 4, client[cs_idx].udp_fd, ptxt);
	//pfd=client[cs_idx].udp_fd; // !!! we set it after connect() (linux)
	return (0);
}

static int sharing_newcamd_send_ecm(ECM_REQUEST * er, uchar * buf)
{
	//int rc=(-1);
	if (!client[cs_idx].udp_sa.sin_addr.s_addr)	// once resolved at least
		return (-1);

	// check server filters
	if (!sharing_newcamd_connect())
		return (-1);

	if (!chk_rsfilter(er, reader[ridx].ncd_disable_server_filt))
		return (-1);

	memcpy(buf, er->ecm, er->l);

	return ((sharing_newcamd_send(buf, er->l, er->srvid) < 1) ? (-1) : 0);
}

static int sharing_newcamd_recv_chk(uchar * dcw, int *rc, uchar * buf, int n)
{
	ushort idx;

	if (n < 21)	// no cw, ignore others
		return (-1);
	*rc = 1;
	idx = (buf[0] << 8) | buf[1];
	memcpy(dcw, buf + 5, 16);
	return (idx);
}

void sharing_newcamd_module(struct s_module *ph)
{
	strcpy(ph->desc, "newcamd");
	ph->type = MOD_CONN_TCP;
	ph->logtxt = ", crypted";
	ph->multi = 1;
	ph->watchdog = 1;
	ph->s_ip = cfg->ncd_srvip;
	ph->s_handler = sharing_newcamd_server;
	ph->recv = sharing_newcamd_recv;
	ph->send_dcw = sharing_newcamd_send_dcw;
	ph->ptab = &cfg->ncd_ptab;
	if (ph->ptab->nports == 0)
		ph->ptab->nports = 1;	// show disabled in log
	ph->c_multi = 1;
	ph->c_init = sharing_newcamd_client_init;
	ph->c_recv_chk = sharing_newcamd_recv_chk;
	ph->c_send_ecm = sharing_newcamd_send_ecm;

}
