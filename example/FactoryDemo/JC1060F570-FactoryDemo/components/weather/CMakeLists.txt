idf_component_register(
    SRCS
        "weather_rules.c"
        "weather_service.c"
        "weather_web.c"
    INCLUDE_DIRS
        "include"
    REQUIRES
        cjson
        esp_http_client
        esp_event
        esp_http_server
        esp_netif
        i18n
        mbedtls
        nvs_flash
    EMBED_FILES "weather.html"
)
