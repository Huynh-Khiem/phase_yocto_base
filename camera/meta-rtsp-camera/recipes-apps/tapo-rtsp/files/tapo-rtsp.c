// SPDX-License-Identifier: MIT
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <gst/app/gstappsink.h>
#include <gst/rtsp/gstrtsptransport.h>
#include <gst/video/video.h>
#include <systemd/sd-daemon.h>

#define CONFIG_ERROR 78
#define STARTUP_TIMEOUT_US (12 * G_USEC_PER_SEC)
#define FRAME_TIMEOUT_US (4 * G_USEC_PER_SEC)
#define REPORT_INTERVAL_US (2 * G_USEC_PER_SEC)
#define SNAPSHOT_PATH "/run/tapo-rtsp/latest.pgm"
#define AI_FRAME_PATH "/run/tapo-rtsp/latest.ppm"

static volatile sig_atomic_t stopping;

static void stop_receiver(int signal_number)
{
    (void)signal_number;
    stopping = 1;
}

static int display_clock(void)
{
    while (!stopping) {
        printf("\r%" G_GINT64_FORMAT "  ", g_get_real_time());
        fflush(stdout);
        g_usleep(10 * 1000);
    }
    putchar('\n');
    return 0;
}

static gboolean select_video(GstElement *source, guint stream, GstCaps *caps,
                             gpointer unused)
{
    (void)source;
    (void)stream;
    (void)unused;
    const GstStructure *format = gst_caps_get_structure(caps, 0);
    const gchar *encoding = gst_structure_get_string(format, "encoding-name");

    return g_strcmp0(gst_structure_get_string(format, "media"), "video") == 0 &&
           encoding && g_ascii_strcasecmp(encoding, "H264") == 0;
}

static gboolean write_frame(GstSample *sample, gint64 received_us, gboolean rgb)
{
    GstVideoInfo info;
    GstVideoFrame frame;

    if (!gst_video_info_from_caps(&info, gst_sample_get_caps(sample)) ||
        !gst_video_frame_map(&frame, &info, gst_sample_get_buffer(sample), GST_MAP_READ))
        return FALSE;

    guint width = GST_VIDEO_FRAME_WIDTH(&frame);
    guint height = GST_VIDEO_FRAME_HEIGHT(&frame);
    gboolean source_rgb = GST_VIDEO_FRAME_FORMAT(&frame) == GST_VIDEO_FORMAT_RGB;
    guint row_size = width * (rgb ? 3 : 1);
    g_autofree gchar *header = g_strdup_printf(
        "%s\n# received_unix_us=%" G_GINT64_FORMAT "\n%u %u\n255\n",
        rgb ? "P6" : "P5", received_us, width, height);
    gsize header_size = strlen(header);
    g_autoptr(GByteArray) image = g_byte_array_sized_new(header_size + row_size * height);
    g_byte_array_append(image, (const guint8 *)header, header_size);
    const guint8 *pixels = GST_VIDEO_FRAME_PLANE_DATA(&frame, 0);
    gint stride = GST_VIDEO_FRAME_PLANE_STRIDE(&frame, 0);

    g_autofree guint8 *gray = source_rgb && !rgb ? g_malloc(width) : NULL;
    for (guint row = 0; row < height; row++) {
        const guint8 *line = pixels + (gssize)row * stride;
        if (gray) {
            for (guint x = 0; x < width; x++)
                gray[x] = (77 * line[3*x] + 150 * line[3*x+1] + 29 * line[3*x+2] + 128) >> 8;
            line = gray;
        }
        g_byte_array_append(image, line, row_size);
    }

    gst_video_frame_unmap(&frame);
    return g_file_set_contents_full(rgb ? AI_FRAME_PATH : SNAPSHOT_PATH,
                                    (const gchar *)image->data, image->len,
                                    G_FILE_SET_CONTENTS_CONSISTENT | G_FILE_SET_CONTENTS_ONLY_EXISTING,
                                    0600, NULL);
}

static bool stream_failed(GstBus *bus)
{
    g_autoptr(GstMessage) message = gst_bus_pop_filtered(bus, GST_MESSAGE_ERROR | GST_MESSAGE_EOS);
    if (!message)
        return false;

    if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_EOS) {
        g_printerr("RTSP stream ended\n");
    } else {
        g_autoptr(GError) error = NULL;
        gst_message_parse_error(message, &error, NULL);
        // Error text and debug details may include authentication information.
        g_printerr("Stream error element=%s domain=%s code=%d\n",
                   GST_OBJECT_NAME(GST_MESSAGE_SRC(message)),
                   g_quark_to_string(error->domain), error->code);
    }

    return true;
}

static int receive_frames(GstElement *pipeline, GstAppSink *sink, bool snapshot, bool ai_frames)
{
    g_autoptr(GstBus) bus = gst_element_get_bus(pipeline);
    if (gst_element_set_state(pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
        stream_failed(bus);
        g_printerr("Cannot start pipeline\n");
        return 1;
    }

    uint64_t watchdog_us;
    gint64 watchdog_interval = 0;
    if (sd_watchdog_enabled(0, &watchdog_us) > 0)
        watchdog_interval = (gint64)(watchdog_us / 3);

    gint64 next_watchdog = 0;
    gint64 last_frame = 0;
    gint64 report_start = g_get_monotonic_time();
    gint64 deadline = report_start + STARTUP_TIMEOUT_US;
    guint64 frames = 0;
    guint report_frames = 0;
    gint64 next_export_warning = 0;

    while (!stopping) {
        if (stream_failed(bus))
            return 1;

        g_autoptr(GstSample) sample = gst_app_sink_try_pull_sample(sink, 100 * GST_MSECOND);
        gint64 now = g_get_monotonic_time();

        if (sample) {
            gint64 received_us = g_get_real_time();
            if (!frames) {
                const GstStructure *format = gst_caps_get_structure(gst_sample_get_caps(sample), 0);
                gint width = 0;
                gint height = 0;
                gst_structure_get_int(format, "width", &width);
                gst_structure_get_int(format, "height", &height);
                g_print("First decoded frame width=%d height=%d\n", width, height);
                report_start = now;
            }

            frames++;
            report_frames++;
            deadline = now + FRAME_TIMEOUT_US;
            if (ai_frames && !write_frame(sample, received_us, TRUE) && now >= next_export_warning) {
                g_printerr("Cannot write " AI_FRAME_PATH "\n");
                next_export_warning = now + REPORT_INTERVAL_US;
            }
            if (now - report_start >= REPORT_INTERVAL_US) {
                g_print("frames=%" G_GUINT64_FORMAT " fps=%.1f latest_gap_ms=%.1f\n",
                        frames, report_frames * (double)G_USEC_PER_SEC / (now - report_start),
                        (now - last_frame) / 1000.0);
                if (snapshot && !write_frame(sample, received_us, FALSE))
                    g_printerr("Cannot write " SNAPSHOT_PATH "\n");
                report_start = now;
                report_frames = 0;
            }
            last_frame = now;
        } else if (now >= deadline) {
            g_printerr("No decoded frame before %s timeout\n", frames ? "stall" : "startup");
            return 1;
        } else {
            // The sink can return immediately before it starts or after EOS.
            g_usleep(10 * 1000);
        }

        if (watchdog_interval > 0 && now >= next_watchdog) {
            sd_notify(0, "WATCHDOG=1");
            next_watchdog = now + watchdog_interval;
        }
    }

    return 0;
}

int main(int argc, char **argv)
{
    signal(SIGINT, stop_receiver);
    signal(SIGTERM, stop_receiver);
    if (argc == 2 && g_str_equal(argv[1], "--clock"))
        return display_clock();

    setvbuf(stdout, NULL, _IOLBF, 0);
    if (argc != 2) {
        g_printerr("Usage: tapo-rtsp CONFIG_FILE | --clock\n");
        return CONFIG_ERROR;
    }

    g_autoptr(GKeyFile) config = g_key_file_new();
    if (!g_key_file_load_from_file(config, argv[1], G_KEY_FILE_NONE, NULL)) {
        g_printerr("Cannot read camera configuration\n");
        return CONFIG_ERROR;
    }

    g_autofree gchar *url = g_key_file_get_string(config, "camera", "url", NULL);
    g_autofree gchar *username = g_key_file_get_string(config, "camera", "username", NULL);
    g_autofree gchar *password = g_key_file_get_string(config, "camera", "password", NULL);
    g_autofree gchar *transport = g_key_file_get_string(config, "camera", "transport", NULL);
    g_autoptr(GError) error = NULL;
    gint latency_ms = g_key_file_get_integer(config, "camera", "latency-ms", &error);
    if (!url || !g_str_has_prefix(url, "rtsp://") || !username || !*username ||
        !password || !*password || error || latency_ms < 0 || latency_ms > 5000 ||
        (g_strcmp0(transport, "udp") != 0 && g_strcmp0(transport, "tcp") != 0)) {
        g_printerr("Invalid camera configuration. Check url, credentials, transport and latency-ms.\n");
        return CONFIG_ERROR;
    }

    bool snapshot = false;
    if (g_key_file_has_key(config, "camera", "snapshot", NULL)) {
        snapshot = g_key_file_get_boolean(config, "camera", "snapshot", &error);
        if (error) {
            g_printerr("snapshot must be true or false\n");
            return CONFIG_ERROR;
        }
    }

    bool ai_frames = false;
    if (g_key_file_has_key(config, "camera", "ai-frames", NULL)) {
        ai_frames = g_key_file_get_boolean(config, "camera", "ai-frames", &error);
        if (error) {
            g_printerr("ai-frames must be true or false\n");
            return CONFIG_ERROR;
        }
    }

    gst_init(NULL, NULL);

    g_autofree gchar *description = g_strdup_printf(
        "rtspsrc name=camera ! rtph264depay ! h264parse ! avdec_h264 max-threads=1 "
        "! videoconvert ! video/x-raw,format=%s "
        "! appsink name=frames sync=false max-buffers=1 drop=true wait-on-eos=false",
        ai_frames ? "RGB" : "GRAY8");
    g_autoptr(GstElement) pipeline = gst_parse_launch(description, &error);
    if (error || !pipeline) {
        g_printerr("Cannot create pipeline: %s\n", error ? error->message : "unknown error");
        return CONFIG_ERROR;
    }

    g_autoptr(GstElement) source = gst_bin_get_by_name(GST_BIN(pipeline), "camera");
    g_autoptr(GstElement) sink = gst_bin_get_by_name(GST_BIN(pipeline), "frames");

    g_object_set(source, "location", url, "user-id", username, "user-pw", password,
                 "protocols", g_str_equal(transport, "udp") ? GST_RTSP_LOWER_TRANS_UDP :
                                                             GST_RTSP_LOWER_TRANS_TCP,
                 "latency", (guint)latency_ms, "drop-on-latency", TRUE,
                 "timeout", (guint64)0, "tcp-timeout", (guint64)4000000,
                 "teardown-timeout", (guint64)0, NULL);
    g_signal_connect(source, "select-stream", G_CALLBACK(select_video), NULL);

    g_print("Starting RTSP receiver transport=%s buffer_ms=%d\n", transport, latency_ms);
    int result = receive_frames(pipeline, GST_APP_SINK(sink), snapshot, ai_frames);
    gst_element_set_state(pipeline, GST_STATE_NULL);
    if (ai_frames)
        unlink(AI_FRAME_PATH);
    return result;
}
