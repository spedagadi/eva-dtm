# - Try to find ffmpeg libraries (libavcodec, libavformat and libavutil)
# Once done this will define
#
# FFMPEG_FOUND - system has ffmpeg or libav
# FFMPEG_INCLUDE_DIR - the ffmpeg include directory
# FFMPEG_LIBRARIES - Link these to use ffmpeg
# FFMPEG_LIBAVCODEC
# FFMPEG_LIBAVFORMAT
# FFMPEG_LIBAVUTIL
#
# Copyright (c) 2008 Andreas Schneider <mail@cynapses.org>
# Modified for other libraries by Lasse Kärkkäinen <tronic>
# Modified for Hedgewars by Stepik777
#
# Redistribution and use is allowed according to the terms of the New
# BSD license.
#

if (FFMPEG_LIBRARIES AND FFMPEG_INCLUDE_DIR)
  # in cache already
  set(FFMPEG_FOUND TRUE)
else (FFMPEG_LIBRARIES AND FFMPEG_INCLUDE_DIR)
  # use pkg-config to get the directories and then use these values
  # in the FIND_PATH() and FIND_LIBRARY() calls
  #find_package(PkgConfig)
  #if (PKG_CONFIG_FOUND)
  #  pkg_check_modules(_FFMPEG_AVCODEC libavcodec)
  #  pkg_check_modules(_FFMPEG_AVFORMAT libavformat)
  #  pkg_check_modules(_FFMPEG_AVUTIL libavutil)

  # pkg_check_modules(_FFMPEG_AVFILTER libavfilter)
  #  pkg_check_modules(_FFMPEG_AVDEVICE libavdevice)
  #  pkg_check_modules(_FFMPEG_SWSCALE libswscale)
  #  pkg_check_modules(_FFMPEG_SWRESAMPLE libswresample)
  #endif (PKG_CONFIG_FOUND)

  find_path(FFMPEG_AVCODEC_INCLUDE_DIR
          NAMES libavcodec/avcodec.h
          PATHS ${_FFMPEG_AVCODEC_INCLUDE_DIRS} /home/ubuntu/autostats-ucv/3rdparty/ffmpeg-4.1.2/install/include
          PATH_SUFFIXES ffmpeg libav
          )

  find_library(FFMPEG_LIBAVCODEC
          NAMES avcodec
          PATHS ${_FFMPEG_AVCODEC_LIBRARY_DIRS} /home/ubuntu/autostats-ucv/3rdparty/ffmpeg-4.1.2/install/lib
          )

  find_library(FFMPEG_LIBAVFORMAT
          NAMES avformat
          PATHS ${_FFMPEG_AVFORMAT_LIBRARY_DIRS} /home/ubuntu/autostats-ucv/3rdparty/ffmpeg-4.1.2/install/lib
          )

  find_library(FFMPEG_LIBAVUTIL
          NAMES avutil
          PATHS ${_FFMPEG_AVUTIL_LIBRARY_DIRS} /home/ubuntu/autostats-ucv/3rdparty/ffmpeg-4.1.2/install/lib
          )

  find_library(FFMPEG_LIBAVFILTER
          NAMES avfilter
          PATHS ${_FFMPEG_AVFILTER_LIBRARY_DIRS} /home/ubuntu/autostats-ucv/3rdparty/ffmpeg-4.1.2/install/lib
          )

  find_library(FFMPEG_LIBAVDEVICE
          NAMES avdevice
          PATHS ${_FFMPEG_AVDEVICE_LIBRARY_DIRS} /home/ubuntu/autostats-ucv/3rdparty/ffmpeg-4.1.2/install/lib
          )

  find_library(FFMPEG_LIBSWSCALE
          NAMES swscale
          PATHS ${_FFMPEG_SWSCALE_LIBRARY_DIRS} /home/ubuntu/autostats-ucv/3rdparty/ffmpeg-4.1.2/install/lib
          )

  find_library(FFMPEG_LIBSWRESAMPLE
          NAMES swresample
          PATHS ${_FFMPEG_SWRESAMPLE_LIBRARY_DIRS} /home/ubuntu/autostats-ucv/3rdparty/ffmpeg-4.1.2/install/lib
          )

  if (FFMPEG_LIBAVCODEC AND FFMPEG_LIBAVFORMAT)
    set(FFMPEG_FOUND TRUE)
  endif()

  if (FFMPEG_FOUND)
    set(FFMPEG_INCLUDE_DIR ${FFMPEG_AVCODEC_INCLUDE_DIR})

    set(FFMPEG_LIBRARIES
            ${FFMPEG_LIBAVDEVICE}
            ${FFMPEG_LIBAVFORMAT}
            ${FFMPEG_LIBAVFILTER}
            ${FFMPEG_LIBAVCODEC}
            ${FFMPEG_LIBAVUTIL}
            ${FFMPEG_LIBSWSCALE}
            ${FFMPEG_LIBSWRESAMPLE}
            )

  endif (FFMPEG_FOUND)

  if (FFMPEG_FOUND)
    if (NOT FFMPEG_FIND_QUIETLY)
      message(STATUS "Found FFMPEG or Libav: ${FFMPEG_LIBRARIES}, ${FFMPEG_INCLUDE_DIR}")
    endif (NOT FFMPEG_FIND_QUIETLY)
  else (FFMPEG_FOUND)
    if (FFMPEG_FIND_REQUIRED)
      message(FATAL_ERROR "Could not find libavcodec or libavformat or libavutil")
    endif (FFMPEG_FIND_REQUIRED)
  endif (FFMPEG_FOUND)

endif (FFMPEG_LIBRARIES AND FFMPEG_INCLUDE_DIR)