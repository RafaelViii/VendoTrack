/*
 * ═══════════════════════════════════════════════════════
 *   BALLPEN VENDO MACHINE — ESP32  v2.0
 *   Price: P20 per pen | 3 Hoppers | Coin Acceptor
 *   Features:
 *     • WiFi + Firebase sales sync
 *     • Offline mode with per-hopper sale tracking
 *     • Adjustable max offline sales per hopper
 *     • Idle-triggered WiFi reconnect (30s timeout)
 *     • Auto-sync offline sales on reconnect
 * ═══════════════════════════════════════════════════════
 *
 *  Coin Acceptor wiring note:
 *    Most coin acceptors send N pulses per coin where
 *    N = coin value in pesos (1-pulse = P1, 5-pulse = P5, etc.)
 *    Adjust COIN_VALUE if yours sends 1 pulse per coin regardless.
 *
 *  LCD note:
 *    Columns 0–1 on both rows are broken.
 *    All text is offset to start at column LCD_OFFSET (2).
 *    Effective display width = 14 characters.
 *
 *  WiFi / Firebase note:
 *    The system connects to WiFi during boot.
 *    If disconnected mid-session, sales are saved locally
 *    (per-hopper, in NVS flash via Preferences).
 *    Reconnect happens ONLY during idle (no coin activity
 *    for IDLE_RECONNECT_MS milliseconds).
 *    On reconnect, all saved offline sales are synced.
 */

#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <WiFiManager.h>

// ──────────────────────────────────────────────────────────
// ★  ADJUSTABLE SETTINGS — change these freely  ★
// ──────────────────────────────────────────────────────────

// Maximum offline sales per hopper before showing
// "Please reconnect" message.  Set lower for testing.
#define MAX_OFFLINE_SALES   5      // ← change to 20 for production

// How long the machine must be fully idle (no coin inserted,
// no button pressed) before it tries to reconnect to WiFi.
// In milliseconds.  30 000 = 30 seconds.
#define IDLE_RECONNECT_MS   30000

const char* WIFI_AP_NAME   = "Smart Vendo Connect";

// Firebase Realtime Database
const char* FB_URL    = "https://smart-vend-63d7a-default-rtdb.asia-southeast1.firebasedatabase.app";
const char* FB_SECRET = "Ug5D6c4i7TQvKM4e7Dsg2bA61640rRW35nwRbBBH";

// Firebase key names for each hopper (must match website fbKey values)
const char* HOPPER_FB_KEY[3] = { "ballpen1", "ballpen2", "ballpen3" };

// ──────────────────────────────────────────────────────────

// ── Pin Definitions ──────────────────────────────────────
#define M1_PUL  23
#define M1_DIR  22
#define M1_ENA  21

#define M2_PUL  19
#define M2_DIR  18
#define M2_ENA  5

#define M3_PUL  17
#define M3_DIR  16
#define M3_ENA  4

#define BTN1        32
#define BTN2        33
#define BTN3        27
#define BTN_CANCEL  14

#define SDA_PIN   25
#define SCL_PIN   26
#define LCD_ADDR  0x27     // Change to 0x3F if LCD doesn't show
#define LCD_COLS  16
#define LCD_ROWS  2

#define COIN_PIN  13

// ── Machine Settings ─────────────────────────────────────
#define PRICE           20    // Pesos
#define COIN_GAP_MS    200    // ms silence after last pulse = coin burst is done
//   1–3   pulses → P1
//   4–7   pulses → P5
//   8–15  pulses → P10
//   16+   pulses → P20
#define STEPS_BOOT    1600    // 360° — startup calibration spin
#define STEPS_DISPENSE 400    // 90°  — dispense one pen
#define TRAVEL_SPEED   500    // Microseconds per half-pulse (lower = faster)
#define CW             LOW
#define DEBOUNCE_MS    50

// ── LCD Layout ───────────────────────────────────────────
#define LCD_OFFSET  2
#define LCD_WIDTH   (LCD_COLS - LCD_OFFSET)   // 14

// ─────────────────────────────────────────────────────────

// ── State Machine ─────────────────────────────────────────
enum State {
  ST_LOADING,          // Boot animation
  ST_INIT_SPIN,        // Startup 360° spin for all hoppers
  ST_WIFI_CONNECT,     // Connecting to WiFi at boot
  ST_IDLE,             // Waiting for coin / idle
  ST_OFFLINE_WARN,     // Hopper at max offline sales — prompt reconnect
  ST_READY,            // Enough coins inserted — pick a pen
  ST_DISPENSE,         // Motor running
  ST_THANKYOU,         // Post-dispense message
  ST_RECONNECTING,     // Attempting WiFi reconnect during idle
};

// ── Structs ───────────────────────────────────────────────
struct Motor {
  uint8_t pul, dir, ena;
};

struct Button {
  uint8_t  pin;
  bool     lastReading;
  bool     stableState;
  uint32_t lastChange;
};

// ── Hardware Objects ──────────────────────────────────────
LiquidCrystal_I2C lcd(LCD_ADDR, LCD_COLS, LCD_ROWS);
Preferences prefs;
WiFiManager wm;

Motor motors[3] = {
  { M1_PUL, M1_DIR, M1_ENA },
  { M2_PUL, M2_DIR, M2_ENA },
  { M3_PUL, M3_DIR, M3_ENA },
};

Button buttons[4] = {
  { BTN1,       HIGH, HIGH, 0 },  // Hopper 1
  { BTN2,       HIGH, HIGH, 0 },  // Hopper 2
  { BTN3,       HIGH, HIGH, 0 },  // Hopper 3
  { BTN_CANCEL, HIGH, HIGH, 0 },  // Cancel
};

// ── WiFi / Firebase Globals ───────────────────────────────
bool      wifiConnected    = false;
uint32_t  lastActivityMs   = 0;    // Timestamp of last coin/button activity
bool      idleDisplayed    = false; // Whether idle screen is currently showing

// ── Offline Sale Tracking (per hopper) ───────────────────
// Saved in NVS flash so they survive power cuts.
// Keys: "off_h0", "off_h1", "off_h2"
int offlineSales[3] = { 0, 0, 0 };

// ── Coin Globals ──────────────────────────────────────────
volatile int      coinPulses   = 0;
volatile uint32_t lastPulseMs  = 0;
int               coinBalance  = 0;
int               pendingAdd   = 0;
uint32_t          lastAnimMs   = 0;
int               lastBalance  = -1;

// ── State Globals ─────────────────────────────────────────
State             state        = ST_LOADING;
bool              entered      = false;
int               offlineWarnHopper = -1;  // Which hopper triggered the warn

#define ANIM_STEP_MS  45

// ─────────────────────────────────────────────────────────
// ISR — coin pulse
// ─────────────────────────────────────────────────────────
void IRAM_ATTR onCoinPulse() {
  coinPulses++;
  lastPulseMs   = millis();
  lastActivityMs = millis();  // Reset idle timer on coin
}

// ─────────────────────────────────────────────────────────
// SETUP
// ─────────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);

  Wire.begin(SDA_PIN, SCL_PIN);
  lcd.init();
  lcd.backlight();

  // Motors
  for (int i = 0; i < 3; i++) {
    pinMode(motors[i].pul, OUTPUT);
    pinMode(motors[i].dir, OUTPUT);
    pinMode(motors[i].ena, OUTPUT);
    digitalWrite(motors[i].ena, LOW);
  }

  // Buttons (active LOW with internal pull-up)
  for (int i = 0; i < 4; i++) {
    pinMode(buttons[i].pin, INPUT_PULLUP);
  }

  // Coin acceptor interrupt
  pinMode(COIN_PIN, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(COIN_PIN), onCoinPulse, FALLING);

  // Load offline sale counts from NVS
  prefs.begin("vendo", false);
  for (int i = 0; i < 3; i++) {
    String key = "off_h" + String(i);
    offlineSales[i] = prefs.getInt(key.c_str(), 0);
    Serial.printf("[NVS] Hopper %d offline sales loaded: %d\n", i+1, offlineSales[i]);
  }

  lastActivityMs = millis();
  Serial.println("Ballpen Vendo v2 — Booting...");
}

// ─────────────────────────────────────────────────────────
// LOOP
// ─────────────────────────────────────────────────────────
void loop() {
  drainCoinPulses();

  switch (state) {
    case ST_LOADING:       doLoading();       break;
    case ST_INIT_SPIN:     doInitSpin();      break;
    case ST_WIFI_CONNECT:  doWifiConnect();   break;
    case ST_IDLE:          doIdle();          break;
    case ST_OFFLINE_WARN:  doOfflineWarn();   break;
    case ST_READY:         doReady();         break;
    case ST_DISPENSE:      /* handled inline */ break;
    case ST_THANKYOU:      doThankYou();      break;
    case ST_RECONNECTING:  doReconnecting();  break;
  }
}

// ─────────────────────────────────────────────────────────
// COIN MAP
// ─────────────────────────────────────────────────────────
int mapCoin(int pulses) {
  if (pulses >= 1  && pulses <= 3)  return 1;
  if (pulses >= 4  && pulses <= 7)  return 5;
  if (pulses >= 8  && pulses <= 15) return 10;
  if (pulses >= 16)                 return 20;
  return 0;
}

void drainCoinPulses() {
  if (coinPulses > 0 && (millis() - lastPulseMs) >= COIN_GAP_MS) {
    noInterrupts();
    int burst  = coinPulses;
    coinPulses = 0;
    interrupts();

    int value = mapCoin(burst);
    if (value > 0) {
      pendingAdd += value;
      Serial.printf("[COIN] %d pulses → P%d | Balance will reach: P%d\n",
                    burst, value, coinBalance + pendingAdd);
    } else {
      Serial.printf("[COIN] %d pulses — ignored (noise)\n", burst);
    }
  }
}

// ─────────────────────────────────────────────────────────
// LCD HELPERS
// ─────────────────────────────────────────────────────────
void lcdPrint(const char* r0, const char* r1) {
  lcd.clear();
  lcd.setCursor(LCD_OFFSET, 0);
  lcd.print(r0);
  lcd.setCursor(LCD_OFFSET, 1);
  lcd.print(r1);
}

void lcdRow(int row, const char* txt) {
  lcd.setCursor(LCD_OFFSET, row);
  char buf[LCD_WIDTH + 1];
  snprintf(buf, sizeof(buf), "%-*s", LCD_WIDTH, txt);
  lcd.print(buf);
}

// ─────────────────────────────────────────────────────────
// FLUSH BUTTONS
// ─────────────────────────────────────────────────────────
void flushButtons() {
  for (int i = 0; i < 4; i++) {
    bool current = digitalRead(buttons[i].pin);
    buttons[i].lastReading  = current;
    buttons[i].stableState  = current;
    buttons[i].lastChange   = millis();
  }
  Serial.println("[BTN] Buttons flushed.");
}

// ─────────────────────────────────────────────────────────
// BUTTON DEBOUNCE
// ─────────────────────────────────────────────────────────
bool buttonPressed(Button &b) {
  bool reading = digitalRead(b.pin);
  if (reading != b.lastReading) {
    b.lastChange  = millis();
    b.lastReading = reading;
  }
  if ((millis() - b.lastChange) >= DEBOUNCE_MS) {
    if (reading != b.stableState) {
      b.stableState = reading;
      if (b.stableState == LOW) {
        lastActivityMs = millis();  // Reset idle timer on button press
        return true;
      }
    }
  }
  return false;
}

// ─────────────────────────────────────────────────────────
// MOTOR STEP (blocking)
// ─────────────────────────────────────────────────────────
void stepMotor(Motor &m, long steps, bool dir, int spd) {
  digitalWrite(m.dir, dir);
  delayMicroseconds(5);
  for (long i = 0; i < steps; i++) {
    digitalWrite(m.pul, HIGH);
    delayMicroseconds(spd);
    digitalWrite(m.pul, LOW);
    delayMicroseconds(spd);
  }
}

// ─────────────────────────────────────────────────────────
// RESET TRANSACTION
// ─────────────────────────────────────────────────────────
void resetTransaction() {
  noInterrupts();
  coinPulses = 0;
  interrupts();
  coinBalance = 0;
  pendingAdd  = 0;
  lastAnimMs  = 0;
  lastBalance = -1;
  state       = ST_IDLE;
  entered     = false;
  idleDisplayed = false;
}

// ─────────────────────────────────────────────────────────
// WiFi HELPERS
// ─────────────────────────────────────────────────────────

// Try to connect to WiFi; returns true if successful.
// timeout_ms: how long to wait before giving up.
bool connectWiFi(int timeout_ms = 10000) {
  (void)timeout_ms;

  VendoState prevState = vendoState;
  vendoState = ST_WIFI_CONNECT;
  lcdPrint(" Connecting WiFi", " Smart Vendo  ");
  Serial.println(F("[WiFi] WiFiManager autoConnect starting..."));

  WiFi.mode(WIFI_STA);

  WiFiManager wm;
  wm.setConfigPortalTimeout(180);

  bool ok = wm.autoConnect(WIFI_AP_NAME);

  if (ok && WiFi.status() == WL_CONNECTED) {
    wifiConnected = true;
    Serial.printf("[WiFi] Connected! IP: %s\n", WiFi.localIP().toString().c_str());
    lcdPrint(" WiFi  Online!", WiFi.localIP().toString().c_str());
    delay(1200);
  } else {
    wifiConnected = false;
    WiFi.disconnect(false, false);
    Serial.println(F("[WiFi] WiFiManager failed or timed out."));
    lcdPrint(" Offline Mode ", " System ready ");
    delay(1200);
  }

  vendoState = prevState;
  return wifiConnected;
}

bool isWifiUp() {
  return (WiFi.status() == WL_CONNECTED);
}

// ─────────────────────────────────────────────────────────
// FIREBASE HELPERS
// ─────────────────────────────────────────────────────────

// POST a sale event to Firebase for one hopper.
// Returns true on success.
bool fbPostSale(int hopperIdx, int qty) {
  if (!isWifiUp()) return false;

  String url = String(FB_URL) + "/sales/live.json?auth=" + String(FB_SECRET);

  HTTPClient http;
  http.begin(url);
  http.addHeader("Content-Type", "application/json");
  http.setTimeout(5000);

  // Payload: includes hopper key, qty sold, and a timestamp placeholder.
  // The website will record the full timestamp with JS Date().
  String json = "{\"hopper\":\"" + String(HOPPER_FB_KEY[hopperIdx]) + "\""
              + ",\"qty\":" + String(qty)
              + ",\"triggered\":true"
              + ",\"product\":\"Ballpen " + String(hopperIdx + 1) + "\""
              + ",\"price\":" + String(PRICE)
              + "}";

  // Use PATCH to /dispense_signal so website listener picks it up
  String signalUrl = String(FB_URL) + "/dispense_signal.json?auth=" + String(FB_SECRET);
  http.end();
  http.begin(signalUrl);
  http.addHeader("Content-Type", "application/json");
  http.setTimeout(5000);

  int code = http.PUT(json);
  bool ok  = (code > 0 && code < 400);

  Serial.printf("[Firebase] Sale signal for hopper %d: HTTP %d (%s)\n",
                hopperIdx + 1, code, ok ? "OK" : "FAILED");
  http.end();
  return ok;
}

// Sync all saved offline sales to Firebase, then clear them.
void syncOfflineSales() {
  Serial.println("[Sync] Syncing offline sales...");
  bool anyFailed = false;

  for (int i = 0; i < 3; i++) {
    if (offlineSales[i] <= 0) continue;

    Serial.printf("[Sync] Hopper %d: %d offline sales\n", i + 1, offlineSales[i]);

    // Post a single aggregate entry for this hopper's offline sales
    String signalUrl = String(FB_URL) + "/dispense_signal.json?auth=" + String(FB_SECRET);
    HTTPClient http;
    http.begin(signalUrl);
    http.addHeader("Content-Type", "application/json");
    http.setTimeout(5000);

    String json = "{\"hopper\":\"" + String(HOPPER_FB_KEY[i]) + "\""
                + ",\"qty\":" + String(offlineSales[i])
                + ",\"triggered\":true"
                + ",\"offline_sync\":true"
                + ",\"product\":\"Ballpen " + String(i + 1) + "\""
                + ",\"price\":" + String(PRICE)
                + "}";

    int code = http.PUT(json);
    bool ok  = (code > 0 && code < 400);
    http.end();

    if (ok) {
      Serial.printf("[Sync] Hopper %d sync OK (%d sales).\n", i + 1, offlineSales[i]);
      offlineSales[i] = 0;
      String key = "off_h" + String(i);
      prefs.putInt(key.c_str(), 0);

      // Small delay between posts to avoid Firebase rate limits
      delay(400);
    } else {
      Serial.printf("[Sync] Hopper %d sync FAILED.\n", i + 1);
      anyFailed = true;
    }
  }

  if (!anyFailed) {
    Serial.println("[Sync] All offline sales synced successfully.");
  } else {
    Serial.println("[Sync] Some sales failed to sync — will retry next reconnect.");
  }
}

// ─────────────────────────────────────────────────────────
// OFFLINE SALE — save one sale for a hopper to NVS
// ─────────────────────────────────────────────────────────
void saveOfflineSale(int hopperIdx) {
  offlineSales[hopperIdx]++;
  String key = "off_h" + String(hopperIdx);
  prefs.putInt(key.c_str(), offlineSales[hopperIdx]);
  Serial.printf("[Offline] Hopper %d offline sale saved. Total: %d\n",
                hopperIdx + 1, offlineSales[hopperIdx]);
}

// Returns true if any hopper has reached or exceeded MAX_OFFLINE_SALES
bool anyHopperAtLimit() {
  for (int i = 0; i < 3; i++) {
    if (offlineSales[i] >= MAX_OFFLINE_SALES) return true;
  }
  return false;
}

// Returns true if the specific hopper is at its limit
bool hopperAtLimit(int idx) {
  return (offlineSales[idx] >= MAX_OFFLINE_SALES);
}

// ─────────────────────────────────────────────────────────
// STATE: LOADING — boot animation
// ─────────────────────────────────────────────────────────
void doLoading() {
  if (!entered) {
    entered = true;

    lcdPrint("PEN-O-MATIC v2", "  Loading...  ");
    delay(900);

    lcd.clear();
    lcd.setCursor(LCD_OFFSET, 0);
    lcd.print("PEN-O-MATIC v2");

    for (int col = LCD_OFFSET; col < LCD_COLS; col++) {
      lcd.setCursor(col, 1);
      lcd.print("=");
      delay(90);
    }
    delay(300);

    for (int i = 0; i < 3; i++) {
      lcdRow(0, "PEN-O-MATIC v2");
      lcdRow(1, "  ** READY ** ");
      delay(350);
      lcdRow(1, "              ");
      delay(300);
    }

    delay(200);
    state   = ST_INIT_SPIN;
    entered = false;
  }
}

// ─────────────────────────────────────────────────────────
// STATE: INIT SPIN — calibrate all 3 hoppers
// ─────────────────────────────────────────────────────────
void doInitSpin() {
  lcdPrint("  Calibrating ", "   Motors...  ");
  delay(600);

  for (int i = 0; i < 3; i++) {
    char r0[LCD_WIDTH + 1];
    snprintf(r0, sizeof(r0), " Hopper %d / 3 ", i + 1);
    lcdRow(0, r0);
    lcdRow(1, "  Spinning... ");
    Serial.printf("[INIT] Spinning Hopper %d 360°...\n", i + 1);
    stepMotor(motors[i], STEPS_BOOT, CW, TRAVEL_SPEED);
    delay(250);
  }

  lcdPrint(" System  Ready", "  All Clear!  ");
  delay(1800);

  // After init, try WiFi
  state   = ST_WIFI_CONNECT;
  entered = false;
}

// ─────────────────────────────────────────────────────────
// STATE: WIFI CONNECT — connect at boot (non-blocking display)
// ─────────────────────────────────────────────────────────
void doWifiConnect() {
  if (!entered) {
    entered = true;
    lcdPrint(" Connecting to", "   Network... ");
    Serial.println("[WiFi] Boot connection attempt...");
  }

  bool ok = connectWiFi(10000);

  if (ok) {
    lcdPrint(" WiFi  Online!",  "  Syncing...  ");
    delay(800);
    syncOfflineSales();   // sync any leftover offline sales from previous session
    lcdPrint(" Sync Complete", "              ");
    delay(600);
  } else {
    lcdPrint(" Offline Mode ", " System ready ");
    delay(1500);
  }

  resetTransaction();   // → ST_IDLE
}

// ─────────────────────────────────────────────────────────
// STATE: IDLE — waiting for coin
// ─────────────────────────────────────────────────────────
void doIdle() {
  if (!entered) {
    entered = true;
    flushButtons();
    wifiConnected = isWifiUp();

    if (wifiConnected) {
      lcdPrint("  Insert  P20 ", " Balance: P0  ");
    } else {
      lcdPrint("  Insert  P20 ", "[OFFLINE]  P0 ");
    }
    lastBalance   = 0;
    idleDisplayed = false;
  }

  // ── Coin count-up animation ───────────────────────────
  if (pendingAdd > 0 && (millis() - lastAnimMs) >= ANIM_STEP_MS) {
    lastAnimMs = millis();
    coinBalance++;
    pendingAdd--;
    char buf[LCD_WIDTH + 1];
    if (wifiConnected) {
      snprintf(buf, sizeof(buf), " Balance: P%-3d", coinBalance);
    } else {
      snprintf(buf, sizeof(buf), "[OFF]   P%-5d", coinBalance);
    }
    lcdRow(1, buf);
    lastBalance = coinBalance;
  }

  // ── Cancel ────────────────────────────────────────────
  if (buttonPressed(buttons[3])) {
    Serial.println("[CANCEL] Cancelled. No refund.");
    lcdPrint(" CANCELLED !! ", " No refund :( ");
    delay(2200);
    resetTransaction();
    return;
  }

  // ── Ready check ───────────────────────────────────────
  if (coinBalance >= PRICE && pendingAdd == 0) {
    state   = ST_READY;
    entered = false;
    return;
  }

  // ── Idle reconnect logic ──────────────────────────────
  // If no coins have been inserted and the machine has been
  // sitting idle for IDLE_RECONNECT_MS, try to reconnect.
  bool isIdle = (coinBalance == 0 && pendingAdd == 0);

  if (isIdle && !wifiConnected &&
      (millis() - lastActivityMs) >= IDLE_RECONNECT_MS) {

    // Only show idle screen and attempt reconnect once per idle period
    if (!idleDisplayed) {
      idleDisplayed = true;
      Serial.println("[IDLE] Idle timeout — attempting WiFi reconnect...");
      state   = ST_RECONNECTING;
      entered = false;
    }
    return;
  }

  // Show idle message when connected and sitting still
  if (isIdle && wifiConnected &&
      (millis() - lastActivityMs) >= IDLE_RECONNECT_MS &&
      !idleDisplayed) {
    idleDisplayed = true;
    lcdPrint("  Insert  P20 ", " Power Saving ");
  }
}

// ─────────────────────────────────────────────────────────
// STATE: RECONNECTING — attempt reconnect during idle
// ─────────────────────────────────────────────────────────
void doReconnecting() {
  if (!entered) {
    entered = true;
    lcdPrint(" Reconnecting.", "  Please wait ");
    Serial.println("[WiFi] Attempting idle reconnect...");
  }

  bool ok = connectWiFi(8000);

  if (ok) {
    lcdPrint(" Back  Online!", "  Syncing...  ");
    delay(600);
    syncOfflineSales();
    lcdPrint(" Sync Complete", "  Insert P20  ");
    delay(800);
    wifiConnected = true;
    Serial.println("[WiFi] Reconnected and synced.");
  } else {
    lcdPrint(" Still Offline", " Insert  P20  ");
    Serial.println("[WiFi] Reconnect failed — staying offline.");
    delay(1200);
  }
  // Reset idle timer so we don't immediately try again
  lastActivityMs = millis();
  resetTransaction();   // → ST_IDLE
}

// ─────────────────────────────────────────────────────────
// STATE: OFFLINE_WARN — hopper at sale limit
// ─────────────────────────────────────────────────────────
void doOfflineWarn() {
  if (!entered) {
    entered = true;
    lcdPrint("Max Offline   ", "Please Connect");
    Serial.printf("[WARN] Hopper %d at offline limit (%d). Prompting reconnect.\n",
                  offlineWarnHopper + 1, MAX_OFFLINE_SALES);
  }

  // Wait for idle + then attempt reconnect
  delay(3000);
  state   = ST_RECONNECTING;
  entered = false;
}

// ─────────────────────────────────────────────────────────
// STATE: READY — coin accepted, select hopper
// ─────────────────────────────────────────────────────────
void doReady() {
  if (!entered) {
    entered = true;
    flushButtons();

    if (wifiConnected) {
      lcdPrint(" Select  Pen: ", " [1]  [2]  [3]");
    } else {
      lcdPrint("[OFFLINE] Pick", " [1]  [2]  [3]");
    }
    Serial.printf("[READY] P%d accepted.\n", coinBalance);
  }

  for (int i = 0; i < 3; i++) {
    if (buttonPressed(buttons[i])) {
      Serial.printf("[SELECT] Hopper %d chosen.\n", i + 1);

      // Check if this hopper has hit its offline limit
      if (!wifiConnected && hopperAtLimit(i)) {
        offlineWarnHopper = i;
        state   = ST_OFFLINE_WARN;
        entered = false;
        return;
      }

      dispense(i);
      return;
    }
  }

  if (buttonPressed(buttons[3])) {
    lcdPrint(" CANCELLED !! ", " No refund :( ");
    delay(2200);
    resetTransaction();
  }
}

// ─────────────────────────────────────────────────────────
// DISPENSE — run motor and record sale
// ─────────────────────────────────────────────────────────
void dispense(int hopperIdx) {
  state = ST_DISPENSE;

  char r0[LCD_WIDTH + 1];
  snprintf(r0, sizeof(r0), " Hopper  %d    ", hopperIdx + 1);
  lcdPrint(r0, " Dispensing.. ");

  Serial.printf("[DISPENSE] Running Hopper %d...\n", hopperIdx + 1);
  stepMotor(motors[hopperIdx], STEPS_DISPENSE, CW, TRAVEL_SPEED);
  Serial.println("[DISPENSE] Done.");

  // ── Record the sale ─────────────────────────────────
  wifiConnected = isWifiUp();

  if (wifiConnected) {
    bool sent = fbPostSale(hopperIdx, 1);
    if (!sent) {
      // Firebase send failed — save offline
      Serial.println("[Firebase] Send failed — saving offline.");
      saveOfflineSale(hopperIdx);
    }
  } else {
    saveOfflineSale(hopperIdx);
    Serial.printf("[Offline] Sale saved locally. Hopper %d total offline: %d/%d\n",
                  hopperIdx + 1, offlineSales[hopperIdx], MAX_OFFLINE_SALES);
  }

  state   = ST_THANKYOU;
  entered = false;
}

// ─────────────────────────────────────────────────────────
// STATE: THANK YOU
// ─────────────────────────────────────────────────────────
void doThankYou() {
  if (!entered) {
    entered = true;
    if (wifiConnected) {
      lcdPrint("  Thank You!  ", "  Enjoy! :)   ");
    } else {
      lcdPrint("  Thank You!  ", " [Offline] :) ");
    }
    Serial.println("[DONE] Transaction complete.");
  }

  delay(2500);
  resetTransaction();   // → ST_IDLE
}
