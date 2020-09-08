cmake_minimum_required(VERSION 2.8)

# VitaSDK defines
if( NOT DEFINED CMAKE_TOOLCHAIN_FILE )
  if( DEFINED ENV{VITASDK} )
    set(CMAKE_TOOLCHAIN_FILE "$ENV{VITASDK}/share/vita.toolchain.cmake" CACHE PATH "toolchain file")
  else()
    message(FATAL_ERROR "Please define VITASDK to point to your SDK path!")
  endif()
endif()

# Project start
set(VITA_APPNAME HOTA)
set(VITA_TITLEID "HOTA00001")

project(${VITA_APPNAME})
include("${VITASDK}/share/vita.cmake" REQUIRED)

# Flags and includes
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fno-exceptions")

include_directories(
  ${VITASDK}/arm-vita-eabi/include/SDL2
)

link_directories(
  ${CMAKE_CURRENT_BINARY_DIR}
)

# Builds
add_executable(${VITA_APPNAME}
  src/animation.c
  src/cd_iso.c
  src/client.c
  src/common.c
  src/debug.c
  src/decode.c
  src/game2bin.c
  src/getopt1.c
  src/getopt.c
  src/lzss.c
  src/main.c
  src/music.c
  src/render.c
  src/rooms.c
  src/scale2x.c
  src/scale3x.c
  src/scale800x480.c
  src/screen.c
  src/sound.c
  src/sprites.c
  src/vm.c
)

target_link_libraries(${VITA_APPNAME}
  SDL2_mixer
  SDL2
  vita2d
  vorbisfile
  vorbis
  ogg
  mpg123
  mikmod
  modplug
  FLAC
  stdc++
  m
  SceAppMgr_stub
  SceAudio_stub
  SceCommonDialog_stub
  SceCtrl_stub
  SceDisplay_stub
  SceGxm_stub
  SceHid_stub
  SceSysmodule_stub
  SceTouch_stub
)

# Create Vita artifacts
vita_create_self(eboot.bin ${VITA_APPNAME} UNSAFE)
vita_create_vpk(${VITA_APPNAME}.vpk ${VITA_TITLEID} eboot.bin
  NAME ${VITA_APP_NAME}
  FILE res/icon0.png sce_sys/icon0.png
  FILE res/bg.png sce_sys/livearea/contents/bg.png
  FILE res/startup.png sce_sys/livearea/contents/startup.png
  FILE res/template.xml sce_sys/livearea/contents/template.xml
)
