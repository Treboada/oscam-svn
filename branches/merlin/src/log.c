#include "globals.h"
#include "log.h"

#include "ac.h"
#include "oscam.h"
#include "monitor.h"
#include "simples.h"
#include "network.h"

#include <stdarg.h>
#include <syslog.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

char logfile[256] = CS_LOGFILE;

static FILE *fp = (FILE *) 0;
static FILE *fps = (FILE *) 0;
static int use_syslog = 0;
static int use_stdout = 0;

#ifdef CS_ANTICASC
FILE *fpa = (FILE *) 0;
int use_ac_log = 0;
#endif

extern int shmsize;
extern int shmid;

static void log_config()
{
	uchar buf[2048];

	if (cfg->nice != 99)
		sprintf((char *) buf, ", nice=%d", cfg->nice);
	else
		buf[0] = '\0';
	log_normal("version=%s, system=%s%s", CS_VERSION, oscam_platform((char *) buf + 64), buf);
	log_normal("max. clients=%d, client max. idle=%d sec",
#ifdef CS_ANTICASC
	       CS_MAXPID - 3, cfg->cmaxidle);
#else
	       CS_MAXPID - 2, cfg->cmaxidle);
#endif
	if (cfg->max_log_size)
		sprintf((char *) buf, "%d Kb", cfg->max_log_size);
	else
		strcpy((char *) buf, "unlimited");
	log_normal("max. logsize=%s", buf);
	log_normal("client timeout=%lu ms, fallback timeout=%lu ms, cache delay=%d ms", cfg->ctimeout, cfg->ftimeout, cfg->delay);
#ifdef CS_NOSHM
	log_normal("shared memory initialized (size=%d, fd=%d)", shmsize, shmid);
#else
	log_normal("shared memory initialized (size=%d, id=%d)", shmsize, shmid);
#endif
}

static void log_switch(char *file, FILE ** f, int (*pfinit) (char *))
{
	if (cfg->max_log_size && mcl) {
		struct stat stlog;

		if (stat(file, &stlog) != 0) {
			fprintf(stderr, "stat('%s',..) failed (errno=%d)\n", file, errno);
			return;
		}

		if (stlog.st_size >= cfg->max_log_size * 1024) {
			int rc;
			char prev_log[128];

			sprintf(prev_log, "%s-prev", file);
			fprintf(*f, "switch log file\n");
			fflush(*f);
			fclose(*f);
			*f = (FILE *) 0;
			rc = rename(file, prev_log);
			if (rc != 0) {
				fprintf(stderr, "rename(%s, %s) failed (errno=%d)\n", file, prev_log, errno);
			} else if (pfinit(file)) {
				oscam_exit(0);
			}
		}
	}
}

void log_write(char *txt)
{
#ifdef CS_ANTICASC
	if (use_ac_log && fpa) {
		log_switch(cfg->ac_logfile, &fpa, ac_init_log);
		fprintf(fpa, "%s", txt);
		fflush(fpa);
	} else
#endif
	if (fp || use_stdout) {
		if (!use_stdout && !use_syslog) {
			log_switch(logfile, &fp, log_init);
		}
		fprintf(fp, "%s", txt);
		fflush(fp);
	}
}

int log_init(char *file)
{
	static char *head = ">> OSCam <<  cardserver started";

	if (!strcmp(file, "stdout")) {
		use_stdout = 1;
		fp = stdout;
		log_normal(head);

		return 0;
	}

	if (strcmp(file, "syslog")) {
		if (!fp) {
			if ((fp = fopen(file, "a+")) <= (FILE *) 0) {
				fp = (FILE *) 0;
				fprintf(stderr, "couldn't open logfile: %s (errno %d)\n", file, errno);
			} else {
				time_t t;
				char line[80];

				memset(line, '-', sizeof (line));
				line[(sizeof (line) / sizeof (char)) - 1] = '\0';
				time(&t);
				fprintf(fp, "\n%s\n>> OSCam <<  cardserver started at %s%s\n", line, ctime(&t), line);
				log_config();
			}
		}

		return (fp <= (FILE *) 0);
	} else {
		openlog("oscam", LOG_NDELAY, LOG_DAEMON);
		use_syslog = 1;
		log_normal(head);

		return (0);
	}
}

static char *log_get_header(int m, char *txt)
{
	if (m) {
		sprintf(txt, "%6d ", getpid());
		if (cs_idx) {
			switch (client[cs_idx].typ) {
				case 'r':
				case 'p':
					sprintf(txt + 7, "%c%02d ", client[cs_idx].typ, cs_idx - 1);
					break;
				case 'm':
				case 'c':
					sprintf(txt + 7, "%c%02d ", client[cs_idx].typ, cs_idx - cdiff);
					break;
#ifdef CS_ANTICASC
				case 'a':
#endif
				case 'l':
				case 'n':
					sprintf(txt + 7, "%c   ", client[cs_idx].typ);
					break;
			}
		} else {
			strcpy(txt + 7, "s   ");
		}
	} else {
		sprintf(txt, "%-11.11s", "");
	}

	return txt;
}

static void log_store_entry(char *txt)
{
#ifdef CS_LOGHISTORY
	char *ptr;

	ptr = (char *) (loghist + (*loghistidx * CS_LOGHISTSIZE));
	ptr[0] = '\1';	// make username unusable
	ptr[1] = '\0';
	if ((client[cs_idx].typ == 'c') || (client[cs_idx].typ == 'm'))
		strncpy(ptr, client[cs_idx].usr, 31);
	strncpy(ptr + 32, txt, CS_LOGHISTSIZE - 33);
	*loghistidx = (*loghistidx + 1) % CS_MAXLOGHIST;
#endif
}

static void log_write_to_log(int flag, char *txt)
{
	int i;
	time_t t;
	struct tm *lt;
	char buf[512], sbuf[16];

	log_get_header(flag, sbuf);
	memcpy(txt, sbuf, 11);

	if (use_syslog && !use_ac_log) {	// system-logfile
		syslog(LOG_INFO, "%s", txt);
	} else {
		time(&t);
		lt = localtime(&t);
		sprintf(buf, "[LOG000]%4d/%02d/%02d %2d:%02d:%02d %s\n", lt->tm_year + 1900, lt->tm_mon + 1, lt->tm_mday, lt->tm_hour, lt->tm_min, lt->tm_sec, txt);

/*
#ifdef CS_ANTICASC
		if (fp || fpa || use_stdout) {		// logfile
#else
		if (fp || use_stdout) {			// logfile
#endif
*/
		if ((*log_fd) && (client[cs_idx].typ != 'l') && (client[cs_idx].typ != 'a')) {
			oscam_write_to_pipe(*log_fd, PIP_ID_LOG, (uchar *) buf + 8, strlen(buf + 8));
		} else {
			log_write(buf + 8);
		}
//		}
	}
	log_store_entry(buf);

	for (i = 0; i < CS_MAXPID; i++)	// monitor-clients
	{
		if ((client[i].pid) && (client[i].log)) {
			if (client[i].monlvl < 2) {
				if ((client[cs_idx].typ != 'c') && (client[cs_idx].typ != 'm'))
					continue;
				if (strcmp(client[cs_idx].usr, client[i].usr))
					continue;
			}
			sprintf(sbuf, "%03d", client[i].logcounter);
			client[i].logcounter = (client[i].logcounter + 1) % 1000;
			memcpy(buf + 4, sbuf, 3);
			monitor_send_idx(i, buf);
		}
	}
}

void log_normal(char *fmt, ...)
{
	char txt[256 + 11];

	va_list params;

	va_start(params, fmt);
	vsprintf(txt + 11, fmt, params);
	va_end(params);
	log_write_to_log(1, txt);
}

void log_close()
{
	if (use_stdout || use_syslog || !fp)
		return;
	fclose(fp);
	fp = (FILE *) 0;
}

void log_debug(char *fmt, ...)
{
	char txt[256];

//	log_normal("log_debug called, cs_ptyp=%d, cs_dblevel=%d, %d", cs_ptyp, client[cs_idx].dbglvl ,cs_ptyp & client[cs_idx].dbglvl);

	if ((cs_ptyp & client[cs_idx].dbglvl) == cs_ptyp) {
		va_list params;

		va_start(params, fmt);
		vsprintf(txt + 11, fmt, params);
		va_end(params);
		log_write_to_log(1, txt);
	}
}

void log_dump(uchar * buf, int n, char *fmt, ...)
{
	int i;
	char txt[512];

	if (fmt)
		log_normal(fmt);

	for (i = 0; i < n; i += 16) {
		sprintf(txt + 11, "%s", cs_hexdump(1, buf + i, (n - i > 16) ? 16 : n - i));
		log_write_to_log(i == 0, txt);
	}
}

void log_ddump(uchar * buf, int n, char *fmt, ...)
{
	int i;
	char txt[512];

	if (((cs_ptyp & client[cs_idx].dbglvl) == cs_ptyp) && (fmt)) {
		va_list params;

		va_start(params, fmt);
		vsprintf(txt + 11, fmt, params);
		va_end(params);
		log_write_to_log(1, txt);
	}
	if (((cs_ptyp | D_DUMP) & client[cs_idx].dbglvl) == (cs_ptyp | D_DUMP)) {
		for (i = 0; i < n; i += 16) {
			sprintf(txt + 11, "%s", cs_hexdump(1, buf + i, (n - i > 16) ? 16 : n - i));
			log_write_to_log(i == 0, txt);
		}
	}
}

int log_init_statistics(char *file)
{
	if ((!fps) && (file[0])) {
		if ((fps = fopen(file, "a")) <= (FILE *) 0) {
			fps = (FILE *) 0;
			log_normal("couldn't open statistics file: %s", file);
		}
	}

	return (fps <= (FILE *) 0);
}

void log_statistics(int idx)
{
	time_t t;
	struct tm *lt;

	if (fps) {
		float cwps;

		log_switch(cfg->usrfile, &fps, log_init_statistics);
		time(&t);
		lt = localtime(&t);
		if (client[idx].cwfound + client[idx].cwnot > 0) {
			cwps = client[idx].last - client[idx].login;
			cwps /= client[idx].cwfound + client[idx].cwnot;
		} else
			cwps = 0;

		fprintf(fps, "%02d.%02d.%02d %02d:%02d:%02d %3.1f %s %s %d %d %d %d %ld %ld %s\n",
			lt->tm_mday, lt->tm_mon + 1, lt->tm_year % 100,
			lt->tm_hour, lt->tm_min, lt->tm_sec, cwps, client[idx].usr[0] ? client[idx].usr : "-", network_inet_ntoa(client[idx].ip), client[idx].port, client[idx].cwfound, client[idx].cwcache, client[idx].cwnot, client[idx].login, client[idx].last, ph[client[idx].ctyp].desc);
		fflush(fps);
	}
}

void log_cwl_write_to_file(ECM_REQUEST *er)
{
	/* This function writes the current CW from ECM struct to a cwl file.
	   The filename is re-calculated and file re-opened every time.
	   This will consume a bit cpu time, but nothing has to be stored between 
	   each call. If not file exists, a header is prepended */

	FILE *pfCWL;
	char srvname[23];

	/* %s / %s   _I  %04X  _  %s  .cwl  */
	char buf[sizeof (cfg->cwlogdir) + 1 + 6 + 2 + 4 + 1 + sizeof (srvname) + 5];
	char date[7];
	unsigned char i, parity, writeheader = 0;
	time_t t;
	struct tm *timeinfo;
	struct s_srvid *this;

	if (cfg->cwlogdir[0]) {	/* CWL logging only if cwlogdir is set in config */
		/* search service name for that id and change characters 
		   causing problems in file name */
		srvname[0] = 0;
		for (this = cfg->srvid; this; this = this->next) {
			if (this->srvid == er->srvid) {
				strncpy(srvname, this->name, sizeof (srvname));
				srvname[sizeof (srvname) - 1] = 0;
				for (i = 0; srvname[i]; i++)
					if (srvname[i] == ' ')
						srvname[i] = '_';
				break;
			}
		}

		/* calc log file name */
		time(&t);
		timeinfo = localtime(&t);
		strftime(date, sizeof (date), "%y%m%d", timeinfo);
		sprintf(buf, "%s/%s_I%04X_%s.cwl", cfg->cwlogdir, date, er->srvid, srvname);

		if ((pfCWL = fopen(buf, "r")) == NULL) {
			/* open failed, assuming file does not exist, yet */
			writeheader = 1;
		} else {
			/* we need to close the file if it was opened correctly */
			fclose(pfCWL);
		}

		if ((pfCWL = fopen(buf, "a+")) == NULL) {
			/* maybe this fails because the subdir does not exist. Is there a common function to create it? */
			/* for the moment do not print to log on every ecm 
			   log_normal(""error opening cw logfile for writing: %s (errno %d)", buf, errno); */
			return;
		}
		if (writeheader) {
			/* no global macro for cardserver name :( */
			fprintf(pfCWL, "# OSCam cardserver v%s - http://streamboard.gmc.to:8001/oscam/wiki\n", CS_VERSION);
			fprintf(pfCWL, "# control word log file for use with tsdec offline decrypter\n");
			strftime(buf, sizeof (buf), "DATE %Y-%m-%d, TIME %H:%M:%S, TZ %Z\n", timeinfo);
			fprintf(pfCWL, "# %s", buf);
			fprintf(pfCWL, "# CAID 0x%04X, SID 0x%04X, SERVICE \"%s\"\n", er->caid, er->srvid, srvname);
		}

		parity = er->ecm[0] & 1;
		fprintf(pfCWL, "%d ", parity);
		for (i = parity * 8; i < 8 + parity * 8; i++)
			fprintf(pfCWL, "%02X ", er->cw[i]);
		/* better use incoming time er->tps rather than current time? */
		strftime(buf, sizeof (buf), "%H:%M:%S\n", timeinfo);
		fprintf(pfCWL, "# %s", buf);
		fflush(pfCWL);
		fclose(pfCWL);
	}	/* if (cfg->pidfile[0]) */
}

