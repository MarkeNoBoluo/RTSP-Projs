cmake_minimum_required(VERSION 3.16)
project(RTSP-Player LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ── Dependencies via pkg-config (Linux) or 3rd/ (Windows) ──────────────────
if(WIN32)
    # Windows: use bundled 3rd/ libraries
    if(CMAKE_SIZEOF_VOID_P EQUAL 8)
        set(ARCH x64)
    else()
        set(ARCH x86)
    endif()

    set(SDL2_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/3rd/SDL2")
    set(SDL2_INCLUDE_DIRS ${SDL2_ROOT}/include)
    set(SDL2_LIB_DIR ${SDL2_ROOT}/lib/${ARCH})

    find_library(SDL2_LIBRARY SDL2 PATHS ${SDL2_LIB_DIR} REQUIRED NO_DEFAULT_PATH)

    set(FFMPEG_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/3rd/FFmpeg/${ARCH}")
    set(FFMPEG_INCLUDE_DIRS ${FFMPEG_ROOT}/include)
    set(FFMPEG_LIBRARY_DIRS ${FFMPEG_ROOT}/lib)

    find_library(AVFORMAT_LIBRARY  avformat  PATHS ${FFMPEG_LIBRARY_DIRS} REQUIRED NO_DEFAULT_PATH)
    find_library(AVCODEC_LIBRARY   avcodec   PATHS ${FFMPEG_LIBRARY_DIRS} REQUIRED NO_DEFAULT_PATH)
    find_library(AVUTIL_LIBRARY    avutil    PATHS ${FFMPEG_LIBRARY_DIRS} REQUIRED NO_DEFAULT_PATH)
    find_library(SWRESAMPLE_LIBRARY swresample PATHS ${FFMPEG_LIBRARY_DIRS} REQUIRED NO_DEFAULT_PATH)
    find_library(SWSCALE_LIBRARY swscale PATHS ${FFMPEG_LIBRARY_DIRS} REQUIRED NO_DEFAULT_PATH)

    set(FFMPEG_LIBRARIES
        ${AVFORMAT_LIBRARY} ${AVCODEC_LIBRARY} ${AVUTIL_LIBRARY}
        ${SWRESAMPLE_LIBRARY} ${SWSCALE_LIBRARY})
else()
    # Linux: support custom FFmpeg build via FFMPEG_ROOT, or system via pkg-config
    find_package(PkgConfig REQUIRED)
    find_package(Threads REQUIRED)
    pkg_check_modules(SDL2 REQUIRED sdl2)

    set(FFMPEG_ROOT "/home/wddkxg/Libs/ffmpeg-4.4.6" CACHE PATH
        "Custom FFmpeg install prefix (e.g. /path/to/ffmpeg_build)")

    if(FFMPEG_ROOT)
        set(FFMPEG_INCLUDE_DIRS ${FFMPEG_ROOT}/include)
        set(FFMPEG_LIB_DIR ${FFMPEG_ROOT}/lib)
        find_library(AVFORMAT_LIBRARY  avformat  PATHS ${FFMPEG_LIB_DIR} REQUIRED NO_DEFAULT_PATH)
        find_library(AVCODEC_LIBRARY   avcodec   PATHS ${FFMPEG_LIB_DIR} REQUIRED NO_DEFAULT_PATH)
        find_library(AVUTIL_LIBRARY    avutil    PATHS ${FFMPEG_LIB_DIR} REQUIRED NO_DEFAULT_PATH)
        find_library(SWRESAMPLE_LIBRARY swresample PATHS ${FFMPEG_LIB_DIR} REQUIRED NO_DEFAULT_PATH)
        find_library(SWSCALE_LIBRARY   swscale   PATHS ${FFMPEG_LIB_DIR} REQUIRED NO_DEFAULT_PATH)
        set(FFMPEG_LIBRARIES
            ${AVFORMAT_LIBRARY} ${AVCODEC_LIBRARY} ${AVUTIL_LIBRARY}
            ${SWRESAMPLE_LIBRARY} ${SWSCALE_LIBRARY})
    else()
        pkg_check_modules(FFMPEG REQUIRED
            libavcodec libavformat libavutil libswresample libswscale)
    endif()
endif()

# ── Sources ─────────────────────────────────────────────────────────────────
set(SOURCES
    src/main.cpp
    src/RTSPlayer.cpp
    src/StreamLifecycleManager.cpp
    src/PlayerStateMachine.cpp
    src/DemuxThread.cpp
    src/VideoDecodeThread.cpp
    src/AudioWorker.cpp
    src/AudioRingBuffer.cpp
    src/SDLAudio.cpp
    src/SDLRenderer.cpp
    src/PacketQueue.cpp
    src/VideoFrameQueue.cpp
    src/AVClock.cpp
    src/PlayerStats.cpp
    src/logger/Logger.cpp
)

# ── Output directories ──────────────────────────────────────────────────────
if(WIN32)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/../bin/MSVC2017_${ARCH}_$<CONFIG>")
    set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/../bin/MSVC2017_${ARCH}_$<CONFIG>")
    set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/../bin/MSVC2017_${ARCH}_$<CONFIG>")
else()
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/../bin/$<CONFIG>")
endif()

# ── Executable ──────────────────────────────────────────────────────────────
add_executable(${PROJECT_NAME} ${SOURCES})

if(WIN32)
    target_compile_options(${PROJECT_NAME} PRIVATE /utf-8)
    if(ARCH STREQUAL "x86")
        target_link_options(${PROJECT_NAME} PRIVATE /SAFESEH:NO)
    endif()
    target_link_libraries(${PROJECT_NAME} PRIVATE
        ${SDL2_LIBRARY} ${FFMPEG_LIBRARIES} winmm)
else()
    target_compile_options(${PROJECT_NAME} PRIVATE -Wall -Wextra)
    target_include_directories(${PROJECT_NAME} PRIVATE
        ${SDL2_INCLUDE_DIRS} ${FFMPEG_INCLUDE_DIRS})
    target_link_libraries(${PROJECT_NAME} PRIVATE
        ${SDL2_LIBRARIES} ${FFMPEG_LIBRARIES} Threads::Threads)
endif()

target_include_directories(${PROJECT_NAME} PRIVATE
    src
    ${SDL2_INCLUDE_DIRS}
    ${FFMPEG_INCLUDE_DIRS}
)

# ── DLL copy (Windows only) ─────────────────────────────────────────────────
if(WIN32)
    add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${SDL2_LIB_DIR}/SDL2.dll"
        "$<TARGET_FILE_DIR:${PROJECT_NAME}>"
    )

    if(ARCH STREQUAL "x64")
        set(FFMPEG_DLLS
            avcodec-61.dll  avformat-61.dll  avutil-59.dll
            swresample-5.dll swscale-8.dll)
    else()
        set(FFMPEG_DLLS
            avcodec-58.dll  avformat-58.dll  avutil-56.dll
            swresample-3.dll swscale-5.dll)
    endif()

    foreach(dll ${FFMPEG_DLLS})
        add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${FFMPEG_ROOT}/bin/${dll}"
            "$<TARGET_FILE_DIR:${PROJECT_NAME}>"
        )
    endforeach()
endif()
