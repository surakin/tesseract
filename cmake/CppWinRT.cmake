# C++/WinRT headers live in <WinSDK>/include/<ver>/cppwinrt/ (SDK 10.0.17134+).
# The MSVC toolchain adds <sdk>/winrt/ as an external path for WRL-style
# Windows Runtime headers, but NOT cppwinrt/ — add it explicitly.
function(tesseract_add_cppwinrt_includes target)
    if(MSVC)
        cmake_host_system_information(RESULT _winsdk_root QUERY WINDOWS_REGISTRY
            "HKLM/SOFTWARE/Microsoft/Windows Kits/Installed Roots"
            VALUE "KitsRoot10" VIEW BOTH)
        if(_winsdk_root)
            file(GLOB _cppwinrt_ver_dirs LIST_DIRECTORIES true
                 "${_winsdk_root}include/10.*")
            list(SORT _cppwinrt_ver_dirs ORDER DESCENDING)
            list(GET  _cppwinrt_ver_dirs 0 _cppwinrt_ver_dir)
            get_filename_component(_cppwinrt_ver "${_cppwinrt_ver_dir}" NAME)
            target_include_directories(${target} SYSTEM PRIVATE
                "${_winsdk_root}include/${_cppwinrt_ver}/cppwinrt")
        endif()
    endif()
endfunction()
