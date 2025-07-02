#include <gst/gst.h>
#include <gst/sdp/sdp.h>
#include <gst/webrtc/webrtc.h>
#include <uWebSockets/App.h>
#include <nlohmann/json.hpp>
#include <iostream>
#include <atomic>
#include <string>
#include <thread>
#include <mutex>

using json = nlohmann::json;

// Global
struct PerSocketData {};
std::mutex wsMutex;
uWS::WebSocket<false, true, PerSocketData>* globalWs = nullptr;
GstElement *pipeline = nullptr;
GstElement *webrtcbin = nullptr;
std::atomic<bool> offer_pending = false;

// Gửi SDP đến browser
void sendSDPToBrowser(const std::string &sdp) {
    std::lock_guard<std::mutex> lock(wsMutex);
    if (globalWs) {
        json msg = {{"type", "sdp"}, {"sdp", sdp}};
        globalWs->send(msg.dump(), uWS::OpCode::TEXT);
    } else {
        std::cerr << "❌ WebSocket not connected, cannot send SDP" << std::endl;
    }
}

// Gửi ICE đến browser
void sendIceCandidateToBrowser(uint32_t sdpMLineIndex, const std::string &candidate) {
    std::lock_guard<std::mutex> lock(wsMutex);
    if (globalWs) {
        json msg = {
            {"type", "ice"},
            {"sdpMLineIndex", sdpMLineIndex},
            {"candidate", candidate}
        };
        globalWs->send(msg.dump(), uWS::OpCode::TEXT);
    } else {
        std::cerr << "❌ WebSocket not connected, cannot send ICE candidate" << std::endl;
    }
}

// Nhận SDP answer từ browser
void handle_browser_sdp_answer(const std::string &sdp_str) {
    GstSDPMessage *sdp;
    gst_sdp_message_new(&sdp);
    gst_sdp_message_parse_buffer((const guint8 *)sdp_str.c_str(), sdp_str.length(), sdp);
    GstWebRTCSessionDescription *answer = gst_webrtc_session_description_new(GST_WEBRTC_SDP_TYPE_ANSWER, sdp);
    g_signal_emit_by_name(webrtcbin, "set-remote-description", answer, nullptr);
    gst_webrtc_session_description_free(answer);
}

// Nhận ICE từ browser
void handle_browser_ice_candidate(int sdpMLineIndex, const std::string &candidate) {
    g_signal_emit_by_name(webrtcbin, "add-ice-candidate", sdpMLineIndex, candidate.c_str());
}

// Hàm callback khi webrtcbin sinh ICE candidate mới
static void on_ice_candidate(GstElement *webrtcbin, guint mline_index, gchar *candidate, gpointer user_data) {
    std::cout << "📦 New ICE candidate: mline=" << mline_index << " candidate=" << candidate << std::endl;

    // Gửi ICE candidate sang browser qua WebSocket
    sendIceCandidateToBrowser(mline_index, candidate);
}


// Gọi khi có client kết nối WebSocket
void startWebSocketServer() {
    std::thread([]() {
        uWS::App().ws<PerSocketData>("/*", {
            .open = [](auto *ws) {
                std::lock_guard<std::mutex> lock(wsMutex);
                globalWs = ws;
                std::cout << "✅ Client connected.\n";

                // Gọi offer lại nếu client kết nối sau
                if (webrtcbin) {
                    g_idle_add([](gpointer) -> gboolean {
                    GstPromise *promise = gst_promise_new_with_change_func(
                        [](GstPromise *promise, gpointer user_data) {
                        const GstStructure *reply = gst_promise_get_reply(promise);
                        GstWebRTCSessionDescription *offer;
                        gst_structure_get(reply, "offer", GST_TYPE_WEBRTC_SESSION_DESCRIPTION, &offer, nullptr);
                        gst_promise_unref(promise);

                        // Set local description
                        g_signal_emit_by_name(webrtcbin, "set-local-description", offer, nullptr);

                        // Gửi SDP cho trình duyệt
                        gchar *sdp_str = gst_sdp_message_as_text(offer->sdp);
                        sendSDPToBrowser(sdp_str);
                        g_free(sdp_str);
                        gst_webrtc_session_description_free(offer);

                        }, nullptr, nullptr
                    );

                    g_signal_emit_by_name(webrtcbin, "create-offer", nullptr, promise);
                    return G_SOURCE_REMOVE;
                }, nullptr);
            }

            },
            .message = [](auto *ws, std::string_view msg, uWS::OpCode opCode) {
                json j = json::parse(msg);
                if (j["type"] == "sdp") {
                    handle_browser_sdp_answer(j["sdp"].get<std::string>());
                    g_signal_connect(webrtcbin, "on-ice-candidate", G_CALLBACK(on_ice_candidate), nullptr);
                } else if (j["type"] == "ice") {
                    handle_browser_ice_candidate(j["sdpMLineIndex"].get<int>(), j["candidate"].get<std::string>());
                }
            },
            .close = [](auto *ws, int code, std::string_view msg) {
                std::lock_guard<std::mutex> lock(wsMutex);
                globalWs = nullptr;
                std::cout << "❌ Client disconnected\n";
            }
        }).listen("0.0.0.0",9001, [](auto *token) {
            if (token) {
                std::cout << "📡 Listening on ws://localhost:9001\n";
            } else {
                std::cerr << "❌ Failed to bind WebSocket port\n";
            }
        }).run();
    }).detach();
}

// Xử lý tín hiệu tạo offer
static void on_negotiation_needed(GstElement *element, gpointer user_data) {
    if (!globalWs) {
        std::cout << "⏳ Waiting for client to connect before creating offer...\n";
        return;
    }

    if (offer_pending) {
        std::cout << "⏸️ Offer already pending. Skipping.\n";
        return;
    }

    offer_pending = true;

    std::cout << "🟢 Creating SDP offer...\n";
    GstPromise *promise = gst_promise_new_with_change_func([](GstPromise *promise, gpointer user_data) {
        const GstStructure *reply = gst_promise_get_reply(promise);
        GstWebRTCSessionDescription *offer;
        gst_structure_get(reply, "offer", GST_TYPE_WEBRTC_SESSION_DESCRIPTION, &offer, nullptr);
        gst_promise_unref(promise);

        g_signal_emit_by_name(webrtcbin, "set-local-description", offer, nullptr);

        gchar *sdp_str = gst_sdp_message_as_text(offer->sdp);
        sendSDPToBrowser(sdp_str);
        g_free(sdp_str);
        gst_webrtc_session_description_free(offer);

        offer_pending = false;
    }, nullptr, nullptr);

    g_signal_emit_by_name(webrtcbin, "create-offer", nullptr, promise);
}




void on_pad_added(GstElement *webrtcbin, GstPad *new_pad, gpointer user_data) {
    gchar *name = gst_pad_get_name(new_pad);
    std::cout << "📦 webrtcbin thêm pad: " << name << std::endl;
    g_free(name);
}

// Xử lý log pipeline
static gboolean bus_callback(GstBus *bus, GstMessage *msg, gpointer user_data) {
    switch (GST_MESSAGE_TYPE(msg)) {
        case GST_MESSAGE_ERROR: {
            GError *err;
            gchar *debug;
            gst_message_parse_error(msg, &err, &debug);
            std::cerr << "❌ ERROR: " << err->message << std::endl;
            g_error_free(err);
            g_free(debug);
            break;
        }
        case GST_MESSAGE_STATE_CHANGED: {
            if (GST_MESSAGE_SRC(msg) == GST_OBJECT(pipeline)) {
                GstState old_state, new_state, pending;
                gst_message_parse_state_changed(msg, &old_state, &new_state, &pending);
                std::cout << "🔄 Pipeline state changed: " 
                          << gst_element_state_get_name(old_state) << " → " 
                          << gst_element_state_get_name(new_state) << std::endl;
            }
            break;
        }
        default: break;
    }
    return TRUE;
}

int main(int argc, char *argv[]) {
    gst_init(&argc, &argv);

    // Tạo pipeline
    pipeline = gst_parse_launch(
    "rtspsrc location=rtsp://localhost:8554/mystream latency=0 ! "
    "rtph264depay ! h264parse ! rtph264pay config-interval=-1 pt=96 ! "
    "queue name=myqueue",
    nullptr);

    if (!pipeline) {
        std::cerr << "❌ Failed to create pipeline\n";
        return -1;
    }

    // Tạo webrtcbin
    webrtcbin = gst_element_factory_make("webrtcbin", "webrtcbin");
    if (!webrtcbin) {
        std::cerr << "❌ Failed to create webrtcbin\n";
        return -1;
    }

    g_object_set(webrtcbin,
    "stun-server", "stun://stun.l.google.com:19302",
    nullptr);
    gst_bin_add(GST_BIN(pipeline), webrtcbin);
    gst_element_sync_state_with_parent(webrtcbin);

    // Kết nối pad
    GstElement *queue = gst_bin_get_by_name(GST_BIN(pipeline), "myqueue");
    GstPad *src = gst_element_get_static_pad(queue, "src");
    GstPad *sink = gst_element_request_pad_simple(webrtcbin, "sink_%u");

    GstCaps *caps = gst_caps_from_string("application/x-rtp,media=video,encoding-name=H264,payload=96");
    gst_pad_set_caps(src, caps);
    gst_caps_unref(caps);

    if (gst_pad_link(src, sink) != GST_PAD_LINK_OK) {
        std::cerr << "❌ Failed to link queue → webrtcbin\n";
    } else {
        std::cout << "✅ Linked queue → webrtcbin\n";
    }

    // Gán callback SDP/ICE
    g_signal_connect(webrtcbin, "on-negotiation-needed", G_CALLBACK(on_negotiation_needed), nullptr);
    g_signal_connect(webrtcbin, "on-ice-candidate", G_CALLBACK(on_ice_candidate), nullptr);
    g_signal_connect(webrtcbin, "pad-added", G_CALLBACK(on_pad_added), nullptr);

    // Theo dõi bus
    GstBus *bus = gst_element_get_bus(pipeline);
    gst_bus_add_watch(bus, bus_callback, nullptr);
    gst_object_unref(bus);

    // Chạy pipeline
    gst_element_set_state(pipeline, GST_STATE_PLAYING);

    // WebSocket
    startWebSocketServer();

    // Main loop
    GMainLoop *loop = g_main_loop_new(nullptr, FALSE);
    g_main_loop_run(loop);

    // Dọn dẹp
    g_main_loop_unref(loop);
    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(pipeline);
    return 0;
}
