/*
  This file is shared internally by render_* family.
  For public facing header please use render.h
*/
#ifdef GL
#ifndef RENDER_GL_SHARED_INCLUDED
#define RENDER_GL_SHARED_INCLUDED

#include "main.h"
#include "util.h"
#include "render.h"
#include "texture.h"
#include "file.h"
#include <stdio.h>
#include "main_sdl.h"
// declare GL 2.0+ entry points (glCreateShader, glGetStringi, ...); without this
// they are implicitly declared as returning int and pointers get truncated on 64-bit
#define GL_GLEXT_PROTOTYPES 1
#ifdef GLES
// OpenGL ES 3.2 (Meta Quest): the GL3 renderer with a few desktop-only calls mapped.
#include <GLES3/gl32.h>
#include <GLES2/gl2ext.h>
#define glClearDepth(d)        glClearDepthf(d)
#define glDepthRange(n,f)      glDepthRangef(n,f)
#define glPolygonMode(face,m)  ((void)0)   // wireframe/points debug modes only
#ifndef GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT
#define GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT 0x84FF
#endif
#ifndef GL_TEXTURE_MAX_ANISOTROPY_EXT
#define GL_TEXTURE_MAX_ANISOTROPY_EXT 0x84FE
#endif
#else
#include "SDL_opengl.h"
#ifndef MACOSX
#include <GL/glu.h>
#endif
#endif

extern render_info_t render_info;

extern GLenum render_last_gl_error;

// TODO invalid pointer
#if (defined(MACOSX) && SDL_VERSION_ATLEAST(2,0,0)) || defined(GLES)
#define gluErrorString(e)\
	(e == 0x0500 ? "invalid enumerant" : \
	(e == 0x0501 ? "invalid value" : \
	(e == 0x0502 ? "invalid operation" : \
	(e == 0x0503 ? "stack overflow" : \
	(e == 0x0504 ? "stack underflow" : \
	(e == 0x0505 ? "out of memory" : \
	(e == 0x0506 ? "invalid framebuffer operation" : \
	(e == 0x8031 ? "table too large" : \
	 "unknown" \
	))))))))
#endif

const char * render_error_description( int e );

#define CHECK_GL_ERRORS \
	do \
	{ \
		GLenum e; \
		while( ( e = glGetError() ) != GL_NO_ERROR ) \
		{ \
			render_last_gl_error = e; \
			DebugPrintf( "GL error: %s (%s:%d)\n", \
				gluErrorString(e),  __FILE__, __LINE__ ); \
		} \
	} while (0)


typedef struct { float anisotropic; } gl_caps_t;
extern gl_caps_t caps;

typedef struct { GLuint id; } texture_t; // Possibly later: GLuint bump_id;

//
// d3d stored the world/view matrixes
// and then multiplied them together before rendering
// in the following order: world * view * projection
// opengl handles only world and projection
// so we must emulate the behavior of world*view
// although we multiply the arguments backwards view*world
//

extern MATRIX proj_matrix;
extern MATRIX view_matrix;
extern MATRIX world_matrix;

#if GL != 1

void mvp_update( GLuint current_program );

extern GLuint vertex_shader;
extern GLuint fragment_shader;
extern GLuint current_program;

LPVERTEXBUFFER _create_buffer( int size, GLenum type, GLenum gettype, GLenum usage );

#define create_buffer( size, type, usage ) \
        _create_buffer( size, type, type ## _BINDING, usage )

#endif // GL != 1

void FSReleaseRenderObject(RENDEROBJECT *renderObject);

#endif // RENDER_GL_SHARED_INCLUDED
#endif // GL ENABLED
