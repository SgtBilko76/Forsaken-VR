/* Android's bionic has no <sys/timeb.h>; util.c only uses ftime() for log timestamps. */
#ifndef FX_COMPAT_SYS_TIMEB_H
#define FX_COMPAT_SYS_TIMEB_H

#include <sys/time.h>
#include <time.h>

struct timeb
{
	time_t time;
	unsigned short millitm;
	short timezone;
	short dstflag;
};

static inline int ftime( struct timeb *tb )
{
	struct timeval tv;
	gettimeofday( &tv, NULL );
	tb->time = tv.tv_sec;
	tb->millitm = (unsigned short)( tv.tv_usec / 1000 );
	tb->timezone = 0;
	tb->dstflag = 0;
	return 0;
}

#endif
