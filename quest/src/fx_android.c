/*
 * fx_android.c - Android start-up for Forsaken on Meta Quest: logcat output, crash handler,
 * game folder.
 */
#include "fx_quest.h"

#include <android/log.h>
#include <dlfcn.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <unwind.h>

#define TAG "Forsaken"

// --- stdout/stderr -> logcat (the game's DebugPrintf writes to stdout) ---

static int log_pipe[ 2 ];

static void * log_thread( void *arg )
{
	char buf[ 1024 ];
	size_t len = 0;
	(void) arg;
	for(;;)
	{
		ssize_t n = read( log_pipe[ 0 ], buf + len, sizeof(buf) - 1 - len );
		if( n <= 0 )
			break;
		len += (size_t) n;
		buf[ len ] = 0;
		// Emit complete lines; flush a full buffer even without a newline.
		char *start = buf, *nl;
		while( (nl = strchr( start, '\n' )) )
		{
			*nl = 0;
			__android_log_write( ANDROID_LOG_INFO, TAG, start );
			start = nl + 1;
		}
		len = strlen( start );
		if( len == sizeof(buf) - 1 )
		{
			__android_log_write( ANDROID_LOG_INFO, TAG, start );
			len = 0;
		}
		else
			memmove( buf, start, len + 1 );
	}
	return NULL;
}

static void redirect_output( void )
{
	pthread_t thread;
	setvbuf( stdout, NULL, _IOLBF, 0 );
	setvbuf( stderr, NULL, _IONBF, 0 );
	if( pipe( log_pipe ) != 0 )
		return;
	dup2( log_pipe[ 1 ], 1 );
	dup2( log_pipe[ 1 ], 2 );
	if( pthread_create( &thread, NULL, log_thread, NULL ) == 0 )
		pthread_detach( thread );
}

// --- crash handler: log lib+offset frames and exit at once (crash_dump64 can hang the headset) ---

struct unwind_state { int depth; };

static _Unwind_Reason_Code unwind_frame( struct _Unwind_Context *ctx, void *arg )
{
	struct unwind_state *state = (struct unwind_state *) arg;
	uintptr_t pc = _Unwind_GetIP( ctx );
	Dl_info info;
	if( ! pc )
		return _URC_END_OF_STACK;
	if( dladdr( (void *) pc, &info ) && info.dli_fname )
	{
		const char *lib = strrchr( info.dli_fname, '/' );
		__android_log_print( ANDROID_LOG_FATAL, TAG, "  #%02d %s+0x%lx (%s)", state->depth,
			lib ? lib + 1 : info.dli_fname, (unsigned long)( pc - (uintptr_t) info.dli_fbase ),
			info.dli_sname ? info.dli_sname : "?" );
	}
	else
		__android_log_print( ANDROID_LOG_FATAL, TAG, "  #%02d 0x%lx", state->depth, (unsigned long) pc );
	return ( ++ state->depth < 40 ) ? _URC_NO_REASON : _URC_END_OF_STACK;
}

static atomic_int crashed;

static void crash_handler( int sig, siginfo_t *si, void *uc )
{
	struct unwind_state state = { 0 };
	(void) uc;
	if( atomic_exchange( &crashed, 1 ) )
		for(;;) pause();
	__android_log_print( ANDROID_LOG_FATAL, TAG, "CRASH: signal %d at %p", sig, si ? si->si_addr : NULL );
	_Unwind_Backtrace( unwind_frame, &state );
	_exit( 128 + sig );
}

static void install_crash_handler( void )
{
	static char stack[ 64 * 1024 ];
	stack_t ss = { 0 };
	struct sigaction sa;
	int sigs[] = { SIGSEGV, SIGBUS, SIGABRT, SIGILL, SIGFPE };
	ss.ss_sp = stack;
	ss.ss_size = sizeof(stack);
	sigaltstack( &ss, NULL );
	memset( &sa, 0, sizeof(sa) );
	sa.sa_sigaction = crash_handler;
	sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
	sigemptyset( &sa.sa_mask );
	for( size_t i = 0; i < sizeof(sigs) / sizeof(sigs[0]); i ++ )
		sigaction( sigs[ i ], &sa, NULL );
}

bool fx_android_init( void )
{
	struct stat st;
	redirect_output();
	install_crash_handler();
	setenv( "ALSOFT_LOGLEVEL", "2", 0 );  // OpenAL Soft warnings/errors -> stderr -> logcat

	if( stat( FX_DATA_DIR, &st ) != 0 || ! S_ISDIR( st.st_mode ) || chdir( FX_DATA_DIR ) != 0 )
	{
		__android_log_print( ANDROID_LOG_ERROR, TAG, "Game folder %s not found (copy the game data there)", FX_DATA_DIR );
		return false;
	}
	// Folders the game writes to.
	mkdir( "savegame", 0775 );
	mkdir( "logs", 0775 );
	mkdir( "pilots", 0775 );
	__android_log_print( ANDROID_LOG_INFO, TAG, "running in %s", FX_DATA_DIR );
	return true;
}
