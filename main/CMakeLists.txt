# IDF v6.0 split the `driver` umbrella into esp_driver_* components; v5.x still
# ships the combined `driver`. Mirror pocket-dial's version-conditional pattern.
if(IDF_VERSION_MAJOR GREATER_EQUAL 6)
    set(DRIVER_REQUIRES esp_driver_gpio esp_driver_i2s)
else()
    set(DRIVER_REQUIRES driver)
endif()

# SIP (TincanUac), the jitter buffer, RTP helpers and G.711 come from the
# shared tincan-core repo (see the top-level CMakeLists.txt). Only the board
# audio path, Wi-Fi bring-up and the media task wiring live here.
idf_component_register(
    SRCS
        "app_main.cpp"
        "audio_io.c"
        "net_wifi.c"
        "media_tasks.cpp"
    INCLUDE_DIRS "."
    REQUIRES
        ${DRIVER_REQUIRES}   # i2s_std + i2s_pdm + gpio
        esp_wifi
        esp_netif
        esp_event
        nvs_flash
        lwip
        esp_timer
        esp_hw_support       # esp_random()
        tincan_uac
        tincan_media
        tincan_codecs
)
