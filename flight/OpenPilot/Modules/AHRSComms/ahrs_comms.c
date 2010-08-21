/**
 ******************************************************************************
 * @addtogroup OpenPilotModules OpenPilot Modules
 * @{ 
 * @addtogroup AHRSCommsModule AHRSComms Module
 * @brief Handles communication with AHRS and updating position
 * Specifically updates the the @ref AttitudeActual "AttitudeActual" and @ref AttitudeRaw "AttitudeRaw" settings objects
 * @{ 
 *
 * @file       ahrs_comms.c
 * @author     The OpenPilot Team, http://www.openpilot.org Copyright (C) 2010.
 * @brief      Module to handle all comms to the AHRS on a periodic basis.
 *
 * @see        The GNU Public License (GPL) Version 3
 *
 ******************************************************************************/
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

/**
 * Input object: AttitudeSettings
 * Output object: AttitudeActual
 *
 * This module will periodically update the value of latest attitude solution
 * that is available from the AHRS.
 * The module settings can configure how often AHRS is polled for a new solution.
 *
 * The module executes in its own thread.
 *
 * UAVObjects are automatically generated by the UAVObjectGenerator from
 * the object definition XML file.
 *
 * Modules have no API, all communication to other modules is done through UAVObjects.
 * However modules may use the API exposed by shared libraries.
 * See the OpenPilot wiki for more details.
 * http://www.openpilot.org/OpenPilot_Application_Architecture
 *
 */

#include "ahrs_comms.h"
#include "attitudeactual.h"
#include "attitudesettings.h"
#include "attituderaw.h"
#include "ahrsstatus.h"
#include "alarms.h"
#include "baroaltitude.h"
#include "stdbool.h"
#include "positionactual.h"
#include "homelocation.h"

#include "pios_opahrs.h" // library for OpenPilot AHRS access functions
#include "pios_opahrs_proto.h"

// Private constants
#define STACK_SIZE 400
#define TASK_PRIORITY (tskIDLE_PRIORITY+4)

// Private types

// Private variables
static xTaskHandle taskHandle;

// Private functions
static void ahrscommsTask(void* parameters);
static void load_baro_altitude(struct opahrs_msg_v1_req_altitude * altitude);
static void load_magnetic_north(struct opahrs_msg_v1_req_north * north);
static void load_position_actual(struct opahrs_msg_v1_req_gps * gps);
static void update_attitude_actual(struct opahrs_msg_v1_rsp_attitude * attitude);
static void update_attitude_raw(struct opahrs_msg_v1_rsp_attituderaw * attituderaw);
static void update_ahrs_status(struct opahrs_msg_v1_rsp_serial * serial);

static bool BaroAltitudeIsUpdatedFlag = false;
static void BaroAltitudeUpdatedCb(UAVObjEvent * ev)
{
  BaroAltitudeIsUpdatedFlag = true;
}

static bool PositionActualIsUpdatedFlag = false;
static void PositionActualUpdatedCb(UAVObjEvent * ev)
{
	PositionActualIsUpdatedFlag = true;
}

static bool HomeLocationIsUpdatedFlag = false;
static void HomeLocationUpdatedCb(UAVObjEvent * ev)
{
	HomeLocationIsUpdatedFlag = true;
}

static bool AHRSKnowsHome = FALSE;
/**
 * Initialise the module, called on startup
 * \returns 0 on success or -1 if initialisation failed
 */
int32_t AHRSCommsInitialize(void)
{
  BaroAltitudeConnectCallback(BaroAltitudeUpdatedCb);
  PositionActualConnectCallback(PositionActualUpdatedCb);
  HomeLocationConnectCallback(HomeLocationUpdatedCb);

  PIOS_OPAHRS_Init();

  // Start main task
  xTaskCreate(ahrscommsTask, (signed char*)"AHRSComms", STACK_SIZE, NULL, TASK_PRIORITY, &taskHandle);

  return 0;
}

static   uint16_t attitude_errors = 0, attituderaw_errors = 0, position_errors = 0, home_errors = 0, altitude_errors = 0;

/**
 * Module thread, should not return.
 */
static void ahrscommsTask(void* parameters)
{
  enum opahrs_result result;
  
  // Main task loop
  while (1) {
    struct opahrs_msg_v1 rsp;

    AlarmsSet(SYSTEMALARMS_ALARM_AHRSCOMMS, SYSTEMALARMS_ALARM_CRITICAL);

    /* Whenever resyncing, assume AHRS doesn't reset and doesn't know home */
    AHRSKnowsHome = FALSE;
    
    /* Spin here until we're in sync */
    while (PIOS_OPAHRS_resync() != OPAHRS_RESULT_OK) {
      vTaskDelay(100 / portTICK_RATE_MS);
    }
      
    /* Give the other side a chance to keep up */
    //vTaskDelay(100 / portTICK_RATE_MS);

    if (PIOS_OPAHRS_GetSerial(&rsp) == OPAHRS_RESULT_OK) {
      update_ahrs_status(&(rsp.payload.user.v.rsp.serial));
    } else {
      /* Comms error */
      continue;
    }

    AlarmsClear(SYSTEMALARMS_ALARM_AHRSCOMMS);

    /* We're in sync with the AHRS, spin here until an error occurs */
    while (1) {
      AttitudeSettingsData settings;

      /* Update settings with latest value */
      AttitudeSettingsGet(&settings);
  
      if ((result = PIOS_OPAHRS_GetAttitude(&rsp)) == OPAHRS_RESULT_OK) {
        update_attitude_actual(&(rsp.payload.user.v.rsp.attitude));
      } else {
        /* Comms error */
        attitude_errors++;
        break;
      }

      if ((result = PIOS_OPAHRS_GetAttitudeRaw(&rsp)) == OPAHRS_RESULT_OK) {
	      update_attitude_raw(&(rsp.payload.user.v.rsp.attituderaw));
      } else {
        /* Comms error */
        attituderaw_errors++;
        break;
      }

      if (BaroAltitudeIsUpdatedFlag) {
	struct opahrs_msg_v1 req;

	load_baro_altitude(&(req.payload.user.v.req.altitude));
	if ((result = PIOS_OPAHRS_SetBaroAltitude(&req)) == OPAHRS_RESULT_OK) {
	  BaroAltitudeIsUpdatedFlag = false;
	} else {
	  /* Comms error */
      altitude_errors++;
      break;
	}
      }

      if (PositionActualIsUpdatedFlag) {
        struct opahrs_msg_v1 req;
        
        load_position_actual(&(req.payload.user.v.req.gps));
        if ((result = PIOS_OPAHRS_SetPositionActual(&req)) == OPAHRS_RESULT_OK) {
          PositionActualIsUpdatedFlag = false;
        } else {
          /* Comms error */
          position_errors++;
          break;
        }
      }

      if (HomeLocationIsUpdatedFlag || !AHRSKnowsHome) {
        struct opahrs_msg_v1 req;
        
        load_magnetic_north(&(req.payload.user.v.req.north));
        if ((result = PIOS_OPAHRS_SetMagNorth(&req)) == OPAHRS_RESULT_OK) {
          HomeLocationIsUpdatedFlag = false;
          AHRSKnowsHome = TRUE;
        } else {
          /* Comms error */
          PIOS_LED_Off(LED2);
          home_errors++;
          break;
        }
      }    
      
      /* Wait for the next update interval */
      vTaskDelay( settings.UpdatePeriod / portTICK_RATE_MS );
    }
  }
}

static void load_magnetic_north(struct opahrs_msg_v1_req_north * mag_north)
{
  HomeLocationData   data;
  
  HomeLocationGet(&data);
  mag_north->Be[0] = data.Be[0];
  mag_north->Be[1] = data.Be[1];
  mag_north->Be[2] = data.Be[2];

  if(data.Be[0] == 0 && data.Be[1] == 0 && data.Be[2] == 0)
  {
    // in case nothing has been set go to default to prevent NaN in attitude solution
    mag_north->Be[0] = 1;  
    mag_north->Be[1] = 0;
    mag_north->Be[2] = 0;
  }
  else {
    // normalize for unit length here
    float len = sqrt(data.Be[0] * data.Be[0] + data.Be[1] * data.Be[1] + data.Be[2] * data.Be[2]);
    mag_north->Be[0] = data.Be[0] / len;
    mag_north->Be[1] = data.Be[1] / len;
    mag_north->Be[2] = data.Be[2] / len;
  }


}

static void load_baro_altitude(struct opahrs_msg_v1_req_altitude * altitude)
{
  BaroAltitudeData   data;

  BaroAltitudeGet(&data);

  altitude->altitude    = data.Altitude;
  altitude->pressure    = data.Pressure;
  altitude->temperature = data.Temperature;
}

static void load_position_actual(struct opahrs_msg_v1_req_gps * gps)
{
  PositionActualData data;
  PositionActualGet(&data);
  
  gps->latitude = data.Latitude;
	gps->longitude = data.Longitude;
	gps->altitude = data.GeoidSeparation;
  gps->heading = data.Heading;
  gps->groundspeed = data.Groundspeed;
  gps->status = data.Status;
}

static void update_attitude_actual(struct opahrs_msg_v1_rsp_attitude * attitude)
{
  AttitudeActualData   data;

  data.q1 = attitude->quaternion.q1;
  data.q2 = attitude->quaternion.q2;
  data.q3 = attitude->quaternion.q3;
  data.q4 = attitude->quaternion.q4;
  
  data.Roll  = attitude->euler.roll;
  data.Pitch = attitude->euler.pitch;
  data.Yaw   = attitude->euler.yaw;
  
  AttitudeActualSet(&data);
}

static void update_attitude_raw(struct opahrs_msg_v1_rsp_attituderaw * attituderaw)
{
  AttitudeRawData    data;

  data.magnetometers[ATTITUDERAW_MAGNETOMETERS_X] = attituderaw->mags.x;
  data.magnetometers[ATTITUDERAW_MAGNETOMETERS_Y] = attituderaw->mags.y;
  data.magnetometers[ATTITUDERAW_MAGNETOMETERS_Z] = attituderaw->mags.z;

  data.gyros[ATTITUDERAW_GYROS_X] = attituderaw->gyros.x;
  data.gyros[ATTITUDERAW_GYROS_Y] = attituderaw->gyros.y;
  data.gyros[ATTITUDERAW_GYROS_Z] = attituderaw->gyros.z;

  data.gyros_filtered[ATTITUDERAW_GYROS_FILTERED_X] = attituderaw->gyros_filtered.x;
  data.gyros_filtered[ATTITUDERAW_GYROS_FILTERED_Y] = attituderaw->gyros_filtered.y;
  data.gyros_filtered[ATTITUDERAW_GYROS_FILTERED_Z] = attituderaw->gyros_filtered.z;
  
  data.gyrotemp[ATTITUDERAW_GYROTEMP_XY] = attituderaw->gyros.xy_temp;
  data.gyrotemp[ATTITUDERAW_GYROTEMP_Z] = attituderaw->gyros.z_temp;

  data.accels[ATTITUDERAW_ACCELS_X] = attituderaw->accels.x;
  data.accels[ATTITUDERAW_ACCELS_Y] = attituderaw->accels.y;
  data.accels[ATTITUDERAW_ACCELS_Z] = attituderaw->accels.z;

  data.accels_filtered[ATTITUDERAW_ACCELS_FILTERED_X] = attituderaw->accels_filtered.x;
  data.accels_filtered[ATTITUDERAW_ACCELS_FILTERED_Y] = attituderaw->accels_filtered.y;
  data.accels_filtered[ATTITUDERAW_ACCELS_FILTERED_Z] = attituderaw->accels_filtered.z;
  
  AttitudeRawSet(&data);
}

static void update_ahrs_status(struct opahrs_msg_v1_rsp_serial * serial)
{
  AhrsStatusData       data;

  // Get the current object data
  AhrsStatusGet(&data);

  for (uint8_t i = 0; i < sizeof(serial->serial_bcd); i++) {
    data.SerialNumber[i] = serial->serial_bcd[i];
  }

  data.CommErrors[AHRSSTATUS_COMMERRORS_ATTITUDE] = attitude_errors;
  data.CommErrors[AHRSSTATUS_COMMERRORS_ATTITUDERAW] = attituderaw_errors;
  data.CommErrors[AHRSSTATUS_COMMERRORS_POSITIONACTUAL] = position_errors;
  data.CommErrors[AHRSSTATUS_COMMERRORS_HOMELOCATION] = home_errors;
  data.CommErrors[AHRSSTATUS_COMMERRORS_ALTITUDE] = altitude_errors;

  AhrsStatusSet(&data);
}

/**
  * @}
  * @}
  */
