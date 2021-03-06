#include "globals.h"
#include "reader-common.h"
#include <stdlib.h>

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

static int set_provider_info(int i)
{
  static uchar ins12[] = { 0xc1, 0x12, 0x00, 0x00, 0x19 }; // get provider info
  int year, month, day;
  struct tm *lt;
  time_t t;
  int valid=0;//0=false, 1=true
  char l_name[16+8+1]=", name: ";

  ins12[2]=i;//select provider
  read_cmd(ins12, NULL); // show provider properties
  
  if ((cta_res[25] != 0x90) || (cta_res[26] != 0x00)) return ERROR;
  reader[ridx].prid[i][0]=0;
  reader[ridx].prid[i][1]=0;//blanken high byte provider code
  memcpy(&reader[ridx].prid[i][2], cta_res, 2);
  
  year = (cta_res[22]>>1) + 1990;
  month = ((cta_res[22]&0x1)<< 3) | (cta_res[23] >>5);
  day = (cta_res[23]&0x1f);
  t=time(NULL);
  lt=localtime(&t);
  if (lt->tm_year + 1900 != year)
     valid = (lt->tm_year + 1900 < year);
  else if (lt->tm_mon + 1 != month)
     valid = (lt->tm_mon + 1 < month);
  else if (lt->tm_mday != day)
     valid = (lt->tm_mday < day);

  memcpy(l_name+8, cta_res+2, 16);
  l_name[sizeof(l_name)-1]=0;
  trim(l_name+8);
  l_name[0]=(l_name[8]) ? ',' : 0;
  reader[ridx].availkeys[i][0]=valid; //misusing availkeys to register validity of provider
  cs_ri_log("[seca-reader] provider: %d, valid: %i%s, expiry date: %4d/%02d/%02d",
         i+1, valid,l_name, year, month, day);
  memcpy(&reader[ridx].sa[i][0], cta_res+18, 4);
  if (valid==1) //if not expired
    cs_ri_log("[seca-reader] SA: %s", cs_hexdump(0, cta_res+18, 4));
  return OK;
}

int seca_card_init(ATR newatr)
{
	get_atr;
	char *card;
	static unsigned short pmap=0;	// provider-maptable
	unsigned long long serial ;
  uchar buf[256];
  static uchar ins0e[] = { 0xc1, 0x0e, 0x00, 0x00, 0x08 }; // get serial number (UA)
  static uchar ins16[] = { 0xc1, 0x16, 0x00, 0x00, 0x07 }; // get nr. of prividers
  int i;

// Unlock parental control
// c1 30 00 01 09
// 00 00 00 00 00 00 00 00 ff
  static uchar ins30[] = { 0xc1, 0x30, 0x00, 0x01, 0x09 };
  static uchar ins30data[] = { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xff };

  buf[0]=0x00;
  if ((atr[10]!=0x0e) || (atr[11]!=0x6c) || (atr[12]!=0xb6) || (atr[13]!=0xd6)) return ERROR;
  switch(atr[7]<<8|atr[8])
  {
    case 0x5084: card="Generic"; break;
    case 0x5384: card="Philips"; break;
    case 0x5130:
    case 0x5430:
    case 0x5760: card="Thompson"; break;
    case 0x5284:
    case 0x5842:
    case 0x6060: card="Siemens"; break;
    case 0x7070: card="Canal+ NL"; break;
    default:     card="Unknown"; break;
  }
  reader[ridx].caid[0]=0x0100;
  memset(reader[ridx].prid, 0xff, sizeof(reader[ridx].prid));
  read_cmd(ins0e, NULL); // read unique id
  memcpy(reader[ridx].hexserial, cta_res+2, 6);
  serial = b2ll(5, cta_res+3) ;
  cs_ri_log("type: SECA, caid: %04X, serial: %llu, card: %s v%d.%d",
         reader[ridx].caid[0], serial, card, atr[9]&0x0F, atr[9]>>4);
  read_cmd(ins16, NULL); // read nr of providers
  pmap=cta_res[2]<<8|cta_res[3];
  for (reader[ridx].nprov=0, i=pmap; i; i>>=1)
    reader[ridx].nprov+=i&1;
 
  for (i=0; i<16; i++)
    if (pmap&(1<<i))
    {
      if (set_provider_info(i) == ERROR)
        return ERROR;
      else
	sprintf((char *) buf+strlen((char *)buf), ",%04lX", b2i(2, &reader[ridx].prid[i][2])); 
    }

  cs_ri_log("providers: %d (%s)", reader[ridx].nprov, buf+1);
// Unlock parental control
  if( cfg->ulparent != 0 ){
	  write_cmd(ins30, ins30data); 
	  cs_ri_log("[seca-reader] ins30_answer: %02x%02x",cta_res[0], cta_res[1]);
  }else {
	  cs_ri_log("[seca-reader] parental locked");
  }	
  cs_log("[seca-reader] ready for requests");
  return OK;
}

static int get_prov_index(char *provid)	//returns provider id or -1 if not found
{
  int prov;
  for (prov=0; prov<reader[ridx].nprov; prov++) //search for provider index
    if (!memcmp(provid, &reader[ridx].prid[prov][2], 2))
      return(prov);
  return(-1);
}
	

int seca_do_ecm(ECM_REQUEST *er)
{
  unsigned char ins3c[] = { 0xc1,0x3c,0x00,0x00,0x00 }; // coding cw
  unsigned char ins3a[] = { 0xc1,0x3a,0x00,0x00,0x10 }; // decoding cw
  int i;
  i=get_prov_index((char *) er->ecm+3);
  if ((i == -1) || (reader[ridx].availkeys[i][0] == 0)) //if provider not found or expired
  	return ERROR;
  ins3c[2]=i;
  ins3c[3]=er->ecm[7]; //key nr
  ins3c[4]=(((er->ecm[1]&0x0f) << 8) | er->ecm[2])-0x05;
  write_cmd(ins3c, er->ecm+8); //ecm request
  unsigned char ins30[] = { 0xC1, 0x30, 0x00, 0x02, 0x09 };
  unsigned char ins30data[] = { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xFF };
  /* We need to use a token */
  if (cta_res[0] == 0x90 && cta_res[1] == 0x1a) {
    write_cmd(ins30, ins30data);
    write_cmd(ins3c, er->ecm+8); //ecm request
  }
  if ((cta_res[0] != 0x90) || (cta_res[1] != 0x00)) return ERROR;
  read_cmd(ins3a, NULL); //get cw's
  if ((cta_res[16] != 0x90) || (cta_res[17] != 0x00)) return ERROR;//exit if response is not 90 00 //TODO: if response is 9027 ppv mode is possible!
  memcpy(er->cw,cta_res,16);
  return OK;
}

int seca_do_emm(EMM_PACKET *ep)
{
  unsigned char ins40[] = { 0xc1,0x40,0x00,0x00,0x00 };
  int i,ins40data_offset;
  int emm_length = ((ep->emm[1] & 0x0f) << 8) + ep->emm[2];

  cs_ddump (ep->emm, emm_length + 3, "EMM:");
  ep->type = ep->emm[0];
  switch (ep->type) {
    case 0x84:	//shared EMM
      {
	//to test if SA matches
	//first find out prov id
	i=get_prov_index((char *) ep->emm+3);
	if (i == -1) 
		return ERROR;
	//prov id found, now test for SA (only first 3 bytes, custom byte does not count)
	if (memcmp (ep->emm + 5, reader[ridx].sa[i], 3)) {
		cs_log("[seca-reader] EMM: Shared update did not match; EMM SA:%02X%02X%02X, provider %i, Reader SA:%s.", ep->emm[5], ep->emm[6], ep->emm[7], i + 1, cs_hexdump (0, reader[ridx].sa[i], 3));
		return ERROR;
	}
	else {
		cs_log("[seca-reader] EMM: Shared update matched for EMM SA %02X%02X%02X, provider %i.", ep->emm[5], ep->emm[6], ep->emm[7], i + 1);
		ins40[3]=ep->emm[9];
		ins40[4]= emm_length - 0x07;
		ins40data_offset = 10;
	}
	break;
      }//end shared EMM
    case 0x82:	//unique EMM
      {
	//first test if UA matches
 	if (memcmp (reader[ridx].hexserial, ep->emm + 3, 6)) {
		cs_log("[seca-reader] EMM: Unique update did not match; EMM Serial:%02X%02X%02X%02X%02X%02X, Reader Serial:%s.", ep->emm[3], ep->emm[4], ep->emm[5], ep->emm[6], ep->emm[7], ep->emm[8], cs_hexdump (0, reader[ridx].hexserial, 6));
		return ERROR;
	}
	else {
		//first find out prov id
		i=get_prov_index((char *) ep->emm+9);
                cs_log("[seca-reader] EMM: Unique update matched EMM Serial:%02X%02X%02X%02X%02X, provider %i.", ep->emm[3], ep->emm[4], ep->emm[5], ep->emm[6], ep->emm[7], ep->emm[8], i + 1);

		if (i==-1) 
			return ERROR;
		ins40[3]=ep->emm[12];
		ins40[4]= emm_length - 0x0A;
		ins40data_offset = 13;
	}
	break;
      } //end unique EMM
    case 0x83:  //new unknown EMM
  /*
EMM:
tp   len       shared-- cust
83 00 74 00 00 00 00 00 38  84C745CB7BFADA4E08F5FB8D0B6A26FA533682D83E6E594F778585F55F4784EF70495B3458C104D3D3F55FEA0F3BD47EC29265E8B2AAC83EBAA396A3890EA87154F41ED16DA6AB46C28E8935B55E4EFAB8215792A1BF61657BDEFAD02050E27F21E62AE29519F4815AB062340B7 */
    case 0x88:			//GA???
    case 0x89:			//GA???
    default:
	cs_log("[seca-reader] EMM: Congratulations, you have discovered a new EMM on SECA. This has not been decoded yet, so send this output to authors:");
        cs_dump (ep->emm, emm_length + 3, "EMM:");
  	return ERROR;	//unknown, no update
  }	//end of switch

  ins40[2]=i;
  write_cmd(ins40, ep->emm + ins40data_offset); //emm request
//TODO  if ((cta_res[16] != 0x90) || (cta_res[17] != 0x00)) return (0);
//  if ((cta_res[16] != 0x90) || (cta_res[17] != 0x19))
//	  seca_card_init(); //if return code = 90 19 then PPUA changed. //untested!!
//  else
  if (cta_res[0] == 0x97) {
	 cs_log("[seca-reader] EMM: Update not necessary.");
	 return OK; //Update not necessary
  }
  if ((cta_res[0] == 0x90) && ((cta_res[1] == 0x00) || (cta_res[1] == 0x19)))
  	if (set_provider_info(i) == OK) //after successfull EMM, print new provider info
	  return OK;
  return ERROR;
}

int seca_card_info (void)
{
//SECA Package BitMap records (PBM) can be used to determine whether the channel is part of the package that the SECA card can decrypt. This module reads the PBM
//from the SECA card. It cannot be used to check the channel, because this information seems to reside in the CA-descriptor, which seems not to be passed on through servers like camd, newcamd, radegast etc.
//
//This module is therefore optical only

  static unsigned char ins34[] = { 0xc1, 0x34, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00 };				//data following is provider Package Bitmap Records
  static unsigned char ins32[] = { 0xc1, 0x32, 0x00, 0x00, 0x20 };				// get PBM
  int prov;

  for (prov = 0; prov < reader[ridx].nprov; prov++) {
    ins32[2] = prov;
    write_cmd (ins34, ins34 + 5);	//prepare card for pbm request
    read_cmd(ins32, NULL);	//pbm request
    uchar pbm[8];		//TODO should be arrayed per prov
    switch (cta_res[0]) {
    case 0x04:
      cs_ri_log ("[seca-reader] no PBM for provider %i", prov + 1);
      break;
    case 0x83:
      memcpy (pbm, cta_res + 1, 8);
      cs_ri_log ("[seca-reader] PBM for provider %i: %s", prov + 1, cs_hexdump (0, pbm, 8));
      break;
    default:
      cs_log ("[seca-reader] ERROR: PBM returns unknown byte %02x", cta_res[0]);
    }
  }
  return OK;
}

