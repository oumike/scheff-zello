# scheff-zello for Linux: the M5Stack CardputerZero (BOARD=cp0, an arm64
# .deb for its launcher) and a desktop simulator (BOARD=sim). The app is
# main/'s shared code over linux/compat (ESP-IDF and FreeRTOS on POSIX) and
# linux/hal (framebuffer or SDL window, evdev keyboard, SDL audio, ALSA mixer).
cmake_minimum_required(VERSION 3.20)

file(READ "${CMAKE_CURRENT_LIST_DIR}/../VERSION" SCHEFF_VERSION)
string(STRIP "${SCHEFF_VERSION}" SCHEFF_VERSION)
project(scheff-zello VERSION ${SCHEFF_VERSION} LANGUAGES C)

set(BOARD "sim" CACHE STRING "cp0 (the CardputerZero) or sim (the desktop simulator)")
set_property(CACHE BOARD PROPERTY STRINGS cp0 sim)
if(NOT BOARD MATCHES "^(cp0|sim)$")
  message(FATAL_ERROR "BOARD must be cp0 or sim, not '${BOARD}'")
endif()

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

set(ROOT ${CMAKE_CURRENT_LIST_DIR}/..)
set(MAIN ${ROOT}/main)

# ---- LVGL ----------------------------------------------------------------
# The ESP build's managed_components copy when it's there, else fetched.
set(LVGL_VERSION 9.3.0)
if(NOT LVGL_SOURCE_DIR AND EXISTS ${ROOT}/managed_components/lvgl__lvgl/lvgl.h)
  file(READ ${ROOT}/managed_components/lvgl__lvgl/library.json _lvgl_json)
  if(_lvgl_json MATCHES "\"version\": \"${LVGL_VERSION}\"")
    set(LVGL_SOURCE_DIR ${ROOT}/managed_components/lvgl__lvgl)
  endif()
endif()
include(FetchContent)
if(NOT LVGL_SOURCE_DIR)
  FetchContent_Declare(lvgl
    GIT_REPOSITORY https://github.com/lvgl/lvgl.git
    GIT_TAG v${LVGL_VERSION}
    GIT_SHALLOW TRUE
    SOURCE_SUBDIR _none)
  FetchContent_MakeAvailable(lvgl)
  set(LVGL_SOURCE_DIR ${lvgl_SOURCE_DIR})
endif()
file(GLOB_RECURSE LVGL_SOURCES CONFIGURE_DEPENDS ${LVGL_SOURCE_DIR}/src/*.c)
add_library(lvgl STATIC ${LVGL_SOURCES})
target_include_directories(lvgl SYSTEM PUBLIC ${LVGL_SOURCE_DIR} ${CMAKE_CURRENT_LIST_DIR})
target_compile_definitions(lvgl PUBLIC LV_CONF_INCLUDE_SIMPLE)
target_compile_options(lvgl PRIVATE -w)

# ---- libraries -----------------------------------------------------------
find_package(PkgConfig REQUIRED)
pkg_check_modules(OPUS REQUIRED IMPORTED_TARGET opus)
pkg_check_modules(CURL REQUIRED IMPORTED_TARGET libcurl)
pkg_check_modules(SDL2 REQUIRED IMPORTED_TARGET sdl2)
pkg_check_modules(CJSON IMPORTED_TARGET libcjson)
if(NOT CJSON_FOUND)
  FetchContent_Declare(cjson
    GIT_REPOSITORY https://github.com/DaveGamble/cJSON.git
    GIT_TAG v1.7.18
    GIT_SHALLOW TRUE
    SOURCE_SUBDIR _none)
  FetchContent_MakeAvailable(cjson)
  add_library(cjson_vendored STATIC ${cjson_SOURCE_DIR}/cJSON.c)
  target_include_directories(cjson_vendored SYSTEM PUBLIC ${cjson_SOURCE_DIR})
  set(CJSON_TARGET cjson_vendored)
else()
  set(CJSON_TARGET PkgConfig::CJSON)
endif()
find_package(Threads REQUIRED)
# The system mixer, so the volume is the OS's own. Required on the device;
# without it (the macOS simulator) the app scales its own stream instead.
if(BOARD STREQUAL "cp0")
  pkg_check_modules(ALSA REQUIRED IMPORTED_TARGET alsa)
else()
  pkg_check_modules(ALSA IMPORTED_TARGET alsa)
endif()

# ---- the web config page -------------------------------------------------
# index_html_start/end as the ESP build's EMBED_TXTFILES gives them: the
# page's bytes and a terminating NUL, end just past it.
set(INDEX_C ${CMAKE_CURRENT_BINARY_DIR}/index_html.c)
add_custom_command(
  OUTPUT ${INDEX_C}
  COMMAND ${CMAKE_COMMAND} -DIN=${MAIN}/web/index.html -DOUT=${INDEX_C} -P ${CMAKE_CURRENT_LIST_DIR}/embed.cmake
  DEPENDS ${MAIN}/web/index.html ${CMAKE_CURRENT_LIST_DIR}/embed.cmake
  VERBATIM)

# ---- the app -------------------------------------------------------------
add_executable(scheff-zello
  main_linux.c
  ${INDEX_C}
  compat/esp_http_server.c
  compat/esp_misc.c
  compat/esp_websocket_client.c
  compat/freertos.c
  compat/nvs.c
  hal/audio_linux.c
  hal/board_linux.c
  hal/cp0_keys.c
  hal/display_linux.c
  hal/fb_linux.c
  hal/net_linux.c
  hal/sdl_window.c
  ${MAIN}/channel_list.c
  ${MAIN}/file_store.c
  ${MAIN}/input_keys.c
  ${MAIN}/jwt_util.c
  ${MAIN}/onboard.c
  ${MAIN}/opus_codec.c
  ${MAIN}/settings.c
  ${MAIN}/ui.c
  ${MAIN}/ui_account.c
  ${MAIN}/ui_channels.c
  ${MAIN}/ui_home.c
  ${MAIN}/ui_saver.c
  ${MAIN}/ui_wifi.c
  ${MAIN}/web_config.c
  ${MAIN}/xml_util.c
  ${MAIN}/yaml_util.c
  ${MAIN}/zello_client.c)
# compat/ first, so its sdkconfig.h and ESP headers are the ones found.
target_include_directories(scheff-zello PRIVATE compat hal ${CMAKE_CURRENT_LIST_DIR} ${MAIN})
target_compile_definitions(scheff-zello PRIVATE SCHEFF_VERSION="${SCHEFF_VERSION}" _GNU_SOURCE)
if(BOARD STREQUAL "sim")
  target_compile_definitions(scheff-zello PRIVATE ZELLO_LINUX_SIM=1)
endif()
target_compile_options(scheff-zello PRIVATE -Wall -Wno-unused-function -Wno-format-truncation)
target_link_libraries(scheff-zello PRIVATE
  lvgl ${CJSON_TARGET} PkgConfig::OPUS PkgConfig::CURL PkgConfig::SDL2 Threads::Threads m)
if(ALSA_FOUND)
  target_compile_definitions(scheff-zello PRIVATE HAVE_ALSA_MIXER=1)
  target_link_libraries(scheff-zello PRIVATE PkgConfig::ALSA)
endif()

# ---- the CardputerZero's .deb --------------------------------------------
if(BOARD STREQUAL "cp0")
  include(packaging/package.cmake)
endif()
