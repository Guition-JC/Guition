idf_component_register(
    SRCS "dns_captive.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES esp_netif lwip
)
