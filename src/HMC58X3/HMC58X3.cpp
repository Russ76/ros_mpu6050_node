/*
HMC58X3.cpp - Interface a Honeywell HMC58X3 or HMC5883L magnetometer to an Arduino via i2c
Copyright (C) 2011 Fabio Varesano <fvaresano@yahoo.it>

Based on:
http://www.arduino.cc/cgi-bin/yabb2/YaBB.pl?num=1274748346
 Modification/extension of the following by E.J.Muller
http://eclecti.cc/hardware/hmc5843-magnetometer-library-for-arduino
 Copyright (c) 2009 Nirav Patel,

The above were based on:
http://www.sparkfun.com/datasheets/Sensors/Magneto/HMC58X3-v11.c
http://www.atmel.com/dyn/resources/prod_documents/doc2545.pdf


This program is free software: you can redistribute it and/or modify
it under the terms of the version 3 GNU General Public License as
published by the Free Software Foundation.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <http://www.gnu.org/licenses/>.

*/

//#define DEBUG (1)
#include <math.h>
#include <stdint.h>
#include <time.h>
#include <sys/time.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <fcntl.h>
#include <ros/ros.h>

#include "HMC58X3.h"
#include "../i2ckernel.h"

#define DEBUG_PRINT(x) ROS_INFO("%s\n", (x));


#define min(a,b) \
    ({ __typeof__ (a) _a = (a); \
    __typeof__ (b) _b = (b); \
    _a < _b ? _a : _b; })


int delayMSec(unsigned long num_ms)
{
    struct timespec ts;

    ts.tv_sec = num_ms / 1000;
    ts.tv_nsec = (num_ms % 1000) * 1000000;

    return nanosleep(&ts, NULL);
}


/*!
    Counts/milli-gauss per gain for the self test bias current.
*/
#if defined(ISHMC5843)
const int counts_per_milligauss[8]={
    1620,
    1300,
    970,
    780,
    530,
    460,
    390,
    280
};
#else // HMC5883L
const int counts_per_milligauss[8]={
    1370,
    1090,
    820,
    660,
    440,
    390,
    330,
    230
};
#endif

/* PUBLIC METHODS */

HMC58X3::HMC58X3(cereal::I2Ckernel &_i2c_ros_client):i2c_client(_i2c_ros_client) {

    x_scale=1.0F;
    y_scale=1.0F;
    z_scale=1.0F;
}


void HMC58X3::init(bool setmode) {
    // note that we don't initialize Wire here.
    // You'll have to do that in setup() in your Arduino program
    delayMSec(5); // you need to wait at least 5ms after power on to initialize
    if (setmode) {
        setMode(0);
    }

    writeReg(HMC58X3_R_CONFA, 0x70); // 8 samples averaged, 75Hz frequency, no artificial bias.
    writeReg(HMC58X3_R_CONFB, 0xA0);
    writeReg(HMC58X3_R_MODE, 0x00);
}


void HMC58X3::setMode(unsigned char mode) {
    if (mode > 2) {
        return;
    }

    writeReg(HMC58X3_R_MODE, mode);
    delayMSec(100);
}

/*
    Calibrate which has a few weaknesses.
    1. Uses wrong gain for first reading.
    2. Uses max instead of max of average when normalizing the axis to one another.
    3. Doesn't use neg bias. (possible improvement in measurement).
*/
void HMC58X3::calibrate(unsigned char gain) {
    x_scale=1; // get actual values
    y_scale=1;
    z_scale=1;
    writeReg(HMC58X3_R_CONFA, 0x010 + HMC_POS_BIAS); // Reg A DOR=0x010 + MS1,MS0 set to pos bias
    setGain(gain);
    float x, y, z, mx=0, my=0, mz=0, t=10;

    for (int i=0; i<(int)t; i++) {
        setMode(1);
        getValues(&x,&y,&z);
        if (x>mx) mx=x;
        if (y>my) my=y;
        if (z>mz) mz=z;
    }

    float max=0;
    if (mx>max) max=mx;
    if (my>max) max=my;
    if (mz>max) max=mz;
    x_max=mx;
    y_max=my;
    z_max=mz;
    x_scale=max/mx; // calc scales
    y_scale=max/my;
    z_scale=max/mz;

    writeReg(HMC58X3_R_CONFA, 0x010); // set RegA/DOR back to default
}   // calibrate().

/*!
    \brief Calibrate using the self test operation.

    Average the values using bias mode to obtain the scale factors.

    \param gain [in] Gain setting for the sensor. See data sheet.
    \param n_samples [in] Number of samples to average together while applying the positive and negative bias.
    \return Returns false if any of the following occurs:
        # Invalid input parameters. (gain>7 or n_samples=0).
        # Id registers are wrong for the compiled device. Unfortunately, we can't distinguish between HMC5843 and HMC5883L.
        # Calibration saturates during the positive or negative bias on any of the readings.
        # Readings are outside of the expected range for bias current.
*/
bool HMC58X3::calibrate(unsigned char gain, unsigned int n_samples)
{
    int16_t xyz[3];                     // 16 bit integer values for each axis.
    int32_t xyz_total[3]={0,0,0};  // 32 bit totals so they won't overflow.
    bool bret=true;                 // Function return value.  Will return false if the wrong identifier is returned, saturation is detected or response is out of range to self test bias.
    char id[3];                     // Three identification registers should return 'H43'.
    int32_t low_limit, high_limit;
    /*
        Make sure we are talking to the correct device.
        Hard to believe Honeywell didn't change the identifier.
    */
    if ((8>gain) && (0<n_samples)) // Notice this allows gain setting of 7 which the data sheet warns against.
    {
        getID(id);

        if (('H' == id[0]) && ('4' == id[1]) && ('3' == id[2]))
        {   /*
                Use the positive bias current to impose a known field on each axis.
                This field depends on the device and the axis.
            */
            writeReg(HMC58X3_R_CONFA, 0x010 + HMC_POS_BIAS); // Reg A DOR=0x010 + MS1,MS0 set to pos bias
            /*
                Note that the  very first measurement after a gain change maintains the same gain as the previous setting.
                The new gain setting is effective from the second measurement and on.
            */
            setGain(gain);
            setMode(1);                         // Change to single measurement mode.
            getRaw(&xyz[0],&xyz[1],&xyz[2]);    // Get the raw values and ignore since this reading may use previous gain.

            for (unsigned int i=0; i<n_samples; i++)
            {
                setMode(1);
                getRaw(&xyz[0],&xyz[1],&xyz[2]);   // Get the raw values in case the scales have already been changed.
                ROS_INFO("%d %d %d \n", xyz[0], xyz[1], xyz[2]);
                /*
                    Since the measurements are noisy, they should be averaged rather than taking the max.
                */
                xyz_total[0]+=xyz[0];
                xyz_total[1]+=xyz[1];
                xyz_total[2]+=xyz[2];
                /*
                    Detect saturation.
                */
                if (-(1<<12) >= min(xyz[0],min(xyz[1],xyz[2])))
                {
                    DEBUG_PRINT("HMC58x3 Self test saturated. Increase range.");
                    bret=false;
                    break;  // Breaks out of the for loop.  No sense in continuing if we saturated.
                }
            }
            /*
                Apply the negative bias. (Same gain)
            */
            ROS_INFO("%d %d %d \n", xyz_total[0], xyz_total[1], xyz_total[2]);
            writeReg(HMC58X3_R_CONFA, 0x010 + HMC_NEG_BIAS); // Reg A DOR=0x010 + MS1,MS0 set to negative bias.
            for (unsigned int i=0; i<n_samples; i++)
            {
                setMode(1);
                getRaw(&xyz[0],&xyz[1],&xyz[2]);   // Get the raw values in case the scales have already been changed.
                ROS_INFO("%d %d %d \n", xyz[0], xyz[1], xyz[2]);
                /*
                    Since the measurements are noisy, they should be averaged.
                */
                xyz_total[0]-=xyz[0];
                xyz_total[1]-=xyz[1];
                xyz_total[2]-=xyz[2];
                /*
                    Detect saturation.
                */
                if (-(1<<12) >= min(xyz[0],min(xyz[1],xyz[2])))
                {
                    DEBUG_PRINT("HMC58x3 Self test saturated. Increase range.");
                    bret=false;
                    break;  // Breaks out of the for loop.  No sense in continuing if we saturated.
                }
            }
            /*
                Compare the values against the expected self test bias gauss.
                Notice, the same limits are applied to all axis.
            */
            ROS_INFO("%d %d %d \n", xyz_total[0], xyz_total[1], xyz_total[2]);

            low_limit =SELF_TEST_LOW_LIMIT *counts_per_milligauss[gain]*2*n_samples;
            high_limit=SELF_TEST_HIGH_LIMIT*counts_per_milligauss[gain]*2*n_samples;
            ROS_INFO("lowl=%d highl=%d\n", low_limit, high_limit);
            if ((true==bret) &&
                    (low_limit <= xyz_total[0]) && (high_limit >= xyz_total[0]) &&
                    (low_limit <= xyz_total[1]) && (high_limit >= xyz_total[1]) &&
                    (low_limit <= xyz_total[2]) && (high_limit >= xyz_total[2]) )
            {   /*
                    Successful calibration.
                    Normalize the scale factors so all axis return the same range of values for the bias field.
                    Factor of 2 is from summation of total of n_samples from both positive and negative bias.
                */
                x_scale=(counts_per_milligauss[gain]*(HMC58X3_X_SELF_TEST_GAUSS*2))/(xyz_total[0]/n_samples);
                y_scale=(counts_per_milligauss[gain]*(HMC58X3_Y_SELF_TEST_GAUSS*2))/(xyz_total[1]/n_samples);
                z_scale=(counts_per_milligauss[gain]*(HMC58X3_Z_SELF_TEST_GAUSS*2))/(xyz_total[2]/n_samples);
            }else
            {
                DEBUG_PRINT("HMC58x3 Self test out of range.");
                bret=false;
            }
            writeReg(HMC58X3_R_CONFA, 0x010); // set RegA/DOR back to default.
        }else
        {
#if defined(ISHMC5843)
            DEBUG_PRINT("HMC5843 failed id check.");
#else
            DEBUG_PRINT("HMC5883L failed id check.");
#endif
            bret=false;
        }
    }else
    {   /*
            Bad input parameters.
        */
        DEBUG_PRINT("HMC58x3 Bad parameters.");
        bret=false;
    }
    return(bret);
}   //  calibrate().

// set data output rate
// 0-6, 4 default, normal operation assumed
void HMC58X3::setDOR(unsigned char DOR) {
    if (DOR>6) return;
    writeReg(HMC58X3_R_CONFA,DOR<<2);
}


void HMC58X3::setGain(unsigned char gain) { 
    // 0-7, 1 default
    if (gain > 7) return;
    writeReg(HMC58X3_R_CONFB, gain << 5);
}


void HMC58X3::writeReg(unsigned char reg, unsigned char val)
{
    /*unsigned char tx[2];

  tx[0] = reg;
  tx[1] = val;
  i2cWrite(HMC58X3_ADDR, tx, 2);*/
    i2c_client._write(HMC58X3_ADDR,reg,&val,1);
}


void HMC58X3::getValues(int16_t *x, int16_t *y, int16_t *z)
{
    float fx,fy,fz;
    getValues(&fx,&fy,&fz);
    *x= (int16_t) (fx + 0.5);
    *y= (int16_t) (fy + 0.5);
    *z= (int16_t) (fz + 0.5);
}


void HMC58X3::getValues(float *x,float *y,float *z)
{
    int16_t xr,yr,zr;

    getRaw(&xr, &yr, &zr);
    *x= ((float) xr) / x_scale;
    *y = ((float) yr) / y_scale;
    *z = ((float) zr) / z_scale;
}


void HMC58X3::getRaw(int16_t *x, int16_t *y, int16_t *z)
{
    unsigned char tx[2];
    unsigned char rx[6];

    /*tx[0] = HMC58X3_R_XM;
  i2cWrite(HMC58X3_ADDR, tx, 1);

  i2cRead(HMC58X3_ADDR, rx, 6);*/

    i2c_client._read(HMC58X3_ADDR,HMC58X3_R_XM,rx,6);

    *x = (rx[0] << 8) | rx[1];

#ifdef ISHMC5843
    *y = (rx[2] << 8) | rx[3];
    *z = (rx[4] << 8) | rx[5];
#else // the Z registers comes before the Y registers in the HMC5883L
    *z = (rx[2] << 8) | rx[3];
    *y = (rx[4] << 8) | rx[5];
#endif
}


void HMC58X3::getValues(float *xyz) {
    getValues(&xyz[0], &xyz[1], &xyz[2]);
}

/*! 
    \brief Retrieve the value of the three ID registers.

    Note:  Both the HMC5843 and HMC5883L have the same 'H43' identification register values. (Looks like someone at Honeywell screwed up.)

    \param id [out] Returns the three id register values.
*/
void HMC58X3::getID(char id[3]) 
{
    unsigned char tx[2];

    /*tx[0] = HMC58X3_R_IDA;
    i2cWrite(HMC58X3_ADDR, tx, 1);

    if (i2cRead(HMC58X3_ADDR, (unsigned char*)id, 3))*/
    if (!i2c_client._read(HMC58X3_ADDR,HMC58X3_R_IDA,(unsigned char*)id, 3))
        id[0] = id[1] = id[2] = 0;
    ROS_INFO("id %d %d %d\n", id[0], id[1], id[2]);
}   // getID().
