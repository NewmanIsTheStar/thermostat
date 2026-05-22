/* The https code in this file is based on the https example code found at
https://github.com/marceloalcocer/picohttps/blob/main/picohttps.c         */



/* Includes *******************************************************************/
#define _GNU_SOURCE
#include <string.h>

// Pico SDK
#include "pico/stdlib.h"            // Standard library
#include "pico/cyw43_arch.h"        // Pico W wireless

// lwIP
#include "lwip/dns.h"               // Hostname resolution
#include "lwip/altcp_tls.h"         // TCP + TLS (+ HTTP == HTTPS)
#include "altcp_tls_mbedtls_structs.h"
#include "lwip/prot/iana.h"         // HTTPS port number

#include "FreeRTOS.h"
#include "FreeRTOSConfig.h"
#include "task.h"

// from ssl_client1.c example program
#include "mbedtls_config.h"  //local copy from https example for pico
#include "mbedtls/platform.h"
#include "mbedtls/net_sockets.h"
#include "mbedtls/debug.h"
#include "mbedtls/ssl.h"
#include "mbedtls/entropy.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/error.h"

#include <string.h>
#include "json_parser.h"
#include "powerwall.h"              // Options, macros, forward declarations
#include "config.h"
#include "pluto.h"


#define GET_REQUEST "GET / HTTP/1.0\r\n\r\n"
#define POWERWALL_LOCKOUT_TIME (24*60*60)

// types
typedef enum
{
    PW_INITIATE,
    PW_CONNECT,
    PW_LOGIN,
    PW_CONFIRM_LOGIN,
    PW_GET_GRID_STATUS,
    PW_CONFIRM_GRID_STATUS,
    PW_GET_BATTERY,
    PW_CONFIRM_BATTERY,
    PW_LOGOUT,
    PW_CONFIRM_LOGOUT,
    PW_TEAR_DOWN,
    PW_LOCKOUT
} PW_STATE_T;

// prototypes
void tear_down(struct altcp_pcb* pcb);
int strip_first_last_quotes(char *token, int length);
void tear_down(struct altcp_pcb* pcb);
bool resolve_hostname(ip_addr_t* ipaddr);
void altcp_free_pcb(struct altcp_pcb* pcb);
void altcp_free_config(struct altcp_tls_config* config);
void altcp_free_arg(struct altcp_callback_arg* arg);
bool connect_to_host(ip_addr_t* ipaddr, struct altcp_pcb** pcb);
void callback_gethostbyname(const char* name, const ip_addr_t* resolved, void* ipaddr);
void callback_altcp_err(void* arg, lwip_err_t err);
lwip_err_t callback_altcp_poll(void* arg, struct altcp_pcb* pcb);
lwip_err_t callback_altcp_sent(void* arg, struct altcp_pcb* pcb, u16_t len);
lwip_err_t callback_altcp_recv(void* arg, struct altcp_pcb* pcb, struct pbuf* buf, lwip_err_t err);
lwip_err_t callback_altcp_connect(void* arg, struct altcp_pcb* pcb, lwip_err_t err);
void powerwall_poll(void);
bool http_request(struct altcp_pcb* pcb, HTTP_REQUEST_TYPE_T type, char *url, char *host, char *content, char *auth_token, char *cookies);
int powerwall_login(struct altcp_pcb* pcb);
int powerwall_get_grid_status(struct altcp_pcb* pcb, char *auth_token, char *cookies);
int powerwall_get_battery_percentage(struct altcp_pcb* pcb, char *auth_token, char *cookies);
int powerwall_logout(struct altcp_pcb* pcb, char *auth_token, char *cookies);
int http_extract_cookies(const char *http_packet, char *cookies, int length);
int wait_for_packet(TickType_t ticks);
int powerwall_sanitize_config(void);

// external variables
extern uint32_t unix_time;
extern NON_VOL_VARIABLES_T config;
extern WEB_VARIABLES_T web;
extern char jsonp_value[255][128];
extern NON_VOL_VARIABLES_T config;

// global variables
char copy_buffer[2048];
int copy_ready = 0;
JSON_PARSER_CONTEXT_T powerwall_parser_context;
uint32_t powerwall_lockout_start;


/*!
 * \brief Wrapper to pace powerwall polling
 *
 * 
 * \return nothing
 */
void powerwall_check(void)
{
    static bool first_poll = true; 
    static TickType_t last_poll= 0;
    TickType_t now = 0;

    now = xTaskGetTickCount();

    // poll powerwall once every 15 ninutes
    if (((now - last_poll) > 1000*60*15) || first_poll)
    {
        powerwall_poll();

        last_poll = now;
        first_poll = false;
    }
}

/*!
 * \brief State machine to read powerwall battery level
 *
 * 
 * \return nothing
 */
void powerwall_poll(void)
{
    static PW_STATE_T powerwall_connection_state = PW_INITIATE;
    static int powerwall_num_access_failures = 0;
    const int max_retries = 2;
    struct altcp_pcb* pcb = NULL;
    char *start_of_json = NULL;
    char *start_of_cookie = NULL;
    int i;
    char cookie[1024];
    char grid_status[256];
    char battery_percentage[256];    
    char authorization_token[256];
    bool access_failure = false;

    powerwall_sanitize_config();

    // check if retry limit reached
    if (powerwall_num_access_failures >= max_retries)
    {
        powerwall_connection_state = PW_LOCKOUT;
        powerwall_lockout_start = unix_time;
        powerwall_num_access_failures = 0;
    }

    switch(powerwall_connection_state)
    {
        default:
        case PW_INITIATE:
            // resolve server hostname
            ip_addr_t ipaddr;
            char* char_ipaddr;
            //printf("Resolving %s\n", PICOHTTPS_HOSTNAME);
            if(!resolve_hostname(&ipaddr))
            {
                printf("Failed to resolve powerwall host = %s\n", config.powerwall_ip);
                break;
            } 
            else
            {
                cyw43_arch_lwip_begin();
                char_ipaddr = ipaddr_ntoa(&ipaddr);
                cyw43_arch_lwip_end();
                //printf("Resolved %s (%s)\n", PICOHTTPS_HOSTNAME, char_ipaddr);

                powerwall_connection_state = PW_CONNECT;
            }

            // deliberate fall through

        case PW_CONNECT:
            // connect to powerwall
            pcb = NULL;
            printf("Connecting to https://%s:%d\n", char_ipaddr, LWIP_IANA_PORT_HTTPS);
            if(!connect_to_host(&ipaddr, &pcb))
            {
                printf("Failed to connect to powerwall at https://%s:%d\n", char_ipaddr, LWIP_IANA_PORT_HTTPS);
                
                access_failure = true;
                break;
            }
            else
            {
                //printf("Connected to https://%s:%d\n", char_ipaddr, LWIP_IANA_PORT_HTTPS);
                //retry = 0;
                powerwall_connection_state = PW_LOGIN;
            }

            // deliberate fall through

        case PW_LOGIN:                                   
            // send login request to powerwall
            //printf("Sending login request\n");
            if(!powerwall_login(pcb))
            {        
                printf("Failed to send powerwall login request\n");
                access_failure = true;

                // tear down connection
                tear_down(pcb);
                pcb = NULL;

                powerwall_connection_state = PW_INITIATE;
                break;
            }
            else
            {
                //printf("Sent login request to https://%s:%d\n", char_ipaddr, LWIP_IANA_PORT_HTTPS);
                //retry = 0;
                powerwall_connection_state = PW_CONFIRM_LOGIN;                
            }

            // deliberate fall through

        case PW_CONFIRM_LOGIN:            
            // Await HTTP response
            //printf("Awaiting login confirmation\n");
            //sleep_ms(5000);
            wait_for_packet(5000);
            //printf("Awaited response\n");

            http_extract_cookies(copy_buffer, cookie, sizeof(cookie));

            //printf("parse json\n");
            start_of_json = strcasestr(copy_buffer, "\r\n{");
            if (start_of_json)
            {
                start_of_json += 2; // point to opening brace

                jsonp_parse_buffer(&powerwall_parser_context, start_of_json, false);
                //jsonp_parse_buffer(start_of_json);
            }
            //jsonp_dump_key_value_pairs();
            //jsonp+dump_tokens(); 

            if (jsonp_get_value(&powerwall_parser_context, "root.\"token\"", authorization_token, sizeof(authorization_token), false))
            {
                printf("Powerwall login failed.\n");
                access_failure = true;

                jsonp_dump_key_value_pairs(&powerwall_parser_context);
                //jsonp_dump_tokens(&powerwall_parser_context);

                // tear down connection
                tear_down(pcb);
                pcb = NULL;

                powerwall_connection_state = PW_INITIATE;
                break;
            } 
            else            
            {
                strip_first_last_quotes(authorization_token, sizeof(authorization_token));
                //printf("Login successful.  Authorization token is = %s\n", authorization_token);
                //retry = 0;
                powerwall_connection_state = PW_GET_GRID_STATUS; 
            } 
           
            // deliberate fallthrough

        case PW_GET_GRID_STATUS:
            // send status request to powerwall
            //printf("Sending grid status request to powerwall\n");
            if(!powerwall_get_grid_status(pcb, authorization_token, cookie))
            {        
                printf("Failed to send powewall grid status request\n");
                access_failure = true;

                // tear down connection
                tear_down(pcb);
                pcb = NULL;

                powerwall_connection_state = PW_INITIATE;
                break;
            }
            else
            {
                //printf("Sent status request in to https://%s:%d\n", char_ipaddr, LWIP_IANA_PORT_HTTPS);
                //retry = 0;
                powerwall_connection_state = PW_CONFIRM_GRID_STATUS;                
            }

            // deliberate fallthrough

        case PW_CONFIRM_GRID_STATUS:  
            // Await status response
            //printf("Awaiting grid status response\n");
            //sleep_ms(5000);
            wait_for_packet(5000);
            //printf("Awaited response\n");

            //printf("parse json\n");
            start_of_json = strcasestr(copy_buffer, "\r\n{");
            if (start_of_json)
            {
                start_of_json += 2; // point to opening brace

                jsonp_parse_buffer(&powerwall_parser_context, start_of_json, false);
                //jsonp_parse_buffer(start_of_json);
            }

            if (jsonp_get_value(&powerwall_parser_context, "root.\"grid_status\"", grid_status, sizeof(grid_status), false))
            {
                printf("FAILED TO GET powerwall GRID STATUS\n");
                access_failure = true;

                if (web.powerwall_grid_status == GRID_UP)
                {
                    // only move to unknown if grid was up, if grid was down continue to assume it is down unitl a response is received
                    web.powerwall_grid_status = GRID_UNKNOWN;  
                }

                // tear down connection
                tear_down(pcb);
                pcb = NULL;

                powerwall_connection_state = PW_INITIATE;             
                break;
            }
            else
            {
                printf("Powerwall Grid Status is = %s\n", grid_status);
                
                if (strcasestr(grid_status, "SystemGridConnected"))
                {
                    web.powerwall_grid_status = GRID_UP;
                }
                else
                {
                    web.powerwall_grid_status = GRID_DOWN;
                }
                
                //printf("==> %d\n", web.powerwall_grid_up);

                //retry = 0;
                //powerwall_connection_state = PW_LOGOUT;
            }

            // deliberate fallthrough

        case PW_GET_BATTERY:
            // send battery request to powerwall
            //printf("Sending battery request to powerwall\n");
            if(!powerwall_get_battery_percentage(pcb, authorization_token, cookie))
            {        
                printf("Failed to send powerwall battery status request\n");
                access_failure = true;

                // tear down connection
                tear_down(pcb);
                pcb = NULL;

                powerwall_connection_state = PW_INITIATE;
                break;
            }
            else
            {
                //printf("Sent status request in to https://%s:%d\n", char_ipaddr, LWIP_IANA_PORT_HTTPS);
                //retry = 0;
                powerwall_connection_state = PW_CONFIRM_GRID_STATUS;                
            }

            // deliberate fallthrough

        case PW_CONFIRM_BATTERY:  
            // Await status response
            //printf("Awaiting battery response\n");
            //sleep_ms(5000);
            wait_for_packet(5000);
            //printf("Awaited battery response\n");

            //printf("parse json\n");
            start_of_json = strcasestr(copy_buffer, "\r\n{");
            if (start_of_json)
            {
                start_of_json += 2; // point to opening brace

                jsonp_parse_buffer(&powerwall_parser_context, start_of_json, false);
                //jsonp_parse_buffer(start_of_json);
            }

            if (jsonp_get_value(&powerwall_parser_context, "root.\"percentage\"", battery_percentage, sizeof(battery_percentage), false))
            {
                printf("FAILED TO GET powerwall BATTERY PERCENTAGE\n");
                access_failure = true;

                // tear down connection
                tear_down(pcb);
                pcb = NULL;

                powerwall_connection_state = PW_INITIATE;            
                break;
            }
            else
            {
                printf("Powerwall Battery Percentage is = %s\n", battery_percentage);

                // value with tenths as all the thresholds are in this format
                web.powerwall_battery_percentage = get_int_with_tenths_from_string(battery_percentage); 

                //printf("==> %d\n", web.powerwall_battery_percentage);

                powerwall_num_access_failures = 0;  // <<<<<<<<<<<<<<<<<<<< sucesss so no need to retry
                //powerwall_connection_state = PW_LOGOUT;
            }
            
            //deleberate fallthrough

        case PW_LOGOUT:
            // send logout request to powerwall
            //printf("Sending logout request\n");
            if(!powerwall_logout(pcb, authorization_token, cookie))
            {        
                printf("Failed to send powerwall logout request\n");

                // tear down connection
                tear_down(pcb);
                pcb = NULL;

                powerwall_connection_state = PW_INITIATE;                
                break;
            }
            else
            {
                //printf("Sent login request to https://%s:%d\n", char_ipaddr, LWIP_IANA_PORT_HTTPS);
                //retry = 0;
                powerwall_connection_state = PW_CONFIRM_LOGIN;                
            }
            
            // deliberate fallthrough
            
        case PW_CONFIRM_LOGOUT:  
            // Await status response
            //printf("Awaiting logout response\n");
            //sleep_ms(5000);
            wait_for_packet(5000);
            //printf("Awaited response\n");

            // Response: HTTP/2 204  date: Thu, 03 Oct 2019 13:48:10 GMT
            //printf("Checking response\n");
            if (strcasestr(copy_buffer, "date"))
            {
                //printf("LOGOUT OK\n");
            }
            else
            {
                printf("LOGOUT FAILED\n");
            }

            // deliberate fallthrough
 
        case PW_TEAR_DOWN:
            tear_down(pcb);
            pcb = NULL;

            //retry = 0;  
            powerwall_connection_state = PW_INITIATE;      
            break;

        case PW_LOCKOUT:
            tear_down(pcb);
            pcb = NULL;
           
            // end lockout after 24 hours -- note that the lockout state will also end if the user alters the configuration
            if ((unix_time - powerwall_lockout_start) < POWERWALL_LOCKOUT_TIME)
            {
                 powerwall_connection_state = PW_INITIATE; 
                 powerwall_num_access_failures = 0;
                 access_failure = false;
            }
            break;            
    }

    if (access_failure)
    {
        powerwall_num_access_failures++;
    }

    // reset parser and powerwall status in preparation for next poll
    jsonp_initialize_context(&powerwall_parser_context);
    jsonp_initialize_cache(&powerwall_parser_context);
    sprintf(grid_status, "UNKNOWN");
    sprintf(battery_percentage, "UNKNOWN");
    sprintf(copy_buffer, "XXXXXXXXXXXXXXXXXXXXXXXXX");

    return;
}



/*!
 * \brief Free all resources allocated for https connection
 *
 * \param[in]   pcb       protocol control block
 * 
 * \return nothing
 */
void tear_down(struct altcp_pcb* pcb)
{
    if(pcb)
    {
        
        if (((struct altcp_callback_arg*)(pcb->arg))->config)
        {
            // free connection configuration
            altcp_free_config(((struct altcp_callback_arg*)(pcb->arg))->config);    
        }
        
        if ((struct altcp_callback_arg*)(pcb->arg))
        {
            // free connection callback argument
            altcp_free_arg((struct altcp_callback_arg*)(pcb->arg));                 
        }

        // free connection PCB
        altcp_free_pcb(pcb);                                                    
        
        pcb = NULL;
    }
}


/*!
 * \brief Resolve hostname
 *
 * \param[in]   ipaddr       32-bit IPv4 address
 * 
 * \return nothing
 */
bool resolve_hostname(ip_addr_t* ipaddr)
{

    // Zero address
    ipaddr->addr = IPADDR_ANY;

    // Attempt resolution
    cyw43_arch_lwip_begin();
    lwip_err_t lwip_err = dns_gethostbyname(
        /*PICOHTTPS_HOSTNAME,*/
        config.powerwall_ip,
        ipaddr,
        callback_gethostbyname,
        ipaddr
    );
    cyw43_arch_lwip_end();
    if(lwip_err == ERR_INPROGRESS){

        // Await resolution
        //
        //  IP address will be made available shortly (by callback) upon DNS
        //  query response.
        //
        while(ipaddr->addr == IPADDR_ANY)
            sleep_ms(PICOHTTPS_RESOLVE_POLL_INTERVAL);
        if(ipaddr->addr != IPADDR_NONE)
            lwip_err = ERR_OK;

    }

    // Return
    return !((bool)lwip_err);

}

/*!
 * \brief Free TCP + TLS protocol control block
 *
 * \param[in]   pcb       Protocol Control Block
 * 
 * \return nothing
 */
void altcp_free_pcb(struct altcp_pcb* pcb)
{
    cyw43_arch_lwip_begin();
    lwip_err_t lwip_err = altcp_close(pcb);         // Frees PCB
    cyw43_arch_lwip_end();
    while(lwip_err != ERR_OK)
    {
        sleep_ms(PICOHTTPS_ALTCP_CONNECT_POLL_INTERVAL);
        cyw43_arch_lwip_begin();
        lwip_err = altcp_close(pcb);                // Frees PCB
        cyw43_arch_lwip_end();
    }
}

/*!
 * \brief Free TCP + TLS connection configuration
 *
 * \param[in]   altcp_tls_config       TLS configuration
 * 
 * \return nothing
 */
void altcp_free_config(struct altcp_tls_config* config)
{
    cyw43_arch_lwip_begin();
    altcp_tls_free_config(config);
    cyw43_arch_lwip_end();
}

/*!
 * \brief Free TCP + TLS connection callback argument
 *
 * \param[in]   altcp_callback_arg       Callback
 * 
 * \return nothing
 */
void altcp_free_arg(struct altcp_callback_arg* arg)
{
    if(arg)
    {
        free(arg);
    }
}

/*!
 * \brief Establish TCP + TLS connection with server
 *
 * \param[in]   ipaddr       IPv4 address
 * \param[in]   pcb          Protocol Control Block
 * 
 * \return true if ok, false on error
 */
bool connect_to_host(ip_addr_t* ipaddr, struct altcp_pcb** pcb)
{

    int connection_wait_loop_count = 0;

    // Instantiate connection configuration
    u8_t ca_cert[] = PICOHTTPS_CA_ROOT_CERT;
    cyw43_arch_lwip_begin();
    struct altcp_tls_config* config = altcp_tls_create_config_client(
        /*ca_cert,
        LEN(ca_cert)*/ NULL, 0
    );
    cyw43_arch_lwip_end();
    if(!config) return false;

    // Instantiate connection PCB
    
    //  Can also do this more generically using;
    
    //    altcp_allocator_t allocator = {
    //      altcp_tls_alloc,       // Allocator function
    //      config                 // Allocator function argument (state)
    //    };
    //    altcp_new(&allocator);
    
    //  No benefit in doing this though; altcp_tls_alloc calls altcp_tls_new
    //  under the hood anyway.
    
    cyw43_arch_lwip_begin();
    *pcb = altcp_tls_new(config, IPADDR_TYPE_V4);
    cyw43_arch_lwip_end();
    if(!(*pcb))
    {
        altcp_free_config(config);
        return false;
    }

    // Configure hostname for Server Name Indication extension
    //
    //  Many servers nowadays require clients to support the [Server Name
    //  Indication[wiki-sni] (SNI) TLS extension. In this extension, the
    //  hostname is included in the in the ClientHello section of the TLS
    //  handshake.
    //
    //  Mbed TLS provides client-side support for SNI extension
    //  (`MBEDTLS_SSL_SERVER_NAME_INDICATION` option), but requires the
    //  hostname in order to do so. Unfortunately, the Mbed TLS port supplied
    //  with lwIP (ALTCP TLS) does not currently provide an interface to pass
    //  the hostname to Mbed TLS. This is a [known issue in lwIP][gh-lwip-pr].
    //
    //  As a workaround, the hostname can instead be set using the underlying
    //  Mbed TLS interface (viz. `mbedtls_ssl_set_hostname` function). This is
    //  somewhat inelegant as it tightly couples our application code to the
    //  underlying TLS library (viz. Mbed TLS). Given that the Pico SDK already
    //  tightly couples us to lwIP though, and that any fix is unlikely to be
    //  backported to the lwIP version in the Pico SDK, this doesn't feel like
    //  too much of a crime…
    //
    //  [wiki-sni]: https://en.wikipedia.org/wiki/Server_Name_Indication
    //  [gh-lwip-pr]: https://github.com/lwip-tcpip/lwip/pull/47/commits/c53c9d02036be24a461d2998053a52991e65b78e
    //
    cyw43_arch_lwip_begin();
    mbedtls_err_t mbedtls_err = mbedtls_ssl_set_hostname(
        &(
            (
                (altcp_mbedtls_state_t*)((*pcb)->state)
            )->ssl_context
        ),
        PICOHTTPS_HOSTNAME
    );
    cyw43_arch_lwip_end();
    if(mbedtls_err){
        altcp_free_pcb(*pcb);
        altcp_free_config(config);
        return false;
    }

    // Configure common argument for connection callbacks
    //
    //  N.b. callback argument must be in scope in callbacks. As callbacks may
    //  fire after current function returns, cannot declare argument locally,
    //  but rather should allocate on the heap. Must then ensure allocated
    //  memory is subsequently freed.
    //
    struct altcp_callback_arg* arg = malloc(sizeof(*arg));
    if(!arg){
        altcp_free_pcb(*pcb);
        altcp_free_config(config);
        return false;
    }
    arg->config = config;
    arg->connected = false;
    cyw43_arch_lwip_begin();
    altcp_arg(*pcb, (void*)arg);
    cyw43_arch_lwip_end();

    // Configure connection fatal error callback
    cyw43_arch_lwip_begin();
    altcp_err(*pcb, callback_altcp_err);
    cyw43_arch_lwip_end();

    // Configure idle connection callback (and interval)
    cyw43_arch_lwip_begin();
    altcp_poll(
        *pcb,
        callback_altcp_poll,
        PICOHTTPS_ALTCP_IDLE_POLL_INTERVAL
    );

    //TODO newman found this on internet for non-tls connection
    //pcb->keep_intvl = 1000; // send "keep-alive" every 1000ms

    cyw43_arch_lwip_end();

    // Configure data acknowledge callback
    cyw43_arch_lwip_begin();
    altcp_sent(*pcb, callback_altcp_sent);
    cyw43_arch_lwip_end();

    // Configure data reception callback
    cyw43_arch_lwip_begin();
    altcp_recv(*pcb, callback_altcp_recv);
    cyw43_arch_lwip_end();

    // Send connection request (SYN)
    cyw43_arch_lwip_begin();
    lwip_err_t lwip_err = altcp_connect(
        *pcb,
        ipaddr,
        LWIP_IANA_PORT_HTTPS,
        callback_altcp_connect
    );
    cyw43_arch_lwip_end();

    // Connection request sent
    if(lwip_err == ERR_OK)
    {

        // Await connection
        //
        //  Sucessful connection will be confirmed shortly in
        //  callback_altcp_connect.
        //
        connection_wait_loop_count = 0;
        while(!(arg->connected))
        {
            sleep_ms(PICOHTTPS_ALTCP_CONNECT_POLL_INTERVAL);
            
            if (connection_wait_loop_count++ > 1000)
            {
                // failed to connect
                lwip_err = ERR_TIMEOUT;

                // Free allocated resources -- we are FUBAR so gamble: leak memory or use after free
                altcp_free_pcb(*pcb);
                altcp_free_config(config);
                altcp_free_arg(arg);                
                break;
            } 
        }                                
            

    } 
    else
    {

        // Free allocated resources
        altcp_free_pcb(*pcb);
        altcp_free_config(config);
        altcp_free_arg(arg);

    }

    //Return
    return !((bool)lwip_err);

}

/*!
 * \brief DNS response callback
 *
 * \param[in]   name       name to resolve
 * \param[in]   resolved   result
 * \param[in]   ipaddr     IPv4 address
 * 
 * \return nothing
 */
void callback_gethostbyname(
    const char* name,
    const ip_addr_t* resolved,
    void* ipaddr)
{
    if(resolved) *((ip_addr_t*)ipaddr) = *resolved;         // Successful resolution
    else ((ip_addr_t*)ipaddr)->addr = IPADDR_NONE;          // Failed resolution
}


/*!
 * \brief TCP + TLS connection error callback
 *
 * \param[in]   arg   argument
 * \param[in]   err   error code
 * 
 * \return nothing
 */
void callback_altcp_err(void* arg, lwip_err_t err)
{

    // Print error code
    printf("Connection error [lwip_err_t err == %d]\n", err);

    // Free ALTCP TLS config
    if( ((struct altcp_callback_arg*)arg)->config )
        altcp_free_config( ((struct altcp_callback_arg*)arg)->config );

    // Free ALTCP callback argument
    altcp_free_arg((struct altcp_callback_arg*)arg);

}

/*!
 * \brief TCP + TLS connection idle callback
 *
 * \param[in]   arg   argument
 * \param[in]   pcb   Protocol Control Block
 * 
 * \return 0 on success or error code
 */
lwip_err_t callback_altcp_poll(void* arg, struct altcp_pcb* pcb)
{
    // Callback not currently used
    return ERR_OK;
}


/*!
 * \brief TCP + TLS data acknowledgement callback
 *
 * \param[in]   arg   argument
 * \param[in]   pcb   Protocol Control Block
 * \param[in]   len   length
 * 
 * \return 0 on success or error code
 */
lwip_err_t callback_altcp_sent(void* arg, struct altcp_pcb* pcb, u16_t len)
{
    ((struct altcp_callback_arg*)arg)->acknowledged = len;
    return ERR_OK;
}

/*!
 * \brief TCP + TLS data reception callback
 *
 * \param[in]   arg   argument
 * \param[in]   pcb   Protocol Control Block
 * \param[in]   buf   buffer
 * \param[in]   err   error code
 * 
 * \return 0 on success or error code
 */
lwip_err_t callback_altcp_recv(
    void* arg,
    struct altcp_pcb* pcb,
    struct pbuf* buf,
    lwip_err_t err)
{

    // Store packet buffer at head of chain
    //
    //  Required to free entire packet buffer chain after processing.
    //
    struct pbuf* head = buf;

    switch(err){

        // No error receiving
        case ERR_OK:

            // Handle packet buffer chain
            //
            //  * buf->tot_len == buf->len — Last buf in chain
            //    * && buf->next == NULL — last buf in chain, no packets in queue
            //    * && buf->next != NULL — last buf in chain, more packets in queue
            //

            if(buf){

                // Print packet buffer
                u16_t i;
                int copy_index = 0;
                while(buf->len != buf->tot_len)
                {
                    for(i = 0; i < buf->len; i++)
                    {
                        //putchar(((char*)buf->payload)[i]);
                        if (copy_index < sizeof(copy_buffer)) copy_buffer[copy_index++] = ((char*)buf->payload)[i];
                    } 
                    buf = buf->next;
                }
                for(i = 0; i < buf->len; i++)
                {
                    //putchar(((char*)buf->payload)[i]);
                    if (copy_index < sizeof(copy_buffer)) copy_buffer[copy_index++] = ((char*)buf->payload)[i];
                } 
                copy_buffer[copy_index] = 0; // ensure zero termination
                copy_ready++;
                assert(buf->next == NULL);

                // Advertise data reception
                altcp_recved(pcb, head->tot_len);

            }

            // …fall-through…

        case ERR_ABRT:

            // Free buf
            pbuf_free(head);        // Free entire pbuf chain

            // Reset error
            err = ERR_OK;           // Only return ERR_ABRT when calling tcp_abort()

    }

    //printf("\nRCV CALLBACK COMPLETED err = %d\n", err);
    // Return error
    return err;

}

/*!
 * \brief TCP + TLS connection establishment callback
 *
 * \param[in]   arg   argument
 * \param[in]   pcb   Protocol Control Block
 * \param[in]   err   error code
 * 
 * \return 0 on success or error code
 */
lwip_err_t callback_altcp_connect(
    void* arg,
    struct altcp_pcb* pcb,
    lwip_err_t err)
{
    ((struct altcp_callback_arg*)arg)->connected = true;
    return ERR_OK;
}

/*!
 * \brief TCP + TLS connection establishment callback
 * 
 * \return true if ok, false on error
 */
int powerwall_init(void)
{
    jsonp_initialize_context(&powerwall_parser_context);
    jsonp_initialize_cache(&powerwall_parser_context);
    // test_http(1);
    // jsonp_dump_key_value_pairs(); 

    return(0); 
}


/*!
 * \brief Send HTTP request
 *
 * \param[in]   pcb        Protocol Control Block
 * \param[in]   type       HTTP request type
 * \param[in]   url        Universal Resource Locator 
 * \param[in]   host       hostname
 * \param[in]   content    http request
 * \param[in]   auth_token authentication token
 * \param[in]   cookies    cookies
 * 
 * \return false on success
 */
bool http_request(struct altcp_pcb* pcb, HTTP_REQUEST_TYPE_T type, char *url, char *host, char *content, char *auth_token, char *cookies)
{
    bool err = false;
    char request[2048];
    int length = 0;
    char length_string[8];
    lwip_err_t lwip_err = -1;
   

    // type
    switch(type)
    {
        case HTTP_GET:
            sprintf(request, "GET ");
            break;
        case HTTP_POST:
            sprintf(request, "POST ");
            break;
        default:
            request[0] = 0;
            err = true;
    }

    // url
    STRAPPEND(request, url);
    STRAPPEND(request, " HTTP/1.1\r\n");

    // host
    STRAPPEND(request, "Host: ");
    STRAPPEND(request, host);
    STRAPPEND(request, "\r\n");

    // accept
    STRAPPEND(request, "Accept: */*\r\n");

    // cookies
    if (cookies)
    {
        STRAPPEND(request, "Cookie: ");
        STRAPPEND(request, cookies);
        STRAPPEND(request, "\r\n");        
    }

    // authorization token
    if (auth_token)
    {
        STRAPPEND(request, "Authorization: Bearer ");
        STRAPPEND(request, auth_token);
        STRAPPEND(request, "\r\n");
    }

    // content
    if (content)
    {
        sprintf(length_string, "%d", strlen(content));

        STRAPPEND(request, "Content-Type: application/json\r\n");
        STRAPPEND(request, "Content-Length: ");
        STRAPPEND(request, length_string);
        STRAPPEND(request, "\r\n\r\n");
        STRAPPEND(request, content);
    }
    STRAPPEND(request, "\r\n");

    // request length 
    length = strlen(request) + 1; // TODO figure out WTF the original code is doing by transmitting one less

    if(!err)
    {
        //printf("SEND [%d bytes]\n%s\n", length, request); 
        
        // Check send buffer and queue length
        //
        //  Docs state that altcp_write() returns ERR_MEM on send buffer too small
        //  _or_ send queue too long. Could either check both before calling
        //  altcp_write, or just handle returned ERR_MEM — which is preferable?
        //
        if(
        altcp_sndbuf(pcb) < (length - 1)
        || altcp_sndqueuelen(pcb) > TCP_SND_QUEUELEN
        ) return -1;

        // Write to send buffer
        cyw43_arch_lwip_begin();
        lwip_err = altcp_write(pcb, request, length -1, 0);
        cyw43_arch_lwip_end();

        // Written to send buffer
        if(lwip_err == ERR_OK){

            // Output send buffer
            ((struct altcp_callback_arg*)(pcb->arg))->acknowledged = 0;
            cyw43_arch_lwip_begin();
            lwip_err = altcp_output(pcb);
            cyw43_arch_lwip_end();

            // Send buffer output
            if(lwip_err == ERR_OK){

                // Await acknowledgement
                while(
                    !((struct altcp_callback_arg*)(pcb->arg))->acknowledged
                ) sleep_ms(PICOHTTPS_HTTP_RESPONSE_POLL_INTERVAL);
                if(
                    ((struct altcp_callback_arg*)(pcb->arg))->acknowledged
                    != (length - 1)
                ) lwip_err = -1;

            }

        }
    }
    // Return
    return !((bool)lwip_err);

}

/*!
 * \brief Send login request to powerwall
 *
 * \param[in]   pcb        Protocol Control Block
 * 
 * \return 0 on success
 */
int powerwall_login(struct altcp_pcb* pcb)
{
    int err = 0;
    char content[256];

    sprintf(content, "{\"username\":\"customer\",\"password\":\"%s\"}", config.powerwall_password);

    err = http_request(pcb, HTTP_POST, "/api/login/Basic", config.powerwall_hostname, content, NULL, NULL);

    return(err);
}


/*!
 * \brief Send grid status request to powerwall
 *
 * \param[in]   pcb             Protocol Control Block
 * \param[in]   auth_token      authentication token
 * \param[in]   cookies         cookies
 * 
 * \return 0 on success
 */
int powerwall_get_grid_status(struct altcp_pcb* pcb, char *auth_token, char *cookies)
{
    int err = 0;   

    err = http_request(pcb, HTTP_GET, "/api/system_status/grid_status", config.powerwall_hostname, NULL, auth_token, cookies);
    
    return(err);
}

/*!
 * \brief Send battery level request to powerwall
 *
 * \param[in]   pcb             Protocol Control Block
 * \param[in]   auth_token      authentication token
 * \param[in]   cookies         cookies
 * 
 * \return 0 on success
 */
int powerwall_get_battery_percentage(struct altcp_pcb* pcb, char *auth_token, char *cookies)
{
    int err = 0;   

    err = http_request(pcb, HTTP_GET, "/api/system_status/soe", config.powerwall_hostname, NULL, auth_token, cookies);
    
    return(err);
}

/*!
 * \brief Send battery logout request to powerwall
 *
 * \param[in]   pcb             Protocol Control Block
 * \param[in]   auth_token      authentication token
 * \param[in]   cookies         cookies
 * 
 * \return 0 on success
 */
int powerwall_logout(struct altcp_pcb* pcb, char *auth_token, char *cookies)
{
    int err = 0;   

    err = http_request(pcb, HTTP_GET, "/api/logout", config.powerwall_hostname, NULL, auth_token, cookies);
    
    return(err);
}

/*!
 * \brief Send battery logout request to powerwall
 *
 * \param[in]   http_packet    received packet
 * \param[in]   cookies        buffer to store cookies
 * \param[in]   length         cookie buffer length
 * 
 * \return 0 on success
 */
int http_extract_cookies(const char *http_packet, char *cookies, int length)
{
    int err = 0;
    char *source = NULL;
    int i = 0;
    int j = 0;

    // check paramters are sane
    if (!http_packet || !cookies || (length < 1))
    {
        err = 1;
    }
    else
    {
        // source pointer to walk through packet
        source = (char *)http_packet;

        // concatenate cookies found in http header
        do
        {
            source = strcasestr(source, "Set-Cookie: ");
            if (source)
            {
                //printf("GOT COOKIE: ");

                // inject a space between cookies
                if (j > 0)
                {
                    cookies[j++] = ' ';
                    cookies[j] = 0;
                }

                // copy cookie
                source += strlen("Set-Cookie: ");
                for (i=0; source[i] && (j < (length - 1)); i++, j++)
                {
                    //printf("%c", source[i]);
                    cookies[j] = source[i];

                    if (source[i] == ';')
                    {
                        // terminate string after semi-colon
                        cookies[++j] = 0;
                        break;
                    } 
                }

                //printf("\n"); 
                
                // move source pointer past copied cookie
                source += i;
            }        
        } while (source && (j < (length - 1)));
    
        // remove final semi-colon
        if (j > 0)
        {
            cookies[j-1] = 0;
        } 
    }

    return(err);
}

/*!
 * \brief Wait for a received packet to be ready for processing with a timeout
 *
 * \param[in]   ticks    maximum wait time
 * 
 * \return 0 on success
 */
int wait_for_packet(TickType_t ticks)
{
    int err = 1;   
    TickType_t wait_time = 0;
    static int last_copy_ready = 0;

    do 
    {
        if (last_copy_ready != copy_ready)
        {
            last_copy_ready = copy_ready;
            err = 0;
            break;
        }

        SLEEP_MS(50);
        wait_time += 50;
    } while(wait_time < ticks);
    
    return(err);
}

/*!
 * \brief Sanitize powerwall configuration
 * 
 * \return 0 on success
 */
int powerwall_sanitize_config(void)
{
    // ensure strings are terminated
    config.powerwall_ip[sizeof(config.powerwall_ip)-1] = 0;
    config.powerwall_hostname[sizeof(config.powerwall_hostname)-1] = 0;
    config.powerwall_password[sizeof(config.powerwall_password)-1] = 0;
    
    return(0);
}

/*!
 * \brief Terminate powerwall locout
 * 
 * \return 0 on success
 */
int powerwall_terminate_lockout(void)
{
    powerwall_lockout_start = unix_time - POWERWALL_LOCKOUT_TIME;
}


/*!
 * \brief Remove first and last double quotes if present.  Alteration occurs within the  passed buffer.
 *
 * \param[in]   token       buffer containing string
 * \param[in]   length      length of buffer
 * 
 * \return 0 if no quotes removed, 1 if quotes removed
 */
int strip_first_last_quotes(char *token, int length)
{
    int err = 0;
    int i;

    if (token && (length > 2)) 
    {
        // we presume that first and last characters are double quotes
        if (token[0] == '"')
        {
            //printf("start quote strip length = %d\n", length);
            for (i=0; i < length-2; i++)
            {
                //printf("checking: %d[%c][%c]\n", i, token[i+1], token[i+2]);
                if (((token[i+1] == '"') && (token[i+2] == 0)) ||
                     (token[i+1] == 0))
                {
                    token[i] = 0;
                    err = 1;
                    break;
                }
                else
                {
                    token[i] = token[i+1];
                }
            }
        }
    }

    return(err);
}