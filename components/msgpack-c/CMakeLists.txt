set(BUILD_SHARED_LIBS OFF)
set(MSGPACK_BUILD_EXAMPLES OFF CACHE BOOL "Disable msgpack examples")
set(MSGPACK_BUILD_TESTS OFF CACHE BOOL "Disable msgpack tests")
set(CMAKE_SKIP_INSTALL_RULES ON)
function(export)
    # This empty function will be called instead of the real 'export' command.
endfunction()
idf_component_register(INCLUDE_DIRS "upstream/include")
add_subdirectory(upstream)
target_include_directories(${COMPONENT_LIB} INTERFACE "${CMAKE_CURRENT_BINARY_DIR}/upstream/include")
target_include_directories(${COMPONENT_LIB} INTERFACE "${CMAKE_CURRENT_BINARY_DIR}/upstream/include/msgpack")
target_link_libraries(${COMPONENT_LIB} INTERFACE msgpack-c)

# ---------------------------------------------------------------------------
# Strip the host-only Apple include from the generated msgpack config header.
#
# msgpack's CMakeLists does `configure_file(cmake/sysdep.h.in include/msgpack/sysdep.h)`,
# and cmake evaluates that on the HOST. Configuring on macOS therefore emits an
# unconditional `#include <TargetConditionals.h>` into the generated header - which
# is then compiled by the Xtensa/RISC-V cross compiler, which has no Apple SDK and
# fails with:
#
#     fatal error: 'TargetConditionals.h' file not found
#
# msgpack only used that header for endianness/availability detection, and the ESP32
# target pins those anyway via the MSGPACK_ENDIAN_* defines. Rather than patch the
# vendored submodule (which would be lost on the next `git submodule update`), gated
# the include at generation time here.
#
# Guarded on MSGPACK_HOST_APPLE_SDK so a macOS host with a usable SDK can still opt
# back in; on every other host the block is a no-op.
# ---------------------------------------------------------------------------
set(_msgpack_sysdep "${CMAKE_CURRENT_BINARY_DIR}/upstream/include/msgpack/sysdep.h")
if(EXISTS "${_msgpack_sysdep}")
    file(READ "${_msgpack_sysdep}" _msgpack_sysdep_content)
    string(FIND "${_msgpack_sysdep_content}" "#include <TargetConditionals.h>" _msgpack_tc_pos)
    if(NOT _msgpack_tc_pos EQUAL -1)
        string(REPLACE
            "#include <TargetConditionals.h>"
            "#if defined(__APPLE__) && defined(MSGPACK_HOST_APPLE_SDK)\n#include <TargetConditionals.h>\n#endif"
            _msgpack_sysdep_content
            "${_msgpack_sysdep_content}")
        file(WRITE "${_msgpack_sysdep}" "${_msgpack_sysdep_content}")
        message(STATUS "msgpack-c: gated the host-only TargetConditionals.h include for the cross compiler")
    endif()
endif()
