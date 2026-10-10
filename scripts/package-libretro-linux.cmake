cmake_minimum_required(VERSION 3.24)

if(NOT EXISTS "${CORE}" OR NOT OUTPUT_DIR)
    message(FATAL_ERROR "CORE and OUTPUT_DIR are required")
endif()

file(GET_RUNTIME_DEPENDENCIES
    LIBRARIES "${CORE}"
    RESOLVED_DEPENDENCIES_VAR dependencies
    UNRESOLVED_DEPENDENCIES_VAR missing
    PRE_EXCLUDE_REGEXES "^linux-vdso" "^ld-linux" "^lib(c|m|dl|pthread|rt)\\.so")
if(missing)
    message(FATAL_ERROR "Unresolved dependencies: ${missing}")
endif()

file(REMOVE_RECURSE "${OUTPUT_DIR}")
file(MAKE_DIRECTORY "${OUTPUT_DIR}")
file(COPY "${CORE}" DESTINATION "${OUTPUT_DIR}")
foreach(dependency IN LISTS dependencies)
    file(COPY "${dependency}" DESTINATION "${OUTPUT_DIR}/lib" FOLLOW_SYMLINK_CHAIN)
endforeach()
file(COPY "${CMAKE_CURRENT_LIST_DIR}/../LICENSE"
    "${CMAKE_CURRENT_LIST_DIR}/../LICENSES"
    "${CMAKE_CURRENT_LIST_DIR}/../LIBRETRO.md" DESTINATION "${OUTPUT_DIR}")
find_program(strip_program NAMES llvm-strip strip REQUIRED)
execute_process(COMMAND "${strip_program}" --strip-unneeded
    "${OUTPUT_DIR}/shadps4_libretro.so" COMMAND_ERROR_IS_FATAL ANY)
