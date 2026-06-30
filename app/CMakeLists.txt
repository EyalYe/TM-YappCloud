# app_yapp — the "Yapp" direct-Todoist app component.
# REQUIRES: taskmaster_core + esp_http_client/esp-tls/mbedtls (HTTPS+crt_bundle) +
# espressif__cjson. tasks.h is header-only (static fns) to avoid a 2-app clash.
idf_component_register(
    SRCS "app_yapp.c"
    REQUIRES taskmaster_core esp_http_client esp-tls mbedtls espressif__cjson
    WHOLE_ARCHIVE
)
