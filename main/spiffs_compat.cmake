# Adapt only the filesystem extent in pinned IDF 5.5.5's VFS implementation.
# Generate a build-local copy so SDK sources and dependency lock stay untouched.
idf_component_get_property(ya_spiffs_lib spiffs COMPONENT_LIB)
idf_component_get_property(ya_spiffs_dir spiffs COMPONENT_DIR)
set(ya_spiffs_input "${ya_spiffs_dir}/esp_spiffs.c")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${ya_spiffs_input}")
file(READ "${ya_spiffs_input}" ya_spiffs_source)
set(ya_spiffs_anchor "efs->cfg.phys_size         = partition->size;")
string(FIND "${ya_spiffs_source}" "${ya_spiffs_anchor}" ya_spiffs_match)
if(ya_spiffs_match LESS 0)
  message(FATAL_ERROR "Pinned SPIFFS extent patch no longer matches; review before building")
endif()
string(REPLACE "${ya_spiffs_anchor}"
  "extern u32_t ya_spiffs_extent(const esp_partition_t *, const spiffs_config *);\n    efs->cfg.phys_size = ya_spiffs_extent(partition, &efs->cfg);"
  ya_spiffs_source "${ya_spiffs_source}")
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/esp_spiffs_ya.c" "${ya_spiffs_source}")
get_target_property(ya_spiffs_sources ${ya_spiffs_lib} SOURCES)
list(FILTER ya_spiffs_sources EXCLUDE REGEX "(^|/)esp_spiffs\\.c$")
set_property(TARGET ${ya_spiffs_lib} PROPERTY SOURCES "${ya_spiffs_sources}")
target_sources(${ya_spiffs_lib} PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/esp_spiffs_ya.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/spiffs_extent.c")
