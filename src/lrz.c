/*
  lrz - receive files with x/y/zmodem
  Copyright (C) until 1988 Chuck Forsberg (Omen Technology INC)
  Copyright (C) 1994 Matt Porter, Michael D. Black
  Copyright (C) 1996, 1997 Uwe Ohse

  This program is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation; either version 2, or (at your option)
  any later version.
  
  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program; if not, write to the Free Software
  Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA
  02111-1307, USA.

  originally written by Chuck Forsberg
*/

#include "zglobal.h"

#define SS_NORMAL 0
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <ctype.h>
#include <errno.h>
#include <getopt.h>
#include <time.h>
#include <locale.h>

#include "timing.h"
#include "long-options.h"
#include "xstrtoul.h"
#include "error.h"
#include "lrzsz.h"

#define MAX_BLOCK 8192

struct lrzsz_config config;

FILE *fout;


int Lastrx;
int Crcflg;
int Firstsec;
int errors;
int Restricted=1;	/* restricted; no /.. or ../ in filenames */
int Readnum = MAX_BLOCK;	/* Number of bytes to ask for in read() from modem */
int skip_if_not_found;

char *Pathname;
const char *program_name;		/* the name by which we were called */

int Topipe=0;
int MakeLCPathname=TRUE;	/* make received pathname lower case */
int Verbose=0;
int Quiet=0;		/* overrides logic that would otherwise set verbose */
int Nflag = 0;		/* Don't really transfer files */
int Rxclob=FALSE;	/* Clobber existing file */
int Rxbinary=FALSE;	/* receive all files in bin mode */
int Rxascii=FALSE;	/* receive files in ascii (translate) mode */
int Thisbinary;		/* current file is to be received in bin mode */
int try_resume=FALSE;
int allow_remote_commands=FALSE;
int junk_path=FALSE;
int no_timeout=FALSE;
enum zm_type_enum protocol;
int	under_rsh=FALSE;
int zmodem_requested=FALSE;

static char *secbuf;

static int rzfiles (struct zm_fileinfo *);
static int tryz (void);
static void checkpath (const char *name);
static void chkinvok (const char *s);
static void report (int sct);
static void uncaps (char *s);
static int IsAnyLower (const char *s);
static int putsec (struct zm_fileinfo *zi, char *buf, size_t n);
static int make_dirs (char *pathname);
static int procheader (char *name, struct zm_fileinfo *);
static int wcgetsec (size_t *Blklen, char *rxbuf, unsigned int maxtime);
static int wcrx (struct zm_fileinfo *);
static int wcrxpn (struct zm_fileinfo *, char *rpn);
static int wcreceive (int argc, char **argp);
static int rzfile (struct zm_fileinfo *);
static void usage (int exitcode, const char *what);
static void usage1 (int exitcode);
static int closeit (struct zm_fileinfo *);
static void ackbibi (void);
static void zmputs (const char *s);
static size_t getfree (void);

static long buffersize=32768;
static unsigned long min_bps=0;
static long min_bps_time=120;

char Lzmanag;		/* Local file management request */
char zconv;		/* ZMODEM file conversion request */
char zmanag;		/* ZMODEM file management request */
char ztrans;		/* ZMODEM file transport request */
int Zctlesc;		/* Encode control characters */
int Zrwindow = 1400;	/* RX window size (controls garbage count) */

int tryzhdrtype=ZRINIT;	/* Header type to send corresponding to Last rx close */
time_t stop_time;

/* called by signal interrupt or terminate to clean things up */
void
bibi(int n)
{
	if (zmodem_requested)
		zmputs(Attn);
	canit(STDOUT_FILENO);
	lrzsz_iomode(0,LRZSZ_IOMODE_RESET, &config);
	error(128+n,0,_("caught signal %d; exiting"), n);
}

static struct option const long_options[] =
{
	{"append", no_argument, NULL, '+'},
	{"twostop", no_argument, NULL, '2'},
	{"ascii", no_argument, NULL, 'a'},
	{"binary", no_argument, NULL, 'b'},
	{"bufsize", required_argument, NULL, 'B'},
	{"allow-commands", no_argument, NULL, 'C'},
	{"allow-remote-commands", no_argument, NULL, 'C'},
	{"escape", no_argument, NULL, 'e'},
	{"rename", no_argument, NULL, 'E'},
	{"help", no_argument, NULL, 'h'},
	{"crc-check", no_argument, NULL, 'H'},
	{"junk-path", no_argument, NULL, 'j'},
	{"disable-timeouts", no_argument, NULL, 'O'},
	{"disable-timeout", no_argument, NULL, 'O'}, /* i can't get it right */
	{"min-bps", required_argument, NULL, 'm'},
	{"min-bps-time", required_argument, NULL, 'M'},
	{"newer", no_argument, NULL, 'n'},
	{"newer-or-longer", no_argument, NULL, 'N'},
	{"protect", no_argument, NULL, 'p'},
	{"resume", no_argument, NULL, 'r'},
	{"restricted", no_argument, NULL, 'R'},
	{"quiet", no_argument, NULL, 'q'},
	{"stop-at", required_argument, NULL, 's'},
	{"timeout", required_argument, NULL, 't'},
	{"keep-uppercase", no_argument, NULL, 'u'},
	{"unrestrict", no_argument, NULL, 'U'},
	{"verbose", no_argument, NULL, 'v'},
	{"windowsize", required_argument, NULL, 'w'},
	{"with-crc", no_argument, NULL, 'c'},
	{"xmodem", no_argument, NULL, 'X'},
	{"ymodem", no_argument, NULL, 1},
	{"zmodem", no_argument, NULL, 'Z'},
	{"overwrite", no_argument, NULL, 'y'},
	{"null", no_argument, NULL, 'D'},
	{"syslog-facility", required_argument, NULL , 10},
	{"syslog-severity", required_argument, NULL , 11},
	{"log-level",       required_argument, NULL , 12},
	{"delay-startup", required_argument, NULL, 4},
	{NULL,0,NULL,0}
};

static void
show_version(void)
{
	printf ("%s (GNU %s) %s\n", program_name, PACKAGE, VERSION);
}

int
main(int argc, char *argv[])
{
	register char *cp;
	register int npats;
	char **patts=NULL; /* keep compiler quiet */
	int exitcode=0;
	int c;
	unsigned int startup_delay=0;

	Rxtimeout = 100;
	setbuf(stderr, NULL);
	if ((cp=getenv("SHELL")) && (strstr(cp, "rsh") || strstr(cp, "rksh")
		|| strstr(cp,"rbash") || strstr(cp, "rshell")))
		under_rsh=TRUE;
	if ((cp=getenv("ZMODEM_RESTRICTED"))!=NULL)
		Restricted=2;

	/* make temporary and unfinished files */
	umask(0077);

	lrzsz_check_stderr(&config);
	chkinvok(argv[0]);	/* if called as [-]rzCOMMAND set flag */

	setlocale (LC_ALL, "");
	bindtextdomain (PACKAGE, LOCALEDIR);
	textdomain (PACKAGE);

    parse_long_options (argc, argv, show_version, usage1);

	while ((c = getopt_long (argc, argv, 
		"a+bB:cCDeEhm:M:OprRqs:St:uUvw:XZy",
		long_options, (int *) 0)) != EOF)
	{
		unsigned long int tmp;
		char *tmpptr;
		enum strtol_error s_err;

		switch (c)
		{
		case 0:
			break;
		case '+': Lzmanag = ZF1_ZMAPND; break;
		case '2': config.io.two_stopbits = TRUE; break;
		case 'a': Rxascii=TRUE;  break;
		case 'b': Rxbinary=TRUE; break;
		case 'B': 
			if (strcmp(optarg,"auto")==0) 
				buffersize=-1;
			else
				buffersize=strtol(optarg,NULL,10);
			break;
		case 'c': Crcflg=TRUE; break;
		case 'C': allow_remote_commands=TRUE; break;
		case 'D': Nflag = TRUE; break;
		case 'E': Lzmanag = ZF1_ZMCHNG; break;
		case 'e': Zctlesc = 1; break;
		case 'h': usage(0,NULL); break;
		case 'H': Lzmanag= ZF1_ZMCRC; break;
		case 'j': junk_path=TRUE; break;
		case 'm':
			s_err = xstrtoul (optarg, &tmpptr, 0, &tmp, "km");
			min_bps = tmp;
			if (s_err != LONGINT_OK)
				STRTOL_FATAL_ERROR (optarg, _("min_bps"), s_err);
			break;
		case 'M':
			s_err = xstrtoul (optarg, NULL, 0, &tmp, NULL);
			min_bps_time = tmp;
			if (s_err != LONGINT_OK)
				STRTOL_FATAL_ERROR (optarg, _("min_bps_time"), s_err);
			if (min_bps_time<=1)
				usage(2,_("min_bps_time must be > 1"));
			break;
		case 'N': Lzmanag = ZF1_ZMNEWL;  break;
		case 'n': Lzmanag = ZF1_ZMNEW;  break;
		case 'O': no_timeout=TRUE; break;
		case 'p': Lzmanag = ZF1_ZMPROT;  break;
		case 'q': Quiet=TRUE; Verbose=0; break;
		case 's':
			if (isdigit((unsigned char) (*optarg))) {
				struct tm *tm;
				time_t t;
				int hh,mm;
				char *nex;
				
				hh = strtoul (optarg, &nex, 10);
				if (hh>23)
					usage(2,_("hour to large (0..23)"));
				if (*nex!=':')
					usage(2, _("unparsable stop time\n"));
				nex++;
                mm = strtoul (optarg, &nex, 10);
				if (mm>59)
					usage(2,_("minute to large (0..59)"));
				
				t=time(NULL);
				tm=localtime(&t);
				tm->tm_hour=hh;
				tm->tm_min=hh;
				stop_time=mktime(tm);
				if (stop_time<t)
					stop_time+=86400L; /* one day more */
				if (stop_time - t <10)
					usage(2,_("stop time to small"));
			} else {
				s_err = xstrtoul (optarg, NULL, 0, &tmp, NULL);
				stop_time = tmp + time(0);
				if (s_err != LONGINT_OK)
					STRTOL_FATAL_ERROR (optarg, _("stop-at"), s_err);
				if (tmp<10)
					usage(2,_("stop time to small"));
			}
			break;


		case 'r': 
			if (try_resume) 
				Lzmanag= ZF1_ZMCRC;
			else
				try_resume=TRUE;  
			break;
		case 'R': Restricted++;  break;
		case 't':
			s_err = xstrtoul (optarg, NULL, 0, &tmp, NULL);
			Rxtimeout = tmp;
			if (s_err != LONGINT_OK)
				STRTOL_FATAL_ERROR (optarg, _("timeout"), s_err);
			if (Rxtimeout<10 || Rxtimeout>1000)
				usage(2,_("timeout out of range 10..1000"));
			break;
		case 'w':
			s_err = xstrtoul (optarg, NULL, 0, &tmp, NULL);
			Zrwindow = tmp;
			if (s_err != LONGINT_OK)
				STRTOL_FATAL_ERROR (optarg, _("window size"), s_err);
			break;
		case 'u':
			MakeLCPathname=FALSE; break;
		case 'U':
			if (!under_rsh)
				Restricted=0;
			else  {
				lrzsz_log(LOG_WARNING, NULL, 
					"--unrestrict option used under restricted shell");
				exit(1);
			}
			break;
		case 'v':
			++Verbose; break;
		case 'X': protocol=ZM_XMODEM; break;
		case 1:   protocol=ZM_YMODEM; break;
		case 'Z': protocol=ZM_ZMODEM; break;
		case 'y':
			Rxclob=TRUE; break;

		case 10:
			lrzsz_set_syslog_facility(optarg);
			break;
		case 11:
			lrzsz_set_syslog_severity(optarg);
			break;
		case 12:
			lrzsz_set_locallog_severity(optarg);
			break;
		case 4:
			s_err = xstrtoul (optarg, NULL, 0, &tmp, NULL);
			startup_delay = tmp;
			if (s_err != LONGINT_OK)
				STRTOL_FATAL_ERROR (optarg, _("startup delay"), s_err);
			break;
		default:
			usage(2,NULL);
		}

	}

	if (getuid()!=geteuid()) {
		error(1,0,
		_("this program was never intended to be used setuid\n"));
	}

	secbuf=malloc(MAX_BLOCK+1);
	if (!secbuf) error(1,0,_("out of memory"));

	/* initialize zsendline tab */
	zsendline_init();
#ifdef HAVE_SIGINTERRUPT
	siginterrupt(SIGALRM,1);
#endif
	if (startup_delay)
		sleep(startup_delay);

	npats = argc - optind;
	patts=&argv[optind];

	if (npats > 1)
		usage(2,_("garbage on commandline"));
	if (protocol!=ZM_XMODEM && npats)
		usage(2, _("garbage on commandline"));
	if (Restricted && allow_remote_commands) {
		allow_remote_commands=FALSE;
	}
	if (config.io.may_use_stderr && !Quiet) {
		if (Verbose == 0)
			Verbose = 2;
	}

	lrzsz_iomode(0, LRZSZ_IOMODE_RAW, &config);
	readline_setup(0, MAX_BLOCK, MAX_BLOCK*2);
	if (signal(SIGINT, bibi) == SIG_IGN) 
		signal(SIGINT, SIG_IGN);
	else
		signal(SIGINT, bibi);
	signal(SIGTERM, bibi);
	signal(SIGPIPE, bibi);
	if (wcreceive(npats, patts)==ERROR) {
		exitcode=0200;
		canit(STDOUT_FILENO);
	}
	lrzsz_iomode(0,LRZSZ_IOMODE_RESET, &config);
	if (exitcode && !zmodem_requested)	/* bellow again with all thy might. */
		canit(STDOUT_FILENO);
	if (Verbose)
	{
		fputs("\r\n",stderr);
		if (exitcode)
			fputs(_("Transfer incomplete\n"),stderr);
		else
			fputs(_("Transfer complete\n"),stderr);
	}
	exit(exitcode);
}

static void
usage1(int exitcode)
{
	usage(exitcode,NULL);
}

static void
usage(int exitcode, const char *what)
{
	FILE *f=stdout;

	if (exitcode)
	{
		if (what)
			fprintf(stderr, "%s: %s\n",program_name,what);
		fprintf (stderr, _("Try `%s --help' for more information.\n"),
			program_name);
		exit(exitcode);
	}

	fprintf(f, _("%s version %s\n"), program_name,
		VERSION);

	fprintf(f,_("Usage: %s [options] [filename.if.xmodem]\n"), program_name);
	fputs(_("Receive files with ZMODEM/YMODEM/XMODEM protocol\n"),f);
	fputs(_(
		"    (X) = option applies to XMODEM only\n"
		"    (Y) = option applies to YMODEM only\n"
		"    (Z) = option applies to ZMODEM only\n"
		),f);
	fputs(_(
"  -+, --append                append to existing files\n"
"  -2, --twostop               use 2 stop bits (you usually do not want to)\n"
"  -a, --ascii                 ASCII transfer (change CR/LF to LF)\n"
"  -b, --binary                binary transfer\n"
"  -B, --bufsize N             buffer N bytes (N==auto: buffer whole file)\n"
"  -c, --with-crc              Use 16 bit CRC (X)\n"
"  -C, --allow-remote-commands allow execution of remote commands (Z)\n"
"  -D, --null                  write all received data to /dev/null\n"
"      --delay-startup N       sleep N seconds before doing anything\n"
"  -e, --escape                Escape control characters (Z)\n"
"  -E, --rename                rename any files already existing\n"
"      --errors N              generate CRC error every N bytes (debugging)\n"
"  -h, --help                  Help, print this usage message\n"
"  -m, --min-bps N             stop transmission if BPS below N\n"
"  -M, --min-bps-time N          for at least N seconds (default: 120)\n"
"  -O, --disable-timeouts      disable timeout code, wait forever for data\n"
"  -p, --protect               protect existing files\n"
"  -q, --quiet                 quiet, no progress reports\n"
"  -r, --resume                try to resume interrupted file transfer (Z)\n"
"  -R, --restricted            restricted, more secure mode\n"
"  -s, --stop-at {HH:MM|+N}    stop transmission at HH:MM or in N seconds\n"
"      --syslog[=off]          turn syslog on or off, if possible\n"
"  -t, --timeout N             set timeout to N tenths of a second\n"
"  -u, --keep-uppercase        keep upper case filenames\n"
"  -U, --unrestrict            disable restricted mode (if allowed to)\n"
"  -v, --verbose               be verbose, provide debugging information\n"
"  -w, --windowsize N          Window is N bytes (Z)\n"
"  -X  --xmodem                use XMODEM protocol\n"
"  -y, --overwrite             Yes, clobber existing file if any\n"
"      --ymodem                use YMODEM protocol\n"
"  -Z, --zmodem                use ZMODEM protocol\n"
"\n"
"short options use the same arguments as the long ones\n"
	),f);
	exit(exitcode);
}

/*
 * Let's receive something already.
 */

static int 
wcreceive(int argc, char **argp)
{
	int c;
	struct zm_fileinfo zi;
	zi.fname=NULL;
	zi.modtime=0;
	zi.mode=0;
	zi.bytes_total=0;
	zi.bytes_sent=0;
	zi.bytes_received=0;
	zi.bytes_skipped=0;
	zi.eof_seen=0;

	if (protocol!=ZM_XMODEM || argc==0) {
		Crcflg=1;
		if ( !Quiet)
			lrzsz_log(LOG_INFO,NULL,"%s waiting to receive.", program_name);
		if ((c=tryz())!=0) {
			if (c == ZCOMPL)
				return OK;
			if (c == ERROR)
				goto fubar;
			c = rzfiles(&zi);

			if (c)
				goto fubar;
		} else {
			for (;;) {
				timing(1,NULL);

				if (wcrxpn(&zi,secbuf)== ERROR)
					goto fubar;
				if (secbuf[0]==0)
					return OK;
				if (procheader(secbuf, &zi) == ERROR)
					goto fubar;
				if (wcrx(&zi)==ERROR)
					goto fubar;

				if (1) {
					double d;
					long bps;
					d=timing(0,NULL);
					if (d==0)
						d=0.5; /* can happen if timing uses time() */
					bps=(zi.bytes_received-zi.bytes_skipped)/d;

					lrzsz_log(LOG_INFO,&zi, "%ld Bytes of %ld, %ld BPS", 
						zi.bytes_received, zi.bytes_total, bps);
				}
			}
		}
	} else {
		char dummy[128];
		dummy[0]='\0'; /* pre-ANSI HPUX cc demands this */
		dummy[1]='\0'; /* procheader uses name + 1 + strlen(name) */
		zi.bytes_total = DEFBYTL;

		timing(1,NULL);

		procheader(dummy, &zi);

		if (Pathname)
			free(Pathname);
		errno=0;
		Pathname=malloc(PATH_MAX+1);
		if (!Pathname)
			error(1,0,_("out of memory"));

		strcpy(Pathname, *argp);
		checkpath(Pathname);

		lrzsz_log(LOG_INFO,NULL,"%s waiting to receive %s.", program_name, Pathname);

		if ((fout=fopen(Pathname, "w")) == NULL) {
			lrzsz_log(LOG_ERR,&zi, "cannot open: %s", strerror(errno));
			return ERROR;
		}
		if (wcrx(&zi)==ERROR) {
			goto fubar;
		}
		if (1) {
			double d;
			long bps;
			d=timing(0,NULL);
			if (d==0)
				d=0.5; /* can happen if timing uses time() */
			bps=(zi.bytes_received-zi.bytes_skipped)/d;
			lrzsz_log(LOG_INFO,&zi, "%ld Bytes, %ld BPS",
					zi.bytes_received, bps);
		}
	}
	return OK;
fubar:
	lrzsz_log(LOG_INFO,&zi, "got error");
	canit(STDOUT_FILENO);
	if (Topipe && fout) {
		pclose(fout);
		return ERROR;
	}
	if (fout)
		fclose(fout);

	if (Restricted && Pathname) {
		unlink(Pathname);
		lrzsz_log(LOG_INFO,&zi,"removed.");
	}
	return ERROR;
}


/*
 * Fetch a pathname from the other end as a C ctyle ASCIZ string.
 * Length is indeterminate as long as less than Blklen
 * A null string represents no more files (YMODEM)
 */
static int 
wcrxpn(struct zm_fileinfo *zi, char *rpn)
{
	register int c;
	size_t Blklen=0;		/* record length of received packets */

	READLINE_PF(1);

et_tu:
	Firstsec=TRUE;
	zi->eof_seen=FALSE;
	sendline(Crcflg?WANTCRC:NAK);
	flushmo();
	purgeline(0); /* Do read next time ... */
	while ((c = wcgetsec(&Blklen, rpn, 100)) != 0) {
		if (c == WCEOT) {
			lrzsz_log(LOG_INFO, zi, "Pathname fetch returned EOT");
			sendline(ACK);
			flushmo();
			purgeline(0);	/* Do read next time ... */
			READLINE_PF(1);
			goto et_tu;
		}
		return ERROR;
	}
	sendline(ACK);
	flushmo();
	return OK;
}

/*
 * Adapted from CMODEM13.C, written by
 * Jack M. Wierda and Roderick W. Hart
 */
static int 
wcrx(struct zm_fileinfo *zi)
{
	register int sectnum, sectcurr;
	register char sendchar;
	size_t Blklen;

	Firstsec=TRUE;sectnum=0; 
	zi->eof_seen=FALSE;
	sendchar=Crcflg?WANTCRC:NAK;

	for (;;) {
		sendline(sendchar);	/* send it now, we're ready! */
		flushmo();
		purgeline(0);	/* Do read next time ... */
		sectcurr=wcgetsec(&Blklen, secbuf, 
			(unsigned int) ((sectnum&0177) ? 50 : 130));
		report(sectcurr);
		if (sectcurr==((sectnum+1) &0377)) {
			sectnum++;
			/* if using xmodem we don't know how long a file is */
			if (zi->bytes_total && R_BYTESLEFT(zi) < Blklen)
				Blklen=R_BYTESLEFT(zi);
			zi->bytes_received+=Blklen;
			if (putsec(zi, secbuf, Blklen)==ERROR)
				return ERROR;
			sendchar=ACK;
		}
		else if (sectcurr==(sectnum&0377)) {
			lrzsz_log(LOG_DEBUG,zi, "Received dup Sector");
			sendchar=ACK;
		}
		else if (sectcurr==WCEOT) {
			if (closeit(zi))
				return ERROR;
			sendline(ACK);
			flushmo();
			purgeline(0);	/* Do read next time ... */
			return OK;
		}
		else if (sectcurr==ERROR)
			return ERROR;
		else {
			lrzsz_log(LOG_DEBUG, zi, "Sync Error");
			return ERROR;
		}
	}
}

/*
 * Wcgetsec fetches a Ward Christensen type sector.
 * Returns sector number encountered or ERROR if valid sector not received,
 * or CAN CAN received
 * or WCEOT if eot sector
 * time is timeout for first char, set to 4 seconds thereafter
 ***************** NO ACK IS SENT IF SECTOR IS RECEIVED OK **************
 *    (Caller must do that when he is good and ready to get next sector)
 */
static int
wcgetsec(size_t *Blklen, char *rxbuf, unsigned int maxtime)
{
	register int checksum, wcj, firstch;
	register unsigned short oldcrc;
	register char *p;
	int sectcurr;

	for (Lastrx=errors=0; errors<RETRYMAX; errors++) {

		if ((firstch=READLINE_PF(maxtime))==STX) {
			*Blklen=1024;
			goto get2;
		}
		if (firstch==SOH) {
			*Blklen=128;
get2:
			sectcurr=READLINE_PF(1);
			if ((sectcurr+(oldcrc=READLINE_PF(1)))==0377) {
				oldcrc=checksum=0;
				for (p=rxbuf,wcj=*Blklen; --wcj>=0; ) {
					if ((firstch=READLINE_PF(1)) < 0)
						goto bilge;
					oldcrc=updcrc(firstch, oldcrc);
					checksum += (*p++ = firstch);
				}
				if ((firstch=READLINE_PF(1)) < 0)
					goto bilge;
				if (Crcflg) {
					oldcrc=updcrc(firstch, oldcrc);
					if ((firstch=READLINE_PF(1)) < 0)
						goto bilge;
					oldcrc=updcrc(firstch, oldcrc);
					if (oldcrc & 0xFFFF)
						lrzsz_log(LOG_DEBUG,NULL,"CRC error");
					else {
						Firstsec=FALSE;
						return sectcurr;
					}
				}
				else if (((checksum-firstch)&0377)==0) {
					Firstsec=FALSE;
					return sectcurr;
				}
				else
					lrzsz_log(LOG_DEBUG, NULL, "Checksum error");
			}
			else
				lrzsz_log(LOG_DEBUG,NULL,"Sector number garbled");
		}
		/* make sure eot really is eot and not just mixmash */
		else if (firstch==EOT && READLINE_PF(1)==TIMEOUT)
			return WCEOT;
		else if (firstch==CAN) {
			if (Lastrx==CAN) {
				lrzsz_log(LOG_DEBUG, NULL, "Sender Cancelled");
				return ERROR;
			} else {
				Lastrx=CAN;
				continue;
			}
		}
		else if (firstch==TIMEOUT) {
			if (Firstsec)
				goto humbug;
bilge:
			lrzsz_log(LOG_DEBUG, NULL, "Timeout");
		}
		else
			lrzsz_log(LOG_DEBUG, NULL, "Got 0%o sector header", firstch);

humbug:
		Lastrx=0;
		{
			int cnt=1000;
			while(cnt-- && READLINE_PF(1)!=TIMEOUT)
				;
		}
		if (Firstsec) {
			sendline(Crcflg?WANTCRC:NAK);
			flushmo();
			purgeline(0);	/* Do read next time ... */
		} else {
			maxtime=40;
			sendline(NAK);
			flushmo();
			purgeline(0);	/* Do read next time ... */
		}
	}
	/* try to stop the bubble machine. */
	canit(STDOUT_FILENO);
	return ERROR;
}

#define ZCRC_DIFFERS (ERROR+1)
#define ZCRC_EQUAL (ERROR+2)
/*
 * do ZCRC-Check for open file f.
 * check at most check_bytes bytes (crash recovery). if 0 -> whole file.
 * remote file size is remote_bytes.
 */
static int 
do_crc_check(FILE *f, struct zm_fileinfo *fi, size_t remote_bytes, size_t check_bytes) 
{
	struct stat st;
	unsigned long crc;
	unsigned long rcrc;
	size_t n;
	int c;
	int t1=0,t2=0;
	if (-1==fstat(fileno(f),&st)) {
		lrzsz_log(LOG_ERR,fi, "cannot fstat open file: %s",strerror(errno));
		return ERROR;
	}
	if (check_bytes==0 && ((size_t) st.st_size)!=remote_bytes)
		return ZCRC_DIFFERS; /* shortcut */

	crc=0xFFFFFFFFL;
	n=check_bytes;
	if (n==0)
		n=st.st_size;
	while (n-- && ((c = getc(f)) != EOF))
		crc = UPDC32(c, crc);
	crc = ~crc;
	clearerr(f);  /* Clear EOF */
	fseek(f, 0L, 0);

	while (t1<3) {
		stohdr(check_bytes);
		zshhdr(ZCRC, Txhdr);
		while(t2<3) {
			size_t tmp;
			c = zgethdr(Rxhdr, 0, &tmp, &config);
			rcrc=(unsigned long) tmp;
			switch (c) {
			default: /* ignore */
				break;
			case ZFIN:
				return ERROR;
			case ZRINIT:
				return ERROR;
			case ZCAN:
				lrzsz_log(LOG_DEBUG,fi, "got ZCAN");
				return ERROR;
				break;
			case ZCRC:
				if (crc!=rcrc)
					return ZCRC_DIFFERS;
				return ZCRC_EQUAL;
				break;
			}
		}
	}
	return ERROR;
}

/*
 * Process incoming file information header
 */
static int
procheader(char *name, struct zm_fileinfo *zi)
{
	const char *openmode;
	char *p;
	static char *name_static=NULL;
	char *nameend;

	if (name_static)
		free(name_static);
	if (junk_path) {
		p=strrchr(name,'/');
		if (p) {
			p++;
			if (!*p) {
				/* alert - file name ended in with a / */
				lrzsz_log(LOG_ERR,zi,"file name ends with a /, skipped");
				return ERROR;
			}
			name=p;
		}
	}
	name_static=malloc(strlen(name)+1);
	if (!name_static)
		error(1,0,_("out of memory"));
	strcpy(name_static,name);
	zi->fname=name_static;

	lrzsz_log(LOG_DEBUG,zi, "zmanag=%x, Lzmanag=%x, zconv=%x",zmanag,Lzmanag,zconv);

	/* set default parameters and overrides */
	openmode = "w";
	Thisbinary = (!Rxascii) || Rxbinary;
	if (Lzmanag)
		zmanag = Lzmanag;

	/*
	 *  Process ZMODEM remote file management requests
	 */
	if (!Rxbinary && zconv == ZCNL)	/* Remote ASCII override */
		Thisbinary = 0;
	if (zconv == ZCBIN)	/* Remote Binary override */
		Thisbinary = TRUE;
	if (Thisbinary && zconv == ZCBIN && try_resume)
		zconv=ZCRESUM;
	if (zmanag == ZF1_ZMAPND && zconv!=ZCRESUM)
		openmode = "a";
	if (skip_if_not_found)
		openmode="r+";

	zi->bytes_total = DEFBYTL;
	zi->mode = 0; 
	zi->eof_seen = 0; 
	zi->modtime = 0;

	nameend = name + 1 + strlen(name);
	if (*nameend) {	/* file coming from Unix or DOS system */
		long modtime;
		long bytes_total;
		int mode;
		sscanf(nameend, "%ld%lo%o", &bytes_total, &modtime, &mode);
		zi->modtime=modtime;
		zi->bytes_total=bytes_total;
		zi->mode=mode;
		if (zi->mode & UNIXFILE)
			++Thisbinary;
	}

	/* Check for existing file */
	if (zconv != ZCRESUM && !Rxclob && (zmanag&ZF1_ZMMASK) != ZF1_ZMCLOB 
		&& (zmanag&ZF1_ZMMASK) != ZF1_ZMAPND
		&& (fout=fopen(name, "r"))) {
		struct stat sta;
		char *tmpname;
		char *ptr;
		int i;
		if (zmanag == ZF1_ZMNEW || zmanag==ZF1_ZMNEWL) {
			if (-1==fstat(fileno(fout),&sta)) {
				lrzsz_log(LOG_ERR,zi,"cannot fstat open file: %s",
					strerror(errno));
				return ERROR;
			}
			if (zmanag == ZF1_ZMNEW) {
				if (sta.st_mtime > zi->modtime) {
					lrzsz_log(LOG_INFO,zi,"skipping: newer file exists");
					return ERROR; /* skips file */
				}
			} else {
				/* newer-or-longer */
				if (((size_t) sta.st_size) >= zi->bytes_total 
					&& sta.st_mtime > zi->modtime) {
					lrzsz_log(LOG_INFO,zi,"skipping: longer+newer file exists");
					return ERROR; /* skips file */
				}
			}
			fclose(fout);
		} else if (zmanag==ZF1_ZMCRC) {
			int r=do_crc_check(fout,zi, zi->bytes_total,0);
			if (r==ERROR) {
				fclose(fout);
				return ERROR;
			}
			if (r!=ZCRC_DIFFERS) {
				return ERROR; /* skips */
			}
			fclose(fout);
		} else {
			size_t namelen;
			fclose(fout);
			if ((zmanag & ZF1_ZMMASK)!=ZF1_ZMCHNG) {
				lrzsz_log(LOG_INFO, zi, "file exists, skipped");
				return ERROR;
			}
			/* try to rename */
			namelen=strlen(name);
			tmpname=xmalloc(namelen+5);
			memcpy(tmpname,name,namelen);
			ptr=tmpname+namelen;
			*ptr++='.';
			i=0;
			do {
				sprintf(ptr,"%d",i++);
			} while (i<1000 && stat(tmpname,&sta)==0);
			if (i==1000)
				return ERROR;
			free(name_static);
			name_static=tmpname;
			zi->fname=name_static;
		}
	}

	if (!*nameend) {		/* File coming from CP/M system */
		for (p=name_static; *p; ++p)		/* change / to _ */
			if ( *p == '/')
				*p = '_';

		if ( *--p == '.')		/* zap trailing period */
			*p = 0;
	}

	if (!zmodem_requested && MakeLCPathname && !IsAnyLower(name_static)
	  && !(zi->mode&UNIXFILE))
		uncaps(name_static);
	if (Topipe > 0) {
		if (Pathname)
			free(Pathname);
		Pathname=malloc((PATH_MAX)*2);
		if (!Pathname)
			error(1,0,_("out of memory"));
		sprintf(Pathname, "%s %s", program_name+2, name_static);
		lrzsz_log(LOG_DEBUG,zi, "Topipe: %s %s",Pathname, Thisbinary?"BIN":"ASCII");
		if ((fout=popen(Pathname, "w")) == NULL)
			return ERROR;
	} else {
		if (protocol==ZM_XMODEM)
			/* we don't have the filename yet */
			return OK; /* dummy */
		if (Pathname)
			free(Pathname);
		Pathname=malloc((PATH_MAX)*2);
		if (!Pathname)
			error(1,0,_("out of memory"));
		strcpy(Pathname, name_static);
		if (Verbose) {
			lrzsz_log(LOG_DEBUG,zi,"Receiving: %s\n", name_static);
		}
		checkpath(name_static);
		if (Nflag)
		{
			/* cast because we might not have a prototyp for strdup :-/ */
			free(name_static);
			name_static=(char *) strdup("/dev/null");
			if (!name_static)
			{
				fprintf(stderr,"%s: %s\n", program_name, _("out of memory"));
				exit(1);
			}
		}

		if (Thisbinary && zconv==ZCRESUM) {
			struct stat st;
			fout = fopen(name_static, "r+");
			if (fout && 0==fstat(fileno(fout),&st))
			{
				int can_resume=TRUE;
				if (zmanag==ZF1_ZMCRC) {
					int r=do_crc_check(fout,zi, zi->bytes_total,st.st_size);
					if (r==ERROR) {
						fclose(fout);
						return ZFERR;
					}
					if (r==ZCRC_DIFFERS) {
						can_resume=FALSE;
					}
				}
				if ((unsigned long)st.st_size > zi->bytes_total) {
					can_resume=FALSE;
				}
				/* retransfer whole blocks */
				zi->bytes_skipped = st.st_size & ~(1023);
				if (can_resume) {
					if (fseek(fout, (long) zi->bytes_skipped, SEEK_SET)) {
						fclose(fout);
						return ZFERR;
					}
				}
				else
					zi->bytes_skipped=0; /* resume impossible, file has changed */
				goto buffer_it;
			}
			zi->bytes_skipped=0;
			if (fout)
				fclose(fout);
		}
		fout = fopen(name_static, openmode);
#ifdef ENABLE_MKDIR
		if ( !fout && Restricted < 2) {
			if (make_dirs(name_static))
				fout = fopen(name_static, openmode);
		}
#endif
		if ( !fout)
		{
			int e=errno;
			lrzsz_log(LOG_ERR, zi, "cannot open: %s",strerror(e));
			return ERROR;
		}
	}
buffer_it:
	if (Topipe == 0) {
		static char *s=NULL;
		static size_t last_length=0;

		if (buffersize==-1 && s) {
			if (zi->bytes_total>last_length) {
				free(s);
				s=NULL;
				last_length=0;
			}
		}
		if (!s && buffersize) {
			last_length=32768;
			if (buffersize==-1) {
				if (zi->bytes_total>0)
					last_length=zi->bytes_total;
			} else 
				last_length=buffersize;
			/* buffer `4096' bytes pages */
			last_length=(last_length+4095)&0xfffff000;
			s=xmalloc(last_length);
		}
		if (s) {
			setvbuf(fout,s,_IOFBF,last_length);
		}
	}
	zi->bytes_received=zi->bytes_skipped;

	return OK;
}

#ifdef ENABLE_MKDIR
/*
 *  Directory-creating routines from Public Domain TAR by John Gilmore
 */

/*
 * After a file/link/symlink/dir creation has failed, see if
 * it's because some required directory was not present, and if
 * so, create all required dirs.
 */
static int
make_dirs(char *pathname)
{
	register char *p;		/* Points into path */
	int madeone = 0;		/* Did we do anything yet? */
	int save_errno = errno;		/* Remember caller's errno */

	if (errno != ENOENT)
		return 0;		/* Not our problem */

	for (p = strchr(pathname, '/'); p != NULL; p = strchr(p+1, '/')) {
		/* Avoid mkdir of empty string, if leading or double '/' */
		if (p == pathname || p[-1] == '/')
			continue;
		/* Avoid mkdir where last part of path is '.' */
		if (p[-1] == '.' && (p == pathname+1 || p[-2] == '/'))
			continue;
		*p = 0;				/* Truncate the path there */
		if ( !mkdir(pathname, 0777)) {	/* Try to create it as a dir */
			lrzsz_log(LOG_INFO,NULL,"Made directory %s\n", pathname);
			madeone++;		/* Remember if we made one */
			*p = '/';
			continue;
		}
		*p = '/';
		if (errno == EEXIST)		/* Directory already exists */
			continue;
		/*
		 * Some other error in the mkdir.  We return to the caller.
		 */
		break;
	}
	errno = save_errno;		/* Restore caller's errno */
	return madeone;			/* Tell them to retry if we made one */
}

#endif /* ENABLE_MKDIR */

/*
 * Putsec writes the n characters of buf to receive file fout.
 *  If not in binary mode, carriage returns, and all characters
 *  starting with CPMEOF are discarded.
 */
static int 
putsec(struct zm_fileinfo *zi, char *buf, size_t n)
{
	register char *p;

	if (n == 0)
		return OK;
	if (Thisbinary) {
		if (fwrite(buf,n,1,fout)!=1)
			return ERROR;
	}
	else {
		if (zi->eof_seen)
			return OK;
		for (p=buf; n>0; ++p,n-- ) {
			if ( *p == '\r')
				continue;
			if (*p == CPMEOF) {
				zi->eof_seen=TRUE;
				return OK;
			}
			putc(*p ,fout);
		}
	}
	return OK;
}

/* make string s lower case */
static void
uncaps(char *s)
{
	for ( ; *s; ++s)
		if (isupper((unsigned char)(*s)))
			*s = tolower(*s);
}
/*
 * IsAnyLower returns TRUE if string s has lower case letters.
 */
static int 
IsAnyLower(const char *s)
{
	for ( ; *s; ++s)
		if (islower((unsigned char)(*s)))
			return TRUE;
	return FALSE;
}

static void
report(int sct)
{
	lrzsz_log(LOG_DEBUG,NULL,"Blocks received: %d",sct);
}

/*
 * If called as [-][dir/../]vrzCOMMAND set Verbose to 1
 * If called as [-][dir/../]rzCOMMAND set the pipe flag
 * If called as rb use YMODEM protocol
 */
static void
chkinvok(const char *s)
{
	const char *p;

	p = s;
	while (*p == '-')
		s = ++p;
	while (*p)
		if (*p++ == '/')
			s = p;
	if (*s == 'v') {
		Verbose=1; ++s;
	}
	program_name = s;
	if (*s == 'l') 
		s++; /* lrz -> rz */
	protocol=ZM_ZMODEM;
	if (s[0]=='r' && s[1]=='x')
		protocol=ZM_XMODEM;
	if (s[0]=='r' && (s[1]=='b' || s[1]=='y'))
		protocol=ZM_YMODEM;
	if (s[2] && protocol!=ZM_XMODEM)
		Topipe = 1;
}

/*
 * Totalitarian Communist pathname processing
 */
static void 
checkpath(const char *name)
{
	if (Restricted) {
		const char *p;
		p=strrchr(name,'/');
		if (p)
			p++;
		else
			p=name;
		/* don't overwrite any file in very restricted mode.
		 * don't overwrite hidden files in restricted mode */
		if ((Restricted==2 || *name=='.') && fopen(name, "r") != NULL) {
			canit(STDOUT_FILENO);
			lrzsz_log(LOG_WARNING,NULL,"%s exists / is hidden", name);
			bibi(-1);
		}
		/* restrict pathnames to current directory  */
		if ( strstr(name, "../")) {
			canit(STDOUT_FILENO);
			lrzsz_log(LOG_WARNING,NULL,"%s contains ../", name);
			bibi(-1);
		}
		if (Restricted > 1) {
			if (name[0]=='.' || strstr(name,"/.")) {
				canit(STDOUT_FILENO);
				lrzsz_log(LOG_WARNING,NULL,"%s: security violation attempted contains ../", name);
				bibi(-1);
			}
		}
	}
}

/*
 * Initialize for Zmodem receive attempt, try to activate Zmodem sender
 *  Handles ZSINIT frame
 *  Return ZFILE if Zmodem filename received, -1 on error,
 *   ZCOMPL if transaction finished,  else 0
 */
static int
tryz(void)
{
	register int c, n;
	int zrqinits_received=0;
	size_t bytes_in_block=0;

	if (protocol!=ZM_ZMODEM)		/* Check for "rb" program name */
		return 0;

	for (n=zmodem_requested?15:5; 
		 (--n + zrqinits_received) >=0 && zrqinits_received<10; ) {
		/* Set buffer length (0) and capability flags */
		stohdr(0L);
#ifdef HAVE_TCSENDBREAK
		Txhdr[ZF0] = CANFC32|CANFDX|CANOVIO|CANBRK;
#else
		Txhdr[ZF0] = CANFC32|CANFDX|CANOVIO;
#endif
		if (Zctlesc)
			Txhdr[ZF0] |= TESCCTL; /* TESCCTL == ESCCTL */
		zshhdr(tryzhdrtype, Txhdr);

		if (tryzhdrtype == ZSKIP)	/* Don't skip too far */
			tryzhdrtype = ZRINIT;	/* CAF 8-21-87 */
again:
		switch (zgethdr(Rxhdr, 0, NULL, &config)) {
		case ZRQINIT:
			/* getting one ZRQINIT is totally ok. Normally a ZFILE follows 
			 * (and might be in our buffer, so don't purge it). But if we
			 * get more ZRQINITs than the sender has started up before us
			 * and sent ZRQINITs while waiting. 
			 */
			zrqinits_received++;
			continue;
		
		case ZEOF:
			continue;
		case TIMEOUT:
			continue;
		case ZFILE:
			zconv = Rxhdr[ZF0];
			if (!zconv)
				/* resume with sz -r is impossible (at least with unix sz)
				 * if this is not set */
				zconv=ZCBIN;
			if (Rxhdr[ZF1] & ZF1_ZMSKNOLOC) {
				Rxhdr[ZF1] &= ~(ZF1_ZMSKNOLOC);
				skip_if_not_found=TRUE;
			}
			zmanag = Rxhdr[ZF1];
			ztrans = Rxhdr[ZF2];
			tryzhdrtype = ZRINIT;
			c = zrdata(secbuf, MAX_BLOCK,&bytes_in_block);
			lrzsz_iomode(0,3, &config);
			if (c == GOTCRCW)
				return ZFILE;
			zshhdr(ZNAK, Txhdr);
			goto again;
		case ZSINIT:
			/* this once was:
			 * Zctlesc = TESCCTL & Rxhdr[ZF0];
			 * trouble: if rz get --escape flag:
			 * - it sends TESCCTL to sz, 
			 *   get a ZSINIT _without_ TESCCTL (yeah - sender didn't know), 
			 *   overwrites Zctlesc flag ...
			 * - sender receives TESCCTL and uses "|=..."
			 * so: sz escapes, but rz doesn't unescape ... not good.
			 */
			Zctlesc |= TESCCTL & Rxhdr[ZF0];
			if (zrdata(Attn, ZATTNLEN,&bytes_in_block) == GOTCRCW) {
				stohdr(1L);
				zshhdr(ZACK, Txhdr);
				goto again;
			}
			zshhdr(ZNAK, Txhdr);
			goto again;
		case ZFREECNT:
			stohdr(getfree());
			zshhdr(ZACK, Txhdr);
			goto again;
		case ZCOMMAND:
			if (zrdata(secbuf, MAX_BLOCK,&bytes_in_block) == GOTCRCW) {
				secbuf[MAX_BLOCK-1]=0;
				lrzsz_log(LOG_WARNING,NULL, "received execution request: %s",
					secbuf);
				return ERROR;
			}
			zshhdr(ZNAK, Txhdr);
			goto again;
		case ZCOMPL:
			goto again;
		default:
			continue;
		case ZFIN:
			ackbibi();
			return ZCOMPL;
		case ZRINIT:
			lrzsz_log(LOG_DEBUG,NULL,"got ZRINIT");
			return ERROR;
		case ZCAN:
			lrzsz_log(LOG_DEBUG,NULL,"got ZCAN");
			return ERROR;
		}
	}
	return 0;
}


/*
 * Receive 1 or more files with ZMODEM protocol
 */
static int
rzfiles(struct zm_fileinfo *zi)
{
	register int c;

	for (;;) {
		timing(1,NULL);
		c = rzfile(zi);
		switch (c) {
		case ZEOF:
	 		{
				double d;
				long bps;
				d=timing(0,NULL);
				if (d==0)
					d=0.5; /* can happen if timing uses time() */
				bps=(zi->bytes_received-zi->bytes_skipped)/d;
				lrzsz_log(LOG_INFO, zi, "%ld Bytes, %ld BPS", (long) zi->bytes_total,bps);
			}
			/* FALL THROUGH */
		case ZSKIP:
			if (c==ZSKIP)
			{
				lrzsz_log(LOG_NOTICE, zi, "skipped");
			}
			switch (tryz()) {
			case ZCOMPL:
				return OK;
			default:
				return ERROR;
			case ZFILE:
				break;
			}
			continue;
		default:
			return c;
		case ERROR:
			lrzsz_log(LOG_NOTICE, zi, "error");
			return ERROR;
		}
	}
}

/* "OOSB" means Out Of Sync Block. I once thought that if sz sents
 * blocks a,b,c,d, of which a is ok, b fails, we might want to save 
 * c and d. But, alas, i never saw c and d.
 */
#define SAVE_OOSB
#ifdef SAVE_OOSB
typedef struct oosb_t {
	size_t pos;
	size_t len;
	char *data;
	struct oosb_t *next;
} oosb_t;
struct oosb_t *anker=NULL;
#endif

/*
 * Receive a file with ZMODEM protocol
 *  Assumes file name frame is in secbuf
 */
static int
rzfile(struct zm_fileinfo *zi)
{
	register int c, n;
	long last_rxbytes=0;
	unsigned long last_bps=0;
	long not_printed=0;
	time_t low_bps=0;
	size_t bytes_in_block=0;

	zi->eof_seen=FALSE;

	n = 20;

	if (procheader(secbuf,zi) == ERROR) {
		lrzsz_log(LOG_ERR, zi," procheader error");
		return (tryzhdrtype = ZSKIP);
	}

	for (;;) {
		stohdr(zi->bytes_received);
		zshhdr(ZRPOS, Txhdr);
		goto skip_oosb;
nxthdr:
#ifdef SAVE_OOSB
		if (anker) {
			oosb_t *akt,*last,*next;
			for (akt=anker,last=NULL;akt;last= akt ? akt : last ,akt=next) {
				if (akt->pos==zi->bytes_received) {
					putsec(zi, akt->data, akt->len);
					zi->bytes_received += akt->len;
					lrzsz_log(LOG_NOTICE,zi,"using saved out-of-sync-paket %lx, len %ld",
						  akt->pos,akt->len);
					goto nxthdr;
				}
				next=akt->next;
				if (akt->pos<zi->bytes_received) {
					lrzsz_log(LOG_NOTICE,zi,"removing unneeded saved out-of-sync-paket %lx, len %ld",
						  akt->pos,akt->len);
					if (last)
						last->next=akt->next;
					else
						anker=akt->next;
					free(akt->data);
					free(akt);
					akt=NULL;
				}
			}
		}
#endif
	skip_oosb:
		c = zgethdr(Rxhdr, 0, NULL, &config);
		switch (c) {
		default:
			lrzsz_log(LOG_NOTICE, zi, "zgethdr returned %d", c);
			return ERROR;
		case ZNAK:
		case TIMEOUT:
			if ( --n < 0) {
				lrzsz_log(LOG_INFO, zi, "zgethdr returned %s",
					   c == ZNAK ? "ZNAK" : "TIMEOUT");
				return ERROR;
			}
		case ZFILE:
			zrdata(secbuf, MAX_BLOCK,&bytes_in_block);
			continue;
		case ZEOF:
			if (rclhdr(Rxhdr) != (long) zi->bytes_received) {
				/*
				 * Ignore eof if it's at wrong place - force
				 *  a timeout because the eof might have gone
				 *  out before we sent our zrpos.
				 */
				errors = 0;  goto nxthdr;
			}
			int err = closeit(zi);
			if (err) {
				tryzhdrtype = ZFERR;
				lrzsz_log(LOG_INFO, zi, "closeit failed: %sd", strerror(err));
				return ERROR;
			}
			// normal EOF
			return c;
		case ERROR:	/* Too much garbage in header search error */
			if ( --n < 0) {
				lrzsz_log(LOG_INFO, zi, "zgethdr returned %d", c);
				return ERROR;
			}
			zmputs(Attn);
			continue;
		case ZSKIP:
			closeit(zi);
			lrzsz_log(LOG_INFO, zi, "sender skipped");
			return c;
		case ZDATA:
			if (rclhdr(Rxhdr) != (long) zi->bytes_received) {
#if defined(SAVE_OOSB)
				oosb_t *neu;
				size_t pos=rclhdr(Rxhdr);
#endif
				if ( --n < 0) {
					lrzsz_log(LOG_NOTICE, zi, "out of sync");
					return ERROR;
				}
#if defined(SAVE_OOSB)
				switch (c = zrdata(secbuf, MAX_BLOCK,&bytes_in_block))
				{
				case GOTCRCW:
				case GOTCRCG:
				case GOTCRCE:
				case GOTCRCQ:
					if (pos>zi->bytes_received) {
						neu=malloc(sizeof(oosb_t));
						if (neu)
							neu->data=malloc(bytes_in_block);
						if (neu && neu->data) {
/* call syslog to tell me if this happens */
							lrzsz_log(LOG_NOTICE, zi, 
								   "saving out-of-sync-block %lx, len %lu",
								   pos, (unsigned long) bytes_in_block);
							memcpy(neu->data,secbuf,bytes_in_block);
							neu->pos=pos;
							neu->len=bytes_in_block;
							neu->next=anker;
							anker=neu;
						}
						else if (neu)
							free(neu);
					}
				}
#endif
				zmputs(Attn);  continue;
			}
moredata:
			if (not_printed > (min_bps ? 3 : 7) 
					|| zi->bytes_received > last_bps / 2 + last_rxbytes) {
				int minleft =  0;
				int secleft =  0;
				time_t now;
				double d;
				d=timing(0,&now);
				if (d==0)
					d=0.5; /* timing() might use time() */
				last_bps=zi->bytes_received/d;
				if (last_bps > 0) {
					minleft =  (R_BYTESLEFT(zi))/last_bps/60;
					secleft =  ((R_BYTESLEFT(zi))/last_bps)%60;
				}
				if (min_bps) {
					if (low_bps) {
						if (last_bps<min_bps) {
							if (now-low_bps>=min_bps_time) {
								/* too bad */
								lrzsz_log(LOG_NOTICE, zi, "bps rate too low: %ld < %ld",
										   last_bps, min_bps);
								return ERROR;
							}
						}
						else
							low_bps=0;
					} else if (last_bps<min_bps) {
						low_bps=now;
					}
				}
				if (stop_time && now>=stop_time) {
					/* too bad */
					lrzsz_log(LOG_NOTICE,zi, "reached stop time");
					return ERROR;
				}
				
				lrzsz_log(LOG_INFO|L_PROGRESS,zi,
					"Bytes received: %7ld/%7ld   BPS:%-6ld ETA %02d:%02d",
						(long) zi->bytes_received, (long) zi->bytes_total, 
						last_bps, minleft, secleft);
				last_rxbytes=zi->bytes_received;
				not_printed=0;
			} else if (Verbose)
				not_printed++;

			switch (c = zrdata(secbuf, MAX_BLOCK,&bytes_in_block)) {
			case ZCAN:
				lrzsz_log(LOG_ERR, zi, "zrdata returned ZCAN", c);
				return ERROR;
			case ERROR:	/* CRC error */
				if ( --n < 0) {
					lrzsz_log(LOG_ERR, zi, "zrdata returned %d", c);
					return ERROR;
				}
				zmputs(Attn);
				continue;
			case TIMEOUT:
				if ( --n < 0) {
					lrzsz_log(LOG_NOTICE, zi, "zrdata returned TIMEOUT");
					return ERROR;
				}
				continue;
			case GOTCRCW:
				n = 20;
				putsec(zi, secbuf, bytes_in_block);
				zi->bytes_received += bytes_in_block;
				stohdr(zi->bytes_received);
				zshhdr(ZACK | 0x80, Txhdr);
				goto nxthdr;
			case GOTCRCQ:
				n = 20;
				putsec(zi, secbuf, bytes_in_block);
				zi->bytes_received += bytes_in_block;
				stohdr(zi->bytes_received);
				zshhdr(ZACK, Txhdr);
				goto moredata;
			case GOTCRCG:
				n = 20;
				putsec(zi, secbuf, bytes_in_block);
				zi->bytes_received += bytes_in_block;
				goto moredata;
			case GOTCRCE:
				n = 20;
				putsec(zi, secbuf, bytes_in_block);
				zi->bytes_received += bytes_in_block;
				goto nxthdr;
			}
		}
	}
}

/*
 * Send a string to the modem, processing for \336 (sleep 1 sec)
 *   and \335 (break signal)
 */
static void
zmputs(const char *s)
{
	const char *p;

	while (s && *s)
	{
		p=strpbrk(s,"\335\336");
		if (!p)
		{
			write(1,s,strlen(s));
			return;
		}
		if (p!=s)
		{
			write(1,s,(size_t) (p-s));
			s=p;
		}
		if (*p=='\336')
			sleep(1);
		else
			sendbrk(0);
		p++;
	}
}

/*
 * Close the receive dataset, return OK or ERROR
 */
static int
closeit(struct zm_fileinfo *zi)
{
	int ret;

	mode_t mode=0600;

	if (Topipe) {
		if (pclose(fout)) {
			return errno;
		}
		return 0;
	}
	if (S_ISREG(zi->mode)) {
		/* we must not make this program executable if running 
		 * under rsh, because the user might have uploaded an
		 * unrestricted shell.
		 */
		if (under_rsh)
			mode = (00666 & zi->mode);
		else
			mode = (07777 & zi->mode);
	}
	ret = close_and_update_meta(fout, Pathname, zi->modtime, mode);
	if (ret) {
		int e=errno;
		lrzsz_log(LOG_ERR, zi, "file close error");
		/* this may be any sort of error, including random data corruption */
		unlink(Pathname);
		return e;
	}
	return 0;
}

/*
 * Ack a ZFIN packet, let byegones be byegones
 */
static void
ackbibi(void)
{
	int n;

	Readnum = 1;
	stohdr(0L);
	for (n=3; --n>=0; ) {
		purgeline(0);
		zshhdr(ZFIN, Txhdr);
		switch (READLINE_PF(100)) {
		case 'O':
			READLINE_PF(1);	/* Discard 2nd 'O' */
			return;
		case RCDO:
			return;
		case TIMEOUT:
		default:
			break;
		}
	}
}

/*
 * Routine to calculate the free bytes on the current file system
 *  ~0 means many free bytes (unknown)
 */
static size_t 
getfree(void)
{
	return((size_t) (~0L));	/* many free bytes ... */
}

/* End of lrz.c */
