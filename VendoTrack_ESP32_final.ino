// ╔══════════════════════════════════════════════════════════════════╗
// ║            VendoTrack ESP32 — Vendo Machine Firmware            ║
// ║  • Reads prices from Firebase (set on website)                  ║
// ║  • Coin acceptor → LCD shows running total                      ║
// ║  • Button press → dispenses product via stepper (calibrated)    ║
// ║  • Signals website via Firebase — website uses JS timestamp     ║
// ║  • No RTC needed: all timestamps come from browser software     ║
// ╚══════════════════════════════════════════════════════════════════╝

// ── Required Libraries (install via Arduino Library Manager) ──
//  · ArduinoJson         (by Benoit Blanchon)
//  · LiquidCrystal I2C  (by Frank de Brabander)
//  · ESP32Servo          (by Kevin Harrington)
//  · WiFiManager         (by tzapu)

#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <ESP32Servo.h>
#include <WiFiManager.h>

// ═══════════════════════════════════════════════════════════
//  ★  CONFIGURE THESE BEFORE UPLOADING  ★
// ═══════════════════════════════════════════════════════════
const char* WIFI_SSID     = "speedxfiber_2.4Ghz";
const char* WIFI_PASSWORD = "143143143";

// Firebase Realtime Database URL (no trailing slash)
const char* FIREBASE_DB   = "https://smart-vend-63d7a-default-rtdb.asia-southeast1.firebasedatabase.app";
// Firebase Console → Project Settings → Service Accounts → Database Secrets
const char* FIREBASE_AUTH = "Ug5D6c4i7TQvKM4e7Dsg2bA61640rRW35nwRbBBH";

// Allan coin acceptor: 1 pulse per coin = P1
const float PESOS_PER_PULSE = 1.0;
// ═══════════════════════════════════════════════════════════

// ── PIN DEFINITIONS ──────────────────────────────────────
#define M1_PUL  23
#define M1_DIR  22
#define M1_ENA  21

#define M2_PUL  19
#define M2_DIR  18
#define M2_ENA  5

#define M3_PUL  17
#define M3_DIR  16
#define M3_ENA  4

#define BTN1    32    // INPUT_PULLUP — press = LOW
#define BTN2    33    // INPUT_PULLUP — press = LOW
#define BTN3    34    // input-only — needs external 10k pull-down, press = HIGH
#define BTN4    35    // input-only — needs external 10k pull-down, press = HIGH

#define SDA_PIN 25
#define SCL_PIN 26

#define COIN_PIN  13  // Allan COUNTER pin → GPIO 13
#define LIMIT_PIN 14  // Reserved

#define SERVO_PIN 2

// ── STEPPER CALIBRATION ───────────────────────────────────
// Your original calibration — do not change
#define TRAVEL_SPEED       500   // microseconds per step
#define STEPS_PER_DISPENSE 400   // 400 steps CW = 90° = 1 item
#define CW  HIGH
#define CCW LOW

// ── PRODUCT TABLE ─────────────────────────────────────────
struct Product {
  const char* name;
  const char* fbKey;
  float       price;
  int         pulPin;
  int         dirPin;
  int         enaPin;
  int         stock;
};

Product products[] = {
  { "Ballpen 1",  "ballpen1",  5.00,  M1_PUL, M1_DIR, M1_ENA, 50  },
  { "Ballpen 2",  "ballpen2",  5.00,  M2_PUL, M2_DIR, M2_ENA, 50  },
  { "Ballpen 3",  "ballpen3",  5.00,  M3_PUL, M3_DIR, M3_ENA, 50  }
};
const int NUM_PRODUCTS = 3;

// ── LCD ───────────────────────────────────────────────────
LiquidCrystal_I2C lcd(0x27, 16, 2); // try 0x3F if blank

// ── SERVO ─────────────────────────────────────────────────
Servo servo;

// ── STATE MACHINE ─────────────────────────────────────────
enum VendoState { IDLE, INSERTING, SELECTED, DISPENSING, COMPLETE, WIFI_SETUP };
VendoState vendoState = IDLE;

int   selectedProduct = -1;
float insertedAmount  = 0.0;
float changeAmount    = 0.0;

// ── COIN ACCEPTOR ─────────────────────────────────────────
volatile int           coinPulses   = 0;
volatile unsigned long lastCoinTime = 0;
const unsigned long    COIN_SETTLE_MS = 150;

// ── BUTTON DEBOUNCE ───────────────────────────────────────
unsigned long btnTime[4] = {0, 0, 0, 0};
const unsigned long BTN_DEBOUNCE_MS = 220;

// ── FIREBASE ──────────────────────────────────────────────
unsigned long lastPricePoll = 0;
const unsigned long PRICE_POLL_MS = 30000;

// ── LCD REFRESH ───────────────────────────────────────────
unsigned long lastLCDRefresh = 0;
const unsigned long LCD_REFRESH_MS = 600;

// ── WIFI ──────────────────────────────────────────────────
bool wifiConnected = false;

// ── FUNCTION DECLARATIONS ─────────────────────────────────
void IRAM_ATTR onCoinPulse();
void processCoins();
void handleButtons();
bool btnPressed(int pin, int idx);
void selectProduct(int idx);
void tryDispense(int idx);
void cancelTransaction();
void dispenseProduct(int idx);
void stepMotor(int pulPin, int dirPin, long steps, bool dir, int spd);
void enableMotor(int enaPin);
void disableMotor(int enaPin);
void updateLCD();
void lcdLine(const char* top, const char* bot);
bool fetchPricesFromFirebase();
bool sendDispenseSignal(int idx, float price, float inserted);
bool resetDispenseSignal();
String firebaseGET(const String& path);
bool firebasePUT(const String& path, const String& body);
void connectWiFi();

// ═══════════════════════════════════════════════════════════
//  SETUP
// ═══════════════════════════════════════════════════════════
void setup() {
  Serial.begin(115200);
  Serial.println(F("\n[VendoTrack] Booting v1.0"));

  // LCD
  Wire.begin(SDA_PIN, SCL_PIN);
  lcd.init();
  lcd.backlight();
  lcdLine("VendoTrack v1.0", "Starting up...");
  delay(800);

  // Motors — all disabled at boot
  int mpins[][3] = {
    {M1_PUL, M1_DIR, M1_ENA},
    {M2_PUL, M2_DIR, M2_ENA},
    {M3_PUL, M3_DIR, M3_ENA}
  };
  for (int i = 0; i < 3; i++) {
    pinMode(mpins[i][0], OUTPUT);
    pinMode(mpins[i][1], OUTPUT);
    pinMode(mpins[i][2], OUTPUT);
    digitalWrite(mpins[i][2], HIGH); // ENA HIGH = disabled
  }

  // Buttons
  pinMode(BTN1, INPUT_PULLUP);
  pinMode(BTN2, INPUT_PULLUP);
  pinMode(BTN3, INPUT);
  pinMode(BTN4, INPUT);

  // Coin acceptor — COUNTER pin is open-collector
  // Idles HIGH, pulls LOW per coin → FALLING edge
  // INPUT_PULLUP keeps signal stable at idle
  pinMode(COIN_PIN, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(COIN_PIN), onCoinPulse, FALLING);

  // Limit switch
  pinMode(LIMIT_PIN, INPUT_PULLUP);

  // Servo home
  servo.attach(SERVO_PIN);
  servo.write(0);
  delay(300);

  // WiFi + Firebase
  connectWiFi();
  if (wifiConnected) {
    lcdLine("Fetching prices", "from Firebase...");
    if (fetchPricesFromFirebase()) {
      lcdLine("Prices loaded!", "Ready to vend!");
    } else {
      lcdLine("Using defaults", "No Firebase yet");
    }
    delay(1200);
    resetDispenseSignal();
  }

  vendoState = IDLE;
  updateLCD();
  Serial.println(F("[VendoTrack] Ready!"));
}

// ═══════════════════════════════════════════════════════════
//  MAIN LOOP
// ═══════════════════════════════════════════════════════════
void loop() {
  processCoins();
  handleButtons();

  if (wifiConnected && millis() - lastPricePoll > PRICE_POLL_MS) {
    fetchPricesFromFirebase();
    lastPricePoll = millis();
  }

  if (millis() - lastLCDRefresh > LCD_REFRESH_MS) {
    updateLCD();
    lastLCDRefresh = millis();
  }

  if (!wifiConnected && millis() % 30000 < 100) {
    connectWiFi();
  }
}

// ═══════════════════════════════════════════════════════════
//  COIN ACCEPTOR
// ═══════════════════════════════════════════════════════════
void IRAM_ATTR onCoinPulse() {
  coinPulses++;
  lastCoinTime = millis(); // updated in ISR so settle window is accurate
}

void processCoins() {
  if (coinPulses == 0) return;

  // Wait for pulse burst to finish before counting
  if (millis() - lastCoinTime < COIN_SETTLE_MS) return;

  noInterrupts();
  int pulses = coinPulses;
  coinPulses = 0;
  interrupts();

  float coinValue = pulses * PESOS_PER_PULSE;
  insertedAmount += coinValue;

  if (vendoState == IDLE) vendoState = INSERTING;

  Serial.printf("[Coin] +P%.2f | Total: P%.2f\n", coinValue, insertedAmount);

  // Auto-dispense if product already selected and now have enough
  if (vendoState == SELECTED && selectedProduct >= 0) {
    if (insertedAmount >= products[selectedProduct].price) {
      tryDispense(selectedProduct);
      return;
    }
  }

  updateLCD();
}

// ═══════════════════════════════════════════════════════════
//  BUTTONS
// ═══════════════════════════════════════════════════════════
bool btnPressed(int pin, int idx) {
  bool active = (pin == BTN3 || pin == BTN4)
                ? (digitalRead(pin) == HIGH)
                : (digitalRead(pin) == LOW);

  if (active && millis() - btnTime[idx] > BTN_DEBOUNCE_MS) {
    btnTime[idx] = millis();
    return true;
  }
  return false;
}

void handleButtons() {
  if (vendoState == DISPENSING || vendoState == COMPLETE) return;

  if (btnPressed(BTN1, 0)) { Serial.println(F("[BTN1]")); selectProduct(0); }
  if (btnPressed(BTN2, 1)) { Serial.println(F("[BTN2]")); selectProduct(1); }
  if (btnPressed(BTN3, 2)) { Serial.println(F("[BTN3]")); selectProduct(2); }
  if (btnPressed(BTN4, 3)) { Serial.println(F("[BTN4] Cancel")); cancelTransaction(); }
}

// ═══════════════════════════════════════════════════════════
//  PRODUCT SELECTION & DISPENSING
// ═══════════════════════════════════════════════════════════
void selectProduct(int idx) {
  if (idx >= NUM_PRODUCTS) return;

  if (insertedAmount <= 0) {
    lcdLine("Insert coin", "first! B4=Cancel");
    delay(1500);
    updateLCD();
    return;
  }

  selectedProduct = idx;
  vendoState = SELECTED;

  Serial.printf("[Select] %s P%.2f | Inserted: P%.2f\n",
                products[idx].name, products[idx].price, insertedAmount);

  if (insertedAmount >= products[idx].price) {
    tryDispense(idx);
  } else {
    updateLCD();
  }
}

void tryDispense(int idx) {
  if (insertedAmount < products[idx].price) {
    updateLCD();
    return;
  }

  float price  = products[idx].price;
  vendoState   = DISPENSING;
  changeAmount = insertedAmount - price;

  Serial.printf("[Dispense] %s | Price:P%.2f | Change:P%.2f\n",
                products[idx].name, price, changeAmount);

  lcdLine("Dispensing...", "Please wait...  ");
  delay(300);

  dispenseProduct(idx);

  if (wifiConnected) {
    bool ok = sendDispenseSignal(idx, price, insertedAmount);
    Serial.printf("[Firebase] Signal: %s\n", ok ? "OK" : "FAILED");
  }

  char line1[17], line2[17];
  snprintf(line1, 17, "Thank you!      ");
  if (changeAmount > 0.005f) {
    snprintf(line2, 17, "Change: P%.2f   ", changeAmount);
  } else {
    snprintf(line2, 17, "Enjoy!          ");
  }
  lcdLine(line1, line2);

  delay(3500);

  insertedAmount  = 0;
  selectedProduct = -1;
  changeAmount    = 0;
  vendoState      = IDLE;
  updateLCD();
}

void cancelTransaction() {
  if (vendoState == DISPENSING) return;

  if (insertedAmount > 0.005f) {
    char line2[17];
    snprintf(line2, 17, "Return P%.2f    ", insertedAmount);
    lcdLine("Cancelled!      ", line2);
    Serial.printf("[Cancel] Returning P%.2f\n", insertedAmount);
    delay(2000);
  }

  insertedAmount  = 0;
  selectedProduct = -1;
  changeAmount    = 0;
  vendoState      = IDLE;
  updateLCD();
}

// ═══════════════════════════════════════════════════════════
//  STEPPER — 400 steps CW = 1 item (your original calibration)
// ═══════════════════════════════════════════════════════════
void stepMotor(int pulPin, int dirPin, long steps, bool dir, int spd) {
  digitalWrite(dirPin, dir);
  delayMicroseconds(5);
  for (long i = 0; i < steps; i++) {
    digitalWrite(pulPin, HIGH);
    delayMicroseconds(spd);
    digitalWrite(pulPin, LOW);
    delayMicroseconds(spd);
  }
}

void enableMotor(int enaPin)  { digitalWrite(enaPin, LOW);  delayMicroseconds(200); }
void disableMotor(int enaPin) { digitalWrite(enaPin, HIGH); }

void dispenseProduct(int idx) {
  enableMotor(products[idx].enaPin);
  delay(50);

  Serial.printf("[Motor%d] 400 steps CW -> %s\n", idx + 1, products[idx].name);
  stepMotor(products[idx].pulPin, products[idx].dirPin,
            STEPS_PER_DISPENSE, CW, TRAVEL_SPEED);

  delay(200);
  disableMotor(products[idx].enaPin);

  if (products[idx].stock > 0) products[idx].stock--;
  Serial.printf("[Stock] %s remaining: %d\n", products[idx].name, products[idx].stock);
}

// ═══════════════════════════════════════════════════════════
//  LCD
// ═══════════════════════════════════════════════════════════
void lcdLine(const char* top, const char* bot) {
  lcd.clear();
  lcd.setCursor(0, 0); lcd.print(top);
  lcd.setCursor(0, 1); lcd.print(bot);
}

void updateLCD() {
  char L1[17], L2[17];

  switch (vendoState) {
    case IDLE:
      lcdLine("Insert Coin...  ", "VendoTrack Ready");
      break;

    case INSERTING:
      snprintf(L1, 17, "Inserted: P%-5.2f", insertedAmount);
      snprintf(L2, 17, "B1:P%-4.0f B2:P%-4.0f",
               products[0].price, products[1].price);
      lcdLine(L1, L2);
      break;

    case SELECTED:
      if (selectedProduct >= 0) {
        float price  = products[selectedProduct].price;
        float needed = price - insertedAmount;
        snprintf(L1, 17, "%-10s P%.2f", products[selectedProduct].name, price);
        if (needed > 0.005f) {
          snprintf(L2, 17, "Need P%.2f more ", needed);
        } else {
          snprintf(L2, 17, "OK! Dispensing..");
        }
        lcdLine(L1, L2);
      }
      break;

    case DISPENSING:
      lcdLine("Dispensing...   ", "Please wait...  ");
      break;

    case COMPLETE:
      lcdLine("Thank you!      ", "Come again!     ");
      break;

    case WIFI_SETUP:
      lcdLine("Connecting WiFi ", WIFI_SSID);
      break;
  }
}

// ═══════════════════════════════════════════════════════════
//  FIREBASE REST API
// ═══════════════════════════════════════════════════════════
String firebaseGET(const String& path) {
  if (!wifiConnected) return "";
  HTTPClient http;
  String url = String(FIREBASE_DB) + path + ".json?auth=" + String(FIREBASE_AUTH);
  http.begin(url);
  http.setTimeout(6000);
  int code = http.GET();
  String resp = (code == 200) ? http.getString() : "";
  http.end();
  if (code != 200) Serial.printf("[Firebase GET] HTTP %d\n", code);
  return resp;
}

bool firebasePUT(const String& path, const String& body) {
  if (!wifiConnected) return false;
  HTTPClient http;
  String url = String(FIREBASE_DB) + path + ".json?auth=" + String(FIREBASE_AUTH);
  http.begin(url);
  http.addHeader("Content-Type", "application/json");
  http.setTimeout(6000);
  int code = http.PUT(body);
  http.end();
  if (code != 200) Serial.printf("[Firebase PUT] HTTP %d\n", code);
  return (code == 200);
}

bool fetchPricesFromFirebase() {
  String resp = firebaseGET("/config/prices");
  if (resp.isEmpty() || resp == "null") return false;

  StaticJsonDocument<512> doc;
  if (deserializeJson(doc, resp) != DeserializationError::Ok) return false;

  bool updated = false;
  for (int i = 0; i < NUM_PRODUCTS; i++) {
    if (doc.containsKey(products[i].fbKey)) {
      float newPrice = doc[products[i].fbKey].as<float>();
      if (abs(newPrice - products[i].price) > 0.001f) {
        Serial.printf("[Price] %s: P%.2f -> P%.2f\n",
                      products[i].name, products[i].price, newPrice);
        products[i].price = newPrice;
        updated = true;
      }
    }
  }
  lastPricePoll = millis();
  if (updated) {
    lcdLine("Prices Updated!", "From website :) ");
    delay(1200);
    updateLCD();
  }
  return true;
}

bool sendDispenseSignal(int idx, float price, float inserted) {
  StaticJsonDocument<256> doc;
  doc["triggered"] = true;
  doc["product"]   = products[idx].name;
  doc["fbKey"]     = products[idx].fbKey;
  doc["price"]     = (float)(round(price    * 100) / 100.0);
  doc["inserted"]  = (float)(round(inserted * 100) / 100.0);
  doc["change"]    = (float)(round((inserted - price) * 100) / 100.0);
  doc["qty"]       = 1;
  // No timestamp here — website stamps it with JS Date() (no RTC needed)

  String body;
  serializeJson(doc, body);
  return firebasePUT("/dispense_signal", body);
}

bool resetDispenseSignal() {
  return firebasePUT("/dispense_signal", "{\"triggered\":false}");
}

// ═══════════════════════════════════════════════════════════
//  WIFI
// ═══════════════════════════════════════════════════════════
void connectWiFi() {
  vendoState = WIFI_SETUP;
  lcdLine("Connecting WiFi ", "Please wait...");
  Serial.println(F("[WiFi] Starting auto-connect"));

  WiFi.mode(WIFI_STA);
  WiFiManager wm;
  wm.setConfigPortalTimeout(180); // seconds
  wm.setConnectTimeout(20);

  // Try saved WiFi first, otherwise start AP portal
  const bool ok = wm.autoConnect("VendoTrack-Setup", "vendotrack");

  if (ok && WiFi.status() == WL_CONNECTED) {
    wifiConnected = true;
    Serial.printf("[WiFi] Connected — IP: %s\n", WiFi.localIP().toString().c_str());
    lcdLine("WiFi Connected! ", WiFi.localIP().toString().c_str());
    delay(1200);
  } else {
    wifiConnected = false;
    Serial.println(F("[WiFi] Failed — AP portal timed out"));
    lcdLine("WiFi Setup AP ", "SSID: VendoTrack");
    delay(1500);
  }
  vendoState = IDLE;
}

// ═══════════════════════════════════════════════════════════
//  WIRING NOTES
// ═══════════════════════════════════════════════════════════
/*
  COIN ACCEPTOR (Allan):
    DC 12   → 12V supply
    GND     → Common GND (shared with ESP32)
    COUNTER → Voltage divider → GPIO 13
    Coin    → Leave unconnected

    Voltage divider (REQUIRED — protects ESP32 from 12V):
      COUNTER ──── 10kΩ ──┬──── GPIO 13
                          │
                         10kΩ
                          │
                         GND

  BUTTONS:
    BTN1 (GPIO 32): → GPIO 32 and → GND         (INPUT_PULLUP)
    BTN2 (GPIO 33): same as BTN1
    BTN3 (GPIO 34): → GPIO 34 and → 3.3V
                    + 10kΩ resistor between GPIO 34 and GND
    BTN4 (GPIO 35): same as BTN3

  STEPPER DRIVERS (DRV8825 / A4988):
    STEP → PUL pins: 23 / 19 / 17
    DIR  → DIR pins: 22 / 18 / 16
    EN   → ENA pins: 21 / 5 / 4   (LOW=on, HIGH=off)

  LCD (I2C 16x2):
    VCC → 5V  |  GND → GND
    SDA → GPIO 25  |  SCL → GPIO 26
    Try address 0x3F if screen stays blank
*/
