cmake_minimum_required(VERSION 3.16)
project(RTSP-Pusher LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ── Dependencies via pkg-config (Linux) or 3rd/ (Windows) ──────────────────
if(WIN32)
    # Windows: use bundled 3rd/ libraries
    if(CMAKE_SIZEOF_VOID_P EQUAL 8)
        set(TARGET_ARCH x64)
        set(BUILD_FLAVOR MSVC2017_x64)
    else()
        set(TARGET_ARCH x86)
        set(BUILD_FLAVOR MSVC2017_x86)
    endif()

    set(SDL2_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/3rd/SDL2")
    set(SDL2_INCLUDE_DIRS ${SDL2_ROOT}/include)
    set(SDL2_LIB_DIR ${SDL2_ROOT}/lib/${TARGET_ARCH})

    find_library(SDL2_LIBRARY SDL2 PATHS ${SDL2_LIB_DIR} REQUIRED NO_DEFAULT_PATH)

    set(FFMPEG_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/3rd/FFmpeg/${TARGET_ARCH}")
    set(FFMPEG_INCLUDE_DIRS ${FFMPEG_ROOT}/include)
    set(FFMPEG_LIBRARY_DIRS ${FFMPEG_ROOT}/lib)

    find_library(AVFORMAT_LIBRARY  avformat  PATHS ${FFMPEG_LIBRARY_DIRS} REQUIRED NO_DEFAULT_PATH)
    find_library(AVCODEC_LIBRARY   avcodec   PATHS ${FFMPEG_LIBRARY_DIRS} REQUIRED NO_DEFAULT_PATH)
    find_library(AVUTIL_LIBRARY    avutil    PATHS ${FFMPEG_LIBRARY_DIRS} REQUIRED NO_DEFAULT_PATH)
    find_library(AVDEVICE_LIBRARY  avdevice  PATHS ${FFMPEG_LIBRARY_DIRS} REQUIRED NO_DEFAULT_PATH)
    find_library(AVFILTER_LIBRARY  avfilter  PATHS ${FFMPEG_LIBRARY_DIRS} REQUIRED NO_DEFAULT_PATH)
    find_library(SWRESAMPLE_LIBRARY swresample PATHS ${FFMPEG_LIBRARY_DIRS} REQUIRED NO_DEFAULT_PATH)
    find_library(SWSCALE_LIBRARY swscale PATHS ${FFMPEG_LIBRARY_DIRS} REQUIRED NO_DEFAULT_PATH)

    set(FFMPEG_LIBRARIES
        ${AVFORMAT_LIBRARY} ${AVCODEC_LIBRARY} ${AVUTIL_LIBRARY}
        ${AVDEVICE_LIBRARY} ${AVFILTER_LIBRARY}
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
        find_library(AVDEVICE_LIBRARY  avdevice  PATHS ${FFMPEG_LIB_DIR} REQUIRED NO_DEFAULT_PATH)
        find_library(AVFILTER_LIBRARY  avfilter  PATHS ${FFMPEG_LIB_DIR} REQUIRED NO_DEFAULT_PATH)
        find_library(SWRESAMPLE_LIBRARY swresample PATHS ${FFMPEG_LIB_DIR} REQUIRED NO_DEFAULT_PATH)
        find_library(SWSCALE_LIBRARY   swscale   PATHS ${FFMPEG_LIB_DIR} REQUIRED NO_DEFAULT_PATH)
        set(FFMPEG_LIBRARIES
            ${AVFORMAT_LIBRARY} ${AVCODEC_LIBRARY} ${AVUTIL_LIBRARY}
            ${AVDEVICE_LIBRARY} ${AVFILTER_LIBRARY}
            ${SWRESAMPLE_LIBRARY} ${SWSCALE_LIBRARY})
    else()
        pkg_check_modules(FFMPEG REQUIRED
            libavcodec libavformat libavutil libavdevice libavfilter
            libswresample libswscale)
    endif()
endif()

# ── Sources ─────────────────────────────────────────────────────────────────
set(SOURCES
    src/main.cpp
    src/RTSPusher.cpp
    src/PusherLifecycleManager.cpp
    src/PusherStateMachine.cpp
    src/ScreenCaptureThread.cpp
    src/VideoRawFrameQueue.cpp
    src/VideoEncodeThread.cpp
    src/GpuVideoEncodeThread.cpp
    src/EncodedPacketQueue.cpp
    src/RTSPMuxThread.cpp
    src/AudioRingBuffer.cpp
    src/SDLAudioCapture.cpp
    src/AudioEncodeThread.cpp
    src/PusherStats.cpp
    src/HardwareEncoderDetector.cpp
    src/logger/Logger.cpp
)

# ── Output directories ──────────────────────────────────────────────────────
if(WIN32)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/../bin/${BUILD_FLAVOR}_$<CONFIG>")
    set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/../bin/${BUILD_FLAVOR}_$<CONFIG>")
    set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/../bin/${BUILD_FLAVOR}_$<CONFIG>")
else()
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/../bin/$<CONFIG>")
endif()

# ── Executable ──────────────────────────────────────────────────────────────
add_executable(${PROJECT_NAME} ${SOURCES})

if(WIN32)
    target_compile_options(${PROJECT_NAME} PRIVATE /utf-8)
    if(MSVC)
        target_compile_options(${PROJECT_NAME} PRIVATE /wd4996)
    endif()
    if(TARGET_ARCH STREQUAL "x86")
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

    if(TARGET_ARCH STREQUAL "x64")
        set(FFMPEG_DLLS
            avcodec-61.dll    avdevice-61.dll   avformat-61.dll
            avfilter-10.dll   avutil-59.dll     swresample-5.dll
            swscale-8.dll
        )
    else()
        set(FFMPEG_DLLS
            avcodec-58.dll    avdevice-58.dll   avformat-58.dll
            avfilter-7.dll    avutil-56.dll     swresample-3.dll
            swscale-5.dll
        )
    endif()

    foreach(dll ${FFMPEG_DLLS})
        add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${FFMPEG_ROOT}/bin/${dll}"
            "$<TARGET_FILE_DIR:${PROJECT_NAME}>"
        )
    endforeach()
endif()
