# app_yapp — the "Yapp" direct-Todoist app component.
# REQUIRES: taskmaster_core (stable app API) + esp_http_client/esp-tls/mbedtls
# (HTTPS + esp_crt_bundle) + espressif__cjson (parse). Core is local in the build.
# WHOLE_ARCHIVE keeps the self-registration constructors from --gc-sections.
idf_component_register(
    SRCS "app_yapp.c" "tasks.c"
    REQUIRES taskmaster_core esp_http_client esp-tls mbedtls espressif__cjson
    WHOLE_ARCHIVE
)
