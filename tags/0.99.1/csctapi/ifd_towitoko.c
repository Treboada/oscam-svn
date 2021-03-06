/*
    ifd_towitoko.c
    This module provides IFD handling functions.

    This file is part of the Unix driver for Towitoko smartcard readers
    Copyright (C) 2000 2001 Carlos Prados <cprados@yahoo.com>

    This version is modified by doz21 to work in a special manner ;)

    This library is free software; you can redistribute it and/or
    modify it under the terms of the GNU Lesser General Public
    License as published by the Free Software Foundation; either
    version 2 of the License, or (at your option) any later version.

    This library is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
    Lesser General Public License for more details.

    You should have received a copy of the GNU Lesser General Public
    License along with this library; if not, write to the Free Software
    Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
*/

#include "defines.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#ifdef OS_HPUX
#include <sys/modem.h>
#endif
#include <termios.h>
#include <unistd.h>
#include <sys/stat.h>
#include <fcntl.h>
#ifdef HAVE_POLL
#include <sys/poll.h>
#else
#include <sys/signal.h>
#include <sys/types.h>
#include <sys/time.h>
#endif
#include <sys/ioctl.h>
#include <time.h>
#include "ifd_towitoko.h"
#include "io_serial.h"
#include "sci_global.h"
#include "sci_ioctl.h"

/*
 * Not exported constants
 */

#define IFD_TOWITOKO_TIMEOUT             1000
#define IFD_TOWITOKO_DELAY               0
#define IFD_TOWITOKO_BAUDRATE            9600
#define IFD_TOWITOKO_PS                  15
#define IFD_TOWITOKO_MAX_TRANSMIT        255
//#define IFD_TOWITOKO_ATR_TIMEOUT	 200
//#define IFD_TOWITOKO_ATR_TIMEOUT	 400
#define IFD_TOWITOKO_ATR_TIMEOUT	 800
#define IFD_TOWITOKO_ATR_MIN_LENGTH      1
#define IFD_TOWITOKO_CLOCK_RATE          (625L * 9600L)
//#define IFD_TOWITOKO_CLOCK_RATE          (372L * 9600L)

#define HI(a) 				(((a) & 0xff00) >> 8)
#define LO(a) 				((a) & 0x00ff)

/*
 * Not exported functions declaration
 */

static int IFD_Towitoko_GetReaderInfo (IFD * ifd);
static void IFD_Towitoko_Clear (IFD * ifd);

#ifdef USE_GPIO

int gpio_outen,gpio_out,gpio_in;
unsigned int pin,gpio;
int gpio_detect=0;

static void set_gpio(int level)
{     
	read(gpio_outen, &gpio, sizeof(gpio));
	gpio |= pin; 
	write(gpio_outen, &gpio, sizeof(gpio));

	read(gpio_out, &gpio, sizeof(gpio));
	if (level>0)
		gpio|=pin;
	else
		gpio&=~pin;
	write(gpio_out, &gpio, sizeof(gpio));
}

static void set_gpio1(int level)
{     
	read(gpio_outen, &gpio, sizeof(gpio));
	gpio |= 2; 
	write(gpio_outen, &gpio, sizeof(gpio));

	read(gpio_out, &gpio, sizeof(gpio));
	if (level>0)
		gpio|=2;
	else
		gpio&=~2;
	write(gpio_out, &gpio, sizeof(gpio));
}

static void set_gpio_input(void)
{
	read(gpio_outen, &gpio, sizeof(gpio));      
	gpio &= ~pin;
	write(gpio_outen, &gpio, sizeof(gpio));
}

static int get_gpio(void)
{
	set_gpio_input();
	read(gpio_in, &gpio, sizeof(gpio));
	return ((int)((gpio&pin)?1:0));
}
#endif

/*
 * Exported functions definition
 */

IFD * IFD_Towitoko_New ()
{
	IFD *ifd;
	
	ifd = (IFD *) malloc (sizeof (IFD));
	
	if (ifd != NULL)
		IFD_Towitoko_Clear (ifd);
	
	return ifd;
}

void IFD_Towitoko_Delete (IFD * ifd)
{
	free (ifd);
}

int IFD_Towitoko_Init (IFD * ifd, IO_Serial * io, BYTE slot)
{
	IO_Serial_Properties props;
	int ret;

#ifdef USE_GPIO	     	
	extern int oscam_card_detect;
	if (oscam_card_detect>4)
	{
		gpio_detect=oscam_card_detect-3;
		pin = 1<<gpio_detect;
		gpio_outen=open("/dev/gpio/outen",O_RDWR);
		gpio_out=open("/dev/gpio/out",O_RDWR);
		gpio_in=open("/dev/gpio/in",O_RDWR); 
		set_gpio_input();
	}
#endif
	
#ifdef DEBUG_IFD
	printf ("IFD: Initialicing slot number %d, com=%d\n", slot, io->com);
#endif
	
//	if ((slot != IFD_TOWITOKO_SLOT_MULTICAM) && (slot != IFD_TOWITOKO_SLOT_A) && (slot != IFD_TOWITOKO_SLOT_B))
	if (slot != IFD_TOWITOKO_SLOT_MULTICAM )
		return IFD_TOWITOKO_PARAM_ERROR;
		
	if(io->com==RTYP_SCI)
	{
		ifd->io = io;
		ifd->slot = slot;
		ifd->type = IFD_TOWITOKO_MULTICAM;
		return IFD_TOWITOKO_OK;
	}

	
	/* Default serial port settings */
	props.input_bitrate = IFD_TOWITOKO_BAUDRATE;
	props.output_bitrate = IFD_TOWITOKO_BAUDRATE;
	props.bits = 8;
	props.stopbits = 2;
	props.parity = IO_SERIAL_PARITY_EVEN;
	props.dtr = IO_SERIAL_HIGH;
//	props.dtr = IO_SERIAL_LOW;
//	props.rts = IO_SERIAL_HIGH;
	props.rts = IO_SERIAL_LOW;
	
		
	if (!IO_Serial_SetProperties (io, &props))
		return IFD_TOWITOKO_IO_ERROR;
		
	/* Default ifd settings */
	
	ifd->io = io;
	ifd->slot = slot;
	ifd->type = IFD_TOWITOKO_MULTICAM;
	
	ret = IFD_Towitoko_SetBaudrate (ifd, IFD_TOWITOKO_BAUDRATE);
	
	if (ret != IFD_TOWITOKO_OK)
	{
		IFD_Towitoko_Clear (ifd);
		return ret;
	}
	
	ret = IFD_Towitoko_SetParity (ifd, IFD_TOWITOKO_PARITY_EVEN);
	
	if (ret != IFD_TOWITOKO_OK)
	{
		IFD_Towitoko_Clear (ifd);
		return ret;
	}
	
	ret = IFD_Towitoko_GetReaderInfo (ifd);
		
	if (ret != IFD_TOWITOKO_OK)
	{
		IFD_Towitoko_Clear (ifd);
	}
	else
	{	
		IO_Serial_Flush(ifd->io);
	}
	
	return ret;
}

int IFD_Towitoko_Close (IFD * ifd)
{
	int ret;

#ifdef USE_GPIO
	if(gpio_detect) 
	{
		close(gpio_outen);
		close(gpio_out);
		close(gpio_in);
       }
#endif
	
#ifdef DEBUG_IFD
	printf ("IFD: Closing slot number %d\n", ifd->slot);
#endif
	
	ret = IFD_Towitoko_SetLED (ifd, IFD_TOWITOKO_LED_OFF);
	if (ret != IFD_TOWITOKO_OK)
		return ret;
		
	IFD_Towitoko_Clear (ifd);
	
	
	return IFD_TOWITOKO_OK;
}

int IFD_Towitoko_SetBaudrate (IFD * ifd, unsigned long baudrate)
{
	IO_Serial_Properties props;
	
	if(ifd->io->com==RTYP_SCI)
	{
		return IFD_TOWITOKO_OK;
	}
	
	if (IFD_Towitoko_GetMaxBaudrate (ifd) < baudrate)
	{
#ifdef DEBUG_IFD
		printf ("IFD: Tried to set unsupported baudrate: %lu", baudrate);
#endif
		return IFD_TOWITOKO_PARAM_ERROR;
	}
	
#ifdef DEBUG_IFD
	printf ("IFD: Setting baudrate to %lu\n", baudrate);
#endif
	
	/* Get current settings */
	if (!IO_Serial_GetProperties (ifd->io, &props))
		return IFD_TOWITOKO_IO_ERROR;
	
	if (props.output_bitrate == baudrate)
		return IFD_TOWITOKO_OK;

	
	/* Set serial device bitrate */
	props.output_bitrate = baudrate;
	props.input_bitrate = baudrate;
	
	if (!IO_Serial_SetProperties (ifd->io, &props))
		return IFD_TOWITOKO_IO_ERROR;
	
	return IFD_TOWITOKO_OK;
}

int IFD_Towitoko_GetBaudrate (IFD * ifd, unsigned long *baudrate)
{
	IO_Serial_Properties props;
	
	if(ifd->io->com==RTYP_SCI)
	{
		return IFD_TOWITOKO_OK;
	}
	
	/* Get current settings */
	if (!IO_Serial_GetProperties (ifd->io, &props))
		return IFD_TOWITOKO_IO_ERROR;
	
	(*baudrate) = props.output_bitrate;
	
	return IFD_TOWITOKO_OK;
}

extern int IFD_Towitoko_SetParity (IFD * ifd, BYTE parity)
{
	IO_Serial_Properties props;
		
	if(ifd->io->com==RTYP_SCI)
	{
		return IFD_TOWITOKO_OK;
	}
	
#ifdef DEBUG_IFD
	printf ("IFD: Parity = %s\n",
	parity == IFD_TOWITOKO_PARITY_ODD ? "Odd" :
	parity == IFD_TOWITOKO_PARITY_EVEN ? "Even" : "Invalid");
#endif
	
	if ((parity != IFD_TOWITOKO_PARITY_EVEN) && (parity != IFD_TOWITOKO_PARITY_ODD) && (parity != IFD_TOWITOKO_PARITY_NONE))
		return IFD_TOWITOKO_PARAM_ERROR;
	
	/* Get current settings */
	if (!IO_Serial_GetProperties (ifd->io, &props))
		return IFD_TOWITOKO_IO_ERROR;
	
	if (props.parity !=parity)
	{
		props.parity = parity;
		
		if (!IO_Serial_SetProperties (ifd->io, &props))
			return IFD_TOWITOKO_IO_ERROR;
	}
	
	return IFD_TOWITOKO_OK;
}

int IFD_Towitoko_SetLED (IFD * ifd, BYTE color)
{	
	return IFD_TOWITOKO_OK;
}

int IFD_Towitoko_GetStatus (IFD * ifd, BYTE * result)
{
	BYTE status[2];
	unsigned int modembits=0;
	int in;
	
//	printf("\n%08X\n", (int)ifd->io);
	
// status : 0 -start, 1 - card, 2- no card

#ifdef SCI_DEV
	if(ifd->io->com==RTYP_SCI)
	{
		if(ioctl(ifd->io->fd, IOCTL_GET_IS_CARD_PRESENT, &in)<0)
			return IFD_TOWITOKO_IO_ERROR;			
	}
	else
#endif
#if defined(TUXBOX) && defined(PPC)
	if ((ifd->io->com==RTYP_DB2COM1) || (ifd->io->com==RTYP_DB2COM2))
	{
		ushort msr=1;
		extern int fdmc;
		IO_Serial_Ioctl_Lock(ifd->io, 1);
		ioctl(fdmc, GET_PCDAT, &msr);
		if (ifd->io->com==RTYP_DB2COM2)
			in=(!(msr & 1));
		else
			in=((msr & 0x0f00) == 0x0f00);
		IO_Serial_Ioctl_Lock(ifd->io, 0);
	}
	else
#endif
#ifdef USE_GPIO
	if (gpio_detect)
		in=get_gpio();
	else
#endif
	{
		extern int oscam_card_detect;
		if (ioctl(ifd->io->fd, TIOCMGET,&modembits)<0)
			return IFD_TOWITOKO_IO_ERROR;
		switch(oscam_card_detect&0x7f)
		{
			case  0: in=(modembits & TIOCM_CAR);	break;
			case  1: in=(modembits & TIOCM_DSR);	break;
			case  2: in=(modembits & TIOCM_CTS);	break;
			case  3: in=(modembits & TIOCM_RNG);	break;
			default: in=0;		// dummy
		}
		if (!(oscam_card_detect&0x80))
			in=!in;
	}

	if (in)
	{       
		if(ifd->status == 0)
		{
			status[0] = IFD_TOWITOKO_CARD_CHANGE;
			ifd->status = 1;
#ifdef USE_GPIO
			if (gpio_detect) set_gpio1(0);
#endif
		}
		else if(ifd->status == 1)
		{
			status[0] = IFD_TOWITOKO_CARD_NOCHANGE;
		}
		else
		{
			status[0] = IFD_TOWITOKO_CARD_CHANGE;
			ifd->status = 1;
#ifdef USE_GPIO
			if (gpio_detect) set_gpio1(0);
#endif
		}
	}
	else
	{
		if(ifd->status == 0)
		{
			status[0] = IFD_TOWITOKO_NOCARD_CHANGE;
			ifd->status = 2;
#ifdef USE_GPIO
			if (gpio_detect) set_gpio1(1);
#endif
		}
		else if(ifd->status == 1)
		{
			status[0] = IFD_TOWITOKO_NOCARD_CHANGE;
			ifd->status = 2;
#ifdef USE_GPIO
			if (gpio_detect) set_gpio1(1);
#endif
		}
		else
		{
			status[0] = IFD_TOWITOKO_NOCARD_NOCHANGE;
		}
	}
	
		
	(*result) = status[0];
	
#ifdef DEBUG_IFD
	printf ("IFD: com%d Status = %s / %s\n", ifd->io->com, IFD_TOWITOKO_CARD(status[0])? "card": "no card", IFD_TOWITOKO_CHANGE(status[0])? "change": "no change");
#endif
	
	return IFD_TOWITOKO_OK;
}

int IFD_Towitoko_ActivateICC (IFD * ifd)
{
#ifdef DEBUG_IFD
		printf ("IFD: Activating card\n");
#endif
#ifdef SCI_DEV
	if(ifd->io->com==RTYP_SCI)
	{
		int in;

#if defined(TUXBOX) && defined(MIPSEL)
		if(ioctl(ifd->io->fd, IOCTL_GET_IS_CARD_PRESENT, &in)<0)
#else
		if(ioctl(ifd->io->fd, IOCTL_GET_IS_CARD_ACTIVATED, &in)<0)
#endif
			return IFD_TOWITOKO_IO_ERROR;
			
		if(in)
		{
			struct timespec req_ts;
			req_ts.tv_sec = 0;
			req_ts.tv_nsec = 50000000;
			nanosleep (&req_ts, NULL);
			return IFD_TOWITOKO_OK;
		}
		else
		{
			return IFD_TOWITOKO_IO_ERROR;
		}
	}
	else
#endif
	{
		return IFD_TOWITOKO_OK;
	}
}

int IFD_Towitoko_DeactivateICC (IFD * ifd)
{
#ifdef DEBUG_IFD
		printf ("IFD: Deactivating card\n");
#endif

#ifdef SCI_DEV
	if(ifd->io->com==RTYP_SCI)
	{
		int in;
		
#if defined(TUXBOX) && defined(MIPSEL)
		if(ioctl(ifd->io->fd, IOCTL_GET_IS_CARD_PRESENT, &in)<0)
#else
		if(ioctl(ifd->io->fd, IOCTL_GET_IS_CARD_ACTIVATED, &in)<0)
#endif
			return IFD_TOWITOKO_IO_ERROR;
			
		if(in)
		{
			if(ioctl(ifd->io->fd, IOCTL_SET_DEACTIVATE)<0)
				return IFD_TOWITOKO_IO_ERROR;
		}
		
		
	}
#endif
	
	return IFD_TOWITOKO_OK;
}

//extern void print_hex_data(unsigned char *data, int len);

int IFD_Towitoko_ResetAsyncICC (IFD * ifd, ATR ** atr)
{	 

#ifdef DEBUG_IFD
	printf ("IFD: Resetting card:\n");
#endif

#ifdef SCI_DEV
	if(ifd->io->com==RTYP_SCI)
	{
		unsigned char buf[SCI_MAX_ATR_SIZE];
		int n = 0;
		SCI_PARAMETERS params;
		static char irdeto[] = "IRDETO";
		
		(*atr) = NULL;
		
		if(ioctl(ifd->io->fd, IOCTL_SET_RESET)<0)
			return IFD_TOWITOKO_IO_ERROR;
			
		if(ioctl(ifd->io->fd, IOCTL_SET_ATR_READY)<0)
			return IFD_TOWITOKO_IO_ERROR;
			
		while(n<SCI_MAX_ATR_SIZE && IO_Serial_Read(ifd->io, IFD_TOWITOKO_ATR_TIMEOUT, 1, buf+n))
		{
			n++;
		}
		
		if(n==0)
			return IFD_TOWITOKO_IO_ERROR;
			
		if(ioctl(ifd->io->fd, IOCTL_GET_PARAMETERS, &params)<0)
			return IFD_TOWITOKO_IO_ERROR;
/*			
		printf("T=%d\n", (int)params.T);
		printf("f=%d\n", (int)params.f);
		printf("ETU=%d\n", (int)params.ETU);
		printf("WWT=%d\n", (int)params.WWT);
		printf("CWT=%d\n", (int)params.CWT);
		printf("BWT=%d\n", (int)params.BWT);
		printf("EGT=%d\n", (int)params.EGT);
		printf("clock=%d\n", (int)params.clock_stop_polarity);
		printf("check=%d\n", (int)params.check);
		printf("P=%d\n", (int)params.P);
		printf("I=%d\n", (int)params.I);
		printf("U=%d\n", (int)params.U);
*/

//		print_hex_data(buf, n);
		if(n>9 && !memcmp(buf+4, irdeto, 6))
		{
			params.T = 14;
			params.WWT = 1500;
			params.EGT = 5;
			buf[0]=0x3B;
		}
/*					
		if(params.ETU>600 && (buf[0]!=0x3B || buf[0]!=0x3F))
		{
			params.T = 14;
			params.WWT = 1500;
			params.EGT = 5;
			buf[0]=0x3B;
		}
*/			
		(*atr) = ATR_New ();
		if(ATR_InitFromArray ((*atr), buf, n) == ATR_OK)
		{
			struct timespec req_ts;
			double a;
			
			ATR_GetParameter(*atr, ATR_PARAMETER_P, &a);
//			printf("atr P=%f\n", a);
			params.P = (unsigned char)a;
			ATR_GetParameter(*atr, ATR_PARAMETER_I, &a);
//			printf("atr I=%f\n", a);
			params.I = (unsigned char)a;


			if(ioctl(ifd->io->fd, IOCTL_SET_PARAMETERS, &params)!=0)
			{
				ATR_Delete (*atr);
				(*atr) = NULL;
				return IFD_TOWITOKO_IO_ERROR;
			}
			

/*
			ioctl(ifd->io->fd, IOCTL_GET_PARAMETERS, &params);
			
		printf("T=%d\n", (int)params.T);
		printf("f=%d\n", (int)params.f);
		printf("ETU=%d\n", (int)params.ETU);
		printf("WWT=%d\n", (int)params.WWT);
		printf("CWT=%d\n", (int)params.CWT);
		printf("BWT=%d\n", (int)params.BWT);
		printf("EGT=%d\n", (int)params.EGT);
		printf("clock=%d\n", (int)params.clock_stop_polarity);
		printf("check=%d\n", (int)params.check);
		printf("P=%d\n", (int)params.P);
		printf("I=%d\n", (int)params.I);
		printf("U=%d\n", (int)params.U);*/


			
			req_ts.tv_sec = 0;
			req_ts.tv_nsec = 50000000;
			nanosleep (&req_ts, NULL);
			return IFD_TOWITOKO_OK;
		}
		else
		{
			ATR_Delete (*atr);
			(*atr) = NULL;
			return IFD_TOWITOKO_IO_ERROR;
		}
	}
	else
#endif
	{
		int ret;
		int parity;
		int i;
		int par[3] = {IFD_TOWITOKO_PARITY_EVEN, IFD_TOWITOKO_PARITY_ODD, IFD_TOWITOKO_PARITY_NONE};
#ifdef HAVE_NANOSLEEP
		struct timespec req_ts;
		req_ts.tv_sec = 0;
		req_ts.tv_nsec = 50000000;
#endif
		
		for(i=0; i<3; i++)
		{
			parity = par[i];
			IO_Serial_Flush(ifd->io);

			ret = IFD_Towitoko_SetParity (ifd, parity);
			if (ret != IFD_TOWITOKO_OK)
				return ret;

			ret = IFD_TOWITOKO_IO_ERROR;

			IO_Serial_Ioctl_Lock(ifd->io, 1);
#ifdef USE_GPIO
			if (gpio_detect)
			{
				set_gpio(0);
				set_gpio1(0);
			}
			else
#endif
				IO_Serial_RTS_Set(ifd->io);

#ifdef HAVE_NANOSLEEP
			nanosleep (&req_ts, NULL);
#else
			usleep (50000L);
#endif
#ifdef USE_GPIO
			if (gpio_detect)
			{
				set_gpio_input();
				set_gpio1(1);
			}
			else
#endif
				IO_Serial_RTS_Clr(ifd->io);
			
			IO_Serial_Ioctl_Lock(ifd->io, 0);

			(*atr) = ATR_New ();

			if(ATR_InitFromStream ((*atr), ifd->io, IFD_TOWITOKO_ATR_TIMEOUT) == ATR_OK)
				ret = IFD_TOWITOKO_OK;

			/* Succesfully retrive ATR */
			if (ret == IFD_TOWITOKO_OK)
			{			
				break;
			}
			else
			{
				ATR_Delete (*atr);
				(*atr) = NULL;
#ifdef USE_GPIO
				if (gpio_detect) set_gpio1(0);
#endif
			}	
		}
	
		IO_Serial_Flush(ifd->io);
#ifndef NO_PAR_SWITCH
		IFD_Towitoko_SetParity (ifd, IFD_TOWITOKO_PARITY_NONE);
#endif
		return ret;
	}
}

int IFD_Towitoko_Transmit (IFD * ifd, IFD_Timings * timings, unsigned size, BYTE * buffer)
{
	unsigned block_delay, char_delay, sent=0, to_send = 0;

#ifdef DEBUG_IFD
	printf ("IFD: Transmit: ");
	for (sent = 0; sent < size; sent++)
	printf ("%X ", buffer[sent]);
	printf ("\n");
#endif


	/* Calculate delays */
	char_delay = IFD_TOWITOKO_DELAY + timings->char_delay;
	block_delay = IFD_TOWITOKO_DELAY + timings->block_delay;

#ifdef USE_GPIO
	if (gpio_detect) set_gpio1(0);
#endif
	for (sent = 0; sent < size; sent = sent + to_send) 
	{
		/* Calculate number of bytes to send */
		to_send = MIN(size, IFD_TOWITOKO_MAX_TRANSMIT);
				
		/* Send data */
		if ((sent == 0) && (block_delay != char_delay))
		{
			if (!IO_Serial_Write (ifd->io, block_delay, 1, buffer))
				return IFD_TOWITOKO_IO_ERROR;
			
			if (!IO_Serial_Write (ifd->io, char_delay, to_send-1, buffer+1))
				return IFD_TOWITOKO_IO_ERROR;
		}
		else
		{
			if (!IO_Serial_Write (ifd->io, char_delay, to_send, buffer+sent))
				return IFD_TOWITOKO_IO_ERROR;
		}
	}
#ifdef USE_GPIO
	if (gpio_detect) set_gpio1(1);
#endif
	return IFD_TOWITOKO_OK;
}

int IFD_Towitoko_Receive (IFD * ifd, IFD_Timings * timings, unsigned size, BYTE * buffer)
{
	unsigned char_timeout, block_timeout;
#ifdef DEBUG_IFD
	int i;
#endif
		
	/* Calculate timeouts */
	char_timeout = IFD_TOWITOKO_TIMEOUT + timings->char_timeout;
	block_timeout = IFD_TOWITOKO_TIMEOUT + timings->block_timeout;
#ifdef USE_GPIO
	if (gpio_detect) set_gpio1(0);
#endif
	if (block_timeout != char_timeout)
	{
		/* Read first byte using block timeout */
		if (!IO_Serial_Read (ifd->io, block_timeout, 1, buffer))
			return IFD_TOWITOKO_IO_ERROR;
		
		if (size > 1)
		{
			/* Read remaining data bytes using char timeout */
			if (!IO_Serial_Read (ifd->io, char_timeout, size - 1, buffer + 1))
				return IFD_TOWITOKO_IO_ERROR;
		}
	}
	else
	{
		/* Read all data bytes with the same timeout */
		if (!IO_Serial_Read (ifd->io, char_timeout, size, buffer))
			return IFD_TOWITOKO_IO_ERROR;
	}
#ifdef USE_GPIO
	if (gpio_detect) set_gpio1(1);
#endif
	
#ifdef DEBUG_IFD
	printf ("IFD: Receive: ");
	for (i = 0; i < size; i++)
	printf ("%X ", buffer[i]);
	printf ("\n");
#endif
	
	return IFD_TOWITOKO_OK;
}


BYTE IFD_Towitoko_GetType (IFD * ifd)
{
	return ifd->type;
}

void IFD_Towitoko_GetDescription (IFD * ifd, BYTE * desc, unsigned length)
{
	char buffer[3];

	if (ifd->type == IFD_TOWITOKO_CHIPDRIVE_EXT_II)
		memcpy (desc,"CE2",MIN(length,3));

	else if  (ifd->type == IFD_TOWITOKO_CHIPDRIVE_EXT_I)
		memcpy (desc,"CE1",MIN(length,3));

	else if (ifd->type == IFD_TOWITOKO_CHIPDRIVE_INT)
		memcpy (desc,"CDI",MIN(length,3));

	else if (ifd->type == IFD_TOWITOKO_CHIPDRIVE_MICRO)
		memcpy (desc,"CDM",MIN(length,3));

	else if (ifd->type == IFD_TOWITOKO_KARTENZWERG_II) 
		memcpy (desc,"KZ2",MIN(length,3));

	else if (ifd->type == IFD_TOWITOKO_KARTENZWERG)
		memcpy (desc,"KZ1",MIN(length,3));
		
	else if (ifd->type == IFD_TOWITOKO_MULTICAM)
		memcpy (desc,"MCM",MIN(length,3));

	else 
		memcpy (desc,"UNK",MIN(length,3));
	
	snprintf (buffer, 3, "%02X", ifd->firmware);

	if (length > 3)
		memcpy (desc+3, buffer, MIN(length-3,2));
}

BYTE
IFD_Towitoko_GetFirmware (IFD * ifd)
{
	return ifd->firmware;
}

BYTE
IFD_Towitoko_GetSlot (IFD * ifd)
{
	return ifd->slot;
}

unsigned
IFD_Towitoko_GetNumSlots (IFD * ifd)
{
	return 1;
}

unsigned long
IFD_Towitoko_GetClockRate (IFD * ifd)
{
 	return IFD_TOWITOKO_CLOCK_RATE;
}

unsigned long 
IFD_Towitoko_GetMaxBaudrate (IFD * ifd)
{
 	return 115200L;
}

/*
 * Not exported funcions definition
 */


static int IFD_Towitoko_GetReaderInfo (IFD * ifd)
{
	BYTE status[3];
	
	status[0] = IFD_TOWITOKO_MULTICAM;
	status[1] = 0x00;  	
	
	ifd->type = status[0];
	ifd->firmware = status[1];
	
#ifdef DEBUG_IFD
	printf ("IFD: Reader type = %s\n",
	status[0] == IFD_TOWITOKO_CHIPDRIVE_EXT_II ? "Chipdrive Extern II" :
	status[0] == IFD_TOWITOKO_CHIPDRIVE_EXT_I ? "Chipdrive Extern I" :
	status[0] == IFD_TOWITOKO_CHIPDRIVE_INT ? "Chipdrive Intern" :
	status[0] == IFD_TOWITOKO_CHIPDRIVE_MICRO ? "Chipdrive Micro" :
	status[0] == IFD_TOWITOKO_KARTENZWERG_II ? "Kartenzwerg II" :
	status[0] == IFD_TOWITOKO_MULTICAM ? "Multicam" : 
	status[0] == IFD_TOWITOKO_KARTENZWERG ? "Kartenzwerg" : "Unknown");
#endif
	
	return IFD_TOWITOKO_OK;
}


static void IFD_Towitoko_Clear (IFD * ifd)
{
	ifd->io = NULL;
	ifd->slot = 0x00;
	ifd->type = 0x00;
	ifd->firmware = 0x00;
	ifd->status = 0;
}
