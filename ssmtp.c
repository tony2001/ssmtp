/*

 $Id: ssmtp.c,v 2.60 2003/08/17 14:17:57 matt Exp $

 sSMTP -- send messages via SMTP to a mailhub for local delivery or forwarding.
 This program is used in place of /usr/sbin/sendmail, called by "mail" (et all).
 sSMTP does a selected subset of sendmail's standard tasks (including exactly
 one rewriting task), and explains if you ask it to do something it can't. It
 then sends the mail to the mailhub via an SMTP connection. Believe it or not,
 this is nothing but a filter

 See COPYRIGHT for the license

*/
#define VERSION "2.60.4"

#include <sys/socket.h>
#include <netinet/in.h>
#include <sys/param.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdarg.h>
#include <syslog.h>
#include <signal.h>
#include <setjmp.h>
#include <string.h>
#include <ctype.h>
#include <netdb.h>
#ifdef HAVE_SSL
#include <openssl/crypto.h>
#include <openssl/x509.h>
#include <openssl/pem.h>
#include <openssl/ssl.h>
#include <openssl/err.h>
#endif
#ifdef MD5AUTH
#include "md5auth/hmac_md5.h"
#endif
#include <fcntl.h>
#include <poll.h>
#include <errno.h>
#include "ssmtp.h"

#define CONNECT_TIMEOUT
#define READ_TIMEOUT
#define WRITE_TIMEOUT

bool_t have_date = False;
bool_t have_from = False;
#ifdef HASTO_OPTION
bool_t have_to = False;
#endif
bool_t minus_t = False;
bool_t minus_v = False;
bool_t override_from = False;
bool_t rewrite_domain = False;
bool_t use_tls = False;			/* Use SSL to transfer mail to HUB */
bool_t use_starttls = False;		/* SSL only after STARTTLS (RFC2487) */
bool_t use_cert = False;		/* Use a certificate to transfer SSL mail */

#define ARPADATE_LENGTH 32		/* Current date in RFC format */
char arpadate[ARPADATE_LENGTH];
char *auth_user = NULL;
char *auth_pass = NULL;
char *auth_method = NULL;		/* Mechanism for SMTP authentication */
char *mail_domain = NULL;
char *from = NULL;		/* Use this as the From: address */
char hostname[MAXHOSTNAMELEN] = "localhost";
char *mailhost = "mailhub";
int mailhost_cmdline = 0;
char *minus_f = NULL;
char *minus_F = NULL;
char *gecos;
char *prog = NULL;
char *root = NULL;
char *tls_cert = "/etc/ssl/certs/ssmtp.pem";	/* Default Certificate */
char *uad = NULL;

int connect_timeout = 3000; /* 3 sec */
int read_timeout = 3000; /* 3 sec */
int write_timeout = 3000; /* 3 sec */

headers_t headers, *ht;

#ifdef DEBUG
int log_level = 1;
#else
int log_level = 0;
#endif
int port = 25;
#ifdef INET6
int p_family = PF_UNSPEC;		/* Protocol family used in SMTP connection */
#endif

jmp_buf TimeoutJmpBuf;			/* Timeout waiting for input from network */

rcpt_t rcpt_list, *rt;

#ifdef HAVE_SSL
SSL *ssl;
#endif

#ifdef MD5AUTH
static char hextab[]="0123456789abcdef";
#endif

static char *config_file_path = CONFIGURATION_FILE;

enum {
	SSMTP_POLL_SUCCESS,
	SSMTP_POLL_FAILURE,
	SSMTP_POLL_TIMEOUT
};

int ssmtp_poll(int sock, int op, int timeout_msec) /* {{{ */
{
	struct pollfd fds[1];
	int poll_res;

	memset(&fds, 0, sizeof(struct pollfd));
	fds[0].fd = sock;
	fds[0].events = op | POLLERR;

	errno = 0;
	poll_res = poll(fds, 1, timeout_msec);

	switch (poll_res) {
		case 0: /* timeout */
			return SSMTP_POLL_TIMEOUT;
		case 1: /* success */
			return SSMTP_POLL_SUCCESS;
	}
	/* error */
	return SSMTP_POLL_FAILURE;
}
/* }}} */

/*
log_event() -- Write event to syslog (or log file if defined)
*/
void log_event(int priority, char *format, ...)
{
	char buf[(BUF_SZ + 1)];
	va_list ap;

	va_start(ap, format);
	(void)vsnprintf(buf, BUF_SZ, format, ap);
	va_end(ap);

#ifdef LOGFILE
	FILE *fp;

	if((fp = fopen("/tmp/ssmtp.log", "a")) != (FILE *)NULL) {
		(void)fprintf(fp, "%s\n", buf);
		(void)fclose(fp);
	}
	else {
		(void)fprintf(stderr, "Can't write to /tmp/ssmtp.log\n");
	}
#endif

#if HAVE_SYSLOG_H
#if OLDSYSLOG
	openlog("sSMTP", LOG_PID);
#else
	openlog("sSMTP", LOG_PID, LOG_MAIL);
#endif
	syslog(priority, "%s", buf);
	closelog();
#endif
}

void smtp_write(int fd, char *format, ...);
int smtp_read(int fd, char *response);
int smtp_read_all(int fd, char *response);
int smtp_okay(int fd, char *response);

/*
dead_letter() -- Save stdin to ~/dead.letter if possible
*/
void dead_letter(void)
{
	char path[(MAXPATHLEN + 1)], buf[(BUF_SZ + 1)];
	struct passwd *pw;
	uid_t uid;
	FILE *fp;

	uid = getuid();
	pw = getpwuid(uid);

	if(isatty(fileno(stdin))) {
		if(log_level > 0) {
			log_event(LOG_ERR,
				"stdin is a TTY - not saving to %s/dead.letter, pw->pw_dir");
		}
		return;
	}

	if(pw == (struct passwd *)NULL) {
		/* Far to early to save things */
		if(log_level > 0) {
			log_event(LOG_ERR, "No sender failing horribly!");
		}
		return;
	}

	if(snprintf(path, BUF_SZ, "%s/dead.letter", pw->pw_dir) == -1) {
		/* Can't use die() here since dead_letter() is called from die() */
		exit(1);
	}

	if((fp = fopen(path, "a")) == (FILE *)NULL) {
		/* Perhaps the person doesn't have a homedir... */
		if(log_level > 0) {
			log_event(LOG_ERR, "Can't open %s failing horribly!", path);
		}
		return;
	}

	/* We start on a new line with a blank line separating messages */
	(void)fprintf(fp, "\n\n");

	while(fgets(buf, sizeof(buf), stdin)) {
		(void)fputs(buf, fp);
	}

	if(fclose(fp) == -1) {
		if(log_level > 0) {
			log_event(LOG_ERR,
				"Can't close %s/dead.letter, possibly truncated", pw->pw_dir);
		}
	}
}

/*
die() -- Write error message, dead.letter and exit
*/
void die(char *format, ...)
{
	char buf[(BUF_SZ + 1)];
	va_list ap;

	va_start(ap, format);
	(void)vsnprintf(buf, BUF_SZ, format, ap);
	va_end(ap);

	(void)fprintf(stderr, "%s: %s\n", prog, buf);
	log_event(LOG_ERR, "%s", buf);

	/* Send message to dead.letter */
	(void)dead_letter();

	exit(1);
}

/*
basename() -- Return last element of path
*/
char *basename(char *str)
{
	char buf[MAXPATHLEN +1], *p;

	if((p = strrchr(str, '/'))) {
		if(strncpy(buf, ++p, MAXPATHLEN) == (char *)NULL) {
			die("basename() -- strncpy() failed");
		}
	}
	else {
		if(strncpy(buf, str, MAXPATHLEN) == (char *)NULL) {
			die("basename() -- strncpy() failed");
		}
	}
	buf[MAXPATHLEN] = '\0';

	return(strdup(buf));
}

/*
strip_pre_ws() -- Return pointer to first non-whitespace character
*/
char *strip_pre_ws(char *str)
{
	char *p;

	p = str;
	while(*p && isspace(*p)) p++;

	return(p);
}

/*
strip_post_ws() -- Return pointer to last non-whitespace character
*/
char *strip_post_ws(char *str)
{
	char *p;

	p = (str + strlen(str));
	while(isspace(*--p)) {
		*p = '\0';
	}

	return(p);
}

/*
addr_parse() -- Parse <user@domain.com> from full email address
*/
char *addr_parse(char *str)
{
	char *p, *q;

#if 0
	(void)fprintf(stderr, "*** addr_parse(): str = [%s]\n", str);
#endif

	/* Simple case with email address enclosed in <> */
	if((p = strdup(str)) == (char *)NULL) {
		die("addr_parse(): strdup()");
	}

	if((q = strchr(p, '<'))) {
		q++;

		if((p = strchr(q, '>'))) {
			*p = '\0';
		}

#if 0
		(void)fprintf(stderr, "*** addr_parse(): q = [%s]\n", q);
#endif

		return(q);
	}

	q = strip_pre_ws(p);
	if(*q == '(') {
		while((*q++ != ')'));
	}
	p = strip_pre_ws(q);

#if 0
	(void)fprintf(stderr, "*** addr_parse(): p = [%s]\n", p);
#endif

	q = strip_post_ws(p);
	if(*q == ')') {
		while((*--q != '('));
		*q = '\0';
	}
	(void)strip_post_ws(p);

#if 0
	(void)fprintf(stderr, "*** addr_parse(): p = [%s]\n", p);
#endif

	return(p);
}

/*
append_domain() -- Fix up address with @domain.com
*/
char *append_domain(char *str)
{
	char buf[(BUF_SZ + 1)];

	if(strchr(str, '@') == (char *)NULL) {
		if(snprintf(buf, BUF_SZ, "%s@%s", str,
#ifdef REWRITE_DOMAIN
			rewrite_domain == True ? mail_domain : hostname
#else
			hostname
#endif
														) == -1) {
				die("append_domain() -- snprintf() failed");
		}
		return(strdup(buf));
	}

	return(strdup(str));
}

/*
standardise() -- Trim off '\n's and double leading dots
*/
void standardise(char *str)
{
	size_t sl;
	char *p;

	if((p = strchr(str, '\n'))) {
		*p = '\0';
	}

	/* Any line beginning with a dot has an additional dot inserted;
	not just a line consisting solely of a dot. Thus we have to slide
	the buffer down one */
	sl = strlen(str);

	if(*str == '.') {
		if((sl + 2) > BUF_SZ) {
			die("standardise() -- Buffer overflow");
		}
		(void)memmove((str + 1), str, (sl + 1));	/* Copy trailing \0 */

		*str = '.';
	}
}

/*
revaliases() -- Parse the reverse alias file
	Fix globals to use any entry for sender
*/
void revaliases(struct passwd *pw)
{
	char buf[(BUF_SZ + 1)], *p;
	FILE *fp;

	/* Try to open the reverse aliases file */
	if((fp = fopen(REVALIASES_FILE, "r"))) {
		/* Search if a reverse alias is defined for the sender */
		while(fgets(buf, sizeof(buf), fp)) {
			/* Make comments invisible */
			if((p = strchr(buf, '#'))) {
				*p = '\0';
			}

			/* Ignore malformed lines and comments */
			if(strchr(buf, ':') == (char *)NULL) {
				continue;
			}

			/* Parse the alias */
			if(((p = strtok(buf, ":"))) && !strcmp(p, pw->pw_name)) {
				if((p = strtok(NULL, ": \t\r\n"))) {
					if((uad = strdup(p)) == (char *)NULL) {
						die("revaliases() -- strdup() failed");
					}
				}

				if((p = strtok(NULL, " \t\r\n:"))) {
					if((mailhost = strdup(p)) == (char *)NULL) {
						die("revaliases() -- strdup() failed");
					}

					if((p = strtok(NULL, " \t\r\n:"))) {
						port = atoi(p);
					}

					if(log_level > 0) {
						log_event(LOG_INFO, "Set MailHub=\"%s\"\n", mailhost);
						log_event(LOG_INFO,
							"via SMTP Port Number=\"%d\"\n", port);
					}
				}
			}
		}

		fclose(fp);
	}
}

/* 
from_strip() -- Transforms "Name <login@host>" into "login@host" or "login@host (Real name)"
*/
char *from_strip(char *str)
{
	char *p;

#if 0
	(void)fprintf(stderr, "*** from_strip(): str = [%s]\n", str);
#endif

	if(strncmp("From:", str, 5) == 0) {
		str += 5;
	}

	/* Remove the real name if necessary - just send the address */
	if((p = addr_parse(str)) == (char *)NULL) {
		die("from_strip() -- addr_parse() failed");
	}
#if 0
	(void)fprintf(stderr, "*** from_strip(): p = [%s]\n", p);
#endif

	return(strdup(p));
}

/*
from_format() -- Generate standard From: line
*/
char *from_format(char *str, bool_t override_from)
{
	char buf[(BUF_SZ + 1)];

	if(override_from) {
		if(minus_f) {
			str = append_domain(minus_f);
		}

		if(minus_F) {
			if(snprintf(buf,
				BUF_SZ, "\"%s\" <%s>", minus_F, str) == -1) {
				die("from_format() -- snprintf() failed");
			}
		}
		else if(gecos) {
			if(snprintf(buf, BUF_SZ, "\"%s\" <%s>", gecos, str) == -1) {
				die("from_format() -- snprintf() failed");
			}
		}
		else {
			if(snprintf(buf, BUF_SZ, "%s", str) == -1) {
				die("from_format() -- snprintf() failed");
			}
		}
	}
	else {
		if(gecos) {
			if(snprintf(buf, BUF_SZ, "\"%s\" <%s>", gecos, str) == -1) {
				die("from_format() -- snprintf() failed");
			}
		}
	}

#if 0
	(void)fprintf(stderr, "*** from_format(): buf = [%s]\n", buf);
#endif

	return(strdup(buf));
}

/*
rcpt_save() -- Store entry into RCPT list
*/
void rcpt_save(char *str)
{
	char *p;

# if 1
	/* Horrible botch for group stuff */
	p = str;
	while(*p) p++;

	if(*--p == ';') {
		return;
	}
#endif

#if 0
	(void)fprintf(stderr, "*** rcpt_save(): str = [%s]\n", str);
#endif

	/* Ignore missing usernames */
	if(*str == '\0') {
		return;
	}

	if((rt->string = strdup(str)) == (char *)NULL) {
		die("rcpt_save() -- strdup() failed");
	}

	rt->next = (rcpt_t *)malloc(sizeof(rcpt_t));
	if(rt->next == (rcpt_t *)NULL) {
		die("rcpt_save() -- malloc() failed");
	}
	rt = rt->next;

	rt->next = (rcpt_t *)NULL;
}

/*
rcpt_parse() -- Break To|Cc|Bcc into individual addresses
*/
void rcpt_parse(char *str)
{
	bool_t in_quotes = False, got_addr = False;
	char *p, *q, *r;

#if 0
	(void)fprintf(stderr, "*** rcpt_parse(): str = [%s]\n", str);
#endif

	if((p = strdup(str)) == (char *)NULL) {
		die("rcpt_parse(): strdup() failed");
	}
	q = p;

	/* Replace <CR>, <LF> and <TAB> */
	while(*q) {
		switch(*q) {
			case '\t':
			case '\n':
			case '\r':
					*q = ' ';
		}
		q++;
	}
	q = p;

#if 0
	(void)fprintf(stderr, "*** rcpt_parse(): q = [%s]\n", q);
#endif

	r = q;
	while(*q) {
		if(*q == '"') {
			in_quotes = (in_quotes ? False : True);
		}

		/* End of string? */
		if(*(q + 1) == '\0') {
			got_addr = True;
		}

		/* End of address? */
		if((*q == ',') && (in_quotes == False)) {
			got_addr = True;

			*q = '\0';
		}

		if(got_addr) {
			while(*r && isspace(*r)) r++;

			rcpt_save(addr_parse(r));
			r = (q + 1);
#if 0
			(void)fprintf(stderr, "*** rcpt_parse(): r = [%s]\n", r);
#endif
			got_addr = False;
		}
		q++;
	}
	free(p);
}

#ifdef MD5AUTH
int crammd5(char *challengeb64, char *username, char *password, char *responseb64)
{
	int i;
	unsigned char digest[MD5_DIGEST_LEN];
	unsigned char digascii[MD5_DIGEST_LEN * 2];
	unsigned char challenge[(BUF_SZ + 1)];
	unsigned char response[(BUF_SZ + 1)];
	unsigned char secret[(MD5_BLOCK_LEN + 1)]; 

	memset (secret,0,sizeof(secret));
	memset (challenge,0,sizeof(challenge));
	strncpy (secret, password, sizeof(secret));	
	if (!challengeb64 || strlen(challengeb64) > sizeof(challenge) * 3 / 4)
		return 0;
	from64tobits(challenge, challengeb64);

	hmac_md5(challenge, strlen(challenge), secret, strlen(secret), digest);

	for (i = 0; i < MD5_DIGEST_LEN; i++) {
		digascii[2 * i] = hextab[digest[i] >> 4];
		digascii[2 * i + 1] = hextab[(digest[i] & 0x0F)];
	}
	digascii[MD5_DIGEST_LEN * 2] = '\0';

	if (sizeof(response) <= strlen(username) + sizeof(digascii))
		return 0;
	
	strncpy (response, username, sizeof(response) - sizeof(digascii) - 2);
	strcat (response, " ");
	strcat (response, digascii);
	to64frombits(responseb64, response, strlen(response));

	return 1;
}
#endif

/*
rcpt_remap() -- Alias systems-level users to the person who
	reads their mail. This is variously the owner of a workstation,
	the sysadmin of a group of stations and the postmaster otherwise.
	We don't just mail stuff off to root on the mailhub :-)
*/
char *rcpt_remap(char *str)
{
	struct passwd *pw;
	if((root==NULL) || strlen(root)==0 || strchr(str, '@') ||
		((pw = getpwnam(str)) == NULL) || (pw->pw_uid > MAXSYSUID)) {
		return(append_domain(str));	/* It's not a local systems-level user */
	}
	else {
		return(append_domain(root));
	}
}

/*
header_save() -- Store entry into header list
*/
void header_save(char *str)
{
	char *p;

#if 0
	(void)fprintf(stderr, "header_save(): str = [%s]\n", str);
#endif

	if((p = strdup(str)) == (char *)NULL) {
		die("header_save() -- strdup() failed");
	}
	ht->string = p;

	if(strncasecmp(ht->string, "From:", 5) == 0) {
#if 1
		/* Hack check for NULL From: line */
		if(*(p + 6) == '\0') {
			return;
		}
#endif

#ifdef REWRITE_DOMAIN
		if(override_from == True) {
			uad = from_strip(ht->string);
		}
		else {
			return;
		}
#endif
		have_from = True;
	}
#ifdef HASTO_OPTION
	else if(strncasecmp(ht->string, "To:" ,3) == 0) {
		have_to = True;
	}
#endif
	else if(strncasecmp(ht->string, "Date:", 5) == 0) {
		have_date = True;
	}

	if(minus_t) {
		/* Need to figure out recipients from the e-mail */
		if(strncasecmp(ht->string, "To:", 3) == 0) {
			p = (ht->string + 3);
			rcpt_parse(p);
		}
		else if(strncasecmp(ht->string, "Bcc:", 4) == 0) {
			p = (ht->string + 4);
			rcpt_parse(p);
		}
		else if(strncasecmp(ht->string, "CC:", 3) == 0) {
			p = (ht->string + 3);
			rcpt_parse(p);
		}
	}

#if 0
	(void)fprintf(stderr, "header_save(): ht->string = [%s]\n", ht->string);
#endif

	ht->next = (headers_t *)malloc(sizeof(headers_t));
	if(ht->next == (headers_t *)NULL) {
		die("header_save() -- malloc() failed");
	}
	ht = ht->next;

	ht->next = (headers_t *)NULL;
}

/*
header_parse() -- Break headers into seperate entries
*/
void header_parse(FILE *stream)
{
	size_t size = BUF_SZ, len = 0;
	char *p = (char *)NULL, *q = NULL;
	bool_t in_header = True;
	char l = 0;
	int c;

	while(in_header && ((c = fgetc(stream)) != EOF)) {
		/* Must have space for up to two more characters, since we
			may need to insert a '\r' */
		if((p == (char *)NULL) || (len >= (size - 1))) {
			size += BUF_SZ;

			p = (char *)realloc(p, (size * sizeof(char)));
			if(p == (char *)NULL) {
				die("header_parse() -- realloc() failed");
			}
			q = (p + len);
		}
		len++;

		if(l == '\n') {
			switch(c) {
				case ' ':
				case '\t':
						/* Must insert '\r' before '\n's embedded in header
						   fields otherwise qmail won't accept our mail
						   because a bare '\n' violates some RFC */
						
						*(q - 1) = '\r';	/* Replace previous \n with \r */
						*q++ = '\n';		/* Insert \n */
						len++;
						
						break;

				case '\n':
						in_header = False;

				default:
						*q = '\0';
						if((q = strrchr(p, '\n'))) {
							*q = '\0';
						}
						header_save(p);

						q = p;
						len = 0;
			}
		}
		*q++ = c;

		l = c;
	}
	(void)free(p);
}

/*
read_config() -- Open and parse config file and extract values of variables
*/
bool_t read_config()
{
	char buf[(BUF_SZ + 1)], *p, *q, *r;
	FILE *fp;

	if((fp = fopen(config_file_path, "r")) == NULL) {
		return(False);
	}

	while(fgets(buf, sizeof(buf), fp)) {
		/* Make comments invisible */
		if((p = strchr(buf, '#'))) {
			*p = '\0';
		}

		/* Ignore malformed lines and comments */
		if(strchr(buf, '=') == (char *)NULL) continue;

		/* Parse out keywords */
		if(((p = strtok(buf, "= \t\n")) != (char *)NULL)
			&& ((q = strtok(NULL, "= \t\n:")) != (char *)NULL)) {
			if(strcasecmp(p, "Root") == 0) {
				if((root = strdup(q)) == (char *)NULL) {
					die("parse_config() -- strdup() failed");
				}

				if(log_level > 0) {
					log_event(LOG_INFO, "Set Root=\"%s\"\n", root);
				}
			}
			else if(strcasecmp(p, "MailHub") == 0 && !mailhost_cmdline) {
				if((mailhost = strdup(q)) == (char *)NULL) {
					die("parse_config() -- strdup() failed");
				}

				if((r = strtok(NULL, "= \t\n:")) != NULL) {
					port = atoi(r);
				}

				if(log_level > 0) {
					log_event(LOG_INFO, "Set MailHub=\"%s\"\n", mailhost);
					log_event(LOG_INFO, "Set RemotePort=\"%d\"\n", port);
				}
			}
			else if(strcasecmp(p, "HostName") == 0) {
				if(strncpy(hostname, q, MAXHOSTNAMELEN) == NULL) {
					die("parse_config() -- strncpy() failed");
				}

				if(log_level > 0) {
					log_event(LOG_INFO, "Set HostName=\"%s\"\n", hostname);
				}
			}
#ifdef REWRITE_DOMAIN
			else if(strcasecmp(p, "RewriteDomain") == 0) {
				if((p = strrchr(q, '@'))) {
					mail_domain = strdup(++p);

					log_event(LOG_ERR,
						"Set RewriteDomain=\"%s\" is invalid\n", q);
					log_event(LOG_ERR,
						"Set RewriteDomain=\"%s\" used\n", mail_domain);
				}
				else {
					mail_domain = strdup(q);
				}

				if(mail_domain == (char *)NULL) {
					die("parse_config() -- strdup() failed");
				}
				rewrite_domain = True;

				if(log_level > 0) {
					log_event(LOG_INFO,
						"Set RewriteDomain=\"%s\"\n", mail_domain);
				}
			}
#endif
			else if(strcasecmp(p, "FromLineOverride") == 0) {
				if(strcasecmp(q, "YES") == 0) {
					override_from = True;
				}
				else {
					override_from = False;
				}

				if(log_level > 0) {
					log_event(LOG_INFO,
						"Set FromLineOverride=\"%s\"\n",
						override_from ? "True" : "False");
				}
			}
			else if(strcasecmp(p, "RemotePort") == 0) {
				port = atoi(q);

				if(log_level > 0) {
					log_event(LOG_INFO, "Set RemotePort=\"%d\"\n", port);
				}
			}
#ifdef HAVE_SSL
			else if(strcasecmp(p, "UseTLS") == 0) {
				if(strcasecmp(q, "YES") == 0) {
					use_tls = True;
				}
				else {
					use_tls = False;
					use_starttls = False;
				}

				if(log_level > 0) { 
					log_event(LOG_INFO,
						"Set UseTLS=\"%s\"\n", use_tls ? "True" : "False");
				}
			}
			else if(strcasecmp(p, "UseSTARTTLS") == 0) {
				if(strcasecmp(q, "YES") == 0) {
					use_starttls = True;
					use_tls = True;
				}
				else {
					use_starttls = False;
				}

				if(log_level > 0) { 
					log_event(LOG_INFO,
						"Set UseSTARTTLS=\"%s\"\n", use_tls ? "True" : "False");
				}
			}
			else if(strcasecmp(p, "UseTLSCert") == 0) {
				if(strcasecmp(q, "YES") == 0) {
					use_cert = True;
				}
				else {
					use_cert = False;
				}

				if(log_level > 0) {
					log_event(LOG_INFO,
						"Set UseTLSCert=\"%s\"\n",
						use_cert ? "True" : "False");
				}
			}
			else if(strcasecmp(p, "TLSCert") == 0) {
				if((tls_cert = strdup(q)) == (char *)NULL) {
					die("parse_config() -- strdup() failed");
				}

				if(log_level > 0) {
					log_event(LOG_INFO, "Set TLSCert=\"%s\"\n", tls_cert);
				}
			}
#endif
			/* Command-line overrides these */
			else if(strcasecmp(p, "AuthUser") == 0 && !auth_user) {
				if((auth_user = strdup(q)) == (char *)NULL) {
					die("parse_config() -- strdup() failed");
				}

				if(log_level > 0) {
					log_event(LOG_INFO, "Set AuthUser=\"%s\"\n", auth_user);
				}
			}
			else if(strcasecmp(p, "AuthPass") == 0 && !auth_pass) {
				if((auth_pass = strdup(q)) == (char *)NULL) {
					die("parse_config() -- strdup() failed");
				}

				if(log_level > 0) {
					log_event(LOG_INFO, "Set AuthPass=\"%s\"\n", auth_pass);
				}
			}
			else if(strcasecmp(p, "AuthMethod") == 0 && !auth_method) {
				if((auth_method = strdup(q)) == (char *)NULL) {
					die("parse_config() -- strdup() failed");
				}

				if(log_level > 0) {
					log_event(LOG_INFO, "Set AuthMethod=\"%s\"\n", auth_method);
				}
			}
			else if (strcasecmp(p, "ConnectTimeout") == 0)
			{
				connect_timeout = atoi(q); 
			}
			else if (strcasecmp(p, "ReadTimeout") == 0)
			{
				read_timeout = atoi(q); 
			}
			else if (strcasecmp(p, "WriteTimeout") == 0)
			{
				write_timeout = atoi(q); 
			}
			else {
				log_event(LOG_INFO, "Unable to set %s=\"%s\"\n", p, q);
			}
		}
	}
	(void)fclose(fp);

	return(True);
}

/*
smtp_open() -- Open connection to a remote SMTP listener
*/
int smtp_open(char *host, int port)
{
	int fd_flags;
#ifdef INET6
	struct addrinfo hints, *ai0, *ai;
	char servname[NI_MAXSERV];
	int s;
#else
	struct sockaddr_in name;
	struct hostent *hent;
	int s, namelen;
#endif

#ifdef HAVE_SSL
	int err;
	char buf[(BUF_SZ + 1)];

	/* Init SSL stuff */
	SSL_CTX *ctx;
	SSL_METHOD *meth;
	X509 *server_cert;

	SSL_load_error_strings();
	SSLeay_add_ssl_algorithms();
	meth=SSLv23_client_method();
	ctx = SSL_CTX_new(meth);
	if(!ctx) {
		log_event(LOG_ERR, "No SSL support initiated\n");
		return(-1);
	}

	if(use_cert == True) { 
		if(SSL_CTX_use_certificate_chain_file(ctx, tls_cert) <= 0) {
			perror("Use certfile");
			return(-1);
		}

		if(SSL_CTX_use_PrivateKey_file(ctx, tls_cert, SSL_FILETYPE_PEM) <= 0) {
			perror("Use PrivateKey");
			return(-1);
		}

		if(!SSL_CTX_check_private_key(ctx)) {
			log_event(LOG_ERR, "Private key does not match the certificate public key\n");
			return(-1);
		}
	}
#endif

#ifdef INET6
	memset(&hints, 0, sizeof(hints));
	hints.ai_family = p_family;
	hints.ai_socktype = SOCK_STREAM;
	snprintf(servname, sizeof(servname), "%d", port);

	/* Check we can reach the host */
	if (getaddrinfo(host, servname, &hints, &ai0)) {
		log_event(LOG_ERR, "Unable to locate %s", host);
		return(-1);
	}

	for (ai = ai0; ai; ai = ai->ai_next) {
		/* Create a socket for the connection */
		s = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
		if (s < 0) {
			continue;
		}

#ifdef CONNECT_TIMEOUT
		fd_flags = fcntl(s, F_GETFL, 0);
		if (fcntl(s, F_SETFL, fd_flags | O_NONBLOCK) != 0) {
			s = -1;
			continue;
		}

		while (1) {
			if (connect(s, ai->ai_addr, ai->ai_addrlen) == 0) {
				goto out_of_loop;
			}

			switch(errno) {
				case EALREADY:
				case EINPROGRESS:
					{
						int poll_res;

						poll_res = ssmtp_poll(s, POLLOUT, connect_timeout);
						if (poll_res == SSMTP_POLL_SUCCESS) {
							/* try again */
							continue;
						}
						s = -1;
						goto out_of_loop;
					}
				case EINTR:
					continue;
				default:
					s = -1;
					goto out_of_loop;
			}
		}
#else
		if (connect(s, ai->ai_addr, ai->ai_addrlen) < 0) {
			s = -1;
			continue;
		}
#endif
		break;
	}

out_of_loop:

	if(s < 0) {
		log_event (LOG_ERR,
			"Unable to connect to \"%s\" port %d.\n", host, port);

		return(-1);
	}
#else
	/* Check we can reach the host */
	if((hent = gethostbyname(host)) == (struct hostent *)NULL) {
		log_event(LOG_ERR, "Unable to locate %s", host);
		return(-1);
	}

	if(hent->h_length > sizeof(hent->h_addr)) {
		log_event(LOG_ERR, "Buffer overflow in gethostbyname()");
		return(-1);
	}

	/* Create a socket for the connection */
	if((s = socket(PF_INET, SOCK_STREAM, IPPROTO_TCP)) < 0) {
		log_event(LOG_ERR, "Unable to create a socket");
		return(-1);
	}

	/* This SHOULD already be in Network Byte Order from gethostbyname() */
	name.sin_addr.s_addr = ((struct in_addr *)(hent->h_addr))->s_addr;
	name.sin_family = hent->h_addrtype;
	name.sin_port = htons(port);

	namelen = sizeof(struct sockaddr_in);

#ifdef CONNECT_TIMEOUT
	fd_flags = fcntl(s, F_GETFL, 0);
	if (fcntl(s, F_SETFL, fd_flags | O_NONBLOCK) != 0) {
		log_event(LOG_ERR, "fcntl(, O_NONBLOCK) failed");
		return(-1);
	}

	while (1) {
		if(connect(s, (struct sockaddr *)&name, namelen) == 0) {
			goto out_of_loop;
		}

		switch(errno) {
			case EALREADY:
			case EINPROGRESS:
				{
					int poll_res;

					poll_res = ssmtp_poll(s, POLLOUT, connect_timeout);
					if (poll_res == SSMTP_POLL_SUCCESS) {
						/* try again */
						continue;
					}
					s = -1;
					log_event(LOG_ERR, "connect(%s:%d) timed out", host, port);
					goto out_of_loop;
				}
			case EINTR:
				continue;
			default:
				s = -1;
				log_event(LOG_ERR, "poll() failed on connect");
				goto out_of_loop;
		}
	}

out_of_loop:
	if (s < 0) {
		log_event(LOG_ERR, "Unable to connect to %s:%d", host, port);
		return(-1);
	}

#else
	if(connect(s, (struct sockaddr *)&name, namelen) < 0) {
		log_event(LOG_ERR, "Unable to connect to %s:%d", host, port);
		return(-1);
	}
#endif
#endif

#ifdef HAVE_SSL
	if(use_tls == True) {
		log_event(LOG_INFO, "Creating SSL connection to host");

		if (use_starttls == True)
		{
			use_tls=False; /* need to write plain text for a while */

			if (smtp_okay(s, buf))
			{
				smtp_write(s, "EHLO %s", hostname);
				if (smtp_okay(s, buf)) {
					smtp_write(s, "STARTTLS"); /* assume STARTTLS regardless */
					if (!smtp_okay(s, buf)) {
						log_event(LOG_ERR, "STARTTLS not working");
						return(-1);
					}
				}
				else
				{
					log_event(LOG_ERR, "Invalid response: %s (%s)", buf, hostname);
				}
			}
			else
			{
				log_event(LOG_ERR, "Invalid response SMTP Server (STARTTLS)");
				return(-1);
			}
			use_tls=True; /* now continue as normal for SSL */
		}

		ssl = SSL_new(ctx);
		if(!ssl) {
			log_event(LOG_ERR, "SSL not working");
			return(-1);
		}
		SSL_set_fd(ssl, s);

		err = SSL_connect(ssl);
		if(err < 0) { 
			perror("SSL_connect");
			return(-1);
		}

		if(log_level > 0 || 1) {
			log_event(LOG_INFO, "SSL connection using %s",
				SSL_get_cipher(ssl));
		}

		server_cert = SSL_get_peer_certificate(ssl);
		if(!server_cert) {
			return(-1);
		}
		X509_free(server_cert);

		/* TODO: Check server cert if changed! */
	}
#endif

	return(s);
}

/*
fd_getc() -- Read a character from an fd
*/
ssize_t fd_getc(int fd, void *c)
{
#ifdef READ_TIMEOUT
	int read_bytes;

	while (1) {
#ifdef HAVE_SSL
		if(use_tls == True) { 
			read_bytes = SSL_read(ssl, c, 1);
		} else {
#endif
			read_bytes = read(fd, c, 1);
#ifdef HAVE_SSL
		}
#endif

		if (read_bytes == -1) {
			switch (errno) {
				case EAGAIN:
					{
						int res;

						res = ssmtp_poll(fd, POLLIN, read_timeout);
						if (res == SSMTP_POLL_SUCCESS) {
							continue;
						}
						log_event(LOG_ERR, "read() timed out");
						return -1;
					}
				default:
					log_event(LOG_ERR, "poll() failed on read");
					return -1;
			}
		} else {
			return read_bytes;
		}
	}
#else
#ifdef HAVE_SSL
	if(use_tls == True) { 
		return(SSL_read(ssl, c, 1));
	}
#endif
	return(read(fd, c, 1));
#endif
}

/*
fd_gets() -- Get characters from a fd instead of an fp
*/
char *fd_gets(char *buf, int size, int fd)
{
	int i = 0;
	char c;

	while((i < size) && (fd_getc(fd, &c) == 1)) {
		if(c == '\r');	/* Strip <CR> */
		else if(c == '\n') {
			break;
		}
		else {
			buf[i++] = c;
		}
	}
	buf[i] = '\0';

	return(buf);
}

/*
smtp_read() -- Get a line and return the initial digit
*/
int smtp_read(int fd, char *response)
{
	do {
		if(fd_gets(response, BUF_SZ, fd) == NULL) {
			return(0);
		}
	}
	while(response[3] == '-');

	if(log_level > 0) {
		log_event(LOG_INFO, "%s\n", response);
	}

	if(minus_v) {
		(void)fprintf(stderr, "[<-] %s\n", response);
	}

	return(atoi(response) / 100);
}

/*
smtp_okay() -- Get a line and test the three-number string at the beginning
				If it starts with a 2, it's OK
*/
int smtp_okay(int fd, char *response)
{
	return((smtp_read(fd, response) == 2) ? 1 : 0);
}

/*
fd_puts() -- Write characters to fd
*/
ssize_t fd_puts(int fd, const void *buf, size_t count) 
{
#ifdef WRITE_TIMEOUT
	int written_bytes, written_bytes_total = 0;

	while (count > 0) {
#ifdef HAVE_SSL
		if(use_tls == True) { 
			written_bytes = SSL_write(ssl, buf + written_bytes_total, count);
		} else {
#endif
			written_bytes = write(fd, buf + written_bytes_total, count);
#ifdef HAVE_SSL
		}
#endif

		if (written_bytes < 0) {
			switch (errno) {
				case EAGAIN:
					{
						int res;

						res = ssmtp_poll(fd, POLLOUT, write_timeout);
						if (res == SSMTP_POLL_SUCCESS) {
							continue;
						}
						return -1;
					}
				default:
					return -1;
			}

		} else {
			written_bytes_total += written_bytes;
			count -= written_bytes;
		}
	}
	return written_bytes_total;
#else
	int fd_flags, ret;

	fd_flags = fcntl(fd, F_GETFL, 0);
	if (fcntl(fd, F_SETFL, fd_flags &~ O_NONBLOCK) != 0) {
		log_event(LOG_ERR, "fcntl(, ) failed");
		return(-1);
	}

#ifdef HAVE_SSL
	if(use_tls == True) { 
		ret = SSL_write(ssl, buf, count));
		if (fcntl(fd, F_SETFL, fd_flags | O_NONBLOCK) != 0) {
			log_event(LOG_ERR, "fcntl(, ) failed");
			return(-1);
		}
		return ret;
	}
#endif
	ret = write(fd, buf, count);

	if (fcntl(fd, F_SETFL, fd_flags | O_NONBLOCK) != 0) {
		log_event(LOG_ERR, "fcntl(, ) failed");
		return(-1);
	}
	return ret;
#endif
}

/*
smtp_write() -- A printf to an fd and append <CR/LF>
*/
void smtp_write(int fd, char *format, ...)
{
	char buf[(BUF_SZ + 1)];
	va_list ap;

	va_start(ap, format);
	if(vsnprintf(buf, (BUF_SZ - 2), format, ap) == -1) {
		die("smtp_write() -- vsnprintf() failed");
	}
	va_end(ap);

	if(log_level > 0) {
		log_event(LOG_INFO, "%s\n", buf);
	}

	if(minus_v) {
		(void)fprintf(stderr, "[->] %s\n", buf);
	}
	(void)strcat(buf, "\r\n");

	(void)fd_puts(fd, buf, strlen(buf));
}

/*
handler() -- A "normal" non-portable version of an alarm handler
			Alas, setting a flag and returning is not fully functional in
			BSD: system calls don't fail when reading from a ``slow'' device
			like a socket. So we longjump instead, which is erronious on
			a small number of machines and ill-defined in the language
*/
void handler(void)
{
	extern jmp_buf TimeoutJmpBuf;

	longjmp(TimeoutJmpBuf, (int)1);
}

/*
ssmtp() -- send the message (exactly one) from stdin to the mailhub SMTP port
*/
int ssmtp(char *argv[])
{
	char buf[(BUF_SZ + 1)], *p, *q;
#ifdef MD5AUTH
	char challenge[(BUF_SZ + 1)];
#endif
	struct passwd *pw;
	int i, sock, res;
	uid_t uid;

	uid = getuid();
	if((pw = getpwuid(uid)) == (struct passwd *)NULL) {
		die("Could not find password entry for UID %d", uid);
	}
	get_arpadate(arpadate);

	if(read_config() == False) {
		log_event(LOG_INFO, "%s/ssmtp.conf not found", SSMTPCONFDIR);
	}

	if((p = strtok(pw->pw_gecos, ";,"))) {
		if((gecos = strdup(p)) == (char *)NULL) {
			die("ssmtp() -- strdup() failed");
		}
	}
	revaliases(pw);

	/* revaliases() may have defined this */
	if(uad == (char *)NULL) {
		uad = append_domain(pw->pw_name);
	}

	ht = &headers;
	rt = &rcpt_list;

	header_parse(stdin);

#if 1
	/* With FromLineOverride=YES set, try to recover sane MAIL FROM address */
	uad = append_domain(uad);
#endif

	from = from_format(uad, override_from);

	/* Now to the delivery of the message */
	(void)signal(SIGALRM, (void(*)())handler);	/* Catch SIGALRM */
	(void)alarm((unsigned) MAXWAIT);			/* Set initial timer */
	if(setjmp(TimeoutJmpBuf) != 0) {
		/* Then the timer has gone off and we bail out */
		die("Connection lost in middle of processing");
	}

	if((sock = smtp_open(mailhost, port)) == -1) {
		die("Cannot open %s:%d", mailhost, port);
	}
	else if (use_starttls == False) /* no initial response after STARTTLS */
	{
		if(smtp_okay(sock, buf) == False)
			die("Invalid response SMTP server");
	}

	/* If user supplied username and password, then try ELHO */
	if(auth_user) {
		smtp_write(sock, "EHLO %s", hostname);
	}
	else {
		smtp_write(sock, "HELO %s", hostname);
	}
	(void)alarm((unsigned) MEDWAIT);

	if(smtp_okay(sock, buf) == False) {
		die("%s (%s)", buf, hostname);
	}

	/* Try to log in if username was supplied */
	if(auth_user) {
#ifdef MD5AUTH
		if(auth_pass == (char *)NULL) {
			auth_pass = strdup("");
		}

		if(strcasecmp(auth_method, "cram-md5") == 0) {
			smtp_write(sock, "AUTH CRAM-MD5");
			(void)alarm((unsigned) MEDWAIT);

			if(smtp_read(sock, buf) != 3) {
				die("Server rejected AUTH CRAM-MD5 (%s)", buf);
			}
			strncpy(challenge, strchr(buf,' ') + 1, sizeof(challenge));

			memset(buf, 0, sizeof(buf));
			crammd5(challenge, auth_user, auth_pass, buf);
		}
		else {
#endif
		    memset(buf, 0, sizeof(buf));
		    to64frombits(buf, auth_user, strlen(auth_user));
		    smtp_write(sock, "AUTH LOGIN %s", buf);

		    (void)alarm((unsigned) MEDWAIT);
		    if(smtp_read(sock, buf) != 3) {
			die("Server didn't accept AUTH LOGIN (%s)", buf);
		    }
		    memset(buf, 0, sizeof(buf));

		    to64frombits(buf, auth_pass, strlen(auth_pass));
#ifdef MD5AUTH
		}
#endif
		smtp_write(sock, "%s", buf);
		(void)alarm((unsigned) MEDWAIT);

		if(smtp_okay(sock, buf) == False) {
			die("Authorization failed (%s)", buf);
		}
	}

	/* Send "MAIL FROM:" line */
	smtp_write(sock, "MAIL FROM:<%s>", uad);

	(void)alarm((unsigned) MEDWAIT);

	if(smtp_okay(sock, buf) == 0) {
		die("%s", buf);
	}

	/* Send all the To: adresses */
	/* Either we're using the -t option, or we're using the arguments */
	if(minus_t) {
		if(rcpt_list.next == (rcpt_t *)NULL) {
			die("No recipients specified although -t option used");
		}
		rt = &rcpt_list;

		while(rt->next) {
			p = rcpt_remap(rt->string);
			smtp_write(sock, "RCPT TO:<%s>", p);

			(void)alarm((unsigned)MEDWAIT);

			if(smtp_okay(sock, buf) == 0) {
				die("%s", buf);
			}

			rt = rt->next;
		}
	}
	else {
		for(i = 1; (argv[i] != NULL); i++) {
			p = strtok(argv[i], ",");
			while(p) {
				/* RFC822 Address -> "foo@bar" */
				q = rcpt_remap(addr_parse(p));
				smtp_write(sock, "RCPT TO:<%s>", q);

				(void)alarm((unsigned) MEDWAIT);

				if(smtp_okay(sock, buf) == 0) {
					die("%s", buf);
				}

				p = strtok(NULL, ",");
			}
		}
	}

	/* Send DATA */
	smtp_write(sock, "DATA");
	(void)alarm((unsigned) MEDWAIT);

	if(smtp_read(sock, buf) != 3) {
		/* Oops, we were expecting "354 send your data" */
		die("%s", buf);
	}

	smtp_write(sock,
		"Received: by %s (sSMTP sendmail emulation); %s", hostname, arpadate);

	if(have_from == False) {
		smtp_write(sock, "From: %s", from);
	}

	if(have_date == False) {
		smtp_write(sock, "Date: %s", arpadate);
	}

#ifdef HASTO_OPTION
	if(have_to == False) {
		smtp_write(sock, "To: postmaster");
	}
#endif

	ht = &headers;
	while(ht->next) {
		smtp_write(sock, "%s", ht->string);
		ht = ht->next;
	}

	(void)alarm((unsigned) MEDWAIT);

	/* End of headers, start body */
	smtp_write(sock, "");

	while(fgets(buf, sizeof(buf), stdin)) {
		/* Trim off \n, double leading .'s */
		standardise(buf);

		smtp_write(sock, "%s", buf);

		(void)alarm((unsigned) MEDWAIT);
	}
	/* End of body */

	smtp_write(sock, ".");
	(void)alarm((unsigned) MAXWAIT);

	res = smtp_okay(sock, buf);
	/* always output the final reply from the MTA */
	fprintf(stdout, "%s: %s\n", prog, buf);

	if(res == 0) {
	    die("%s", "");
	}

	/* Close conection */
	(void)signal(SIGALRM, SIG_IGN);

	smtp_write(sock, "QUIT");
	(void)smtp_okay(sock, buf);
	(void)close(sock);

	log_event(LOG_INFO, "Sent mail for %s (%s)", from_strip(uad), buf);

	return(0);
}

/*
paq() - Write error message and exit
*/
void paq(char *format, ...)
{
	va_list ap;   

	va_start(ap, format);
	(void)vfprintf(stderr, format, ap);
	va_end(ap);

	exit(0);
}

/*
parse_options() -- Pull the options out of the command-line
	Process them (special-case calls to mailq, etc) and return the rest
*/
char **parse_options(int argc, char *argv[])
{
	static char Version[] = VERSION;
	static char *new_argv[MAXARGS];
	int i, j, add, new_argc;

	new_argv[0] = argv[0];
	new_argc = 1;

	if(strcmp(prog, "mailq") == 0) {
		/* Someone wants to know the queue state... */
		paq("mailq: Mail queue is empty\n");
	}
	else if(strcmp(prog, "newaliases") == 0) {
		/* Someone wanted to rebuild aliases */
		paq("newaliases: Aliases are not used in sSMTP\n");
	}

	i = 1;
	while(i < argc) {
		if(argv[i][0] != '-') {
			new_argv[new_argc++] = argv[i++];
			continue;
		}
		j = 0;

		add = 1;
		while(argv[i][++j] != 0) {
			switch(argv[i][j]) {
#ifdef INET6
			case '6':
				p_family = PF_INET6;
				continue;

			case '4':
				p_family = PF_INET;
			continue;
#endif

			case 'a':
				switch(argv[i][++j]) {
				case 'u':
					if((!argv[i][(j + 1)])
						&& argv[(i + 1)]) {
						auth_user = strdup(argv[i+1]);
						if(auth_user == (char *)NULL) {
							die("parse_options() -- strdup() failed");
						}
						add++;
					}
					else {
						auth_user = strdup(argv[i]+j+1);
						if(auth_user == (char *)NULL) {
							die("parse_options() -- strdup() failed");
						}
					}
					goto exit;

				case 'p':
					if((!argv[i][(j + 1)])
						&& argv[(i + 1)]) {
						auth_pass = strdup(argv[i+1]);
						if(auth_pass == (char *)NULL) {
							die("parse_options() -- strdup() failed");
						}
						add++;
					}
					else {
						auth_pass = strdup(argv[i]+j+1);
						if(auth_pass == (char *)NULL) {
							die("parse_options() -- strdup() failed");
						}
					}
					goto exit;

/*
#ifdef MD5AUTH
*/
				case 'm':
					if(!argv[i][j+1]) { 
						auth_method = strdup(argv[i+1]);
						add++;
					}
					else {
						auth_method = strdup(argv[i]+j+1);
					}
				}
				goto exit;
/*
#endif
*/

			case 'b':
				switch(argv[i][++j]) {

				case 'a':	/* ARPANET mode */
						paq("-ba is not supported by sSMTP\n");
				case 'd':	/* Run as a daemon */
						paq("-bd is not supported by sSMTP\n");
				case 'i':	/* Initialise aliases */
						paq("%s: Aliases are not used in sSMTP\n", prog);
				case 'm':	/* Default addr processing */
						continue;

				case 'p':	/* Print mailqueue */
						paq("%s: Mail queue is empty\n", prog);
				case 's':	/* Read SMTP from stdin */
						paq("-bs is not supported by sSMTP\n");
				case 't':	/* Test mode */
						paq("-bt is meaningless to sSMTP\n");
				case 'v':	/* Verify names only */
						paq("-bv is meaningless to sSMTP\n");
				case 'z':	/* Create freeze file */
						paq("-bz is meaningless to sSMTP\n");
				}

			/* Configfile name */
			case 'C':
				if (argv[i][++j]) {
					config_file_path = strdup(argv[i] + j);
				}
				else {
					config_file_path = strdup(argv[i+1]);
					add ++;
				}
				goto exit;

				/* mailhost address */
			case 'I':
				if((mailhost = strdup(argv[i+1])) != (char *)NULL) {
					char *r;
					if((r = strchr(mailhost, ':')) != NULL) {
						port = atoi(++r);
					}
					mailhost_cmdline = 1;
					if (new_argc > 0) {
						new_argc--;
					}
				}
				continue;

			/* Debug */
			case 'd':
				log_level = 1;
				/* Almost the same thing... */
				minus_v = True;

				continue;

			/* Insecure channel, don't trust userid */
			case 'E':
					continue;

			case 'R':
				/* Amount of the message to be returned */
				if(!argv[i][j+1]) {
					add++;
					goto exit;
				}
				else {
					/* Process queue for recipient */
					continue;
				}

			/* Fullname of sender */
			case 'F':
				if((!argv[i][(j + 1)]) && argv[(i + 1)]) {
					minus_F = strdup(argv[(i + 1)]);
					if(minus_F == (char *)NULL) {
						die("parse_options() -- strdup() failed");
					}
					add++;
				}
				else {
					minus_F = strdup(argv[i]+j+1);
					if(minus_F == (char *)NULL) {
						die("parse_options() -- strdup() failed");
					}
				}
				goto exit;

			/* Set from/sender address */
			case 'f':
			/* Obsolete -f flag */
			case 'r':
				if((!argv[i][(j + 1)]) && argv[(i + 1)]) {
					minus_f = strdup(argv[(i + 1)]);
					if(minus_f == (char *)NULL) {
						die("parse_options() -- strdup() failed");
					}
					add++;
				}
				else {
					minus_f = strdup(argv[i]+j+1);
					if(minus_f == (char *)NULL) {
						die("parse_options() -- strdup() failed");
					}
				}
				goto exit;

			/* Set hopcount */
			case 'h':
				continue;

			/* Ignore originator in adress list */
			case 'm':
				continue;

			/* Use specified message-id */
			case 'M':
				goto exit;

			/* DSN options */
			case 'N':
				add++;
				goto exit;

			/* No aliasing */
			case 'n':
				continue;

			case 'o':
				switch(argv[i][++j]) {

				/* Alternate aliases file */
				case 'A':
					goto exit;

				/* Delay connections */
				case 'c':
					continue;

				/* Run newaliases if required */
				case 'D':
					paq("%s: Aliases are not used in sSMTP\n", prog);

				/* Deliver now, in background or queue */
				/* This may warrant a diagnostic for b or q */
				case 'd':
						continue;

				/* Errors: mail, write or none */
				case 'e':
					j++;
					continue;

				/* Set tempfile mode */
				case 'F':
					goto exit;

				/* Save ``From ' lines */
				case 'f':
					continue;

				/* Set group id */
				case 'g':
					goto exit;

				/* Helpfile name */
				case 'H':
					continue;

				/* DATA ends at EOF, not \n.\n */
				case 'i':
					continue;

				/* Log level */
				case 'L':
					goto exit;

				/* Send to me if in the list */
				case 'm':
					continue;

				/* Old headers, spaces between adresses */
				case 'o':
					paq("-oo is not supported by sSMTP\n");

				/* Queue dir */
				case 'Q':
					goto exit;

				/* Read timeout */
				case 'r':
					goto exit;

				/* Always init the queue */
				case 's':
					continue;

				/* Stats file */
				case 'S':
					goto exit;

				/* Queue timeout */
				case 'T':
					goto exit;

				/* Set timezone */
				case 't':
					goto exit;

				/* Set uid */
				case 'u':
					goto exit;

				/* Set verbose flag */
				case 'v':
					minus_v = True;
					continue;
				}
				break;

			/* Process the queue [at time] */
			case 'q':
					paq("%s: Mail queue is empty\n", prog);

			/* Read message's To/Cc/Bcc lines */
			case 't':
				minus_t = True;
				continue;

			/* minus_v (ditto -ov) */
			case 'v':
				minus_v = True;
				break;

			/* Say version and quit */
			/* Similar as die, but no logging */
			case 'V':
				paq("sSMTP %s (Not sendmail at all)\n", Version);
			}
		}

		exit:
		i += add;
	}
	new_argv[new_argc] = NULL;

	if(new_argc <= 1 && !minus_t) {
		paq("%s: No recipients supplied - mail will not be sent\n", prog);
	}

	if(new_argc > 1 && minus_t) {
		paq("%s: recipients with -t option not supported\n", prog);
	}

	return(&new_argv[0]);
}

/*
main() -- make the program behave like sendmail, then call ssmtp
*/
int main(int argc, char **argv)
{
	char **new_argv;

	/* Try to be bulletproof :-) */
	(void)signal(SIGHUP, SIG_IGN);
	(void)signal(SIGINT, SIG_IGN);
	(void)signal(SIGTTIN, SIG_IGN);
	(void)signal(SIGTTOU, SIG_IGN);

	/* Set the globals */
	prog = basename(argv[0]);

	if(gethostname(hostname, MAXHOSTNAMELEN) == -1) {
		die("Cannot get the name of this machine");
	}
	new_argv = parse_options(argc, argv);

	exit(ssmtp(new_argv));
}
