target_include_directories(${PROJECT_NAME} PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_sources(${PROJECT_NAME} PRIVATE
        worker_tasks.c        
        thermostat_task.c
        thermostat_aht10.c
        thermostat_metrics.c
        thermostat_physical_ui.c
        thermostat_web_ui.c
        thermostat_hvac.c
        powerwall.c
        json_parser.c
        tm1637.c
        ssi.c
        cgi.c        
)

set(WEB_FILES)
file(GLOB WEB_FILES "${CMAKE_CURRENT_SOURCE_DIR}/html_files/*.shtml")
pico_add_library(pico_httpd_content NOFLAG)
pico_set_lwip_httpd_content(pico_httpd_content INTERFACE ${WEB_FILES})

# create "generated" directory
file(MAKE_DIRECTORY ${CMAKE_CURRENT_LIST_DIR}/generated)

# create pio header file for controlling seven segment display 
add_custom_command(OUTPUT ${CMAKE_CURRENT_LIST_DIR}/generated/tm1637.py
        DEPENDS ${CMAKE_CURRENT_LIST_DIR}/tm1637.pio
        COMMAND Pioasm -o python ${CMAKE_CURRENT_LIST_DIR}/tm1637.pio ${CMAKE_CURRENT_LIST_DIR}/generated/tm1637.py
        VERBATIM) 

pico_generate_pio_header(thermostat ${CMAKE_CURRENT_LIST_DIR}/tm1637.pio OUTPUT_DIR ${CMAKE_CURRENT_LIST_DIR}/generated)
