# libopus for the macOS AppKit build. Included from the root CMakeLists.txt.
#
# audiopus_sys links libopus dynamically and the app bundle ships that dylib
# in Contents/Frameworks. dyld refuses a library whose minimum macOS is newer
# than the running system, and Homebrew's opus bottle targets the build
# machine's own macOS, so a bundle built against it won't launch on anything
# older. tesseract_mac_opus() therefore builds opus from source for
# CMAKE_OSX_DEPLOYMENT_TARGET, one build per architecture (opus's SIMD feature
# probes don't work in a multi-arch compile), lipo'd into
# build/opus/universal/lib (thin when only one architecture is configured).
#
# Single-arch builds can opt out by exporting OPUS_LIB_DIR to a directory
# holding a libopus.dylib (and pkgconfig/opus.pc): CI does this, and the
# deployment-target check in ui/macos/CMakeLists.txt still applies to it.
# Universal builds always build opus here, since an external opus is thin.
#
# Sets OPUS_LIB so the find_library() calls in the root and ui/macos
# CMakeLists.txt, and the PKG_CONFIG_PATH derived from it, pick it up as is.
# Call before the first find_library(OPUS_LIB ...).

function(tesseract_mac_opus)
    list(LENGTH CMAKE_OSX_ARCHITECTURES arch_count)
    if(DEFINED ENV{OPUS_LIB_DIR} AND NOT "$ENV{OPUS_LIB_DIR}" STREQUAL "" AND arch_count LESS_EQUAL 1)
        # Drop a cached OPUS_LIB from an earlier source build (or another
        # location) so find_library() searches OPUS_LIB_DIR afresh.
        file(REAL_PATH "$ENV{OPUS_LIB_DIR}" ext_dir)
        if(OPUS_LIB)
            get_filename_component(cached_dir "${OPUS_LIB}" DIRECTORY)
            file(REAL_PATH "${cached_dir}" cached_dir)
            if(NOT cached_dir STREQUAL ext_dir)
                unset(OPUS_LIB CACHE)
            endif()
        endif()
        message(STATUS "libopus: using OPUS_LIB_DIR=$ENV{OPUS_LIB_DIR}")
        return()
    endif()

    set(archs ${CMAKE_OSX_ARCHITECTURES})
    if(NOT archs)
        set(archs ${CMAKE_HOST_SYSTEM_PROCESSOR})
    endif()

    set(opus_tag "v1.5.2")
    set(root "${CMAKE_BINARY_DIR}/opus")
    set(universal "${root}/universal")
    set(stamp_file "${root}/universal.stamp")
    set(stamp "${opus_tag};${archs};${CMAKE_OSX_DEPLOYMENT_TARGET};${CMAKE_OSX_SYSROOT};${CMAKE_C_COMPILER}")

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
        foreach(arch IN LISTS archs)
            set(bdir "${root}/${arch}/build")
            set(idir "${root}/${arch}/install")
            file(REMOVE_RECURSE "${root}/${arch}")
            message(STATUS "Building libopus ${opus_tag} for ${arch} (macOS ${CMAKE_OSX_DEPLOYMENT_TARGET})")
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

        # Assemble the output tree from the first architecture's layout:
        # lipo each real dylib, recreate each symlink (libopus.dylib ->
        # libopus.0.dylib -> libopus.0.10.1.dylib), copy headers.
        list(GET archs 0 first_arch)
        set(first_lib "${root}/${first_arch}/install/lib")
        file(MAKE_DIRECTORY "${universal}/lib/pkgconfig")
        file(GLOB dylibs RELATIVE "${first_lib}" "${first_lib}/libopus*.dylib")
        foreach(name IN LISTS dylibs)
            if(IS_SYMLINK "${first_lib}/${name}")
                file(READ_SYMLINK "${first_lib}/${name}" link_target)
                file(CREATE_LINK "${link_target}" "${universal}/lib/${name}" SYMBOLIC)
            else()
                set(inputs "")
                foreach(arch IN LISTS archs)
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
        "libopus built for the deployment target by cmake/MacOpus.cmake" FORCE)
endfunction()
