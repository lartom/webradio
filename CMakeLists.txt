cmake_minimum_required(VERSION 3.24)
project(webradio VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Option to enable MusicBrainz metadata fetching
option(ENABLE_MUSICBRAINZ "Enable MusicBrainz metadata fetching" OFF)

# Option to enable metadata debug view
option(METADATA_DEBUG_VIEW "Enable metadata debug panel" OFF)

include(FetchContent)
FetchContent_Declare(
    nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG v3.11.3
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(nlohmann_json)

# Fetch and build libcurl if MusicBrainz is enabled
if(ENABLE_MUSICBRAINZ)
    FetchContent_Declare(
        curl
        GIT_REPOSITORY https://github.com/curl/curl.git
        GIT_TAG curl-8_5_0
        GIT_SHALLOW TRUE
    )
    
    # Configure curl build options for static library
    set(CURL_ENABLE_SSL ON CACHE BOOL "" FORCE)
    set(CURL_USE_OPENSSL ON CACHE BOOL "" FORCE)
    set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
    set(BUILD_CURL_EXE OFF CACHE BOOL "" FORCE)
    set(HTTP_ONLY ON CACHE BOOL "" FORCE)  # Only HTTP/HTTPS support needed
    
    FetchContent_MakeAvailable(curl)
endif()

find_package(PkgConfig REQUIRED)
pkg_check_modules(FFMPEG REQUIRED
    libavformat
    libavcodec
    libavutil
    libswresample
)
pkg_check_modules(NCURSESW REQUIRED ncursesw)

# Determine source files based on options
set(RADIO_PLAYER_SOURCES src/webradio.cpp src/tui.cpp src/fft_spectrum.cpp)
if(ENABLE_MUSICBRAINZ)
    list(APPEND RADIO_PLAYER_SOURCES src/metadata_fetcher.cpp)
endif()

add_executable(webradio ${RADIO_PLAYER_SOURCES})

target_include_directories(webradio PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${FFMPEG_INCLUDE_DIRS}
    ${NCURSESW_INCLUDE_DIRS}
)

target_link_libraries(webradio PRIVATE
    nlohmann_json::nlohmann_json
    ${FFMPEG_LIBRARIES}
    ${NCURSESW_LIBRARIES}
)

if(ENABLE_MUSICBRAINZ)
    target_link_libraries(webradio PRIVATE libcurl_static)
    target_compile_definitions(webradio PRIVATE ENABLE_MUSICBRAINZ=1)
endif()

if(METADATA_DEBUG_VIEW)
    target_compile_definitions(webradio PRIVATE METADATA_DEBUG_VIEW=1)
endif()

target_compile_options(webradio PRIVATE
    ${FFMPEG_CFLAGS_OTHER}
    ${NCURSESW_CFLAGS_OTHER}
)

if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    target_compile_definitions(webradio PRIVATE DEBUG_BUILD AUDIO_DEBUG=1)
else()
    target_compile_definitions(webradio PRIVATE AUDIO_DEBUG=0)
endif()

set_target_properties(webradio PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
)

install(TARGETS webradio
    RUNTIME DESTINATION bin
    LIBRARY DESTINATION lib
    ARCHIVE DESTINATION lib
)
