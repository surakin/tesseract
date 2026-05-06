cmake_minimum_required(VERSION 3.25)

# ---------------------------------------------------------------------------
# matrix_client – common C++20 static library
# ---------------------------------------------------------------------------
add_library(matrix_client STATIC
    src/client.cpp
    src/event_handler_bridge.cpp
)

target_compile_features(matrix_client PUBLIC cxx_std_20)

target_include_directories(matrix_client
    PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
)

# Link against the Rust static library produced by Corrosion.
# The cxx bridge (matrix-sdk-ffi-bridge) is compiled as a separate object
# archive by the Rust build script and made available via corrosion_add_cxxbridge.
target_link_libraries(matrix_client
    PUBLIC
        matrix-sdk-ffi          # Corrosion target (Rust)
        matrix_sdk_bridge_cxx   # cxx generated C++ glue (from corrosion_add_cxxbridge)
)

# Platform link requirements for the Rust static lib.
if(WIN32)
    target_link_libraries(matrix_client PUBLIC
        Ws2_32 Bcrypt Userenv Ntdll)
elseif(UNIX)
    target_link_libraries(matrix_client PUBLIC
        pthread dl)
endif()
