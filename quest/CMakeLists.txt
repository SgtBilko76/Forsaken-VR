# Forsaken VR for Meta Quest: builds libmain.so (game + OpenXR host) and libSDL2.so.
# Used by quest/app/build.gradle; the desktop build stays on the root Makefile.
#
# Dependencies:
#   SDL2 2.32.10            git submodule quest/external/SDL
#   Lua 5.1.4, luasocket 2.0.2, ENet 1.2.5   vendored in quest/thirdparty (versions the game was written for)
#   libpng, zlib, openal-soft                 vcpkg arm64-android (quest/vcpkg/vcpkg.json, tools/quest/build_deps.sh)
cmake_minimum_required(VERSION 3.22)
project(ForsakenQuest C CXX)

set(ROOT ${CMAKE_CURRENT_SOURCE_DIR}/..)
set(TP ${CMAKE_CURRENT_SOURCE_DIR}/thirdparty)
set(FX_DEPS ${ROOT}/build-android-deps/arm64-android CACHE PATH "vcpkg arm64-android install dir")
if(NOT EXISTS ${FX_DEPS}/lib/libopenal.a)
	message(FATAL_ERROR "Missing ${FX_DEPS}: run tools/quest/build_deps.sh first")
endif()

# Release-level optimisation even for gradle's debug variant (it passes CMAKE_BUILD_TYPE=Debug).
set(CMAKE_C_FLAGS_DEBUG "-O2 -g")
set(CMAKE_CXX_FLAGS_DEBUG "-O2 -g")

# --- SDL2 (shared: SDLActivity loads libSDL2.so + libmain.so) ---
set(SDL_SHARED ON CACHE BOOL "" FORCE)
set(SDL_STATIC OFF CACHE BOOL "" FORCE)
set(SDL_TEST OFF CACHE BOOL "" FORCE)
set(SDL2_DISABLE_INSTALL ON CACHE BOOL "" FORCE)
set(SDL2_DISABLE_UNINSTALL ON CACHE BOOL "" FORCE)
add_subdirectory(external/SDL EXCLUDE_FROM_ALL)

# --- Lua 5.1.4 ---
file(GLOB LUA_SRC ${TP}/lua-5.1.4/src/*.c)
list(REMOVE_ITEM LUA_SRC ${TP}/lua-5.1.4/src/lua.c ${TP}/lua-5.1.4/src/luac.c ${TP}/lua-5.1.4/src/print.c)
add_library(lua51 STATIC ${LUA_SRC})
target_compile_definitions(lua51 PUBLIC LUA_USE_POSIX LUA_USE_DLOPEN)
# Lua 5.1 strings live behind a header struct (svalue = ts+1): FORTIFY's size checks abort on them.
target_compile_options(lua51 PRIVATE -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=0)
target_include_directories(lua51 PUBLIC ${TP}/lua-5.1.4/src)

# --- luasocket 2.0.2 (socket.core + mime.core, registered by lua_common.c) ---
set(LS ${TP}/luasocket-2.0.2/src)
add_library(luasocket STATIC ${LS}/luasocket.c ${LS}/timeout.c ${LS}/buffer.c ${LS}/io.c ${LS}/auxiliar.c
	${LS}/options.c ${LS}/inet.c ${LS}/tcp.c ${LS}/udp.c ${LS}/except.c ${LS}/select.c ${LS}/usocket.c ${LS}/mime.c)
target_include_directories(luasocket PUBLIC ${LS})
target_link_libraries(luasocket PUBLIC lua51)

# --- ENet 1.2.5 ---
set(EN ${TP}/enet-1.2.5)
add_library(enet STATIC ${EN}/callbacks.c ${EN}/host.c ${EN}/list.c ${EN}/packet.c ${EN}/peer.c ${EN}/protocol.c ${EN}/unix.c)
target_include_directories(enet PUBLIC ${EN}/include)
target_compile_definitions(enet PRIVATE HAS_SOCKLEN_T HAS_POLL HAS_FCNTL HAS_MSGHDR_FLAGS HAS_INET_PTON HAS_INET_NTOP HAS_GETADDRINFO HAS_GETNAMEINFO)

# --- game ---
file(GLOB GAME_SRC ${ROOT}/*.c)
add_library(main SHARED ${GAME_SRC} src/fx_android.c src/fx_game.c src/fx_openxr.cpp)
target_include_directories(main PRIVATE ${ROOT} src src/compat ${FX_DEPS}/include ${FX_DEPS}/include/AL)
target_compile_definitions(main PRIVATE
	GL=3 GLES=1 FX_OPENXR=1
	NET_ENET_2 BSP LUA_USE_APICHECK TEXTURE_PNG SOUND_SUPPORT SOUND_OPENAL AL_LIBTYPE_STATIC
	# The upstream makefile always builds these in ("non debug build would probably crash").
	DEBUG_ON DEBUG_COMP DEBUG_SPOTFX_SOUND DEBUG_VIEWPORT)
# Old C: same relaxations as the desktop build; x86 char is signed.
target_compile_options(main PRIVATE $<$<COMPILE_LANGUAGE:C>:-std=gnu99 -fcommon -fsigned-char
	-Wno-error=incompatible-pointer-types -Wno-error=implicit-function-declaration -Wno-error=int-conversion
	-Wno-implicit-int -Wno-int-conversion -Wno-incompatible-pointer-types -Wno-implicit-function-declaration
	-Wno-error=format-security -Wno-format-security -Wno-pointer-sign>)
find_package(OpenXR REQUIRED CONFIG)  # prefab: org.khronos.openxr:openxr_loader_for_android
target_link_libraries(main PRIVATE SDL2 lua51 luasocket enet
	${FX_DEPS}/lib/libopenal.a ${FX_DEPS}/lib/libfmt.a ${FX_DEPS}/lib/libpng16.a ${FX_DEPS}/lib/libz.a
	OpenXR::openxr_loader GLESv3 EGL OpenSLES android log m dl atomic)
