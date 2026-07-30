# SPDX-License-Identifier: MPL-2.0

add_library(ccore_openvpn3 SHARED
    src/ccore_openvpn3.cpp
    src/ccore_openvpn3_runtime.cpp
)

target_include_directories(ccore_openvpn3
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE ${PROJECT_SOURCE_DIR}
)

target_compile_definitions(ccore_openvpn3 PRIVATE
    OPENVPN_CORE_API_VISIBILITY_HIDDEN
    OPENVPN_EXTERNAL_TRANSPORT_FACTORY
)

add_core_dependencies(ccore_openvpn3)

if(NOT MSVC)
    # OpenSSL, LZ4 and fmt are linked statically.  Keep their public symbols
    # private to this engine so the proprietary host sees only our reviewed C
    # ABI and cannot accidentally bind to a bundled dependency implementation.
    target_link_options(ccore_openvpn3 PRIVATE
        "-Wl,--exclude-libs,ALL"
        # Android lld otherwise emits a non-deterministic build ID even when
        # the stripped loadable bytes are identical across clean builds.
        "-Wl,--build-id=none"
        "-Wl,--version-script=${CMAKE_CURRENT_SOURCE_DIR}/exports.map"
    )
endif()

set_target_properties(ccore_openvpn3 PROPERTIES
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN YES
    OUTPUT_NAME ccore_openvpn3
)

install(TARGETS ccore_openvpn3
    LIBRARY DESTINATION lib
    RUNTIME DESTINATION bin
)
install(FILES include/ccore_openvpn3.h DESTINATION include)
