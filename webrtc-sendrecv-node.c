//  sudo ldconfig && make
//  ./webrtc-sendrecv --peer-id=2120
#include <gst/gst.h>
#include <gst/sdp/sdp.h>
#define GST_USE_UNSTABLE_API
#include <gst/webrtc/webrtc.h>
#include <libsoup/soup.h>
#include <json-glib/json-glib.h>
#include <string.h>

static GMainLoop *loop;
static GstElement *pipe1, *webrtc1;
static GObject *send_channel, *receive_channel;
// static GObject *send_channel;

static SoupWebsocketConnection *ws_conn = NULL;
static const gchar *peer_id = NULL;
static const gchar *server_url = "ws://192.168.68.103:8443";
static gboolean disable_ssl = FALSE;

static GOptionEntry entries[] = {
    {NULL},
};

static gchar *
get_string_from_json_object(JsonObject *object)
{
    JsonNode *root;
    JsonGenerator *generator;
    gchar *text;

    /* Make it the root node */
    root = json_node_init_object(json_node_alloc(), object);
    generator = json_generator_new();
    json_generator_set_root(generator, root);
    text = json_generator_to_data(generator, NULL);

    /* Release everything */
    g_object_unref(generator);
    json_node_free(root);
    return text;
}

static void
send_ice_candidate_message(GstElement *webrtc G_GNUC_UNUSED, guint mlineindex,
                           gchar *candidate, gpointer user_data G_GNUC_UNUSED)
{
    gchar *text;
    JsonObject *ice, *msg;

    ice = json_object_new();
    json_object_set_string_member(ice, "candidate", candidate);
    json_object_set_int_member(ice, "sdpMLineIndex", mlineindex);
    msg = json_object_new();
    json_object_set_object_member(msg, "ice", ice);
    text = get_string_from_json_object(msg);
    json_object_unref(msg);

    soup_websocket_connection_send_text(ws_conn, text);
    g_free(text);
}

static void
send_sdp_to_peer(GstWebRTCSessionDescription *desc)
{
    gchar *text;
    JsonObject *msg, *sdp;

    text = gst_sdp_message_as_text(desc->sdp);
    sdp = json_object_new();

    if (desc->type == GST_WEBRTC_SDP_TYPE_OFFER)
    {
        // g_print("Sending offer", text);
       
        json_object_set_string_member(sdp, "type", "offer");
    }

    json_object_set_string_member(sdp, "sdp", text);
    g_free(text);

    // msg = json_object_new();
    // json_object_set_object_member(msg, "sdp", sdp);
    text = get_string_from_json_object(sdp);
    json_object_unref(sdp);

    soup_websocket_connection_send_text(ws_conn, text);
    g_print("Sending offer:\n%s\n", text);
    g_free(text);
}

/* Offer created by our pipeline, to be sent to the peer */
static void
on_offer_created(GstPromise *promise, gpointer user_data)
{
    
    GstWebRTCSessionDescription *offer = NULL;
    const GstStructure *reply;

    g_assert_cmphex(gst_promise_wait(promise), ==, GST_PROMISE_RESULT_REPLIED);
    reply = gst_promise_get_reply(promise);
    gst_structure_get(reply, "offer", GST_TYPE_WEBRTC_SESSION_DESCRIPTION, &offer, NULL);
    gst_promise_unref(promise);

    promise = gst_promise_new();
    g_signal_emit_by_name(webrtc1, "set-local-description", offer, promise);
    gst_promise_interrupt(promise);
    gst_promise_unref(promise);

    /* Send offer to peer */
    send_sdp_to_peer(offer);
    gst_webrtc_session_description_free(offer);
}

static void
on_negotiation_needed(GstElement *element, gpointer user_data)
{
        GArray *transceivers;
        g_signal_emit_by_name(element, "get-transceivers", &transceivers);
        GstWebRTCRTPTransceiver *transceiver = g_array_index(transceivers, GstWebRTCRTPTransceiver *, 0);
        transceiver->direction = GST_WEBRTC_RTP_TRANSCEIVER_DIRECTION_SENDONLY;
        g_object_set(transceiver, "fec-type", GST_WEBRTC_FEC_TYPE_ULP_RED, NULL);
        g_object_set(transceiver, "do-nack", TRUE, NULL);

        GstPromise *promise;
        promise =
            gst_promise_new_with_change_func(on_offer_created, user_data, NULL);
        ;
        g_signal_emit_by_name(webrtc1, "create-offer", NULL, promise);
}

#define STUN_SERVER " stun-server=stun://stun.l.google.com:19302 "
#define RTP_CAPS_VP8 "application/x-rtp,media=video,encoding-name=VP8,payload="


static void
data_channel_on_open(GObject *dc, gpointer user_data)
{
    GBytes *bytes = g_bytes_new("data", strlen("data"));
    g_print("data channel opened\n");
    g_signal_emit_by_name(dc, "send-string", "Hi! from GStreamer");
    g_signal_emit_by_name(dc, "send-data", bytes);
    g_bytes_unref(bytes);
}

static void
data_channel_on_message_string(GObject *dc, gchar *str, gpointer user_data)
{
    g_print("Received data channel message: %s\n", str);
}

static void
connect_data_channel_signals(GObject *data_channel)
{
    g_signal_connect(data_channel, "on-open", G_CALLBACK(data_channel_on_open), NULL);
    g_signal_connect(data_channel, "on-message-string", G_CALLBACK(data_channel_on_message_string), NULL);
}

static void
on_data_channel(GstElement *webrtc, GObject *data_channel,
                gpointer user_data)
{
    connect_data_channel_signals(data_channel);
    receive_channel = data_channel;
}

static gboolean
start_pipeline(void)
{
    GstStateChangeReturn ret;
    GError *error = NULL;

    pipe1 = gst_parse_launch("webrtcbin bundle-policy=max-bundle name=sendrecv " STUN_SERVER
                         "v4l2src device=/dev/video0 ! videoconvert ! videorate ! video/x-raw,framerate=60/1 ! "
                         "nvvidconv ! video/x-raw(memory:NVMM),format=I420 ! omxh264enc ! "
                         "h264parse ! rtph264pay ! "
                         "sendrecv. ",
                         &error);
                    
    webrtc1 = gst_bin_get_by_name(GST_BIN(pipe1), "sendrecv");
    g_assert_nonnull(webrtc1);
    g_signal_emit_by_name(webrtc1, "create-data-channel", "channel", NULL, &send_channel);
    g_signal_connect(webrtc1, "on-negotiation-needed", G_CALLBACK(on_negotiation_needed), NULL);

    g_signal_connect(webrtc1, "on-ice-candidate", G_CALLBACK(send_ice_candidate_message), NULL);

    gst_element_set_state(pipe1, GST_STATE_READY);

    g_signal_emit_by_name(webrtc1, "create-data-channel", "channel", NULL, &send_channel);
    if (send_channel)
    {
        g_print("Created data channel\n");
        connect_data_channel_signals(send_channel);
    }
    else
    {
        g_print("Could not create data channel, is usrsctp available?\n");
    }

    g_signal_connect(webrtc1, "on-data-channel", G_CALLBACK(on_data_channel), NULL);

    gst_object_unref(webrtc1);

    g_print("Starting pipeline\n");
    ret = gst_element_set_state(GST_ELEMENT(pipe1), GST_STATE_PLAYING);

    return TRUE;
 
}

/* One mega message handler for our asynchronous calling mechanism */
// static void
on_server_message(SoupWebsocketConnection *conn, SoupWebsocketDataType type,
                  GBytes *message, gpointer user_data)
{
    gchar *text;

    switch (type)
    {
    case SOUP_WEBSOCKET_DATA_TEXT:
    {
        gsize size;
        const gchar *data = g_bytes_get_data(message, &size);
        /* Convert to NULL-terminated string */
        text = g_strndup(data, size);
        break;
    }
    default:
        g_assert_not_reached();
    }
        JsonNode *root;
        JsonObject *object, *child;
        JsonParser *parser = json_parser_new();
        if (!json_parser_load_from_data(parser, text, -1, NULL))
        {
            g_object_unref(parser);
            goto out;
        }

        root = json_parser_get_root(parser);
        object = json_node_get_object(root);
       
        if (json_object_has_member(object, "sdp"))
        {
            int ret;
            GstSDPMessage *sdp;
            const gchar *text, *sdptype;
            GstWebRTCSessionDescription *answer;

            sdptype = json_object_get_string_member(object, "type");

            text = json_object_get_string_member(object, "sdp");
            ret = gst_sdp_message_new(&sdp);
            g_assert_cmphex(ret, ==, GST_SDP_OK);
            ret = gst_sdp_message_parse_buffer((guint8 *)text, strlen(text), sdp);
            g_assert_cmphex(ret, ==, GST_SDP_OK);

            if (g_str_equal(sdptype, "answer"))
            {
                g_print("Received answer:\n%s\n", text);
                answer = gst_webrtc_session_description_new(GST_WEBRTC_SDP_TYPE_ANSWER,
                                                            sdp);
                g_assert_nonnull(answer);

                /* Set remote description on our pipeline */
                {
                    GstPromise *promise = gst_promise_new();
                    g_signal_emit_by_name(webrtc1, "set-remote-description", answer, promise);
                    gst_promise_interrupt(promise);
                    gst_promise_unref(promise);
                    g_print("Received answer-----------------");
                }
                // app_state = PEER_CALL_STARTED;
            }
        }

        // gchar **parts = g_strsplit(text, "|", 2);
        // g_print("Received answer\n");

        // gchar *type_str = g_strdup(parts[1]);
        // gchar *sdp_str = g_strdup(parts[0]);

        // if (g_strcmp0(type_str, "answer") == 0){

        //     g_print("Received answer\n");
        //     GstSDPMessage *sdp_message;
        //     int ret = gst_sdp_message_new(&sdp_message);
        //     g_assert_cmphex(ret, ==, GST_SDP_OK);
        //     ret = gst_sdp_message_parse_buffer((guint8 *)sdp_str, strlen(sdp_str), sdp_message);
        //     g_assert_cmphex(ret, ==, GST_SDP_OK);

        //     GstPromise *promise;

        //     GstWebRTCSessionDescription *answer = gst_webrtc_session_description_new(GST_WEBRTC_SDP_TYPE_ANSWER,
        //                                                     sdp_str);
        //     g_assert_nonnull(answer);

        //         /* Set remote description on our pipeline */
        //     {
        //             GstPromise *promise = gst_promise_new();
        //             g_signal_emit_by_name(webrtc1, "set-remote-description", answer, promise);
        //             gst_promise_interrupt(promise);
        //             gst_promise_unref(promise);
        //             g_print("Received answer-----------------");
        //     }
        //         // app_state = PEER_CALL_STARTED;
        //     }
        // g_free(type_str);
        // g_free(sdp_str);
        // g_strfreev(parts);
        // g_free(text);

        // }

        // else
        // {
        //     g_printerr("Ignoring unknown JSON message:\n%s\n", text);
        // }
        // g_object_unref(parser);
// 
out:
    g_free(text);
}

// static void
// on_server_message(SoupWebsocketConnection *conn, SoupWebsocketDataType type,
//                   GBytes *message, gpointer user_data)
// {
//     gchar *text;

//     switch (type)
//     {
//     case SOUP_WEBSOCKET_DATA_TEXT:
//     {
//         gsize size;
//         const gchar *data = g_bytes_get_data(message, &size);
//         /* Convert to NULL-terminated string */
//         text = g_strndup(data, size);
//         break;
//     }
//     default:
//         g_assert_not_reached();
//     }

//     gchar **parts = g_strsplit(text, "|", 2);
//     gchar *type_str = g_strdup(parts[1]);
//     gchar *sdp_str = g_strdup(parts[0]);

//     if (g_strcmp0(type_str, "answer") == 0)
//     {
//         GstSDPMessage *sdp_message;
//         int ret = gst_sdp_message_new(&sdp_message);
//         if (ret != GST_SDP_OK)
//         {
//             g_printerr("Failed to create SDP message\n");
//             goto cleanup;
//         }

//         ret = gst_sdp_message_parse_buffer((guint8 *)sdp_str, strlen(sdp_str), sdp_message);
//         if (ret != GST_SDP_OK)
//         {
//             g_printerr("Failed to parse SDP message\n");
//             gst_sdp_message_free(sdp_message);
//             goto cleanup;
//         }

//         GstWebRTCSessionDescription *answer = gst_webrtc_session_description_new(GST_WEBRTC_SDP_TYPE_ANSWER, sdp_message);
//         if (answer == NULL)
//         {
//             g_printerr("Failed to create WebRTC session description\n");
//             gst_sdp_message_free(sdp_message);
//             goto cleanup;
//         }

//         /* Set remote description on our pipeline */
//         {
//             GstPromise *promise = gst_promise_new();
//             g_signal_emit_by_name(webrtc1, "set-remote-description", answer, promise);
//             gst_promise_interrupt(promise);
//             gst_promise_unref(promise);
//             g_print("Received answer\n");
//         }
//     }

// cleanup:
//     g_free(type_str);
//     g_free(sdp_str);
//     g_strfreev(parts);
//     g_free(text);
// }

static void
on_server_connected(SoupSession *session, GAsyncResult *res,
                    SoupMessage *msg)
{
    GError *error = NULL;

    ws_conn = soup_session_websocket_connect_finish(session, res, &error);

    g_assert_nonnull(ws_conn);

    g_print("Connected to signalling server\n");

    g_signal_connect(ws_conn, "message", G_CALLBACK(on_server_message), NULL);

}

static void
connect_to_websocket_server_async(void)
{
    SoupLogger *logger;
    SoupMessage *message;
    SoupSession *session;
    const char *https_aliases[] = {"ws", NULL};

    session =
        soup_session_new_with_options(SOUP_SESSION_SSL_STRICT, !disable_ssl,
                                      SOUP_SESSION_SSL_USE_SYSTEM_CA_FILE, TRUE,
                                      SOUP_SESSION_HTTPS_ALIASES, https_aliases, NULL);

    logger = soup_logger_new(SOUP_LOGGER_LOG_BODY, -1);
    soup_session_add_feature(session, SOUP_SESSION_FEATURE(logger));
    g_object_unref(logger);

    message = soup_message_new(SOUP_METHOD_GET, server_url);

    g_print("Connecting to server...\n");

    /* Once connected, we will register */
    soup_session_websocket_connect_async(session, message, NULL, NULL, NULL, (GAsyncReadyCallback)on_server_connected, message);
    start_pipeline();
}

int main(int argc, char *argv[])
{
    GOptionContext *context;
    GError *error = NULL;

    context = g_option_context_new("- gstreamer webrtc sendrecv demo");
    g_option_context_add_main_entries(context, entries, NULL);
    g_option_context_add_group(context, gst_init_get_option_group());
    if (!g_option_context_parse(context, &argc, &argv, &error))
    {
        g_printerr("Error initializing: %s\n", error->message);
        return -1;
    }

    loop = g_main_loop_new(NULL, FALSE);

    connect_to_websocket_server_async();

    g_main_loop_run(loop);
    g_main_loop_unref(loop);

    if (pipe1)
    {
        gst_element_set_state(GST_ELEMENT(pipe1), GST_STATE_NULL);
        g_print("Pipeline stopped\n");
        gst_object_unref(pipe1);
    }

    return 0;
}
