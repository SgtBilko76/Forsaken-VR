/*
 * fx_openxr.cpp - OpenXR host for Forsaken on Meta Quest (OpenGL ES 3.2).
 *
 * Stage 1 ("virtual screen"): the game renders each frame into an offscreen framebuffer at a
 * fixed size; fx_vr_present() copies it into an OpenXR quad layer floating in front of the
 * player. Touch controllers drive the menus by injected key presses.
 *
 * Session/action code follows X-Wing Revival VR's RaptorEngine/VR/OpenXR.cpp.
 */
#include "fx_quest.h"

#include <SDL.h>
#include <SDL_system.h>
#include <EGL/egl.h>
#include <GLES3/gl32.h>
#include <GLES2/gl2ext.h>
#include <android/log.h>
#include <jni.h>
#include <cmath>
#include <cstring>
#include <vector>

#define XR_USE_PLATFORM_ANDROID 1
#define XR_USE_GRAPHICS_API_OPENGL_ES 1
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#ifndef GL_FRAMEBUFFER_SRGB_EXT
#define GL_FRAMEBUFFER_SRGB_EXT 0x8DB9
#endif

#define TAG "ForsakenXR"
#define LOGI(...) __android_log_print( ANDROID_LOG_INFO, TAG, __VA_ARGS__ )
#define LOGE(...) __android_log_print( ANDROID_LOG_ERROR, TAG, __VA_ARGS__ )
#define XR_CHECK(expr) do { XrResult res_ = (expr); if( XR_FAILED(res_) ) { LOGE( "%s failed (%d)", #expr, (int) res_ ); return false; } } while(0)

namespace
{
	// Game render size = virtual screen pixels (4:3 like the original game).
	const int GameW = 1280, GameH = 960;
	// Virtual screen: 2.4 m wide, 2 m in front of the recentred head.
	const float ScreenDist = 2.0f, ScreenWidth = 2.4f;

	XrInstance Instance = XR_NULL_HANDLE;
	XrSystemId System = XR_NULL_SYSTEM_ID;
	XrSession Session = XR_NULL_HANDLE;
	XrSpace BaseSpace = XR_NULL_HANDLE;   // raw LOCAL
	XrSpace AppSpace = XR_NULL_HANDLE;    // LOCAL re-anchored by recentring
	XrSpace ViewSpace = XR_NULL_HANDLE;
	XrSessionState State = XR_SESSION_STATE_UNKNOWN;
	bool Running = false;
	bool RecenterPending = true;
	bool PerfSettings = false;
	bool BDControllers = false;
	XrTime DisplayTime = 0;
	float DisplayRefresh = 72.f;

	// Touch controller input.
	XrActionSet ActionSet = XR_NULL_HANDLE;
	XrAction ActTrigger, ActGrip, ActStick, ActStickClick, ActLower, ActUpper, ActMenu, ActAim, ActHaptic;
	XrPath HandPaths[ 2 ];
	XrSpace AimSpaces[ 2 ] = { XR_NULL_HANDLE, XR_NULL_HANDLE };

	struct Hand
	{
		float Trigger, Grip, StickX, StickY;
		bool StickClick, Lower, Upper, Menu;
	};
	Hand Hands[ 2 ];

	// Screen swapchain + one framebuffer per image.
	XrSwapchain Screen = XR_NULL_HANDLE;
	std::vector<XrSwapchainImageOpenGLESKHR> ScreenImages;
	std::vector<GLuint> ScreenFBOs;

	// The game's own framebuffer (flat screen in menus, HUD panel in flight).
	GLuint GameFBO = 0, GameColor = 0, GameDepth = 0;

	// Stereo eyes (flight): one swapchain + framebuffers + depth per eye.
	struct EyeTarget
	{
		XrSwapchain Handle = XR_NULL_HANDLE;
		int W = 0, H = 0;
		std::vector<XrSwapchainImageOpenGLESKHR> Images;
		std::vector<GLuint> FBOs;
		GLuint Depth = 0;
		bool Acquired = false;
	};
	EyeTarget Eyes[ 2 ];
	int64_t SwapFormat = 0;

	// Frame state: a headset frame is open (xrWaitFrame + xrBeginFrame done) while the game renders.
	bool FrameOpen = false;
	bool ShouldRender = false;
	bool ViewsValid = false;
	XrView Views[ 2 ] = { { XR_TYPE_VIEW }, { XR_TYPE_VIEW } };
	bool HudMode = false;      // eyes rendered this frame; the game framebuffer holds the HUD
	float HudFov = 90.f;       // the game's horizontal field of view (HUD overlays are laid out for it)
	const float HudDist = 5.0f;
	// HUD panel size relative to the game's field of view, set per frame by the game: < 1 pulls the corner
	// read-outs toward the centre (flight), 1 = overlays line up with the 3D view (title screens).
	float HudScale = 1.f;
	bool HudKeyBlack = false;  // title screens: black around the menu text is see-through

	// HUD copy: game framebuffer -> HUD swapchain, alpha = brightness (the HUD is drawn additively on black).
	GLuint HudProgram = 0;
	GLuint TintProgram = 0;


	bool HasExtension( const std::vector<XrExtensionProperties> &props, const char *name )
	{
		for( size_t i = 0; i < props.size(); i ++ )
			if( strcmp( props[ i ].extensionName, name ) == 0 )
				return true;
		return false;
	}

	bool InitInstance( void )
	{
		JNIEnv *env = (JNIEnv *) SDL_AndroidGetJNIEnv();
		JavaVM *vm = NULL;
		env->GetJavaVM( &vm );
		jobject activity = (jobject) SDL_AndroidGetActivity();

		PFN_xrInitializeLoaderKHR init_loader = NULL;
		XR_CHECK( xrGetInstanceProcAddr( XR_NULL_HANDLE, "xrInitializeLoaderKHR", (PFN_xrVoidFunction *) &init_loader ) );
		XrLoaderInitInfoAndroidKHR loader_info = { XR_TYPE_LOADER_INIT_INFO_ANDROID_KHR };
		loader_info.applicationVM = vm;
		loader_info.applicationContext = activity;
		XR_CHECK( init_loader( (const XrLoaderInitInfoBaseHeaderKHR *) &loader_info ) );

		uint32_t count = 0;
		XR_CHECK( xrEnumerateInstanceExtensionProperties( NULL, 0, &count, NULL ) );
		std::vector<XrExtensionProperties> props( count, { XR_TYPE_EXTENSION_PROPERTIES } );
		XR_CHECK( xrEnumerateInstanceExtensionProperties( NULL, count, &count, props.data() ) );

		std::vector<const char *> enabled;
		enabled.push_back( XR_KHR_OPENGL_ES_ENABLE_EXTENSION_NAME );
		enabled.push_back( XR_KHR_ANDROID_CREATE_INSTANCE_EXTENSION_NAME );
		if( HasExtension( props, "XR_BD_controller_interaction" ) )
		{
			enabled.push_back( "XR_BD_controller_interaction" );
			BDControllers = true;
		}
		if( HasExtension( props, "XR_FB_display_refresh_rate" ) )
			enabled.push_back( "XR_FB_display_refresh_rate" );
		if( HasExtension( props, XR_EXT_PERFORMANCE_SETTINGS_EXTENSION_NAME ) )
		{
			enabled.push_back( XR_EXT_PERFORMANCE_SETTINGS_EXTENSION_NAME );
			PerfSettings = true;
		}

		XrInstanceCreateInfoAndroidKHR android_info = { XR_TYPE_INSTANCE_CREATE_INFO_ANDROID_KHR };
		android_info.applicationVM = vm;
		android_info.applicationActivity = activity;
		XrInstanceCreateInfo create = { XR_TYPE_INSTANCE_CREATE_INFO };
		create.next = &android_info;
		create.enabledExtensionCount = (uint32_t) enabled.size();
		create.enabledExtensionNames = enabled.data();
		strcpy( create.applicationInfo.applicationName, "Forsaken VR" );
		strcpy( create.applicationInfo.engineName, "ProjectX" );
		create.applicationInfo.applicationVersion = 1;
		create.applicationInfo.engineVersion = 1;
		create.applicationInfo.apiVersion = XR_MAKE_VERSION( 1, 0, 34 );
		XR_CHECK( xrCreateInstance( &create, &Instance ) );

		XrSystemGetInfo sys = { XR_TYPE_SYSTEM_GET_INFO };
		sys.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
		XR_CHECK( xrGetSystem( Instance, &sys, &System ) );
		return true;
	}

	XrPath Path( const char *str )
	{
		XrPath path = XR_NULL_PATH;
		xrStringToPath( Instance, str, &path );
		return path;
	}

	bool CreateAction( XrAction *action, XrActionType type, const char *name, const char *localized )
	{
		XrActionCreateInfo info = { XR_TYPE_ACTION_CREATE_INFO };
		info.actionType = type;
		strcpy( info.actionName, name );
		strcpy( info.localizedActionName, localized );
		info.countSubactionPaths = 2;
		info.subactionPaths = HandPaths;
		XR_CHECK( xrCreateAction( ActionSet, &info, action ) );
		return true;
	}

	bool InitActions( void )
	{
		XrActionSetCreateInfo set_info = { XR_TYPE_ACTION_SET_CREATE_INFO };
		strcpy( set_info.actionSetName, "gameplay" );
		strcpy( set_info.localizedActionSetName, "Gameplay" );
		XR_CHECK( xrCreateActionSet( Instance, &set_info, &ActionSet ) );

		HandPaths[ 0 ] = Path( "/user/hand/left" );
		HandPaths[ 1 ] = Path( "/user/hand/right" );

		if( ! ( CreateAction( &ActTrigger,    XR_ACTION_TYPE_FLOAT_INPUT,      "trigger",      "Trigger" )
		     && CreateAction( &ActGrip,       XR_ACTION_TYPE_FLOAT_INPUT,      "grip",         "Grip" )
		     && CreateAction( &ActStick,      XR_ACTION_TYPE_VECTOR2F_INPUT,   "thumbstick",   "Thumbstick" )
		     && CreateAction( &ActStickClick, XR_ACTION_TYPE_BOOLEAN_INPUT,    "stick_click",  "Thumbstick click" )
		     && CreateAction( &ActLower,      XR_ACTION_TYPE_BOOLEAN_INPUT,    "button_lower", "A / X" )
		     && CreateAction( &ActUpper,      XR_ACTION_TYPE_BOOLEAN_INPUT,    "button_upper", "B / Y" )
		     && CreateAction( &ActMenu,       XR_ACTION_TYPE_BOOLEAN_INPUT,    "menu",         "Menu" )
		     && CreateAction( &ActAim,        XR_ACTION_TYPE_POSE_INPUT,       "aim",          "Aim" )
		     && CreateAction( &ActHaptic,     XR_ACTION_TYPE_VIBRATION_OUTPUT, "haptic",       "Haptic" ) ) )
			return false;

		struct { XrAction action; const char *path; } binds[] = {
			{ ActTrigger,    "/user/hand/left/input/trigger/value" },
			{ ActTrigger,    "/user/hand/right/input/trigger/value" },
			{ ActGrip,       "/user/hand/left/input/squeeze/value" },
			{ ActGrip,       "/user/hand/right/input/squeeze/value" },
			{ ActStick,      "/user/hand/left/input/thumbstick" },
			{ ActStick,      "/user/hand/right/input/thumbstick" },
			{ ActStickClick, "/user/hand/left/input/thumbstick/click" },
			{ ActStickClick, "/user/hand/right/input/thumbstick/click" },
			{ ActLower,      "/user/hand/left/input/x/click" },
			{ ActLower,      "/user/hand/right/input/a/click" },
			{ ActUpper,      "/user/hand/left/input/y/click" },
			{ ActUpper,      "/user/hand/right/input/b/click" },
			{ ActMenu,       "/user/hand/left/input/menu/click" },
			{ ActAim,        "/user/hand/left/input/aim/pose" },
			{ ActAim,        "/user/hand/right/input/aim/pose" },
			{ ActHaptic,     "/user/hand/left/output/haptic" },
			{ ActHaptic,     "/user/hand/right/output/haptic" },
		};
		std::vector<XrActionSuggestedBinding> suggested;
		for( size_t i = 0; i < sizeof(binds) / sizeof(binds[ 0 ]); i ++ )
			suggested.push_back( { binds[ i ].action, Path( binds[ i ].path ) } );
		XrInteractionProfileSuggestedBinding profile = { XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING };
		profile.interactionProfile = Path( "/interaction_profiles/oculus/touch_controller" );
		profile.countSuggestedBindings = (uint32_t) suggested.size();
		profile.suggestedBindings = suggested.data();
		XR_CHECK( xrSuggestInteractionProfileBindings( Instance, &profile ) );

		if( BDControllers )
		{
			const char *pico[] = { "/interaction_profiles/bytedance/pico4_controller", "/interaction_profiles/bytedance/pico_neo3_controller" };
			for( size_t i = 0; i < 2; i ++ )
			{
				profile.interactionProfile = Path( pico[ i ] );
				xrSuggestInteractionProfileBindings( Instance, &profile );
			}
		}
		return true;
	}

	bool InitSession( void )
	{
		EGLDisplay display = eglGetCurrentDisplay();
		EGLContext context = eglGetCurrentContext();
		if( display == EGL_NO_DISPLAY || context == EGL_NO_CONTEXT )
		{
			LOGE( "no current EGL context" );
			return false;
		}
		EGLint config_id = 0, n = 0;
		eglQueryContext( display, context, EGL_CONFIG_ID, &config_id );
		EGLConfig config = NULL;
		const EGLint attrs[] = { EGL_CONFIG_ID, config_id, EGL_NONE };
		if( ! eglChooseConfig( display, attrs, &config, 1, &n ) || n != 1 )
		{
			LOGE( "could not recover EGLConfig" );
			return false;
		}

		PFN_xrGetOpenGLESGraphicsRequirementsKHR get_reqs = NULL;
		XR_CHECK( xrGetInstanceProcAddr( Instance, "xrGetOpenGLESGraphicsRequirementsKHR", (PFN_xrVoidFunction *) &get_reqs ) );
		XrGraphicsRequirementsOpenGLESKHR reqs = { XR_TYPE_GRAPHICS_REQUIREMENTS_OPENGL_ES_KHR };
		XR_CHECK( get_reqs( Instance, System, &reqs ) );

		XrGraphicsBindingOpenGLESAndroidKHR binding = { XR_TYPE_GRAPHICS_BINDING_OPENGL_ES_ANDROID_KHR };
		binding.display = display;
		binding.config = config;
		binding.context = context;
		XrSessionCreateInfo create = { XR_TYPE_SESSION_CREATE_INFO };
		create.next = &binding;
		create.systemId = System;
		XR_CHECK( xrCreateSession( Instance, &create, &Session ) );

		XrReferenceSpaceCreateInfo space = { XR_TYPE_REFERENCE_SPACE_CREATE_INFO };
		space.poseInReferenceSpace.orientation.w = 1.f;
		space.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
		XR_CHECK( xrCreateReferenceSpace( Session, &space, &BaseSpace ) );
		XR_CHECK( xrCreateReferenceSpace( Session, &space, &AppSpace ) );
		space.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
		XR_CHECK( xrCreateReferenceSpace( Session, &space, &ViewSpace ) );

		for( int h = 0; h < 2; h ++ )
		{
			XrActionSpaceCreateInfo aim = { XR_TYPE_ACTION_SPACE_CREATE_INFO };
			aim.action = ActAim;
			aim.subactionPath = HandPaths[ h ];
			aim.poseInActionSpace.orientation.w = 1.f;
			XR_CHECK( xrCreateActionSpace( Session, &aim, &AimSpaces[ h ] ) );
		}
		XrSessionActionSetsAttachInfo attach = { XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO };
		attach.countActionSets = 1;
		attach.actionSets = &ActionSet;
		XR_CHECK( xrAttachSessionActionSets( Session, &attach ) );

		// 72 Hz, highest sustained clocks.
		PFN_xrEnumerateDisplayRefreshRatesFB enum_rates = NULL;
		PFN_xrRequestDisplayRefreshRateFB request_rate = NULL;
		PFN_xrGetDisplayRefreshRateFB get_rate = NULL;
		if( XR_SUCCEEDED( xrGetInstanceProcAddr( Instance, "xrEnumerateDisplayRefreshRatesFB", (PFN_xrVoidFunction *) &enum_rates ) ) && enum_rates
		 && XR_SUCCEEDED( xrGetInstanceProcAddr( Instance, "xrRequestDisplayRefreshRateFB", (PFN_xrVoidFunction *) &request_rate ) ) && request_rate
		 && XR_SUCCEEDED( xrGetInstanceProcAddr( Instance, "xrGetDisplayRefreshRateFB", (PFN_xrVoidFunction *) &get_rate ) ) && get_rate )
		{
			uint32_t rate_count = 0;
			enum_rates( Session, 0, &rate_count, NULL );
			std::vector<float> rates( rate_count );
			enum_rates( Session, rate_count, &rate_count, rates.data() );
			float best = 0.f;
			for( float r : rates )
				if( r <= 72.5f && r > best )
					best = r;
			if( best > 0.f )
				request_rate( Session, best );
			get_rate( Session, &DisplayRefresh );
		}
		PFN_xrPerfSettingsSetPerformanceLevelEXT set_level = NULL;
		if( PerfSettings && XR_SUCCEEDED( xrGetInstanceProcAddr( Instance, "xrPerfSettingsSetPerformanceLevelEXT", (PFN_xrVoidFunction *) &set_level ) ) && set_level )
		{
			set_level( Session, XR_PERF_SETTINGS_DOMAIN_CPU_EXT, XR_PERF_SETTINGS_LEVEL_BOOST_EXT );
			set_level( Session, XR_PERF_SETTINGS_DOMAIN_GPU_EXT, XR_PERF_SETTINGS_LEVEL_SUSTAINED_HIGH_EXT );
		}
		return true;
	}

	bool InitScreen( void )
	{
		uint32_t count = 0;
		XR_CHECK( xrEnumerateSwapchainFormats( Session, 0, &count, NULL ) );
		std::vector<int64_t> formats( count );
		XR_CHECK( xrEnumerateSwapchainFormats( Session, count, &count, formats.data() ) );

		// The game's colours are already gamma-encoded: sRGB swapchain only if the write conversion can be turned off.
		const char *ext = (const char *) glGetString( GL_EXTENSIONS );
		bool srgb_control = ext && strstr( ext, "GL_EXT_sRGB_write_control" );
		int64_t format = 0;
		for( int64_t f : formats )
		{
			if( f == GL_SRGB8_ALPHA8 && srgb_control )
				format = f;
			else if( f == GL_RGBA8 && ! format )
				format = f;
		}
		if( ! format )
			format = formats[ 0 ];
		if( format == GL_SRGB8_ALPHA8 )
			glDisable( GL_FRAMEBUFFER_SRGB_EXT );
		SwapFormat = format;

		XrSwapchainCreateInfo info = { XR_TYPE_SWAPCHAIN_CREATE_INFO };
		info.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
		info.format = format;
		info.sampleCount = 1;
		info.width = GameW;
		info.height = GameH;
		info.faceCount = 1;
		info.arraySize = 1;
		info.mipCount = 1;
		XR_CHECK( xrCreateSwapchain( Session, &info, &Screen ) );

		uint32_t images = 0;
		XR_CHECK( xrEnumerateSwapchainImages( Screen, 0, &images, NULL ) );
		ScreenImages.assign( images, { XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_ES_KHR } );
		XR_CHECK( xrEnumerateSwapchainImages( Screen, images, &images, (XrSwapchainImageBaseHeader *) ScreenImages.data() ) );
		ScreenFBOs.assign( images, 0 );
		glGenFramebuffers( images, ScreenFBOs.data() );
		for( uint32_t i = 0; i < images; i ++ )
		{
			glBindFramebuffer( GL_FRAMEBUFFER, ScreenFBOs[ i ] );
			glFramebufferTexture2D( GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, ScreenImages[ i ].image, 0 );
			if( glCheckFramebufferStatus( GL_FRAMEBUFFER ) != GL_FRAMEBUFFER_COMPLETE )
			{
				LOGE( "screen framebuffer %u incomplete", i );
				return false;
			}
		}
		LOGI( "screen swapchain %dx%d format 0x%x (sRGB write control %s), %u images",
			GameW, GameH, (unsigned) format, srgb_control ? "yes" : "no", images );

		// The game's framebuffer.
		glGenTextures( 1, &GameColor );
		glBindTexture( GL_TEXTURE_2D, GameColor );
		glTexStorage2D( GL_TEXTURE_2D, 1, GL_RGBA8, GameW, GameH );
		glGenRenderbuffers( 1, &GameDepth );
		glBindRenderbuffer( GL_RENDERBUFFER, GameDepth );
		glRenderbufferStorage( GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, GameW, GameH );
		glGenFramebuffers( 1, &GameFBO );
		glBindFramebuffer( GL_FRAMEBUFFER, GameFBO );
		glFramebufferTexture2D( GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, GameColor, 0 );
		glFramebufferRenderbuffer( GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, GameDepth );
		if( glCheckFramebufferStatus( GL_FRAMEBUFFER ) != GL_FRAMEBUFFER_COMPLETE )
		{
			LOGE( "game framebuffer incomplete" );
			return false;
		}
		glViewport( 0, 0, GameW, GameH );
		return true;
	}

	bool InitEyes( void )
	{
		uint32_t count = 0;
		XR_CHECK( xrEnumerateViewConfigurationViews( Instance, System, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &count, NULL ) );
		if( count != 2 )
			return false;
		XrViewConfigurationView config[ 2 ] = { { XR_TYPE_VIEW_CONFIGURATION_VIEW }, { XR_TYPE_VIEW_CONFIGURATION_VIEW } };
		XR_CHECK( xrEnumerateViewConfigurationViews( Instance, System, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 2, &count, config ) );
		for( int e = 0; e < 2; e ++ )
		{
			EyeTarget *eye = &Eyes[ e ];
			eye->W = config[ e ].recommendedImageRectWidth;
			eye->H = config[ e ].recommendedImageRectHeight;
			// Stay near native Quest 3 resolution even if the runtime recommends supersampling.
			float scale = fminf( 1.f, fminf( 2064.f / eye->W, 2208.f / eye->H ) );
			eye->W = (int)( eye->W * scale ) & ~1;
			eye->H = (int)( eye->H * scale ) & ~1;

			XrSwapchainCreateInfo info = { XR_TYPE_SWAPCHAIN_CREATE_INFO };
			info.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
			info.format = SwapFormat;
			info.sampleCount = 1;
			info.width = eye->W;
			info.height = eye->H;
			info.faceCount = 1;
			info.arraySize = 1;
			info.mipCount = 1;
			XR_CHECK( xrCreateSwapchain( Session, &info, &eye->Handle ) );
			uint32_t images = 0;
			XR_CHECK( xrEnumerateSwapchainImages( eye->Handle, 0, &images, NULL ) );
			eye->Images.assign( images, { XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_ES_KHR } );
			XR_CHECK( xrEnumerateSwapchainImages( eye->Handle, images, &images, (XrSwapchainImageBaseHeader *) eye->Images.data() ) );

			glGenRenderbuffers( 1, &eye->Depth );
			glBindRenderbuffer( GL_RENDERBUFFER, eye->Depth );
			glRenderbufferStorage( GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, eye->W, eye->H );
			eye->FBOs.assign( images, 0 );
			glGenFramebuffers( images, eye->FBOs.data() );
			for( uint32_t i = 0; i < images; i ++ )
			{
				glBindFramebuffer( GL_FRAMEBUFFER, eye->FBOs[ i ] );
				glFramebufferTexture2D( GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, eye->Images[ i ].image, 0 );
				glFramebufferRenderbuffer( GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, eye->Depth );
				if( glCheckFramebufferStatus( GL_FRAMEBUFFER ) != GL_FRAMEBUFFER_COMPLETE )
				{
					LOGE( "eye %d framebuffer %u incomplete", e, i );
					return false;
				}
			}
			LOGI( "eye %d swapchain %dx%d, %u images", e, eye->W, eye->H, images );
		}
		glBindFramebuffer( GL_FRAMEBUFFER, GameFBO );
		return true;
	}

	GLuint CompileShader( GLenum type, const char *src )
	{
		GLuint sh = glCreateShader( type );
		glShaderSource( sh, 1, &src, NULL );
		glCompileShader( sh );
		GLint ok = 0;
		glGetShaderiv( sh, GL_COMPILE_STATUS, &ok );
		if( ! ok )
		{
			char log[ 512 ];
			glGetShaderInfoLog( sh, sizeof(log), NULL, log );
			LOGE( "HUD shader: %s", log );
		}
		return sh;
	}

	void InitHudProgram( void )
	{
		const char *vs =
			"#version 300 es\n"
			"out vec2 uv;\n"
			"void main() {\n"
			"  vec2 p = vec2( float( (gl_VertexID << 1) & 2 ), float( gl_VertexID & 2 ) );\n"
			"  uv = p;\n"
			"  gl_Position = vec4( p * 2.0 - 1.0, 0.0, 1.0 );\n"
			"}\n";
		const char *fs =
			"#version 300 es\n"
			"precision mediump float;\n"
			"uniform sampler2D tex;\n"
			"uniform float key_black;\n"
			"in vec2 uv;\n"
			"out vec4 color;\n"
			"void main() {\n"
			"  vec4 c = texture( tex, uv );\n"
			"  float lum = max( max( c.r, c.g ), c.b );\n"
			// premultiplied; key_black: black pixels see-through even where alpha is set (title menu text)
			"  color = vec4( c.rgb, mix( max( lum, c.a ), min( 1.0, lum * 3.0 ), key_black ) );\n"
			"}\n";
		HudProgram = glCreateProgram();
		GLuint vshader = CompileShader( GL_VERTEX_SHADER, vs );
		glAttachShader( HudProgram, vshader );
		glAttachShader( HudProgram, CompileShader( GL_FRAGMENT_SHADER, fs ) );
		glLinkProgram( HudProgram );

		// Damage glow: transparent in the middle, the colour rising toward the edge of view.
		const char *tint =
			"#version 300 es\n"
			"precision mediump float;\n"
			"uniform vec4 tint;\n"
			"in vec2 uv;\n"
			"out vec4 color;\n"
			"void main() {\n"
			"  float d = length( (uv - 0.5) * 2.0 );\n"
			"  float edge = 0.25 + 0.75 * smoothstep( 0.3, 1.1, d );\n"
			"  color = vec4( tint.rgb, tint.a * edge );\n"
			"}\n";
		TintProgram = glCreateProgram();
		glAttachShader( TintProgram, vshader );
		glAttachShader( TintProgram, CompileShader( GL_FRAGMENT_SHADER, tint ) );
		glLinkProgram( TintProgram );
	}


	XrVector3f Rotate( const XrQuaternionf &q, float x, float y, float z )
	{
		float tx = 2.f * (q.y * z - q.z * y);
		float ty = 2.f * (q.z * x - q.x * z);
		float tz = 2.f * (q.x * y - q.y * x);
		XrVector3f r;
		r.x = x + q.w * tx + (q.y * tz - q.z * ty);
		r.y = y + q.w * ty + (q.z * tx - q.x * tz);
		r.z = z + q.w * tz + (q.x * ty - q.y * tx);
		return r;
	}

	void DoRecenter( void )
	{
		XrSpaceLocation loc = { XR_TYPE_SPACE_LOCATION };
		if( XR_FAILED( xrLocateSpace( ViewSpace, BaseSpace, DisplayTime, &loc ) ) )
			return;
		const XrSpaceLocationFlags needed = XR_SPACE_LOCATION_ORIENTATION_VALID_BIT | XR_SPACE_LOCATION_POSITION_VALID_BIT;
		if( (loc.locationFlags & needed) != needed )
			return;
		// Head position and yaw only, so the screen stays upright.
		XrVector3f fwd = Rotate( loc.pose.orientation, 0.f, 0.f, -1.f );
		float yaw = atan2f( -fwd.x, -fwd.z );
		XrReferenceSpaceCreateInfo space = { XR_TYPE_REFERENCE_SPACE_CREATE_INFO };
		space.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
		space.poseInReferenceSpace.orientation.y = sinf( yaw / 2.f );
		space.poseInReferenceSpace.orientation.w = cosf( yaw / 2.f );
		space.poseInReferenceSpace.position = loc.pose.position;
		XrSpace new_space = XR_NULL_HANDLE;
		if( XR_SUCCEEDED( xrCreateReferenceSpace( Session, &space, &new_space ) ) )
		{
			xrDestroySpace( AppSpace );
			AppSpace = new_space;
			RecenterPending = false;
			LOGI( "recentered" );
		}
	}

	void PollEvents( void )
	{
		XrEventDataBuffer event = { XR_TYPE_EVENT_DATA_BUFFER };
		while( xrPollEvent( Instance, &event ) == XR_SUCCESS )
		{
			if( event.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED )
			{
				State = ((const XrEventDataSessionStateChanged *) &event)->state;
				LOGI( "session state %d", (int) State );
				if( State == XR_SESSION_STATE_READY )
				{
					XrSessionBeginInfo begin = { XR_TYPE_SESSION_BEGIN_INFO };
					begin.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
					if( XR_SUCCEEDED( xrBeginSession( Session, &begin ) ) )
						Running = true;
					RecenterPending = true;
				}
				else if( State == XR_SESSION_STATE_STOPPING )
				{
					Running = false;
					xrEndSession( Session );
				}
				else if( State == XR_SESSION_STATE_EXITING || State == XR_SESSION_STATE_LOSS_PENDING )
				{
					Running = false;
					SDL_Event quit;
					quit.type = SDL_QUIT;
					SDL_PushEvent( &quit );
				}
			}
			else if( event.type == XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING )
			{
				// System recenter moved LOCAL: drop our own offset so they don't stack.
				XrReferenceSpaceCreateInfo space = { XR_TYPE_REFERENCE_SPACE_CREATE_INFO };
				space.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
				space.poseInReferenceSpace.orientation.w = 1.f;
				XrSpace new_space = XR_NULL_HANDLE;
				if( XR_SUCCEEDED( xrCreateReferenceSpace( Session, &space, &new_space ) ) )
				{
					xrDestroySpace( AppSpace );
					AppSpace = new_space;
				}
			}
			event = { XR_TYPE_EVENT_DATA_BUFFER };
		}
	}

	float GetFloat( XrAction action, int h )
	{
		XrActionStateGetInfo info = { XR_TYPE_ACTION_STATE_GET_INFO };
		info.action = action;
		info.subactionPath = HandPaths[ h ];
		XrActionStateFloat state = { XR_TYPE_ACTION_STATE_FLOAT };
		if( XR_SUCCEEDED( xrGetActionStateFloat( Session, &info, &state ) ) && state.isActive )
			return state.currentState;
		return 0.f;
	}

	bool GetBool( XrAction action, int h )
	{
		XrActionStateGetInfo info = { XR_TYPE_ACTION_STATE_GET_INFO };
		info.action = action;
		info.subactionPath = HandPaths[ h ];
		XrActionStateBoolean state = { XR_TYPE_ACTION_STATE_BOOLEAN };
		if( XR_SUCCEEDED( xrGetActionStateBoolean( Session, &info, &state ) ) && state.isActive )
			return state.currentState;
		return false;
	}

	void SyncInput( void )
	{
		memset( Hands, 0, sizeof(Hands) );
		if( State != XR_SESSION_STATE_FOCUSED )
			return;
		XrActiveActionSet active = { ActionSet, XR_NULL_PATH };
		XrActionsSyncInfo sync = { XR_TYPE_ACTIONS_SYNC_INFO };
		sync.countActiveActionSets = 1;
		sync.activeActionSets = &active;
		if( XR_FAILED( xrSyncActions( Session, &sync ) ) )
			return;
		for( int h = 0; h < 2; h ++ )
		{
			Hand *hand = &Hands[ h ];
			hand->Trigger = GetFloat( ActTrigger, h );
			hand->Grip = GetFloat( ActGrip, h );
			XrActionStateGetInfo info = { XR_TYPE_ACTION_STATE_GET_INFO };
			info.action = ActStick;
			info.subactionPath = HandPaths[ h ];
			XrActionStateVector2f stick = { XR_TYPE_ACTION_STATE_VECTOR2F };
			if( XR_SUCCEEDED( xrGetActionStateVector2f( Session, &info, &stick ) ) && stick.isActive )
			{
				hand->StickX = stick.currentState.x;
				hand->StickY = stick.currentState.y;
			}
			hand->StickClick = GetBool( ActStickClick, h );
			hand->Lower = GetBool( ActLower, h );
			hand->Upper = GetBool( ActUpper, h );
			hand->Menu = (h == 0) && GetBool( ActMenu, h );
		}
	}


	// --- Stage 1 controls: menus by key presses ---

	void SendKey( SDL_Keycode key, bool down )
	{
		SDL_Event e;
		memset( &e, 0, sizeof(e) );
		e.type = down ? SDL_KEYDOWN : SDL_KEYUP;
		e.key.state = down ? SDL_PRESSED : SDL_RELEASED;
		e.key.keysym.sym = key;
		e.key.keysym.scancode = SDL_GetScancodeFromKey( key );
		e.key.timestamp = SDL_GetTicks();
		SDL_PushEvent( &e );
	}

	// One physical input -> one key, with keyboard-style auto-repeat while held.
	struct KeyButton
	{
		SDL_Keycode Key;
		bool Down;
		Uint32 Next;
		void Update( bool pressed, bool repeat )
		{
			Uint32 now = SDL_GetTicks();
			if( pressed && ! Down )
			{
				SendKey( Key, true );
				Next = now + 400;
			}
			else if( pressed && repeat && (int)(now - Next) >= 0 )
			{
				SendKey( Key, true );
				Next = now + 110;
			}
			else if( ! pressed && Down )
				SendKey( Key, false );
			Down = pressed;
		}
	};

	KeyButton KeyEnter = { SDLK_RETURN }, KeyEscape = { SDLK_ESCAPE };
	KeyButton KeyUp = { SDLK_UP }, KeyDown = { SDLK_DOWN }, KeyLeft = { SDLK_LEFT }, KeyRight = { SDLK_RIGHT };
	Uint32 RecenterHeld = 0;
	SDL_Joystick *Touch = NULL;
	bool WasFlying = false;

	bool CreateTouchJoystick( void )
	{
		SDL_VirtualJoystickDesc desc;
		memset( &desc, 0, sizeof(desc) );
		desc.version = SDL_VIRTUAL_JOYSTICK_DESC_VERSION;
		desc.type = SDL_JOYSTICK_TYPE_GAMECONTROLLER;
		desc.naxes = FX_TOUCH_AXES;
		desc.nbuttons = FX_TOUCH_BUTTONS;
		desc.vendor_id = 0x2833;   // Oculus
		desc.product_id = 0xF5A7;  // private id for this virtual device
		desc.name = FX_TOUCH_JOYSTICK_NAME;
		int index = SDL_JoystickAttachVirtualEx( &desc );
		if( index < 0 || ! (Touch = SDL_JoystickOpen( index )) )
		{
			LOGE( "virtual joystick: %s", SDL_GetError() );
			return false;
		}
		LOGI( "virtual joystick '%s' at index %d", FX_TOUCH_JOYSTICK_NAME, index );
		return true;
	}

	Sint16 Axis( float v )
	{
		v = v < -1.f ? -1.f : (v > 1.f ? 1.f : v);
		return (Sint16)( v * 32767.f );
	}

	void UpdateTouchJoystick( bool flying )
	{
		if( ! Touch )
			return;
		const Hand &l = Hands[ 0 ], &r = Hands[ 1 ];
		bool recentering = l.StickClick && r.StickClick;
		// Outside flight the pad rests, so menu keys don't double up with joystick input.
		bool on = flying;
		SDL_JoystickSetVirtualAxis( Touch, FX_TOUCH_AXIS_LEFT_X,  on ? Axis( l.StickX ) : 0 );
		SDL_JoystickSetVirtualAxis( Touch, FX_TOUCH_AXIS_LEFT_Y,  on ? Axis( -l.StickY ) : 0 );
		SDL_JoystickSetVirtualAxis( Touch, FX_TOUCH_AXIS_RIGHT_X, on ? Axis( r.StickX ) : 0 );
		SDL_JoystickSetVirtualAxis( Touch, FX_TOUCH_AXIS_RIGHT_Y, on ? Axis( r.StickY ) : 0 );
		bool buttons[ FX_TOUCH_BUTTONS ] = {
			r.Trigger > 0.5f, l.Trigger > 0.5f, r.Grip > 0.5f, l.Grip > 0.5f,
			r.Lower, r.Upper, l.Lower, l.Upper,
			l.StickClick && ! recentering, r.StickClick && ! recentering };
		for( int b = 0; b < FX_TOUCH_BUTTONS; b ++ )
			SDL_JoystickSetVirtualButton( Touch, b, (on && buttons[ b ]) ? SDL_PRESSED : SDL_RELEASED );
	}

	void UpdateControls( void )
	{
		const Hand &l = Hands[ 0 ], &r = Hands[ 1 ];
		bool flying = fx_game_flying();
		if( flying != WasFlying )
			LOGI( "controls: %s", flying ? "flight (virtual joystick)" : "menus (keys)" );
		WasFlying = flying;
		UpdateTouchJoystick( flying );

		if( flying )
		{
			// Flight: only the Menu button opens the in-game menu (Escape); everything else is the joystick.
			KeyEnter.Update( false, false );
			KeyUp.Update( false, false );
			KeyDown.Update( false, false );
			KeyLeft.Update( false, false );
			KeyRight.Update( false, false );
			KeyEscape.Update( l.Menu, false );
		}
		else
		{
			float sx = fabsf( r.StickX ) > fabsf( l.StickX ) ? r.StickX : l.StickX;
			float sy = fabsf( r.StickY ) > fabsf( l.StickY ) ? r.StickY : l.StickY;
			KeyEnter.Update( r.Lower || l.Lower || r.Trigger > 0.6f, false );
			KeyEscape.Update( r.Upper || l.Upper || l.Menu, false );
			KeyUp.Update( sy > 0.6f, true );
			KeyDown.Update( sy < -0.6f, true );
			KeyLeft.Update( sx < -0.6f, true );
			KeyRight.Update( sx > 0.6f, true );
		}

		// Hold both stick clicks for 1 s: recenter the screen in front of you.
		if( l.StickClick && r.StickClick )
		{
			if( ! RecenterHeld )
				RecenterHeld = SDL_GetTicks();
			else if( RecenterHeld != 0xFFFFFFFF && SDL_GetTicks() - RecenterHeld > 1000 )
			{
				RecenterPending = true;
				RecenterHeld = 0xFFFFFFFF;
			}
		}
		else
			RecenterHeld = 0;
	}


	void ReleaseEyes( void )
	{
		for( int e = 0; e < 2; e ++ )
			if( Eyes[ e ].Acquired )
			{
				XrSwapchainImageReleaseInfo release = { XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
				xrReleaseSwapchainImage( Eyes[ e ].Handle, &release );
				Eyes[ e ].Acquired = false;
			}
	}

	// Copies the game framebuffer into the screen swapchain: straight blit (menus) or as HUD with alpha.
	bool CopyGameImage( bool hud )
	{
		uint32_t index = 0;
		XrSwapchainImageAcquireInfo acquire = { XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
		XrSwapchainImageWaitInfo wait = { XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
		wait.timeout = XR_INFINITE_DURATION;
		if( XR_FAILED( xrAcquireSwapchainImage( Screen, &acquire, &index ) ) || XR_FAILED( xrWaitSwapchainImage( Screen, &wait ) ) )
			return false;

		GLboolean scissor = glIsEnabled( GL_SCISSOR_TEST );
		GLboolean mask[ 4 ];
		glGetBooleanv( GL_COLOR_WRITEMASK, mask );
		glDisable( GL_SCISSOR_TEST );
		glColorMask( GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE );
		if( ! hud )
		{
			glBindFramebuffer( GL_READ_FRAMEBUFFER, GameFBO );
			glBindFramebuffer( GL_DRAW_FRAMEBUFFER, ScreenFBOs[ index ] );
			glBlitFramebuffer( 0, 0, GameW, GameH, 0, 0, GameW, GameH, GL_COLOR_BUFFER_BIT, GL_NEAREST );
		}
		else
		{
			// Shader copy; restore the game's GL state afterwards (it caches program and texture state).
			GLint program, texture, active, viewport[ 4 ];
			glGetIntegerv( GL_CURRENT_PROGRAM, &program );
			glGetIntegerv( GL_ACTIVE_TEXTURE, &active );
			glActiveTexture( GL_TEXTURE0 );
			glGetIntegerv( GL_TEXTURE_BINDING_2D, &texture );
			glGetIntegerv( GL_VIEWPORT, viewport );
			GLboolean blend = glIsEnabled( GL_BLEND ), depth = glIsEnabled( GL_DEPTH_TEST ), cull = glIsEnabled( GL_CULL_FACE );
			glDisable( GL_BLEND );
			glDisable( GL_DEPTH_TEST );
			glDisable( GL_CULL_FACE );
			glBindFramebuffer( GL_FRAMEBUFFER, ScreenFBOs[ index ] );
			glViewport( 0, 0, GameW, GameH );
			glUseProgram( HudProgram );
			glUniform1i( glGetUniformLocation( HudProgram, "tex" ), 0 );
			glUniform1f( glGetUniformLocation( HudProgram, "key_black" ), HudKeyBlack ? 1.f : 0.f );
			glBindTexture( GL_TEXTURE_2D, GameColor );
			glDrawArrays( GL_TRIANGLES, 0, 3 );
			glBindTexture( GL_TEXTURE_2D, texture );
			glActiveTexture( active );
			glUseProgram( program );
			glViewport( viewport[ 0 ], viewport[ 1 ], viewport[ 2 ], viewport[ 3 ] );
			if( blend ) glEnable( GL_BLEND );
			if( depth ) glEnable( GL_DEPTH_TEST );
			if( cull ) glEnable( GL_CULL_FACE );
		}
		glColorMask( mask[ 0 ], mask[ 1 ], mask[ 2 ], mask[ 3 ] );
		if( scissor )
			glEnable( GL_SCISSOR_TEST );
		XrSwapchainImageReleaseInfo release = { XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
		xrReleaseSwapchainImage( Screen, &release );
		return true;
	}

	// Ends the open headset frame with what the game drew: stereo eyes + HUD panel, or the flat screen.
	void EndFrame( void )
	{
		XrCompositionLayerProjectionView proj_views[ 2 ];
		XrCompositionLayerProjection proj = { XR_TYPE_COMPOSITION_LAYER_PROJECTION };
		XrCompositionLayerQuad quad = { XR_TYPE_COMPOSITION_LAYER_QUAD };
		const XrCompositionLayerBaseHeader *layers[ 2 ];
		uint32_t layer_count = 0;
		bool stereo = HudMode && Eyes[ 0 ].Acquired && Eyes[ 1 ].Acquired;

		if( ShouldRender )
		{
			if( stereo )
			{
				for( int e = 0; e < 2; e ++ )
				{
					proj_views[ e ] = { XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW };
					proj_views[ e ].pose = Views[ e ].pose;
					proj_views[ e ].fov = Views[ e ].fov;
					proj_views[ e ].subImage.swapchain = Eyes[ e ].Handle;
					proj_views[ e ].subImage.imageRect.extent.width = Eyes[ e ].W;
					proj_views[ e ].subImage.imageRect.extent.height = Eyes[ e ].H;
				}
				proj.space = AppSpace;
				proj.viewCount = 2;
				proj.views = proj_views;
				layers[ layer_count ++ ] = (const XrCompositionLayerBaseHeader *) &proj;
			}
			ReleaseEyes();
			if( CopyGameImage( stereo ) )
			{
				quad.space = AppSpace;
				quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
				quad.subImage.swapchain = Screen;
				quad.subImage.imageRect.extent.width = GameW;
				quad.subImage.imageRect.extent.height = GameH;
				quad.pose.orientation.w = 1.f;
				if( stereo )
				{
					// HUD laid out for the game's field of view, straight ahead of the ship.
					quad.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
					quad.pose.position.z = - HudDist;
					quad.size.width = HudScale * 2.f * HudDist * tanf( HudFov * (float) M_PI / 360.f );
				}
				else
				{
					quad.pose.position.z = - ScreenDist;
					quad.size.width = ScreenWidth;
				}
				quad.size.height = quad.size.width * GameH / (float) GameW;
				layers[ layer_count ++ ] = (const XrCompositionLayerBaseHeader *) &quad;
			}
		}
		ReleaseEyes();

		XrFrameEndInfo end = { XR_TYPE_FRAME_END_INFO };
		end.displayTime = DisplayTime;
		end.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
		end.layerCount = layer_count;
		end.layers = layer_count ? layers : NULL;
		xrEndFrame( Session, &end );
		FrameOpen = false;
		HudMode = false;
	}

	// Waits for the next headset frame and begins it; reads head and controller poses for it.
	void BeginFrame( void )
	{
		XrFrameWaitInfo wait_info = { XR_TYPE_FRAME_WAIT_INFO };
		XrFrameState frame_state = { XR_TYPE_FRAME_STATE };
		if( XR_FAILED( xrWaitFrame( Session, &wait_info, &frame_state ) ) )
			return;
		DisplayTime = frame_state.predictedDisplayTime;
		XrFrameBeginInfo begin_info = { XR_TYPE_FRAME_BEGIN_INFO };
		if( XR_FAILED( xrBeginFrame( Session, &begin_info ) ) )
			return;
		FrameOpen = true;
		ShouldRender = frame_state.shouldRender;
		if( RecenterPending )
			DoRecenter();
		SyncInput();

		XrViewLocateInfo locate = { XR_TYPE_VIEW_LOCATE_INFO };
		locate.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
		locate.displayTime = DisplayTime;
		locate.space = AppSpace;
		XrViewState view_state = { XR_TYPE_VIEW_STATE };
		uint32_t view_count = 2;
		Views[ 0 ] = { XR_TYPE_VIEW };
		Views[ 1 ] = { XR_TYPE_VIEW };
		ViewsValid = XR_SUCCEEDED( xrLocateViews( Session, &locate, &view_state, 2, &view_count, Views ) )
			&& (view_state.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT)
			&& (view_state.viewStateFlags & XR_VIEW_STATE_POSITION_VALID_BIT);
	}
}


extern "C" bool fx_vr_init( int *width, int *height )
{
	if( ! (InitInstance() && InitActions() && InitSession() && InitScreen() && InitEyes()) )
		return false;
	InitHudProgram();
	CreateTouchJoystick();
	*width = GameW;
	*height = GameH;
	LOGI( "OpenXR ready, %.0f Hz, game %dx%d", DisplayRefresh, GameW, GameH );
	return true;
}


extern "C" void fx_vr_present( void )
{
	PollEvents();
	if( FrameOpen )
		EndFrame();
	if( Running )
	{
		BeginFrame();  // xrWaitFrame paces the game to the headset
		UpdateControls();
	}
	else
		SDL_Delay( 10 );
	glBindFramebuffer( GL_FRAMEBUFFER, GameFBO );
}


extern "C" bool fx_vr_stereo_ready( void )
{
	return FrameOpen && ShouldRender && ViewsValid;
}


extern "C" bool fx_vr_begin_eye( int e, fx_eye_t *out )
{
	EyeTarget *eye = &Eyes[ e ? 1 : 0 ];
	if( ! fx_vr_stereo_ready() )
		return false;
	uint32_t index = 0;
	if( ! eye->Acquired )
	{
		XrSwapchainImageAcquireInfo acquire = { XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
		XrSwapchainImageWaitInfo wait = { XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
		wait.timeout = XR_INFINITE_DURATION;
		if( XR_FAILED( xrAcquireSwapchainImage( eye->Handle, &acquire, &index ) ) || XR_FAILED( xrWaitSwapchainImage( eye->Handle, &wait ) ) )
			return false;
		eye->Acquired = true;
	}
	glBindFramebuffer( GL_FRAMEBUFFER, eye->FBOs[ index ] );

	// Eye pose relative to the recentred head origin, converted to the game's camera axes
	// (x right, y up, z forward; OpenXR looks down -z): R_game = C R C, p_game = C p, C = diag(1,1,-1).
	const XrPosef &pose = Views[ e ? 1 : 0 ].pose;
	XrQuaternionf q = pose.orientation;
	float r[ 3 ][ 3 ] = {
		{ 1 - 2*(q.y*q.y + q.z*q.z), 2*(q.x*q.y - q.z*q.w),     2*(q.x*q.z + q.y*q.w) },
		{ 2*(q.x*q.y + q.z*q.w),     1 - 2*(q.x*q.x + q.z*q.z), 2*(q.y*q.z - q.x*q.w) },
		{ 2*(q.x*q.z - q.y*q.w),     2*(q.y*q.z + q.x*q.w),     1 - 2*(q.x*q.x + q.y*q.y) } };
	const float c[ 3 ] = { 1.f, 1.f, -1.f };
	for( int i = 0; i < 3; i ++ )
		for( int j = 0; j < 3; j ++ )
			out->rot[ i ][ j ] = c[ i ] * c[ j ] * r[ i ][ j ];
	out->pos[ 0 ] = pose.position.x;
	out->pos[ 1 ] = pose.position.y;
	out->pos[ 2 ] = - pose.position.z;
	const XrFovf &fov = Views[ e ? 1 : 0 ].fov;
	out->tan_left = tanf( fov.angleLeft );
	out->tan_right = tanf( fov.angleRight );
	out->tan_down = tanf( fov.angleDown );
	out->tan_up = tanf( fov.angleUp );
	out->width = eye->W;
	out->height = eye->H;
	return true;
}


extern "C" void fx_vr_eye_tint( float r, float g, float b, float strength )
{
	// Full-viewport pass over the bound eye; restore the game's cached GL state afterwards.
	GLint program, viewport[ 4 ], src, dst;
	glGetIntegerv( GL_CURRENT_PROGRAM, &program );
	glGetIntegerv( GL_VIEWPORT, viewport );
	glGetIntegerv( GL_BLEND_SRC_RGB, &src );
	glGetIntegerv( GL_BLEND_DST_RGB, &dst );
	GLboolean blend = glIsEnabled( GL_BLEND ), depth = glIsEnabled( GL_DEPTH_TEST ), cull = glIsEnabled( GL_CULL_FACE ), scissor = glIsEnabled( GL_SCISSOR_TEST );
	GLboolean depth_mask;
	glGetBooleanv( GL_DEPTH_WRITEMASK, &depth_mask );
	glDisable( GL_DEPTH_TEST );
	glDisable( GL_CULL_FACE );
	glDisable( GL_SCISSOR_TEST );
	glDepthMask( GL_FALSE );
	glEnable( GL_BLEND );
	glBlendFunc( GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA );
	const EyeTarget &eye = Eyes[ 0 ];
	glViewport( 0, 0, eye.W, eye.H );
	glUseProgram( TintProgram );
	glUniform4f( glGetUniformLocation( TintProgram, "tint" ), r, g, b, fminf( 1.f, strength * 1.6f ) );
	glDrawArrays( GL_TRIANGLES, 0, 3 );
	glUseProgram( program );
	glViewport( viewport[ 0 ], viewport[ 1 ], viewport[ 2 ], viewport[ 3 ] );
	glBlendFunc( src, dst );
	if( ! blend ) glDisable( GL_BLEND );
	if( depth ) glEnable( GL_DEPTH_TEST );
	if( cull ) glEnable( GL_CULL_FACE );
	if( scissor ) glEnable( GL_SCISSOR_TEST );
	glDepthMask( depth_mask );
}


extern "C" void fx_vr_end_eyes( float hud_fov, float hud_scale )
{
	HudMode = Eyes[ 0 ].Acquired && Eyes[ 1 ].Acquired;
	HudFov = hud_fov;
	HudScale = hud_scale;
	HudKeyBlack = hud_scale >= 1.f;
	// The HUD and in-game menus are drawn next into the game framebuffer, on a transparent background.
	glBindFramebuffer( GL_FRAMEBUFFER, GameFBO );
	GLboolean mask[ 4 ];
	glGetBooleanv( GL_COLOR_WRITEMASK, mask );
	GLboolean scissor = glIsEnabled( GL_SCISSOR_TEST );
	glDisable( GL_SCISSOR_TEST );
	glColorMask( GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE );
	glClearColor( 0.f, 0.f, 0.f, 0.f );
	glClear( GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT );
	glColorMask( mask[ 0 ], mask[ 1 ], mask[ 2 ], mask[ 3 ] );
	if( scissor )
		glEnable( GL_SCISSOR_TEST );
}
