//oscam-log runs in master thread so doesnt need to be threadsafe
#include "globals.h"
#include <syslog.h>
#include <stdlib.h>

int number_of_chars_printed = 0;

static FILE *fp=(FILE *)0;
static FILE *fps=(FILE *)0;
static int use_syslog=0;
static int use_stdout=0;

#ifdef CS_ANTICASC
FILE *fpa=(FILE *)0;
#endif

static void switch_log(char* file, FILE **f, int (*pfinit)(char*))
{
	if( cfg->max_log_size )
	{
		struct stat stlog;
		if( stat(file, &stlog)!=0 )
		{
			fprintf(stderr, "stat('%s',..) failed (errno=%d)\n", file, errno);
			return;
		}

		if( stlog.st_size >= cfg->max_log_size*1024 ) {
			int rc;
			char prev_log[128];
			sprintf(prev_log, "%s-prev", file);
			fprintf(*f, "switch log file\n");
			fflush(*f);
			fclose(*f);
			*f = (FILE *)0;
			rc = rename(file, prev_log);
			if( rc!=0 ) {
				fprintf(stderr, "rename(%s, %s) failed (errno=%d)\n",
						file, prev_log, errno);
			}
			else if( pfinit(file))
				cs_exit(0);
		}
	}
}

void cs_write_log(char *txt)
{
#ifdef CS_ANTICASC
	if( cur_client()->typ == 'a' && fpa ) {
		switch_log(cfg->ac_logfile, &fpa, ac_init_log);
		fprintf(fpa, "%s", txt);
		fflush(fpa);
	}
	else
#endif
		// filter out entries with leading 's' and forward to statistics
		if(txt[0] == 's') {
			if (fps) {
				switch_log(cfg->usrfile, &fps, cs_init_statistics);
				fprintf(fps, "%s", txt + 1); // remove the leading 's' and write to file
				fflush(fps);
			}
		} else {
			if (fp || use_stdout) {
				if( !use_stdout && !use_syslog)
					switch_log(cfg->logfile, &fp, cs_init_log);
				if (!cfg->disablelog){
					fprintf(fp, "%s", txt);
					fflush(fp);
				}
			}
		}
}

int cs_init_log(char *file)
{
	static char *head = ">> OSCam <<  cardserver started version " CS_VERSION ", build #" CS_SVN_VERSION " (" CS_OSTYPE ")";

	if (!strcmp(file, "stdout")) {
		use_stdout = 1;
		fp = stdout;
		cs_log(head);
		cs_log_config();
		return(0);
	}
	if (strcmp(file, "syslog")) {
		if (!fp) {
			if ((fp = fopen(file, "a+")) <= (FILE *)0) {
				fp = (FILE *)0;
				fprintf(stderr, "couldn't open logfile: %s (errno %d)\n", file, errno);
			} else {
				time_t t;
				char line[80];
				memset(line, '-', sizeof(line));
				line[(sizeof(line)/sizeof(char)) - 1] = '\0';
				time(&t);
				if (!cfg->disablelog)
					fprintf(fp, "\n%s\n>> OSCam <<  cardserver started at %s%s\n", line, ctime(&t), line);
				cs_log_config();
			}
		}
		return(fp <= (FILE *)0);
	} else {
		openlog("oscam", LOG_NDELAY, LOG_DAEMON);
		use_syslog = 1;
		cs_log(head);
		cs_log_config();
		return(0);
	}
}

static char *get_log_header(int m, char *txt)
{
	if(m) {
		switch (cur_client()->typ) {
			case 'r':
			case 'c':
			case 'p':
				sprintf(txt, "%c %8d ", cur_client()->typ, get_threadnum(cur_client()));
				break;
			default:
				sprintf(txt, "%c          ", cur_client()->typ);
				break;
		}
	} else
		sprintf(txt, "%-11.11s", "");

	return(txt);
}

static void write_to_log(int flag, char *txt)
{
	//flag = -1 is old behaviour, before implementation of debug_nolf (=debug no line feed)
	//
	time_t t;
	struct tm *lt;
	char sbuf[16];
	char log_buf[700];

	//  get_log_header(flag, sbuf);
	//  memcpy(txt, sbuf, 11);

#ifdef CS_ANTICASC
	if (use_syslog && cur_client()->typ != 'a') // system-logfile
#else
	if (use_syslog) // system-logfile
#endif
		syslog(LOG_INFO, "%s", txt);

	time(&t);
	lt=localtime(&t);

	switch(flag) {
		case -1:
		sprintf(log_buf, "[LOG000]%4d/%02d/%02d %2d:%02d:%02d %s\n",
				lt->tm_year+1900, lt->tm_mon+1, lt->tm_mday,
				lt->tm_hour, lt->tm_min, lt->tm_sec, txt);
		break;
		case 1:
			sprintf(log_buf, "[LOG000]%4d/%02d/%02d %2d:%02d:%02d            %s",
					lt->tm_year+1900, lt->tm_mon+1, lt->tm_mday,
					lt->tm_hour, lt->tm_min, lt->tm_sec, txt);
			break;
		case 16:
			number_of_chars_printed = 0;
			sprintf(log_buf, "[LOG000]%s\n", txt);
			break;
		default:
			sprintf(log_buf, "[LOG000]%s", txt);
	}

	cs_write_log(log_buf + 8);
#ifdef CS_LOGHISTORY
	store_logentry(log_buf);
#endif
	if ((cur_client()->typ != 'c') && (cur_client()->typ != 'm'))
		return;
	struct s_client *prev, *cl;
	for (prev=first_client, cl=first_client->next; prev->next != NULL; prev=prev->next, cl=cl->next)
	{
		if ((cl->pid) && (cl->log))
		{
			if (cl->monlvl<2)
				if (strcmp(cur_client()->usr, cl->usr))
					continue;
			sprintf(sbuf, "%03d", cl->logcounter);
			cl->logcounter = (cl->logcounter+1) % 1000;
			memcpy(log_buf + 4, sbuf, 3);
			monitor_send_idx(cl, log_buf);
		}
	}
}

void cs_log(const char *fmt,...)
{
	char log_txt[512];
	get_log_header(1, log_txt);
	va_list params;
	va_start(params, fmt);
	vsprintf(log_txt+11, fmt, params);
	va_end(params);
	write_to_log(-1, log_txt);
}

void cs_close_log(void)
{
	if (use_stdout || use_syslog || !fp) return;
	fclose(fp);
	fp=(FILE *)0;
}
#ifdef WITH_DEBUG
void cs_debug(const char *fmt,...)
{
	//  cs_log("cs_debug called, cs_ptyp=%d, cs_dblevel=%d, %d", cs_ptyp, cs_dblevel ,cur_client()->cs_ptyp & cs_dblevel);
	char log_txt[512];
	if (cs_dblevel & cur_client()->cs_ptyp)
	{
		get_log_header(1, log_txt);
		va_list params;
		va_start(params, fmt);
		vsprintf(log_txt+11, fmt, params);
		va_end(params);
		write_to_log(-1, log_txt);
	}
}

void cs_debug_mask(unsigned short mask, const char *fmt,...)
{
	char log_txt[512];
	if (cs_dblevel & mask)
	{
		get_log_header(1, log_txt);
		va_list params;
		va_start(params, fmt);
		vsprintf(log_txt+11, fmt, params);
		va_end(params);
		write_to_log(-1, log_txt);
	}
}

void cs_debug_nolf(const char *fmt,...)
{
	char log_txt[512];
	if (cs_dblevel & cur_client()->cs_ptyp)
	{
		va_list params;
		va_start(params, fmt);
		vsprintf(log_txt, fmt, params);
		va_end(params);
		if(!memcmp(log_txt,"\n", 1)) {
			number_of_chars_printed = 0;
		}
		else
			number_of_chars_printed++;
		write_to_log(number_of_chars_printed, log_txt);
	}
}
#endif
void cs_dump(const uchar *buf, int n, char *fmt, ...)
{
	char log_txt[512];
	int i;

	if( fmt )
	{
		get_log_header(1, log_txt);
		va_list params;
		va_start(params, fmt);
		vsprintf(log_txt+11, fmt, params);
		va_end(params);
		write_to_log(-1, log_txt);
		//printf("LOG: %s\n", txt); fflush(stdout);
	}

	for( i=0; i<n; i+=16 )
	{
		get_log_header(0, log_txt);
		sprintf(log_txt+11, "%s", cs_hexdump(1, buf+i, (n-i>16) ? 16 : n-i));
		write_to_log(-1, log_txt);
	}
}
#ifdef WITH_DEBUG
void cs_ddump(const uchar *buf, int n, char *fmt, ...)
{
	char log_txt[512];
	int i;

	//if (((cs_ptyp & cs_dblevel)==cs_ptyp) && (fmt))
	if ((cur_client()->cs_ptyp & cs_dblevel) && (fmt))
	{
		get_log_header(1, log_txt);
		va_list params;
		va_start(params, fmt);
		vsprintf(log_txt+11, fmt, params);
		va_end(params);
		write_to_log(-1, log_txt);
		//printf("LOG: %s\n", txt); fflush(stdout);
	}
	//if (((cur_client()->cs_ptyp | D_DUMP) & cs_dblevel)==(cs_ptyp | D_DUMP))
	if (cur_client()->cs_ptyp & cs_dblevel)
	{
		for (i=0; i<n; i+=16)
		{
			get_log_header(0, log_txt);
			sprintf(log_txt+11, "%s", cs_hexdump(1, buf+i, (n-i>16) ? 16 : n-i));
			write_to_log(-1, log_txt);
		}
	}
}

void cs_ddump_mask(unsigned short mask, const uchar *buf, int n, char *fmt, ...)
{

	char log_txt[512];
	int i;
	//if (((cs_ptyp & cs_dblevel)==cs_ptyp) && (fmt))
	if ((mask & cs_dblevel) && (fmt))
	{
		get_log_header(1, log_txt);
		va_list params;
		va_start(params, fmt);
		vsprintf(log_txt+11, fmt, params);
		va_end(params);
		write_to_log(-1, log_txt);
		//printf("LOG: %s\n", txt); fflush(stdout);
	}
	//if (((cs_ptyp | D_DUMP) & cs_dblevel)==(cs_ptyp | D_DUMP))
	if (mask & cs_dblevel)
	{
		for (i=0; i<n; i+=16)
		{
			get_log_header(0, log_txt);
			sprintf(log_txt+11, "%s", cs_hexdump(1, buf+i, (n-i>16) ? 16 : n-i));
			write_to_log(-1, log_txt);
		}
	}
}
#endif
int cs_init_statistics(char *file) 
{
	if ((!fps) && (file != NULL))
	{
		if ((fps=fopen(file, "a+"))<=(FILE *)0)
		{
			fps=(FILE *)0;
			cs_log("couldn't open statistics file: %s", file);
		}
	}
	return(fps<=(FILE *)0);
}

void cs_statistics(struct s_client * client)
{
	if (!cfg->disableuserfile){
		time_t t;
		struct tm *lt;
		char buf[512];

		float cwps;

		time(&t);
		lt=localtime(&t);
		if (client->cwfound+client->cwnot>0)
		{
			cwps=client->last-client->login;
			cwps/=client->cwfound+client->cwnot;
		}
		else
			cwps=0;

		char *channel ="";
		if(cfg->mon_appendchaninfo)
			channel = get_servicename(client->last_srvid,client->last_caid);

		int lsec;
		if ((client->last_caid == 0xFFFF) && (client->last_srvid == 0xFFFF))
			lsec = client->last - client->login; //client leave calc total duration
		else
			lsec = client->last - client->lastswitch;

		int secs = 0, fullmins = 0, mins = 0, fullhours = 0;

		if((lsec > 0) && (lsec < 1000000)) {
			secs = lsec % 60;
			if (lsec > 60) {
				fullmins = lsec / 60;
				mins = fullmins % 60;
				if(fullmins > 60) {
					fullhours = fullmins / 60;
				}
			}
		}

		/* statistics entry start with 's' to filter it out on other end of pipe
		 * so we can use the same Pipe as Log
		 */
		sprintf(buf, "s%02d.%02d.%02d %02d:%02d:%02d %3.1f %s %s %d %d %d %d %d %d %d %ld %ld %02d:%02d:%02d %s %04X:%04X %s\n",
				lt->tm_mday, lt->tm_mon+1, lt->tm_year%100,
				lt->tm_hour, lt->tm_min, lt->tm_sec, cwps,
				client->usr[0] ? client->usr : "-",
				cs_inet_ntoa(client->ip),
				client->port,
				client->cwfound,
				client->cwcache,
				client->cwnot,
				client->cwignored,
				client->cwtout,
				client->cwtun,
				client->login,
				client->last,
				fullhours, mins, secs,
				ph[client->ctyp].desc,
				client->last_caid,
				client->last_srvid,
				channel);

		cs_write_log(buf);
	}
}
