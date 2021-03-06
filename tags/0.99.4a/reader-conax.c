#include "globals.h"
#include "reader-common.h"

extern uchar cta_res[];
extern ushort cta_lr;

#define write_cmd(cmd, data) \
{ \
        if (card_write(cmd, data)) return ERROR; \
}

#define read_cmd(cmd, data) \
{ \
        if (card_write(cmd, NULL)) return ERROR; \
}

static char *chid_date(const uchar *ptr, char *buf, int l)
{
  if (buf)
  {
    snprintf(buf, l, "%04d/%02d/%02d",
              1990+(ptr[1]>>4)+(((ptr[0]>>5)&7)*10), ptr[1]&0xf, ptr[0]&0x1f);
  }
  return(buf);
}

static int read_record(uchar *cmd, uchar *data)
{
  uchar insCA[] = {0xDD, 0xCA, 0x00, 0x00, 0x00};

  write_cmd(cmd, data);		// select record
  if (cta_res[0]!=0x98)
    return(-1);
  insCA[4]=cta_res[1];		// get len
  read_cmd(insCA, NULL);	// read record
  if ((cta_res[cta_lr-2]!=0x90) || (cta_res[cta_lr-1]))
    return(-1);
  return(cta_lr-2);
}

int conax_card_init(ATR newatr)
{
  int i, j, n;
  uchar ins26[] = {0xDD, 0x26, 0x00, 0x00, 0x03, 0x10, 0x01, 0x40};
  uchar ins82[] = {0xDD, 0x82, 0x00, 0x00, 0x11, 0x11, 0x0f, 0x01, 0xb0, 0x0f, 0xff, \
                   0xff, 0xfb, 0x00, 0x00, 0x09, 0x04, 0x0b, 0x00, 0xe0, 0x30, 0x2b };

  uchar cardver=0;

  get_hist;
  if ((hist_size < 4) || (memcmp(hist,"0B00",4)))
    return ERROR;

  reader[ridx].caid[0]=0xB00;

  if ((n=read_record(ins26, ins26+5))<0) return ERROR;	// read caid, card-version
  for (i=0; i<n; i+=cta_res[i+1]+2)
    switch(cta_res[i])
    {
      case 0x20: cardver=cta_res[i+2]; break;
      case 0x28: reader[ridx].caid[0]=(cta_res[i+2]<<8)|cta_res[i+3];
    }

  // Ins82 command needs to use the correct CAID reported in nano 0x28
  ins82[17]=(reader[ridx].caid[0]>>8)&0xFF;
  ins82[18]=(reader[ridx].caid[1])&0xFF;

  if ((n=read_record(ins82, ins82+5))<0) return ERROR;	// read serial

  for (j=0, i=2; i<n; i+=cta_res[i+1]+2)
    switch(cta_res[i])
    {
      case 0x23:
        if ( cta_res[i+5] != 0x00)
        {
          memcpy(reader[ridx].hexserial, &cta_res[i+3], 6);
        }else{
          memcpy(reader[ridx].sa[j], &cta_res[i+5], 4);
        j++;
    }
    break;
  }

  /* we have one provider, 0x0000 */
  reader[ridx].nprov = 1;
  memset(reader[ridx].prid, 0x00, sizeof(reader[ridx].prid));

  cs_ri_log("type: Conax, caid: %04X, serial: %llu, card: v%d",
         reader[ridx].caid[0], b2ll(6, reader[ridx].hexserial), cardver);
  cs_ri_log("Providers:%d", reader[ridx].nprov);

  for (j=0; j<reader[ridx].nprov; j++)
  {
    cs_ri_log("Provider:%d  Provider-Id:%06X", j+1, b2ll(4, reader[ridx].prid[j]));
    cs_ri_log("Provider:%d  SharedAddress:%08X", j+1, b2ll(4, reader[ridx].sa[j]));
  }

  cs_log("[conax-reader] ready for requests");
  return OK;
}

static int conax_send_pin(void)
{
  unsigned char insPIN[] = { 0xDD,0xC8,0x00,0x00,0x07,0x1D,0x05,0x01,0x00,0x00,0x00,0x00 }; //Last four are the Pin-Code
  memcpy(insPIN+8,reader[ridx].pincode,4);

  write_cmd(insPIN, insPIN+5);
  cs_ri_log("sending pincode to card");

  return OK;
}


int conax_do_ecm(ECM_REQUEST *er)
{
  int i,j,n, rc=0;
  unsigned char insA2[]  = { 0xDD,0xA2,0x00,0x00,0x00 };
  unsigned char insCA[]  = { 0xDD,0xCA,0x00,0x00,0x00 };

  unsigned char buf[256];

  if ((n=CheckSctLen(er->ecm, 3))<0)
    return ERROR;

  buf[0]=0x14;
  buf[1]=n+1;
  buf[2]=0;

  memcpy(buf+3, er->ecm, n);
  insA2[4]=n+3;

  write_cmd(insA2, buf);  // write Header + ECM

  while ((cta_res[cta_lr-2]==0x98) && 	// Antwort
  ((insCA[4]=cta_res[cta_lr-1])>0) && (insCA[4]!=0xFF))
  {
    read_cmd(insCA, NULL);  //Codeword auslesen

    if ((cta_res[cta_lr-2]==0x98) ||
    ((cta_res[cta_lr-2]==0x90) ))
    {
      for(i=0; i<cta_lr-2; i+=cta_res[i+1]+2)
      {
        switch (cta_res[i])
        {
          case 0x25:
            if ( (cta_res[i+1]>=0xD) && !((n=cta_res[i+4])&0xFE) )
            {
            rc|=(1<<n);
            memcpy(er->cw+(n<<3), cta_res+i+7, 8);
            }
            break;
          case 0x31:
            if ( (cta_res[i+1]==0x02  && cta_res[i+2]==0x00  && cta_res[i+3]==0x00) || \
            (cta_res[i+1]==0x02  && cta_res[i+2]==0x40  && cta_res[i+3]==0x00) )
              break;
            else if (strcmp(reader[ridx].pincode, "none"))
            {
              conax_send_pin();
              write_cmd(insA2, buf);  // write Header + ECM

              while ((cta_res[cta_lr-2]==0x98) &&   // Antwort
                      ((insCA[4]=cta_res[cta_lr-1])>0) && (insCA[4]!=0xFF))
              {
                read_cmd(insCA, NULL);  //Codeword auslesen

                if ((cta_res[cta_lr-2]==0x98) ||
                    ((cta_res[cta_lr-2]==0x90) && (!cta_res[cta_lr-1])))
                {
                  for(j=0;j<cta_lr-2; j+=cta_res[j+1]+2)
                    if ((cta_res[j]==0x25) &&       // access: is cw
                        (cta_res[j+1]>=0xD) &&      // 0xD: 5 header + 8 cw
                        !((n=cta_res[j+4])&0xFE))   // cw idx must be 0 or 1
                    {
                      rc|=(1<<n);
                      memcpy(er->cw+(n<<3), cta_res+j+7, 8);
                    }
                }
              }
            }
            break;
        }
      }
    }
  }
  if (rc==3)
    return OK;
  else
    return ERROR;
}

int conax_do_emm(EMM_PACKET *ep)
{
  unsigned char insEMM[] = { 0xDD,0x84,0x00,0x00,0x00 };
  unsigned char buf[255];
  int rc=0;

  const int l = ep->emm[2];

  insEMM[4]=l+5;
  buf[0]=0x12;
  buf[1]=l+3;
  memcpy(buf+2, ep->emm, buf[1]);
  write_cmd(insEMM, buf);

  rc=((cta_res[0]==0x90)&&(cta_res[1]==0x00));

  if (rc)
    return OK;
  else
    return ERROR;
}

int conax_card_info(void)
{
  int type, i, j, k, n=0;
  ushort provid;
  char provname[32], pdate[32];
  uchar insC6[] = {0xDD, 0xC6, 0x00, 0x00, 0x03, 0x1C, 0x01, 0x00};
  uchar ins26[] = {0xDD, 0x26, 0x00, 0x00, 0x03, 0x1C, 0x01, 0x01};
  uchar insCA[] = {0xDD, 0xCA, 0x00, 0x00, 0x00};
  char *txt[] = { "Package", "PPV-Event" };
  uchar *cmd[] = { insC6, ins26 };

  cs_log("[conax-reader] card detected");
  cs_log("[conax-reader] type: Conax");

  for (type=0; type<2; type++)
  {
    n=0;
    write_cmd(cmd[type], cmd[type]+5);
    while (cta_res[cta_lr-2]==0x98)
    {
      insCA[4]=cta_res[cta_lr-1];		// get len
      read_cmd(insCA, NULL);		// read
      if ((cta_res[cta_lr-2]==0x90) || (cta_res[cta_lr-2]==0x98))
      {
        for (j=0; j<cta_lr-2; j+=cta_res[j+1]+2)
        {
          provid=(cta_res[j+2+type]<<8) | cta_res[j+3+type];
          for (k=0, i=j+4+type; (i<j+cta_res[j+1]) && (k<2); i+=cta_res[i+1]+2)
          {
            int l;
            switch(cta_res[i])
            {
              case 0x01: l=(cta_res[i+1]<(sizeof(provname)-1)) ?
                           cta_res[i+1] : sizeof(provname)-1;
                         memcpy(provname, cta_res+i+2, l);
                         provname[l]='\0';
                         break;
              case 0x30: chid_date(cta_res+i+2, pdate+(k++<<4), 15);
                         break;
            }
          }
          cs_ri_log("%s: %d, id: %04X, date: %s - %s, name: %s",
                    txt[type], ++n, provid, pdate, pdate+16, trim(provname));
        }
      }
    }
  }
  return OK;
}
