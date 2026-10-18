/*
 * fx_android.c - Android start-up for Forsaken on Meta Quest: logcat output, crash handler,
 * installing the files packed in the APK, game folder.
 */
#include "fx_quest.h"

#include <SDL.h>
#include <android/asset_manager.h>
#include <android/asset_manager_jni.h>
#include <android/log.h>
#include <errno.h>
#include <jni.h>
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

// --- files packed in the APK -> FX_DATA_DIR ---
// assets/fxfiles.idx lists "<size>\t<path>" for every file below assets/fx/: scripts/, configs/,
// pilots/ and, in the full APK, data/ (the game data). They are written to FX_DATA_DIR when the
// index changes (new APK); configs/ and pilots/ only when missing, so the player's settings stay.

static AAssetManager *get_asset_manager( void )
{
	JNIEnv *env = (JNIEnv *) SDL_AndroidGetJNIEnv();
	jobject activity = (jobject) SDL_AndroidGetActivity();
	if( ! env || ! activity )
		return NULL;
	jclass cls = (*env)->GetObjectClass( env, activity );
	jmethodID get = (*env)->GetMethodID( env, cls, "getAssets", "()Landroid/content/res/AssetManager;" );
	jobject assets = (*env)->CallObjectMethod( env, activity, get );
	jobject global = (*env)->NewGlobalRef( env, assets );  // kept for the process lifetime
	(*env)->DeleteLocalRef( env, assets );
	(*env)->DeleteLocalRef( env, cls );
	(*env)->DeleteLocalRef( env, activity );
	return AAssetManager_fromJava( env, global );
}

static void make_dirs( char *path )  // creates every parent folder of path
{
	for( char *p = path + 1; *p; p ++ )
		if( *p == '/' )
		{
			*p = 0;
			mkdir( path, 0775 );
			*p = '/';
		}
}

static bool write_asset( AAssetManager *mgr, const char *name, const char *dest )
{
	static char buf[ 1 << 20 ];
	char asset[ 1024 ], part[ 1100 ];
	snprintf( asset, sizeof(asset), "fx/%s", name );
	snprintf( part, sizeof(part), "%s.part", dest );
	AAsset *a = AAssetManager_open( mgr, asset, AASSET_MODE_STREAMING );
	if( ! a )
		return false;
	FILE *f = fopen( part, "wb" );
	bool ok = f != NULL;
	int n;
	while( ok && (n = AAsset_read( a, buf, sizeof(buf) )) > 0 )
		ok = fwrite( buf, 1, n, f ) == (size_t) n;
	AAsset_close( a );
	if( f && fclose( f ) != 0 )
		ok = false;
	ok = ok && rename( part, dest ) == 0;
	if( ! ok )
		unlink( part );
	return ok;
}

static void install_packed_files( void )
{
	AAssetManager *mgr = get_asset_manager();
	AAsset *idx = mgr ? AAssetManager_open( mgr, "fxfiles.idx", AASSET_MODE_BUFFER ) : NULL;
	if( ! idx )
		return;
	size_t len = AAsset_getLength( idx );
	char *list = malloc( len + 1 );
	memcpy( list, AAsset_getBuffer( idx ), len );
	list[ len ] = 0;
	AAsset_close( idx );

	// The settings screen for "All files access" opens on first start; wait until it is granted.
	char probe[ 256 ], marker[ 256 ], hash[ 32 ], old[ 32 ] = "";
	snprintf( probe, sizeof(probe), "%s/.write-test", FX_DATA_DIR );
	mkdir( FX_DATA_DIR, 0775 );
	for( int tries = 0; ; tries ++ )
	{
		FILE *f = fopen( probe, "w" );
		if( f ) { fclose( f ); unlink( probe ); break; }
		if( tries == 600 )
		{
			__android_log_print( ANDROID_LOG_ERROR, TAG, "no write access to %s (errno %d)", FX_DATA_DIR, errno );
			free( list );
			return;
		}
		mkdir( FX_DATA_DIR, 0775 );
		usleep( 500 * 1000 );
	}

	unsigned h = 2166136261u;  // FNV-1a of the index: changes with every APK whose files change
	for( size_t i = 0; i < len; i ++ )
		h = (h ^ (unsigned char) list[ i ]) * 16777619u;
	snprintf( hash, sizeof(hash), "%08x", h );
	snprintf( marker, sizeof(marker), "%s/.apk-files", FX_DATA_DIR );
	FILE *m = fopen( marker, "r" );
	if( m ) { if( ! fgets( old, sizeof(old), m ) ) old[ 0 ] = 0; fclose( m ); }
	if( ! strcmp( old, hash ) )
	{
		free( list );
		return;
	}

	int files = 0, written = 0, failed = 0;
	double mb = 0;
	Uint32 start = SDL_GetTicks();
	for( char *line = strtok( list, "\n" ); line; line = strtok( NULL, "\n" ) )
	{
		char *tab = strchr( line, '\t' );
		if( ! tab )
			continue;
		const char *name = tab + 1;
		char dest[ 1024 ];
		struct stat st;
		files ++;
		snprintf( dest, sizeof(dest), "%s/%s", FX_DATA_DIR, name );
		bool settings = ! strncmp( name, "configs/", 8 ) || ! strncmp( name, "pilots/", 7 );
		if( settings && stat( dest, &st ) == 0 )
			continue;
		make_dirs( dest );
		if( write_asset( mgr, name, dest ) )
		{
			written ++;
			mb += atof( line ) / 1048576.0;
		}
		else
		{
			failed ++;
			__android_log_print( ANDROID_LOG_ERROR, TAG, "installing %s failed (errno %d)", name, errno );
		}
	}
	__android_log_print( ANDROID_LOG_INFO, TAG, "packed files: %d listed, %d written (%.0f MB), %d failed, %.1f s",
		files, written, mb, failed, ( SDL_GetTicks() - start ) / 1000.0 );
	if( ! failed && (m = fopen( marker, "w" )) )
	{
		fputs( hash, m );
		fclose( m );
	}
	free( list );
}

bool fx_android_init( void )
{
	struct stat st;
	redirect_output();
	install_crash_handler();
	setenv( "ALSOFT_LOGLEVEL", "2", 0 );  // OpenAL Soft warnings/errors -> stderr -> logcat
	install_packed_files();

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
