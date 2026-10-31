idf_component_register(
    SRCS
        "desktop_language.c"
    INCLUDE_DIRS
        "include"
    # Pure C: the catalog, the selection and the lookups need no ESP-IDF service.
)
