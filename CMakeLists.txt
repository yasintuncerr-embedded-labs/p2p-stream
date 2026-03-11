cmake_minimum_required(VERSION 3.16)
project(p2p-stream C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)


# ── Compiler flags ───────────────────────────────────────────────────────
add_compile_options(
    -Wall -Wextra -Wpedantic
    -Wno-unused-parameter
    -O2
    -fstack-protector-strong
)


# ── Find GStreamer via pkg-config ─────────────────────────────────────────
find_package(PkgConfig REQUIRED)

pkg_check_modules(GST      REQUIRED gstreamer-1.0>=1.16)
pkg_check_modules(GST_APP  REQUIRED gstreamer-app-1.0)
pkg_check_modules(GLIB     REQUIRED glib-2.0)

# Optional: gst-rtsp-server for SINK_RTSP
pkg_check_modules(GST_RTSP gstreamer-rtsp-server-1.0)



# ── Sources ───────────────────────────────────────────────────────────────
set(SOURCES
    src/main.c
    src/logger.c
    src/state_machine.c
    src/net_monitor.c
    src/control.c
    src/pipeline/profile.c
    src/pipeline/pipeline.c
    src/pipeline/host.c
    src/pipeline/client.c
)

add_executable(p2p-stream ${SOURCES})


# ── Include dirs ──────────────────────────────────────────────────────────
target_include_directories(p2p-stream PRIVATE
    ${GST_INCLUDE_DIRS}
    ${GST_APP_INCLUDE_DIRS}
    ${GLIB_INCLUDE_DIRS}
    src/
    src/pipeline/
)


# ── Link libraries ────────────────────────────────────────────────────────
target_link_libraries(p2p-stream
    ${GST_LIBRARIES}
    ${GST_APP_LIBRARIES}
    ${GLIB_LIBRARIES}
    pthread
    m
)


if(GST_RTSP_FOUND)
    target_include_directories(p2p-stream PRIVATE ${GST_RTSP_INCLUDE_DIRS})
    target_link_libraries(p2p-stream ${GST_RTSP_LIBRARIES})
    target_compile_definitions(p2p-stream PRIVATE HAVE_GST_RTSP_SERVER)
    message(STATUS "gst-rtsp-server: FOUND — RTSP sink enabled")
else()
    message(WARNING "gst-rtsp-server: NOT FOUND — RTSP sink will be limited")
endif()


# ── Link flags (pkg-config) ───────────────────────────────────────────────
target_link_options(p2p-stream PRIVATE
    ${GST_LDFLAGS}
    ${GLIB_LDFLAGS}
)



# ── Install ───────────────────────────────────────────────────────────────
install(TARGETS p2p-stream DESTINATION /usr/local/bin)
install(DIRECTORY device-profiles/ DESTINATION /etc/p2p-stream/device-profiles)
install(FILES systemd/p2p-stream.service
        DESTINATION /etc/systemd/system)

message(STATUS "GStreamer version : ${GST_VERSION}")
message(STATUS "GLib    version   : ${GLIB_VERSION}")
message(STATUS "Install prefix    : ${CMAKE_INSTALL_PREFIX}")
