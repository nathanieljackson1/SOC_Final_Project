
// #define _DEBUG
#include "chu_init.h"
#include "gpio_cores.h"
#include "xadc_core.h"
#include "sseg_core.h"
#include "i2c_core.h"

// reads either SW0-6 or SW8-14 based on segsSel input and returns SW value
// this is used at the temperature limit input
int getTempLimit(GpiCore *sw_p, int segsSel) {
   int s, limit;

   s = sw_p->read();
   if (segsSel == 1) {
      limit = (s >> 8) & 0x7f;
   } else {
      limit = s & 0x7f; 
   }
   return limit;
}

// Shifts the upper limit 8 bits to the left and combines with the lower limit and combines them.
// They are then output to the LEDs to mirror SW0-6 anf SW8-14
void dispTempLimit(GpoCore *led_p, int lowerLim, int upperLim) {
   int ledDisp = 0;

   ledDisp = ledDisp | (lowerLim & 0x7f);
   ledDisp = ledDisp | ((upperLim & 0x7f) << 8);
   led_p->write(ledDisp);
}

// reads either SW7 or SW5 based on segsSel input and returns SW value
// this is used at the temperature format select
int getTempFormat(GpiCore *sw_p, int segsSel) {
   int s;
   if (segsSel == 1) {
      s = sw_p->read(15);
   } else {
      s = sw_p->read(7);
   }
   return s;

}

// sets a RGB to red if color = 1, or green if color = 0. rgbPos determines which RGB is set.
// Used to display if a temperature surpassed the user selected limit
void setRGB(PwmCore *pwm_p, int color, int rgbPos) {
   double bright, duty;
   bright = 30.0; // 30% brightness
   duty = bright / 100.0;
   
   if (rgbPos == 1) {
      for (int n = 0; n < 3; n++) {
         pwm_p->set_duty(0.0, n + 3);
      }
      pwm_p->set_duty(duty, color + 4);
   } else {
      for (int n = 0; n < 3; n++) {
         pwm_p->set_duty(0.0, n);
      }
      pwm_p->set_duty(duty, color + 1);
   }
}

// Reads the Temperature from the XADC Cores, and outputs it as a float
// Used as the internal temperature
float getIntTempC(XadcCore *adc_p) {
   double reading;
   float tempC;

      // display on-chip sensor and 4 channels in console
      uart.disp("FPGA temp: ");
      reading = adc_p->read_fpga_temp();
      uart.disp(reading, 3);
      uart.disp("\n\r");
      tempC = (float) reading;
      return tempC;
}

// Reads the Temperature from the I2C Cores, and outputs it as a float
// Used as the external temperature
float getExtTempC(I2cCore *adt7420_p) {
   const uint8_t DEV_ADDR = 0x4b;
   uint8_t wbytes[2], bytes[2];
   //int ack;
   uint16_t tmp;
   float tmpC;

   wbytes[0] = 0x00;
   adt7420_p->write_transaction(DEV_ADDR, wbytes, 1, 1);
   adt7420_p->read_transaction(DEV_ADDR, bytes, 2, 0);

   // conversion
   tmp = (uint16_t) bytes[0];
   tmp = (tmp << 8) + (uint16_t) bytes[1];
   if (tmp & 0x8000) {
      tmp = tmp >> 3;
      tmpC = (float) ((int) tmp - 8192) / 16;
   } else {
      tmp = tmp >> 3;
      tmpC = (float) tmp / 16;
   }
   uart.disp("temperature (C): ");
   uart.disp(tmpC);
   uart.disp("\n\r");
   return tmpC;
}

// Converts Celsius float to Fahrenheit float
float cel2fer(float tmpC) {
   float tmpF;
   tmpF = (tmpC * (9.00f / 5.00f)) + 32.00f;
   return tmpF;
}

// Clears all digits and decimal points on the seven segment display
void clearDisp(SsegCore *sseg_p) {
   // clear digits and dp
   const uint8_t BLANK = 0xff;
   for (int i = 0; i < 8; i++) {
      sseg_p->write_1ptn(BLANK, i);
   }
   sseg_p->set_dp(0x00);
}

// Displays the appripriate temperature based on user input (C or F). dislpays first decimal if double digit temp.
// segsSel determines if it is on the right (0) or left (1) side of the sevensegment
// displays whole number if triple digit temp. Outputs bool flagging if the displayed temp is at least 100
bool dispTemp(SsegCore *sseg_p, float tmpC, float tmpF, int isFer, int segsSel) {
   const uint8_t BLANK = 0xff;
   int posAdj, tempInt, whole, hundreds, tens, ones, tenths;
   bool isCel;
   bool isHundred = false; 
   float temp;

   // segsSel = 0 -> right 4 digits, posAdj = 0
   // segsSel = 1 -> left 4 digits, posAdj = 4
   if (segsSel == 1) {
      posAdj = 4;
   } else {
      posAdj = 0;
   }

   if (isFer == 1) {
      isCel = false;
      temp = tmpF;
   } else {
      isCel = true;
      temp = tmpC;
   }

   if (temp < 0.00f) {
      temp = 0.00f;
   }

   tempInt = (int)(temp * 10.00f + 0.50f);
   if (temp >= 100.00f) {
      whole = (int)(temp + 0.5f);
   } else {
      whole = tempInt / 10;
   }
   hundreds = (whole / 100) % 10;
   tens = (whole / 10) % 10;
   ones = whole % 10;
   tenths = tempInt % 10;
   
   if (whole >= 100){
      isHundred = true;
   }

   if (isHundred) {
      sseg_p->write_1ptn(sseg_p->h2s(hundreds), 3+posAdj);
      sseg_p->write_1ptn(sseg_p->h2s(tens), 2+posAdj);
      sseg_p->write_1ptn(sseg_p->h2s(ones), 1+posAdj);
   } else {
      if (whole >= 10) {
         sseg_p->write_1ptn(sseg_p->h2s(tens), 3+posAdj);
      } else {
         sseg_p->write_1ptn(BLANK, 3+posAdj);
      }
      sseg_p->write_1ptn(sseg_p->h2s(ones), 2+posAdj);
      sseg_p->write_1ptn(sseg_p->h2s(tenths), 1+posAdj);
   }

   if (isCel) {
      sseg_p->write_1ptn(sseg_p->h2s(0x0C), 0+posAdj);   // hex C pattern
   } else {
      sseg_p->write_1ptn(sseg_p->h2s(0x0F), 0+posAdj);   // hex F pattern
   }
   
   return isHundred; 
}

// Properly places the decimal points on seven segment display based on the bool output from dispTemp()
void dispDp(SsegCore *sseg_p, bool intIsHundred, bool extIsHundred) {
    uint8_t dpPos;

    if (intIsHundred && extIsHundred) { // both temps >= 100
        dpPos = (1 << 1) | (1 << 5);
    }
    else if (intIsHundred && !extIsHundred) { // only internal temp >= 100
        dpPos = (1 << 2) | (1 << 5);
    }
    else if (!intIsHundred && extIsHundred) { // only external temp >= 100
        dpPos = (1 << 1) | (1 << 6);
    }
    else { // neither is >= 100
        dpPos = (1 << 2) | (1 << 6);
    }

    sseg_p->set_dp(dpPos);
}

GpoCore led(get_slot_addr(BRIDGE_BASE, S2_LED));
GpiCore sw(get_slot_addr(BRIDGE_BASE, S3_SW));
XadcCore adc(get_slot_addr(BRIDGE_BASE, S5_XDAC));
PwmCore pwm(get_slot_addr(BRIDGE_BASE, S6_PWM));
SsegCore sseg(get_slot_addr(BRIDGE_BASE, S8_SSEG));
I2cCore adt7420(get_slot_addr(BRIDGE_BASE, S10_I2C));


int main() {
   // internal temp is Left digits 4-7 and RGB, external temp is Right digits 0-3 and RGB
   // Left=1, Right=0
   int intLimit, extLimit, intIsFer, extIsFer, intColor, extColor;
   float intTempC, extTempC, intTempF, extTempF;
   bool intIsHundred, extIsHundred;

   pwm.set_freq(50);
   while (1) {
      
      // User Input
      intLimit = getTempLimit(&sw, 1);
      extLimit = getTempLimit(&sw, 0);
      dispTempLimit(&led, extLimit, intLimit);
      intIsFer = getTempFormat(&sw, 1);
      extIsFer = getTempFormat(&sw, 0);
      
      // Sensing
      intTempC = getIntTempC(&adc);
      extTempC = getExtTempC(&adt7420);
      intTempF = cel2fer(intTempC);
      extTempF = cel2fer(extTempC);
      
      // RGB Display
      if (intTempC > intLimit) {
         intColor = 1; // Red
      } else {
         intColor = 0; // Green 
      }
      if (extTempC > extLimit) {
         extColor = 1; // Red
      } else {
         extColor = 0; // Green
      }
      setRGB(&pwm, intColor, 1);
      setRGB(&pwm, extColor, 0);
      
      // Sseg Display
      clearDisp(&sseg);
      intIsHundred = dispTemp(&sseg, intTempC, intTempF, intIsFer, 1);
      extIsHundred = dispTemp(&sseg, extTempC, extTempF, extIsFer, 0);
      dispDp(&sseg, intIsHundred, extIsHundred);
      
      sleep_ms(200);
   } //while
} //main

