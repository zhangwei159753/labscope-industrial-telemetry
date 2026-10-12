# Bridge Trantor's legacy `pg_lib` target to CMake's imported PostgreSQL target.
# vcpkg exposes PostgreSQL_LIBRARIES as `optimized;...;debug;...`, which is
# valid in target_link_libraries() but invalid in INTERFACE_LINK_LIBRARIES.
find_package(PostgreSQL QUIET)

if(PostgreSQL_FOUND)
    set(PG_LIBRARIES "${PostgreSQL_LIBRARIES}")
    set(PG_INCLUDE_DIRS "${PostgreSQL_INCLUDE_DIRS}")

    if(NOT TARGET pg_lib)
        add_library(pg_lib INTERFACE IMPORTED)
        set_target_properties(pg_lib PROPERTIES
            INTERFACE_INCLUDE_DIRECTORIES "${PostgreSQL_INCLUDE_DIRS}"
            INTERFACE_LINK_LIBRARIES PostgreSQL::PostgreSQL
        )
    endif()
endif()

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(pg
    DEFAULT_MSG
    PG_LIBRARIES
    PG_INCLUDE_DIRS
)
mark_as_advanced(PG_INCLUDE_DIRS PG_LIBRARIES)
