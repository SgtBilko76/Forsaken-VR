#include "main.h"
#include "util.h"
#include "sound.h"
#include "music.h"
#include "timer.h"

extern BYTE MyGameStatus;

// number of tracks in the integrated Forsaken Original Game Soundtrack (Vol. 1)
#define MUSIC_TRACK_COUNT 27

// track table: integrated soundtrack track paths (44.1kHz stereo WAV)
static const char *MusicTrackPaths[ MUSIC_TRACK_COUNT ] = {
	"data\\sound\\music\\forsaken_menu_music_option_1.wav",
	"data\\sound\\music\\forsaken_02_kill_for_mother.wav",
	"data\\sound\\music\\forsaken_03_ready_or_not.wav",
	"data\\sound\\music\\forsaken_04_viral_infection.wav",
	"data\\sound\\music\\forsaken_05_domino_effect.wav",
	"data\\sound\\music\\forsaken_06_hotel_robloxia.wav",
	"data\\sound\\music\\forsaken_07_shadows.wav",
	"data\\sound\\music\\forsaken_08_resting_place.wav",
	"data\\sound\\music\\forsaken_09_sundowner.wav",
	"data\\sound\\music\\forsaken_10_my_cool_bluu_tune.wav",
	"data\\sound\\music\\forsaken_11_cut_throat_tea_party.wav",
	"data\\sound\\music\\forsaken_12_debito_di_sonno.wav",
	"data\\sound\\music\\forsaken_13_bad_faith.wav",
	"data\\sound\\music\\forsaken_14_one_stud_angel.wav",
	"data\\sound\\music\\forsaken_15_don_t_touch_my_chainsaw.wav",
	"data\\sound\\music\\forsaken_16_the_end_is_nigh.wav",
	"data\\sound\\music\\forsaken_17_killer_vanity.wav",
	"data\\sound\\music\\forsaken_18_ff9cae.wav",
	"data\\sound\\music\\forsaken_19_cool_one.wav",
	"data\\sound\\music\\forsaken_20_banana_bus_breakdown.wav",
	"data\\sound\\music\\forsaken_21_chimes_of_the_lucid.wav",
	"data\\sound\\music\\forsaken_22_corrupted_reflection.wav",
	"data\\sound\\music\\forsaken_23_just_survivors_and_killers.wav",
	"data\\sound\\music\\forsaken_24_obsession.wav",
	"data\\sound\\music\\forsaken_25_unreggcognizable.wav",
	"data\\sound\\music\\forsaken_26_a_grave_soul_now_run.wav",
	"data\\sound\\music\\forsaken_27_plead.wav",
};

// duration of every integrated soundtrack track in seconds (Forsaken OST Vol. 1, 2025)
static const float MusicTrackDurations[ MUSIC_TRACK_COUNT ] = {
	96.65f, 122.12f, 109.04f, 132.06f, 96.01f, 58.09f, 75.17f, 108.25f,
	154.09f, 107.90f, 164.47f, 157.64f, 125.92f, 141.78f, 91.99f, 163.76f,
	79.23f, 115.66f, 215.44f, 72.96f, 112.02f, 93.20f, 140.95f, 220.81f,
	163.23f, 73.17f, 90.33f,
};

// gain for the music source (millibels, 0 = no change, -10000 = silence)
#define MUSIC_VOLUME_MILLI (-800)

static sound_source_t *MusicSource = NULL;
static sound_buffer_t *MusicBuffer = NULL;
static int MusicTrackIndex = 0;
static char MusicFile[ 1024 ] = { 0 };
static bool MusicIsPlaying = false;
static px_timer_t MusicTimer;
static float MusicTrackDuration = 0.0F;
static int MusicGameplayLevel = 0;
static int MusicGameplayTrackIndex = 1;
static bool MusicWasInGameplay = false;

bool Music_Init( void )
{
	timer_clear( & MusicTimer );
	return true;
}

void Music_Shutdown( void )
{
	Music_Stop();
}

bool Music_Start( const char *path )
{
	// release the previous track before loading a new one
	if( MusicSource != NULL )
	{
		sound_release_source( MusicSource );
		MusicSource = NULL;
	}
	if( MusicBuffer != NULL )
	{
		sound_release_buffer( MusicBuffer );
		MusicBuffer = NULL;
	}
	MusicIsPlaying = false;

	if( path == NULL )
		return false;

	strncpy( MusicFile, path, sizeof( MusicFile ) - 1 );
	MusicFile[ sizeof( MusicFile ) - 1 ] = 0;

	if( MusicTrackIndex < 0 || MusicTrackIndex >= MUSIC_TRACK_COUNT )
		MusicTrackIndex = 0;

	int resolved_index = -1;
	for( int i = 0; i < MUSIC_TRACK_COUNT; ++i )
	{
		if( strcmp( path, MusicTrackPaths[ i ] ) == 0 )
		{
			resolved_index = i;
			break;
		}
	}
	if( resolved_index >= 0 )
		MusicTrackIndex = resolved_index;

	MusicTrackDuration = MusicTrackDurations[ MusicTrackIndex ];

	MusicBuffer = sound_load( MusicFile );
	if( MusicBuffer == NULL )
	{
		DebugPrintf( "Music: failed to load '%s'\n", MusicFile );
		return false;
	}

	MusicSource = sound_source( MusicBuffer );
	if( MusicSource == NULL )
	{
		sound_release_buffer( MusicBuffer );
		MusicBuffer = NULL;
		return false;
	}

	timer_clear( & MusicTimer );
	MusicTrackDuration = MusicTrackDurations[ MusicTrackIndex ];

	sound_play_looping( MusicSource );
	sound_volume( MusicSource, MUSIC_VOLUME_MILLI );
	MusicIsPlaying = true;

	DebugPrintf( "Music: playing '%s'\n", MusicFile );

	return true;
}

void Music_Stop( void )
{
	if( MusicSource != NULL )
	{
		sound_stop( MusicSource );
		sound_release_source( MusicSource );
		MusicSource = NULL;
	}
	if( MusicBuffer != NULL )
	{
		sound_release_buffer( MusicBuffer );
		MusicBuffer = NULL;
	}
	MusicIsPlaying = false;
}

bool Music_IsPlaying( void )
{
	return MusicIsPlaying;
}

void Music_Update( void )
{
	bool playing_state = false;
	bool is_menu_state = false;

	switch( MyGameStatus )
	{
		case STATUS_Title:
		case STATUS_StartingMultiplayer:
		case STATUS_PreStartingMultiplayerSynch:
		case STATUS_StartingMultiplayerSynch:
			// title / menus -> user music (swapped first track)
			is_menu_state = true;
			playing_state = true;
			break;

		case STATUS_Normal:
		case STATUS_BetweenLevels:
		case STATUS_ChangeLevelPostInitView:
		case STATUS_ChangeLevelPostPlayingDemo:
		case STATUS_StartingSinglePlayer:
		case STATUS_PostStartingSinglePlayer:
		case STATUS_SinglePlayer:
		case STATUS_PlayingDemo:
			playing_state = true;
			break;

		default:
			break;
	}

	if( !playing_state )
	{
		Music_Stop();
		return;
	}

	// a level just started -> use that level's single track and loop it
	if( playing_state && !is_menu_state && !MusicWasInGameplay )
	{
		MusicGameplayLevel += 1;
		if( MusicGameplayLevel > MUSIC_TRACK_COUNT )
			MusicGameplayLevel = MUSIC_TRACK_COUNT;
		MusicGameplayTrackIndex = MusicGameplayLevel;
		MusicWasInGameplay = true;
		Music_Stop();
		Music_Start( MusicTrackPaths[ MusicGameplayTrackIndex ] );
		return;
	}

	bool was_in_gameplay = MusicWasInGameplay;
	if( is_menu_state )
		MusicWasInGameplay = false;

	// back in a menu state -> stop the level music; the start block plays the user music there
	if( is_menu_state && was_in_gameplay )
	{
		Music_Stop();
		return;
	}

	// start the music: menu -> swapped first track (user music), gameplay -> fixed level track
	if( !MusicIsPlaying )
	{
		int track_index = is_menu_state ? 0 : MusicGameplayTrackIndex;
		if( is_menu_state )
			MusicTrackIndex = 0;
		else
			MusicTrackIndex = MusicGameplayTrackIndex;
		Music_Start( MusicTrackPaths[ track_index ] );
		return;
	}

	// play the next track as soon as the current one reached its end
	float elapsed = timer_run( & MusicTimer );
	if( elapsed >= MusicTrackDuration )
	{
		if( is_menu_state && MusicTrackIndex == 0 )
		{
			// the swapped user music reached its end -> continue with the next track, which repeats
			MusicTrackIndex = ( MusicTrackIndex + 1 ) % MUSIC_TRACK_COUNT;
			Music_Stop();
			Music_Start( MusicTrackPaths[ MusicTrackIndex ] );
			return;
		}

		// gameplay -> only one track per level -> loop it
		Music_Stop();
		Music_Start( MusicTrackPaths[ MusicTrackIndex ] );
	}
}
