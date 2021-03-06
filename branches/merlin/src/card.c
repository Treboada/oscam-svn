#include "globals.h"
#include "card.h"

#include "reader/common.h"

#include "sharing/camd33.h"
#include "sharing/camd35.h"
#include "sharing/newcamd.h"
#include "sharing/radegast.h"
#include "sharing/serial.h"

#include "cache.h"
#include "oscam.h"
#include "simples.h"
#include "log.h"
#include "network.h"

#include <stdlib.h>
#include <time.h>
#include <unistd.h>

int ridx = 0, logfd = 0;

static int proxy;
static struct s_emm *emmcache;
static int last_idx = 1;
static ushort idx = 1;

static void card_casc_check_dcw(int idx, int rc, uchar * cw)
{
	int i;

	for (i = 1; i < CS_MAXPENDING; i++) {
		if ((ecmtask[i].rc >= 10) && (!memcmp(ecmtask[i].ecmd5, ecmtask[idx].ecmd5, CS_ECMSTORESIZE))) {
			if (rc) {
				ecmtask[i].rc = (i == idx) ? 1 : 2;
				if (ecmtask[i].gbxRidx)
					ecmtask[i].rc = 0;
				memcpy(ecmtask[i].cw, cw, 16);
			} else
				ecmtask[i].rc = 0;
			oscam_write_ecm_answer(fd_c2m, &ecmtask[i]);
			ecmtask[i].idx = 0;
		}
	}
}

static int card_casc_recv_timer(uchar * buf, int l, int msec)
{
	struct timeval tv;
	fd_set fds;
	int rc;

	if (!pfd)
		return (-1);
	tv.tv_sec = msec / 1000;
	tv.tv_usec = (msec % 1000) * 1000;
	FD_ZERO(&fds);
	FD_SET(pfd, &fds);
	select(pfd + 1, &fds, 0, 0, &tv);
	rc = 0;
	if (FD_ISSET(pfd, &fds))
		if (!(rc = reader[ridx].ph.recv(buf, l)))
			rc = -1;

	return rc;
}

static void card_casc_do_sock_log()
{
	int i, idx;
	ushort caid, srvid;
	ulong provid;

	idx = reader[ridx].ph.c_recv_log(&caid, &provid, &srvid);
	client[cs_idx].last = time((time_t) 0);
	if (idx < 0)
		return;	// no dcw-msg received

	for (i = 1; i < CS_MAXPENDING; i++) {
		if ((ecmtask[i].rc >= 10)
		    && (ecmtask[i].idx == idx)
		    && (ecmtask[i].caid == caid)
		    && (ecmtask[i].prid == provid)
		    && (ecmtask[i].srvid == srvid)) {
			card_casc_check_dcw(i, 0, ecmtask[i].cw);	// send "not found"
			break;
		}
	}
}

static void card_casc_do_sock(int w)
{
	int i, n, idx, rc, j;
	uchar buf[1024];
	uchar dcw[16];

	if ((n = card_casc_recv_timer(buf, sizeof (buf), w)) <= 0) {
		if (reader[ridx].ph.type == MOD_CONN_TCP) {
			log_debug("card_casc_do_sock: close connection");
			network_tcp_connection_close(client[cs_idx].udp_fd);
		}
		return;
	}
	client[cs_idx].last = time((time_t) 0);
	idx = reader[ridx].ph.c_recv_chk(dcw, &rc, buf, n);
	if (idx < 0)
		return;	// no dcw received
	reader[ridx].last_g = time((time_t *) 0);	// for reconnect timeout
//	log_normal("card_casc_do_sock: last_s=%d, last_g=%d", reader[ridx].last_s, reader[ridx].last_g);
	if (!idx)
		idx = last_idx;
	j = 0;
	for (i = 1; i < CS_MAXPENDING; i++) {

		if (ecmtask[i].idx == idx) {
			card_casc_check_dcw(i, rc, dcw);
			j = 1;
			break;
		}
	}
}

static void card_casc_get_dcw(int n)
{
	int w;
	struct timeb tps, tpe;

	tpe = ecmtask[n].tps;
	tpe.millitm += cfg->srtimeout;
	tpe.time += (tpe.millitm / 1000);
	tpe.millitm %= 1000;

	cs_ftime(&tps);
	while (((w = 1000 * (tpe.time - tps.time) + tpe.millitm - tps.millitm) > 0)
	       && (ecmtask[n].rc >= 10)) {
		card_casc_do_sock(w);
		cs_ftime(&tps);
	}
	if (ecmtask[n].rc >= 10)
		card_casc_check_dcw(n, 0, ecmtask[n].cw);	// simulate "not found"
}

static int card_casc_process_ecm(ECM_REQUEST * er)
{
	int rc, n, i, sflag;
	time_t t;

	uchar buf[512];

	t = time((time_t *) 0);
	for (n = 0, i = sflag = 1; i < CS_MAXPENDING; i++) {
		if ((t - ecmtask[i].tps.time > ((cfg->ctimeout + 500) / 1000) + 1) && (ecmtask[i].rc >= 10))	// drop timeouts
		{
			ecmtask[i].rc = 0;
		}
		if ((!n) && (ecmtask[i].rc < 10))	// free slot found
			n = i;
		if ((ecmtask[i].rc >= 10) &&	// ecm already pending
		    (!memcmp(er->ecmd5, ecmtask[i].ecmd5, CS_ECMSTORESIZE)) && (er->level <= ecmtask[i].level))	// ... this level at least
			sflag = 0;
	}
	if (!n) {
		log_normal("WARNING: ecm pending table overflow !!");
		return (-2);
	}
	memcpy(&ecmtask[n], er, sizeof (ECM_REQUEST));
	if (reader[ridx].type == R_NEWCAMD)
		ecmtask[n].idx = (reader[ridx].ncd_msgid == 0) ? 2 : reader[ridx].ncd_msgid + 1;
	else
		ecmtask[n].idx = idx++;
	ecmtask[n].rc = 10;
	log_debug("---- ecm_task %d, idx %d, sflag=%d, level=%d", n, ecmtask[n].idx, sflag, er->level);

	if (reader[ridx].ph.type == MOD_CONN_TCP && reader[ridx].tcp_rto) {
		int rto = abs(reader[ridx].last_s - reader[ridx].last_g);

		if (rto >= reader[ridx].tcp_rto) {
			log_debug("rto=%d", rto);
			network_tcp_connection_close(client[cs_idx].udp_fd);
		}
	}

	if (cfg->show_ecm_dw && !client[cs_idx].dbglvl)
		log_dump(er->ecm, er->l, 0);
	rc = 0;
	if (sflag) {
		if (!client[cs_idx].udp_sa.sin_addr.s_addr)	// once resolved at least
			oscam_resolve();

		if ((rc = reader[ridx].ph.c_send_ecm(&ecmtask[n], buf)))
			card_casc_check_dcw(n, 0, ecmtask[n].cw);	// simulate "not found"
		else
			last_idx = ecmtask[n].idx;
		reader[ridx].last_s = t;	// used for inactive_timeout and reconnect_timeout in TCP reader

		if (!reader[ridx].ph.c_multi)
			card_casc_get_dcw(n);
	}
//	log_normal("card_casc_process_ecm 1: last_s=%d, last_g=%d", reader[ridx].last_s, reader[ridx].last_g);

	if (idx > 0x1ffe)
		idx = 1;

	return rc;
}

static int card_store_emm(uchar * emm, uchar type)
{
	static int rotate = 0;
	int rc;

	memcpy(emmcache[rotate].emm, emm, emm[2]);
	emmcache[rotate].type = type;
	emmcache[rotate].count = 1;
//	log_debug("EMM stored (index %d)", rotate);
	rc = rotate;
	rotate = (rotate + 1) % CS_EMMCACHESIZE;

	return rc;
}

static void card_get_ecm(ECM_REQUEST * er)
{
	if ((er->rc < 10)) {
		oscam_send_dcw(er);
		return;
	}
	er->ocaid = er->caid;
	if (!oscam_chk_bcaid(er, &reader[ridx].ctab)) {
		log_debug("caid %04X filtered", er->caid);
		er->rcEx = E2_CAID;
		er->rc = 0;
		oscam_write_ecm_answer(fd_c2m, er);
		return;
	}
	if (cache_lookup_ecm(er, client[er->cidx].grp)) {
		er->rc = 2;
		oscam_write_ecm_answer(fd_c2m, er);
		return;
	}
	if (proxy) {
		client[cs_idx].last_srvid = er->srvid;
		client[cs_idx].last_caid = er->caid;
		card_casc_process_ecm(er);
		return;
	}
	er->rc = reader_common_process_ecm(&reader[ridx], er);
	oscam_write_ecm_answer(fd_c2m, er);
//	if (reader[ridx].type == 'r') reader[ridx].qlen--;
}

static void card_send_dcw(ECM_REQUEST * er)
{
	if ((er->rc < 10)) {
		oscam_send_dcw(er);
	}
}

static int card_do_emm(EMM_PACKET * ep)
{
	int i, no, rc, ecs;
	char *rtxt[] = { "error", "written", "skipped" };
	struct timeb tps, tpe;

	cs_ftime(&tps);

	if (memcmp(ep->hexserial, reader[ridx].hexserial, 8))
		return (3);

	no = 0;
	for (i = ecs = 0; (i < CS_EMMCACHESIZE) && (!ecs); i++)
		if (!memcmp(emmcache[i].emm, ep->emm, ep->emm[2])) {
			if (reader[ridx].cachemm)
				ecs = (reader[ridx].rewritemm > emmcache[i].count) ? 1 : 2;
			else
				ecs = 1;
			no = ++emmcache[i].count;
			i--;
		}

	if ((rc = ecs) < 2) {
		rc = (proxy) ? 0 : reader_common_process_emm(&reader[ridx], ep);
		if (!ecs) {
			i = card_store_emm(ep->emm, ep->type);
			no = 1;
		}
	}
	if (rc)
		client[cs_idx].lastemm = time((time_t) 0);

	if (reader[ridx].logemm >= rc) {
		cs_ftime(&tpe);
//		log_normal("%s type=%02x, len=%d, idx=%d, cnt=%d: %s (%d ms)", cs_inet_ntoa(client[ep->cidx].ip), emmcache[i].type, ep->emm[2], i, no, rtxt[rc], 1000*(tpe.time-tps.time)+tpe.millitm-tps.millitm);
		log_normal("%s type=%02x, len=%d, idx=%d, cnt=%d: %s (%d ms)", oscam_username(ep->cidx), emmcache[i].type, ep->emm[2], i, no, rtxt[rc], 1000 * (tpe.time - tps.time) + tpe.millitm - tps.millitm);
	}

	return rc;
}

static int card_listen(int fd1, int fd2)
{
	int fdmax, tcp_toflag, use_tv = (!proxy);
	int is_tcp = (reader[ridx].ph.type == MOD_CONN_TCP);
	fd_set fds;
	struct timeval tv;

#ifdef CS_WITH_GBOX
	if (reader[ridx].typ == R_GBOX) {
		struct timeb tpe;
		int ms, x;

		cs_ftime(&tpe);
		for (x = 0; x < CS_MAXPENDING; x++) {
			ms = 1000 * (tpe.time - ecmtask[x].tps.time) + tpe.millitm - ecmtask[x].tps.millitm;
			if (ecmtask[x].rc == 10 && ms > cfg->ctimeout && ridx == ecmtask[x].gbxRidx) {
//				log_normal("hello rc=%d idx:%d x:%d ridx%d ridx:%d",ecmtask[x].rc,ecmtask[x].idx,x,ridx,ecmtask[x].gbxRidx);
				ecmtask[x].rc = 5;
				send_dcw(&ecmtask[x]);

			}
		}
	}
#endif

	if (master_pid != getppid())
		oscam_exit(0);
	tcp_toflag = (fd2 && is_tcp && reader[ridx].tcp_ito && reader[ridx].tcp_connected);
	tv.tv_sec = 0;
	tv.tv_usec = 100000L;
	if (tcp_toflag) {
		tv.tv_sec = reader[ridx].tcp_ito * 60;
		tv.tv_usec = 0;
		use_tv = 1;
	}
	FD_ZERO(&fds);
	FD_SET(fd1, &fds);
	if (fd2)
		FD_SET(fd2, &fds);
	if (logfd)
		FD_SET(logfd, &fds);
	fdmax = (fd1 > fd2) ? fd1 : fd2;
	fdmax = (fdmax > logfd) ? fdmax : logfd;
	if (select(fdmax + 1, &fds, 0, 0, (use_tv) ? &tv : 0) < 0)
		return 0;
	if (master_pid != getppid())
		oscam_exit(0);

	if ((logfd) && (FD_ISSET(logfd, &fds))) {
		log_debug("select: log-socket ist set");
		return 3;
	}

	if ((fd2) && (FD_ISSET(fd2, &fds))) {
		log_debug("select: socket is set");
		return 2;
	}

	if (FD_ISSET(fd1, &fds)) {
		if (tcp_toflag) {
			time_t now;
			int time_diff;

			time(&now);
			time_diff = abs(now - reader[ridx].last_s);
			if (time_diff > (reader[ridx].tcp_ito * 60)) {
				log_debug("%s inactive_timeout (%d), close connection (fd=%d)", reader[ridx].ph.desc, time_diff, fd2);
				network_tcp_connection_close(fd2);
			}
		}
		log_debug("select: pipe is set");
		return (1);
	}

	if (tcp_toflag) {
		log_debug("%s inactive_timeout (%d), close connection (fd=%d)", reader[ridx].ph.desc, tv.tv_sec, fd2);
		network_tcp_connection_close(fd2);
		return (0);
	}

	if (!proxy)
		reader_common_check_health(&reader[ridx]);

	return 0;
}

static void card_do_pipe()
{
	char *ptr;

	switch (oscam_read_from_pipe(fd_m2c, &ptr, 0)) {
		case PIP_ID_ECM:
			card_get_ecm((ECM_REQUEST *) ptr);
			break;
		case PIP_ID_DCW:
			card_send_dcw((ECM_REQUEST *) ptr);
			break;
		case PIP_ID_EMM:
			card_do_emm((EMM_PACKET *) ptr);
			break;
		case PIP_ID_CIN:
			reader_common_load_card(&reader[ridx]);
			break;
	}
}

static void card_main()
{
	while (1) {
		switch (card_listen(fd_m2c, pfd)) {
			case 1:
				card_do_pipe();
				break;
			case 2:
				card_casc_do_sock(0);
				break;
			case 3:
				card_casc_do_sock_log();
				break;
		}
	}
}

void card_start_reader()
{
	cs_ptyp = D_READER;

	if ((proxy = reader[ridx].type & R_IS_CASCADING)) {
		client[cs_idx].typ = 'p';
		client[cs_idx].port = reader[ridx].r_port;
		strcpy(client[cs_idx].usr, reader[ridx].r_usr);
		switch (reader[ridx].type) {
			case R_CAMD33:
				sharing_camd33_module(&reader[ridx].ph);
				break;
			case R_CAMD35:
				sharing_camd35_module_udp(&reader[ridx].ph);
				break;
			case R_NEWCAMD:
				sharing_newcamd_module(&reader[ridx].ph);
				break;
			case R_RADEGAST:
				sharing_radegast_module(&reader[ridx].ph);
				break;
			case R_SERIAL:
				sharing_serial_module(&reader[ridx].ph);
				break;
			case R_CS378X:
				sharing_camd35_module_tcp(&reader[ridx].ph);
				break;
#ifdef CS_WITH_GBOX
			case R_GBOX:
				module_gbox(&reader[ridx].ph);
				strcpy(client[cs_idx].usr, reader[ridx].label);
				break;
#endif
		}
		if (!(reader[ridx].ph.c_init)) {
			log_normal("FATAL: %s-protocol not supporting cascading", reader[ridx].ph.desc);
			sleep(1);
			oscam_exit(1);
		}
		if (reader[ridx].ph.c_init())
			oscam_exit(1);
		if ((reader[ridx].log_port) && (reader[ridx].ph.c_init_log))
			reader[ridx].ph.c_init_log();
	} else {
		client[cs_idx].ip = network_inet_addr("127.0.0.1");
		if (!reader_common_init(&reader[ridx]))
			oscam_exit(1);
	}

	emmcache = (struct s_emm *) malloc(CS_EMMCACHESIZE * (sizeof (struct s_emm)));
	if (!emmcache) {
		log_normal("Cannot allocate memory (errno=%d)", errno);
		oscam_exit(1);
	}
	memset(emmcache, 0, CS_EMMCACHESIZE * (sizeof (struct s_emm)));

	ecmtask = (ECM_REQUEST *) malloc(CS_MAXPENDING * (sizeof (ECM_REQUEST)));
	if (!ecmtask) {
		log_normal("Cannot allocate memory (errno=%d)", errno);
		oscam_exit(1);
	}
	memset(ecmtask, 0, CS_MAXPENDING * (sizeof (ECM_REQUEST)));

	card_main();
	oscam_exit(0);
}
