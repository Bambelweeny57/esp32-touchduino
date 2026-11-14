FILE(GLOB_RECURSE app_sources ${CMAKE_SOURCE_DIR}/src/*.c ${CMAKE_SOURCE_DIR}/src/*.cpp)
FILE(GLOB engine_sources ${CMAKE_SOURCE_DIR}/src/engine/*.cpp)

idf_component_register(SRCS ${app_sources}
                       INCLUDE_DIRS ".")

target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-unused-variable -Wno-unused-but-set-variable -Wno-missing-field-initializers)
# Upstream MaxDuino code uses partial switches, implicit fallthrough etc.
set_source_files_properties(${engine_sources} PROPERTIES COMPILE_OPTIONS "-Wno-switch;-Wno-implicit-fallthrough;-Wno-parentheses;-Wno-unused-function;-Wno-sign-compare;-Wno-maybe-uninitialized;-Wno-narrowing;-Wno-bool-operation;-Wno-logical-not-parentheses;-Wno-unused-value;-Wno-misleading-indentation;-Wno-type-limits;-Wno-format")
