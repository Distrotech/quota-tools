/*
 *
 *	Common things for all utilities
 *
 *	Jan Kara <jack@suse.cz> - sponsored by SuSE CR
 *
 *      Jani Jaakkola <jjaakkol@cs.helsinki.fi> - syslog support
 */

#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <syslog.h>
#include <sys/types.h>
#include <sys/stat.h>

#include "pot.h"
#include "common.h"

static int enable_syslog=0;

void use_syslog(void)
{
	openlog(progname,0,LOG_DAEMON);
	enable_syslog=1;
}

static void do_syslog(int level, const char *format, va_list args)
{
	char buf[1024];
	int i, j;
	
	vsnprintf(buf,sizeof(buf),format,args);
	/* This while removes newlines from the log, so that
	 * syslog() will be called once for every line */
	for (i = 0; buf[i]; i = j) {
		for (j = i; buf[j] && buf[j] != '\n'; j++);
		if (buf[j] == '\n')
			buf[j++] = '\0';
		syslog(level, "%s", buf + i);
	}
}

void die(int ret, char *fmtstr, ...)
{
	va_list args;

	va_start(args, fmtstr);
	if (enable_syslog) {
		do_syslog(LOG_CRIT, fmtstr, args);
		syslog(LOG_CRIT, "Exiting with status %d", ret);
	} else {
		fprintf(stderr, "%s: ", progname);
		vfprintf(stderr, fmtstr, args);
	}
	va_end(args);
	exit(ret);
}

void errstr(char *fmtstr, ...)
{
	va_list args;

	va_start(args, fmtstr);
	if (enable_syslog)
		do_syslog(LOG_ERR, fmtstr, args);
	else {
		fprintf(stderr, "%s: ", progname);
		vfprintf(stderr, fmtstr, args);
	}
	va_end(args);
}

void *smalloc(size_t size)
{
	void *ret = malloc(size);

	if (!ret) {
		fputs("Not enough memory.\n", stderr);
		exit(3);
	}
	return ret;
}

void *srealloc(void *ptr, size_t size)
{
	void *ret = realloc(ptr, size);

	if (!ret) {
		fputs("Not enough memory.\n", stderr);
		exit(3);
	}
	return ret;
}

void sstrncpy(char *d, const char *s, size_t len)
{
	strncpy(d, s, len);
	d[len - 1] = 0;
}

void sstrncat(char *d, const char *s, size_t len)
{
	strncat(d, s, len);
	d[len - 1] = 0;
}

char *sstrdup(const char *s)
{
	char *r = strdup(s);

	if (!r) {
		puts("Not enough memory.");
		exit(3);
	}
	return r;
}

void version(void)
{
	printf(_("Quota utilities version %s.\n"), PACKAGE_VERSION);
	printf(_("Compiled with:%s\n"), COMPILE_OPTS);
	printf(_("Bugs to %s\n"), MY_EMAIL);
}
