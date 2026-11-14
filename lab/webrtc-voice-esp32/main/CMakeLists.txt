idf_component_register(SRCS "main.c" "webrtc.c" "media_sys.c" "mic.c" "webrtc_http_server.c"
                       EMBED_TXTFILES "webrtc_test.html" "certs/servercert.pem" "certs/prvtkey.pem"
                       INCLUDE_DIRS ".")
