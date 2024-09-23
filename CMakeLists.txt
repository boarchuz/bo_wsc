set(include_dirs
    .
)

set(srcs
   bo_wsc.c
)

set(requires
    esp_wifi
    nvs_flash
)

idf_component_register(
    SRCS ${srcs}
    INCLUDE_DIRS ${include_dirs}
    REQUIRES ${requires}
)
