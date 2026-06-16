idf_component_register(
    SRCS
        "ambient_led_strip.cc"
    INCLUDE_DIRS
        "include"
    EMBED_TXTFILES
    REQUIRES
    	"esp_timer" "esp_driver_gpio" 
)
