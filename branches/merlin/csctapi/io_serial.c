   /*
      io_serial.c
      Serial port input/output functions

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
#include <errno.h>
#ifdef OS_HPUX
#  include <sys/modem.h>
#endif
#include <termios.h>
#include <unistd.h>
#include <sys/stat.h>
#include <fcntl.h>
#ifdef HAVE_POLL
#  include <sys/poll.h>
#else
#  include <sys/signal.h>
#  include <sys/types.h>
#endif
#include <sys/time.h>
#include <sys/ioctl.h>
#include <time.h>
#include "io_serial.h"
#include "mc_global.h"

#ifdef OS_LINUX
#  include <linux/serial.h>
#endif

/*
 * Internal functions declaration
 */

static int IO_Serial_Bitrate_to_Speed(int bitrate);

static int IO_Serial_Bitrate_from_Speed(int speed);

static bool IO_Serial_WaitToRead(int hnd, unsigned delay_ms, unsigned timeout_ms);

static bool IO_Serial_WaitToWrite(IO_Serial * io, unsigned delay_ms, unsigned timeout_ms);

static bool IO_Serial_InitPnP(IO_Serial * io);

static void IO_Serial_Clear(IO_Serial * io);

static bool IO_Serial_GetPropertiesCache(IO_Serial * io, IO_Serial_Properties * props);

static void IO_Serial_SetPropertiesCache(IO_Serial * io, IO_Serial_Properties * props);

static void IO_Serial_ClearPropertiesCache(IO_Serial * io);

static int _in_echo_read = 0;
extern int reader_serial_need_dummy_char;

extern unsigned long reader_serial_bitrate_optimal;
extern unsigned long reader_serial_bitrate_effective;

int fdmc = (-1);

#if defined(TUXBOX) && defined(PPC)
extern int *oscam_sem;

void IO_Serial_Ioctl_Lock(IO_Serial * io, int flag)
{
	if ((io->reader_type != RTYP_DB2COM1) && (io->reader_type != RTYP_DB2COM2))
		return;
	if (!flag)
		*oscam_sem = 0;
	else
		while (*oscam_sem != io->reader_type) {
			while (*oscam_sem)
				usleep(2000);
			*oscam_sem = io->reader_type;
			usleep(1000);
		}
}

static bool IO_Serial_DTR_RTS_dbox2(int mcport, int dtr, int set)
{
	int rc;
	unsigned short msr;
	unsigned short rts_bits[2] = { 0x10, 0x800 };
	unsigned short dtr_bits[2] = { 0x100, 0 };

#  ifdef DEBUG_IO
	printf("IO: multicam.o %s %s\n", dtr ? "dtr" : "rts", set ? "set" : "clear");
	fflush(stdout);
#  endif
	if ((rc = ioctl(fdmc, GET_PCDAT, &msr)) >= 0) {
		if (dtr)	// DTR
		{
			if (dtr_bits[mcport]) {
				if (set)
					msr &= (unsigned short) (~dtr_bits[mcport]);
				else
					msr |= dtr_bits[mcport];
				rc = ioctl(fdmc, SET_PCDAT, &msr);
			} else
				rc = 0;	// Dummy, can't handle using multicam.o
		} else	// RTS
		{
			if (set)
				msr &= (unsigned short) (~rts_bits[mcport]);
			else
				msr |= rts_bits[mcport];
			rc = ioctl(fdmc, SET_PCDAT, &msr);
		}
	}
	return ((rc < 0) ? FALSE : TRUE);
}
#endif

bool IO_Serial_DTR_RTS(IO_Serial * io, int dtr, int set)
{
	unsigned int msr;
	unsigned int mbit;

#if defined(TUXBOX) && defined(PPC)
	if ((io->reader_type == RTYP_DB2COM1) || (io->reader_type == RTYP_DB2COM2))
		return (IO_Serial_DTR_RTS_dbox2(io->reader_type == RTYP_DB2COM2, dtr, set));
#endif

	mbit = (dtr) ? TIOCM_DTR : TIOCM_RTS;
#if defined(TIOCMBIS) && defined(TIOBMBIC)
	if (ioctl(io->fd, set ? TIOCMBIS : TIOCMBIC, &mbit) < 0)
		return FALSE;
#else
	if (ioctl(io->fd, TIOCMGET, &msr) < 0)
		return FALSE;
	if (set)
		msr |= mbit;
	else
		msr &= ~mbit;
	return ((ioctl(io->fd, TIOCMSET, &msr) < 0) ? FALSE : TRUE);
#endif
}

/*
 * Public functions definition
 */

IO_Serial *IO_Serial_New(void)
{
	IO_Serial *io;

	io = (IO_Serial *) malloc(sizeof (IO_Serial));

	if (io != NULL)
		IO_Serial_Clear(io);

	return io;
}

bool IO_Serial_Init(IO_Serial * io, char *device, unsigned long frequency, unsigned short reader_type, bool pnp)
{
#ifdef DEBUG_IO
	printf("IO: Opening serial port %s\n", device);
#endif

	memcpy(io->device, device, sizeof(io->device));
	io->frequency = frequency;
	io->reader_type = reader_type;

#ifdef SCI_DEV
	if (reader_type == RTYP_SCI)
		io->fd = open(io->device, O_RDWR);
	else
#endif

#ifdef OS_MACOSX
		io->fd = open(io->device, O_RDWR | O_NOCTTY | O_NDELAY);
#else
		io->fd = open(io->device, O_RDWR | O_NOCTTY | O_SYNC);
#endif

	if (io->fd < 0)
		return FALSE;

#if defined(TUXBOX) && defined(PPC)
	if ((reader_type == RTYP_DB2COM1) || (reader_type == RTYP_DB2COM2))
		if ((fdmc = open(DEV_MULTICAM, O_RDWR)) < 0) {
			close(io->fd);
			return FALSE;
		}
#endif

	if (reader_type != RTYP_SCI) {
		if (pnp)
			IO_Serial_InitPnP(io);
	}

	return TRUE;
}

bool IO_Serial_GetProperties(IO_Serial * io, IO_Serial_Properties * props)
{
	struct termios currtio;
	speed_t i_speed, o_speed;
	unsigned int mctl;

#ifdef SCI_DEV
	if (io->reader_type == RTYP_SCI)
		return FALSE;
#endif

	if (IO_Serial_GetPropertiesCache(io, props))
		return TRUE;

	if (tcgetattr(io->fd, &currtio) != 0)
		return FALSE;

	o_speed = cfgetospeed(&currtio);
	props->output_bitrate = IO_Serial_Bitrate_from_Speed(o_speed);

	i_speed = cfgetispeed(&currtio);
	props->input_bitrate = IO_Serial_Bitrate_from_Speed(i_speed);

	/* Check for custom bitrate */
#ifdef OS_LINUX
	if (props->input_bitrate == 38400 && props->output_bitrate == 38400) {
		struct serial_struct s;
		bzero (&s, sizeof (s));
		if (ioctl(io->fd, TIOCGSERIAL, &s) >= 0) {
			if ((s.flags & ASYNC_SPD_CUST) != 0 && s.baud_base > 0 && s.custom_divisor > 0) {
				unsigned long effective_bitrate = (unsigned long) s.baud_base / s.custom_divisor;
				props->input_bitrate = effective_bitrate;
				props->output_bitrate = effective_bitrate;
			}
		}
	}
#endif

	switch (currtio.c_cflag & CSIZE) {
		case CS5:
			props->bits = 5;
			break;
		case CS6:
			props->bits = 6;
			break;
		case CS7:
			props->bits = 7;
			break;
		case CS8:
			props->bits = 8;
			break;
	}

	if (((currtio.c_cflag) & PARENB) == PARENB) {
		if (((currtio.c_cflag) & PARODD) == PARODD)
			props->parity = IO_SERIAL_PARITY_ODD;
		else
			props->parity = IO_SERIAL_PARITY_EVEN;
	} else {
		props->parity = IO_SERIAL_PARITY_NONE;
	}

	if (((currtio.c_cflag) & CSTOPB) == CSTOPB)
		props->stopbits = 2;
	else
		props->stopbits = 1;

	if (ioctl(io->fd, TIOCMGET, &mctl) < 0)
		return FALSE;

	props->dtr = ((mctl & TIOCM_DTR) ? IO_SERIAL_HIGH : IO_SERIAL_LOW);
	props->rts = ((mctl & TIOCM_RTS) ? IO_SERIAL_HIGH : IO_SERIAL_LOW);

	IO_Serial_SetPropertiesCache(io, props);

#ifdef DEBUG_IO
	printf("IO: Getting properties: %ld bps; %d bits/byte; %s parity; %d stopbits; dtr=%d; rts=%d\n", props->input_bitrate, props->bits, props->parity == IO_SERIAL_PARITY_EVEN ? "Even" : props->parity == IO_SERIAL_PARITY_ODD ? "Odd" : "None", props->stopbits, props->dtr, props->rts);
#endif

	return TRUE;
}

bool IO_Serial_SetProperties(IO_Serial * io, IO_Serial_Properties * props)
{
	struct termios newtio;

#ifdef SCI_DEV
	if (io->reader_type == RTYP_SCI)
		return FALSE;
#endif

	memset(&newtio, 0, sizeof (newtio));

	/* Save the optimal bitrate value for OScam */
	reader_serial_bitrate_optimal = props->output_bitrate;

	/* Set the bitrate */
#ifdef DEBUG_IO
	printf("IO: Optimal bitrate should be = %lu\n", props->output_bitrate);
#endif
#ifdef OS_LINUX
	/* Check if the bitrate is not a standard value */
	if (props->output_bitrate != props->input_bitrate || (props->output_bitrate == 230400 || props->output_bitrate == 115200 || props->output_bitrate == 57600 || props->output_bitrate == 38400 || props->output_bitrate == 19200 || props->output_bitrate == 9600 || props->output_bitrate == 4800 || props->output_bitrate == 2400 || props->output_bitrate == 1800 || props->output_bitrate == 1200 || props->output_bitrate == 600 || props->output_bitrate == 300 || props->output_bitrate == 200 || props->output_bitrate == 150 || props->output_bitrate == 134 || props->output_bitrate == 110 || props->output_bitrate == 75 || props->output_bitrate == 50 || props->output_bitrate == 0)) {
#  ifdef DEBUG_IO
		printf("IO: Using standard bitrate of %lu\n", props->output_bitrate);
#  endif
#endif
		/* Standard bitrate */
		int output_speed = IO_Serial_Bitrate_to_Speed(props->output_bitrate);
		int input_speed = IO_Serial_Bitrate_to_Speed(props->input_bitrate);
		cfsetospeed(&newtio, output_speed);
		cfsetispeed(&newtio, input_speed);

		/* Save the effective bitrate value for OScam */
		reader_serial_bitrate_effective = IO_Serial_Bitrate_from_Speed(output_speed);
#ifdef OS_LINUX
	} else {
		/* Special bitrate : these structures are only available on linux as fas as we know so limit this code to OS_LINUX */
		unsigned long standard_bitrate = props->output_bitrate;
		unsigned long effective_bitrate = IO_Serial_Bitrate_from_Speed(IO_Serial_Bitrate_to_Speed(standard_bitrate));

		struct serial_struct s;
		bzero (&s, sizeof (s));
		if (ioctl(io->fd, TIOCGSERIAL, &s) >= 0) {
			unsigned long wanted_bitrate = props->output_bitrate;
			unsigned long custom_divisor = ((unsigned long) s.baud_base + (wanted_bitrate / 2)) / wanted_bitrate;
			/* Check if custom_divisor is greater than zero */
			if (custom_divisor > 0) {
				effective_bitrate = (unsigned long) s.baud_base / custom_divisor;

				/* Check if a custom_divisor is needed */
				if (effective_bitrate == 230400 || effective_bitrate == 115200 || effective_bitrate == 57600 || effective_bitrate == 38400 || effective_bitrate == 19200 || effective_bitrate == 9600 || effective_bitrate == 4800 || effective_bitrate == 2400 || effective_bitrate == 1800 || effective_bitrate == 1200 || effective_bitrate == 600 || effective_bitrate == 300 || effective_bitrate == 200 || effective_bitrate == 150 || effective_bitrate == 134 || effective_bitrate == 110 || effective_bitrate == 75 || effective_bitrate == 50 || effective_bitrate == 0) {
					/* Use standard bitrate value */
					if ((s.flags & ASYNC_SPD_CUST) != 0) {
						s.flags |= ASYNC_SPD_MASK;
						s.flags &= ~ASYNC_SPD_CUST;
						ioctl(io->fd, TIOCSSERIAL, &s);
					}
					standard_bitrate = effective_bitrate;
# ifdef DEBUG_IO
					printf("IO: Using standard bitrate of %lu (baud_base too small)\n", standard_bitrate);
# endif
				} else {
					/* Use custom divisor */
					s.custom_divisor = custom_divisor;
					s.flags &= ~ASYNC_SPD_MASK;
					s.flags |= ASYNC_SPD_CUST;
					if (ioctl(io->fd, TIOCSSERIAL, &s) >= 0) {
						standard_bitrate = 38400;
					}
# ifdef DEBUG_IO
					printf("IO: Using special bitrate of = %lu (%+.2f%% off)\n", effective_bitrate, ((double) (effective_bitrate - wanted_bitrate)) / wanted_bitrate * 100);
# endif
				}
			}
		}

		/* Save the effective bitrate value for OScam */
		reader_serial_bitrate_effective = effective_bitrate;

		/* Set the standard bitrate value */
		cfsetospeed(&newtio, IO_Serial_Bitrate_to_Speed(standard_bitrate));
		cfsetispeed(&newtio, IO_Serial_Bitrate_to_Speed(standard_bitrate));
	}
#endif

	/* Set the character size */
	switch (props->bits) {
		case 5:
			newtio.c_cflag |= CS5;
			break;

		case 6:
			newtio.c_cflag |= CS6;
			break;

		case 7:
			newtio.c_cflag |= CS7;
			break;

		case 8:
			newtio.c_cflag |= CS8;
			break;
	}

	/* Set the parity */
	switch (props->parity) {
		case IO_SERIAL_PARITY_ODD:
			newtio.c_cflag |= PARENB;
			newtio.c_cflag |= PARODD;
			break;

		case IO_SERIAL_PARITY_EVEN:
			newtio.c_cflag |= PARENB;
			newtio.c_cflag &= ~PARODD;
			break;

		case IO_SERIAL_PARITY_NONE:
			newtio.c_cflag &= ~PARENB;
			break;
	}

	/* Set the number of stop bits */
	switch (props->stopbits) {
		case 1:
			newtio.c_cflag &= (~CSTOPB);
			break;
		case 2:
			newtio.c_cflag |= CSTOPB;
			break;
	}

	/* Selects raw (non-canonical) input and output */
	newtio.c_lflag &= ~(ICANON | ECHO | ECHOE | ISIG);
	newtio.c_oflag &= ~OPOST;
#if 1
	newtio.c_iflag |= IGNPAR;
	/* Ignore parity errors!!! Windows driver does so why shouldn't I? */
#endif
	/* Enable receiber, hang on close, ignore control line */
	newtio.c_cflag |= CREAD | HUPCL | CLOCAL;

	/* Read 1 byte minimun, no timeout specified */
	newtio.c_cc[VMIN] = 1;
	newtio.c_cc[VTIME] = 0;

//      tcdrain(io->fd);
	if (tcsetattr(io->fd, TCSANOW, &newtio) < 0)
		return FALSE;
//      tcflush(io->fd, TCIOFLUSH);
//      if (tcsetattr (io->fd, TCSAFLUSH, &newtio) < 0)
//              return FALSE;

	IO_Serial_Ioctl_Lock(io, 1);
	IO_Serial_DTR_RTS(io, 0, props->rts == IO_SERIAL_HIGH);
	IO_Serial_DTR_RTS(io, 1, props->dtr == IO_SERIAL_HIGH);
	IO_Serial_Ioctl_Lock(io, 0);

	IO_Serial_SetPropertiesCache(io, props);

#ifdef DEBUG_IO
	printf("IO: Setting properties: device=%s, %ld bps; %d bits/byte; %s parity; %d stopbits; dtr=%d; rts=%d\n", io->device, props->input_bitrate, props->bits, props->parity == IO_SERIAL_PARITY_EVEN ? "Even" : props->parity == IO_SERIAL_PARITY_ODD ? "Odd" : "None", props->stopbits, props->dtr, props->rts);
#endif
	return TRUE;
}

void IO_Serial_Flush(IO_Serial * io)
{
	BYTE b;

	while (IO_Serial_Read(io, 1000, 1, &b));
}


void IO_Serial_GetPnPId(IO_Serial * io, BYTE * pnp_id, unsigned *length)
{
	(*length) = io->PnP_id_size;
	memcpy(pnp_id, io->PnP_id, io->PnP_id_size);
}

bool IO_Serial_Read(IO_Serial * io, unsigned timeout, unsigned size, BYTE * data)
{
	BYTE c;
	int count = 0;


	if ((io->reader_type != RTYP_SCI) && (io->wr > 0)) {
		BYTE buf[256];
		int n = io->wr;

		io->wr = 0;

		if (!IO_Serial_Read(io, timeout, n, buf)) {
			return FALSE;
		}
	}
#ifdef DEBUG_IO
	printf("IO: Receiving: ");
	fflush(stdout);
#endif
	for (count = 0; count < size * (_in_echo_read ? (1 + reader_serial_need_dummy_char) : 1); count++) {
		if (IO_Serial_WaitToRead(io->fd, 0, timeout)) {
			if (read(io->fd, &c, 1) != 1) {
#ifdef DEBUG_IO
				printf("ERROR\n");
				fflush(stdout);
#endif
				return FALSE;
			}
			data[_in_echo_read ? count / (1 + reader_serial_need_dummy_char) : count] = c;

#ifdef DEBUG_IO
			printf("%X ", c);
			fflush(stdout);
#endif
		} else {
#ifdef DEBUG_IO
			printf("TIMEOUT\n");
			fflush(stdout);
#endif
			tcflush(io->fd, TCIFLUSH);
			return FALSE;
		}
	}

	_in_echo_read = 0;

#ifdef DEBUG_IO
	printf("\n");
	fflush(stdout);
#endif

	return TRUE;
}

bool IO_Serial_Write(IO_Serial * io, unsigned delay, unsigned size, BYTE * data)
{
	unsigned count, to_send;
	BYTE data_w[512];
	int i_w;

#ifdef DEBUG_IO
	unsigned i;

	printf("IO: Sending: ");
	fflush(stdout);
#endif
	/* Discard input data from previous commands */
//      tcflush (io->fd, TCIFLUSH);

	for (count = 0; count < size; count += to_send) {
//              if (io->reader_type == RTYP_SCI)
//                      to_send = 1;
//              else
		to_send = (delay ? 1 : size);

		if (IO_Serial_WaitToWrite(io, delay, 1000)) {
			for (i_w = 0; i_w < to_send; i_w++) {
				data_w[(1 + reader_serial_need_dummy_char) * i_w] = data[count + i_w];
				if (reader_serial_need_dummy_char) {
					data_w[2 * i_w + 1] = 0x00;
				}
			}
			unsigned int u = write(io->fd, data_w, (1 + reader_serial_need_dummy_char) * to_send);

			_in_echo_read = 1;
			if (u != (1 + reader_serial_need_dummy_char) * to_send) {
#ifdef DEBUG_IO
				printf("ERROR\n");
				fflush(stdout);
#endif
				if (io->reader_type != RTYP_SCI)
					io->wr += u;
				return FALSE;
			}

			if (io->reader_type != RTYP_SCI)
				io->wr += to_send;

#ifdef DEBUG_IO
			for (i = 0; i < (1 + reader_serial_need_dummy_char) * to_send; i++)
				printf("%X ", data_w[count + i]);
			fflush(stdout);
#endif
		} else {
#ifdef DEBUG_IO
			printf("TIMEOUT\n");
			fflush(stdout);
#endif
//                      tcflush (io->fd, TCIFLUSH);
			return FALSE;
		}
	}

#ifdef DEBUG_IO
	printf("\n");
	fflush(stdout);
#endif

	return TRUE;
}

bool IO_Serial_Close(IO_Serial * io)
{
#ifdef DEBUG_IO
	printf("IO: Clossing serial port %s\n", io->device);
#endif

#if defined(TUXBOX) && defined(PPC)
	close(fdmc);
#endif
	if (close(io->fd) != 0)
		return FALSE;

	IO_Serial_ClearPropertiesCache(io);
	IO_Serial_Clear(io);

	return TRUE;
}

void IO_Serial_Delete(IO_Serial * io)
{
	if (io->props != NULL)
		free(io->props);

	free(io);
}

/*
 * Internal functions definition
 */

static int IO_Serial_Bitrate_to_Speed(int bitrate)
{
	static const struct BaudRates { int real; speed_t apival; } BaudRateTab[] = {
#ifdef B50
		{     50, B50     },
#endif
#ifdef B75
		{     75, B75     },
#endif
#ifdef B110
		{    110, B110    },
#endif
#ifdef B134
		{    134, B134    },
#endif
#ifdef B150
		{    150, B150    },
#endif
#ifdef B200
		{    200, B200    },
#endif
#ifdef B300
		{    300, B300    },
#endif
#ifdef B600
		{    600, B600    },
#endif
#ifdef B1200
		{   1200, B1200   },
#endif
#ifdef B1800
		{   1800, B1800   },
#endif
#ifdef B2400
		{   2400, B2400   },
#endif
#ifdef B4800
		{   4800, B4800   },
#endif
#ifdef B9600
		{   9600, B9600   },
#endif
#ifdef B19200
		{  19200, B19200  },
#endif
#ifdef B38400
		{  38400, B38400  },
#endif
#ifdef B57600
		{  57600, B57600  },
#endif
#ifdef B115200
		{ 115200, B115200 },
#endif
#ifdef B230400
		{ 230400, B230400 },
#endif
	};

	speed_t result = B0;
	int i, d_result = bitrate;

	for ( i = 0 ; i < (int)(sizeof(BaudRateTab)/sizeof(struct BaudRates)) ; i++) {
	 	int d = abs(BaudRateTab[i].real - bitrate);
		if (d < d_result) {
			result = BaudRateTab[i].apival;
			d_result = d;
		} else {
			break;
		}
	}

	return result;
}

static int IO_Serial_Bitrate_from_Speed(int speed)
{
	switch (speed) {
#ifdef B0
		case B0:
			return 0;
#endif
#ifdef B50
		case B50:
			return 50;
#endif
#ifdef B75
		case B75:
			return 75;
#endif
#ifdef B110
		case B110:
			return 110;
#endif
#ifdef B134
		case B134:
			return 134;
#endif
#ifdef B150
		case B150:
			return 150;
#endif
#ifdef B200
		case B200:
			return 200;
#endif
#ifdef B300
		case B300:
			return 300;
#endif
#ifdef B600
		case B600:
			return 600;
#endif
#ifdef B1200
		case B1200:
			return 1200;
#endif
#ifdef B1800
		case B1800:
			return 1800;
#endif
#ifdef B2400
		case B2400:
			return 2400;
#endif
#ifdef B4800
		case B4800:
			return 4800;
#endif
#ifdef B9600
		case B9600:
			return 9600;
#endif
#ifdef B19200
		case B19200:
			return 19200;
#endif
#ifdef B38400
		case B38400:
			return 38400;
#endif
#ifdef B57600
		case B57600:
			return 57600;
#endif
#ifdef B115200
		case B115200:
			return 115200;
#endif
#ifdef B230400
		case B230400:
			return 230400;
#endif
		default:
			return 1200;
	}

	return 0;	/* Should never get here */
}

static bool IO_Serial_WaitToRead(int hnd, unsigned delay_ms, unsigned timeout_ms)
{
	fd_set rfds;
	fd_set erfds;
	struct timeval tv;
	int select_ret;
	int in_fd;

	if (delay_ms > 0) {
#ifdef HAVE_NANOSLEEP
		struct timespec req_ts;

		req_ts.tv_sec = delay_ms / 1000;
		req_ts.tv_nsec = (delay_ms % 1000) * 1000000L;
		nanosleep(&req_ts, NULL);
#else
		usleep(delay_ms * 1000L);
#endif
	}

	in_fd = hnd;

	FD_ZERO(&rfds);
	FD_SET(in_fd, &rfds);

	FD_ZERO(&erfds);
	FD_SET(in_fd, &erfds);

	tv.tv_sec = timeout_ms / 1000;
	tv.tv_usec = (timeout_ms % 1000) * 1000L;
	select_ret = select(in_fd + 1, &rfds, NULL, &erfds, &tv);
	if (select_ret == -1) {
		printf("select_ret=%i\n", select_ret);
		printf("errno =%d\n", errno);
		fflush(stdout);
		return (FALSE);
	}

	if (FD_ISSET(in_fd, &erfds)) {
		printf("fd is in error fds\n");
		printf("errno =%d\n", errno);
		fflush(stdout);
		return (FALSE);
	}

	return (FD_ISSET(in_fd, &rfds));
}

static bool IO_Serial_WaitToWrite(IO_Serial * io, unsigned delay_ms, unsigned timeout_ms)
{
	fd_set wfds;
	fd_set ewfds;
	struct timeval tv;
	int select_ret;
	int out_fd;

#ifdef SCI_DEV
	if (io->reader_type == RTYP_SCI)
		return TRUE;
#endif

	if (delay_ms > 0) {
#ifdef HAVE_NANOSLEEP
		struct timespec req_ts;

		req_ts.tv_sec = delay_ms / 1000;
		req_ts.tv_nsec = (delay_ms % 1000) * 1000000L;
		nanosleep(&req_ts, NULL);
#else
		usleep(delay_ms * 1000L);
#endif
	}

	out_fd = io->fd;

	FD_ZERO(&wfds);
	FD_SET(out_fd, &wfds);

	FD_ZERO(&ewfds);
	FD_SET(out_fd, &ewfds);

	tv.tv_sec = timeout_ms / 1000L;
	tv.tv_usec = (timeout_ms % 1000) * 1000L;

	select_ret = select(out_fd + 1, NULL, &wfds, &ewfds, &tv);

	if (select_ret == -1) {
		printf("select_ret=%d\n", select_ret);
		printf("errno =%d\n", errno);
		fflush(stdout);
		return (FALSE);
	}

	if (FD_ISSET(out_fd, &ewfds)) {
		printf("fd is in ewfds\n");
		printf("errno =%d\n", errno);
		fflush(stdout);
		return (FALSE);
	}

	return (FD_ISSET(out_fd, &wfds));

}

static void IO_Serial_Clear(IO_Serial * io)
{
	io->fd = -1;
	io->props = NULL;
	memset(io->PnP_id, 0, IO_SERIAL_PNPID_SIZE);
	io->PnP_id_size = 0;
	io->wr = 0;
}

static void IO_Serial_SetPropertiesCache(IO_Serial * io, IO_Serial_Properties * props)
{
#ifdef SCI_DEV
	if (io->reader_type == RTYP_SCI)
		return;
#endif

	if (io->props == NULL)
		io->props = (IO_Serial_Properties *) malloc(sizeof (IO_Serial_Properties));
#ifdef DEBUG_IO
	printf("IO: Catching properties\n");
#endif

	memcpy(io->props, props, sizeof (IO_Serial_Properties));
}

static bool IO_Serial_GetPropertiesCache(IO_Serial * io, IO_Serial_Properties * props)
{
	if (io->props != NULL) {
		memcpy(props, io->props, sizeof (IO_Serial_Properties));
#ifdef DEBUG_IO
               printf("IO: Getting properties (catched): %ld bps; %d bits/byte; %s parity; %d stopbits; dtr=%d; rts=%d\n", props->input_bitrate, props->bits, props->parity == IO_SERIAL_PARITY_EVEN ? "Even" : props->parity == IO_SERIAL_PARITY_ODD ? "Odd" : "None", props->stopbits, props->dtr, props->rts);
#endif
		return TRUE;
	}

	return FALSE;
}

static void IO_Serial_ClearPropertiesCache(IO_Serial * io)
{
#ifdef DEBUG_IO
	printf("IO: Clearing properties cache\n");
#endif
	if (io->props != NULL) {
		free(io->props);
		io->props = NULL;
	}
}

static bool IO_Serial_InitPnP(IO_Serial * io)
{
	IO_Serial_Properties props;
	int i = 0;

	props.input_bitrate = io->frequency / 372;
	props.output_bitrate = io->frequency / 372;
	props.parity = IO_SERIAL_PARITY_NONE;
	props.bits = 8;
	props.stopbits = 1;
	props.dtr = IO_SERIAL_HIGH;
	if (io->reader_type == RTYP_PHOENIX) {
		props.rts = IO_SERIAL_LOW;
	} else if (io->reader_type == RTYP_SMARTMOUSE) {
		props.rts = IO_SERIAL_HIGH;
	} else {
		props.rts = IO_SERIAL_LOW;
	}

	if (!IO_Serial_SetProperties(io, &props))
		return FALSE;

	while ((i < IO_SERIAL_PNPID_SIZE) && IO_Serial_Read(io, 200, 1, &(io->PnP_id[i])))
		i++;

	io->PnP_id_size = i;
	return TRUE;
}
