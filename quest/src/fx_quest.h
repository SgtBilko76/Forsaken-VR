/*
 * fx_quest.h - Meta Quest platform layer for Forsaken (Android + OpenXR).
 *
 * The game keeps its desktop render path (GL3 renderer on OpenGL ES 3.2) and draws every frame
 * into an offscreen framebuffer. The OpenXR host shows that image in the headset and turns the
 * Touch controllers into SDL input.
 */
#ifndef FX_QUEST_H
#define FX_QUEST_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Data folder on the headset (configs, data, pilots, scripts, savegame).
#define FX_DATA_DIR "/sdcard/Forsaken"

// Logging to logcat, crash handler, chdir into FX_DATA_DIR. Call first thing in main().
bool fx_android_init( void );

// After the GL ES context exists: starts OpenXR, creates the game framebuffer and binds it.
// Returns the game's render size (the virtual screen's pixel size).
bool fx_vr_init( int *width, int *height );

// End of a game frame: shows it in the headset, waits for the next headset frame, reads the
// controllers, and binds the game framebuffer again.
void fx_vr_present( void );

// Touch controllers as one virtual joystick (created by fx_vr_init, before the game opens joysticks).
// The game binds it by name; default flight layout in config.c ConfigureQuestTouch().
#define FX_TOUCH_JOYSTICK_NAME "Quest Touch Controllers"
enum
{
	FX_TOUCH_AXIS_LEFT_X,     // slide left/right
	FX_TOUCH_AXIS_LEFT_Y,     // forward (-) / back (+)
	FX_TOUCH_AXIS_RIGHT_X,    // turn left/right
	FX_TOUCH_AXIS_RIGHT_Y,    // nose down (-) / up (+)
	FX_TOUCH_AXES
};
enum
{
	FX_TOUCH_BUTTON_RIGHT_TRIGGER,
	FX_TOUCH_BUTTON_LEFT_TRIGGER,
	FX_TOUCH_BUTTON_RIGHT_GRIP,
	FX_TOUCH_BUTTON_LEFT_GRIP,
	FX_TOUCH_BUTTON_A,
	FX_TOUCH_BUTTON_B,
	FX_TOUCH_BUTTON_X,
	FX_TOUCH_BUTTON_Y,
	FX_TOUCH_BUTTON_LEFT_STICK,
	FX_TOUCH_BUTTON_RIGHT_STICK,
	FX_TOUCH_BUTTONS
};

// True while the player is flying (in a level, no menu open): Touch drives the virtual joystick.
// Otherwise the controllers send menu keys.
bool fx_game_flying( void );

// --- Stereo (flight) ---
// One eye's view for the frame that is being rendered.
typedef struct
{
	int width, height;          // eye image size in pixels
	float rot[ 3 ][ 3 ];        // eye -> head-origin rotation in game camera axes (x right, y up, z forward), column vectors
	float pos[ 3 ];             // eye position relative to the recentred head origin, metres, game axes
	float tan_left, tan_right, tan_down, tan_up;  // asymmetric field of view (left/down negative)
} fx_eye_t;

// True if this frame can be rendered in stereo (headset frame open, head pose valid).
bool fx_vr_stereo_ready( void );
// Binds the eye's framebuffer and returns its view. eye: 0 = left, 1 = right.
bool fx_vr_begin_eye( int eye, fx_eye_t *out );
// Damage glow over the current eye image: colour strongest at the edge of view, faint in the centre.
void fx_vr_eye_tint( float r, float g, float b, float strength );
// After both eyes: binds the HUD target (game framebuffer, cleared transparent). hud_fov = the game's
// horizontal FOV in degrees; hud_scale = panel size relative to it (1 = overlays line up with the 3D view;
// title screens, black is see-through).
void fx_vr_end_eyes( float hud_fov, float hud_scale );

#ifdef __cplusplus
}
#endif

#endif
