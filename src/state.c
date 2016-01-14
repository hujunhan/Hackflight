#include "board.h"
#include "mw.h"
#include "config.h"

int16_t  gyroADC[3], accADC[3], accSmooth[3], magADC[3];
int32_t  accSum[3];
uint32_t accTimeSum = 0;        // keep track for integration of acc
int      accSumCount = 0;
int16_t  smallAngle = 0;
int32_t  baroPressure = 0;
int32_t  baroTemperature = 0;
uint32_t baroPressureSum = 0;
int32_t  BaroAlt = 0;
int32_t  AltPID = 0;
int32_t  baroAlt_offset = 0;
int32_t  SonarAlt = 0;
float    sonarTransition = 0;
int32_t  EstAlt;                // in cm
int32_t  AltHold;
int32_t  setVelocity = 0;
uint8_t  velocityControl = 0;
int32_t  errorVelocityI = 0;
int32_t  vario = 0;                      // variometer in cm/s
int16_t  throttleAngleCorrection = 0;    // correction of throttle in lateral wind,
float    magneticDeclination = 0.0f;       // calculated at startup from config
float    accVelScale;
float    throttleAngleScale;
float    fc_acc;

// **************
// gyro+acc IMU
// **************
int16_t gyroData[3] = { 0, 0, 0 };
int16_t gyroZero[3] = { 0, 0, 0 };
int16_t angle[2] = { 0, 0 };     // absolute angle inclination in multiple of 0.1 degree    180 deg = 1800
float anglerad[2] = { 0.0f, 0.0f };    // absolute angle inclination in radians

static void getEstimatedAttitude(void);

void imuInit(void)
{
    smallAngle = lrintf(acc_1G * cosf(RAD * CONFIG_SMALL_ANGLE));
    accVelScale = 9.80665f / acc_1G / 10000.0f;
    throttleAngleScale = (1800.0f / M_PI) * (900.0f / CONFIG_THROTTLE_CORRECTION_ANGLE);

    fc_acc = 0.5f / (M_PI * CONFIG_ACCZ_LPF_CUTOFF); // calculate RC time constant used in the accZ lpf
}

void computeIMU(void)
{
    Gyro_getADC();
    ACC_getADC();
    getEstimatedAttitude();

    gyroData[YAW] = gyroADC[YAW];
    gyroData[ROLL] = gyroADC[ROLL];
    gyroData[PITCH] = gyroADC[PITCH];
}

// **************************************************
// Simplified IMU based on "Complementary Filter"
// Inspired by http://starlino.com/imu_guide.html
//
// adapted by ziss_dm : http://www.multiwii.com/forum/viewtopic.php?f=8&t=198
//
// The following ideas was used in this project:
// 1) Rotation matrix: http://en.wikipedia.org/wiki/Rotation_matrix
//
// Currently Magnetometer uses separate CF which is used only
// for heading approximation.
//
// **************************************************

#define INV_GYR_CMPF_FACTOR   (1.0f / ((float)CONFIG_GYRO_CMPF_FACTOR + 1.0f))

typedef struct fp_vector {
    float X;
    float Y;
    float Z;
} t_fp_vector_def;

typedef union {
    float A[3];
    t_fp_vector_def V;
} t_fp_vector;

t_fp_vector EstG;

// Normalize a vector
void normalizeV(struct fp_vector *src, struct fp_vector *dest)
{
    float length;

    length = sqrtf(src->X * src->X + src->Y * src->Y + src->Z * src->Z);
    if (length != 0) {
        dest->X = src->X / length;
        dest->Y = src->Y / length;
        dest->Z = src->Z / length;
    }
}

// Rotate Estimated vector(s) with small angle approximation, according to the gyro data
void rotateV(struct fp_vector *v, float *delta)
{
    struct fp_vector v_tmp = *v;

    // This does a  "proper" matrix rotation using gyro deltas without small-angle approximation
    float mat[3][3];
    float cosx, sinx, cosy, siny, cosz, sinz;
    float coszcosx, sinzcosx, coszsinx, sinzsinx;

    cosx = cosf(delta[ROLL]);
    sinx = sinf(delta[ROLL]);
    cosy = cosf(delta[PITCH]);
    siny = sinf(delta[PITCH]);
    cosz = cosf(delta[YAW]);
    sinz = sinf(delta[YAW]);

    coszcosx = cosz * cosx;
    sinzcosx = sinz * cosx;
    coszsinx = sinx * cosz;
    sinzsinx = sinx * sinz;

    mat[0][0] = cosz * cosy;
    mat[0][1] = -cosy * sinz;
    mat[0][2] = siny;
    mat[1][0] = sinzcosx + (coszsinx * siny);
    mat[1][1] = coszcosx - (sinzsinx * siny);
    mat[1][2] = -sinx * cosy;
    mat[2][0] = (sinzsinx) - (coszcosx * siny);
    mat[2][1] = (coszsinx) + (sinzcosx * siny);
    mat[2][2] = cosy * cosx;

    v->X = v_tmp.X * mat[0][0] + v_tmp.Y * mat[1][0] + v_tmp.Z * mat[2][0];
    v->Y = v_tmp.X * mat[0][1] + v_tmp.Y * mat[1][1] + v_tmp.Z * mat[2][1];
    v->Z = v_tmp.X * mat[0][2] + v_tmp.Y * mat[1][2] + v_tmp.Z * mat[2][2];
}

int32_t applyDeadband(int32_t value, int32_t deadband)
{
    if (abs(value) < deadband) {
        value = 0;
    } else if (value > 0) {
        value -= deadband;
    } else if (value < 0) {
        value += deadband;
    }
    return value;
}

// rotate acc into Earth frame and calculate acceleration in it
void acc_calc(uint32_t deltaT)
{
    static int32_t accZoffset = 0;
    static float accz_smooth = 0;
    float dT = 0;
    float rpy[3];
    t_fp_vector accel_ned;

    // deltaT is measured in us ticks
    dT = (float)deltaT * 1e-6f;

    // the accel values have to be rotated into the earth frame
    rpy[0] = -(float)anglerad[ROLL];
    rpy[1] = -(float)anglerad[PITCH];
    rpy[2] = -(float)heading * RAD;

    accel_ned.V.X = accSmooth[0];
    accel_ned.V.Y = accSmooth[1];
    accel_ned.V.Z = accSmooth[2];

    rotateV(&accel_ned.V, rpy);

    if (CONFIG_ACC_UNARMEDCAL == 1) {
        if (!armed) {
            accZoffset -= accZoffset / 64;
            accZoffset += accel_ned.V.Z;
        }
        accel_ned.V.Z -= accZoffset / 64;  // compensate for gravitation on z-axis
    } else
        accel_ned.V.Z -= acc_1G;

    accz_smooth = accz_smooth + (dT / (fc_acc + dT)) * (accel_ned.V.Z - accz_smooth); // low pass filter

    // apply Deadband to reduce integration drift and vibration influence and
    // sum up Values for later integration to get velocity and distance
    accSum[X] += applyDeadband(lrintf(accel_ned.V.X), CONFIG_ACCXY_DEADBAND);
    accSum[Y] += applyDeadband(lrintf(accel_ned.V.Y), CONFIG_ACCXY_DEADBAND);
    accSum[Z] += applyDeadband(lrintf(accz_smooth), CONFIG_ACCZ_DEADBAND);

    accTimeSum += deltaT;
    accSumCount++;
}

void accSum_reset(void)
{
    accSum[0] = 0;
    accSum[1] = 0;
    accSum[2] = 0;
    accSumCount = 0;
    accTimeSum = 0;
}

// baseflight calculation by Luggi09 originates from arducopter
static int16_t calculateHeading(t_fp_vector *vec)
{
    int16_t head;

    float cosineRoll = cosf(anglerad[ROLL]);
    float sineRoll = sinf(anglerad[ROLL]);
    float cosinePitch = cosf(anglerad[PITCH]);
    float sinePitch = sinf(anglerad[PITCH]);
    float Xh = vec->A[X] * cosinePitch + vec->A[Y] * sineRoll * sinePitch + vec->A[Z] * sinePitch * cosineRoll;
    float Yh = vec->A[Y] * cosineRoll - vec->A[Z] * sineRoll;
    float hd = (atan2f(Yh, Xh) * 1800.0f / M_PI + magneticDeclination) / 10.0f;
    head = lrintf(hd);
    if (head < 0)
        head += 360;

    return head;
}

static void getEstimatedAttitude(void)
{
    int32_t axis;
    int32_t accMag = 0;
    static t_fp_vector EstN = { .A = { 1.0f, 0.0f, 0.0f } };
    static float accLPF[3];
    static uint32_t previousT;
    uint32_t currentT = micros();
    uint32_t deltaT;
    float scale, deltaGyroAngle[3];
    deltaT = currentT - previousT;
    scale = deltaT * gyro.scale;
    previousT = currentT;

    // Initialization
    for (axis = 0; axis < 3; axis++) {
        deltaGyroAngle[axis] = gyroADC[axis] * scale;
        if (CONFIG_ACC_LPF_FACTOR > 0) {
            accLPF[axis] = accLPF[axis] * (1.0f - (1.0f / CONFIG_ACC_LPF_FACTOR)) + accADC[axis] * 
                (1.0f / CONFIG_ACC_LPF_FACTOR);
            accSmooth[axis] = accLPF[axis];
        } else {
            accSmooth[axis] = accADC[axis];
        }
        accMag += (int32_t)accSmooth[axis] * accSmooth[axis];
    }
    accMag = accMag * 100 / ((int32_t)acc_1G * acc_1G);

    rotateV(&EstG.V, deltaGyroAngle);

    // Apply complementary filter (Gyro drift correction)
    // If accel magnitude >1.15G or <0.85G and ACC vector outside of the limit range => we neutralize the effect of 
    // accelerometers in the angle estimation.  To do that, we just skip filter, as EstV already rotated by Gyro.
    if (72 < (uint16_t)accMag && (uint16_t)accMag < 133) {
        for (axis = 0; axis < 3; axis++)
            EstG.A[axis] = (EstG.A[axis] * (float)CONFIG_GYRO_CMPF_FACTOR + accSmooth[axis]) * INV_GYR_CMPF_FACTOR;
    }

    useSmallAngle = (EstG.A[Z] > smallAngle);

    // Attitude of the estimated vector
    anglerad[ROLL] = atan2f(EstG.V.Y, EstG.V.Z);
    anglerad[PITCH] = atan2f(-EstG.V.X, sqrtf(EstG.V.Y * EstG.V.Y + EstG.V.Z * EstG.V.Z));
    angle[ROLL] = lrintf(anglerad[ROLL] * (1800.0f / M_PI));
    angle[PITCH] = lrintf(anglerad[PITCH] * (1800.0f / M_PI));

    rotateV(&EstN.V, deltaGyroAngle);
    normalizeV(&EstN.V, &EstN.V);
    heading = calculateHeading(&EstN);

    acc_calc(deltaT); // rotate acc vector into earth frame

    if (CONFIG_THROTTLE_CORRECTION_VALUE) {

        float cosZ = EstG.V.Z / sqrtf(EstG.V.X * EstG.V.X + EstG.V.Y * EstG.V.Y + EstG.V.Z * EstG.V.Z);

        if (cosZ <= 0.015f) { // we are inverted, vertical or with a small angle < 0.86 deg
            throttleAngleCorrection = 0;
        } else {
            int deg = lrintf(acosf(cosZ) * throttleAngleScale);
            if (deg > 900)
                deg = 900;
            throttleAngleCorrection = lrintf(CONFIG_THROTTLE_CORRECTION_VALUE * sinf(deg / (900.0f * M_PI / 2.0f)));
        }
    }
}

static bool sonarInRange(void)
{
    return SonarAlt > 20 && SonarAlt < 765;
}

// complementary filter
static float cfilter(float a, float b, float c) 
{
    return a * c + b * (1 - c);
}

int getEstimatedAltitude(void)
{
    static uint32_t previousT;
    static float accZ_old;
    static float accelVel;
    static float accelAlt;
    static int32_t lastBaroAlt;
    static int32_t baroGroundAltitude;
    static int32_t baroGroundPressure;

    uint32_t currentT = micros();

    int16_t tiltAngle = max(abs(angle[ROLL]), abs(angle[PITCH]));

    uint32_t dTime = currentT - previousT;

    if (dTime < CONFIG_ALT_UPDATE_USEC)
        return 0;
    previousT = currentT;

    // Automatically calibrate baro on startup
    if (calibratingB > 0) {
        baroGroundPressure -= baroGroundPressure / 8;
        baroGroundPressure += baroPressureSum / (CONFIG_BARO_TAB_SIZE - 1);
        baroGroundAltitude = (1.0f - powf((baroGroundPressure / 8) / 101325.0f, 0.190295f)) * 4433000.0f;

        accelVel = 0;
        accelAlt = 0;
        calibratingB--;
    }

    // Calculates height from ground via baro readings in cm
    // See: https://github.com/diydrones/ardupilot/blob/master/libraries/AP_Baro/AP_Baro.cpp#L140
    int32_t BaroAlt_tmp = lrintf((1.0f - powf((float)(baroPressureSum / (CONFIG_BARO_TAB_SIZE - 1)) 
                    / 101325.0f, 0.190295f)) * 4433000.0f) - baroGroundAltitude;

    // Additional low-pass filter to reduce baro noise
    //BaroAlt = lrintf((float)BaroAlt * CONFIG_BARO_NOISE_LPF + (float)BaroAlt_tmp * (1.0f - CONFIG_BARO_NOISE_LPF)); 
    BaroAlt = lrintf(cfilter(BaroAlt, BaroAlt_tmp, CONFIG_BARO_NOISE_LPF));

    // Calculate sonar altitude only if the sonar is facing downwards(<25deg)
    SonarAlt = (tiltAngle > 250) ? -1 : SonarAlt * (900.0f - tiltAngle) / 900.0f;

    // Fuse SonarAlt and BaroAlt
    if (sonarInRange()) {
        baroAlt_offset = BaroAlt - SonarAlt;
        BaroAlt = SonarAlt;
    } else {
        BaroAlt = BaroAlt - baroAlt_offset;
        if (SonarAlt > 0) {
            sonarTransition = (300 - SonarAlt) / 100.0f;
            //BaroAlt = SonarAlt * sonarTransition + BaroAlt * (1.0f - sonarTransition);
            BaroAlt = cfilter(SonarAlt, BaroAlt, sonarTransition); 
        }
    }

    // delta acc reading time in seconds
    float dt = accTimeSum * 1e-6f; 

    // Integrator - velocity, cm/sec
    float accZ_tmp = (float)accSum[2] / (float)accSumCount;
    float vel_acc = accZ_tmp * accVelScale * (float)accTimeSum;

    // integrate accelerometer velocity to get distance (x= a/2 * t^2)
    accelAlt += (vel_acc * 0.5f) * dt + accelVel * dt;                                         
    accelVel += vel_acc;

    // complementary filter for altitude estimation (baro & acc)
    //accelAlt = accelAlt * CONFIG_BARO_CF_ALT + (float)BaroAlt * (1.0f - CONFIG_BARO_CF_ALT);      
    accelAlt = cfilter(accelAlt, BaroAlt, CONFIG_BARO_CF_ALT);

    EstAlt = sonarInRange() ? BaroAlt : accelAlt;

    accSum_reset();

    int32_t baroVel = (BaroAlt - lastBaroAlt) * 1000000.0f / dTime;
    lastBaroAlt = BaroAlt;

    baroVel = constrain(baroVel, -1500, 1500);    // constrain baro velocity +/- 1500cm/s
    baroVel = applyDeadband(baroVel, 10);         // to reduce noise near zero

    // Apply complementary filter to keep the calculated velocity based on baro velocity (i.e. near real velocity).
    // By using CF it's possible to correct the drift of integrated accZ (velocity) without loosing the phase, 
    // i.e without delay
    //accelVel = accelVel * CONFIG_BARO_CF_VEL + baroVel * (1 - CONFIG_BARO_CF_VEL);
    accelVel = cfilter(accelVel, baroVel, CONFIG_BARO_CF_VEL);
    int32_t vel_tmp = lrintf(accelVel);

    // set vario
    vario = applyDeadband(vel_tmp, 5);

    int32_t setVel = setVelocity;

    if (tiltAngle < 800) { // only calculate pid if the copters thrust is facing downwards(<80deg)

        // Altitude P-Controller
        if (!velocityControl) {
            int32_t error = constrain(AltHold - EstAlt, -500, 500);
            error = applyDeadband(error, 10);       // remove small P parametr to reduce noise near zero position
            setVel = constrain((CONFIG_ALT_P * error / 128), -300, +300); // limit velocity to +/- 3 m/s
        } 

        // Velocity PID-Controller
        // P
        int32_t error = setVel - vel_tmp;
        AltPID = constrain((CONFIG_VEL_P * error / 32), -300, +300);

        // I
        errorVelocityI += (CONFIG_VEL_I * error);
        errorVelocityI = constrain(errorVelocityI, -(8196 * 200), (8196 * 200));
        AltPID += errorVelocityI / 8196;     // I in the range of +/-200

        // D
        AltPID -= constrain(CONFIG_VEL_D * (accZ_tmp + accZ_old) / 512, -150, 150);

    } else {
        AltPID = 0;
    }

    accZ_old = accZ_tmp;

    return 1;
}

