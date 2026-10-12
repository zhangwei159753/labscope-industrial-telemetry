# Use explicit library paths on Windows hosts where CMake's registry probe fails.
# Other hosts use CMake's standard FindZLIB module.
if(LAB_ZLIB_INCLUDE_DIR AND LAB_ZLIB_LIBRARY)
    set(ZLIB_FOUND TRUE)
    set(ZLIB_INCLUDE_DIR "${LAB_ZLIB_INCLUDE_DIR}")
    set(ZLIB_INCLUDE_DIRS "${LAB_ZLIB_INCLUDE_DIR}")
    set(ZLIB_LIBRARY "${LAB_ZLIB_LIBRARY}")
    set(ZLIB_LIBRARIES "${LAB_ZLIB_LIBRARY}")
    if(NOT TARGET ZLIB::ZLIB)
        add_library(ZLIB::ZLIB UNKNOWN IMPORTED)
        set_target_properties(ZLIB::ZLIB PROPERTIES
            IMPORTED_LOCATION "${LAB_ZLIB_LIBRARY}"
            INTERFACE_INCLUDE_DIRECTORIES "${LAB_ZLIB_INCLUDE_DIR}")
    endif()
else()
    # vcpkg and other package managers may provide a config package. Prefer it
    # before CMake's Windows registry probe, which is unavailable in restricted
    # environments.
    find_package(ZLIB CONFIG QUIET NO_MODULE)
    if(ZLIB_FOUND)
        return()
    endif()
    include("${CMAKE_ROOT}/Modules/FindZLIB.cmake")
endif()
