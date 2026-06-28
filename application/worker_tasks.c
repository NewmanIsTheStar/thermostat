/**
 * Copyright (c) 2024 NewmanIsTheStar
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */
#include <stdio.h>
#include <stdlib.h>
#include "stdarg.h"

#include "pico/stdlib.h"
#include "FreeRTOS.h"
#include "FreeRTOSConfig.h"
#include "task.h"

#include "worker_tasks.h"

// include header for each worker task here
#include "thermostat.h"
#include "mqtt.h"

// worker tasks to launch and monitor
WORKER_TASK_T worker_tasks[] =
{
    //  function        name                    stack   priority        
    {   thermostat_task,"Thermostat Task",      8096,   1},       
    {   mqtt_task,      "MQTT Task",            8096,   1},   

    // end of table
    {   NULL,           NULL,               0,      0,         }
};




