cmake_minimum_required(VERSION 3.25)

add_executable(tesseract_win32 WIN32
    src/main.cpp
    src/MainWindow.cpp
    src/LoginDialog.cpp
)

target_compile_features(tesseract_win32 PRIVATE cxx_std_20)

target_include_directories(tesseract_win32 PRIVATE src)

target_link_libraries(tesseract_win32 PRIVATE
    # The two C++ archives have a circular dependency (client ↔ bridge), so they
    # use WHOLE_ARCHIVE to guarantee all symbols are present regardless of order.
    # The Rust staticlib links normally — WHOLE_ARCHIVE would duplicate Windows
    # import-descriptor stubs (__IMPORT_DESCRIPTOR_kernel32, etc.) across crates.
    $<LINK_LIBRARY:WHOLE_ARCHIVE,tesseract_sdk_bridge_cxx,tesseract_client>
    tesseract_sdk_ffi-static
    comctl32
    gdiplus
    ole32
)

target_compile_definitions(tesseract_win32 PRIVATE
    UNICODE _UNICODE
    WIN32_LEAN_AND_MEAN NOMINMAX)


set_target_properties(tesseract_win32 PROPERTIES
    OUTPUT_NAME "Tesseract")
