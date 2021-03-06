#include "globals.h"
#ifdef MODULE_CAMD33

#define REQ_SIZE	4

static int32_t camd33_send(uchar *buf, int32_t ml)
{
  int32_t l;
  if (!cur_client()->pfd) return(-1);
  l=boundary(4, ml);
  memset(buf+ml, 0, l-ml);
  cs_ddump_mask(D_CLIENT, buf, l, "send %d bytes to client", l);
  if (cur_client()->crypted)
    aes_encrypt(buf, l);
  return(send(cur_client()->pfd, buf, l, 0));
}

static int32_t camd33_recv(struct s_client * client, uchar *buf, int32_t l)
{
  int32_t n;
  if (!client->pfd) return(-1);
  if ((n=recv(client->pfd, buf, l, 0))>0)
  {
    client->last=time((time_t *) 0);
    if (client->crypted)
      aes_decrypt(buf, n);
  }
  cs_ddump_mask(D_CLIENT, buf, n, "received %d bytes from client", n);
  return(n);
}

static void camd33_request_emm()
{
  uchar mbuf[20];
	struct s_reader *aureader = NULL, *rdr = NULL;

	//TODO: just take the first reader in list
	LL_ITER itr = ll_iter_create(cur_client()->aureader_list);
	while ((rdr = ll_iter_next(&itr))) {
		aureader=rdr;
		break;
	}

	if (!aureader) return;

  if (aureader->hexserial[0])
  {
    log_emm_request(aureader);
    mbuf[0]=0;
    mbuf[1]=aureader->caid>>8;
    mbuf[2]=aureader->caid&0xff;
    memcpy(mbuf+3, aureader->hexserial, 4);
    memcpy(mbuf+7, &aureader->prid[0][1], 3);
    memcpy(mbuf+10, &aureader->prid[2][1], 3);
    camd33_send(mbuf, 13);
  }
}

static void camd33_auth_client(uchar *camdbug)
{
  int32_t i, rc;
  uchar *usr=NULL, *pwd=NULL;
  struct s_auth *account;
  uchar mbuf[1024];

  cur_client()->crypted=cfg.c33_crypted;

  if (cur_client()->crypted)
    cur_client()->crypted = check_ip(cfg.c33_plain, cur_client()->ip) ? 0 : 1;

  if (cur_client()->crypted)
    aes_set_key((char *) cfg.c33_key);

  mbuf[0]=0;
  camd33_send(mbuf, 1);	// send login-request

  for (rc=0, camdbug[0]=0, mbuf[0]=1; (rc<2) && (mbuf[0]); rc++)
  {
    i=process_input(mbuf, sizeof(mbuf), 1);
    if ((i>0) && (!mbuf[0]))
    {
      usr=mbuf+1;
      pwd=usr+strlen((char *)usr)+2;
    }
    else
      memcpy(camdbug+1, mbuf, camdbug[0]=i);
  }
  for (rc=-1, account=cfg.account; (usr) && (account) && (rc<0); account=account->next)
    if ((!strcmp((char *)usr, account->usr)) && (!strcmp((char *)pwd, account->pwd)))
      rc=cs_auth_client(cur_client(), account, NULL);
  if (!rc)
    camd33_request_emm();
  else
  {
    if (rc<0) cs_auth_client(cur_client(), 0, usr ? "invalid account" : "no user given");
    cs_disconnect_client(cur_client());
  }
}

static void camd33_send_dcw(struct s_client *UNUSED(client), ECM_REQUEST *er)
{
  uchar mbuf[1024];
  mbuf[0]=2;
  memcpy(mbuf+1, &er->msgid, 4);	// get pin
  memcpy(mbuf+5, er->cw, 16);
  camd33_send(mbuf, 21);
  if (!cfg.c33_passive)
    camd33_request_emm();
}

static void camd33_process_ecm(uchar *buf, int32_t l)
{
  ECM_REQUEST *er;
  if (!(er=get_ecmtask()))
    return;
  memcpy(&er->msgid, buf+3, 4);	// save pin
  er->l=l-7;
  er->caid=b2i(2, buf+1);
  memcpy(er->ecm , buf+7, er->l);
  get_cw(cur_client(), er);
}

static void camd33_process_emm(uchar *buf, int32_t l)
{
  EMM_PACKET epg;
  memset(&epg, 0, sizeof(epg));
  epg.l=l-7;
  memcpy(epg.caid     , buf+1, 2);
  memcpy(epg.hexserial, buf+3, 4);
  memcpy(epg.emm      , buf+7, epg.l);
  do_emm(cur_client(), &epg);
}

static void * camd33_server(struct s_client *UNUSED(client), uchar *mbuf, int32_t n)
{
	switch(mbuf[0]) {
		case 2:
			camd33_process_ecm(mbuf, n);
			break;
		case 3:
			camd33_process_emm(mbuf, n);
			break;
		default:
			cs_debug_mask(D_CLIENT, "unknown command !");
	}

	return NULL;
}

static void camd33_server_init(struct s_client *UNUSED(client)) {
	uchar camdbug[256];

	camd33_auth_client(camdbug);
}

void module_camd33(struct s_module *ph)
{
  static PTAB ptab; //since there is always only 1 camd33 server running, this is threadsafe
  ptab.ports[0].s_port = cfg.c33_port;
  ph->ptab = &ptab;
  ph->ptab->nports = 1;

  cs_strncpy(ph->desc, "camd33", sizeof(ph->desc));
  ph->type=MOD_CONN_TCP;
  ph->listenertype = LIS_CAMD33TCP;
  ph->logtxt=cfg.c33_crypted ? ", crypted" : ", UNCRYPTED!";
  ph->multi=1;
  ph->s_ip=cfg.c33_srvip;
  ph->s_handler=camd33_server;
  ph->s_init=camd33_server_init;
  ph->recv=camd33_recv;
  ph->send_dcw=camd33_send_dcw;
  ph->num=R_CAMD33;
}
#endif
