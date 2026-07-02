#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Wire.h>
#include <WebServer.h>
#include <Preferences.h>
#include <Update.h>
#include <stdarg.h>

// Wi-Fi Credentials
const char* ssid     = "roods";
const char* password = "Frinov25!+!";

// Your Server Endpoint (Replace with your API Gateway, Firebase, or Webhook URL)
const char* serverTarget = "http://your-api-endpoint.com/api/logs";

// Assign ESP32 GPIO Pins
const int PUMP_FILL    = 22;
const int PUMP_SUCK    = 25;
const int PUMP_REAGENT = 26;
const int PUMP_FLUSH   = 27;
const int STIRRER_PIN  = 5;  // Stirrer/mixer relay or MOSFET output

// pH probe analog input.
// Use an ADC-only pin such as GPIO34/35/36/39 on most ESP32 boards.
const int PH_PIN = 34;

// Timing Configuration
const unsigned long ONE_HOUR          = 3600000;
const unsigned long TIME_FILL_50ML    = 15000;
const unsigned long TIME_SUCK_DOWN    = 10000;
const unsigned long TIME_REAGENT_DOSE = 1500;
const unsigned long TIME_RO_FLUSH     = 12000;
const unsigned long TIME_DRAIN_ALL    = 18000;
const unsigned long TIME_STIR_MIX     = 15000; // Stir after reagent dose, then shut off

// Reagent pump calibration.
// Calibration page runs the reagent pump for 10 seconds.
// Enter collected mL and the ESP32 saves calculated mL/min.
const unsigned long REAGENT_CAL_RUN_MS = 10000UL;
const unsigned long REAGENT_PRIME_MS   = 750UL;
const unsigned long REAGENT_PULSE_ON_MS  = 75UL;
const unsigned long REAGENT_PULSE_OFF_MS = 225UL;

float reagentMlPerMin = 30.0f;      // default until calibrated
float lastReagentOnMs = 0.0f;       // actual ON time used during last slow dose
float lastReagentEstimatedMl = 0.0f;

unsigned long lastTestTime = 0;
const unsigned long FIRST_TEST_DELAY_MS = 30000UL; // lets the web page come up after boot

// Test variables
float tankPH = 0.0;
float postReagentPH = 0.0;
float calculatedDKH = 0.0;

// Web calibration
WebServer server(80);
Preferences prefs;

// Browser serial monitor.
// Open http://ESP32-IP/serial to watch logs without USB.
String webLogBuffer = "";
const size_t WEB_LOG_MAX_CHARS = 12000;


float ph7Adc = 1900.0f;   // Saved ADC value while probe is in pH 7.00 solution
float ph10Adc = 1300.0f;  // Saved ADC value while probe is in pH 10.00 solution
float phOffset = 0.0f;    // Small manual trim after calibration

float convertToDKH(float baseline, float dropped);
float readPH();
float readRawPhAdc();
float rawAdcToPh(float raw);
void connectToWiFi();
void sendDataToCloud(float phValue, float alkValue);
void loadPhCalibration();
void savePhCalibration();
void loadReagentCalibration();
void saveReagentCalibration();
float getSlowReagentDoseOnMs();
float estimateReagentMlFromOnMs(float onMs);
void setupWebServer();
void handleRoot();
void handleApiPh();
void handleSerialPage();
void handleSerialText();
void handleClearSerial();
void appendWebLog(const String& msg);
void logLine(const String& msg);
void logPrint(const String& msg);
void logPrintf(const char* fmt, ...);
void handleCalibrate();
void handleSetCalibration();
void handleSaveReagentCalibration();
void handleTestReagentCalibration();
void handleTestStirrer();
void stirrerOff(const char* reason);
void allOutputsOff(const char* reason);
void handleOtaPage();
void handleOtaUpload();
void handleOtaDone();
void handleNotFound();
void webSafeDelay(unsigned long waitMs);
bool hasRealServerTarget();

enum TestState {
  IDLE,
  FILL_TANK_WATER,
  SUCK_TO_25ML,
  MEASURE_TANK_PH,
  DOSE_REAGENT,
  MEASURE_ALKALINITY,
  RO_RINSE,
  FINAL_DRAIN
};

TestState currentState = IDLE;

void setup() {
  Serial.begin(115200);
  logLine("\nBooting reef alkalinity tester...");

  // Safe boot: set outputs LOW before and after pinMode so pumps/stirrer stay OFF.
  digitalWrite(PUMP_FILL, LOW);
  digitalWrite(PUMP_SUCK, LOW);
  digitalWrite(PUMP_REAGENT, LOW);
  digitalWrite(PUMP_FLUSH, LOW);
  digitalWrite(STIRRER_PIN, LOW);

  pinMode(PUMP_FILL, OUTPUT);
  pinMode(PUMP_SUCK, OUTPUT);
  pinMode(PUMP_REAGENT, OUTPUT);
  pinMode(PUMP_FLUSH, OUTPUT);
  pinMode(STIRRER_PIN, OUTPUT);

  allOutputsOff("boot");

  analogReadResolution(12);
  analogSetPinAttenuation(PH_PIN, ADC_11db);

  loadPhCalibration();
  loadReagentCalibration();

  connectToWiFi();
  setupWebServer();

  logLine("pH calibration page:");
  logLine("http://" + WiFi.localIP().toString());
  logLine("Browser serial monitor:");
  logLine("http://" + WiFi.localIP().toString() + "/serial");
  logLine("Also works at:");
  logLine("http://" + WiFi.localIP().toString() + "/webserial");
  logLine("OTA update page:");
  logLine("http://" + WiFi.localIP().toString() + "/ota");
}

void loop() {
  server.handleClient();

  // Keep Wi-Fi connected automatically
  if (WiFi.status() != WL_CONNECTED) {
    connectToWiFi();
  }

  unsigned long currentMillis = millis();

  switch (currentState) {
    case IDLE:
      allOutputsOff("idle");
      if ((lastTestTime == 0 && currentMillis >= FIRST_TEST_DELAY_MS) ||
          (lastTestTime != 0 && currentMillis - lastTestTime >= ONE_HOUR)) {
        logLine("\n--- Starting New Hourly Test Cycle ---");
        currentState = FILL_TANK_WATER;
      }
      break;

    case FILL_TANK_WATER:
      stirrerOff("fill");
      digitalWrite(PUMP_FILL, HIGH);
      webSafeDelay(TIME_FILL_50ML);
      digitalWrite(PUMP_FILL, LOW);
      currentState = SUCK_TO_25ML;
      break;

    case SUCK_TO_25ML:
      stirrerOff("suck");
      digitalWrite(PUMP_SUCK, HIGH);
      webSafeDelay(TIME_SUCK_DOWN);
      digitalWrite(PUMP_SUCK, LOW);
      currentState = MEASURE_TANK_PH;
      break;

    case MEASURE_TANK_PH:
      webSafeDelay(1000);
      tankPH = readPH();
      logLine("[3/7] Tank pH Recorded: " + String(tankPH, 2));
      currentState = DOSE_REAGENT;
      break;

    case DOSE_REAGENT: {
      // Reagent dosing: fast prime followed by slow pulses for better accuracy.
      lastReagentOnMs = 0.0f;
      lastReagentEstimatedMl = 0.0f;

      digitalWrite(PUMP_REAGENT, HIGH);
      webSafeDelay(REAGENT_PRIME_MS);   // Prime tubing
      digitalWrite(PUMP_REAGENT, LOW);
      lastReagentOnMs += REAGENT_PRIME_MS;
      webSafeDelay(200);

      unsigned long pulseTime = (TIME_REAGENT_DOSE > REAGENT_PRIME_MS) ? (TIME_REAGENT_DOSE - REAGENT_PRIME_MS) : 0;
      unsigned long elapsed = 0;

      while (elapsed < pulseTime) {
        digitalWrite(PUMP_REAGENT, HIGH);
        webSafeDelay(REAGENT_PULSE_ON_MS);
        digitalWrite(PUMP_REAGENT, LOW);
        lastReagentOnMs += REAGENT_PULSE_ON_MS;
        webSafeDelay(REAGENT_PULSE_OFF_MS);
        elapsed += (REAGENT_PULSE_ON_MS + REAGENT_PULSE_OFF_MS);
      }

      lastReagentEstimatedMl = estimateReagentMlFromOnMs(lastReagentOnMs);

      logLine("[4/7] Reagent dosed slow pulse mode. ON ms=" + String(lastReagentOnMs, 0) +
              " estimated=" + String(lastReagentEstimatedMl, 3) + " mL. Stirrer ON for mixing.");
      digitalWrite(STIRRER_PIN, HIGH);
      webSafeDelay(TIME_STIR_MIX);
      stirrerOff("mix-complete");

      webSafeDelay(3000); // Let bubbles/turbulence settle before final pH read
      currentState = MEASURE_ALKALINITY;
      break;
    }

    case MEASURE_ALKALINITY:
      postReagentPH = readPH();
      calculatedDKH = convertToDKH(tankPH, postReagentPH);

      logLine("[5/7] Test Finished. Alk: " + String(calculatedDKH, 2) + " dKH");

      sendDataToCloud(tankPH, calculatedDKH);

      currentState = RO_RINSE;
      break;

    case RO_RINSE:
      stirrerOff("ro-rinse");
      currentState = FINAL_DRAIN;
      break;

    case FINAL_DRAIN:
      allOutputsOff("final-drain");
      lastTestTime = millis();
      currentState = IDLE;
      break;
  }
}

// --- Wi-Fi ---

void connectToWiFi() {
  if (WiFi.status() == WL_CONNECTED) return;

  logLine("Connecting to Wi-Fi: " + String(ssid));
  WiFi.begin(ssid, password);

  int timeoutCount = 0;
  while (WiFi.status() != WL_CONNECTED && timeoutCount < 20) {
    if (server.client()) server.handleClient();
    delay(500);
    logPrint(".");
    timeoutCount++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    logLine("\nWi-Fi Connected successfully!");
    logLine("IP Address: " + WiFi.localIP().toString());
  } else {
    logLine("\nWi-Fi connection failed. Will retry next cycle.");
    allOutputsOff("wifi-failed");
  }
}

// --- pH calibration and reading ---

void loadPhCalibration() {
  prefs.begin("ph-cal", true);
  ph7Adc = prefs.getFloat("ph7", ph7Adc);
  ph10Adc = prefs.getFloat("ph10", ph10Adc);
  phOffset = prefs.getFloat("offset", phOffset);
  prefs.end();

  logPrintf("Loaded pH calibration: pH7 ADC=%.1f pH10 ADC=%.1f offset=%.3f",
            ph7Adc, ph10Adc, phOffset);
}

void savePhCalibration() {
  prefs.begin("ph-cal", false);
  prefs.putFloat("ph7", ph7Adc);
  prefs.putFloat("ph10", ph10Adc);
  prefs.putFloat("offset", phOffset);
  prefs.end();

  logPrintf("Saved pH calibration: pH7 ADC=%.1f pH10 ADC=%.1f offset=%.3f",
            ph7Adc, ph10Adc, phOffset);
}


void loadReagentCalibration() {
  prefs.begin("reagent", true);
  reagentMlPerMin = prefs.getFloat("mlMin", reagentMlPerMin);
  prefs.end();

  if (!isfinite(reagentMlPerMin) || reagentMlPerMin <= 0.0f) {
    reagentMlPerMin = 30.0f;
  }

  logPrintf("Loaded reagent pump calibration: %.3f mL/min", reagentMlPerMin);
}

void saveReagentCalibration() {
  if (!isfinite(reagentMlPerMin) || reagentMlPerMin <= 0.0f) {
    reagentMlPerMin = 30.0f;
  }

  prefs.begin("reagent", false);
  prefs.putFloat("mlMin", reagentMlPerMin);
  prefs.end();

  logPrintf("Saved reagent pump calibration: %.3f mL/min", reagentMlPerMin);
}

float getSlowReagentDoseOnMs() {
  float onMs = (float)REAGENT_PRIME_MS;

  unsigned long pulseTime = (TIME_REAGENT_DOSE > REAGENT_PRIME_MS) ? (TIME_REAGENT_DOSE - REAGENT_PRIME_MS) : 0;
  unsigned long elapsed = 0;

  while (elapsed < pulseTime) {
    onMs += (float)REAGENT_PULSE_ON_MS;
    elapsed += (REAGENT_PULSE_ON_MS + REAGENT_PULSE_OFF_MS);
  }

  return onMs;
}

float estimateReagentMlFromOnMs(float onMs) {
  if (!isfinite(onMs) || onMs <= 0.0f) return 0.0f;
  return (reagentMlPerMin / 60000.0f) * onMs;
}

float readRawPhAdc() {
  const int samples = 25;
  uint32_t total = 0;

  for (int i = 0; i < samples; i++) {
    total += analogRead(PH_PIN);
    delay(5);
  }

  return (float)total / (float)samples;
}

float rawAdcToPh(float raw) {
  if (fabs(ph10Adc - ph7Adc) < 5.0f) {
    return 8.15f; // Fallback if calibration is invalid
  }

  float slope = (10.00f - 7.00f) / (ph10Adc - ph7Adc);
  float ph = 7.00f + ((raw - ph7Adc) * slope) + phOffset;

  if (!isfinite(ph)) return 8.15f;
  if (ph < 0.0f) ph = 0.0f;
  if (ph > 14.0f) ph = 14.0f;

  return ph;
}

float readPH() {
  float raw = readRawPhAdc();
  return rawAdcToPh(raw);
}

// --- Web page ---

void setupWebServer() {
  server.on("/", HTTP_GET, handleRoot);
  server.on("/api/ph", HTTP_GET, handleApiPh);
  server.on("/serial", HTTP_GET, handleSerialPage);
  server.on("/webserial", HTTP_GET, handleSerialPage);
  server.on("/serial.txt", HTTP_GET, handleSerialText);
  server.on("/webserial.txt", HTTP_GET, handleSerialText);
  server.on("/clear-serial", HTTP_POST, handleClearSerial);
  server.on("/calibrate", HTTP_POST, handleCalibrate);
  server.on("/set", HTTP_POST, handleSetCalibration);
  server.on("/save-reagent-cal", HTTP_POST, handleSaveReagentCalibration);
  server.on("/test-reagent-cal", HTTP_POST, handleTestReagentCalibration);
  server.on("/test-stirrer", HTTP_POST, handleTestStirrer);
  server.on("/ota", HTTP_GET, handleOtaPage);
  server.on("/ota", HTTP_POST, handleOtaDone, handleOtaUpload);
  server.onNotFound(handleNotFound);
  server.begin();
  logLine("pH calibration web server started on port 80.");
}

void handleRoot() {
  float raw = readRawPhAdc();
  float ph = rawAdcToPh(raw);
  float voltage = raw * 3.3f / 4095.0f;

  String html;
  html.reserve(8500);
  html += F("<!doctype html><html><head><meta name='viewport' content='width=device-width,initial-scale=1'>");
  html += F("<title>pH Probe Calibration</title>");
  html += F("<style>body{font-family:Arial;margin:20px;background:#f6f8fb;color:#111}.card{background:white;border-radius:14px;padding:18px;margin:14px 0;box-shadow:0 2px 10px #0001}button{font-size:18px;padding:12px 16px;border:0;border-radius:10px;background:#1565c0;color:white;margin:6px 0}input{font-size:18px;padding:10px;width:120px}.val{font-size:28px;font-weight:bold}.muted{color:#666}.danger{background:#b00020}</style>");
  html += F("</head><body><h2>pH Probe Calibration</h2>");

  html += F("<div class='card'><div class='muted'>Current Reading</div>");
  html += "<div class='val'>pH " + String(ph, 2) + "</div>";
  html += "Raw ADC: " + String(raw, 1) + "<br>";
  html += "Voltage: " + String(voltage, 3) + " V<br>";
  html += "State: " + String((int)currentState) + "<br>";
  html += F("<p class='muted'>Refresh the page to update the reading. The web server now stays alive during pump/stirrer waits.</p></div>");

  html += F("<div class='card'><h3>Two-point calibration</h3>");
  html += F("<p>Put probe in pH 7.00 buffer, wait for it to stabilize, then press Save pH 7.</p>");
  html += F("<form method='POST' action='/calibrate'><input type='hidden' name='point' value='7'><button>Save pH 7.00</button></form>");
  html += F("<p>Rinse probe, put it in pH 10.00 buffer, wait for it to stabilize, then press Save pH 10.</p>");
  html += F("<form method='POST' action='/calibrate'><input type='hidden' name='point' value='10'><button>Save pH 10.00</button></form></div>");

  html += F("<div class='card'><h3>Manual values</h3>");
  html += F("<form method='POST' action='/set'>");
  html += "pH 7 ADC <input name='ph7' value='" + String(ph7Adc, 1) + "'><br><br>";
  html += "pH 10 ADC <input name='ph10' value='" + String(ph10Adc, 1) + "'><br><br>";
  html += "Offset <input name='offset' value='" + String(phOffset, 3) + "'><br><br>";
  html += F("<button>Save Manual Calibration</button></form></div>");

  html += F("<div class='card'><h3>Reagent Pump Calibration</h3>");
  html += "Reagent GPIO: " + String(PUMP_REAGENT) + "<br>";
  html += "Saved flow: <b>" + String(reagentMlPerMin, 3) + " mL/min</b><br>";
  html += "Calibration run: " + String(REAGENT_CAL_RUN_MS / 1000UL) + " seconds<br>";
  html += "Current slow dose ON time: " + String(REAGENT_PRIME_MS) + " ms prime + " + String(REAGENT_PULSE_ON_MS) + " ms pulses<br>";
  html += "Estimated reagent per test: <b>" + String(estimateReagentMlFromOnMs(getSlowReagentDoseOnMs()), 3) + " mL</b><br>";
  html += "Last test reagent: " + String(lastReagentEstimatedMl, 3) + " mL from " + String(lastReagentOnMs, 0) + " ms ON<br>";
  html += F("<p class='muted'>To calibrate: put the reagent tube in a measuring cup, press Run Reagent 10 Seconds, then enter the collected mL below.</p>");
  html += F("<form method='POST' action='/test-reagent-cal'><button class='danger'>Run Reagent 10 Seconds</button></form>");
  html += F("<form method='POST' action='/save-reagent-cal'>Collected mL <input name='collectedMl' value='5.0'><br><br>");
  html += F("<button>Save Reagent Calibration</button></form></div>");

  html += F("<div class='card'><h3>Stirrer</h3>");
  html += "Stirrer GPIO: " + String(STIRRER_PIN) + "<br>Mix time: " + String(TIME_STIR_MIX / 1000UL) + " seconds<br>";
  html += F("<p class='muted'>The stirrer only runs during reagent mixing and is forced OFF between tests.</p>");
  html += F("<form method='POST' action='/test-stirrer'><button>Test Stirrer 5 Seconds</button></form></div>");

  html += F("<div class='card'><h3>Browser Serial Monitor</h3><p><a href='/serial'>Open /serial live log</a></p><p><a href='/webserial'>Open /webserial live log</a></p></div>");
  html += F("<div class='card'><h3>JSON</h3><p><a href='/api/ph'>/api/ph</a></p></div>");
  html += F("<div class='card'><h3>Firmware OTA</h3><p>Upload a compiled .bin file from PlatformIO.</p><p><a href='/ota'>Open OTA Update Page</a></p></div>");
  html += F("</body></html>");

  server.send(200, "text/html", html);
}

void handleApiPh() {
  float raw = readRawPhAdc();
  float ph = rawAdcToPh(raw);
  float voltage = raw * 3.3f / 4095.0f;

  JsonDocument doc;
  doc["ok"] = true;
  doc["ph"] = ph;
  doc["rawAdc"] = raw;
  doc["voltage"] = voltage;
  doc["calibration"]["ph7Adc"] = ph7Adc;
  doc["calibration"]["ph10Adc"] = ph10Adc;
  doc["calibration"]["offset"] = phOffset;
  doc["reagent"]["pin"] = PUMP_REAGENT;
  doc["reagent"]["mlPerMin"] = reagentMlPerMin;
  doc["reagent"]["calRunSeconds"] = REAGENT_CAL_RUN_MS / 1000UL;
  doc["reagent"]["primeMs"] = REAGENT_PRIME_MS;
  doc["reagent"]["pulseOnMs"] = REAGENT_PULSE_ON_MS;
  doc["reagent"]["pulseOffMs"] = REAGENT_PULSE_OFF_MS;
  doc["reagent"]["lastOnMs"] = lastReagentOnMs;
  doc["reagent"]["lastEstimatedMl"] = lastReagentEstimatedMl;
  doc["reagent"]["slowDoseOnMs"] = getSlowReagentDoseOnMs();
  doc["reagent"]["estimatedMlPerTest"] = estimateReagentMlFromOnMs(getSlowReagentDoseOnMs());
  doc["stirrer"]["pin"] = STIRRER_PIN;
  doc["stirrer"]["mixSeconds"] = TIME_STIR_MIX / 1000UL;

  String out;
  serializeJson(doc, out);
  server.send(200, "application/json", out);
}


void handleSerialPage() {
  String html;
  html.reserve(3500);
  html += F("<!doctype html><html><head><meta name='viewport' content='width=device-width,initial-scale=1'>");
  html += F("<title>Browser Serial Monitor</title>");
  html += F("<style>body{font-family:Arial;margin:18px;background:#111;color:#eee}.top{display:flex;gap:10px;align-items:center;flex-wrap:wrap}a,button{font-size:16px;padding:10px 12px;border-radius:8px;border:0;background:#1565c0;color:white;text-decoration:none}button.clear{background:#b00020}pre{background:#000;color:#0f0;padding:14px;border-radius:10px;white-space:pre-wrap;min-height:70vh;overflow:auto}.muted{color:#aaa}</style>");
  html += F("</head><body><div class='top'><h2>Browser Serial Monitor</h2><a href='/'>Home</a>");
  html += F("<form method='POST' action='/clear-serial'><button class='clear'>Clear Log</button></form></div>");
  html += F("<p class='muted'>Auto-refreshes every 2 seconds. USB Serial still works too.</p>");
  html += F("<pre id='log'>Loading...</pre>");
  html += F("<script>async function load(){try{let r=await fetch('/serial.txt?ms='+Date.now());let t=await r.text();let e=document.getElementById('log');e.textContent=t||'No log yet';e.scrollTop=e.scrollHeight;}catch(err){document.getElementById('log').textContent='Log fetch failed: '+err;}}load();setInterval(load,2000);</script>");
  html += F("</body></html>");
  server.send(200, "text/html", html);
}

void handleSerialText() {
  server.send(200, "text/plain", webLogBuffer);
}

void handleClearSerial() {
  webLogBuffer = "";
  logLine("Browser serial log cleared.");
  server.sendHeader("Location", "/serial", true);
  server.send(302, "text/plain", "");
}

void appendWebLog(const String& msg) {
  webLogBuffer += msg;
  while (webLogBuffer.length() > WEB_LOG_MAX_CHARS) {
    int cut = webLogBuffer.indexOf('\n', 500);
    if (cut < 0) {
      webLogBuffer.remove(0, webLogBuffer.length() - WEB_LOG_MAX_CHARS);
      break;
    }
    webLogBuffer.remove(0, cut + 1);
  }
}

void logLine(const String& msg) {
  Serial.println(msg);
  appendWebLog(msg + "\n");
}

void logPrint(const String& msg) {
  Serial.print(msg);
  appendWebLog(msg);
}

void logPrintf(const char* fmt, ...) {
  char buf[256];

  va_list args;
  va_start(args, fmt);
  vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);

  logLine(String(buf));
}


void handleCalibrate() {
  if (!server.hasArg("point")) {
    server.send(400, "text/plain", "Missing point");
    return;
  }

  float raw = readRawPhAdc();
  String point = server.arg("point");

  if (point == "7") {
    ph7Adc = raw;
  } else if (point == "10") {
    ph10Adc = raw;
  } else {
    server.send(400, "text/plain", "Invalid point");
    return;
  }

  savePhCalibration();
  server.sendHeader("Location", "/", true);
  server.send(302, "text/plain", "");
}

void handleSetCalibration() {
  if (server.hasArg("ph7")) ph7Adc = server.arg("ph7").toFloat();
  if (server.hasArg("ph10")) ph10Adc = server.arg("ph10").toFloat();
  if (server.hasArg("offset")) phOffset = server.arg("offset").toFloat();

  savePhCalibration();
  server.sendHeader("Location", "/", true);
  server.send(302, "text/plain", "");
}


void handleSaveReagentCalibration() {
  if (!server.hasArg("collectedMl")) {
    server.send(400, "text/plain", "Missing collectedMl");
    return;
  }

  float collectedMl = server.arg("collectedMl").toFloat();
  if (!isfinite(collectedMl) || collectedMl <= 0.0f) {
    server.send(400, "text/plain", "Invalid collected mL");
    return;
  }

  reagentMlPerMin = collectedMl * (60000.0f / (float)REAGENT_CAL_RUN_MS);
  saveReagentCalibration();

  server.sendHeader("Location", "/", true);
  server.send(302, "text/plain", "");
}

void handleTestReagentCalibration() {
  allOutputsOff("manual-reagent-cal-before");
  logLine("Manual reagent calibration: ON for " + String(REAGENT_CAL_RUN_MS / 1000UL) + " seconds");

  digitalWrite(PUMP_REAGENT, HIGH);
  webSafeDelay(REAGENT_CAL_RUN_MS);
  digitalWrite(PUMP_REAGENT, LOW);

  allOutputsOff("manual-reagent-cal-complete");
  server.sendHeader("Location", "/", true);
  server.send(302, "text/plain", "");
}


void stirrerOff(const char* reason) {
  if (digitalRead(STIRRER_PIN) != LOW) {
    logLine("Stirrer OFF: " + String(reason ? reason : "off"));
  }
  digitalWrite(STIRRER_PIN, LOW);
}

void allOutputsOff(const char* reason) {
  digitalWrite(PUMP_FILL, LOW);
  digitalWrite(PUMP_SUCK, LOW);
  digitalWrite(PUMP_REAGENT, LOW);
  digitalWrite(PUMP_FLUSH, LOW);
  digitalWrite(STIRRER_PIN, LOW);

  static unsigned long lastLogMs = 0;
  if (millis() - lastLogMs > 30000UL || (reason && strcmp(reason, "boot") == 0)) {
    lastLogMs = millis();
    logLine("All outputs OFF: " + String(reason ? reason : "safe-state"));
  }
}

void handleTestStirrer() {
  allOutputsOff("manual-stirrer-test-before");
  logLine("Manual stirrer test: ON for 5 seconds");
  digitalWrite(STIRRER_PIN, HIGH);
  webSafeDelay(5000);
  allOutputsOff("manual-test-complete");
  server.sendHeader("Location", "/", true);
  server.send(302, "text/plain", "");
}

void handleOtaPage() {
  String html;
  html.reserve(3500);
  html += F("<!doctype html><html><head><meta name='viewport' content='width=device-width,initial-scale=1'>");
  html += F("<title>OTA Firmware Update</title>");
  html += F("<style>body{font-family:Arial;margin:20px;background:#f6f8fb;color:#111}.card{background:white;border-radius:14px;padding:18px;margin:14px 0;box-shadow:0 2px 10px #0001}button{font-size:18px;padding:12px 16px;border:0;border-radius:10px;background:#1565c0;color:white;margin:8px 0}input{font-size:16px;padding:10px;width:100%;box-sizing:border-box}.warn{color:#b00020;font-weight:bold}.muted{color:#666}</style>");
  html += F("</head><body><h2>OTA Firmware Update</h2>");
  html += F("<div class='card'><p class='warn'>Local test first. Uploading the wrong .bin can stop the controller from booting.</p>");
  html += F("<p class='muted'>In PlatformIO, use the firmware file from: <br><b>.pio/build/esp32doit-devkit-v1/firmware.bin</b></p>");
  html += F("<form method='POST' action='/ota' enctype='multipart/form-data'>");
  html += F("<input type='file' name='firmware' accept='.bin' required><br><br>");
  html += F("<button type='submit'>Upload Firmware</button></form>");
  html += F("<p><a href='/'>Back to pH calibration</a></p></div>");
  html += F("</body></html>");
  server.send(200, "text/html", html);
}

void handleOtaUpload() {
  HTTPUpload& upload = server.upload();

  if (upload.status == UPLOAD_FILE_START) {
    allOutputsOff("ota-start");
    logPrintf("OTA upload started: %s", upload.filename.c_str());

    size_t updateSize = UPDATE_SIZE_UNKNOWN;
    if (!Update.begin(updateSize)) {
      Update.printError(Serial);
    }
  } else if (upload.status == UPLOAD_FILE_WRITE) {
    if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
      Update.printError(Serial);
    }
  } else if (upload.status == UPLOAD_FILE_END) {
    if (Update.end(true)) {
      logPrintf("OTA upload complete: %u bytes. Rebooting...", upload.totalSize);
    } else {
      Update.printError(Serial);
    }
  } else if (upload.status == UPLOAD_FILE_ABORTED) {
    Update.end();
    logLine("OTA upload aborted.");
  }
}

void handleOtaDone() {
  bool ok = !Update.hasError();
  String html;
  html.reserve(1500);
  html += F("<!doctype html><html><head><meta name='viewport' content='width=device-width,initial-scale=1'>");
  html += F("<title>OTA Result</title></head><body style='font-family:Arial;margin:20px'>");

  if (ok) {
    html += F("<h2>Firmware uploaded successfully</h2><p>Device is rebooting now.</p>");
    server.send(200, "text/html", html);
    delay(750);
    ESP.restart();
  } else {
    html += F("<h2>OTA failed</h2><p>Check Serial Monitor for the Update error.</p><p><a href='/ota'>Try again</a></p>");
    server.send(500, "text/html", html);
  }
}

void handleNotFound() {
  server.send(404, "text/plain", "Not found");
}

// --- Cloud and alk math ---


void webSafeDelay(unsigned long waitMs) {
  unsigned long start = millis();
  while (millis() - start < waitMs) {
    server.handleClient();
    delay(10);
    yield();
  }
}

bool hasRealServerTarget() {
  String target = String(serverTarget);
  return target.startsWith("http") && target.indexOf("your-api-endpoint") < 0;
}

void sendDataToCloud(float phValue, float alkValue) {
  if (WiFi.status() != WL_CONNECTED) {
    logLine("Error: Cannot send data, Wi-Fi disconnected.");
    return;
  }

  if (!hasRealServerTarget()) {
    logLine("Cloud send skipped: serverTarget is still the placeholder URL.");
    return;
  }

  HTTPClient http;
  http.setTimeout(5000);
  http.begin(serverTarget);
  http.addHeader("Content-Type", "application/json");

  JsonDocument jsonDoc;
  jsonDoc["ph"] = phValue;
  jsonDoc["alkalinity"] = alkValue;
  jsonDoc["device_id"] = "reef_tester_01";

  String jsonPayload;
  serializeJson(jsonDoc, jsonPayload);

  logLine("Sending JSON Payload: " + jsonPayload);

  int httpResponseCode = http.POST(jsonPayload);

  if (httpResponseCode > 0) {
    logLine("HTTP Response code: " + String(httpResponseCode));
    String response = http.getString();
    logLine(response);
  } else {
    logLine("Error code running POST request: " + String(httpResponseCode));
  }

  http.end();
}

float convertToDKH(float baseline, float dropped) {
  float pHDelta = baseline - dropped;
  return 8.4 - (pHDelta * 2.1); // Placeholder math
}
