cmake_minimum_required(VERSION 3.16)
project(RTSP-Player LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Architecture detection
if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    set(ARCH x64)
else()
    set(ARCH x86)
endif()
message(STATUS "Target architecture: ${ARCH}")

set(SDL2_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/3rd/SDL2")
set(SDL2_INCLUDE_DIRS ${SDL2_ROOT}/include)
set(SDL2_LIB_DIR ${SDL2_ROOT}/lib/${ARCH})

find_library(SDL2_LIBRARY SDL2 PATHS ${SDL2_LIB_DIR} REQUIRED NO_DEFAULT_PATH)

set(FFMPEG_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/3rd/FFmpeg")
set(FFMPEG_INCLUDE_DIRS ${FFMPEG_ROOT}/${ARCH}/include)
set(FFMPEG_LIBRARY_DIRS ${FFMPEG_ROOT}/${ARCH}/lib)

find_library(AVFORMAT_LIBRARY  avformat  PATHS ${FFMPEG_LIBRARY_DIRS} REQUIRED NO_DEFAULT_PATH)
find_library(AVCODEC_LIBRARY   avcodec   PATHS ${FFMPEG_LIBRARY_DIRS} REQUIRED NO_DEFAULT_PATH)
find_library(AVUTIL_LIBRARY    avutil    PATHS ${FFMPEG_LIBRARY_DIRS} REQUIRED NO_DEFAULT_PATH)
find_library(SWRESAMPLE_LIBRARY swresample PATHS ${FFMPEG_LIBRARY_DIRS} REQUIRED NO_DEFAULT_PATH)
find_library(SWSCALE_LIBRARY swscale PATHS ${FFMPEG_LIBRARY_DIRS} REQUIRED NO_DEFAULT_PATH)

set(SOURCES
    src/main.cpp
    src/RTSPlayer.cpp
    src/StreamLifecycleManager.cpp
    src/PlayerStateMachine.cpp
    src/DemuxThread.cpp
    src/VideoDecodeThread.cpp
    src/AudioWorker.cpp
    src/SDLRenderer.cpp
    src/SDLAudio.cpp
    src/AudioRingBuffer.cpp
    src/PacketQueue.cpp
    src/VideoFrameQueue.cpp
    src/AVClock.cpp
    src/PlayerStats.cpp
    src/logger/Logger.cpp
)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/../bin/MSVC2017_${ARCH}_$<CONFIG>")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/../bin/MSVC2017_${ARCH}_$<CONFIG>")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/../bin/MSVC2017_${ARCH}_$<CONFIG>")

add_executable(${PROJECT_NAME} ${SOURCES})
target_compile_options(${PROJECT_NAME} PRIVATE /utf-8)

# /SAFESEH:NO is x86-only
if(ARCH STREQUAL "x86")
    target_link_options(${PROJECT_NAME} PRIVATE /SAFESEH:NO)
endif()

target_include_directories(${PROJECT_NAME} PRIVATE
    src
    ${FFMPEG_INCLUDE_DIRS}
    ${SDL2_INCLUDE_DIRS}
)

target_link_libraries(${PROJECT_NAME} PRIVATE
    ${SDL2_LIBRARY}
    ${AVFORMAT_LIBRARY}
    ${AVCODEC_LIBRARY}
    ${AVUTIL_LIBRARY}
    ${SWRESAMPLE_LIBRARY}
    ${SWSCALE_LIBRARY}
    winmm
)

# DLL filenames per architecture
if(ARCH STREQUAL "x64")
    set(AVCODEC_DLL     avcodec-61.dll)
    set(AVFORMAT_DLL    avformat-61.dll)
    set(AVUTIL_DLL      avutil-59.dll)
    set(SWRESAMPLE_DLL  swresample-5.dll)
    set(SWSCALE_DLL     swscale-8.dll)
else()
    set(AVCODEC_DLL     avcodec-58.dll)
    set(AVFORMAT_DLL    avformat-58.dll)
    set(AVUTIL_DLL      avutil-56.dll)
    set(SWRESAMPLE_DLL  swresample-3.dll)
    set(SWSCALE_DLL     swscale-5.dll)
endif()

add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "${SDL2_LIB_DIR}/SDL2.dll"
    "${FFMPEG_ROOT}/${ARCH}/bin/${AVCODEC_DLL}"
    "${FFMPEG_ROOT}/${ARCH}/bin/${AVFORMAT_DLL}"
    "${FFMPEG_ROOT}/${ARCH}/bin/${AVUTIL_DLL}"
    "${FFMPEG_ROOT}/${ARCH}/bin/${SWRESAMPLE_DLL}"
    "${FFMPEG_ROOT}/${ARCH}/bin/${SWSCALE_DLL}"
    "$<TARGET_FILE_DIR:${PROJECT_NAME}>"
)
