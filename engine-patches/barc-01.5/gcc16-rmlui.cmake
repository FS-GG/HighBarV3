# Build-only compatibility for pinned RmlUi under GCC 16.
function(barc_apply_rmlui_gcc16_compat)
  if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND
     CMAKE_CXX_COMPILER_VERSION VERSION_GREATER_EQUAL 16)
    foreach(target_name rmlui_debugger GameHeadless engine-headless)
      if(TARGET ${target_name})
        target_compile_options(${target_name} PRIVATE
          "$<$<COMPILE_LANGUAGE:CXX>:-include;cstdint>")
      endif()
    endforeach()
  endif()
endfunction()

cmake_language(DEFER CALL barc_apply_rmlui_gcc16_compat)
