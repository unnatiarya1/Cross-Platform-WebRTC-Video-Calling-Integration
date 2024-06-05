CC := gcc
LIBS := $(shell pkg-config --libs glib-2.0 gstreamer-1.0 gstreamer-sdp-1.0 gstreamer-webrtc-1.0 json-glib-1.0 libsoup-2.4)
CFLAGS := -O0 -ggdb -Wall -fno-omit-frame-pointer \
    $(shell pkg-config --cflags glib-2.0 gstreamer-1.0 gstreamer-sdp-1.0 gstreamer-webrtc-1.0 json-glib-1.0 libsoup-2.4)

#  for flutter 
webrtc-sendrecv: webrtc-sendrecv.c
#  for node 
# fin: fin.c   
	$(CC) $(CFLAGS) $^ $(LIBS) -o $@
