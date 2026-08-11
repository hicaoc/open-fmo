idf_component_register(
    SRCS mdc_decode.c mdc_encode.c
    INCLUDE_DIRS .
)

target_compile_options(${COMPONENT_LIB} PRIVATE
    -Wno-unused-function
    -Wno-implicit-fallthrough
)
