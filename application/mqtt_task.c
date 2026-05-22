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
//#include "weather.h"
#include "mqtt.h"
#include "flash.h"
#include "calendar.h"
#include "utility.h"
#include "config.h"
//#include "led_strip.h"
//#include "message.h"
//#include "altcp_tls_mbedtls_structs.h"
//#include "powerwall.h"
#include "pluto.h"
//#include "tm1637.h"
#include "thermostat.h"

#define DISCOVERY_PAYLOAD_BUFFER_SIZE (2400)   // large payload sent to home assistant for automatic device discovery
#define ALL_RELAYS (8)                         // message indicating all relay states need to be published
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

    NUM_TOPICS                  = 4
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
void mqttst_publish_relay_state(int relay, mqtt_client_t *client, void *arg);

// external variables
extern uint32_t unix_time;
extern NON_VOL_VARIABLES_T config;
extern WEB_VARIABLES_T web;

// global variables
MQTT_INITIALIZATION_T mqtt_initialization_table[] =
{
    {mqttst_initialize_topic_staus,               false},
    {mqttst_initialize_queue,                     false},     
    {mqttst_initialize_connection,                false}, 
    {mqttst_initialize_subscription,              false},  
    {mqttst_initialize_ha_discovery,              false}, 
    {mqttst_initialize_ha_states,                 false},                 
};
MQTT_TOPIC_STATUS_T mqtt_status_table[NUM_TOPICS];
bool connection_initialized = false;
bool discovery_initialized = false;
bool states_initialized = false;
bool connection_completed = false;
bool subscription_complete = false;
bool discovery_completed = false;
bool states_completed = false;
int states_outstanding = 0;
ip_addr_t broker_ip;
int relay_to_switch = -1;
int relay_desired_state = -1;
static QueueHandle_t mqtt_queue = NULL;                     // indicates user has change relay state
static uint8_t mqtt_message = 0;                            // relay that has changed state
static bool mqtt_queue_initialized = false;                 // queue initialization status
static mqtt_client_t *mqtt_client;
static char *homeassistant_discovery_payload = NULL;
static int connection_backoff_ms = CONNECTION_BACKOFF_MS_DEFAULT;

/*!
 * \brief Support relay control and monitoring via MQTT
 *
 * \param params unused garbage
 * 
 * \return nothing
 */
void mqtt_task(void *params)
{
    int relay_changed = 0;

    printf("MQTT task started!\n");

    // check and correct critical user configuration settings
    mqttst_sanitize_user_config();
     
    while (true)
    {         
        // initialize all subsystems that are not already up
        mqttst_initialize();

        // wait for timeout period but abort immediately if a relay changes state
        relay_changed = mqttst_wait(MQTT_TASK_LOOP_DELAY);

        if (relay_changed) 
        {
            if (connection_completed)
            {
                // if a specific relay has changed then publish it first
                // if ((mqtt_message >=0) && (mqtt_message < config.rmtsw_relay_max))
                // {
                //     mqttst_publish_relay_state(mqtt_message, mqtt_client, NULL);
                // }

                // publish all relay states
                mqttst_publish_all_thermostat_states(mqtt_client, NULL);
            }
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
    struct mqtt_connect_client_info_t ci;

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
                    connection_initialized = true;
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
 * \brief Initial publication of relay states
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
            application_restart(REBOOT_MQTT_F1);
        } 
    }
}

/*SUBSCRIBE************************************************************************************/
// 1. Publish Callback: Receives the topic
/*!
 * \brief Receive MQTT command topic that identifies the relay
 *
 * \param params unused garbage
 * 
 * \return nothing
 */
void mqttst_incoming_publish_cb(void *arg, const char *topic, u32_t tot_len) 
{
    char expected_domain[32];
    
    sprintf(expected_domain, "relay-c-%02x-%02x-%02x-%02x-%02x-%02x", web.mac[0], web.mac[1], web.mac[2], web.mac[3], web.mac[4], web.mac[5]); 
    
    //printf("Topic: %s, Total Length: %u\n", topic, (unsigned int)tot_len);

    if (strlen(topic) == strlen("relay-c-00-11-22-33-44-55/X/command"))
    {
        if ((strncasecmp(topic, expected_domain, strlen(expected_domain)) == 0) &&
            (strncasecmp(topic + strlen(expected_domain) + strlen("/X/"), "command", strlen("command")) == 0) &&
            isdigit(topic[strlen(expected_domain) + strlen("/")]))
        {
            relay_to_switch = topic[strlen(expected_domain) + strlen("/")] - '0' - 1;  // switch to zero base
            //printf("got relay to switch = %d\n", relay_to_switch);
        }
        else
        {
            //send_syslog_message("mqtt", "unexpected command rejected");
        }
    }
}

// 2. Data Callback: Receives payload chunks
/*!
 * \brief Receive MQTT command data that specifies the relay state (ON or OFF)
 *
 * \param params unused garbage
 * 
 * \return nothing
 */
void mqttst_incoming_data_cb(void *arg, const u8_t *data, u16_t len, u8_t flags) 
{
    if (flags & MQTT_DATA_FLAG_LAST)  // TODO: make this accept and aggregate data received in multiple chunks
    {
        //printf("Final message received: %.*s AND relay to switch = %d\n", len, (const char*)data, relay_to_switch);

        // if ((relay_to_switch >=0) && (relay_to_switch < config.rmtsw_relay_max))
        // {
        //     if (strncasecmp(data, "ON", 2) == 0)
        //     {
        //         web.rmtsw_relay_desired_state[relay_to_switch] = true;
        //         rmtsw_queue_send((uint8_t)relay_to_switch);
        //         mqttst_queue_send((uint8_t)relay_to_switch);                    
        //     }
        //     else if (strncasecmp(data, "OFF", 3) == 0)
        //     {
        //         web.rmtsw_relay_desired_state[relay_to_switch] = false;
        //         rmtsw_queue_send((uint8_t)relay_to_switch);
        //         mqttst_queue_send((uint8_t)relay_to_switch);  
        //     }
            
        //     relay_to_switch = -1;
        // }
       
    }
    else if (flags)
    {
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
        //send_syslog_message("mqtt", "publish failed");
        application_restart(REBOOT_MQTT_F2);
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

// /*!
//  * \brief Send relay state publication
//  *
//  * \param relay 0 - 7
//  * 
//  * \return nothing
//  */
// void mqttst_publish_state(int relay, mqtt_client_t *client, void *arg)
// {
//     const char *pub_payload = "Pico2W Hello!";
//     err_t err;
//     u8_t qos = 2; // 0, 1, or 2
//     u8_t retain = 0;
//     char state[64];
//     char state_payload[8];
//     static MQTT_CALLBACK_ID_T state_arg = MQTT_CALLBACK_STATE_ID;

//     CLIP(relay, 0, 7);

//     sprintf(state, "relay-s-%02x-%02x-%02x-%02x-%02x-%02x/%d/state", web.mac[0], web.mac[1], web.mac[2], web.mac[3], web.mac[4], web.mac[5], relay+1);

//     if (web.rmtsw_relay_desired_state[relay])
//     {
//         sprintf(state_payload, "ON");
//     }
//     else
//     {
//         sprintf(state_payload, "OFF");
//     }

//     // send state
//     retain = 0;
//     cyw43_arch_lwip_begin();
//     err = mqtt_publish(client, state, state_payload, strlen(state_payload), qos, retain, mqttst_pub_request_cb, &state_arg);
//     cyw43_arch_lwip_end();

//     if(err != ERR_OK) 
//     {
//         printf("Publish state error: %d\n", err);
//         //send_syslog_message("mqtt", "publish state error %d", err);
//         application_restart(REBOOT_MQTT_F3);
//     }

//     //printf("published new state. %s = %s\n", state, state_payload);
// }

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
    STRNCAT(buffer, "\",\"device_class\":\"climate\",\"modes\":[\"off\",\"heat\",\"cool\",\"auto\"],\"mode_state_topic\":\"", len);
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
    sprintf(temp_string, "st-%02x-%02x-%02x-%02x-%02x-%02x/hvac/temperature/setpoint", web.mac[0], web.mac[1], web.mac[2], web.mac[3], web.mac[4], web.mac[5]);
    STRNCAT(buffer, temp_string, len);
    STRNCAT(buffer, "\",", len);
    STRNCAT(buffer, "\"temperature_command_topic\":\"", len);
    sprintf(temp_string, "st-%02x-%02x-%02x-%02x-%02x-%02x/hvac/temperature/setpoint", web.mac[0], web.mac[1], web.mac[2], web.mac[3], web.mac[4], web.mac[5]);
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
 * \brief send relay state to the mqtt broker and wait for callback confirmation
 *
 * \param relay 0 - 7
 * 
 * \return nothing
 */
void mqttst_publish_relay_state(int relay, mqtt_client_t *client, void *arg)
{
    int j = 0;

    // if ((relay >= 0) && (relay < config.rmtsw_relay_max))
    // {
    //     states_outstanding = 1;
    //     mqttst_publish_state(relay, client, arg);

    //     // sleep until callback complete or 5 seconds elapse
    //     for(j=0; (j < 100) && states_outstanding; j++)
    //     {
    //         SLEEP_MS(50);
    //     }
    // }
}

/*!
 * \brief trigger publication of relay states by mqtt task
 * 
 * \return nothing
 */
void mqttst_thermostat_refresh(void)
{
    // relay states have changed
    mqttst_queue_send(ALL_RELAYS);
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

    sprintf(mqtt_status_table[TOPIC_MODE_STATE].topic_name,             "st-%02x-%02x-%02x-%02x-%02x-%02x/hvac/mode/state",             web.mac[0], web.mac[1], web.mac[2], web.mac[3], web.mac[4], web.mac[5]);
    sprintf(mqtt_status_table[TOPIC_MODE_SET].topic_name,               "st-%02x-%02x-%02x-%02x-%02x-%02x/hvac/mode/set",               web.mac[0], web.mac[1], web.mac[2], web.mac[3], web.mac[4], web.mac[5]);    
    sprintf(mqtt_status_table[TOPIC_TEMPERATURE_CURRENT].topic_name,    "st-%02x-%02x-%02x-%02x-%02x-%02x/hvac/temperature/current",    web.mac[0], web.mac[1], web.mac[2], web.mac[3], web.mac[4], web.mac[5]);    
    sprintf(mqtt_status_table[TOPIC_TEMPERATURE_SETPOINT].topic_name,   "st-%02x-%02x-%02x-%02x-%02x-%02x/hvac/temperature/setpoint",   web.mac[0], web.mac[1], web.mac[2], web.mac[3], web.mac[4], web.mac[5]);

    for(i=0; i < NUM_TOPICS; i++)
    {
        mqtt_status_table[i].last_change_time    = now;
        mqtt_status_table[i].last_published_time = now;
    }

    return(err);
}


/*!
 * \brief Send relay state publication
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


    //printf("published new state. %s = %s\n", state, state_payload);

    switch(topic_id)
    {
    case TOPIC_MODE_STATE:        
        thermostat_get_mode_string(web.thermostat_effective_mode, state_payload, sizeof(state_payload));
        break;
    case TOPIC_MODE_SET:
        err = 1;
        break; 
    case TOPIC_TEMPERATURE_CURRENT:
        snprintf(state_payload, sizeof(state_payload), "%d", web.thermostat_temperature);
        break;
    case TOPIC_TEMPERATURE_SETPOINT:
        snprintf(state_payload, sizeof(state_payload), "%d", web.thermostat_set_point);
        break;
    default:
        err = 2;
        break;
    }

    if (!err)
    {
        STRNCPY(state, mqtt_status_table[topic_id].topic_name, sizeof(state));

        // send state
        retain = 0;
        cyw43_arch_lwip_begin();
        err = mqtt_publish(client, state, state_payload, strlen(state_payload), qos, retain, mqttst_pub_request_cb, &state_arg);
        cyw43_arch_lwip_end();

        if(err != ERR_OK) 
        {
            printf("Publish state error: %d\n", err);
            //send_syslog_message("mqtt", "publish state error %d", err);
            application_restart(REBOOT_MQTT_F3);
        }

    }
}