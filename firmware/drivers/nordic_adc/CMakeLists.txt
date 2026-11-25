# Include source directory
# Already included /include in the upper level
zephyr_library_sources_ifdef(CONFIG_NORDIC_ADC nordic_adc.c)