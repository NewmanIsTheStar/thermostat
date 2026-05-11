/**
 * Copyright (c) 2025 NewmanIsTheStar
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

#include "FreeRTOS.h"
#include "FreeRTOSConfig.h"
#include "task.h"
#include "timers.h"
#include "queue.h"

#include "stdarg.h"

#include "watchdog.h"
#include "weather.h"
#include "thermostat.h"
#include "flash.h"
#include "calendar.h"
#include "utility.h"
#include "config.h"
#include "led_strip.h"
#include "message.h"
#include "altcp_tls_mbedtls_structs.h"
#include "powerwall.h"
#include "config.h"
#include "pluto.h"
#include "tm1637.h"

// defines
#define SIZE_CLIMATE_HISTORY (100)          
#define SIZE_TREND_WINDOW (10)
#define LAG_MIN_REPORTABLE_TEMP_DELTA (2) 

typedef enum
{
    HEATING_DATASET = 0,
    COOLING_DATASET = 1,
    STABLE_DATASET  = 2,    
    NUM_DATASETS    = 3
} CLIMATE_DATASET_T;

typedef enum
{
    NEGATIVE_DELTA = 0,
    POSITIVE_DELTA = 1,
    SMALL_DELTA   = 2,
    NUM_DELTAS     = 3    
} CLIMATE_DELTA_CATAGORY_T;

typedef struct
{
    CLIMATE_DATAPOINT_T buffer[SIZE_CLIMATE_HISTORY];
    int buffer_index;
    int buffer_population;
} CLIMATE_HISTORY_T;

typedef enum
{
    TREND_DOWN    = 0,
    TREND_UP      = 1,
    TREND_STABLE  = 2,    
    NUM_TRENDS    = 3
} CLIMATE_TREND_DIRECTION_T;

typedef struct
{
    CLIMATE_DATAPOINT_T sample_buffer[SIZE_TREND_WINDOW];
    CLIMATE_DATAPOINT_T delta_buffer[SIZE_TREND_WINDOW];    
    int buffer_index;
    int buffer_population;
    CLIMATE_DATAPOINT_T moving_average;
    long int gradient;
    int deltas[NUM_DELTAS];
    CLIMATE_TREND_DIRECTION_T current_trend;
    TickType_t trend_start_tick;
    TickType_t current_tick;
    TickType_t trend_length;
    int trend_up_max;
    int trend_down_min;
} CLIMATE_TREND_T;

typedef struct
{
    TickType_t hvac_off_tick[NUM_MOMENTUMS];
    long int hvac_off_temperature[NUM_MOMENTUMS];      
    bool measurement_in_progress[NUM_MOMENTUMS];
    TickType_t extrema_delay[NUM_MOMENTUMS]; 
    long int extrema_temperature[NUM_MOMENTUMS];
    TickType_t lag_delay[NUM_MOMENTUMS];    
    long int  lag_temperature_delta[NUM_MOMENTUMS];    
} CLIMATE_LAG_DATA_T;

// prototypes
int update_history_buffer(CLIMATE_DATAPOINT_T *sample);
int update_trend_buffer(CLIMATE_DATAPOINT_T *sample, CLIMATE_DATAPOINT_T *previous_sample);
int get_climate_history_buffer_index(int num_samples_in_past);
int get_climate_trend_buffer_index(int num_samples_in_past);
int smooth_temperature_history(void);

// external variables
extern uint32_t unix_time;
extern WEB_VARIABLES_T web;
extern NON_VOL_VARIABLES_T config;

// gobal variables
static CLIMATE_HISTORY_T climate_history;
static CLIMATE_LAG_DATA_T climate_lag;
static CLIMATE_TREND_T climate_trend;


/*!
 * \brief Predict time until temperature reached
 * 
 * \return 0 
 */
int predicted_time_to_temperature(long int target_temperature)
{
    CLIMATE_DATAPOINT_T latest_sample;
    long int temperature_delta = 0;
    long int predicted_time_in_samples = 0;

    // get a copy of the latest sample from the history buffer
    latest_sample = climate_history.buffer[(climate_history.buffer_index + NUM_ROWS(climate_history.buffer) - 1)%NUM_ROWS(climate_history.buffer)];
    temperature_delta = target_temperature - latest_sample.temperaturex10;

    if (temperature_delta == 0)
    {
        printf("target temperature reached\n");
    }
    else
    {

        predicted_time_in_samples = (temperature_delta*100)/climate_trend.gradient; 
        web.thermostat_temperature_prediction = predicted_time_in_samples;        

        if (predicted_time_in_samples < 0)
        {
            printf("PREDICTION: temperature will *never* reach target %d\n", target_temperature);
        }
        else
        {
            printf("PREDICTION: temperature will reach %d in %d samples (%d seconds)\n", target_temperature, predicted_time_in_samples, predicted_time_in_samples*THERMOSTAT_TASK_LOOP_DELAY/1000);
        }
    }

    return(predicted_time_in_samples);
}

/*!
 * \brief Initialize Temperature Metrics
 * 
 * \return 0 
 */
int initialize_climate_metrics(void)
{
    memset(&climate_history, 0, sizeof(climate_history));
    memset(&climate_lag, 0, sizeof(climate_lag));
    memset(&climate_trend, 0, sizeof(climate_trend));
    climate_trend.trend_up_max   = -500;   // -50.0  Celcius
    climate_trend.trend_down_min = -1500;  // +150.0 Celcius

    return(0);
}

/*!
 * \brief Accumulate Climate Metrics
 * 
 * \return 0
 */
// TODO: should be using time_t
int accumlate_metrics(uint32_t unix_time, long int temperaturex10, long int humidityx10)
{
    static CLIMATE_DATAPOINT_T previous_sample = {0,0,0};
    CLIMATE_DATAPOINT_T new_sample; 

    // create sample
    new_sample.unix_time = unix_time;
    new_sample.temperaturex10 = temperaturex10;
    new_sample.humidityx10 = humidityx10;

    // remove noise from the temperature
    //new_sample.temperaturex10 = filter_temperature_noise(new_sample.temperaturex10);

    // add new temperature and humidity to trend buffer
    update_trend_buffer(&new_sample, &previous_sample);

    // add new sample to history buffer if temperature has changed or we have less than two samples in the buffer (we need at least 2 points to plot a flat line)
    if ((new_sample.temperaturex10 != previous_sample.temperaturex10) || climate_history.buffer_population <= 2)
    {
        // remember the sample for next time
        previous_sample = new_sample;

        // add new temperature and humidity to history buffer
        update_history_buffer(&new_sample);

        // smooth out single sample trend reversals by replacing with straight line
        smooth_temperature_history();    
    }
    else
    {
        // check if we have two sequential samples with the same temperature (i.e. a flat line graph)
        if (climate_history.buffer[get_climate_history_buffer_index(2)].temperaturex10 == climate_history.buffer[get_climate_history_buffer_index(1)].temperaturex10)
        {
            // save buffer space by updating the previous sample rather than storing a new sample with the same temperature
            climate_history.buffer[get_climate_history_buffer_index(1)] = new_sample;

            // remember the sample for next time
            previous_sample = new_sample;        
        }
        else
        {
            // remember the sample for next time
            previous_sample = new_sample;

            // add new temperature and humidity to history buffer
            update_history_buffer(&new_sample);            
        }
    }

    return(0);
}


/*!
 * \brief Add new temperature and humidity sample to history buffer
 * 
 * \return 0 
 * 
 */
int update_history_buffer(CLIMATE_DATAPOINT_T *sample)
{
    // store temperature and timestamp in history buffer
    climate_history.buffer[climate_history.buffer_index] = *sample;
   
    // increment index for history buffer
    climate_history.buffer_index  = (climate_history.buffer_index  + 1)%NUM_ROWS(climate_history.buffer);    

    // update population for history buffer
    if (climate_history.buffer_population < NUM_ROWS(climate_history.buffer)) climate_history.buffer_population++;  

    return(0);
}

/*!
 * \brief Compute moving average, gradient and determine trend
 * 
 * \return moving_average temperature
 */
int update_trend_buffer(CLIMATE_DATAPOINT_T *sample, CLIMATE_DATAPOINT_T *previous_sample)
{
    int i;
    long int gradient = 0;
    char timestamp[32];
   
    // store sample in sample_buffer
    climate_trend.sample_buffer[climate_trend.buffer_index] = *sample;

   // compute delta and store in delta buffer
    climate_trend.delta_buffer[climate_trend.buffer_index].unix_time = sample->unix_time;
    if (climate_trend.buffer_population >= 1)
    {
        climate_trend.delta_buffer[climate_trend.buffer_index].temperaturex10 = sample->temperaturex10 - previous_sample->temperaturex10;
        climate_trend.delta_buffer[climate_trend.buffer_index].humidityx10    = sample->humidityx10    - previous_sample->humidityx10;
    }
    else
    {
        // first sample has zero delta
        climate_trend.delta_buffer[climate_trend.buffer_index].temperaturex10 = 0;
        climate_trend.delta_buffer[climate_trend.buffer_index].humidityx10    = 0;        
    }
        
    // increment index for trend buffers
    climate_trend.buffer_index  = (climate_trend.buffer_index  + 1)%NUM_ROWS(climate_trend.delta_buffer);    

    // update population for trend buffers
    if (climate_trend.buffer_population < NUM_ROWS(climate_trend.delta_buffer)) climate_trend.buffer_population++;    

    // compute moving average and gradient TODO: humidity
    climate_trend.moving_average.temperaturex10 = 0;
    gradient = 0;
    for(i=0; i < climate_trend.buffer_population; i++)
    {
        climate_trend.moving_average.temperaturex10 += climate_trend.sample_buffer[i].temperaturex10; 
        gradient += climate_trend.delta_buffer[i].temperaturex10;
    }
    climate_trend.moving_average.temperaturex10 = climate_trend.moving_average.temperaturex10/climate_trend.buffer_population;
    climate_trend.moving_average.unix_time = unix_time;
    climate_trend.gradient = gradient*10/climate_trend.buffer_population;

    // print temperature and gradient
    get_timestamp_from_unix_time(climate_trend.moving_average.unix_time, timestamp, NUM_ROWS(timestamp), 0, 1);
    printf("%s     Temperature %c%ld.%ld degrees     Gradient %c%ld.%02ld degrees per sample\n", timestamp, climate_trend.moving_average.temperaturex10<0?'-':' ', abs(climate_trend.moving_average.temperaturex10)/10, abs(climate_trend.moving_average.temperaturex10%10), climate_trend.gradient<0?'-':' ', abs(climate_trend.gradient)/100, abs(climate_trend.gradient%100));
    
    // update web interface TODO: should web variables be long int?
    web.thermostat_temperature_moving_average = climate_trend.moving_average.temperaturex10;
    web.thermostat_temperature_gradient = climate_trend.gradient;
   
    // zero delta counters
    climate_trend.deltas[NEGATIVE_DELTA] = 0;
    climate_trend.deltas[POSITIVE_DELTA] = 0;
    climate_trend.deltas[SMALL_DELTA] = 0;

    // count deltas
    for(i=0; i < climate_trend.buffer_population; i++)
    {
        if (climate_trend.delta_buffer[i].temperaturex10 < 0) climate_trend.deltas[NEGATIVE_DELTA]++;
        if (climate_trend.delta_buffer[i].temperaturex10 > 0) climate_trend.deltas[POSITIVE_DELTA]++;
        if (abs(climate_trend.delta_buffer[i].temperaturex10) < 5) climate_trend.deltas[SMALL_DELTA]++;
    }

    //printf("Temp Sample = %d\tMoving Average = %d [%d, %d, %d] ", temperaturex10, climate_trend.moving_average, climate_trend.deltas[NEGATIVE_DELTA], climate_trend.deltas[POSITIVE_DELTA], climate_trend.deltas[SMALL_DELATA]);

    if ((climate_trend.deltas[NEGATIVE_DELTA]>= 1) && (climate_trend.deltas[POSITIVE_DELTA] == 0))
    {
        //printf("Trending down @ %ld per sample\n", gradient);
        if (climate_trend.current_trend != TREND_DOWN)
        {
            climate_trend.current_tick = xTaskGetTickCount();  //TODO: use timestamp from the sample?
            climate_trend.trend_length = climate_trend.current_tick - climate_trend.trend_start_tick;
            //printf("Trend changed at tick %lu.  Trend lasted %lu. Maximum = %d\n", climate_trend.current_tick, climate_trend.trend_length, climate_trend.trend_up_max);

            climate_trend.current_trend = TREND_DOWN;
            climate_trend.trend_down_min = 1500;
            climate_trend.trend_start_tick = climate_trend.current_tick;
        }
        else
        {
            // remember lowest temperature
            if (sample->temperaturex10 < climate_trend.trend_down_min)
            {
                climate_trend.trend_down_min = sample->temperaturex10;
            }
        }
    } 
    else if ((climate_trend.deltas[POSITIVE_DELTA] >= 1) && (climate_trend.deltas[NEGATIVE_DELTA] == 0))
    {
        //printf("Trending up @ %ld per sample\n", gradient);
        if (climate_trend.current_trend != TREND_UP)
        {
            climate_trend.current_tick = xTaskGetTickCount();  //TODO: use timestamp from the sample?
            climate_trend.trend_length = climate_trend.current_tick - climate_trend.trend_start_tick;
            //printf("Trend changed at tick %lu.  Trend lasted %lu. Minimum = %d\n", climate_trend.current_tick, climate_trend.trend_length, climate_trend.trend_down_min);

            climate_trend.current_trend  = TREND_UP;
            climate_trend.trend_up_max = -500;
            climate_trend.trend_start_tick = climate_trend.current_tick;
        }   
        else
        {
            if (sample->temperaturex10 > climate_trend.trend_up_max)
            {
                climate_trend.trend_up_max = sample->temperaturex10;
            }
        }          
    }
    else if ((climate_trend.deltas[SMALL_DELTA] == NUM_ROWS(climate_trend.delta_buffer)) && (abs(climate_trend.deltas[NEGATIVE_DELTA]-climate_trend.deltas[POSITIVE_DELTA]) < 3))
    {
        //printf("Stable\n");   // no change to trend as we allow the previous trend to continue after a plateau    
    }
    else
    {
        //printf("No trend detected\n");
    } 

    return(climate_trend.moving_average.temperaturex10);
}


/*!
 * \brief Record when hvac heating or cooling ended
 * 
 * \return nothing
 */
void mark_hvac_off(CLIMATE_LAG_T lag_type, long int temperaturex10)
{
    climate_lag.hvac_off_tick[lag_type] = xTaskGetTickCount();
    climate_lag.hvac_off_temperature[lag_type] = temperaturex10;    
    climate_lag.extrema_delay[lag_type]= 0;
    climate_lag.extrema_temperature[lag_type]= 0;    
    climate_lag.measurement_in_progress[lag_type] = true;
}

/*!
 * \brief Track how long heating or cooling continued after hvac stopped
 * 
 * \return nothing
 */
void track_hvac_extrema(CLIMATE_LAG_T lag_type, long int temperaturex10)
{
    if (climate_lag.measurement_in_progress[lag_type])
    {
        switch(lag_type)
        {
        case HEATING_LAG:
            if (temperaturex10 > climate_lag.extrema_temperature[lag_type])
            {
                climate_lag.extrema_delay[lag_type] = xTaskGetTickCount() - climate_lag.hvac_off_tick[lag_type];
                //climate_lag.extrema_temperature[lag_type] =  temperaturex10 - climate_lag.hvac_off_temperature[lag_type];
                climate_lag.extrema_temperature[lag_type] =  temperaturex10;                

            }
            break;
        case COOLING_LAG:
            if (temperaturex10 < climate_lag.extrema_temperature[lag_type])
            {
                climate_lag.extrema_delay[lag_type] = xTaskGetTickCount() - climate_lag.hvac_off_tick[lag_type];
                //climate_lag.extrema_temperature[lag_type] =  temperaturex10 - climate_lag.hvac_off_temperature[lag_type];
                climate_lag.extrema_temperature[lag_type] =  temperaturex10;
            }    
            break;
        default:
            break;
        }
    }
}

/*!
 * \brief Set lag based on tracked extrema
 * 
 * \return nothing
 */
void set_hvac_lag(CLIMATE_LAG_T lag_type)
{
    if (climate_lag.measurement_in_progress[lag_type])
    {
        climate_lag.lag_delay[lag_type] = climate_lag.extrema_delay[lag_type];
        climate_lag.lag_temperature_delta[lag_type] =  climate_lag.extrema_temperature[lag_type] - climate_lag.hvac_off_temperature[lag_type]; 
        climate_lag.measurement_in_progress[lag_type] = false;

        // report lag if temperature trend continued for more than 0.2 degrees after HVAC shutoff
        if (abs(climate_lag.lag_temperature_delta[lag_type]) > LAG_MIN_REPORTABLE_TEMP_DELTA)
        {
            send_syslog_message("lag", "Type = %s Time Lag = %ld ms Temperature Lag = %c%ld.%ld degrees\n", lag_type?"Heating":"Cooling", climate_lag.lag_delay[lag_type], climate_lag.lag_temperature_delta[lag_type]<0?'-':' ', abs(climate_lag.lag_temperature_delta[lag_type]/10), abs(climate_lag.lag_temperature_delta[lag_type]%10));        
        }
    }
}

/*!
 * \brief Send a climate syslog message
 *
 * \param[in]   log_name      name of log file on server
 * \param[in]   format, ...   variable parameters printf style  
 * 
 * \return num bytes sent or -1 on error
 */
void log_climate_change(int temperaturex10, int humidityx10)
{
    static int sent_temperaturex10 = 0;
    static int sent_humidityx10 = 0;

    // check if values changed
    if ((temperaturex10 != sent_temperaturex10) || (humidityx10 != sent_humidityx10))
    {
        send_syslog_message("temperature", "Temperature = %c%ld.%ld Humidity = %ld.%ld\n", temperaturex10<0?'-':' ', abs(temperaturex10/10), abs(temperaturex10%10), humidityx10/10, humidityx10%10);

        // remember what we sent
        sent_temperaturex10 = temperaturex10;
        sent_humidityx10 = humidityx10;
    }
    
    return;
}

/*!
 * \brief print temperature history for insertion into web page  
 *
 * \param[out]  buffer           pointer to output buffer
 * \param[in]   length           size of output buffer  
 * \param[in]   start_position   offset into buffer to place output
 * \param[in]   num_data_points  max number of (time, temperature) pairs to output  
 * \return number of bytes printed
 */
int print_temperature_history(char *buffer, int length, int start_position, int num_data_points)
{
    int i;
    int history_index = 0;
    int printed_characters = 0;
    int total_printed_characters = 0;
    char iso_timestamp[32];
    char *buff = 0;

    // output xy data in format expected by javascript e.g. { x: '2025-10-01T08:00:00', y: 65 },
    buff = buffer;
    *buffer = 0;

    if (start_position < climate_history.buffer_population)
    {
        // limit data points to number in the circular buffer
        if ((start_position + num_data_points) > climate_history.buffer_population)
        {
            num_data_points = climate_history.buffer_population - start_position;
        }

        if (num_data_points > 0)
        {
            for(i=0; i < num_data_points; i++)
            {
                if (climate_history.buffer_population == NUM_ROWS(climate_history.buffer))
                {
                    // circular buffer is full, so oldest entry is the current buffer index (that we will overwrite next with new data)
                    history_index = (climate_history.buffer_index + start_position + i)%climate_history.buffer_population;
                }
                else
                {
                    // circular buffer not full, so oldest entry is at buffer index 0
                    history_index = start_position + i;   
                    
                    //printf("Circular buffer not full because %d != %d\n", climate_history.buffer_population, NUM_ROWS(climate_history.buffer));
                }

                // generatre timestamp string
                unix_to_iso8601(climate_history.buffer[history_index].unix_time, iso_timestamp, sizeof(iso_timestamp)); 

                // print xy data
                printed_characters = snprintf(buffer, length, "{x: '%s', y: %d.%d },\n", iso_timestamp, climate_history.buffer[history_index].temperaturex10/10, climate_history.buffer[history_index].temperaturex10%10);
                //printf("i=%d history_index=%d time=%s\n", i, history_index, iso_timestamp);
                if (printed_characters < length)
                {
                    total_printed_characters += printed_characters;
                    length -= printed_characters;
                    buffer += printed_characters;
                }
                else
                {
                    // hit the end of buffer truncate at START of last print attempt (erasing the last line printed)
                    *buffer = 0;
                    printf("BUFFER TERMINATED due to excess length\n%s\n", buff);
                    break;
                }
            }
        }
    }



    return(total_printed_characters);
}
        
/*!
 * \brief Filter out noise in temperature readings
 * 
 * \return temperature with small deviations from moving average removed
 */
int filter_temperature_noise(long int temperaturex10)
{
    long int noise_threshold = 0;  

    if (config.use_archaic_units)
    {
        noise_threshold = 2;
    }
    else
    {
        noise_threshold = 1;
    }

    if (abs(temperaturex10 - climate_trend.moving_average.temperaturex10) <= noise_threshold)
    {
        temperaturex10 = climate_trend.moving_average.temperaturex10;
    }

    return(temperaturex10);
}

/*!
 * \brief Get buffer index of a past hsitory sample where 0 = next sample, 1 = latest sample, 2 = previous sample, etc.
 * 
 * \return index of past sample
 */
int get_climate_history_buffer_index(int num_samples_in_past) 
{
    int index_of_past_sample = 0;

    CLIP(num_samples_in_past, 0, climate_history.buffer_population);

    index_of_past_sample = (climate_history.buffer_index + NUM_ROWS(climate_history.buffer) - num_samples_in_past)%NUM_ROWS(climate_history.buffer);

    return(index_of_past_sample);
}

/*!
 * \brief Get buffer index of a past trend sample where 0 = next sample, 1 = latest sample, 2 = previous sample, etc.
 * 
 * \return index of past sample
 */
int get_climate_trend_buffer_index(int num_samples_in_past) 
{
    int index_of_past_sample = 0;

    CLIP(num_samples_in_past, 0, climate_trend.buffer_population);

    index_of_past_sample = (climate_trend.buffer_index + NUM_ROWS(climate_trend.delta_buffer) - num_samples_in_past)%NUM_ROWS(climate_trend.delta_buffer);

    return(index_of_past_sample);
}


/*!
 * \brief Filter out single sample delta reversals
 * 
 * \return smoothed temperature reading
 */
int smooth_temperature_history(void)
{
    long int delta_1 = 0;
    long int delta_2 = 0;
    long int delta_3 = 0;
    bool delta_1_positive = false;
    bool delta_2_positive = false;
    bool delta_3_positive = false;
    long int temp_1 = 0;
    long int temp_2 = 0;
    long int temp_3 = 0;
    long int smoothed_penultimate_temperature = 0;
    int i = 0;
 
    // filter out unrealistic short reversals in temperature trend that last one sample
    if (climate_trend.buffer_population > 3)
    {
        // scan through trend buffer examining sequences of three sammples
        for (i=0; i < (climate_trend.buffer_population-3); i++)
        {
            // get temperature delta values for last three samples
            delta_1 = climate_trend.delta_buffer[get_climate_trend_buffer_index(i+1)].temperaturex10;  // ultimate
            delta_2 = climate_trend.delta_buffer[get_climate_trend_buffer_index(i+2)].temperaturex10;  // penultimate
            delta_3 = climate_trend.delta_buffer[get_climate_trend_buffer_index(i+3)].temperaturex10;  // antepenultimate

            // determine if delta was positive or negative for past three samples
            delta_1_positive = delta_1>0?true:false;
            delta_2_positive = delta_2>0?true:false;
            delta_3_positive = delta_3>0?true:false; 
            
            // check if delta reversed direction for one sample -- we want to filter this out
            if (((delta_3 == 0) || (delta_3_positive == delta_1_positive)) && ((delta_2 != 0) && (delta_1 != 0) && (delta_2_positive != delta_1_positive)))
            {
                // delta reversed for one sample so replace penultimate sample to form straight line
                temp_1 = climate_history.buffer[get_climate_history_buffer_index(i+1)].temperaturex10;
                temp_2 = climate_history.buffer[get_climate_history_buffer_index(i+2)].temperaturex10;
                temp_3 = climate_history.buffer[get_climate_history_buffer_index(i+3)].temperaturex10;

                // average ultimate and antepenultimate temperatures
                smoothed_penultimate_temperature = (temp_1 + temp_3)/2;  

                printf("smoothing temperature @ sample %d in the past.  Original sequence = %ld, %ld, %ld.  New sequence = %ld, %ld, %ld\n", i, temp_3, temp_2, temp_1, temp_3, smoothed_penultimate_temperature, temp_1);

                // overwrite penultimate temperature with smoothed version
                climate_history.buffer[get_climate_history_buffer_index(i+2)].temperaturex10 = smoothed_penultimate_temperature;  
                
                // overwrite penultimate temperature delta
                climate_trend.delta_buffer[get_climate_trend_buffer_index(i+2)].temperaturex10 = smoothed_penultimate_temperature - temp_3;
            }
        }
    }

    return(0);
}
