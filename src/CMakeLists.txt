## This component bundles the demo application and the small support modules it
## depends on. The radio code lives in the same component so the app can include
## the headers directly and native tests can reuse the same sources.
idf_component_register(
    SRCS
        # Main console application and helper modules.
        "main.cpp"
        "audio_packet.cpp"
        "audio_reassembler.cpp"
        "esp32_nrf24_hal.cpp"
        "morse.cpp"
        "nrf24.cpp"
        "radio_manager.cpp"
        "validation.cpp"
    INCLUDE_DIRS
        # Local component headers plus the shared project include directory.
        "."
        "../include"
    REQUIRES
        # Public dependencies used directly by the implementation.
        esp_driver_gpio
        esp_driver_spi
        esp_timer
        esp_rom
    PRIV_REQUIRES
        # Serial Peripheral Interface Flash File System (SPIFFS) is only used
        # internally by main.cpp, so it remains private.
        spiffs
)
