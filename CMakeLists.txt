cmake_minimum_required(VERSION 3.24)
project(webradio VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(FetchContent)
FetchContent_Declare(
    nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG v3.11.3
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(nlohmann_json)

find_package(PkgConfig REQUIRED)
pkg_check_modules(FFMPEG REQUIRED
    libavformat
    libavcodec
    libavutil
    libswresample
)
pkg_check_modules(NCURSESW REQUIRED ncursesw)

add_executable(webradio src/webradio.cpp src/tui.cpp src/fft_spectrum.cpp)

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
