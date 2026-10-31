add_executable(mapkern_render main.cpp)
target_link_libraries(mapkern_render PRIVATE mapkern)
target_include_directories(mapkern_render PRIVATE ${PROJECT_SOURCE_DIR}/third_party)
set_target_properties(mapkern_render PROPERTIES OUTPUT_NAME mapkern-render)
