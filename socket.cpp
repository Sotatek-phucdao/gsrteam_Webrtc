#include <libsoup/soup.h>
#include <nlohmann/json.hpp>
#include <gst/gst.h>
#include <gst/sdp/sdp.h>
#include <gst/webrtc/webrtc.h>
#include <mutex>      // ✅ Cho std::mutex, std::lock_guard
#include <iostream>   // ✅ Cho std::cerr, std::endl
#include <thread>


using json = nlohmann::json;


SoupServer *globel_ws = nullptr;
SoupWebsocketConnection *websocket_conn = nullptr;
GstElement *pipeline = nullptr;
GstElement *webrtcbin = nullptr;
std::mutex wsMutex;
static bool offer_sent = false;

void start_pipe_line();
void sendSDPToBrowser(const std::string &sdp) {
    std::lock_guard<std::mutex> lock(wsMutex);
    if (globel_ws) {
        json msg = {{"type", "sdp"}, {"sdp", sdp}};
        soup_websocket_connection_send_text(websocket_conn, msg.dump().c_str());
    } else {
        std::cerr << "❌ WebSocket not connected, cannot send SDP" << std::endl;
    }
}

void sendIceCandidateToBrowser(uint32_t sdpMLineIndex, const std::string &candidate) {
    std::lock_guard<std::mutex> lock(wsMutex);
    if (globel_ws) {
        std::cout << "➡️ Sending ICE candidate to client (mline: " << sdpMLineIndex << "): " << candidate << std::endl; // Thêm dòng này
        json msg = {
            {"type", "ice"},
            {"sdpMLineIndex", sdpMLineIndex},
            {"candidate", candidate}
        };
        soup_websocket_connection_send_text(websocket_conn, msg.dump().c_str());
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

void handle_browser_ice_candidate(int sdpMLineIndex, const std::string &candidate) {
    g_signal_emit_by_name(webrtcbin, "add-ice-candidate", sdpMLineIndex, candidate.c_str());
}



static void on_websocket_connected(SoupServer *server,
                       SoupServerMessage *msg,
                       const char *path,
                       SoupWebsocketConnection *conn,
                       gpointer user_data)
{
    g_print("🔗 WebSocket client connected: %s\n", path);
    websocket_conn = conn;
    g_object_ref(websocket_conn);
    g_signal_connect(conn, "message", G_CALLBACK(+[](
        SoupWebsocketConnection *conn,
        SoupWebsocketDataType type,
        GBytes *message,
        gpointer user_data) {
        
        if (type == SOUP_WEBSOCKET_DATA_TEXT) {
            gsize size;
            const gchar *data = (const gchar*)g_bytes_get_data(message, &size);
            g_print("📩 Received: %.*s\n", (int)size, data);
            json j = json::parse(data);
            if (j["type"] == "sdp") {
                handle_browser_sdp_answer(j["sdp"].get<std::string>());
                std::cout << "✅ Received SDP answer from browser" << std::endl;
            } else if (j["type"] == "ice") {
                handle_browser_ice_candidate(j["sdpMLineIndex"].get<int>(), j["candidate"].get<std::string>());
            }    
            // Echo lại dữ liệu
            //soup_websocket_connection_send_text(conn, data);
        }
    }), NULL);
    g_signal_connect(conn, "closed", G_CALLBACK(+[](
        SoupWebsocketConnection *conn,
        gpointer user_data) {

        g_print("❌ WebSocket client disconnected\n");

        // Giải phóng kết nối WebSocket nếu cần
        if (websocket_conn) {
            g_object_unref(websocket_conn);
            websocket_conn = nullptr;
        }
        offer_sent = false; // Đặt lại cờ offer_sent để có thể tạo offer mới
        // 👉 Tái tạo pipeline
        start_pipe_line();

    }), NULL);
}
static void on_offer_created(GstPromise *promise, gpointer user_data) {
    GstWebRTCSessionDescription *offer = nullptr;

    // Đợi kết quả (nếu promise chưa hoàn tất)
    gst_promise_wait(promise);
    std::cout << "✅ Offer created successfully" << std::endl;
    // ✅ Trả về const GstStructure* 
    const GstStructure *reply = gst_promise_get_reply(promise);
    gst_structure_get(reply, "offer", GST_TYPE_WEBRTC_SESSION_DESCRIPTION, &offer, nullptr);
    gst_promise_unref(promise);

    // 👉 Gửi SDP offer đến trình duyệt
    gchar *sdp_string = gst_sdp_message_as_text(offer->sdp);
    sendSDPToBrowser(sdp_string);  // <-- Bạn cần định nghĩa hàm này để gửi WebSocket
    std::cout << "➡️ Sending SDP offer to browser:\n" << sdp_string << std::endl;
    g_free(sdp_string);

    // 👉 Gán local description
    g_signal_emit_by_name(webrtcbin, "set-local-description", offer, nullptr);
    gst_webrtc_session_description_free(offer);
}

static void on_negotiation_needed(GstElement *webrtcbin) {
    while (!offer_sent)
    {
        if (websocket_conn)
        {
            GstPromise *promise = gst_promise_new_with_change_func(on_offer_created, nullptr, nullptr);
            g_signal_emit_by_name(webrtcbin, "create-offer", nullptr, promise);
            offer_sent = true;
        }
    }
    return;
}


static void on_ice_candidate(GstElement *webrtcbin, guint mlineindex, gchar *candidate, gpointer user_data) {
    // Gửi ICE Candidate lên trình duyệt
    sendIceCandidateToBrowser(mlineindex, candidate);
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

void on_pad_added(GstElement *webrtcbin, GstPad *new_pad, gpointer user_data) {
    gchar *name = gst_pad_get_name(new_pad);
    std::cout << "📦 webrtcbin thêm pad: " << name << std::endl;
    g_free(name);
}

void start_websocket_server(){
    g_autoptr(GMainLoop) loop = NULL;
    g_autoptr(GError) error = NULL;

    // Không cần truyền server header trong Libsoup 3
    globel_ws = soup_server_new(NULL);

    // Thêm handler WebSocket tại path "/ws"
    soup_server_add_websocket_handler(globel_ws,
                                      "/ws",     // path
                                      NULL,      // origin
                                      NULL,      // protocols
                                      on_websocket_connected,
                                      NULL, NULL);

    // Nếu không có flags, truyền 0 cast sang enum
    if (!soup_server_listen_all(globel_ws, 8080, (SoupServerListenOptions)0, &error)) {
        g_printerr("❌ Failed to start server: %s\n", error->message);
        return ;
    }

    g_print("✅ WebSocket server running at ws://127.0.0.1:8080/ws\n");
    

}
// xu ly server (thu nghiem ket noi websocket)

void start_pipe_line(){
    pipeline = gst_parse_launch(
    "rtspsrc location=rtsp://localhost:8554/mystream latency=0 ! "
    "rtph264depay ! h264parse ! rtph264pay config-interval=-1 pt=96 ! "
    "queue name=myqueue",
    nullptr);

    if (!pipeline) {
        std::cerr << " Failed to create pipeline\n";
        return;
    }

    // Tạo webrtcbin
    webrtcbin = gst_element_factory_make("webrtcbin", "webrtcbin");
    if (!webrtcbin) {
        std::cerr << " Failed to create webrtcbin\n";
        return ;
    }

   g_object_set(webrtcbin,
    "stun-server", "stun://stun.l.google.com:19302",
    NULL);

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
        std::cerr << " Failed to link queue to webrtcbin\n";
    } else {
        std::cout << "Linked queue to webrtcbin\n";
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
}

int main(int argc, char **argv)
{
    g_setenv("GST_DEBUG", "3", TRUE);
    gst_init(&argc, &argv);

    
    // Khởi tạo WebSocket server (truyền async callback)
    start_websocket_server();

    // Khởi tạo pipeline
    start_pipe_line();

    GMainLoop *loop = g_main_loop_new(nullptr, FALSE);
    g_main_loop_run(loop);


    
    // Dọn dẹp
    g_main_loop_unref(loop);
    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(pipeline);

    return 0;
}
