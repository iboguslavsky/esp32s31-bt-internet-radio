file(GLOB HELIX_AAC_SRCS "src/*.c" "src/utils/*.c")

idf_component_register(
    SRCS ${HELIX_AAC_SRCS}
    INCLUDE_DIRS "src"
    PRIV_REQUIRES heap
)

target_compile_definitions(${COMPONENT_LIB} PRIVATE USE_DEFAULT_STDLIB)
target_compile_options(${COMPONENT_LIB} PRIVATE -O3 -Wno-array-bounds -Wno-unused-but-set-variable -Wno-maybe-uninitialized -Wno-implicit-fallthrough)
