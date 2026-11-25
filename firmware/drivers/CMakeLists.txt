zephyr_include_directories(${PROJECT_SOURCE_DIR}/include/drivers)
add_subdirectory_ifdef(CONFIG_MAX32664    max32664)
add_subdirectory_ifdef(CONFIG_KX132       kx132)
add_subdirectory_ifdef(CONFIG_NORDIC_ADC  nordic_adc)
