# Compiler & Flags
CXX = g++
CXXFLAGS = -Wall -g -std=c++17

# Output binary
TARGET = webrtc_gstreamer

# Source files
SRC = main.cpp

# GStreamer & jsoncpp flags
PKG_FLAGS = $(shell pkg-config --cflags --libs gstreamer-1.0 gstreamer-webrtc-1.0 gstreamer-sdp-1.0) -ljsoncpp

# uWebSockets static linking
UWS_LIBS = -L/usr/local/lib -l:uSockets.a -lssl -lcrypto -lz -lpthread -l:uSockets.a
INCLUDES = -I/usr/local/include

# Default target
all: $(TARGET)

# Build binary
$(TARGET): $(SRC)
	$(CXX) $(CXXFLAGS) $(INCLUDES) -o $@ $^ $(PKG_FLAGS) $(UWS_LIBS)

# Clean target
clean:
	rm -f $(TARGET)

# Rebuild
rebuild: clean all
