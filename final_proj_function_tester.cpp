#include <cstdint>
#include <cstdio>
#include <cmath>
#include <array>

// Test Helpers //////////////////////////////////////////////////

static int g_fail = 0;

// checks expected bool value
#define EXPECT_TRUE(cond) do { \
  if (!(cond)) { \
    std::printf("[FAIL] %s\n", #cond); \
    g_fail++; \
  } else { \
    std::printf("[PASS] %s\n", #cond); \
  } \
} while (0)

// checks expected int value
#define EXPECT_EQ_INT(a,b) do { \
  int a_val = (a), b_val = (b); \
  if (a_val != b_val) { \
    std::printf("[FAIL] %s != %s  (%d vs %d)\n", #a, #b, a_val, b_val); \
    g_fail++; \
  } else { \
    std::printf("[PASS] %s == %s  (%d)\n", #a, #b, a_val); \
  } \
} while (0)

// checks expected uint32_t value
#define EXPECT_EQ_U32(a,b) do { \
  uint32_t a_val = (a), b_val = (b); \
  if (a_val != b_val) { \
    std::printf("[FAIL] %s != %s  (0x%08X vs 0x%08X)\n", #a, #b, a_val, b_val); \
    g_fail++; \
  } else { \
    std::printf("[PASS] %s == %s  (0x%08X)\n", #a, #b, a_val); \
  } \
} while (0)

// checks expected float with tolerance = eps
#define EXPECT_NEAR(a,b,eps) do { \
  float a_val = (a), b_val = (b); \
  if (std::fabs(a_val - b_val) > (eps)) { \
    std::printf("[FAIL] |%s-%s| > %g  (%.6f vs %.6f)\n", #a, #b, (double)(eps), a_val, b_val); \
    g_fail++; \
  } else { \
    std::printf("[PASS] %s ~= %s  (%.6f)\n", #a, #b, a_val); \
  } \
} while (0)

#define bit_read(data, n) (((data) >> (n)) & 0x01)


// Mock structs matching the MMIO Cores and member functions //////////////////////////////

struct GpiCore {
  uint32_t sw = 0;
  void set(uint32_t v) { sw = v; }
  int read() { return (int)sw; }
  int read(int bit_pos) { return (int)bit_read(sw, bit_pos); }
};

struct GpoCore {
  uint32_t ledOutput = 0;
  void write(uint32_t v) { ledOutput = v; }
};

struct PwmCore {
  std::array<double, 8> duty{};
  int freq = 0;

  void set_freq(int f) { freq = f; }
  void set_duty(double d, int ch) {
    if (ch >= 0 && ch < (int)duty.size()) duty[ch] = d;
  }
};

struct SsegCore {
  std::array<uint8_t, 8> digit{};
  uint8_t dp = 0;

  uint8_t h2s(int x) { return (uint8_t)(x & 0xFF); }
  void write_1ptn(uint8_t ptn, int pos) {
    if (pos >= 0 && pos < (int)digit.size()) digit[pos] = ptn;
  }
  void set_dp(uint8_t pt) { dp = pt; }
};

// Project Functions //////////////////////////////////////////////////

// reads either SW0-6 or SW8-14 based on segSel input and returns SW value
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

// reads either SW7 or SW5 based on segSel input and returns SW value
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

// sets a RGB to red if color = 1, or green if color = 0. RGB sel determines which RGB is set.
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
// segSel determines if it is on the right (0) or left (1) side of the sevensegment
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

// Tests ////////////////////////////////////////////////////////////

// checks that the switches are read correctly and that the correct switches are mirrored on the LEDs
static void test_switch_decode_and_led_mirror() {
  std::puts("\n=== test_switch_decode_and_led_mirror ===");

  GpiCore sw;
  GpoCore led;

  uint32_t v;
  int extLim;
  int intLim;
  int extFmt;
  int intFmt;

  uint32_t extChoice;
  uint32_t intChoice;

  extChoice = 0x12; // SW0-6
  intChoice = 0x34; // SW8-14

  // Case 1: SW7=0, SW15=0
  v = 0;
  v = v | (extChoice & 0x7Fu);
  v = v | ((intChoice & 0x7Fu) << 8);
  v = v & ~(1u << 7);
  v = v & ~(1u << 15);
  sw.set(v);

  extLim = getTempLimit(&sw, 0);
  intLim = getTempLimit(&sw, 1);
  EXPECT_EQ_INT(extLim, (int)extChoice);
  EXPECT_EQ_INT(intLim, (int)intChoice);

  dispTempLimit(&led, extLim, intLim);
  EXPECT_EQ_U32(led.ledOutput, (uint32_t)((intChoice << 8) | extChoice));

  extFmt = getTempFormat(&sw, 0);
  intFmt = getTempFormat(&sw, 1);
  EXPECT_EQ_INT(extFmt, 0);
  EXPECT_EQ_INT(intFmt, 0);

  // Case 2: SW7=1, SW15=0
  v = 0;
  v = v | (extChoice & 0x7Fu);
  v = v | ((intChoice & 0x7Fu) << 8);
  v = v | (1u << 7);
  v = v & ~(1u << 15);
  sw.set(v);

  extLim = getTempLimit(&sw, 0);
  intLim = getTempLimit(&sw, 1);
  EXPECT_EQ_INT(extLim, (int)extChoice);
  EXPECT_EQ_INT(intLim, (int)intChoice);

  dispTempLimit(&led, extLim, intLim);
  EXPECT_EQ_U32(led.ledOutput, (uint32_t)((intChoice << 8) | extChoice));

  extFmt = getTempFormat(&sw, 0);
  intFmt = getTempFormat(&sw, 1);
  EXPECT_EQ_INT(extFmt, 1);
  EXPECT_EQ_INT(intFmt, 0);

  // Case 3: SW7=0, SW15=1
  v = 0;
  v = v | (extChoice & 0x7Fu);
  v = v | ((intChoice & 0x7Fu) << 8);
  v = v & ~(1u << 7);
  v = v | (1u << 15);
  sw.set(v);

  extLim = getTempLimit(&sw, 0);
  intLim = getTempLimit(&sw, 1);
  EXPECT_EQ_INT(extLim, (int)extChoice);
  EXPECT_EQ_INT(intLim, (int)intChoice);

  dispTempLimit(&led, extLim, intLim);
  EXPECT_EQ_U32(led.ledOutput, (uint32_t)((intChoice << 8) | extChoice));

  extFmt = getTempFormat(&sw, 0);
  intFmt = getTempFormat(&sw, 1);
  EXPECT_EQ_INT(extFmt, 0);
  EXPECT_EQ_INT(intFmt, 1);

  // Case 4: SW7=1, SW15=1
  v = 0;
  v = v | (extChoice & 0x7Fu);
  v = v | ((intChoice & 0x7Fu) << 8);
  v = v | (1u << 7);
  v = v | (1u << 15);
  sw.set(v);

  extLim = getTempLimit(&sw, 0);
  intLim = getTempLimit(&sw, 1);
  EXPECT_EQ_INT(extLim, (int)extChoice);
  EXPECT_EQ_INT(intLim, (int)intChoice);

  dispTempLimit(&led, extLim, intLim);
  EXPECT_EQ_U32(led.ledOutput, (uint32_t)((intChoice << 8) | extChoice));

  extFmt = getTempFormat(&sw, 0);
  intFmt = getTempFormat(&sw, 1);
  EXPECT_EQ_INT(extFmt, 1);
  EXPECT_EQ_INT(intFmt, 1);
}

// tests Celsius to Fahrenheit conversion
static void test_cel2fer() {
  std::puts("\n=== test cel2fer ===");
  EXPECT_NEAR(cel2fer(0.0f), 32.0f, 1e-5f);
  EXPECT_NEAR(cel2fer(100.0f), 212.0f, 1e-5f);
  EXPECT_NEAR(cel2fer(25.0f), 77.0f, 1e-5f);
}

// tests that the seven segment digits and DPs clear correctly
static void test_clearDisp() {
  std::puts("\n=== test clearDisp ===");
  SsegCore sseg;
  for (int i = 0; i < 8; i++) {
    sseg.digit[i] = 0x00;
  }
  sseg.dp = 0xFF;

  clearDisp(&sseg);
  for (int i = 0; i < 8; i++) {
    EXPECT_EQ_INT(sseg.digit[i], 0xFF);
  }
  EXPECT_EQ_INT(sseg.dp, 0x00);
}

// Tests the two RGB outputs
static void test_setRGB() {
  std::puts("\n=== test setRGB ===");
  PwmCore pwm;

  setRGB(&pwm, 0, 0);
  EXPECT_NEAR((float)pwm.duty[0], 0.0f, 1e-6f);
  EXPECT_NEAR((float)pwm.duty[1], 0.3f, 1e-6f);
  EXPECT_NEAR((float)pwm.duty[2], 0.0f, 1e-6f);

  setRGB(&pwm, 1, 1);
  EXPECT_NEAR((float)pwm.duty[3], 0.0f, 1e-6f);
  EXPECT_NEAR((float)pwm.duty[4], 0.0f, 1e-6f);
  EXPECT_NEAR((float)pwm.duty[5], 0.3f, 1e-6f);
}

// tests dispTemp() at various temp ranges for C and F
static void test_dispTemp() {
  std::puts("\n=== test_dispTemp_rounding_and_modes ===");

  SsegCore sseg;

  float tempC;
  float tempF;
  bool hund;

  // 1) tempC = 9.76, Celsius format
  tempC = 9.76f;
  tempF = cel2fer(tempC);

  // Interior (left)
  clearDisp(&sseg);
  hund = dispTemp(&sseg, tempC, tempF, 0, 1);
  EXPECT_TRUE(hund == false);
  EXPECT_EQ_INT(sseg.digit[7], 0xFF); 
  EXPECT_EQ_INT(sseg.digit[6], 9);    
  EXPECT_EQ_INT(sseg.digit[5], 8);    
  EXPECT_EQ_INT(sseg.digit[4], 0x0C); 

  // Exterior (right)
  clearDisp(&sseg);
  hund = dispTemp(&sseg, tempC, tempF, 0, 0);
  EXPECT_TRUE(hund == false);
  EXPECT_EQ_INT(sseg.digit[3], 0xFF);
  EXPECT_EQ_INT(sseg.digit[2], 9);
  EXPECT_EQ_INT(sseg.digit[1], 8);
  EXPECT_EQ_INT(sseg.digit[0], 0x0C);

  // 2) tempC = 9.76, Fahrenheit format
  tempC = 9.76f;
  tempF = cel2fer(tempC);

  // Interior (left)
  clearDisp(&sseg);
  hund = dispTemp(&sseg, tempC, tempF, 1, 1);
  EXPECT_TRUE(hund == false);
  EXPECT_EQ_INT(sseg.digit[7], 4);
  EXPECT_EQ_INT(sseg.digit[6], 9);
  EXPECT_EQ_INT(sseg.digit[5], 6);
  EXPECT_EQ_INT(sseg.digit[4], 0x0F);

  // Exterior (right)
  clearDisp(&sseg);
  hund = dispTemp(&sseg, tempC, tempF, 1, 0);
  EXPECT_TRUE(hund == false);
  EXPECT_EQ_INT(sseg.digit[3], 4);
  EXPECT_EQ_INT(sseg.digit[2], 9);
  EXPECT_EQ_INT(sseg.digit[1], 6);
  EXPECT_EQ_INT(sseg.digit[0], 0x0F);

  // 3) tempC = 37.75, Celsius format
  tempC = 37.75f;
  tempF = cel2fer(tempC);

  // Interior (left)
  clearDisp(&sseg);
  hund = dispTemp(&sseg, tempC, tempF, 0, 1);
  EXPECT_TRUE(hund == false);
  EXPECT_EQ_INT(sseg.digit[7], 3);
  EXPECT_EQ_INT(sseg.digit[6], 7);
  EXPECT_EQ_INT(sseg.digit[5], 8);
  EXPECT_EQ_INT(sseg.digit[4], 0x0C);

  // Exterior (right)
  clearDisp(&sseg);
  hund = dispTemp(&sseg, tempC, tempF, 0, 0);
  EXPECT_TRUE(hund == false);
  EXPECT_EQ_INT(sseg.digit[3], 3);
  EXPECT_EQ_INT(sseg.digit[2], 7);
  EXPECT_EQ_INT(sseg.digit[1], 8);
  EXPECT_EQ_INT(sseg.digit[0], 0x0C);

  // 4) tempC = 37.75, Fahrenheit format
  tempC = 37.75f;
  tempF = cel2fer(tempC);

  // Interior (left)
  clearDisp(&sseg);
  hund = dispTemp(&sseg, tempC, tempF, 1, 1);
  EXPECT_TRUE(hund == true);
  EXPECT_EQ_INT(sseg.digit[7], 1);
  EXPECT_EQ_INT(sseg.digit[6], 0);
  EXPECT_EQ_INT(sseg.digit[5], 0);
  EXPECT_EQ_INT(sseg.digit[4], 0x0F);

  // Exterior (right)
  clearDisp(&sseg);
  hund = dispTemp(&sseg, tempC, tempF, 1, 0);
  EXPECT_TRUE(hund == true);
  EXPECT_EQ_INT(sseg.digit[3], 1);
  EXPECT_EQ_INT(sseg.digit[2], 0);
  EXPECT_EQ_INT(sseg.digit[1], 0);
  EXPECT_EQ_INT(sseg.digit[0], 0x0F);

  // 5) tempC = 39.24, Celsius format
  tempC = 39.24f;
  tempF = cel2fer(tempC);

  // Interior (left)
  clearDisp(&sseg);
  hund = dispTemp(&sseg, tempC, tempF, 0, 1);
  EXPECT_TRUE(hund == false);
  EXPECT_EQ_INT(sseg.digit[7], 3);
  EXPECT_EQ_INT(sseg.digit[6], 9);
  EXPECT_EQ_INT(sseg.digit[5], 2);
  EXPECT_EQ_INT(sseg.digit[4], 0x0C);

  // Exterior (right)
  clearDisp(&sseg);
  hund = dispTemp(&sseg, tempC, tempF, 0, 0);
  EXPECT_TRUE(hund == false);
  EXPECT_EQ_INT(sseg.digit[3], 3);
  EXPECT_EQ_INT(sseg.digit[2], 9);
  EXPECT_EQ_INT(sseg.digit[1], 2);
  EXPECT_EQ_INT(sseg.digit[0], 0x0C);

  // 6) tempC = 39.24, Fahrenheit format
  tempC = 39.24f;
  tempF = cel2fer(tempC);

  // Interior (left)
  clearDisp(&sseg);
  hund = dispTemp(&sseg, tempC, tempF, 1, 1);
  EXPECT_TRUE(hund == true);
  EXPECT_EQ_INT(sseg.digit[7], 1);
  EXPECT_EQ_INT(sseg.digit[6], 0);
  EXPECT_EQ_INT(sseg.digit[5], 3);
  EXPECT_EQ_INT(sseg.digit[4], 0x0F);

  // Exterior (right)
  clearDisp(&sseg);
  hund = dispTemp(&sseg, tempC, tempF, 1, 0);
  EXPECT_TRUE(hund == true);
  EXPECT_EQ_INT(sseg.digit[3], 1);
  EXPECT_EQ_INT(sseg.digit[2], 0);
  EXPECT_EQ_INT(sseg.digit[1], 3);
  EXPECT_EQ_INT(sseg.digit[0], 0x0F);

  // 7) tempC = 105.49, Celsius format
  tempC = 105.49f;
  tempF = cel2fer(tempC);

  // Interior (left)
  clearDisp(&sseg);
  hund = dispTemp(&sseg, tempC, tempF, 0, 1);
  EXPECT_TRUE(hund == true);
  EXPECT_EQ_INT(sseg.digit[7], 1);
  EXPECT_EQ_INT(sseg.digit[6], 0);
  EXPECT_EQ_INT(sseg.digit[5], 5);
  EXPECT_EQ_INT(sseg.digit[4], 0x0C);

  // Exterior (right)
  clearDisp(&sseg);
  hund = dispTemp(&sseg, tempC, tempF, 0, 0);
  EXPECT_TRUE(hund == true);
  EXPECT_EQ_INT(sseg.digit[3], 1);
  EXPECT_EQ_INT(sseg.digit[2], 0);
  EXPECT_EQ_INT(sseg.digit[1], 5);
  EXPECT_EQ_INT(sseg.digit[0], 0x0C);

  // 8) tempC = 105.49, Fahrenheit format
  tempC = 105.49f;
  tempF = cel2fer(tempC);

  // Interior (left)
  clearDisp(&sseg);
  hund = dispTemp(&sseg, tempC, tempF, 1, 1);
  EXPECT_TRUE(hund == true);
  EXPECT_EQ_INT(sseg.digit[7], 2);
  EXPECT_EQ_INT(sseg.digit[6], 2);
  EXPECT_EQ_INT(sseg.digit[5], 2);
  EXPECT_EQ_INT(sseg.digit[4], 0x0F);

  // Exterior (right)
  clearDisp(&sseg);
  hund = dispTemp(&sseg, tempC, tempF, 1, 0);
  EXPECT_TRUE(hund == true);
  EXPECT_EQ_INT(sseg.digit[3], 2);
  EXPECT_EQ_INT(sseg.digit[2], 2);
  EXPECT_EQ_INT(sseg.digit[1], 2);
  EXPECT_EQ_INT(sseg.digit[0], 0x0F);

  // 9) tempC = -5.32, Celsius format (clamped to 0.0)
  tempC = -5.32f;
  tempF = cel2fer(tempC);

  // Interior (left)
  clearDisp(&sseg);
  hund = dispTemp(&sseg, tempC, tempF, 0, 1);
  EXPECT_TRUE(hund == false);
  EXPECT_EQ_INT(sseg.digit[7], 0xFF);
  EXPECT_EQ_INT(sseg.digit[6], 0);
  EXPECT_EQ_INT(sseg.digit[5], 0);
  EXPECT_EQ_INT(sseg.digit[4], 0x0C);

  // Exterior (right)
  clearDisp(&sseg);
  hund = dispTemp(&sseg, tempC, tempF, 0, 0);
  EXPECT_TRUE(hund == false);
  EXPECT_EQ_INT(sseg.digit[3], 0xFF);
  EXPECT_EQ_INT(sseg.digit[2], 0);
  EXPECT_EQ_INT(sseg.digit[1], 0);
  EXPECT_EQ_INT(sseg.digit[0], 0x0C);

  // 10) tempC = -5.32, Fahrenheit format
  tempC = -5.32f;
  tempF = cel2fer(tempC);

  // Interior (left)
  clearDisp(&sseg);
  hund = dispTemp(&sseg, tempC, tempF, 1, 1);
  EXPECT_TRUE(hund == false);
  EXPECT_EQ_INT(sseg.digit[7], 2);
  EXPECT_EQ_INT(sseg.digit[6], 2);
  EXPECT_EQ_INT(sseg.digit[5], 4);
  EXPECT_EQ_INT(sseg.digit[4], 0x0F);

  // Exterior (right)
  clearDisp(&sseg);
  hund = dispTemp(&sseg, tempC, tempF, 1, 0);
  EXPECT_TRUE(hund == false);
  EXPECT_EQ_INT(sseg.digit[3], 2);
  EXPECT_EQ_INT(sseg.digit[2], 2);
  EXPECT_EQ_INT(sseg.digit[1], 4);
  EXPECT_EQ_INT(sseg.digit[0], 0x0F);

  // 11) tempC = -20.82, Celsius format (clamped to 0.0)
  tempC = -20.82f;
  tempF = cel2fer(tempC);

  // Interior (left)
  clearDisp(&sseg);
  hund = dispTemp(&sseg, tempC, tempF, 0, 1);
  EXPECT_TRUE(hund == false);
  EXPECT_EQ_INT(sseg.digit[7], 0xFF);
  EXPECT_EQ_INT(sseg.digit[6], 0);
  EXPECT_EQ_INT(sseg.digit[5], 0);
  EXPECT_EQ_INT(sseg.digit[4], 0x0C);

  // Exterior (right)
  clearDisp(&sseg);
  hund = dispTemp(&sseg, tempC, tempF, 0, 0);
  EXPECT_TRUE(hund == false);
  EXPECT_EQ_INT(sseg.digit[3], 0xFF);
  EXPECT_EQ_INT(sseg.digit[2], 0);
  EXPECT_EQ_INT(sseg.digit[1], 0);
  EXPECT_EQ_INT(sseg.digit[0], 0x0C);

  // 12) tempC = -20.82, Fahrenheit format (clamped to 0.0)
  tempC = -20.82f;
  tempF = cel2fer(tempC);

  // Interior (left)
  clearDisp(&sseg);
  hund = dispTemp(&sseg, tempC, tempF, 1, 1);
  EXPECT_TRUE(hund == false);
  EXPECT_EQ_INT(sseg.digit[7], 0xFF);
  EXPECT_EQ_INT(sseg.digit[6], 0);
  EXPECT_EQ_INT(sseg.digit[5], 0);
  EXPECT_EQ_INT(sseg.digit[4], 0x0F);

  // Exterior (right)
  clearDisp(&sseg);
  hund = dispTemp(&sseg, tempC, tempF, 1, 0);
  EXPECT_TRUE(hund == false);
  EXPECT_EQ_INT(sseg.digit[3], 0xFF);
  EXPECT_EQ_INT(sseg.digit[2], 0);
  EXPECT_EQ_INT(sseg.digit[1], 0);
  EXPECT_EQ_INT(sseg.digit[0], 0x0F);
}

// tests the 4 different decimal point display configurations
static void test_dispDp() {
  std::puts("\n=== test dispDp ===");
  SsegCore sseg;

  dispDp(&sseg, false, false);
  EXPECT_EQ_INT(sseg.dp, (1 << 2) | (1 << 6));

  dispDp(&sseg, true, false);
  EXPECT_EQ_INT(sseg.dp, (1 << 2) | (1 << 5));

  dispDp(&sseg, false, true);
  EXPECT_EQ_INT(sseg.dp, (1 << 1) | (1 << 6));

  dispDp(&sseg, true, true);
  EXPECT_EQ_INT(sseg.dp, (1 << 1) | (1 << 5));
}

// Test Implementations
int main() {
  test_switch_decode_and_led_mirror();
  test_cel2fer();
  test_clearDisp();
  test_setRGB();
  test_dispTemp();
  test_dispDp();

  if (g_fail == 0) {
    std::puts("\nALL TESTS PASSED ");
    return 0;
  }
  std::printf("\nTESTS FAILED   count=%d\n", g_fail);
  return 1;
}
