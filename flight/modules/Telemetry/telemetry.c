/**
 ******************************************************************************
 * @addtogroup OpenPilotModules OpenPilot Modules
 * @{
 * @addtogroup TelemetryModule Telemetry Module
 * @brief Main telemetry module
 * Starts three tasks (RX, TX, and priority TX) that watch event queues
 * and handle all the telemetry of the UAVobjects
 * @{
 *
 * @file       telemetry.c
 * @author     The OpenPilot Team, http://www.openpilot.org Copyright (C) 2015.
 * @brief      Telemetry module, handles telemetry and UAVObject updates
 * @see        The GNU Public License (GPL) Version 3
 *
 *****************************************************************************/
/*
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License
 * for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
 */

/* Telemetry uses four tasks. Two are created for the main telemetry
 * stream called "TelTx" and "TelRx". Two are created to handle the OPLink
 * radio connection, called "RadioTx" and "Radio Rx", the latter being
 * overridden by USB if connected.
 *
 * The telemetry port to use is defined by PIOS_COM_TELEM_RF in
 * PIOS_Board_Init().
 *
 * A UAVTalk connection instance, telemUavTalkCon, is associated with the main
 * telemetry channel and another, radioUavTalkCon, with the radio channel.
 * Associated with each instance is a transmit routine which will send data
 * to the appropriate port.
 *
 * Data is passed on the telemetry channels using queues. If
 * PIOS_TELEM_PRIORITY_QUEUE is defined then two queues are created, one normal
 * priority and the other high priority.
 *
 * The "Tx" tasks read events first from the priority queue and then from
 * the normal queue, passing each event to processObjEvent() which ultimately
 * passes each event to the UAVTalk library which results in the appropriate
 * transmit routine being called to send the data back to the recipient on
 * the telemetry or radio link.
 */

#include <openpilot.h>

#include "telemetry.h"

#include "flighttelemetrystats.h"
#include "gcstelemetrystats.h"
#include "hwsettings.h"
#include "taskinfo.h"

// Private constants
#define MAX_QUEUE_SIZE            TELEM_QUEUE_SIZE
// Three different stack size parameter are accepted for Telemetry(RX PIOS_TELEM_RX_STACK_SIZE)
// Tx(PIOS_TELEM_TX_STACK_SIZE) and Radio RX(PIOS_TELEM_RADIO_RX_STACK_SIZE)
#ifdef PIOS_TELEM_RX_STACK_SIZE
#define STACK_SIZE_RX_BYTES       PIOS_TELEM_RX_STACK_SIZE
#define STACK_SIZE_TX_BYTES       PIOS_TELEM_TX_STACK_SIZE
#else
#define STACK_SIZE_RX_BYTES       PIOS_TELEM_STACK_SIZE
#define STACK_SIZE_TX_BYTES       PIOS_TELEM_STACK_SIZE
#endif

#ifdef PIOS_TELEM_RADIO_RX_STACK_SIZE
#define STACK_SIZE_RADIO_RX_BYTES PIOS_TELEM_RADIO_RX_STACK_SIZE
#define STACK_SIZE_RADIO_TX_BYTES PIOS_TELEM_RADIO_TX_STACK_SIZE
#else
#define STACK_SIZE_RADIO_RX_BYTES STACK_SIZE_RX_BYTES
#define STACK_SIZE_RADIO_TX_BYTES STACK_SIZE_TX_BYTES
#endif
#define TASK_PRIORITY_RX          (tskIDLE_PRIORITY + 2)
#define TASK_PRIORITY_TX          (tskIDLE_PRIORITY + 2)
#define TASK_PRIORITY_RADRX       (tskIDLE_PRIORITY + 2)
#define TASK_PRIORITY_RADTX       (tskIDLE_PRIORITY + 2)
#define REQ_TIMEOUT_MS            250
#define MAX_RETRIES               2
#define STATS_UPDATE_PERIOD_MS    4000
#define CONNECTION_TIMEOUT_MS     8000

// Private types
typedef struct {
    // Port on which to communicate telemetry information
    uint32_t     telemetryPort;
    // Main telemetry queue
    xQueueHandle mainQueue;
        #ifdef PIOS_TELEM_PRIORITY_QUEUE
    // Priority telemetry queue
    xQueueHandle priorityQueue;
        #endif /* PIOS_TELEM_PRIORITY_QUEUE */
    // Transmit/receive task handles
    xTaskHandle  telemetryTxTaskHandle;
    xTaskHandle  telemetryRxTaskHandle;
    // Telemetry stream
    UAVTalkConnection uavTalkCon;
} telemetryContext;

// Main telemetry port
static telemetryContext telemHandle;
static int32_t transmitData(uint8_t *data, int32_t length);
static void registerTelemObject(UAVObjHandle obj);

// OPLink telemetry port
static telemetryContext radioHandle;
static int32_t transmitRadioData(uint8_t *data, int32_t length);
static void registerRadioObject(UAVObjHandle obj);

// Telemetry stats
static uint32_t txErrors;
static uint32_t txRetries;
static uint32_t timeOfLastObjectUpdate;

static void telemetryTxTask(void *parameters);
static void telemetryRxTask(void *parameters);
static void updateObject(
    telemetryContext *telemetryHandle,
    UAVObjHandle obj,
    int32_t eventType);
static void processObjEvent(
    telemetryContext *telemetryHandle,
    UAVObjEvent *ev);
static int32_t setUpdatePeriod(
    telemetryContext *telemetryHandle,
    UAVObjHandle obj,
    int32_t updatePeriodMs);
static int32_t setLoggingPeriod(
    telemetryContext *telemetryHandle,
    UAVObjHandle obj,
    int32_t updatePeriodMs);
static void updateTelemetryStats();
static void gcsTelemetryStatsUpdated();
static void updateSettings();

/**
 * Initialise the telemetry module
 * \return -1 if initialisation failed
 * \return 0 on success
 */
int32_t TelemetryStart(void)
{
    UAVObjIterate(&registerTelemObject);
    UAVObjIterate(&registerRadioObject);

    // Listen to objects of interest
#ifdef PIOS_TELEM_PRIORITY_QUEUE
    GCSTelemetryStatsConnectQueue(telemHandle.priorityQueue);
    GCSTelemetryStatsConnectQueue(radioHandle.priorityQueue);
#else /* PIOS_TELEM_PRIORITY_QUEUE */
    GCSTelemetryStatsConnectQueue(telemHandle.mainQueue);
    GCSTelemetryStatsConnectQueue(radioHandle.mainQueue);
#endif /* PIOS_TELEM_PRIORITY_QUEUE */

    // Start telemetry tasks
    xTaskCreate(telemetryTxTask,
                "TelTx",
                STACK_SIZE_TX_BYTES / 4,
                &telemHandle,
                TASK_PRIORITY_TX,
                &telemHandle.telemetryTxTaskHandle);
    PIOS_TASK_MONITOR_RegisterTask(TASKINFO_RUNNING_TELEMETRYTX,
                                   telemHandle.telemetryTxTaskHandle);
    xTaskCreate(telemetryRxTask,
                "TelRx",
                STACK_SIZE_RX_BYTES / 4,
                &telemHandle,
                TASK_PRIORITY_RX,
                &telemHandle.telemetryRxTaskHandle);
    PIOS_TASK_MONITOR_RegisterTask(TASKINFO_RUNNING_TELEMETRYRX,
                                   telemHandle.telemetryRxTaskHandle);

    xTaskCreate(telemetryTxTask,
                "RadioTx",
                STACK_SIZE_RADIO_TX_BYTES / 4,
                &radioHandle,
                TASK_PRIORITY_RADTX,
                &radioHandle.telemetryTxTaskHandle);
    PIOS_TASK_MONITOR_RegisterTask(TASKINFO_RUNNING_TELEMETRYTX,
                                   radioHandle.telemetryTxTaskHandle);
    xTaskCreate(telemetryRxTask,
                "RadioRx",
                STACK_SIZE_RADIO_RX_BYTES / 4,
                &radioHandle,
                TASK_PRIORITY_RADRX,
                &radioHandle.telemetryRxTaskHandle);
    PIOS_TASK_MONITOR_RegisterTask(TASKINFO_RUNNING_RADIORX,
                                   radioHandle.telemetryRxTaskHandle);

    return 0;
}

/**
 * Initialise the telemetry module
 * \return -1 if initialisation failed
 * \return 0 on success
 */
int32_t TelemetryInitialize(void)
{
    FlightTelemetryStatsInitialize();
    GCSTelemetryStatsInitialize();

    // Initialize vars
    timeOfLastObjectUpdate    = 0;

    // Create object queues
    telemHandle.mainQueue     = xQueueCreate(MAX_QUEUE_SIZE,
                                             sizeof(UAVObjEvent));
    radioHandle.mainQueue     = xQueueCreate(MAX_QUEUE_SIZE,
                                             sizeof(UAVObjEvent));
#if defined(PIOS_TELEM_PRIORITY_QUEUE)
    telemHandle.priorityQueue = xQueueCreate(MAX_QUEUE_SIZE,
                                             sizeof(UAVObjEvent));
    radioHandle.priorityQueue = xQueueCreate(MAX_QUEUE_SIZE,
                                             sizeof(UAVObjEvent));
#endif /* PIOS_TELEM_PRIORITY_QUEUE */

    // Update telemetry settings
    telemHandle.telemetryPort = PIOS_COM_TELEM_RF;

#ifdef PIOS_INCLUDE_RFM22B
    radioHandle.telemetryPort = PIOS_COM_RF;
#else /* PIOS_INCLUDE_RFM22B */
    radioHandle.telemetryPort = 0;
#endif /* PIOS_INCLUDE_RFM22B */
#ifdef PIOS_INCLUDE_USB
    // if USB is connected, USB takes precedence for telemetry
    if (PIOS_COM_Available(PIOS_COM_TELEM_USB)) {
        radioHandle.telemetryPort = PIOS_COM_TELEM_USB;
    }
#endif /* PIOS_INCLUDE_USB */

    HwSettingsInitialize();
    updateSettings();

    // Initialise UAVTalk
    telemHandle.uavTalkCon = UAVTalkInitialize(&transmitData);
    radioHandle.uavTalkCon = UAVTalkInitialize(&transmitRadioData);

    // Create periodic event that will be used to update the telemetry stats
    // FIXME STATS_UPDATE_PERIOD_MS is 4000ms while FlighTelemetryStats update period is 5000ms...
    txErrors  = 0;
    txRetries = 0;
    UAVObjEvent ev;
    memset(&ev, 0, sizeof(UAVObjEvent));

#ifdef PIOS_TELEM_PRIORITY_QUEUE
    EventPeriodicQueueCreate(&ev,
                             telemHandle.priorityQueue,
                             STATS_UPDATE_PERIOD_MS);
    EventPeriodicQueueCreate(&ev,
                             radioHandle.priorityQueue,
                             STATS_UPDATE_PERIOD_MS);
#else /* PIOS_TELEM_PRIORITY_QUEUE */
    EventPeriodicQueueCreate(&ev,
                             telemHandle.mainQueue,
                             STATS_UPDATE_PERIOD_MS);
    EventPeriodicQueueCreate(&ev,
                             radioHandle.mainQueue,
                             STATS_UPDATE_PERIOD_MS);
#endif /* PIOS_TELEM_PRIORITY_QUEUE */

    return 0;
}

MODULE_INITCALL(TelemetryInitialize, TelemetryStart);

/**
 * Register a new object, adds object to local list and connects the queue depending on the object's
 * telemetry settings.
 * \param[in] obj Object to connect
 */
static void registerTelemObject(UAVObjHandle obj)
{
    if (UAVObjIsMetaobject(obj)) {
        // Only connect change notifications for meta objects.  No periodic updates
#ifdef PIOS_TELEM_PRIORITY_QUEUE
        UAVObjConnectQueue(obj, telemHandle.priorityQueue, EV_MASK_ALL_UPDATES);
#else /* PIOS_TELEM_PRIORITY_QUEUE */
        UAVObjConnectQueue(obj, telemHandle.mainQueue, EV_MASK_ALL_UPDATES);
#endif /* PIOS_TELEM_PRIORITY_QUEUE */
    } else {
        // Setup object for periodic updates
        updateObject(
            &telemHandle,
            obj,
            EV_NONE);
    }
}

static void registerRadioObject(UAVObjHandle obj)
{
    if (UAVObjIsMetaobject(obj)) {
        // Only connect change notifications for meta objects.  No periodic updates
#ifdef PIOS_TELEM_PRIORITY_QUEUE
        UAVObjConnectQueue(obj, radioHandle.priorityQueue, EV_MASK_ALL_UPDATES);
#else /* PIOS_TELEM_PRIORITY_QUEUE */
        UAVObjConnectQueue(obj, radioHandle.mainQueue, EV_MASK_ALL_UPDATES);
#endif /* PIOS_TELEM_PRIORITY_QUEUE */
    } else {
        // Setup object for periodic updates
        updateObject(
            &radioHandle,
            obj,
            EV_NONE);
    }
}

/**
 * Update object's queue connections and timer, depending on object's settings
 * \param[in] obj Object to updates
 */
static void updateObject(
    telemetryContext *telemetryHandle,
    UAVObjHandle obj,
    int32_t eventType)
{
    UAVObjMetadata metadata;
    UAVObjUpdateMode updateMode, loggingMode;
    int32_t eventMask;

    if (UAVObjIsMetaobject(obj)) {
        // This function updates the periodic updates for the object.
        // Meta Objects cannot have periodic updates.
        PIOS_Assert(false);
        return;
    }

    // Get metadata
    UAVObjGetMetadata(obj, &metadata);
    updateMode  = UAVObjGetTelemetryUpdateMode(&metadata);
    loggingMode = UAVObjGetLoggingUpdateMode(&metadata);

    // Setup object depending on update mode
    eventMask   = 0;
    switch (updateMode) {
    case UPDATEMODE_PERIODIC:
        // Set update period
        setUpdatePeriod(telemetryHandle,
                        obj,
                        metadata.telemetryUpdatePeriod);
        // Connect queue
        eventMask |= EV_UPDATED_PERIODIC | EV_UPDATED_MANUAL | EV_UPDATE_REQ;
        break;
    case UPDATEMODE_ONCHANGE:
        // Set update period
        setUpdatePeriod(telemetryHandle, obj, 0);
        // Connect queue
        eventMask |= EV_UPDATED | EV_UPDATED_MANUAL | EV_UPDATE_REQ;
        break;
    case UPDATEMODE_THROTTLED:
        if ((eventType == EV_UPDATED_PERIODIC) || (eventType == EV_NONE)) {
            // If we received a periodic update, we can change back to update on change
            eventMask |= EV_UPDATED | EV_UPDATED_MANUAL | EV_UPDATE_REQ;
            // Set update period on initialization and metadata change
            if (eventType == EV_NONE) {
                setUpdatePeriod(telemetryHandle,
                                obj,
                                metadata.telemetryUpdatePeriod);
            }
        } else {
            // Otherwise, we just received an object update, so switch to periodic for the timeout period to prevent more updates
            eventMask |= EV_UPDATED_PERIODIC | EV_UPDATED_MANUAL | EV_UPDATE_REQ;
        }
        break;
    case UPDATEMODE_MANUAL:
        // Set update period
        setUpdatePeriod(telemetryHandle, obj, 0);
        // Connect queue
        eventMask |= EV_UPDATED_MANUAL | EV_UPDATE_REQ;
        break;
    }
    switch (loggingMode) {
    case UPDATEMODE_PERIODIC:
        // Set update period
        setLoggingPeriod(telemetryHandle, obj, metadata.loggingUpdatePeriod);
        // Connect queue
        eventMask |= EV_LOGGING_PERIODIC | EV_LOGGING_MANUAL;
        break;
    case UPDATEMODE_ONCHANGE:
        // Set update period
        setLoggingPeriod(telemetryHandle, obj, 0);
        // Connect queue
        eventMask |= EV_UPDATED | EV_LOGGING_MANUAL;
        break;
    case UPDATEMODE_THROTTLED:
        if ((eventType == EV_LOGGING_PERIODIC) || (eventType == EV_NONE)) {
            // If we received a periodic update, we can change back to update on change
            eventMask |= EV_UPDATED | EV_LOGGING_MANUAL;
            // Set update period on initialization and metadata change
            if (eventType == EV_NONE) {
                setLoggingPeriod(telemetryHandle,
                                 obj,
                                 metadata.loggingUpdatePeriod);
            }
        } else {
            // Otherwise, we just received an object update, so switch to periodic for the timeout period to prevent more updates
            eventMask |= EV_LOGGING_PERIODIC | EV_LOGGING_MANUAL;
        }
        break;
    case UPDATEMODE_MANUAL:
        // Set update period
        setLoggingPeriod(telemetryHandle, obj, 0);
        // Connect queue
        eventMask |= EV_LOGGING_MANUAL;
        break;
    }

    // note that all setting objects have implicitly IsPriority=true
#ifdef PIOS_TELEM_PRIORITY_QUEUE
    if (UAVObjIsPriority(obj)) {
        UAVObjConnectQueue(obj, telemetryHandle->priorityQueue, eventMask);
    } else
#endif /* PIOS_TELEM_PRIORITY_QUEUE */
    UAVObjConnectQueue(obj, telemetryHandle->mainQueue, eventMask);
}


/**
 * Processes queue events
 */
static void processObjEvent(
    telemetryContext *telemetryHandle,
    UAVObjEvent *ev)
{
    UAVObjMetadata metadata;
    UAVObjUpdateMode updateMode;
    int32_t retries;
    int32_t success;

    if (ev->obj == 0) {
        updateTelemetryStats();
    } else if (ev->obj == GCSTelemetryStatsHandle()) {
        gcsTelemetryStatsUpdated();
    } else {
        // Get object metadata
        UAVObjGetMetadata(ev->obj, &metadata);
        updateMode = UAVObjGetTelemetryUpdateMode(&metadata);

        // Act on event
        retries    = 0;
        success    = -1;
        if ((ev->event == EV_UPDATED && (updateMode == UPDATEMODE_ONCHANGE || updateMode == UPDATEMODE_THROTTLED))
            || ev->event == EV_UPDATED_MANUAL
            || (ev->event == EV_UPDATED_PERIODIC && updateMode != UPDATEMODE_THROTTLED)) {
            // Send update to GCS (with retries)
            while (retries < MAX_RETRIES && success == -1) {
                // call blocks until ack is received or timeout
                success = UAVTalkSendObject(telemetryHandle->uavTalkCon,
                                            ev->obj,
                                            ev->instId,
                                            UAVObjGetTelemetryAcked(&metadata), REQ_TIMEOUT_MS);
                if (success == -1) {
                    ++retries;
                }
            }
            // Update stats
            txRetries += retries;
            if (success == -1) {
                ++txErrors;
            }
        } else if (ev->event == EV_UPDATE_REQ) {
            // Request object update from GCS (with retries)
            while (retries < MAX_RETRIES && success == -1) {
                // call blocks until update is received or timeout
                success = UAVTalkSendObjectRequest(telemetryHandle->uavTalkCon,
                                                   ev->obj,
                                                   ev->instId,
                                                   REQ_TIMEOUT_MS);
                if (success == -1) {
                    ++retries;
                }
            }
            // Update stats
            txRetries += retries;
            if (success == -1) {
                ++txErrors;
            }
        }
        // If this is a metaobject then make necessary telemetry updates
        if (UAVObjIsMetaobject(ev->obj)) {
            // linked object will be the actual object the metadata are for
            updateObject(
                telemetryHandle,
                UAVObjGetLinkedObj(ev->obj),
                EV_NONE);
        } else {
            if (updateMode == UPDATEMODE_THROTTLED) {
                // If this is UPDATEMODE_THROTTLED, the event mask changes on every event.
                updateObject(
                    telemetryHandle,
                    ev->obj,
                    ev->event);
            }
        }
    }
    // Log UAVObject if necessary
    if (ev->obj) {
        updateMode = UAVObjGetLoggingUpdateMode(&metadata);
        if ((ev->event == EV_UPDATED && (updateMode == UPDATEMODE_ONCHANGE || updateMode == UPDATEMODE_THROTTLED))
            || ev->event == EV_LOGGING_MANUAL
            || (ev->event == EV_LOGGING_PERIODIC && updateMode != UPDATEMODE_THROTTLED)) {
            if (ev->instId == UAVOBJ_ALL_INSTANCES) {
                success = UAVObjGetNumInstances(ev->obj);
                for (retries = 0; retries < success; retries++) {
                    UAVObjInstanceWriteToLog(ev->obj, retries);
                }
            } else {
                UAVObjInstanceWriteToLog(ev->obj, ev->instId);
            }
        }
        if (updateMode == UPDATEMODE_THROTTLED) {
            // If this is UPDATEMODE_THROTTLED, the event mask changes on every event.
            updateObject(
                telemetryHandle,
                ev->obj,
                ev->event);
        }
    }
}

/**
 * Telemetry transmit task, regular priority
 */
static void telemetryTxTask(void *parameters)
{
    telemetryContext *telemetryHandle = (telemetryContext *)parameters;
    UAVObjEvent ev;

    /* Check for a bad context */
    if (!telemetryHandle) {
        return;
    }

    // Loop forever
    while (1) {
        /**
         * Tries to empty the high priority queue before handling any standard priority item
         */
#ifdef PIOS_TELEM_PRIORITY_QUEUE
        // empty priority queue, non-blocking
        while (xQueueReceive(telemetryHandle->priorityQueue, &ev, 0) == pdTRUE) {
            // Process event
            processObjEvent(telemetryHandle, &ev);
        }
        // check regular queue and process update - non-blocking
        if (xQueueReceive(telemetryHandle->mainQueue, &ev, 0) == pdTRUE) {
            // Process event
            processObjEvent(telemetryHandle, &ev);
            // if both queues are empty, wait on priority queue for updates (1 tick) then repeat cycle
        } else if (xQueueReceive(telemetryHandle->priorityQueue, &ev, 1) == pdTRUE) {
            // Process event
            processObjEvent(telemetryHandle, &ev);
        }
#else
        // wait on queue for updates (1 tick) then repeat cycle
        if (xQueueReceive(telemetryHandle->mainQueue, &ev, 1) == pdTRUE) {
            // Process event
            processObjEvent(telemetryHandle, &ev);
        }
#endif /* PIOS_TELEM_PRIORITY_QUEUE */
    }
}


/**
 * Telemetry receive task. Processes queue events and periodic updates.
 */
static void telemetryRxTask(void *parameters)
{
    telemetryContext *telemetryHandle = (telemetryContext *)parameters;

    /* Check for a bad context */
    if (!telemetryHandle) {
        return;
    }

    // Task loop
    while (1) {
        uint32_t inputPort = telemetryHandle->telemetryPort;

        if (inputPort) {
            // Block until data are available
            uint8_t serial_data[1];
            uint16_t bytes_to_process;

            bytes_to_process = PIOS_COM_ReceiveBuffer(inputPort, serial_data, sizeof(serial_data), 500);
            if (bytes_to_process > 0) {
                for (uint8_t i = 0; i < bytes_to_process; i++) {
                    UAVTalkProcessInputStream(telemetryHandle->uavTalkCon, serial_data[i]);
                }
            }
        } else {
            vTaskDelay(5);
        }
    }
}


/**
 * Transmit data buffer to the modem or USB port.
 * \param[in] data Data buffer to send
 * \param[in] length Length of buffer
 * \return -1 on failure
 * \return number of bytes transmitted on success
 */
static int32_t transmitData(uint8_t *data, int32_t length)
{
    uint32_t outputPort;

    outputPort = PIOS_COM_TELEM_RF;

    // Anticipate next input on the port on which this output occurs
    telemHandle.telemetryPort = outputPort;

    if (outputPort) {
        return PIOS_COM_SendBuffer(outputPort, data, length);
    }

    return -1;
}


/**
 * Transmit data buffer to the radioport.
 * \param[in] data Data buffer to send
 * \param[in] length Length of buffer
 * \return -1 on failure
 * \return number of bytes transmitted on success
 */
static int32_t transmitRadioData(uint8_t *data, int32_t length)
{
#ifdef PIOS_INCLUDE_RFM22B
    uint32_t outputPort = PIOS_COM_RF;
#else /* PIOS_INCLUDE_RFM22B */
    uint32_t outputPort = 0;
#endif /* PIOS_INCLUDE_RFM22B */
#ifdef PIOS_INCLUDE_USB
    // if USB is connected, USB takes precedence for telemetry
    if (PIOS_COM_Available(PIOS_COM_TELEM_USB)) {
        outputPort = PIOS_COM_TELEM_USB;
    }
#endif /* PIOS_INCLUDE_USB */

    // Anticipate next input on the port on which this output occurs
    radioHandle.telemetryPort = outputPort;

    if (outputPort) {
        return PIOS_COM_SendBuffer(outputPort, data, length);
    }

    return -1;
}

/**
 * Set update period of object (it must be already setup for periodic updates)
 * \param[in] obj The object to update
 * \param[in] updatePeriodMs The update period in ms, if zero then periodic updates are disabled
 * \return 0 Success
 * \return -1 Failure
 */
static int32_t setUpdatePeriod(
    telemetryContext *telemetryHandle,
    UAVObjHandle obj,
    int32_t updatePeriodMs)
{
    UAVObjEvent ev;
    int32_t ret;

    // Add or update object for periodic updates
    ev.obj    = obj;
    ev.instId = UAVOBJ_ALL_INSTANCES;
    ev.event  = EV_UPDATED_PERIODIC;
    ev.lowPriority = true;

#ifdef PIOS_TELEM_PRIORITY_QUEUE
    xQueueHandle targetQueue = UAVObjIsPriority(obj) ? telemetryHandle->priorityQueue :
                               telemetryHandle->mainQueue;
#else /* PIOS_TELEM_PRIORITY_QUEUE */
    xQueueHandle targetQueue = telemetryHandle->mainQueue;
#endif /* PIOS_TELEM_PRIORITY_QUEUE */

    ret = EventPeriodicQueueUpdate(&ev, targetQueue, updatePeriodMs);
    if (ret == -1) {
        ret = EventPeriodicQueueCreate(&ev, targetQueue, updatePeriodMs);
    }
    return ret;
}

/**
 * Set logging update period of object (it must be already setup for periodic updates)
 * \param[in] obj The object to update
 * \param[in] updatePeriodMs The update period in ms, if zero then periodic updates are disabled
 * \return 0 Success
 * \return -1 Failure
 */
static int32_t setLoggingPeriod(
    telemetryContext *telemetryHandle,
    UAVObjHandle obj,
    int32_t updatePeriodMs)
{
    UAVObjEvent ev;
    int32_t ret;

    // Add or update object for periodic updates
    ev.obj    = obj;
    ev.instId = UAVOBJ_ALL_INSTANCES;
    ev.event  = EV_LOGGING_PERIODIC;
    ev.lowPriority = true;

#ifdef PIOS_TELEM_PRIORITY_QUEUE
    xQueueHandle targetQueue = UAVObjIsPriority(obj) ? telemetryHandle->priorityQueue :
                               telemetryHandle->mainQueue;
#else /* PIOS_TELEM_PRIORITY_QUEUE */
    xQueueHandle targetQueue = telemetryHandle->mainQueue;
#endif /* PIOS_TELEM_PRIORITY_QUEUE */

    ret = EventPeriodicQueueUpdate(&ev, targetQueue, updatePeriodMs);
    if (ret == -1) {
        ret = EventPeriodicQueueCreate(&ev, targetQueue, updatePeriodMs);
    }
    return ret;
}

/**
 * Called each time the GCS telemetry stats object is updated.
 * Trigger a flight telemetry stats update if a connection is not
 * yet established.
 */
static void gcsTelemetryStatsUpdated()
{
    FlightTelemetryStatsData flightStats;
    GCSTelemetryStatsData gcsStats;

    FlightTelemetryStatsGet(&flightStats);
    GCSTelemetryStatsGet(&gcsStats);
    if (flightStats.Status != FLIGHTTELEMETRYSTATS_STATUS_CONNECTED || gcsStats.Status != GCSTELEMETRYSTATS_STATUS_CONNECTED) {
        updateTelemetryStats();
    }
}

/**
 * Update telemetry statistics and handle connection handshake
 */
static void updateTelemetryStats()
{
    UAVTalkStats utalkStats;
    FlightTelemetryStatsData flightStats;
    GCSTelemetryStatsData gcsStats;
    uint8_t forceUpdate;
    uint8_t connectionTimeout;
    uint32_t timeNow;

    // Get stats
    UAVTalkGetStats(telemHandle.uavTalkCon, &utalkStats, true);
    UAVTalkAddStats(radioHandle.uavTalkCon, &utalkStats, true);

    // Get object data
    FlightTelemetryStatsGet(&flightStats);
    GCSTelemetryStatsGet(&gcsStats);

    // Update stats object
    if (flightStats.Status == FLIGHTTELEMETRYSTATS_STATUS_CONNECTED) {
        flightStats.TxDataRate    = (float)utalkStats.txBytes / ((float)STATS_UPDATE_PERIOD_MS / 1000.0f);
        flightStats.TxBytes      += utalkStats.txBytes;
        flightStats.TxFailures   += txErrors;
        flightStats.TxRetries    += txRetries;

        flightStats.RxDataRate    = (float)utalkStats.rxBytes / ((float)STATS_UPDATE_PERIOD_MS / 1000.0f);
        flightStats.RxBytes      += utalkStats.rxBytes;
        flightStats.RxFailures   += utalkStats.rxErrors;
        flightStats.RxSyncErrors += utalkStats.rxSyncErrors;
        flightStats.RxCrcErrors  += utalkStats.rxCrcErrors;
    } else {
        flightStats.TxDataRate   = 0;
        flightStats.TxBytes      = 0;
        flightStats.TxFailures   = 0;
        flightStats.TxRetries    = 0;

        flightStats.RxDataRate   = 0;
        flightStats.RxBytes      = 0;
        flightStats.RxFailures   = 0;
        flightStats.RxSyncErrors = 0;
        flightStats.RxCrcErrors  = 0;
    }
    txErrors  = 0;
    txRetries = 0;

    // Check for connection timeout
    timeNow   = xTaskGetTickCount() * portTICK_RATE_MS;
    if (utalkStats.rxObjects > 0) {
        timeOfLastObjectUpdate = timeNow;
    }
    if ((timeNow - timeOfLastObjectUpdate) > CONNECTION_TIMEOUT_MS) {
        connectionTimeout = 1;
    } else {
        connectionTimeout = 0;
    }

    // Update connection state
    forceUpdate = 1;
    if (flightStats.Status == FLIGHTTELEMETRYSTATS_STATUS_DISCONNECTED) {
        // Wait for connection request
        if (gcsStats.Status == GCSTELEMETRYSTATS_STATUS_HANDSHAKEREQ) {
            flightStats.Status = FLIGHTTELEMETRYSTATS_STATUS_HANDSHAKEACK;
        }
    } else if (flightStats.Status == FLIGHTTELEMETRYSTATS_STATUS_HANDSHAKEACK) {
        // Wait for connection
        if (gcsStats.Status == GCSTELEMETRYSTATS_STATUS_CONNECTED) {
            flightStats.Status = FLIGHTTELEMETRYSTATS_STATUS_CONNECTED;
        } else if (gcsStats.Status == GCSTELEMETRYSTATS_STATUS_DISCONNECTED) {
            flightStats.Status = FLIGHTTELEMETRYSTATS_STATUS_DISCONNECTED;
        }
    } else if (flightStats.Status == FLIGHTTELEMETRYSTATS_STATUS_CONNECTED) {
        if (gcsStats.Status != GCSTELEMETRYSTATS_STATUS_CONNECTED || connectionTimeout) {
            flightStats.Status = FLIGHTTELEMETRYSTATS_STATUS_DISCONNECTED;
        } else {
            forceUpdate = 0;
        }
    } else {
        flightStats.Status = FLIGHTTELEMETRYSTATS_STATUS_DISCONNECTED;
    }

    // TODO: check whether is there any error condition worth raising an alarm
    // Disconnection is actually a normal (non)working status so it is not raising alarms anymore.
    if (flightStats.Status == FLIGHTTELEMETRYSTATS_STATUS_CONNECTED) {
        AlarmsClear(SYSTEMALARMS_ALARM_TELEMETRY);
    }

    // Update object
    FlightTelemetryStatsSet(&flightStats);

    // Force telemetry update if not connected
    if (forceUpdate) {
        FlightTelemetryStatsUpdated();
    }
}

/**
 * Update the telemetry settings, called on startup.
 * FIXME: This should be in the TelemetrySettings object. But objects
 * have too much overhead yet. Also the telemetry has no any specific
 * settings, etc. Thus the HwSettings object which contains the
 * telemetry port speed is used for now.
 */
static void updateSettings()
{
    if (telemHandle.telemetryPort) {
        // Retrieve settings
        uint8_t speed;
        HwSettingsTelemetrySpeedGet(&speed);

        // Set port speed
        switch (speed) {
        case HWSETTINGS_TELEMETRYSPEED_2400:
            PIOS_COM_ChangeBaud(telemHandle.telemetryPort, 2400);
            break;
        case HWSETTINGS_TELEMETRYSPEED_4800:
            PIOS_COM_ChangeBaud(telemHandle.telemetryPort, 4800);
            break;
        case HWSETTINGS_TELEMETRYSPEED_9600:
            PIOS_COM_ChangeBaud(telemHandle.telemetryPort, 9600);
            break;
        case HWSETTINGS_TELEMETRYSPEED_19200:
            PIOS_COM_ChangeBaud(telemHandle.telemetryPort, 19200);
            break;
        case HWSETTINGS_TELEMETRYSPEED_38400:
            PIOS_COM_ChangeBaud(telemHandle.telemetryPort, 38400);
            break;
        case HWSETTINGS_TELEMETRYSPEED_57600:
            PIOS_COM_ChangeBaud(telemHandle.telemetryPort, 57600);
            break;
        case HWSETTINGS_TELEMETRYSPEED_115200:
            PIOS_COM_ChangeBaud(telemHandle.telemetryPort, 115200);
            break;
        }
    }
}


/**
 * @}
 * @}
 */
