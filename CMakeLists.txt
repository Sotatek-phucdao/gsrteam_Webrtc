cmake_minimum_required(VERSION 3.14)
project(simple_server CXX)

find_package(nlohmann_json CONFIG REQUIRED)

# Đường dẫn include
include_directories(
    "C:/msys64/mingw64/include"
    "C:/msys64/mingw64/include/libsoup-3.0"
    "C:/msys64/mingw64/include/glib-2.0"
    "C:/msys64/mingw64/lib/glib-2.0/include"
    "C:/msys64/mingw64/include/gstreamer-1.0"
    "C:/msys64/mingw64/include/gstreamer-1.0/gst/webrtc"
    "C:/msys64/mingw64/include/json-glib-1.0"
)

# Đường dẫn thư viện
link_directories(
    "C:/msys64/mingw64/lib"
)

# File nguồn
add_executable(simple_server socket.cpp)

# Link các thư viện cần thiết
target_link_libraries(simple_server
    # LibSoup và GLib
    soup-3.0
    gio-2.0
    glib-2.0
    gobject-2.0
    intl

    # GStreamer core
    gstreamer-1.0
    gstbase-1.0

    # GStreamer SDP & WebRTC
    gstsdp-1.0
    gstwebrtc-1.0

    # GStreamer plugins (nếu cần)
    x264
    gstapp-1.0
    gstaudio-1.0
    gstvideo-1.0

    # nlohmann json
    nlohmann_json::nlohmann_json
)
