#include <gst/gst.h>
#include <gst/rtsp-server/rtsp-server.h>

int main(int argc, char *argv[]) {
    gst_init(&argc, &argv);

    GstRTSPServer *server = gst_rtsp_server_new();
    GstRTSPMountPoints *mounts = gst_rtsp_server_get_mount_points(server);

    // Tạo một factory pipeline RTSP phát từ file video (sửa path nếu cần)
    GstRTSPMediaFactory *factory = gst_rtsp_media_factory_new();

    // Phát từ file MP4 (thay path nếu cần)
    gst_rtsp_media_factory_set_launch(factory,
        "( filesrc location= /mnt/c/Users/ADMIN/Downloads/videosample/sample-5s.mp4 ! qtdemux ! h264parse ! rtph264pay name=pay0 pt=96 )");

    // Mount đường dẫn RTSP
    gst_rtsp_mount_points_add_factory(mounts, "/mystream", factory);
    g_object_unref(mounts);

    // Khởi động server
    gst_rtsp_server_attach(server, NULL);

    g_print("RTSP server đang chạy tại rtsp://127.0.0.1:8554/mystream\n");

    // Vòng lặp chính
    GMainLoop *loop = g_main_loop_new(NULL, FALSE);
    g_main_loop_run(loop);

    return 0;
}
