foreach(target mvt image inflate pmtiles json style glyphs dem sqlite)
  add_executable(fuzz_${target} ${target}.cpp)
  target_link_libraries(fuzz_${target} PRIVATE mapkern_core)
endforeach()
