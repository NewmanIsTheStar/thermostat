/**
 * Copyright (c) 2024 NewmanIsTheStar
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */
#include <stdio.h>
#include <stdlib.h>

#include "pico/cyw43_arch.h"
#include "pico/stdlib.h"
#include "pico/util/datetime.h"
//#include "hardware/rtc.h"
#include "hardware/watchdog.h"
#include <hardware/flash.h>
#include "hardware/i2c.h"

#include "lwip/netif.h"
#include "lwip/ip4_addr.h"
#include "lwip/apps/lwiperf.h"
#include "lwip/opt.h"
#include "lwip/sockets.h"
#include "lwip/sys.h"
#include "lwip/apps/mqtt.h"

#include "FreeRTOS.h"
#include "FreeRTOSConfig.h"
#include "task.h"
#include "timers.h"
#include "queue.h"

#include "stdarg.h"

#include "watchdog.h"
#include "mqtt.h"
#include "flash.h"
#include "calendar.h"
#include "utility.h"
#include "config.h"
#include "pluto.h"
#include "thermostat.h"

#define DISCOVERY_PAYLOAD_BUFFER_SIZE (2400)   // large payload sent to home assistant for automatic device discovery
#define PUBLISH_ALL_DATA (67)                  // message indicating all thermostat states need to be published
#define CONNECTION_BACKOFF_MS_DEFAULT (500)    // minimum number of milliseconds to wait between attempt to connect to the broker

// typdedefs
typedef struct
{
    int (*initialization)(void);
    bool initialization_complete;
} MQTT_INITIALIZATION_T;

typedef struct
{
    char topic_name[64];
    uint32_t last_change_time;
    uint32_t last_published_time;   
} MQTT_TOPIC_STATUS_T;

typedef enum
{
    TOPIC_MODE_STATE            = 0,
    TOPIC_MODE_SET              = 1,
    TOPIC_TEMPERATURE_CURRENT   = 2,
    TOPIC_TEMPERATURE_SETPOINT  = 3,
    TOPIC_TEMPERATURE_SET       = 4,

    NUM_TOPICS                  = 5
} MQTT_TOPIC_ID_T;


// prototypes -- mqttst_ prefix is used for local functions, whereas lwip functions use mqtt_ 
int mqttst_sanitize_user_config(void);
int mqttst_initialize(void);
int mqttst_deinitialize(int (*subsytem_init_func)(void));
int mqttst_initialize_connection(void);
void mqttst_connection_cb(mqtt_client_t *client, void *arg, mqtt_connection_status_t status);
void mqttst_incoming_publish_cb(void *arg, const char *topic, u32_t tot_len); 
void mqttst_incoming_data_cb(void *arg, const u8_t *data, u16_t len, u8_t flags);
void mqttst_sub_request_cb(void *arg, err_t result);
void mqttst_start_sub(mqtt_client_t *client);
void mqttst_pub_request_cb(void *arg, err_t result);
void mqttst_publish_discovery(mqtt_client_t *client, void *arg);
int mqttst_initialize_subscription(void);
int mqttst_initialize_ha_discovery(void);
int mqttst_initialize_ha_states(void);
int mqttst_initialize_topic_staus(void);
void mqttst_publish_state(MQTT_TOPIC_ID_T topic_id, mqtt_client_t *client);
int mqttst_construct_discovery_topic(char *buffer, size_t len);
int mqttst_construct_discovery_payload(char *buffer, size_t len);
void mqttst_publish_all_thermostat_states(mqtt_client_t *client, void *arg);
int mqttst_wait(TickType_t timeout);
void mqttst_queue_send(uint8_t message);
int mqttst_initialize_queue(void);
void mqttst_tear_down(void);
void mqttst_request_connection_restart(void);

// external variables
extern uint32_t unix_time;
extern NON_VOL_VARIABLES_T config;
extern WEB_VARIABLES_T web;

// global variables
static MQTT_INITIALIZATION_T mqtt_initialization_table[] =
{
    {mqttst_initialize_topic_staus,               false},
    {mqttst_initialize_queue,                     false},     
    {mqttst_initialize_connection,                false}, 
    {mqttst_initialize_subscription,              false},  
    {mqttst_initialize_ha_discovery,              false}, 
    {mqttst_initialize_ha_states,                 false},                 
};
static MQTT_TOPIC_STATUS_T mqtt_status_table[NUM_TOPICS];
static bool connection_process_started = false;
static bool discovery_initialized = false;
static bool states_initialized = false;
static bool connection_completed = false;
static bool subscription_complete = false;
static bool discovery_completed = false;
static bool states_completed = false;
static int states_outstanding = 0;
static bool connection_restart_request = false;
static ip_addr_t broker_ip;
static MQTT_TOPIC_ID_T mqtt_rx_payload_type = NUM_TOPICS;
static QueueHandle_t mqtt_queue = NULL;                     
static uint8_t mqtt_message = 0;                            
static bool mqtt_queue_initialized = false;             
static mqtt_client_t *mqtt_client;
static char *homeassistant_discovery_payload = NULL;
static int connection_backoff_ms = CONNECTION_BACKOFF_MS_DEFAULT;
static char mqtt_requested_mode[16];
static char mqtt_requested_temperature[16];
/*!
 * \brief Support thermostat control and monitoring via MQTT
 *
 * \param params unused garbage
 * 
 * \return nothing
 */
void mqtt_task(void *params)
{
    int mqtt_request = 0;
    int desired_temperature = 0;

    printf("MQTT task started!\n");

    // check and correct critical user configuration settings
    mqttst_sanitize_user_config();
     
    while (true)
    {         
        // initialize all subsystems that are not already up
        mqttst_initialize();

        // wait for timeout period but abort immediately if a command is received
        mqtt_request = mqttst_wait(MQTT_TASK_LOOP_DELAY);

        if (mqtt_request) 
        {
            if (connection_completed)
            {
                switch(mqtt_message)
                {
                case TOPIC_MODE_SET:
                    if (strcasecmp(mqtt_requested_mode, "off") == 0)
                    {
                        display_set_mode(HVAC_OFF);
                    } 
                    else if (strcasecmp(mqtt_requested_mode, "heat") == 0)
                    {
                        display_set_mode(HVAC_HEATING_ONLY);
                    }
                    else if (strcasecmp(mqtt_requested_mode, "cool") == 0)
                    {
                        display_set_mode(HVAC_COOLING_ONLY);
                    }
                    else if (strcasecmp(mqtt_requested_mode, "auto") == 0)
                    {
                        display_set_mode(HVAC_AUTO);
                    } 
                    else if (strcasecmp(mqtt_requested_mode, "fan_only") == 0)
                    {
                        display_set_mode(HVAC_FAN_ONLY);
                    }                     
                    display_fake_button_press();                    
                    break;
                case TOPIC_TEMPERATURE_SET:
                    desired_temperature = get_int_with_tenths_from_string(mqtt_requested_temperature);
                    if ((desired_temperature > 100) && (desired_temperature < 900))  // TODO: sane limits for Celcius and Archaic units
                    {
                        display_set_setpoint_offset(desired_temperature - display_get_base_temperature());
                    }
                    display_fake_button_press();
                    break;
                case PUBLISH_ALL_DATA:
                    mqttst_publish_all_thermostat_states(mqtt_client, NULL);
                    break;
                default:
                    break;
                }                
            }
        }

        if (connection_restart_request)
        {
            mqttst_tear_down();
            connection_restart_request = false;
        }

        // tell watchdog task that we are still alive
        watchdog_pulse((int *)params);           
    }              
}



/*!
 * \brief initialize subsystems
 *
 * \param params none
 * 
 * \return 0 on success
 */
int mqttst_initialize(void)
{
    static bool init_complete = false;
    int err = 0;
    int i;

    for (i=0; i < NUM_ROWS(mqtt_initialization_table); i++)
    {
        if (!mqtt_initialization_table[i].initialization_complete)
        {
            mqtt_initialization_table[i].initialization_complete = !mqtt_initialization_table[i].initialization();            

            if (!mqtt_initialization_table[i].initialization_complete)
            {
                err++;
                printf("MQTT Error initializing subsystem %d\n", i);
            }
        }
    }

    if (err)
    {
        printf("MQTT %d subsystems failed to initialize\n", err);
        
    } else if (!init_complete)
    {
        printf("MQTT all subsystems sucessfully initialized\n");
        init_complete = true;
    }

    return(err);
}

/*!
 * \brief deinitialize a subsytem
 *
 * \param params none
 * 
 * \return 0 on success
 */
int mqttst_deinitialize(int (*subsytem_init_func)(void))
{
    int err = 1;
    int i;

    for (i=0; i < NUM_ROWS(mqtt_initialization_table); i++)
    {
        if (mqtt_initialization_table[i].initialization == subsytem_init_func)
        {
            mqtt_initialization_table[i].initialization_complete = false;
            err = 0;
            break;
        }
    }

    return(err);
}


 /*!
 * \brief perform sanity check on critical user config values
 *
 * \param params none
 * 
 * \return 0 on success
 */
int mqttst_sanitize_user_config(void)
{   

    return(0);
}

 /*!
 * \brief Begin MQTT connection process
 *
 * \param params none
 * 
 * \return 0 on success
 */
int mqttst_initialize_connection(void)
{
    int err = -1;
    static struct mqtt_connect_client_info_t ci;

    if (config.mqtt_broker_address[0] != 0)
    {
        broker_ip.addr = address_string_to_ip(config.mqtt_broker_address);
   
        if (broker_ip.addr)
        {
            memset(&ci, 0, sizeof(ci));
            ci.client_id = "pi_pico2w_client";
            ci.client_user = config.mqtt_user;
            ci.client_pass = config.mqtt_password;
            ci.keep_alive = 60;

            cyw43_arch_lwip_begin();
            mqtt_client = mqtt_client_new();
            cyw43_arch_lwip_end();

            if (mqtt_client != NULL) 
            {
                cyw43_arch_lwip_begin();
                err = mqtt_client_connect(mqtt_client, &broker_ip, MQTT_PORT, mqttst_connection_cb, NULL, &ci);
                cyw43_arch_lwip_end();

                //printf("mqtt connect returned %d\n", err);

                if (err == ERR_OK)
                {
                    connection_process_started = true;
                }
            }
        }

        SLEEP_MS(connection_backoff_ms);        
    }
    //printf("initialize connection returning %d\n", err);

    return(err);
}

 /*!
 * \brief Begin subscription process
 *
 * \param params none
 * 
 * \return 0 on success
 */
int mqttst_initialize_subscription(void)
{
    int err = -1;
    struct mqtt_connect_client_info_t ci;

    if (connection_completed)
    {
        // Subscribe to a topic here
        mqttst_start_sub(mqtt_client);
        err = 0;
    }

    return(err);
}

 /*!
 * \brief Begin home assistant mqtt device discovery process
 *
 * \param params none
 * 
 * \return 0 on success
 */
int mqttst_initialize_ha_discovery(void)
{
    int err = -1;

    if (connection_completed)
    {
        //printf("about to call publish discovery\n");

        mqttst_publish_discovery(mqtt_client, NULL);
        
        discovery_initialized = true;

        err = 0;
    }

    //printf("initialize discovery returning %d\n", err);
    
    return(err);
}

 /*!
 * \brief Initial publication of thermostat states
 *
 * \param params none
 * 
 * \return 0 on success
 */
int mqttst_initialize_ha_states(void)
{
    int err = -1;

    if (discovery_completed)
    {
        //printf("about to call publish all states\n");

        mqttst_publish_all_thermostat_states(mqtt_client, NULL);
        
        states_initialized = true;

        err = 0;
    }
    //printf("initialize states returning %d\n", err);
    
    return(err);
}


// Callback for connection status
/*!
 * \brief Receive connection process completion status
 *
 * \param params unused garbage
 * 
 * \return nothing
 */
void mqttst_connection_cb(mqtt_client_t *client, void *arg, mqtt_connection_status_t status)
{
    if (status == MQTT_CONNECT_ACCEPTED)
    {
        //printf("MQTT Connected!\n");
        connection_completed = true;
        connection_backoff_ms = CONNECTION_BACKOFF_MS_DEFAULT;

        // // Subscribe to a topic here
        // mqtt_start_sub(client);

    } else
    {
        printf("MQTT Connection failed: %d\n", status);

        // double connection attempt backoff time up to approx 5 minutes
        if (connection_backoff_ms < 5*60000)
        {
            connection_backoff_ms *=2;
        }
        else
        {
            mqttst_request_connection_restart();
        } 
    }
}

/*!
 * \brief Receive MQTT command topic that identifies the incomming command
 *
 * \param params unused garbage
 * 
 * \return nothing
 */
void mqttst_incoming_publish_cb(void *arg, const char *topic, u32_t tot_len) 
{
    if (strcasecmp(topic, mqtt_status_table[TOPIC_MODE_SET].topic_name) == 0)
    {
        mqtt_rx_payload_type = TOPIC_MODE_SET;
    }    
    else if (strcasecmp(topic, mqtt_status_table[TOPIC_TEMPERATURE_SET].topic_name) == 0)
    {
        mqtt_rx_payload_type = TOPIC_TEMPERATURE_SET;
    }      
}

/*!
 * \brief Receive MQTT command data that specifies either a mode or setpoint
 *
 * \param params unused garbage
 * 
 * \return nothing
 */
void mqttst_incoming_data_cb(void *arg, const u8_t *data, u16_t len, u8_t flags) 
{
    if (flags & MQTT_DATA_FLAG_LAST)  // TODO: make this accept and aggregate data received in multiple chunks
    {
        switch(mqtt_rx_payload_type)
        {
        case TOPIC_MODE_SET:
            CLIP(len, 0, sizeof(mqtt_requested_mode));
            memcpy(mqtt_requested_mode, data, len);
            mqtt_requested_mode[len] = 0;
            //printf("RX requested mod: %s\n", mqtt_requested_mode);            
            mqttst_queue_send((uint8_t)TOPIC_MODE_SET);
            break;
        case TOPIC_TEMPERATURE_SET:
            CLIP(len, 0, sizeof(mqtt_requested_temperature));
            memcpy(mqtt_requested_temperature, data, len);
            mqtt_requested_temperature[len] = 0;
            //printf("RX requested temperature: %s\n", mqtt_requested_temperature);
            mqttst_queue_send((uint8_t)TOPIC_TEMPERATURE_SET);        
            break;
        default:
            break;
        }
       
        mqtt_rx_payload_type = NUM_TOPICS;

    }
    else if (flags)
    {
        printf("unhandled partial publication payload packet \n");
        //send_syslog_message("mqtt", "unhandled partial packet");
    }
}

// 3. Sub Request Callback: Confirms subscription status
/*!
 * \brief Receives status of subscripton process upon completetion
 *
 * \param params unused garbage
 * 
 * \return nothing
 */
void mqttst_sub_request_cb(void *arg, err_t result) 
{
    if (result == ERR_OK)
    {
        subscription_complete = true;
    }
    else
    {
        printf("Subscribe result: %d\n", result);
        //send_syslog_message("mqtt", "subscribe failed %d", result);
    }
}

// 4. Setup
/*!
 * \brief Send subscription
 *
 * \param params unused garbage
 * 
 * \return nothing
 */
void mqttst_start_sub(mqtt_client_t *client)
{
    int err;
    static char topic[32];

    // Set callbacks
    cyw43_arch_lwip_begin();
    mqtt_set_inpub_callback(client, mqttst_incoming_publish_cb, mqttst_incoming_data_cb, NULL);
    cyw43_arch_lwip_end();

    // Subscribe
    sprintf(topic, "st-%02x-%02x-%02x-%02x-%02x-%02x/#", web.mac[0], web.mac[1], web.mac[2], web.mac[3], web.mac[4], web.mac[5]); 
    cyw43_arch_lwip_begin();   
    err = mqtt_subscribe(client, topic, 1, mqttst_sub_request_cb, NULL);    
    cyw43_arch_lwip_end();

    //printf("subscribe result = %d\n", err);
}

/*PUBLISH**********************************************************************************************/
// 1. Define callback for publish completion
/*!
 * \brief Receive publication process status upon completion
 *
 * \param params unused garbage
 * 
 * \return nothing
 */
void mqttst_pub_request_cb(void *arg, err_t result) 
{
    if(result != ERR_OK) 
    {
        printf("Publish failed: %d\n", result);
        mqttst_request_connection_restart();
    } else 
    {
        //printf("Publish success\n");
    }

    if (arg)
    {
        //printf("publish callback received argument %d\n", *(MQTT_CALLBACK_ID_T *)arg);
        switch(*(MQTT_CALLBACK_ID_T *)arg)
        {
        case MQTT_CALLBACK_DISCOVERY_ID:
            if (homeassistant_discovery_payload)
            {
                free(homeassistant_discovery_payload);
                homeassistant_discovery_payload = NULL;
                //printf("freed discovery payload buffer\n");
                discovery_completed = true;
            }
            break;
        case MQTT_CALLBACK_STATE_ID:
            if (states_outstanding > 0)
            {
                states_outstanding--;
            }
            states_completed = true;
            break;    
        default:        
            break;
        }
    } 
}

// 2. Example publish function
/*!
 * \brief Send discovery publication
 *
 * \param params unused garbage
 * 
 * \return nothing
 */
void mqttst_publish_discovery(mqtt_client_t *client, void *arg)
{
    //const char *pub_payload = "Pico2W Hello!";
    err_t err;
    u8_t qos = 1; // 0, 1, or 2
    u8_t retain = 0;
    //static char discovery_payload[DISCOVERY_PAYLOAD_BUFFER_SIZE];
    static char discovery_topic[60];
    static MQTT_CALLBACK_ID_T discovery_arg = MQTT_CALLBACK_DISCOVERY_ID;

    //printf("Constructing discovery topic\n");
    mqttst_construct_discovery_topic(discovery_topic, sizeof(discovery_topic));
    //printf("Topic follows\n%s\n", discovery_topic);    


    if (!homeassistant_discovery_payload)
    {
        homeassistant_discovery_payload = malloc(DISCOVERY_PAYLOAD_BUFFER_SIZE);
    }

    if (homeassistant_discovery_payload)
    {    
        //printf("Constructing discovery payload\n");
        mqttst_construct_discovery_payload(homeassistant_discovery_payload, DISCOVERY_PAYLOAD_BUFFER_SIZE);
        //printf("Payload follows\n%s\n", homeassistant_discovery_payload);
        //printf("size of payload = %d\n", strlen(homeassistant_discovery_payload));
    
        // remove device from home assistant
        retain = 1;
        cyw43_arch_lwip_begin();
        err = mqtt_publish(client, discovery_topic, "", 0, qos, retain, mqttst_pub_request_cb, arg);
        cyw43_arch_lwip_end();

        SLEEP_MS(1000);

        // add device to home assistant
        retain = 1;
        cyw43_arch_lwip_begin();
        err = mqtt_publish(client, discovery_topic, homeassistant_discovery_payload, strlen(homeassistant_discovery_payload), qos, retain, mqttst_pub_request_cb, &discovery_arg);
        cyw43_arch_lwip_end();;

        if(err != ERR_OK) 
        {
            printf("Publish discovery error: %d\n", err);
        }
    }
    else
    {
        printf("failed to send home assistant device discovery topic due to lack of memory\n");
    }
}

/*!
 * \brief Send thermostat state publication
 *
 * \param topic  topic to publish
 * 
 * \return nothing
 */
void mqttst_publish_state(MQTT_TOPIC_ID_T topic_id, mqtt_client_t *client)
{
    const char *pub_payload = "Pico2W Hello!";
    err_t err = 0;
    u8_t qos = 2; // 0, 1, or 2
    u8_t retain = 0;
    char state[64];
    char state_payload[64];
    static MQTT_CALLBACK_ID_T state_arg = MQTT_CALLBACK_STATE_ID;

    switch(topic_id)
    {
    case TOPIC_MODE_STATE:        
        thermostat_get_home_assistant_mode_string(web.thermostat_effective_mode, state_payload, sizeof(state_payload));
        break;
    case TOPIC_MODE_SET:
        err = 1;
        break; 
    case TOPIC_TEMPERATURE_CURRENT:
        snprintf(state_payload, sizeof(state_payload), "%c%d.%d", web.thermostat_temperature<0?'-':' ', abs(web.thermostat_temperature/10), abs(web.thermostat_temperature%10));        
        break;
    case TOPIC_TEMPERATURE_SETPOINT:
        snprintf(state_payload, sizeof(state_payload), "%c%d.%d", web.thermostat_set_point<0?'-':' ', abs(web.thermostat_set_point/10), abs(web.thermostat_set_point%10));
        break;
    case TOPIC_TEMPERATURE_SET:
        err = 2;
        break;         
    default:
        err = 3;
        break;
    }

    if (!err)
    {
        STRNCPY(state, mqtt_status_table[topic_id].topic_name, sizeof(state));

        //printf("PUBLISHING: topic = %s payload = %s\n", state, state_payload);
        // send state
        retain = 0;
        cyw43_arch_lwip_begin();
        err = mqtt_publish(client, state, state_payload, strlen(state_payload), qos, retain, mqttst_pub_request_cb, &state_arg);
        cyw43_arch_lwip_end();

        if(err != ERR_OK) 
        {
            printf("Publish state error: %d\n", err);
            mqttst_request_connection_restart();
        }

    }
}

/*!
 * \brief print home assistant discovery payload into callers buffer
 *
 * \param params unused garbage
 * 
 * \return nothing
 */
int mqttst_construct_discovery_payload(char *buffer, size_t len)
{
    int err = 0;
    int i; 
    char temp_string[64];

    *buffer = 0;

/* TEMPLATE
{
  "name": "Thermostat",
  "unique_id": "mqtt_thermostat_12345",
  "device_class": "climate",
  "modes": ["off", "heat", "cool", "auto"],
  "mode_state_topic": "house/hvac/mode/state",
  "mode_command_topic": "house/hvac/mode/set",
  "current_temperature_topic": "house/hvac/temperature/current",
  "temperature_state_topic": "house/hvac/temperature/setpoint",
  "temperature_command_topic": "house/hvac/temperature/setpoint"
}
*/

    STRNCAT(buffer, "{\"name\": \"Thermostat\",\"unique_id\":\"", len);
    sprintf(temp_string, "st-%02x-%02x-%02x-%02x-%02x-%02x", web.mac[0], web.mac[1], web.mac[2], web.mac[3], web.mac[4], web.mac[5]);
    STRNCAT(buffer, temp_string, len);
    STRNCAT(buffer, "\",\"device_class\":\"climate\",\"modes\":[\"off\",\"heat\",\"cool\",\"auto\",\"fan_only\"],\"mode_state_topic\":\"", len);
    sprintf(temp_string, "st-%02x-%02x-%02x-%02x-%02x-%02x/hvac/mode/state", web.mac[0], web.mac[1], web.mac[2], web.mac[3], web.mac[4], web.mac[5]);
    STRNCAT(buffer, temp_string, len);
    STRNCAT(buffer, "\",", len);
    STRNCAT(buffer, "\"mode_command_topic\":\"", len);
    sprintf(temp_string, "st-%02x-%02x-%02x-%02x-%02x-%02x/hvac/mode/set", web.mac[0], web.mac[1], web.mac[2], web.mac[3], web.mac[4], web.mac[5]);
    STRNCAT(buffer, temp_string, len);
    STRNCAT(buffer, "\",", len);
    STRNCAT(buffer, "\"current_temperature_topic\":\"", len);
    sprintf(temp_string, "st-%02x-%02x-%02x-%02x-%02x-%02x/hvac/temperature/current", web.mac[0], web.mac[1], web.mac[2], web.mac[3], web.mac[4], web.mac[5]);
    STRNCAT(buffer, temp_string, len);
    STRNCAT(buffer, "\",", len);
    STRNCAT(buffer, "\"temperature_state_topic\":\"", len);
    sprintf(temp_string, "st-%02x-%02x-%02x-%02x-%02x-%02x/hvac/temperature/setpoint/state", web.mac[0], web.mac[1], web.mac[2], web.mac[3], web.mac[4], web.mac[5]);
    STRNCAT(buffer, temp_string, len);
    STRNCAT(buffer, "\",", len);
    STRNCAT(buffer, "\"temperature_command_topic\":\"", len);
    sprintf(temp_string, "st-%02x-%02x-%02x-%02x-%02x-%02x/hvac/temperature/setpoint/command", web.mac[0], web.mac[1], web.mac[2], web.mac[3], web.mac[4], web.mac[5]);
    STRNCAT(buffer, temp_string, len);
    STRNCAT(buffer, "\"}", len);

    return(err);
}

/*!
 * \brief print home assistant discovery topic into callers buffer
 *
 * \param params unused garbage
 * 
 * \return nothing
 */
int mqttst_construct_discovery_topic(char *buffer, size_t len)
{
    int err = 0;
    int i; 
    char temp_string[32];

    *buffer = 0;

    // STRNCAT(buffer, "homeassistant/device/rmtsw-", len);
    // sprintf(temp_string, "%02x-%02x-%02x-%02x-%02x-%02x", web.mac[0], web.mac[1], web.mac[2], web.mac[3], web.mac[4], web.mac[5]);
    // STRNCAT(buffer, temp_string, len);
    // STRNCAT(buffer, "/config", len);  
    
    STRNCAT(buffer, "homeassistant/climate/st-", len);
    sprintf(temp_string, "%02x-%02x-%02x-%02x-%02x-%02x", web.mac[0], web.mac[1], web.mac[2], web.mac[3], web.mac[4], web.mac[5]);
    STRNCAT(buffer, temp_string, len);
    STRNCAT(buffer, "/config", len);  

    return(0);
}

/*!
 * \brief send all thermostat states to the mqtt broker sequentially
 *
 * \param params unused garbage
 * 
 * \return nothing
 */
void mqttst_publish_all_thermostat_states(mqtt_client_t *client, void *arg)
{
    int j = 0;

    states_outstanding = 0;

    if (j < 100)
    {
        states_outstanding++;
        mqttst_publish_state(TOPIC_MODE_STATE, client);
        
        // wait for callback to zero states_outstanding
        for(j=0; (j < 100) && states_outstanding; j++)
        {
            SLEEP_MS(50);
        }             
    }   

        if (j < 100)
    {
        states_outstanding++;
        mqttst_publish_state(TOPIC_TEMPERATURE_CURRENT, client);

        // wait for callback to zero states_outstanding
        for(j=0; (j < 100) && states_outstanding; j++)
        {
            SLEEP_MS(50);
        }             
    }  

    if (j < 100)
    {
        states_outstanding++;
        mqttst_publish_state(TOPIC_TEMPERATURE_SETPOINT, client);
                    
        // wait for callback to zero states_outstanding        
        for(j=0; (j < 100) && states_outstanding; j++)
        {
            SLEEP_MS(50);
        }             
    }      
}

/*!
 * \brief trigger publication of thermostat states by mqtt task
 * 
 * \return nothing
 */
void mqttst_thermostat_refresh(void)
{
    // thermostat states have changed
    mqttst_queue_send(PUBLISH_ALL_DATA);
}

/*!
 * \brief wait for timeout or queue
 * 
 * \return true if timeout preempted
 */
int mqttst_wait(TickType_t timeout)
{
    int err = 0;

    if (xQueueReceive(mqtt_queue, &mqtt_message, timeout) == pdPASS)
    {
        // got a message
        err = 1;
    }

    return(err);
}

/*!
 * \brief send a message to the mqtt_task queue
 *
 * \param message one byte message
 * 
 * \return nothing
 */
void mqttst_queue_send(uint8_t message)
{
    static uint8_t message_store = 0;

    if (mqtt_queue_initialized)
    {
        message_store = message;

        // send the message to the queue
        xQueueSend(mqtt_queue, &message_store, 0);
    }
}

/*!
 * \brief initialize a queue for sending messages to the mqtt_task
 * 
 * \return nothing
 */
int mqttst_initialize_queue(void)
{
    int err = 0;

    // create queue for to pass interrupt messages to task
    mqtt_queue = xQueueCreate(1, sizeof(uint8_t));

    mqtt_queue_initialized = true;

    return(err);
}

/*!
 * \brief initialize a topic status table
 * 
 * \return nothing
 */
int mqttst_initialize_topic_staus(void)
{
    int err = 0;
    int i;
    uint32_t now;

    now = unix_time;

    sprintf(mqtt_status_table[TOPIC_MODE_STATE].topic_name,             "st-%02x-%02x-%02x-%02x-%02x-%02x/hvac/mode/state",                   web.mac[0], web.mac[1], web.mac[2], web.mac[3], web.mac[4], web.mac[5]);
    sprintf(mqtt_status_table[TOPIC_MODE_SET].topic_name,               "st-%02x-%02x-%02x-%02x-%02x-%02x/hvac/mode/set",                     web.mac[0], web.mac[1], web.mac[2], web.mac[3], web.mac[4], web.mac[5]);    
    sprintf(mqtt_status_table[TOPIC_TEMPERATURE_CURRENT].topic_name,    "st-%02x-%02x-%02x-%02x-%02x-%02x/hvac/temperature/current",          web.mac[0], web.mac[1], web.mac[2], web.mac[3], web.mac[4], web.mac[5]);    
    sprintf(mqtt_status_table[TOPIC_TEMPERATURE_SETPOINT].topic_name,   "st-%02x-%02x-%02x-%02x-%02x-%02x/hvac/temperature/setpoint/state",   web.mac[0], web.mac[1], web.mac[2], web.mac[3], web.mac[4], web.mac[5]);
    sprintf(mqtt_status_table[TOPIC_TEMPERATURE_SET].topic_name,        "st-%02x-%02x-%02x-%02x-%02x-%02x/hvac/temperature/setpoint/command", web.mac[0], web.mac[1], web.mac[2], web.mac[3], web.mac[4], web.mac[5]);    

    for(i=0; i < NUM_TOPICS; i++)
    {
        mqtt_status_table[i].last_change_time    = now;
        mqtt_status_table[i].last_published_time = now;
    }

    return(err);
}


/*!
 * \brief safely tear down the mqtt connection
 * 
 * \return nothing
 */
void mqttst_tear_down(void)
{
    u8_t connection_up = 0;

    if (mqtt_client != NULL)
    {
        cyw43_arch_lwip_begin();
        connection_up = mqtt_client_is_connected(mqtt_client);
        cyw43_arch_lwip_end();

        if (connection_up)
        {
            cyw43_arch_lwip_begin();
            mqtt_disconnect(mqtt_client);
            cyw43_arch_lwip_end();           
        }

        mqtt_client = NULL;
    }

    // mark connection down
    connection_process_started = false;
    connection_completed = false;
    discovery_completed = false;

    // deinitialize connection related functions (so that they will be rerun)
    mqttst_deinitialize(mqttst_initialize_ha_states);
    mqttst_deinitialize(mqttst_initialize_ha_discovery);
    mqttst_deinitialize(mqttst_initialize_subscription);
    mqttst_deinitialize(mqttst_initialize_connection);

    SLEEP_MS(connection_backoff_ms);
}

/*!
 * \brief request connection restart
 * 
 * \return nothing
 */
void mqttst_request_connection_restart(void)
{
    connection_restart_request = true;
}
