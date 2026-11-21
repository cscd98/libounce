# SPDX-License-Identifier: MIT
#
# Call after creating the core's shared-library target.
enable_language(ASM)

function(libounce_add_module target)
  cmake_parse_arguments(PARSE_ARGV 1 MODULE "" "OUTPUT_NAME" "")
  if(MODULE_UNPARSED_ARGUMENTS OR MODULE_KEYWORDS_MISSING_VALUES)
    message(FATAL_ERROR "Invalid arguments to libounce_add_module")
  endif()

  set(module_output "${target}")
  if(DEFINED MODULE_OUTPUT_NAME)
    set(module_output "${MODULE_OUTPUT_NAME}")
    set_target_properties(${target} PROPERTIES OUTPUT_NAME "${module_output}")
  endif()

  if(NOT LIBOUNCE_BUILD_MODULES)
    message(FATAL_ERROR "Use the libounce/cmake/Switch2Module.cmake toolchain for NRO modules")
  endif()
  set(module_dir "${CMAKE_CURRENT_FUNCTION_LIST_DIR}")
  target_sources(${target} PRIVATE "${module_dir}/lib/module-start.o")
  target_compile_options(${target} PRIVATE -fPIC -mtls-dialect=trad -ftls-model=global-dynamic)
  target_compile_options(${target} PRIVATE "$<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions;-fno-rtti;-fuse-cxa-atexit>")
  target_link_options(${target} PRIVATE
    -nostdlib -Wl,-Bsymbolic,--hash-style=sysv,-z,text
    "-Wl,-T,$ENV{DEVKITPRO}/libounce/switch.ld"
    "-Wl,-T,${module_dir}/module/module-tls.ld")
  target_link_libraries(${target} PRIVATE
    -Wl,--whole-archive "${module_dir}/lib/libounce_module.a" -Wl,--no-whole-archive gcc)
  set_target_properties(${target} PROPERTIES PREFIX "" SUFFIX ".elf")
  set_property(TARGET ${target} APPEND PROPERTY LINK_DEPENDS
    "${module_dir}/module/module-tls.ld" "${module_dir}/lib/libounce_module.a"
    "$ENV{DEVKITPRO}/libounce/switch.ld")
  add_custom_command(TARGET ${target} POST_BUILD
    COMMAND "$ENV{DEVKITPRO}/tools/bin/elf2nro"
      "$<TARGET_FILE:${target}>" "$<TARGET_FILE_DIR:${target}>/${module_output}.nro"
    BYPRODUCTS "${CMAKE_BINARY_DIR}/${module_output}.nro"
    VERBATIM)
endfunction()
