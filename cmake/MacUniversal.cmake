# Universal (arm64 + x86_64) macOS builds. Included from the root
# CMakeLists.txt; does nothing unless CMAKE_OSX_ARCHITECTURES lists more than
# one architecture, so single-arch builds are unaffected.
#
# C++/ObjC++ targets go universal through CMAKE_OSX_ARCHITECTURES alone, but
# two inputs don't:
#   - Corrosion (vendored v0.5) builds exactly one Rust_CARGO_TARGET. The
#     second architecture's Rust archive is built by a custom target below and
#     lipo'd together with the first.
#   - libopus comes from Homebrew or the system, thin for the build machine.
#     It is built from source per architecture and lipo'd into one dylib.

set(TESSERACT_MAC_UNIVERSAL OFF)
if(APPLE AND CMAKE_OSX_ARCHITECTURES)
    list(LENGTH CMAKE_OSX_ARCHITECTURES _mu_arch_count)
    if(_mu_arch_count GREATER 1)
        set(TESSERACT_MAC_UNIVERSAL ON)
    endif()
endif()

if(NOT TESSERACT_MAC_UNIVERSAL)
    return()
endif()

if(CMAKE_CONFIGURATION_TYPES)
    message(FATAL_ERROR "Universal macOS builds need a single-config generator (Ninja or Makefiles)")
endif()

set(_mu_triple_arm64  "aarch64-apple-darwin")
set(_mu_triple_x86_64 "x86_64-apple-darwin")
set(_mu_triples "")
foreach(_arch IN LISTS CMAKE_OSX_ARCHITECTURES)
    if(NOT DEFINED _mu_triple_${_arch})
        message(FATAL_ERROR "Universal macOS build: unsupported architecture '${_arch}' (expected arm64 and x86_64)")
    endif()
    list(APPEND _mu_triples "${_mu_triple_${_arch}}")
endforeach()
if(NOT Rust_CARGO_TARGET IN_LIST _mu_triples)
    message(FATAL_ERROR
        "Universal macOS build: Rust_CARGO_TARGET='${Rust_CARGO_TARGET}' is not one of ${_mu_triples}")
endif()
set(_mu_other_triples ${_mu_triples})
list(REMOVE_ITEM _mu_other_triples "${Rust_CARGO_TARGET}")
list(LENGTH _mu_other_triples _mu_other_count)
if(NOT _mu_other_count EQUAL 1)
    message(FATAL_ERROR "Universal macOS build: expected exactly arm64 and x86_64 in CMAKE_OSX_ARCHITECTURES")
endif()
list(GET _mu_other_triples 0 TESSERACT_MAC_OTHER_TRIPLE)

# ---------------------------------------------------------------------------
# libopus: one build per architecture (opus's SIMD feature probes don't work
# in a multi-arch compile), lipo'd into build/opus/universal/lib. Sets
# OPUS_LIB so the find_library() calls in the root and ui/macos
# CMakeLists.txt, and the PKG_CONFIG_PATH derived from it, pick it up as is.
# Call before the first find_library(OPUS_LIB ...).
# ---------------------------------------------------------------------------
function(tesseract_mac_universal_opus)
    set(opus_tag "v1.5.2")
    set(root "${CMAKE_BINARY_DIR}/opus")
    set(universal "${root}/universal")
    set(stamp_file "${root}/universal.stamp")
    set(stamp "${opus_tag};${CMAKE_OSX_ARCHITECTURES};${CMAKE_OSX_DEPLOYMENT_TARGET};${CMAKE_OSX_SYSROOT};${CMAKE_C_COMPILER}")

    set(up_to_date FALSE)
    if(EXISTS "${stamp_file}" AND EXISTS "${universal}/lib/libopus.dylib"
       AND EXISTS "${universal}/lib/pkgconfig/opus.pc")
        file(READ "${stamp_file}" old_stamp)
        if(old_stamp STREQUAL stamp)
            set(up_to_date TRUE)
        endif()
    endif()

    if(NOT up_to_date)
        # Download only: SOURCE_SUBDIR points at a directory with no
        # CMakeLists.txt, so MakeAvailable doesn't add opus to this project.
        include(FetchContent)
        FetchContent_Declare(opus
            GIT_REPOSITORY https://github.com/xiph/opus.git
            GIT_TAG        ${opus_tag}
            GIT_SHALLOW    TRUE
            SOURCE_SUBDIR  tesseract-no-cmake)
        FetchContent_MakeAvailable(opus)

        set(extra_args "")
        if(CMAKE_OSX_SYSROOT)
            list(APPEND extra_args "-DCMAKE_OSX_SYSROOT=${CMAKE_OSX_SYSROOT}")
        endif()
        if(CMAKE_MAKE_PROGRAM)
            list(APPEND extra_args "-DCMAKE_MAKE_PROGRAM=${CMAKE_MAKE_PROGRAM}")
        endif()
        # The root project enables only CXX/OBJCXX, so CMAKE_C_COMPILER is
        # usually empty; opus then picks the default cc (Apple Clang).
        if(CMAKE_C_COMPILER)
            list(APPEND extra_args "-DCMAKE_C_COMPILER=${CMAKE_C_COMPILER}")
        endif()

        file(REMOVE_RECURSE "${universal}")
        foreach(arch IN LISTS CMAKE_OSX_ARCHITECTURES)
            set(bdir "${root}/${arch}/build")
            set(idir "${root}/${arch}/install")
            file(REMOVE_RECURSE "${root}/${arch}")
            message(STATUS "Building libopus ${opus_tag} for ${arch}")
            execute_process(
                COMMAND ${CMAKE_COMMAND} -S "${opus_SOURCE_DIR}" -B "${bdir}"
                        -G "${CMAKE_GENERATOR}"
                        -DCMAKE_BUILD_TYPE=Release
                        -DCMAKE_INSTALL_PREFIX=${idir}
                        -DCMAKE_OSX_ARCHITECTURES=${arch}
                        -DCMAKE_OSX_DEPLOYMENT_TARGET=${CMAKE_OSX_DEPLOYMENT_TARGET}
                        -DCMAKE_POLICY_VERSION_MINIMUM=3.5
                        -DOPUS_BUILD_SHARED_LIBRARY=ON
                        -DOPUS_BUILD_PROGRAMS=OFF
                        -DOPUS_BUILD_TESTING=OFF
                        ${extra_args}
                COMMAND_ERROR_IS_FATAL ANY)
            execute_process(
                COMMAND ${CMAKE_COMMAND} --build "${bdir}"
                COMMAND_ERROR_IS_FATAL ANY)
            execute_process(
                COMMAND ${CMAKE_COMMAND} --install "${bdir}"
                COMMAND_ERROR_IS_FATAL ANY)
        endforeach()

        # Assemble the universal tree from the first architecture's layout:
        # lipo each real dylib, recreate each symlink (libopus.dylib ->
        # libopus.0.dylib -> libopus.0.10.1.dylib), copy headers.
        list(GET CMAKE_OSX_ARCHITECTURES 0 first_arch)
        set(first_lib "${root}/${first_arch}/install/lib")
        file(MAKE_DIRECTORY "${universal}/lib/pkgconfig")
        file(GLOB dylibs RELATIVE "${first_lib}" "${first_lib}/libopus*.dylib")
        foreach(name IN LISTS dylibs)
            if(IS_SYMLINK "${first_lib}/${name}")
                file(READ_SYMLINK "${first_lib}/${name}" link_target)
                file(CREATE_LINK "${link_target}" "${universal}/lib/${name}" SYMBOLIC)
            else()
                set(inputs "")
                foreach(arch IN LISTS CMAKE_OSX_ARCHITECTURES)
                    list(APPEND inputs "${root}/${arch}/install/lib/${name}")
                endforeach()
                execute_process(
                    COMMAND lipo -create ${inputs} -output "${universal}/lib/${name}"
                    COMMAND_ERROR_IS_FATAL ANY)
            endif()
        endforeach()
        file(COPY "${root}/${first_arch}/install/include" DESTINATION "${universal}")

        file(READ "${root}/${first_arch}/install/lib/pkgconfig/opus.pc" pc)
        string(REPLACE "${root}/${first_arch}/install" "${universal}" pc "${pc}")
        file(WRITE "${universal}/lib/pkgconfig/opus.pc" "${pc}")

        file(WRITE "${stamp_file}" "${stamp}")
    endif()

    set(OPUS_LIB "${universal}/lib/libopus.dylib" CACHE FILEPATH
        "Universal libopus built by cmake/MacUniversal.cmake" FORCE)
endfunction()

# ---------------------------------------------------------------------------
# Second Rust slice. Mirrors the cargo invocation Corrosion generates for
# tesseract_sdk_ffi (same env, features, rustflags, target dir) with the other
# architecture's --target, then lipo's the two archives into the file
# Corrosion publishes as tesseract_sdk_ffi-static's IMPORTED_LOCATION. Call
# after corrosion_import_crate and after the `ar d` POST_BUILD on
# cargo-build_tesseract_sdk_ffi (POST_BUILD commands run in declaration order).
# ---------------------------------------------------------------------------
function(tesseract_mac_universal_rust_slice)
    set(triple "${TESSERACT_MAC_OTHER_TRIPLE}")
    string(REPLACE "-" "_" triple_us "${triple}")

    get_target_property(custom_profile tesseract_sdk_ffi INTERFACE_CORROSION_CARGO_PROFILE)
    if(custom_profile)
        message(FATAL_ERROR "Universal macOS build: a custom Corrosion cargo profile is not supported")
    endif()
    # Same choice Corrosion makes: --release for everything except Debug.
    if(CMAKE_BUILD_TYPE STREQUAL "Debug" OR NOT CMAKE_BUILD_TYPE)
        set(profile_flag "")
        set(profile_dir "debug")
    else()
        set(profile_flag "--release")
        set(profile_dir "release")
    endif()

    set(tgt tesseract_sdk_ffi)
    set(env_genex      "$<GENEX_EVAL:$<TARGET_PROPERTY:${tgt},${_CORR_PROP_ENV_VARS}>>")
    set(features       "$<GENEX_EVAL:$<TARGET_PROPERTY:${tgt},${_CORR_PROP_FEATURES}>>")
    set(features_genex "$<$<BOOL:${features}>:--features=$<JOIN:${features},$<COMMA>>>")
    set(all_features   "$<GENEX_EVAL:$<TARGET_PROPERTY:${tgt},${_CORR_PROP_ALL_FEATURES}>>")
    set(all_features_genex "$<$<BOOL:${all_features}>:--all-features>")
    set(no_default     "$<GENEX_EVAL:$<TARGET_PROPERTY:${tgt},${_CORR_PROP_NO_DEFAULT_FEATURES}>>")
    set(no_default_genex "$<$<BOOL:${no_default}>:--no-default-features>")
    set(global_rf      "$<TARGET_GENEX_EVAL:${tgt},$<TARGET_PROPERTY:${tgt},INTERFACE_CORROSION_RUSTFLAGS>>")
    set(global_rf_genex "$<$<BOOL:${global_rf}>:RUSTFLAGS=$<JOIN:${global_rf}, >>")
    set(local_rf       "$<TARGET_GENEX_EVAL:${tgt},$<TARGET_PROPERTY:${tgt},INTERFACE_CORROSION_LOCAL_RUSTFLAGS>>")
    set(local_rf_delim "$<$<BOOL:${local_rf}>:-->")
    set(local_rf_genex "$<$<BOOL:${local_rf}>:${local_rf}>")
    set(flags_genex    "$<GENEX_EVAL:$<TARGET_PROPERTY:${tgt},INTERFACE_CORROSION_CARGO_FLAGS>>")

    # Same guards as Corrosion: CC_<triple> only when C is enabled (the root
    # project enables only CXX/OBJCXX), SDKROOT only when a sysroot is set.
    set(cc_env "")
    if(CMAKE_C_COMPILER)
        list(APPEND cc_env "CC_${triple_us}=${CMAKE_C_COMPILER}")
    endif()
    if(CMAKE_OSX_SYSROOT)
        list(APPEND cc_env "SDKROOT=${CMAKE_OSX_SYSROOT}")
    endif()

    set(target_dir "${CMAKE_BINARY_DIR}/cargo/build")
    set(other_archive "${target_dir}/${triple}/${profile_dir}/libtesseract_sdk_ffi.a")

    add_custom_target(cargo-build_tesseract_sdk_ffi_${triple_us}
        COMMAND ${CMAKE_COMMAND} -E env
            "${env_genex}"
            "${global_rf_genex}"
            "CXX_${triple_us}=${CMAKE_CXX_COMPILER}"
            "AR_${triple_us}=${CMAKE_AR}"
            ${cc_env}
            "CORROSION_BUILD_DIR=${CMAKE_BINARY_DIR}"
            "CARGO_BUILD_RUSTC=${_CORROSION_RUSTC}"
            "${_CORROSION_CARGO}" rustc
                --lib
                --target=${triple}
                ${all_features_genex}
                ${no_default_genex}
                ${features_genex}
                --package tesseract-sdk-ffi
                --manifest-path "${CMAKE_SOURCE_DIR}/Cargo.toml"
                --target-dir "${target_dir}"
                ${profile_flag}
                ${flags_genex}
                ${local_rf_delim}
                ${local_rf_genex}
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        COMMENT "Building tesseract_sdk_ffi for ${triple}"
        USES_TERMINAL
        COMMAND_EXPAND_LISTS
        VERBATIM)
    # Both cargo runs share one target dir; run them one after the other.
    add_dependencies(cargo-build_tesseract_sdk_ffi_${triple_us} _cargo-build_tesseract_sdk_ffi)
    add_dependencies(cargo-build_tesseract_sdk_ffi cargo-build_tesseract_sdk_ffi_${triple_us})

    # Registered after the `ar d` POST_BUILD, so the primary archive is already
    # stripped here; strip the copy of the other slice the same way, then
    # lipo. Corrosion's copy_if_different re-copies the thin primary over the
    # fat file on every build (the contents differ), so this is idempotent.
    set(thin_copy "${CMAKE_BINARY_DIR}/${triple}/libtesseract_sdk_ffi.a")
    set(primary "$<TARGET_PROPERTY:tesseract_sdk_ffi-static,IMPORTED_LOCATION>")
    add_custom_command(
        TARGET cargo-build_tesseract_sdk_ffi
        POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/${triple}"
        COMMAND ${CMAKE_COMMAND} -E copy "${other_archive}" "${thin_copy}"
        COMMAND sh -c "${CMAKE_AR} d '${thin_copy}' cppgen_plugin.o protozero_plugin.o main.o || true"
        COMMAND lipo -create "${primary}" "${thin_copy}" -output "${thin_copy}.fat"
        COMMAND ${CMAKE_COMMAND} -E rename "${thin_copy}.fat" "${primary}"
        COMMENT "Merging the arm64 and x86_64 libtesseract_sdk_ffi.a slices"
        VERBATIM)
endfunction()
