// Segway controll code
// by Aaron Lebahn

/* ============================================
I2Cdev device library code is placed under the MIT license
Copyright (c) 2012 Jeff Rowberg

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
===============================================
*/

// Arduino Wire library is required if I2Cdev I2CDEV_ARDUINO_WIRE implementation
// is used in I2Cdev.h
#include "Wire.h"

// I2Cdev and MPU6050 must be installed as libraries, or else the .cpp/.h files
// for both classes must be in the include path of your project
#include "I2Cdev.h"

#include "MPU6050_6Axis_MotionApps20.h"
//#include "MPU6050.h" // not necessary if using MotionApps include file

MPU6050 mpu;

/* =========================================================================
   NOTE: In addition to connection 3.3v, GND, SDA, and SCL, this sketch
   depends on the MPU-6050's INT pin being connected to the Arduino's
   external interrupt #0 pin. On the Arduino Uno and Mega 2560, this is
   digital I/O pin 2.
 * ========================================================================= */

//set the pins for the motor controllers
#define LCW  4  //org
#define LCCW 6  //wht-org
#define LBRK 8  //wht-blu
#define LADJ 10 //blu
#define RCW  5  //org
#define RCCW 7  //wht-org
#define RBRK 9  //wht-blu
#define RADJ 11 //blu
//error signals (may not be used)
#define LERR 12 //wht-brn
#define RERR 13 //wht-brn
// COM/GND is brn

// MPU control/status vars
bool dmpReady = false;  // set true if DMP init was successful
uint8_t mpuIntStatus;   // holds actual interrupt status byte from MPU
uint8_t devStatus;      // return status after each device operation (0 = success, !0 = error)
uint16_t packetSize;    // expected DMP packet size (default is 42 bytes)
uint16_t fifoCount;     // count of all bytes currently in FIFO
uint8_t fifoBuffer[64]; // FIFO storage buffer

// orientation/motion vars
Quaternion q;           // [w, x, y, z]         quaternion container
VectorInt16 aa;         // [x, y, z]            accel sensor measurements
VectorInt16 aaReal;     // [x, y, z]            gravity-free accel sensor measurements
VectorInt16 aaWorld;    // [x, y, z]            world-frame accel sensor measurements
VectorInt16 gyro;       // [x, y, z]            gyroscopic output
VectorFloat gravity;    // [x, y, z]            gravity vector
float euler[3];         // [psi, theta, phi]    Euler angle container
float ypr[3];           // [yaw, pitch, roll]   yaw/pitch/roll container and gravity vector

// packet structure for InvenSense teapot demo
uint8_t teapotPacket[14] = { '$', 0x02, 0,0, 0,0, 0,0, 0,0, 0x00, 0x00, '\r', '\n' };

int sgn(int val) {
    return (0 < val) - (val < 0);
}

// ================================================================
// ===                  MATH HELPER FUNCTION                    ===
// ================================================================

float getLeverArm(Quaternion *rot)   //indicates how far the center of gravity is from the axel
{
    return 2*(rot->w*rot->x+rot->y*rot->z);
}

// ================================================================
// ===                  MOTOR CONTROL HELPER                    ===
// ================================================================

void setMotorSpeed(bool left, int speed)
{
    int cw, ccw, brk, adj;
    cw  = left?LCW:RCW;
    ccw = left?LCCW:RCCW;
    adj = left?LADJ:RADJ;

    analogWrite(adj,abs(speed));
    if (speed == 0)
    {
        digitalWrite(cw,HIGH);
        digitalWrite(ccw,HIGH);
    }
    else if (speed > 0)
    {
        digitalWrite(ccw,HIGH);
        delay(1);
        digitalWrite(cw,LOW);
    }
    else //speed <0
    {
        digitalWrite(cw,HIGH);
        delay(1);
        digitalWrite(ccw,LOW);
    }
}

// ================================================================
// ===               INTERRUPT DETECTION ROUTINE                ===
// ================================================================

volatile bool mpuInterrupt = false;     // indicates whether MPU interrupt pin has gone high
void dmpDataReady() {
    mpuInterrupt = true;
}



// ================================================================
// ===                      INITIAL SETUP                       ===
// ================================================================

void setup() {
    // join I2C bus (I2Cdev library doesn't do this automatically)
    Wire.begin();

    // initialize serial communication
    // (115200 chosen because it is required for Teapot Demo output, but it's
    // really up to you depending on your project)
    Serial.begin(115200);

    // initialize device
    Serial.println(F("Initializing I2C devices..."));
    mpu.initialize();

    // verify connection
    Serial.println(F("Testing device connections..."));
    Serial.println(mpu.testConnection() ? F("MPU6050 connection successful") : F("MPU6050 connection failed"));

    // wait for ready
    /*Serial.println(F("\nSend any character to begin DMP programming and demo: "));
    while (Serial.available() && Serial.read()); // empty buffer
    while (!Serial.available());                 // wait for data
    while (Serial.available() && Serial.read());*/ // empty buffer again

    // load and configure the DMP
    Serial.println(F("Initializing DMP..."));
    devStatus = mpu.dmpInitialize();
    
    // make sure it worked (returns 0 if so)
    if (devStatus == 0) {
        // turn on the DMP, now that it's ready
        Serial.println(F("Enabling DMP..."));
        mpu.setDMPEnabled(true);

        // enable Arduino interrupt detection
        Serial.println(F("Enabling interrupt detection (Arduino external interrupt 0)..."));
        attachInterrupt(0, dmpDataReady, RISING);
        mpuIntStatus = mpu.getIntStatus();

        // set our DMP Ready flag so the main loop() function knows it's okay to use it
        Serial.println(F("DMP ready! Waiting for first interrupt..."));
        dmpReady = true;

        // get expected DMP packet size for later comparison
        packetSize = mpu.dmpGetFIFOPacketSize();
    } else {
        // ERROR!
        // 1 = initial memory load failed
        // 2 = DMP configuration updates failed
        // (if it's going to break, usually the code will be 1)
        Serial.print(F("DMP Initialization failed (code "));
        Serial.print(devStatus);
        Serial.println(F(")"));
    }

    pinMode(LCW, OUTPUT);
    pinMode(LCCW, OUTPUT);
    pinMode(LBRK, OUTPUT);
    pinMode(LADJ, OUTPUT);
    pinMode(RCW, OUTPUT);
    pinMode(RCCW, OUTPUT);
    pinMode(RBRK, OUTPUT);
    pinMode(RADJ, OUTPUT);

    pinMode(LERR, INPUT);
    pinMode(RERR, INPUT);

    //set braking to on and initialize motor speed to 0.
    digitalWrite(LBRK,LOW);
    digitalWrite(RBRK,LOW);
    setMotorSpeed(false, 0);
    setMotorSpeed(true, 0);
}



// ================================================================
// ===                    MAIN PROGRAM LOOP                     ===
// ================================================================

void loop() {
    // if programming failed, don't try to do anything
    if (!dmpReady) return;

    // wait for MPU interrupt or extra packet(s) available
    while (!mpuInterrupt && fifoCount < packetSize) {
        // other program behavior stuff here
        // .
        // .
        // .
        // if you are really paranoid you can frequently test in between other
        // stuff to see if mpuInterrupt is true, and if so, "break;" from the
        // while() loop to immediately process the MPU data
        // .
        // .
        // .
    }

    // reset interrupt flag and get INT_STATUS byte
    mpuInterrupt = false;
    mpuIntStatus = mpu.getIntStatus();

    // get current FIFO count
    fifoCount = mpu.getFIFOCount();

    // check for overflow (this should never happen unless our code is too inefficient)
    if ((mpuIntStatus & 0x10) || fifoCount == 1024) {
        // reset so we can continue cleanly
        mpu.resetFIFO();
        Serial.println(F("FIFO overflow!"));

    // otherwise, check for DMP data ready interrupt (this should happen frequently)
    } else if (mpuIntStatus & 0x02) {
        // wait for correct available data length, should be a VERY short wait
        while (fifoCount < packetSize) fifoCount = mpu.getFIFOCount();

        // read a packet from FIFO
        mpu.getFIFOBytes(fifoBuffer, packetSize);
        
        // track FIFO count here in case there is > 1 packet available
        // (this lets us immediately read more without waiting for an interrupt)
        fifoCount -= packetSize;

        // display quaternion values in easy matrix form: w x y z
        mpu.dmpGetQuaternion(&q, fifoBuffer);
        mpu.dmpGetAccel(&aa, fifoBuffer);
        mpu.dmpGetGyro(&gyro, fifoBuffer);
        //mpu.dmpGetGravity(&gravity, &q);
        //mpu.dmpGetEuler(euler, &q);
        float leverArm = getLeverArm(&q);
        Serial.print("lever arm\t");
        Serial.print(leverArm);
        Serial.print("\tangvel\t");
        Serial.print(gyro.y);
        /*Serial.print("quat\t");
        Serial.print(q.w);
        Serial.print("\t");
        Serial.print(q.x);
        Serial.print("\t");
        Serial.print(q.y);
        Serial.print("\t");
        Serial.print(q.z);
        Serial.print("\tgyro\t");
        Serial.print(gyro.x);
        Serial.print("\t");
        Serial.print(gyro.y);
        Serial.print("\t");
        Serial.println(gyro.z);
        Serial.print("gravity\t");
        Serial.print(gravity.x);
        Serial.print("\t");
        Serial.print(gravity.y);
        Serial.print("\t");
        Serial.print(gravity.z);*/
        Serial.print("\taccell\t");
        Serial.print(aa.x);
        Serial.print("\t");
        Serial.print(aa.y);
        Serial.print("\t");
        Serial.println(aa.z);

        //setMotorSpeed(true,255*sgn(leverArm)*sqrt(abs(leverArm)));
        //setMotorSpeed(false,-255*sgn(leverArm)*sqrt(abs(leverArm)));
        setMotorSpeed(true,255*leverArm);
        setMotorSpeed(false,-255*leverArm);
    }
}
