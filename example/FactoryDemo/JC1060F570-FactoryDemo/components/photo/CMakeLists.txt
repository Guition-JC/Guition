idf_component_register(
    SRCS
        "photo_catalog.c"
        "photo_source.c"
    INCLUDE_DIRS
        "include"
    # The catalog rules are pure maths; the source only enumerates directory
    # entries. Neither needs a service, so this component requires nothing.
)
