if(CONFIG_PAW3222)
    zephyr_library()
    zephyr_library_sources(src/paw3222.c)
    zephyr_library_sources_ifdef(CONFIG_PAW3222_POINTER_ACCELERATION
                                 src/paw3222_pointer_acceleration.c)
    zephyr_library_include_directories(${CMAKE_SOURCE_DIR}/include)
endif()
