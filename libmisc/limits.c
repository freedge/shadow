/*
 * Copyright (c) 1989 - 1994, Julianne Frances Haugh
 * Copyright (c) 1996 - 1999, Marek Michałkiewicz
 * Copyright (c) 2003 - 2006, Tomasz Kłoczko
 * Copyright (c) 2007 - 2010, Nicolas François
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. The name of the copyright holders or contributors may not be used to
 *    endorse or promote products derived from this software without
 *    specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A
 * PARTICULAR PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT
 * HOLDERS OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/*
 * Separated from setup.c.  --marekm
 * Resource limits thanks to Cristian Gafton.
 * Enhancements of resource limit code by Thomas Orgis <thomas@orgis.org>
 */

#include <config.h>

#ifndef USE_PAM

#ident "$Id$"

#include <sys/types.h>
#include <sys/stat.h>
#include <stdio.h>
#include <ctype.h>
#include "prototypes.h"
#include "defines.h"
#include <pwd.h>
#include "getdef.h"
#ifdef HAVE_SYS_RESOURCE_H
#include <sys/resource.h>
#define LIMITS
#endif
#ifdef LIMITS
#ifndef LIMITS_FILE
#define LIMITS_FILE "/etc/limits"
#endif
#define LOGIN_ERROR_RLIMIT	1
#define LOGIN_ERROR_LOGIN	2
/* Set a limit on a resource */
/*
 *	rlimit - RLIMIT_XXXX
 *	value - string value to be read
 *	multiplier - value*multiplier is the actual limit
 */
static int setrlimit_value (unsigned int resource,
                            const char *value,
                            unsigned int multiplier)
{
	struct rlimit rlim;
	rlim_t limit;

	/* The "-" is special, not belonging to a strange negative limit.
	 * It is infinity, in a controlled way.
	 */
	if ('-' == value[0]) {
		limit = RLIM_INFINITY;
	}
	else {
		/* We cannot use getlong here because it fails when there
		 * is more to the value than just this number!
		 * Also, we are limited to base 10 here (hex numbers will not
		 * work with the limit string parser as is anyway)
		 */
		char *endptr;
		long longlimit = strtol (value, &endptr, 10);
		if ((0 == longlimit) && (value == endptr)) {
			/* No argument at all. No-op.
			 * FIXME: We could instead throw an error, though.
			 */
			return 0;
		}
		longlimit *= multiplier;
		limit = (rlim_t)longlimit;
		if (longlimit != limit)
		{
			/* FIXME: Again, silent error handling...
			 * Wouldn't screaming make more sense?
			 */
			return 0;
		}
	}

	rlim.rlim_cur = limit;
	rlim.rlim_max = limit;
	if (setrlimit (resource, &rlim) != 0) {
		return LOGIN_ERROR_RLIMIT;
	}
	return 0;
}


static int set_prio (const char *value)
{
	long prio;

	if (   (getlong (value, &prio) == 0)
	    || (prio != (int) prio)) {
		return 0;
	}
	if (setpriority (PRIO_PROCESS, 0, (int) prio) != 0) {
		return LOGIN_ERROR_RLIMIT;
	}
	return 0;
}


static int set_umask (const char *value)
{
	unsigned long int mask;

	if (   (getulong (value, &mask) == 0)
	    || (mask != (mode_t) mask)) {
		return 0;
	}

	(void) umask ((mode_t) mask);
	return 0;
}


/* Counts the number of user logins and check against the limit */
static int check_logins (const char *name, const char *maxlogins)
{
#ifdef USE_UTMPX
	struct utmpx *ut;
#else				/* !USE_UTMPX */
	struct utmp *ut;
#endif				/* !USE_UTMPX */
	unsigned long limit, count;

	if (getulong (maxlogins, &limit) == 0) {
		return 0;
	}

	if (0 == limit) {	/* maximum 0 logins ? */
		SYSLOG ((LOG_WARN, "No logins allowed for `%s'\n", name));
		return LOGIN_ERROR_LOGIN;
	}

	count = 0;
#ifdef USE_UTMPX
	setutxent ();
	while ((ut = getutxent ()))
#else				/* !USE_UTMPX */
	setutent ();
	while ((ut = getutent ()))
#endif				/* !USE_UTMPX */
	{
		if (USER_PROCESS != ut->ut_type) {
			continue;
		}
		if ('\0' == ut->ut_user[0]) {
			continue;
		}
		if (strncmp (name, ut->ut_user, sizeof (ut->ut_user)) != 0) {
			continue;
		}
		count++;
		if (count > limit) {
			break;
		}
	}
#ifdef USE_UTMPX
	endutxent ();
#else				/* !USE_UTMPX */
	endutent ();
#endif				/* !USE_UTMPX */
	/*
	 * This is called after setutmp(), so the number of logins counted
	 * includes the user who is currently trying to log in.
	 */
	if (count > limit) {
		SYSLOG ((LOG_WARN,
		         "Too many logins (max %lu) for %s\n",
		         limit, name));
		return LOGIN_ERROR_LOGIN;
	}
	return 0;
}

/* Function setup_user_limits - checks/set limits for the current login
 * Original idea from Joel Katz's lshell. Ported to shadow-login
 * by Cristian Gafton - gafton@sorosis.ro
 *
 * We are passed a string of the form ('BASH' constants for ulimit)
 *     [Aa][Cc][Dd][Ff][Mm][Nn][Rr][Ss][Tt][Uu][Ll][Pp][Ii][Oo]
 *     (eg. 'C2F256D2048N5' or 'C2 F256 D2048 N5')
 * where:
 * [Aa]: a = RLIMIT_AS		max address space (KB)
 * [Cc]: c = RLIMIT_CORE	max core file size (KB)
 * [Dd]: d = RLIMIT_DATA	max data size (KB)
 * [Ff]: f = RLIMIT_FSIZE	max file size (KB)
 * [Ii]: i = RLIMIT_NICE    max nice value (0..39 translates to 20..-19)
 * [Kk]: k = file creation masK (umask)
 * [Ll]: l = max number of logins for this user
 * [Mm]: m = RLIMIT_MEMLOCK	max locked-in-memory address space (KB)
 * [Nn]: n = RLIMIT_NOFILE	max number of open files
 * [Oo]: o = RLIMIT_RTPRIO  max real time priority (linux/sched.h 0..MAX_RT_PRIO)
 * [Pp]: p = process priority -20..20 (negative = high, positive = low)
 * [Rr]: r = RLIMIT_RSS		max resident set size (KB)
 * [Ss]: s = RLIMIT_STACK	max stack size (KB)
 * [Tt]: t = RLIMIT_CPU		max CPU time (MIN)
 * [Uu]: u = RLIMIT_NPROC	max number of processes
 *
 * NOTE: Remember to extend the "no-limits" string below when adding a new
 * limit...
 *
 * Return value:
 *		0 = okay, of course
 *		LOGIN_ERROR_RLIMIT = error setting some RLIMIT
 *		LOGIN_ERROR_LOGIN  = error - too many logins for this user
 *
 * buf - the limits string
 * name - the username
 */
static int do_user_limits (const char *buf, const char *name)
{
	const char *pp;
	int retval = 0;
	bool reported = false;

	pp = buf;
	/* Skip leading whitespace. */
	while ((' ' == *pp) || ('\t' == *pp)) {
		pp++;
	}

	/* The special limit string "-" results in no limit for all known
	 * limits.
	 * We achieve that by parsing a full limit string, parts of it
	 * being ignored if a limit type is not known to the system.
	 * Though, there will be complaining for unknown limit types.
	 */
	if (strcmp (pp, "-") == 0) {
		/* Remember to extend this, too, when adding new limits!
		 * Oh... but "unlimited" does not make sense for umask,
		 * or does it? (K-)
		 */
		pp = "A- C- D- F- I- L- M- N- O- P- R- S- T- U-";
	}

	while ('\0' != *pp) {
		switch (*pp++) {
#ifdef RLIMIT_AS
		case 'a':
		case 'A':
			/* RLIMIT_AS - max address space (KB) */
			retval |= setrlimit_value (RLIMIT_AS, pp, 1024);
			break;
#endif
#ifdef RLIMIT_CORE
		case 'c':
		case 'C':
			/* RLIMIT_CORE - max core file size (KB) */
			retval |= setrlimit_value (RLIMIT_CORE, pp, 1024);
			break;
#endif
#ifdef RLIMIT_DATA
		case 'd':
		case 'D':
			/* RLIMIT_DATA - max data size (KB) */
			retval |= setrlimit_value (RLIMIT_DATA, pp, 1024);
			break;
#endif
#ifdef RLIMIT_FSIZE
		case 'f':
		case 'F':
			/* RLIMIT_FSIZE - Maximum filesize (KB) */
			retval |= setrlimit_value (RLIMIT_FSIZE, pp, 1024);
			break;
#endif
#ifdef RLIMIT_NICE
		case 'i':
		case 'I':
			/* RLIMIT_NICE - max scheduling priority (0..39) */
			retval |= setrlimit_value (RLIMIT_NICE, pp, 1);
			break;
#endif
		case 'k':
		case 'K':
			retval |= set_umask (pp);
			break;
		case 'l':
		case 'L':
			/* LIMIT the number of concurrent logins */
			retval |= check_logins (name, pp);
			break;
#ifdef RLIMIT_MEMLOCK
		case 'm':
		case 'M':
			/* RLIMIT_MEMLOCK - max locked-in-memory address space (KB) */
			retval |= setrlimit_value (RLIMIT_MEMLOCK, pp, 1024);
			break;
#endif
#ifdef RLIMIT_NOFILE
		case 'n':
		case 'N':
			/* RLIMIT_NOFILE - max number of open files */
			retval |= setrlimit_value (RLIMIT_NOFILE, pp, 1);
			break;
#endif
#ifdef RLIMIT_RTPRIO
		case 'o':
		case 'O':
			/* RLIMIT_RTPRIO - max real time priority (0..MAX_RT_PRIO) */
			retval |= setrlimit_value (RLIMIT_RTPRIO, pp, 1);
			break;
#endif
		case 'p':
		case 'P':
			retval |= set_prio (pp);
			break;
#ifdef RLIMIT_RSS
		case 'r':
		case 'R':
			/* RLIMIT_RSS - max resident set size (KB) */
			retval |= setrlimit_value (RLIMIT_RSS, pp, 1024);
			break;
#endif
#ifdef RLIMIT_STACK
		case 's':
		case 'S':
			/* RLIMIT_STACK - max stack size (KB) */
			retval |= setrlimit_value (RLIMIT_STACK, pp, 1024);
			break;
#endif
#ifdef RLIMIT_CPU
		case 't':
		case 'T':
			/* RLIMIT_CPU - max CPU time (MIN) */
			retval |= setrlimit_value (RLIMIT_CPU, pp, 60);
			break;
#endif
#ifdef RLIMIT_NPROC
		case 'u':
		case 'U':
			/* RLIMIT_NPROC - max number of processes */
			retval |= setrlimit_value (RLIMIT_NPROC, pp, 1);
			break;
#endif
		default:
			/* Only report invalid strings once */
			/* Note: A string can be invalid just because a
			 * specific (theoretically valid) setting is not
			 * supported by this build.
			 * It is just a warning in syslog anyway. The line
			 * is still processed
			 */
			if (!reported) {
				SYSLOG ((LOG_WARN,
				         "Invalid limit string: '%s'",
				         pp-1));
				reported = true;
				retval |= LOGIN_ERROR_RLIMIT;
			}
		}
		/* After parsing one limit setting (or just complaining
		 * about it), one still needs to skip its argument to
		 * prevent a bogus warning on trying to parse that as
		 * limit specification.
		 * So, let's skip all digits, "-" and our limited set of
		 * whitespace.
		 */
		while (   isdigit (*pp)
		       || ('-'  == *pp)
		       || (' '  == *pp)
		       || ('\t' ==*pp)) {
			pp++;
		}
	}
	return retval;
}

/* Check if user uname is in the group gname.
 * Can I be sure that gr_mem contains no UID as string?
 * Returns true when user is in the group, false when not.
 * Any error is treated as false.
 */
static bool user_in_group (const char *uname, const char *gname)
{
	struct group *groupdata;

	if (uname == NULL || gname == NULL) {
		return false;
	}

	/* We are not claiming to be re-entrant!
	 * In case of paranoia or a multithreaded login program,
	 * one needs to add some mess for getgrnam_r. */
	groupdata = getgrnam (gname);
	if (NULL == groupdata) {
		SYSLOG ((LOG_WARN, "Nonexisting group `%s' in limits file.",
		         gname));
		return false;
	}

	return is_on_list (groupdata->gr_mem, uname);
}

static int setup_user_limits (const char *uname)
{
	FILE *fil;
	char buf[1024];
	char name[1024];
	char limits[1024];
	char deflimits[1024];
	char tempbuf[1024];

	/* init things */
	memzero (buf, sizeof (buf));
	memzero (name, sizeof (name));
	memzero (limits, sizeof (limits));
	memzero (deflimits, sizeof (deflimits));
	memzero (tempbuf, sizeof (tempbuf));

	/* start the checks */
	fil = fopen (LIMITS_FILE, "r");
	if (fil == NULL) {
		return 0;
	}
	/* The limits file have the following format:
	 * - '#' (comment) chars only as first chars on a line;
	 * - username must start on first column (or *, or @group)
	 *
	 * FIXME: A better (smarter) checking should be done
	 */
	while (fgets (buf, 1024, fil) != NULL) {
		if (('#' == buf[0]) || ('\n' == buf[0])) {
			continue;
		}
		memzero (tempbuf, sizeof (tempbuf));
		/* a valid line should have a username, then spaces,
		 * then limits
		 * we allow the format:
		 * username    L2  D2048  R4096
		 * where spaces={' ',\t}. Also, we reject invalid limits.
		 * Imposing a limit should be done with care, so a wrong
		 * entry means no care anyway :-).
		 *
		 * A '-' as a limits strings means no limits
		 *
		 * The username can also be:
		 *  '*': the default limits (only the last is taken into
		 *       account)
		 *  @group: the limit applies to the members of the group
		 *
		 * To clarify: The first entry with matching user name rules,
		 * everything after it is ignored. If there is no user entry,
		 * the last encountered entry for a matching group rules.
		 * If there is no matching group entry, the default limits rule.
		 */
		if (sscanf (buf, "%s%[ACDFIKLMNOPRSTUacdfiklmnoprstu0-9 \t-]",
		            name, tempbuf) == 2) {
			if (strcmp (name, uname) == 0) {
				strcpy (limits, tempbuf);
				break;
			} else if (strcmp (name, "*") == 0) {
				strcpy (deflimits, tempbuf);
			} else if (name[0] == '@') {
				/* If the user is in the group, the group
				 * limits apply unless later a line for
				 * the specific user is found.
				 */
				if (user_in_group (uname, name+1)) {
					strcpy (limits, tempbuf);
				}
			}
		}
	}
	(void) fclose (fil);
	if (limits[0] == '\0') {
		/* no user specific limits */
		if (deflimits[0] == '\0') {	/* no default limits */
			return 0;
		}
		strcpy (limits, deflimits);	/* use the default limits */
	}
	return do_user_limits (limits, uname);
}
#endif				/* LIMITS */


static void setup_usergroups (const struct passwd *info)
{
	const struct group *grp;

/*
 *	if not root, and UID == GID, and username is the same as primary
 *	group name, set umask group bits to be the same as owner bits
 *	(examples: 022 -> 002, 077 -> 007).
 */
	if ((0 != info->pw_uid) && (info->pw_uid == info->pw_gid)) {
		/* local, no need for xgetgrgid */
		grp = getgrgid (info->pw_gid);
		if (   (NULL != grp)
		    && (strcmp (info->pw_name, grp->gr_name) == 0)) {
			mode_t tmpmask;
			tmpmask = umask (0777);
			tmpmask = (tmpmask & ~070) | ((tmpmask >> 3) & 070);
			(void) umask (tmpmask);
		}
	}
}

/*
 *	set the process nice, ulimit, and umask from the password file entry
 */

void setup_limits (const struct passwd *info)
{
	char *cp;

	if (getdef_bool ("USERGROUPS_ENAB")) {
		setup_usergroups (info);
	}

	/*
	 * See if the GECOS field contains values for NICE, UMASK or ULIMIT.
	 * If this feature is enabled in /etc/login.defs, we make those
	 * values the defaults for this login session.
	 */

	if (getdef_bool ("QUOTAS_ENAB")) {
#ifdef LIMITS
		if (info->pw_uid != 0) {
			if ((setup_user_limits (info->pw_name) & LOGIN_ERROR_LOGIN) != 0) {
				(void) fputs (_("Too many logins.\n"), shadow_logfd);
				(void) sleep (2); /* XXX: Should be FAIL_DELAY */
				exit (EXIT_FAILURE);
			}
		}
#endif
		for (cp = info->pw_gecos; cp != NULL; cp = strchr (cp, ',')) {
			if (',' == *cp) {
				cp++;
			}

			if (strncmp (cp, "pri=", 4) == 0) {
				long int inc;
				if (   (getlong (cp + 4, &inc) == 1)
				    && (inc >= -20) && (inc <= 20)) {
					errno = 0;
					if (   (nice ((int) inc) != -1)
					    || (0 != errno)) {
						continue;
					}
				}

				/* Failed to parse or failed to nice() */
				SYSLOG ((LOG_WARN,
				         "Can't set the nice value for user %s",
				         info->pw_name));

				continue;
			}
			if (strncmp (cp, "ulimit=", 7) == 0) {
				long int blocks;
				if (   (getlong (cp + 7, &blocks) == 0)
				    || (blocks != (int) blocks)
				    || (set_filesize_limit ((int) blocks) != 0)) {
					SYSLOG ((LOG_WARN,
					         "Can't set the ulimit for user %s",
					         info->pw_name));
				}
				continue;
			}
			if (strncmp (cp, "umask=", 6) == 0) {
				unsigned long int mask;
				if (   (getulong (cp + 6, &mask) == 0)
				    || (mask != (mode_t) mask)) {
					SYSLOG ((LOG_WARN,
					         "Can't set umask value for user %s",
					         info->pw_name));
				} else {
					(void) umask ((mode_t) mask);
				}

				continue;
			}
		}
	}
}

#else				/* !USE_PAM */
extern int errno;		/* warning: ANSI C forbids an empty source file */
#endif				/* !USE_PAM */

