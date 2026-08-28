
# Core esp_webrtc component sources only
set(component_srcdirs "src")

# AppRTC signalling
set(signalling_srcdirs  "impl/apprtc_signal")
set(signalling_incdirs  "impl/apprtc_signal")

list(APPEND component_srcdirs ${signalling_srcdirs})

idf_component_register(
    SRC_DIRS ${component_srcdirs}
    INCLUDE_DIRS ./include ${signalling_incdirs}
    REQUIRES esp_http_client esp_websocket_client esp_netif webrtc_utils espressif__cjson esp_timer
)
