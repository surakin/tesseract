cmake_minimum_required(VERSION 3.24)

project(BetterText
    VERSION 0.1.0
    DESCRIPTION "Native Win32 modern rich text input control"
    LANGUAGES CXX)

if(NOT WIN32)
    message(FATAL_ERROR "BetterText currently targets native Win32 on Windows 10+.")
endif()

option(BETTERTEXT_BUILD_DEMO "Build the BetterText Win32 demo application" ON)
option(BETTERTEXT_BUILD_TESTS "Build BetterText tests" ON)

add_library(bettertext STATIC
    src/BetterText.cpp
    src/BetterTextControl.cpp
    src/BetterTextDocument.cpp
    src/BetterTextJson.cpp
)

target_compile_features(bettertext PUBLIC cxx_std_20)
target_compile_definitions(bettertext
    PUBLIC
        UNICODE
        _UNICODE
        WIN32_LEAN_AND_MEAN
        NOMINMAX
        _WIN32_WINNT=0x0A00
)
target_include_directories(bettertext
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_link_libraries(bettertext
    PUBLIC
        d2d1
        d3d11
        dwrite
        dxgi
        windowscodecs
        ole32
        imm32
        uiautomationcore
)

if(BETTERTEXT_BUILD_DEMO)
    add_executable(bettertext_demo WIN32 demo/main.cpp)
    target_compile_features(bettertext_demo PRIVATE cxx_std_20)
    target_link_libraries(bettertext_demo PRIVATE bettertext)

    set(BETTERTEXT_NOTO_EMOJI_FONT
        "${CMAKE_CURRENT_SOURCE_DIR}/third_party/fonts/NotoColorEmoji_WindowsCompatible.ttf")
    set(BETTERTEXT_NOTO_EMOJI_LICENSE
        "${CMAKE_CURRENT_SOURCE_DIR}/third_party/fonts/NotoEmoji-LICENSE")
    if(EXISTS "${BETTERTEXT_NOTO_EMOJI_FONT}")
        add_custom_command(TARGET bettertext_demo POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:bettertext_demo>/fonts"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${BETTERTEXT_NOTO_EMOJI_FONT}"
                "$<TARGET_FILE_DIR:bettertext_demo>/fonts/NotoColorEmoji_WindowsCompatible.ttf"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${BETTERTEXT_NOTO_EMOJI_LICENSE}"
                "$<TARGET_FILE_DIR:bettertext_demo>/fonts/NotoEmoji-LICENSE")
    endif()
endif()

if(BETTERTEXT_BUILD_TESTS)
    enable_testing()
    add_executable(bettertext_tests
        tests/document_tests.cpp
    )
    target_compile_features(bettertext_tests PRIVATE cxx_std_20)
    target_include_directories(bettertext_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(bettertext_tests PRIVATE bettertext)
    add_test(NAME bettertext_tests COMMAND bettertext_tests)
endif()
