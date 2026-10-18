/*
 * fx_game.c - small queries into the game's state for the Quest layer.
 */
#include "fx_quest.h"
#include "main.h"
#include "title.h"

extern BYTE MyGameStatus;

bool fx_game_flying( void )
{
	return ( MyGameStatus == STATUS_SinglePlayer || MyGameStatus == STATUS_Normal )
		&& CurrentMenu == NULL;
}
