#include "globals.h"

#define REQ_SIZE	4
static	uchar	camdbug[256];		// camd send wrong order
static	uchar	*req;

static int camd33_send(uchar *buf, int ml)
{
  int l;
  if (!pfd) return(-1);
  l=boundary(4, ml);
  memset(buf+ml, 0, l-ml);
  cs_ddump(buf, l, "send %d bytes to client", l);
  if (client[cs_idx].crypted)
    aes_encrypt(buf, l);
  return(send(pfd, buf, l, 0));
}

static int camd33_recv(uchar *buf, int l)
{
  int n;
  if (!pfd) return(-1);
  if ((n=recv(pfd, buf, l, 0))>0)
  {
    client[cs_idx].last=time((time_t *) 0);
    if (client[cs_idx].crypted)
      aes_decrypt(buf, n);
  }
  cs_ddump(buf, n, "received %d bytes from client", n);
  return(n);
}

static void camd33_request_emm()
{
  int au;
  au=client[cs_idx].au;
  if ((au<0) || (au>CS_MAXREADER)) return;  // TODO
  if (reader[au].hexserial[0])
  {
    log_emm_request(au);
    mbuf[0]=0;
    mbuf[1]=reader[au].caid[0]>>8;
    mbuf[2]=reader[au].caid[0]&0xff;
    memcpy(mbuf+3, reader[au].hexserial, 4);
    memcpy(mbuf+7, &reader[au].prid[0][1], 3);
    memcpy(mbuf+10, &reader[au].prid[2][1], 3);
    camd33_send(mbuf, 13);
  }
}

static void camd33_auth_client()
{
  int i, rc;
  uchar *usr=NULL, *pwd=NULL;
  struct s_auth *account;

  client[cs_idx].crypted=cfg->c33_crypted;
  if (client[cs_idx].crypted)
  {
    struct s_ip *p_ip;
    for (p_ip=cfg->c33_plain; (p_ip) && (client[cs_idx].crypted); p_ip=p_ip->next)
      if ((client[cs_idx].ip>=p_ip->ip[0]) && (client[cs_idx].ip<=p_ip->ip[1]))
        client[cs_idx].crypted=0;
  }
  if (client[cs_idx].crypted)
    aes_set_key((char *) cfg->c33_key);

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
  for (rc=-1, account=cfg->account; (usr) && (account) && (rc<0); account=account->next)
    if ((!strcmp((char *)usr, account->usr)) && (!strcmp((char *)pwd, account->pwd)))
      rc=cs_auth_client(account, NULL);
  if (!rc)
    camd33_request_emm();
  else
  {
    if (rc<0) cs_auth_client(0, usr ? "invalid account" : "no user given");
    cs_exit(0);
  }
}

static int get_request(uchar *buf, int n)
{
  int rc, w;

  if (camdbug[0])
  {
    memcpy(buf, camdbug+1, rc=camdbug[0]);
    camdbug[0]=0;
    return(rc);
  }
  for (rc=w=0; !rc;)
  {
    switch (rc=process_input(buf, 16, (w) ? cfg->ctimeout : cfg->cmaxidle))
    {
      case -9:
        rc=0;
      case  0:
        if ((w) || cfg->c33_passive)
          rc=-1;
        else
        {
          buf[0]=0;
          camd33_send(buf, 1);
          w++;
        }
      case -1:
        break;
      default:
        if (!memcmp(buf+1, client[cs_idx].usr, strlen(client[cs_idx].usr)))
        {
          cs_log("%s still alive", cs_inet_ntoa(client[cs_idx].ip));
          rc=w=0;
        }
	      else
        {
          switch (buf[0])
          {
            case  2:
            case  3: w=boundary(4, buf[9]+10); break;
            default: w=n;	// garbage ?
          }
          w=process_input(buf+16, w-16, 0);
          if (w>0) rc+=w;
        }
    }
  }
  if (rc<0) rc=0;
  return(rc);
}

static void camd33_send_dcw(ECM_REQUEST *er)
{
  mbuf[0]=2;
  memcpy(mbuf+1, req+(er->cpti*REQ_SIZE), 4);	// get pin
  memcpy(mbuf+5, er->cw, 16);
  camd33_send(mbuf, 21);
  if (!cfg->c33_passive)
    camd33_request_emm();
}

static void camd33_process_ecm(uchar *buf, int l)
{
  ECM_REQUEST *er;
  if (!(er=get_ecmtask()))
    return;
  memcpy(req+(er->cpti*REQ_SIZE), buf+3, 4);	// save pin
  er->l=l-7;
  er->caid=b2i(2, buf+1);
  memcpy(er->ecm , buf+7, er->l);
  get_cw(er);
}

static void camd33_process_emm(uchar *buf, int l)
{
  memset(&epg, 0, sizeof(epg));
  epg.l=l-7;
  memcpy(epg.caid     , buf+1, 2);
  memcpy(epg.hexserial, buf+3, 4);
  memcpy(epg.emm      , buf+7, epg.l);
  do_emm(&epg);
}

static void camd33_server()
{
  int n;

  req=(uchar *)malloc(CS_MAXPENDING*REQ_SIZE);
  if (!req)
  {
    cs_log("Cannot allocate memory (errno=%d)", errno);
    cs_exit(1);
  }
  memset(req, 0, CS_MAXPENDING*REQ_SIZE);

  camd33_auth_client();

  while ((n=get_request(mbuf, sizeof(mbuf)))>0)
  {
    switch(mbuf[0])
    {
      case 2:
        camd33_process_ecm(mbuf, n);
        break;
      case 3:
        camd33_process_emm(mbuf, n);
        break;
      default:
        cs_debug("unknown command !");
    }
  }
  cs_disconnect_client();
}

void module_camd33(struct s_module *ph)
{
  static PTAB ptab;
  ptab.ports[0].s_port = cfg->c33_port;
  ph->ptab = &ptab;
  ph->ptab->nports = 1;

  strcpy(ph->desc, "camd 3.3x");
  ph->type=MOD_CONN_TCP;
  ph->logtxt=cfg->c33_crypted ? ", crypted" : ", UNCRYPTED!";
  ph->multi=1;
  ph->watchdog=1;
  ph->s_ip=cfg->c33_srvip;
  ph->s_handler=camd33_server;
  ph->recv=camd33_recv;
  ph->send_dcw=camd33_send_dcw;
}
