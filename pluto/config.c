/**
 * Copyright (c) 2024 NewmanIsTheStar
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */
#include "pico/cyw43_arch.h"
#include "pico/stdlib.h"
#include "pico/flash.h"
#include <hardware/flash.h>

#include "lwip/sockets.h"


#include "FreeRTOS.h"
#include "FreeRTOSConfig.h"
#include "task.h"

#include "config.h"
#include "pluto.h"
#include "utility.h"

#include "flash.h"

//#define FLASH_TARGET_OFFSET (PICO_FLASH_SIZE_BYTES - FLASH_SECTOR_SIZE)
//#define DISABLE_CONFIG_VALIDATION (1)
//#define DISABLE_CONFIG_UPGRADE (1)
//#define DISABLE_CONFIG_WRITE [1]

bool config_compare_flash_ram(bool stop_at_first_difference);
int config_validate(void);
void config_system_variable_initialize(void);
void config_system_to_v1(void *previous_config);
void config_blank_to_v1(void *previous_config);
void config_v1_to_v2(void *previous_config);
void config_v2_to_v3(void *previous_config);
void config_v3_to_v4(void *previous_config);
void config_v4_to_v5(void *previous_config);
void config_v5_to_v6(void *previous_config);
void config_v6_to_v7(void *previous_config);
void config_v7_to_v8(void *previous_config);
void config_v8_to_v9(void *previous_config);
void config_v9_to_v10(void *previous_config);
void config_v10_to_v11(void *previous_config);
void config_v11_to_v12(void *previous_config);

NON_VOL_VARIABLES_T config;
static int config_dirty_flag = 0;
static NON_VOL_CONVERSION_T config_info[] =
{
    {1,      offsetof(NON_VOL_VARIABLES_T_VERSION_1, version),   offsetof(NON_VOL_VARIABLES_T_VERSION_1, crc),   &config_blank_to_v1},
    {2,      offsetof(NON_VOL_VARIABLES_T_VERSION_2, version),   offsetof(NON_VOL_VARIABLES_T_VERSION_2, crc),   &config_v1_to_v2}, 
    {3,      offsetof(NON_VOL_VARIABLES_T_VERSION_3, version),   offsetof(NON_VOL_VARIABLES_T_VERSION_3, crc),   &config_v2_to_v3}, 
    {4,      offsetof(NON_VOL_VARIABLES_T_VERSION_4, version),   offsetof(NON_VOL_VARIABLES_T_VERSION_4, crc),   &config_v3_to_v4},  
    {5,      offsetof(NON_VOL_VARIABLES_T_VERSION_5, version),   offsetof(NON_VOL_VARIABLES_T_VERSION_5, crc),   &config_v4_to_v5}, 
    {6,      offsetof(NON_VOL_VARIABLES_T_VERSION_6, version),   offsetof(NON_VOL_VARIABLES_T_VERSION_6, crc),   &config_v5_to_v6},   
    {7,      offsetof(NON_VOL_VARIABLES_T_VERSION_7, version),   offsetof(NON_VOL_VARIABLES_T_VERSION_7, crc),   &config_v6_to_v7},   
    {8,      offsetof(NON_VOL_VARIABLES_T_VERSION_8, version),   offsetof(NON_VOL_VARIABLES_T_VERSION_8, crc),   &config_v7_to_v8},  
    {9,      offsetof(NON_VOL_VARIABLES_T_VERSION_9, version),   offsetof(NON_VOL_VARIABLES_T_VERSION_9, crc),   &config_v8_to_v9},    
    {10,     offsetof(NON_VOL_VARIABLES_T_VERSION_10, version),  offsetof(NON_VOL_VARIABLES_T_VERSION_10, crc),  &config_v9_to_v10},   
    {11,     offsetof(NON_VOL_VARIABLES_T_VERSION_11, version),  offsetof(NON_VOL_VARIABLES_T_VERSION_11, crc),  &config_v10_to_v11},
    {12,     offsetof(NON_VOL_VARIABLES_T, version),             offsetof(NON_VOL_VARIABLES_T, crc),             &config_v11_to_v12},                                
};



/*!
 * \brief Set default values for configuration v1
 * 
 * \return 0 on success, -1 on error
 */
void config_blank_to_v1(void *previous_config)
{
    int i;

    printf("Initializing configuration version 1\n");

    // set initial values in ram buffer
    config.version = 1;
    config.personality = SPRINKLER_USURPER;

    config.irrigation_enable = 1;

    for(i=0; i<7; i++)
    {
        config.day_schedule_enable[i] = 1;
        config.day_start[i] = 0;
        config.day_duration[i] = 0;
        config.day_start_alternate[i] = 0;
        config.day_duration_alternate[i] = 0; 
    }

    for(i=0; i<32; i++)
    {
        config.schedule_opportunity_start[i] = i;
        config.schedule_opportunity_duration[i] = i;
    }

    config.timezone_offset = -6*60;
    config.daylightsaving_enable = 1;
    STRNCPY(config.wifi_country, "World Wide", sizeof(config.wifi_country));    
    STRNCPY(config.daylightsaving_start, "Second Sunday in March", sizeof(config.daylightsaving_start));
    STRNCPY(config.daylightsaving_end, "First Sunday in November", sizeof(config.daylightsaving_end));
    STRNCPY(config.time_server[0], "pool.ntp.org", sizeof(config.time_server[0]));
    STRNCPY(config.time_server[1], "time.google.com", sizeof(config.time_server[1]));
    STRNCPY(config.time_server[2], "time.facebook.com", sizeof(config.time_server[2]));
    STRNCPY(config.time_server[3], "time.windows.com", sizeof(config.time_server[3]));        
    STRNCPY(config.weather_station_ip, "weather-station.badnet", sizeof(config.weather_station_ip)); 
    STRNCPY(config.syslog_server_ip, "spud.badnet", sizeof(config.syslog_server_ip));         
    STRNCPY(config.govee_light_ip, "govee.badnet", sizeof(config.govee_light_ip));     

    // config.syslog_enable = 0;
    
    config.weather_station_enable = 1;
    config.wind_threshold = 15;
    config.rain_week_threshold = 50;
    config.rain_day_threshold = 10;
    config.relay_normally_open = false;
    config.gpio_number = 3;

    // blank network setting with DHCP enabled
    config.wifi_ssid[0] = 0;
    config.wifi_password[0] = 0;
    config.dhcp_enable = 1;
    config.ip_address[0] = 0;
    config.network_mask[0] = 0;

    // led string settings
    config.led_number = 0;
    config.led_pattern = 0;
    config.led_pattern_when_irrigation_active = 0;
    config.led_pattern_when_irrigation_terminated = 0;    
    config.led_speed = 100;
    config.led_rgbw = 0;
    config.led_pin = 7;
    config.led_sustain_duration = 0;

    // moodlight settings
    config.use_govee_to_indicate_irrigation_status = 0;
    config.govee_irrigation_active_red = 50;
    config.govee_irrigation_active_green = 200; 
    config.govee_irrigation_active_blue = 50;    
    config.govee_irrigation_usurped_red = 200;
    config.govee_irrigation_usurped_green = 50;
    config.govee_irrigation_usurped_blue = 50;
    config.govee_sustain_duration = 60;    

    // foibles
    config.use_archaic_units = 1;
    config.use_simplified_english = 1;
    config.use_monday_as_week_start = 0;
    
}

/*!
 * \brief Convert configuration from v1 to v2 and set default values for new parameters
 * 
 * \return 0 on success, -1 on error
 */
void config_v1_to_v2(void *previous_config)
{
    int i = 0;

    printf("Converting configuration from version 1 to version 2\n");
    config.version = 2;
    
    for(i = 0; i < NUM_ROWS(config.soil_moisture_threshold); i++)
    {
        config.soil_moisture_threshold[i] = 70; 
    }
}

/*!
 * \brief Convert configuration from v2 to v3 and set default values for new parameters
 * 
 * \return 0 on success, -1 on error
 */
void config_v2_to_v3(void *previous_config)
{
    int i = 0;
    int j = 0;

    printf("Converting configuration from version 2 to version 3\n"); 
    config.version = 3;     

    config.zone_max = 1;

    for (j=0; j < 16; j++)
    {
        config.zone_gpio[j] = -1;
    }
    config.zone_gpio[0] = config.gpio_number;

    for(i = 0; i < 16; i++)
    {
        sprintf(config.zone_name[i], "Zone %d", j);
        config.zone_enable[i] = 1;

        for (j=0; j < 7; j++)
        {                      
            config.zone_duration[i][j] = 0;
        }
    }

    for (j=0; j < 7; j++)
    {        
        config.zone_duration[0][j] = config.day_duration[j];
    }    
}

/*!
 * \brief Convert configuration from v3 to v4 and set default values for new parameters
 * 
 * \return 0 on success, -1 on error
 */
void config_v3_to_v4(void *previous_config)
{
    int i = 0;
    int j = 0;

    printf("Converting configuration from version 3 to version 4\n"); 
    config.version = 4;     

    config.led_strip_remote_enable = 0;

    for (i=0; i < 6; i++)
    {
        config.led_strip_remote_ip[i][0] = 0;
    }   
}

/*!
 * \brief Convert configuration from v4 to v5 and set default values for new parameters
 * 
 * \return 0 on success, -1 on error
 */
void config_v4_to_v5(void *previous_config)
{
    int i = 0;
    int j = 0;

    printf("Converting configuration from version 4 to version 5\n"); 
    config.version = 5;     

    // v5 is now defunct -- all new parameters will be initialized on upgrade to v6


    // config.thermostat_enable = 0;
    // config.heating_gpio = -1;
    // config.cooling_gpio = -1;
    // config.fan_gpio = -1;
    // config.heating_to_cooling_lockout_mins = 1;
    // config.minimum_heating_on_mins = 1;
    // config.minimum_cooling_on_mins = 1;
    // config.minimum_heating_off_mins = 1;
    // config.minimum_cooling_off_mins = 1;

    // for(i=0; i<NUM_ROWS(config.thermostat_period_end_mow); i++)
    // {
    //     config.setpoint_start_mow[i] = -1;
    //     config.thermostat_period_end_mow[i] = 0;
    //     config.thermostat_period_setpoint_index[i] = 0;
    //     config.thermostat_period_number = i;
    // }

    // for(i=0; i<NUM_ROWS(config.setpoint_name); i++)
    // {
    //     config.setpoint_name[i][0] = 0;
    //     config.setpoint_temperaturex10[i] = 21;
    //     config.setpoint_number = i;

    //     sprintf(config.setpoint_name[i], "Setpoint%d", i);
    // }

    // config.powerwall_ip[0] = 0;
    // STRNCPY(config.powerwall_hostname, "powerwall", sizeof(config.powerwall_hostname));
    // config.powerwall_password[0] = 0;

    // config.grid_down_heating_setpoint_decrease = 10;
    // config.grid_down_cooling_setpoint_increase = 10;
    // config.grid_down_heating_disable_battery_level = 40;
    // config.grid_down_heating_enable_battery_level = 60;
    // config.grid_down_cooling_disable_battery_level = 70;
    // config.grid_down_cooling_enable_battery_level = 90;

    // for(i=0; i<NUM_ROWS(config.gpio_default); i++)
    // {
    //     config.gpio_default[i] = GP_UNINITIALIZED;
    // }
}

/*!
 * \brief Convert configuration from v5 to v6 and set default values for new parameters
 * 
 * \return 0 on success, -1 on error
 */
void config_v5_to_v6(void *previous_config)
{
    int i = 0;
    int j = 0;

    printf("Converting configuration from version 5 to version 6\n"); 
    config.version = 6;     

    for(i=0; i<NUM_ROWS(config.gpio_default); i++)
    {
        config.gpio_default[i] = GP_UNINITIALIZED;
    }

    config.thermostat_enable = 0;
    config.heating_gpio = -1;
    config.cooling_gpio = -1;
    config.fan_gpio = -1;
    config.heating_to_cooling_lockout_mins = 10;
    config.minimum_heating_on_mins = 5;
    config.minimum_cooling_on_mins = 5;
    config.minimum_heating_off_mins = 5;
    config.minimum_cooling_off_mins = 5;
    config.thermostat_mode = 0;
    config.max_cycles_per_hour = 6;

    for(i=0; i<NUM_ROWS(config.setpoint_temperaturex10); i++)
    {
        config.setpoint_start_mow[i] = -1;
        config.setpoint_temperaturex10[i] = 210;
    }

    config.powerwall_ip[0] = 0;
    STRNCPY(config.powerwall_hostname, "powerwall", sizeof(config.powerwall_hostname));
    config.powerwall_password[0] = 0;

    config.grid_down_heating_setpoint_decrease = 10;
    config.grid_down_cooling_setpoint_increase = 10;
    config.grid_down_heating_disable_battery_level = 400;
    config.grid_down_heating_enable_battery_level = 600;
    config.grid_down_cooling_disable_battery_level = 700;
    config.grid_down_cooling_enable_battery_level = 900;
    
    for (i=0; i < 6; i++)
    {
        config.temperature_sensor_remote_ip[i][0] = 0;
    }     

}

/*!
 * \brief Convert configuration from v6 to v7 and set default values for new parameters
 * 
 * \return 0 on success, -1 on error
 */
void config_v6_to_v7(void *previous_config)
{
    int i = 0;
    int j = 0;

    printf("Converting configuration from version 6 to version 7\n"); 
    config.version = 7;     

    config.thermostat_mode_button_gpio = -1;
    config.thermostat_increase_button_gpio = -1;
    config.thermostat_decrease_button_gpio = -1;
    config.thermostat_temperature_sensor_clock_gpio = -1;
    config.thermostat_temperature_sensor_data_gpio = -1;
    config.thermostat_seven_segment_display_clock_gpio = -1;
    config.thermostat_seven_segment_display_data_gpio = -1;   
    
    // TEST TEST TEST
    // config.thermostat_mode_button_gpio = 22;
    // config.thermostat_increase_button_gpio = 16;
    // config.thermostat_decrease_button_gpio = 17;
    // config.thermostat_temperature_sensor_data_gpio = 10;
    // config.thermostat_temperature_sensor_clock_gpio = 11;
    // config.thermostat_seven_segment_display_clock_gpio = 13;
    // config.thermostat_seven_segment_display_data_gpio = 12;      

}

/*!
 * \brief Convert configuration from v7 to v8 and set default values for new parameters
 * 
 * \return 0 on success, -1 on error
 */
void config_v7_to_v8(void *previous_config)
{
    int i = 0;
    int j = 0;

    printf("Converting configuration from version 7 to version 8\n"); 
    config.version = 8;     

    config.outside_temperature_threshold = 50;  

}

/*!
 * \brief Convert configuration from v8 to v9 and set default values for new parameters
 * 
 * \return 0 on success, -1 on error
 */
void config_v8_to_v9(void *previous_config)
{
    printf("Converting configuration from version 8 to version 9\n"); 
    config.version = 9;     

    config.thermostat_display_brightness = 7;  
}

 /*!
 * \brief Convert configuration from v9 to v10 and set default values for new parameters
 * 
 * \return 0 on success, -1 on error
 */
void config_v9_to_v10(void *previous_config)
{
    printf("Converting configuration from version 9 to version 10\n"); 
    config.version = 10;     

    config.thermostat_display_num_digits = 4;  
}

 /*!
 * \brief Convert configuration from v10 to v11 and set default values for new parameters
 * 
 * \return 0 on success, -1 on error
 */
void config_v10_to_v11(void *previous_config)
{
    int i;

    printf("Converting configuration from version 10 to version 11\n"); 
    config.version = 11;     

    for(i=0; i<NUM_ROWS(config.setpoint_heating_temperaturex10); i++)
    {
        config.setpoint_heating_temperaturex10[i] = 210;
        config.setpoint_cooling_temperaturex10[i] = 210;        
    }

    config.thermostat_hysteresis = 10;
}

 /*!
 * \brief Convert configuration from v11 to v12 and set default values for new parameters
 * 
 * \return 0 on success, -1 on error
 */
void config_v11_to_v12(void *previous_config)
{
    int i, j;
    NON_VOL_VARIABLES_T_VERSION_11 *temp_config = NULL;
    NON_VOL_VARIABLES_T_VERSION_11 *config_v11 = NULL;

    printf("Attempting complex conversion. This may take some time.\n");

    if (previous_config)
    {
        if (((NON_VOL_VARIABLES_T_VERSION_11 *)previous_config)->version == 11)
        {
            printf("Previous config version 11 is available in flash\n");
        }
        // this does not work because all the legacy conversion functions acutally work on the latest config structure
        // no conversion is needed if all you have is a RAM copy that was just created
        // else
        // {
        //     printf("Previous config is not available from flash. Allocating temporary RAM buffer.\n");
        //     previous_config = NULL;

        //     // allocate temporary buffer
        //     temp_config = malloc(sizeof(NON_VOL_VARIABLES_T_VERSION_11));
        //     if (temp_config)
        //     {
        //         memcpy(temp_config, &config, sizeof(NON_VOL_VARIABLES_T_VERSION_11));
        //         previous_config = temp_config;
        //     }
        //     else
        //     {
        //         printf("Failed to allocate RAM for conversion to v12.  Conversion in RAM not possible.  Buring flash with v11.\n");
        //         if (!flash_write_non_volatile_variables())
        //         {
        //             previous_config = flash_get_config_location();

        //             if (((NON_VOL_VARIABLES_T_VERSION_11 *)previous_config)->version != 11)
        //             {
        //                 printf("Unexpected version found in flash %d\n", ((NON_VOL_VARIABLES_T_VERSION_11 *)previous_config)->version);
        //                 previous_config = NULL;
        //             }
        //         }
                
        //         if (previous_config == NULL)
        //         {
        //             printf("Unable to perform conversion using either flash or ram\n");
        //         }
        //     }
        // }
    }

    if (previous_config)
    {
        config_v11 = (NON_VOL_VARIABLES_T_VERSION_11 *)previous_config;

        printf("Converting configuration from version 11 to version 12\n"); 
        config.version = 12; 

        // ***system config start ***
        //int version;
        config.personality = config_v11->personality;
        memcpy(config.wifi_ssid, config_v11->wifi_ssid, sizeof(config.wifi_ssid));
        memcpy(config.wifi_password, config_v11->wifi_password, sizeof(config.wifi_password));
        memcpy(config.wifi_country, config_v11->wifi_country, sizeof(config.wifi_country));                
        config.dhcp_enable = config_v11->dhcp_enable;
        //memcpy(config.host_name, config_v11->host_name, sizeof(config.host_name)); 
        STRNCPY(config.host_name, "usurper", sizeof(config.host_name));          
        memcpy(config.ip_address, config_v11->ip_address, sizeof(config.ip_address));   
        memcpy(config.network_mask, config_v11->network_mask, sizeof(config.network_mask));   
        memcpy(config.gateway, config_v11->gateway, sizeof(config.gateway));   
        config.timezone_offset = config_v11->timezone_offset;
        config.daylightsaving_enable = config_v11->daylightsaving_enable;     
        memcpy(config.daylightsaving_start, config_v11->daylightsaving_start, sizeof(config.daylightsaving_start)); 
        memcpy(config.daylightsaving_end, config_v11->daylightsaving_end, sizeof(config.daylightsaving_end)); 
        memcpy(config.time_server, config_v11->time_server, sizeof(config.time_server));                 
        config.syslog_enable = config_v11->syslog_enable; 
        memcpy(config.syslog_server_ip, config_v11->syslog_server_ip, sizeof(config.syslog_server_ip)); 
        config.use_archaic_units = config_v11->use_archaic_units; 
        config.use_simplified_english = config_v11->use_simplified_english; 
        config.use_monday_as_week_start = config_v11->use_monday_as_week_start; 
        memcpy(config.gpio_default, config_v11->gpio_default, sizeof(config.gpio_default));   
        config.mqtt_user[0] = 0;
        config.mqtt_password[0] = 0;        
        config.mqtt_broker_address[0] = 0;

        // ***application config start*** 
        config.thermostat_enable = config_v11->thermostat_enable; 
        config.heating_gpio = config_v11->heating_gpio; 
        config.cooling_gpio = config_v11->cooling_gpio; 
        config.fan_gpio = config_v11->fan_gpio; 
        config.heating_to_cooling_lockout_mins = config_v11->heating_to_cooling_lockout_mins; 
        config.minimum_heating_on_mins = config_v11->minimum_heating_on_mins; 
        config.minimum_cooling_on_mins = config_v11->minimum_cooling_on_mins; 
        config.minimum_heating_off_mins = config_v11->minimum_heating_off_mins; 
        config.minimum_cooling_off_mins = config_v11->minimum_cooling_off_mins; 
        config.thermostat_mode = config_v11->thermostat_mode; 
        config.max_cycles_per_hour = config_v11->max_cycles_per_hour; 
        config.setpoint_number = config_v11->setpoint_number; 
        memcpy(config.setpoint_name, config_v11->setpoint_name, sizeof(config.setpoint_name)); 
        memcpy(config.setpoint_temperaturex10, config_v11->setpoint_temperaturex10, sizeof(config.setpoint_temperaturex10)); 
        config.thermostat_hysteresis = config_v11->thermostat_hysteresis; 
        memcpy(config.setpoint_start_mow, config_v11->setpoint_start_mow, sizeof(config.setpoint_start_mow)); 
        memcpy(config.setpoint_mode, config_v11->setpoint_mode, sizeof(config.setpoint_mode)); 
        memcpy(config.powerwall_ip, config_v11->powerwall_ip, sizeof(config.powerwall_ip)); 
        memcpy(config.powerwall_hostname, config_v11->powerwall_hostname, sizeof(config.powerwall_hostname)); 
        memcpy(config.powerwall_password, config_v11->powerwall_password, sizeof(config.powerwall_password)); 
        config.grid_down_heating_setpoint_decrease = config_v11->grid_down_heating_setpoint_decrease; 
        config.grid_down_cooling_setpoint_increase = config_v11->grid_down_cooling_setpoint_increase; 
        config.grid_down_heating_disable_battery_level = config_v11->grid_down_heating_disable_battery_level; 
        config.grid_down_heating_enable_battery_level = config_v11->grid_down_heating_enable_battery_level; 
        config.grid_down_cooling_disable_battery_level = config_v11->grid_down_cooling_disable_battery_level; 
        config.grid_down_cooling_enable_battery_level = config_v11->grid_down_cooling_enable_battery_level; 
        memcpy(config.temperature_sensor_remote_ip, config_v11->temperature_sensor_remote_ip, sizeof(config.temperature_sensor_remote_ip)); 
        config.thermostat_mode_button_gpio = config_v11->thermostat_mode_button_gpio; 
        config.thermostat_increase_button_gpio = config_v11->thermostat_increase_button_gpio; 
        config.thermostat_decrease_button_gpio = config_v11->thermostat_decrease_button_gpio; 
        config.thermostat_temperature_sensor_clock_gpio = config_v11->thermostat_temperature_sensor_clock_gpio; 
        config.thermostat_temperature_sensor_data_gpio = config_v11->thermostat_temperature_sensor_data_gpio; 
        config.thermostat_seven_segment_display_clock_gpio = config_v11->thermostat_seven_segment_display_clock_gpio; 
        config.thermostat_seven_segment_display_data_gpio = config_v11->thermostat_seven_segment_display_data_gpio; 
        config.outside_temperature_threshold = config_v11->outside_temperature_threshold; 
        config.thermostat_display_brightness = config_v11->thermostat_display_brightness; 
        config.thermostat_display_num_digits = config_v11->thermostat_display_num_digits; 
        memcpy(config.setpoint_heating_temperaturex10, config_v11->setpoint_heating_temperaturex10, sizeof(config.setpoint_heating_temperaturex10)); 
        memcpy(config.setpoint_cooling_temperaturex10, config_v11->setpoint_cooling_temperaturex10, sizeof(config.setpoint_cooling_temperaturex10)); 
    }


    if (temp_config)
    {
      free(temp_config);
    }  
}

// ************************************************************************************************************************
// ************************************************************************************************************************

/*!
 * \brief Record that configuration copy in RAM was altered and may now differ from the flash copy
 */
void config_changed(void)
{
    config_dirty_flag = 1;
}

/*!
 * \brief Check if RAM copy of configuration differs from flash copy.  Optionally clear the dirty flag.
 * 
 * \param[in]    clear_flag Set the dirty flag to false after returning its value
 * 
 * \return true if config in RAM differs from config in flash, otherwise flase
 */
bool config_dirty(bool clear_flag)
{
    int dirty = false;

    if (config_dirty_flag)
    {
        dirty = true;

        if (clear_flag)
        {
            config_dirty_flag = 0;
        }
    }

    return (dirty);
}

/*!
 * \brief Copy the configuation from flash into RAM.  Set default values if flash is corrupt.
 * 
 * \return 0 on success, -1 on error
 */
int config_read(void)
{
    int err = 0;

    // read configuration from flash
    flash_read_non_volatile_variables(CONFIG_STANDARD);

#ifdef DISABLE_CONFIG_VALIDATION
    printf("Configuration validation disabled!  Using whatever random garbage happens to be in flash...\n");
#else
    // check and correct configuration
    config_validate();
#endif

    return(err);
}

/*!
 * \brief Copy the configuration from RAM into flash if they differ.
 * 
 * \return 0 on success, -1 on error
 */
int config_write(void)
{
    int err = 0;

    #ifdef DISABLE_CONFIG_WRITE
    printf("Configuration Writes are disabled!\n");
    #else
    // write configuration to flash if altered recently
    if (config_dirty(true))
    {
        // wait for 5 second period with no config changes
        do 
        {
            SLEEP_MS(5000);
        } while (config_dirty(true));

        // update crc
        config.system_crc = crc_buffer((uint8_t *)&config, offsetof(NON_VOL_VARIABLES_T, system_crc));         
        config.crc = crc_buffer((uint8_t *)&config, offsetof(NON_VOL_VARIABLES_T, crc)); 
         
        // compare ram and flash copies
        if (memcmp((char *)(XIP_BASE +  FLASH_TARGET_OFFSET), ((char *)&config), sizeof(config)))
        {
            printf("Writing configuration to flash\n");

            if (err = flash_write_non_volatile_variables())
            {
                printf("Failed to write configuraiton to flash (%d)\n", err);                
            } 
            else if (config_compare_flash_ram(false))
            {
                flash_dump_config(CONFIG_STANDARD);
            }          
        }           
        else
        {
            printf("Refusing to write configuration to flash as RAM and flash copies are identical\n");
        }

        // check for collision
        if (config.crc != crc_buffer((uint8_t *)&config, offsetof(NON_VOL_VARIABLES_T, crc)))
        {
            // config was updated by another task after we computed the crc and possibly before we wrote to flash
            printf("Config update occured while writing to flash, will retry\n");
            
            config_changed();

            err = -1;
        }          
    }  
    #endif

    return(err);
}


/*!
 * \brief Compare flash and RAM copies of configuration
 * 
 * \return 0 = no difference, 1 = difference
 */
bool config_compare_flash_ram(bool stop_at_first_difference)
{
    NON_VOL_VARIABLES_T *non_vol;
    int i;
    int len;
    uint16_t ram_crc;
    uint16_t flash_crc;    
    bool difference_found = false;

    for (i=0; i<sizeof(config); i++)
    {
        if (((char *)(XIP_BASE +  FLASH_TARGET_OFFSET))[i] != ((char *)&config)[i])
        {
            if (!difference_found)
            {
                // printf headings
                printf("     offset\tflash\tram\n");
            }

            // print difference
            printf("%08x:\t%02x \t%02x\n", i, ((char *)(XIP_BASE +  FLASH_TARGET_OFFSET))[i], ((char *)&config)[i]);
            
            difference_found = true;

            if (stop_at_first_difference)
            {
                break;
            }
        }
    }
    
    return(difference_found);
}

/*!
 * \brief Check configuration is valid and upgrade if necessary 
 * 
 * \return 0 on success, -1 on error
 */
int config_validate(void)
{
    int err = 0;
    int i = 0;
    int version_from_flash = 0;
    uint16_t crc_from_flash = 0;
    uint16_t calculated_crc = 0;
    int latest_valid_config_version = 0;
    void *previous_config = NULL;
    CONFIG_TYPE_T config_type;

    for(config_type=CONFIG_STANDARD; config_type < NUM_CONFIG_TYPES; config_type++)
    {

        // read configuration into RAM
        flash_read_non_volatile_variables(config_type); 

        // check for valid configuration
        for(i=0; i < NUM_ROWS(config_info); i++)
        {
            version_from_flash = *((int *)((uint8_t *)&config + config_info[i].version_offset));
            crc_from_flash = *((uint16_t *)((uint8_t *)&config + config_info[i].crc_offset));
            calculated_crc = crc_buffer((uint8_t *)&config, config_info[i].crc_offset);        

            if ((version_from_flash == config_info[i].version) && (crc_from_flash == calculated_crc))
            {
                printf("Found valid configuration version %d\n", version_from_flash);
                latest_valid_config_version = version_from_flash;
            }
        }

        // check if we found a valid config version
        if (latest_valid_config_version != 0)        
        {
            // we found a valid config so stop searching
            break;
        }
        else
        {
            // no valid config so try to fallback to system config only
            crc_from_flash = *((uint16_t *)((uint8_t *)&config + offsetof(NON_VOL_VARIABLES_T, system_crc)));
            calculated_crc = crc_buffer((uint8_t *)&config, offsetof(NON_VOL_VARIABLES_T, system_crc));

            if(crc_from_flash == calculated_crc)
            {
                printf("Found valid system configuration variables (e.g. network config).  These will be preserved.\n");
                break;
            }
            else
            {
                config_system_variable_initialize();
            }
        }

    }

#ifndef DISABLE_CONFIG_UPGRADE

    // obtain pointer to previous config if available
    if (version_from_flash > 0)
    {
        previous_config = flash_get_config_location(config_type);
    }

    // upgrade configuration sequentially to latest version 
    for(i=0; i < NUM_ROWS(config_info); i++)
    {
        if (latest_valid_config_version < config_info[i].version)
        {
            config_info[i].upgrade_function(previous_config);
        }
    }
#else
    if (latest_valid_config_version < config_info[i].version)
    {
        for (;;)
        {
            printf("BAD CONFIG!\n");
            flash_dump();

            SLEEP_MS(10000);
        }
    }
#endif

    return(err);
}


/*!
 * \brief Set a default time server in config if all four time server entries are blank
 * 
 * \return 0 on success, -1 on error
 */
int config_timeserver_failsafe(void)
{
    // failsafe - if no timeserver configured try pool.ntp.org
    if ((config.time_server[0][0] == 0) &&
        (config.time_server[1][0] == 0) &&
        (config.time_server[2][0] == 0) &&
        (config.time_server[3][0] == 0))
    {
        STRNCPY(config.time_server[0], "pool.ntp.org", sizeof(config.time_server[0]));
    }

    return(0);
}

/*!
 * \brief Set default values for system variables
 * 
 * \return 0 on success, -1 on error
 */
void config_system_variable_initialize(void)
{
    int i;

    printf("Initializing configuration system variables in RAM\n");

    // personality
    config.personality = NO_PERSONALITY;

    // network
    STRNCPY(config.wifi_country, "World Wide", sizeof(config.wifi_country));      
    config.wifi_ssid[0] = 0;
    config.wifi_password[0] = 0;
    config.dhcp_enable = 1;
    STRNCPY(config.host_name, APP_NAME, sizeof(config.host_name));
    config.ip_address[0] = 0;
    config.network_mask[0] = 0;
    
    // time
    config.timezone_offset = -6*60;
    config.daylightsaving_enable = 1;  
    STRNCPY(config.daylightsaving_start, "Second Sunday in March", sizeof(config.daylightsaving_start));
    STRNCPY(config.daylightsaving_end, "First Sunday in November", sizeof(config.daylightsaving_end));
    STRNCPY(config.time_server[0], "pool.ntp.org", sizeof(config.time_server[0]));
    STRNCPY(config.time_server[1], "time.google.com", sizeof(config.time_server[1]));
    STRNCPY(config.time_server[2], "time.facebook.com", sizeof(config.time_server[2]));
    STRNCPY(config.time_server[3], "time.windows.com", sizeof(config.time_server[3]));        

    // syslog
    STRNCPY(config.syslog_server_ip, "spud.badnet", sizeof(config.syslog_server_ip));         
    config.syslog_enable = 0;
    
    // foibles
    config.use_archaic_units = 1;
    config.use_simplified_english = 1;
    config.use_monday_as_week_start = 0;

    // gpio
    for(i=0; i<NUM_ROWS(config.gpio_default); i++)
    {
        config.gpio_default[i] = GP_UNINITIALIZED;
    } 
    
    // mqtt
    config.mqtt_user[0] = 0;
    config.mqtt_password[0] = 0;
    config.mqtt_broker_address[0] = 0;
}

 /*!
 * \brief Copy system settings from unrecognized configuration version to v1
 * 
 * \return 0 on success, -1 on error
 */
void config_system_to_v1(void *previous_config)
{
    int i;
    // NON_VOL_VARIABLES_T_VERSION_11 *temp_config = NULL;
    NON_VOL_VARIABLES_T_VERSION_1 *config_v1 = NULL;
    NON_VOL_VARIABLES_T *config_unrecognized = NULL;

    printf("Attempting to preserve system settings.\n");

    if (previous_config)
    {
        config_unrecognized = (NON_VOL_VARIABLES_T *)previous_config;
        config_v1 = (NON_VOL_VARIABLES_T_VERSION_1 *)&config;

        // ***system config start ***
        //int version;
        config_v1->personality = config_unrecognized->personality;
        memcpy(config_v1->wifi_ssid, config_unrecognized->wifi_ssid, sizeof(config_v1->wifi_ssid));
        memcpy(config_v1->wifi_password, config_unrecognized->wifi_password, sizeof(config_v1->wifi_password));
        memcpy(config_v1->wifi_country, config_unrecognized->wifi_country, sizeof(config_v1->wifi_country));                
        config_v1->dhcp_enable = config_unrecognized->dhcp_enable;
        //memcpy(config_v1->host_name, config_unrecognized->host_name, sizeof(config_v1->host_name)); 
        //STRNCPY(config_v1->host_name, "thermostat", sizeof(config_v1->host_name));          
        memcpy(config_v1->ip_address, config_unrecognized->ip_address, sizeof(config_v1->ip_address));   
        memcpy(config_v1->network_mask, config_unrecognized->network_mask, sizeof(config_v1->network_mask));   
        memcpy(config_v1->gateway, config_unrecognized->gateway, sizeof(config_v1->gateway));   
        config_v1->timezone_offset = config_unrecognized->timezone_offset;
        config_v1->daylightsaving_enable = config_unrecognized->daylightsaving_enable;     
        memcpy(config_v1->daylightsaving_start, config_unrecognized->daylightsaving_start, sizeof(config_v1->daylightsaving_start)); 
        memcpy(config_v1->daylightsaving_end, config_unrecognized->daylightsaving_end, sizeof(config_v1->daylightsaving_end)); 
        memcpy(config_v1->time_server, config_unrecognized->time_server, sizeof(config_v1->time_server));                 
        config_v1->syslog_enable = config_unrecognized->syslog_enable; 
        memcpy(config_v1->syslog_server_ip, config_unrecognized->syslog_server_ip, sizeof(config_v1->syslog_server_ip)); 
        config_v1->use_archaic_units = config_unrecognized->use_archaic_units; 
        config_v1->use_simplified_english = config_unrecognized->use_simplified_english; 
        config_v1->use_monday_as_week_start = config_unrecognized->use_monday_as_week_start; 
        //memcpy(config_v1->gpio_default, config_unrecognized->gpio_default, sizeof(config_v1->gpio_default));   
        // config_v1->mqtt_user[0] = 0;
        // config_v1->mqtt_password[0] = 0;        
        // config_v1->mqtt_broker_address[0] = 0;

    }
}
