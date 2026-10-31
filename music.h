#ifndef MUSIC_INCLUDED
#define MUSIC_INCLUDED

#include "main.h"

// ---------------------------------------------------------------------------
// Integrated Forsaken Original Game Soundtrack (Vol. 1) playback.
//
// The game ships without any music sources in the sound system, so the
// approved OST tracks are integrated into the data folder as 44.1kHz stereo
// WAV files and played through OpenAL. The first track starts on the main menu,
// then the next track starts as soon as the current one reached its end.
// ---------------------------------------------------------------------------

bool Music_Init( void );
void Music_Shutdown( void );

bool Music_Start( const char *path );
void Music_Stop( void );
bool Music_IsPlaying( void );

void Music_Update( void );

#endif
