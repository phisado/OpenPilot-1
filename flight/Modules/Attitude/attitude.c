/**
 ******************************************************************************
 * @addtogroup OpenPilotModules OpenPilot Modules
 * @{
 * @addtogroup Attitude Copter Control Attitude Estimation
 * @brief Acquires sensor data and computes attitude estimate
 * Specifically updates the the @ref AttitudeActual "AttitudeActual" and @ref AttitudeRaw "AttitudeRaw" settings objects
 * @{
 *
 * @file       attitude.c
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
 * Input objects: None, takes sensor data via pios
 * Output objects: @ref AttitudeRaw @ref AttitudeActual
 *
 * This module computes an attitude estimate from the sensor data
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

#include "pios.h"
#include "attitude.h"
#include "gyros.h"
#include "accels.h"
#include "attitudeactual.h"
#include "attitudesettings.h"
#include "flightstatus.h"
#include "manualcontrolcommand.h"
#include "CoordinateConversions.h"
#include <pios_board_info.h>
 
// Private constants
#define STACK_SIZE_BYTES 540
#define TASK_PRIORITY (tskIDLE_PRIORITY+3)

#define SENSOR_PERIOD 4

#define PI_MOD(x) (fmod(x + M_PI, M_PI * 2) - M_PI)
// Private types

// Private variables
static xTaskHandle taskHandle;

// Private functions
static void AttitudeTask(void *parameters);

static float gyro_correct_int[3] = {0,0,0};

static int32_t updateSensorsCC3D(AccelsData * accelsData, GyrosData * gyrosData);
static void updateAttitude(AccelsData *, GyrosData *);
static void settingsUpdatedCb(UAVObjEvent * objEv);

static float accelKi = 0;
static float accelKp = 0;
static bool accel_filter_enabled = false;
static float accels_filtered[3];
static float grot_filtered[3];
static float yawBiasRate = 0;
static float gyroGain = 0.42;
static int16_t accelbias[3];
static float q[4] = {1,0,0,0};
static float R[3][3];
static int8_t rotate = 0;
static bool zero_during_arming = false;
static bool bias_correct_gyro = true;

// For running trim flights
static volatile bool trim_requested = false;
static volatile int32_t trim_accels[3];
static volatile int32_t trim_samples;
int32_t const MAX_TRIM_FLIGHT_SAMPLES = 65535;

#define GRAV         9.81f
#define ACCEL_SCALE  (GRAV * 0.004f)
/* 0.004f is gravity / LSB */

/**
 * D I G I T A L   L O W   P A S S   F I L T E R
 *
 * Digital 4-th order Chebyshev type II low pass filter
 * cheby2(4,60,10/200) - 60dB attenuation, 10Hz cutoff @ 400Hz sampling
 */
#define _b0  0.00098778675104f
#define _b1 -0.00376234890193f
#define _b2  0.00555374469529f
#define _b3 -0.00376234890193f
#define _b4  0.00098778675104f

#define _a1 -3.87812973499889f
#define _a2  5.64176257281588f
#define _a3 -3.64887595541910f
#define _a4  0.88524773799562f

typedef struct {
	float inputTm1, inputTm2, inputTm3, inputTm4;
	float outputTm1, outputTm2, outputTm3, outputTm4;
} fourthOrderData_t, fourthOrderData;

fourthOrderData filterParams_acc[3];
fourthOrderData filterParams_grot[3];

/**
 *
 *
 */
float computeFourthOrder(float currentInput, fourthOrderData * filterParameters)
{
	float output;

	output =
		_b0 * currentInput +
		_b1 * filterParameters->inputTm1 +
		_b2 * filterParameters->inputTm2 +
		_b3 * filterParameters->inputTm3 +
		_b4 * filterParameters->inputTm4 -
		_a1 * filterParameters->outputTm1 -
		_a2 * filterParameters->outputTm2 -
		_a3 * filterParameters->outputTm3 -
		_a4 * filterParameters->outputTm4;

	filterParameters->inputTm4 = filterParameters->inputTm3;
	filterParameters->inputTm3 = filterParameters->inputTm2;
	filterParameters->inputTm2 = filterParameters->inputTm1;
	filterParameters->inputTm1 = currentInput;

	filterParameters->outputTm4 = filterParameters->outputTm3;
	filterParameters->outputTm3 = filterParameters->outputTm2;
	filterParameters->outputTm2 = filterParameters->outputTm1;
	filterParameters->outputTm1 = output;

	return output;
}

/**
 * Initialise the module, called on startup
 * \returns 0 on success or -1 if initialisation failed
 */
int32_t AttitudeStart(void)
{
	
	// Start main task
	xTaskCreate(AttitudeTask, (signed char *)"Attitude", STACK_SIZE_BYTES/4, NULL, TASK_PRIORITY, &taskHandle);
	TaskMonitorAdd(TASKINFO_RUNNING_ATTITUDE, taskHandle);
	PIOS_WDG_RegisterFlag(PIOS_WDG_ATTITUDE);
	
	return 0;
}

/**
 * Initialise the module, called on startup
 * \returns 0 on success or -1 if initialisation failed
 */
int32_t AttitudeInitialize(void)
{
	AttitudeActualInitialize();
	AttitudeSettingsInitialize();
	AccelsInitialize();
	GyrosInitialize();

	// Initialize quaternion
	AttitudeActualData attitude;
	AttitudeActualGet(&attitude);
	attitude.q1 = 1;
	attitude.q2 = 0;
	attitude.q3 = 0;
	attitude.q4 = 0;
	AttitudeActualSet(&attitude);

	// Cannot trust the values to init right above if BL runs
	gyro_correct_int[0] = 0;
	gyro_correct_int[1] = 0;
	gyro_correct_int[2] = 0;

	q[0] = 1;
	q[1] = 0;
	q[2] = 0;
	q[3] = 0;
	for(uint8_t i = 0; i < 3; i++)
		for(uint8_t j = 0; j < 3; j++)
			R[i][j] = 0;

	trim_requested = false;

	AttitudeSettingsConnectCallback(&settingsUpdatedCb);

	return 0;
}

MODULE_INITCALL(AttitudeInitialize, AttitudeStart)

/**
 * Module thread, should not return.
 */
 
int32_t accel_test;
int32_t gyro_test;
static void AttitudeTask(void *parameters)
{
	uint8_t init = 0;
	AlarmsClear(SYSTEMALARMS_ALARM_ATTITUDE);

#if defined(PIOS_INCLUDE_MPU6050)

	gyro_test = accel_test = PIOS_MPU6050_Test();

#endif /* PIOS_INCLUDE_MPU6050 */

	// Force settings update to make sure rotation loaded
	settingsUpdatedCb(AttitudeSettingsHandle());

	// Main task loop
	while (1) {
		
		FlightStatusData flightStatus;
		FlightStatusGet(&flightStatus);
		
		if((xTaskGetTickCount() < 7000) && (xTaskGetTickCount() > 1000)) {
			// For first 7 seconds use accels to get gyro bias
			accelKp = 1;
			accelKi = 0.9;
			yawBiasRate = 0.23;
			accel_filter_enabled = false;
			init = 0;
		}
		else if (zero_during_arming && (flightStatus.Armed == FLIGHTSTATUS_ARMED_ARMING)) {
			accelKp = 1;
			accelKi = 0.9;
			yawBiasRate = 0.23;
			accel_filter_enabled = false;
			init = 0;
		} else if (init == 0) {
			// Reload settings (all the rates)
			AttitudeSettingsAccelKiGet(&accelKi);
			AttitudeSettingsAccelKpGet(&accelKp);
			AttitudeSettingsYawBiasRateGet(&yawBiasRate);
			accel_filter_enabled = true;
			init = 1;
		}
		
		PIOS_WDG_UpdateFlag(PIOS_WDG_ATTITUDE);

		AccelsData accels;
		GyrosData gyros;
		int32_t retval = 0;

		retval = updateSensorsCC3D(&accels, &gyros);

		// Only update attitude when sensor data is good
		if (retval != 0)
			AlarmsSet(SYSTEMALARMS_ALARM_ATTITUDE, SYSTEMALARMS_ALARM_ERROR);
		else {
			// Do not update attitude data in simulation mode
			if (!AttitudeActualReadOnly())
				updateAttitude(&accels, &gyros);

			AlarmsClear(SYSTEMALARMS_ALARM_ATTITUDE);
		}
	}
}

/**
 * Get an update from the sensors
 * @param[in] attitudeRaw Populate the UAVO instead of saving right here
 * @return 0 if successfull, -1 if not
 */
static int32_t updateSensorsCC3D(AccelsData * accelsData, GyrosData * gyrosData)
{
	float accels[3], gyros[3];

	// Do not read raw sensor data in simulation mode
	if (GyrosReadOnly() || AccelsReadOnly())
		return 0;

#if defined(PIOS_INCLUDE_MPU6050)

	static struct pios_mpu6050_data mpu6050_data;
	xQueueHandle queue = PIOS_MPU6050_GetQueue();

	if(xQueueReceive(queue, (void *) &mpu6050_data, SENSOR_PERIOD) == errQUEUE_EMPTY){
		return -1;	// Error, no data
	}

	gyros[0] =  mpu6050_data.gyro_x * PIOS_MPU6050_GetScale();
	gyros[1] = -mpu6050_data.gyro_y * PIOS_MPU6050_GetScale();
	gyros[2] = -mpu6050_data.gyro_z * PIOS_MPU6050_GetScale();

	accels[0] =  mpu6050_data.accel_x * PIOS_MPU6050_GetAccelScale();
	accels[1] = -mpu6050_data.accel_y * PIOS_MPU6050_GetAccelScale();
	accels[2] = -mpu6050_data.accel_z * PIOS_MPU6050_GetAccelScale();

	gyrosData->temperature  = 35.0f + ((float) mpu6050_data.temperature + 512.0f) / 340.0f;
	accelsData->temperature = 35.0f + ((float) mpu6050_data.temperature + 512.0f) / 340.0f;

#endif /* PIOS_INCLUDE_MPU6050 */

	if(rotate) {
		// TODO: rotate sensors too so stabilization is well behaved
		float vec_out[3];
		rot_mult(R, accels, vec_out);
		accels[0] = vec_out[0];
		accels[1] = vec_out[1];
		accels[2] = vec_out[2];
		rot_mult(R, gyros, vec_out);
		gyros[0] = vec_out[0];
		gyros[1] = vec_out[1];
		gyros[2] = vec_out[2];
	}

	accelsData->x = accels[0] - accelbias[0] * ACCEL_SCALE; // Applying arbitrary scale here to match CC v1
	accelsData->y = accels[1] - accelbias[1] * ACCEL_SCALE;
	accelsData->z = accels[2] - accelbias[2] * ACCEL_SCALE;

	gyrosData->x = gyros[0];
	gyrosData->y = gyros[1];
	gyrosData->z = gyros[2];

	if(bias_correct_gyro) {
		// Applying integral component here so it can be seen on the gyros and correct bias
		gyrosData->x += gyro_correct_int[0];
		gyrosData->y += gyro_correct_int[1];
		gyrosData->z += gyro_correct_int[2];
	}

	// Because most crafts wont get enough information from gravity to zero yaw gyro, we try
	// and make it average zero (weakly)
	gyro_correct_int[2] += - gyrosData->z * yawBiasRate;

	GyrosSet(gyrosData);
	AccelsSet(accelsData);

	return 0;
}

static inline void apply_accel_filter(const float *raw, float *filtered, fourthOrderData *filterParams)
{
	if (accel_filter_enabled) {
		filtered[0] = computeFourthOrder(raw[0],&filterParams[0]);
		filtered[1] = computeFourthOrder(raw[1],&filterParams[1]);
		filtered[2] = computeFourthOrder(raw[2],&filterParams[2]);
	} else {
		filtered[0] = raw[0];
		filtered[1] = raw[1];
		filtered[2] = raw[2];
	}
}

static void updateAttitude(AccelsData * accelsData, GyrosData * gyrosData)
{
	float dT;
	portTickType thisSysTime = xTaskGetTickCount();
	static portTickType lastSysTime = 0;

	dT = (thisSysTime == lastSysTime) ? 0.001 : (portMAX_DELAY & (thisSysTime - lastSysTime)) / portTICK_RATE_MS / 1000.0f;
	lastSysTime = thisSysTime;

	// Bad practice to assume structure order, but saves memory
	float * gyros = &gyrosData->x;
	float * accels = &accelsData->x;

	float grot[3];
	float accel_err[3];

	// Apply smoothing to accel values, to reduce vibration noise before main calculations.
	apply_accel_filter(accels, accels_filtered, filterParams_acc);

	// Rotate gravity to body frame and cross with accels
	grot[0] = -(2 * (q[1] * q[3] - q[0] * q[2]));
	grot[1] = -(2 * (q[2] * q[3] + q[0] * q[1]));
	grot[2] = -(q[0] * q[0] - q[1]*q[1] - q[2]*q[2] + q[3]*q[3]);

	apply_accel_filter(grot, grot_filtered, filterParams_grot);

	CrossProduct((const float *)accels_filtered, (const float *)grot_filtered, accel_err);

	// Account for accel magnitude
	float accel_mag = sqrtf(accels_filtered[0]*accels_filtered[0] + accels_filtered[1]*accels_filtered[1] + accels_filtered[2]*accels_filtered[2]);
	if (accel_mag < 1.0e-3f)
		return;

	// Account for filtered gravity vector magnitude
	float grot_mag;

	if (accel_filter_enabled)
		grot_mag = sqrtf(grot_filtered[0]*grot_filtered[0] + grot_filtered[1]*grot_filtered[1] + grot_filtered[2]*grot_filtered[2]);
	else
		grot_mag = 1.0f;

	if (grot_mag < 1.0e-3f)
		return;

	accel_err[0] /= (accel_mag*grot_mag);
	accel_err[1] /= (accel_mag*grot_mag);
	accel_err[2] /= (accel_mag*grot_mag);

	// Accumulate integral of error.  Scale here so that units are (deg/s) but Ki has units of s
	gyro_correct_int[0] += accel_err[0] * accelKi;
	gyro_correct_int[1] += accel_err[1] * accelKi;

	//gyro_correct_int[2] += accel_err[2] * accelKi;

	// Correct rates based on error, integral component dealt with in updateSensors
	gyros[0] += accel_err[0] * accelKp / dT;
	gyros[1] += accel_err[1] * accelKp / dT;
	gyros[2] += accel_err[2] * accelKp / dT;

	{ // scoping variables to save memory
		// Work out time derivative from INSAlgo writeup
		// Also accounts for the fact that gyros are in deg/s
		float qdot[4];
		qdot[0] = (-q[1] * gyros[0] - q[2] * gyros[1] - q[3] * gyros[2]) * dT * M_PI / 180 / 2;
		qdot[1] = (q[0] * gyros[0] - q[3] * gyros[1] + q[2] * gyros[2]) * dT * M_PI / 180 / 2;
		qdot[2] = (q[3] * gyros[0] + q[0] * gyros[1] - q[1] * gyros[2]) * dT * M_PI / 180 / 2;
		qdot[3] = (-q[2] * gyros[0] + q[1] * gyros[1] + q[0] * gyros[2]) * dT * M_PI / 180 / 2;

		// Take a time step
		q[0] = q[0] + qdot[0];
		q[1] = q[1] + qdot[1];
		q[2] = q[2] + qdot[2];
		q[3] = q[3] + qdot[3];

		if(q[0] < 0) {
			q[0] = -q[0];
			q[1] = -q[1];
			q[2] = -q[2];
			q[3] = -q[3];
		}
	}

	// Renomalize
	float qmag = sqrtf(q[0]*q[0] + q[1]*q[1] + q[2]*q[2] + q[3]*q[3]);
	q[0] = q[0] / qmag;
	q[1] = q[1] / qmag;
	q[2] = q[2] / qmag;
	q[3] = q[3] / qmag;

	// If quaternion has become inappropriately short or is nan reinit.
	// THIS SHOULD NEVER ACTUALLY HAPPEN
	if((fabs(qmag) < 1e-3) || (qmag != qmag)) {
		q[0] = 1;
		q[1] = 0;
		q[2] = 0;
		q[3] = 0;
	}

	AttitudeActualData attitudeActual;
	AttitudeActualGet(&attitudeActual);

	quat_copy(q, &attitudeActual.q1);

	// Convert into eueler degrees (makes assumptions about RPY order)
	Quaternion2RPY(&attitudeActual.q1,&attitudeActual.Roll);

	AttitudeActualSet(&attitudeActual);
}

static void settingsUpdatedCb(UAVObjEvent * objEv) {
	AttitudeSettingsData attitudeSettings;
	AttitudeSettingsGet(&attitudeSettings);

	accelKp = attitudeSettings.AccelKp;
	accelKi = attitudeSettings.AccelKi;
	yawBiasRate = attitudeSettings.YawBiasRate;
	gyroGain = attitudeSettings.GyroGain;

	zero_during_arming = attitudeSettings.ZeroDuringArming == ATTITUDESETTINGS_ZERODURINGARMING_TRUE;
	bias_correct_gyro = attitudeSettings.BiasCorrectGyro == ATTITUDESETTINGS_BIASCORRECTGYRO_TRUE;

	accelbias[0] = attitudeSettings.AccelBias[ATTITUDESETTINGS_ACCELBIAS_X];
	accelbias[1] = attitudeSettings.AccelBias[ATTITUDESETTINGS_ACCELBIAS_Y];
	accelbias[2] = attitudeSettings.AccelBias[ATTITUDESETTINGS_ACCELBIAS_Z];

	gyro_correct_int[0] = attitudeSettings.GyroBias[ATTITUDESETTINGS_GYROBIAS_X] / 100.0f;
	gyro_correct_int[1] = attitudeSettings.GyroBias[ATTITUDESETTINGS_GYROBIAS_Y] / 100.0f;
	gyro_correct_int[2] = attitudeSettings.GyroBias[ATTITUDESETTINGS_GYROBIAS_Z] / 100.0f;

	// Indicates not to expend cycles on rotation
	if(attitudeSettings.BoardRotation[0] == 0 && attitudeSettings.BoardRotation[1] == 0 &&
	   attitudeSettings.BoardRotation[2] == 0) {
		rotate = 0;

		// Shouldn't be used but to be safe
		float rotationQuat[4] = {1,0,0,0};
		Quaternion2R(rotationQuat, R);
	} else {
		float rotationQuat[4];
		const float rpy[3] = {attitudeSettings.BoardRotation[ATTITUDESETTINGS_BOARDROTATION_ROLL],
			attitudeSettings.BoardRotation[ATTITUDESETTINGS_BOARDROTATION_PITCH],
			attitudeSettings.BoardRotation[ATTITUDESETTINGS_BOARDROTATION_YAW]};
		RPY2Quaternion(rpy, rotationQuat);
		Quaternion2R(rotationQuat, R);
		rotate = 1;
	}

	if (attitudeSettings.TrimFlight == ATTITUDESETTINGS_TRIMFLIGHT_START) {
		trim_accels[0] = 0;
		trim_accels[1] = 0;
		trim_accels[2] = 0;
		trim_samples = 0;
		trim_requested = true;
	} else if (attitudeSettings.TrimFlight == ATTITUDESETTINGS_TRIMFLIGHT_LOAD) {
		trim_requested = false;
		attitudeSettings.AccelBias[ATTITUDESETTINGS_ACCELBIAS_X] = trim_accels[0] / trim_samples;
		attitudeSettings.AccelBias[ATTITUDESETTINGS_ACCELBIAS_Y] = trim_accels[1] / trim_samples;
		// Z should average -grav
		attitudeSettings.AccelBias[ATTITUDESETTINGS_ACCELBIAS_Z] = trim_accels[2] / trim_samples + GRAV / ACCEL_SCALE;
		attitudeSettings.TrimFlight = ATTITUDESETTINGS_TRIMFLIGHT_NORMAL;
		AttitudeSettingsSet(&attitudeSettings);
	} else
		trim_requested = false;
}
/**
 * @}
 * @}
 */
