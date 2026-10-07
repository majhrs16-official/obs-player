cmake_minimum_required(VERSION 3.16)

project(obs-music-playlist)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(PkgConfig REQUIRED)
find_package(Freetype REQUIRED)

if(WIN32)
    set(OBS_INCLUDE_DIRS
        "C:/Program Files/obs-studio/include"
        "C:/Program Files (x86)/obs-studio/include"
    )
    set(OBS_LIBRARIES
        "C:/Program Files/obs-studio/bin/64bit/obs.lib"
        "C:/Program Files (x86)/obs-studio/bin/64bit/obs.lib"
    )
else()
    set(OBS_INCLUDE_DIRS
        "/usr/include/obs"
        "/usr/local/include/obs"
    )
    set(OBS_LIBRARIES "obs")
endif()

find_path(OBS_INCLUDE_DIR
    NAMES obs-module.h
    PATHS ${OBS_INCLUDE_DIRS}
    REQUIRED
)

find_library(OBS_LIBRARY
    NAMES ${OBS_LIBRARIES}
    REQUIRED
)

if(NOT WIN32)
    pkg_check_modules(AVFORMAT REQUIRED IMPORTED_TARGET libavformat)
    pkg_check_modules(AVCODEC REQUIRED IMPORTED_TARGET libavcodec)
    pkg_check_modules(AVUTIL REQUIRED IMPORTED_TARGET libavutil)
    pkg_check_modules(SWSCALE REQUIRED IMPORTED_TARGET libswresample)
else()
    # On Windows, FFmpeg is typically bundled with OBS or installed via vcpkg
    find_package(AVFORMAT REQUIRED)
    find_package(AVCODEC REQUIRED)
    find_package(AVUTIL REQUIRED)
    find_package(SWSCALE REQUIRED)
endif()

add_library(obs-music-playlist MODULE
    src/plugin-main.cpp
    src/playlist-state.cpp
    src/music-playlist-audio.cpp
    src/music-playlist-text.cpp
)

target_include_directories(obs-music-playlist
    PRIVATE
        ${OBS_INCLUDE_DIR}
        ${FREETYPE_INCLUDE_DIRS}
        ${AVFORMAT_INCLUDE_DIRS}
        ${AVCODEC_INCLUDE_DIRS}
        ${AVUTIL_INCLUDE_DIRS}
        ${SWSCALE_INCLUDE_DIRS}
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(obs-music-playlist
    PRIVATE
        ${OBS_LIBRARY}
        Freetype::Freetype
        ${AVFORMAT_LIBRARIES}
        ${AVCODEC_LIBRARIES}
        ${AVUTIL_LIBRARIES}
        ${SWSCALE_LIBRARIES}
)

set_target_properties(obs-music-playlist PROPERTIES
    PREFIX ""
    OUTPUT_NAME "obs-music-playlist"
)

if(WIN32)
    set(OBS_USER_PLUGIN_DIR
        "$ENV{APPDATA}/obs-studio/plugins/obs-music-playlist/bin/64bit"
    )
else()
    set(OBS_USER_PLUGIN_DIR
        "$ENV{HOME}/.config/obs-studio/plugins/obs-music-playlist/bin/64bit"
    )
endif()

install(
    TARGETS obs-music-playlist
    LIBRARY DESTINATION "${OBS_USER_PLUGIN_DIR}"
)
