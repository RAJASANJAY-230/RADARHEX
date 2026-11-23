#include <WiFi.h>
#include <HTTPClient.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <DFRobot_C4001.h>
#include <ESP32Servo.h>

// RADARHEX all-in-one firmware:
// C4001 mmWave + vibration + HC-SR04 + servo scanner + Wi-Fi + 0.96" OLED.

const char *WIFI_SSID = "RADAR";
const char *WIFI_PASSWORD = "12345678";
const char *TELEMETRY_URL = "http://10.238.169.12:8001/api/telemetry";
const char *DEVICE_ID = "RHX-ESP32-01";

// C4001 UART pins.
constexpr uint32_t USB_BAUD = 115200;
constexpr uint32_t RADAR_BAUD = 9600;
constexpr int RADAR_RX_PIN = 16;  // C4001 TX -> GPIO16.
constexpr int RADAR_TX_PIN = 17;  // C4001 RX -> GPIO17.

// HC-SR04, servo and vibration pins.
constexpr int ULTRASONIC_TRIG_PIN = 25;
constexpr int ULTRASONIC_ECHO_PIN = 26;  // Use 10k/20k divider.
constexpr int SERVO_PIN = 33;
constexpr int VIBRATION_PIN = 32;        // Module DO -> GPIO32 (NOT GPIO0).

// 0.96" SSD1306 OLED (128x64, I2C).
constexpr int OLED_SDA_PIN = 21;         // OLED SDA -> GPIO21
constexpr int OLED_SCL_PIN = 22;         // OLED SCL -> GPIO22
constexpr uint8_t OLED_ADDRESS = 0x3C;   // Try 0x3D if the screen stays blank.
constexpr int OLED_WIDTH = 128;
constexpr int OLED_HEIGHT = 64;
constexpr uint32_t SERVER_TIMEOUT_MS = 3000;  // No server reply for 3 s = link lost.

constexpr int RIGHT_ANGLE = 30;
constexpr int CENTRE_ANGLE = 90;
constexpr int LEFT_ANGLE = 150;
constexpr float OBSTACLE_LIMIT_CM = 30.0f;
constexpr uint32_t ECHO_TIMEOUT_US = 30000;
constexpr uint32_t TELEMETRY_INTERVAL_MS = 500;
constexpr uint32_t WIFI_RETRY_MS = 5000;
constexpr uint32_t RADAR_RETRY_MS = 2000;

// ---- Vibration settings ----
// Level the DO pin goes to WHEN vibration happens.
// Most SW-420 modules: HIGH on vibration. If yours is inverted, use LOW.
constexpr int VIB_ACTIVE_LEVEL = HIGH;
constexpr uint32_t VIBRATION_HOLD_MS = 2000;  // how long the alert stays on
constexpr uint32_t VIB_DEBOUNCE_MS = 30;      // ignore edges closer than this
constexpr uint32_t VIB_SERVO_BLANK_MS = 600;  // ignore sensor after servo moves
constexpr uint32_t VIB_BOOT_SETTLE_MS = 1500; // ignore edges right after boot

DFRobot_C4001_UART radar(&Serial1, RADAR_BAUD, RADAR_RX_PIN, RADAR_TX_PIN);
Servo scannerServo;
Adafruit_SSD1306 display(OLED_WIDTH, OLED_HEIGHT, &Wire, -1);

bool radarOnline = false;
bool oledOnline = false;
uint32_t lastRadarAttemptAt = 0;
uint32_t lastWiFiAttemptAt = 0;
uint32_t lastTelemetryAt = 0;

// Server link tracking (drives the OLED status line).
bool serverEverOk = false;
uint32_t lastServerOkAt = 0;

// Latest radar result for the OLED.
bool personDetected = false;
float personRadius = -1.0f;   // metres, C4001 range; -1 when no person

enum LinkState { LINK_WAITING, LINK_CONNECTED, LINK_DISCONNECTED };

float centreDistance = -1.0f;
float leftDistance = -1.0f;
float rightDistance = -1.0f;
bool obstacleDetected = false;
int servoAngle = CENTRE_ANGLE;
String selectedDirection = "CENTRE";

// Vibration state.
bool vibrationRaw = false;
uint32_t lastVibrationAt = 0;
volatile bool vibEvent = false;
volatile uint32_t vibLastEdgeMs = 0;
volatile uint32_t vibIgnoreUntilMs = 0;

void IRAM_ATTR onVibrationEdge() {
  const uint32_t now = millis();
  if ((int32_t)(now - vibIgnoreUntilMs) < 0) return;      // servo/boot blanking
  if (now - vibLastEdgeMs < VIB_DEBOUNCE_MS) return;      // debounce
  vibLastEdgeMs = now;
  vibEvent = true;                                        // one pulse = one event
}

void blankVibration(uint32_t ms) {
  vibIgnoreUntilMs = millis() + ms;
}

void updateVibrationSensor() {
  vibrationRaw = digitalRead(VIBRATION_PIN) == VIB_ACTIVE_LEVEL;

  if (vibEvent) {
    vibEvent = false;
    lastVibrationAt = millis();
    Serial.println("VIBRATION DETECTED");
  }
}

void beginWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  lastWiFiAttemptAt = millis();
}

void maintainWiFi() {
  if (WiFi.status() == WL_CONNECTED) return;
  if (millis() - lastWiFiAttemptAt < WIFI_RETRY_MS) return;

  WiFi.disconnect();
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  lastWiFiAttemptAt = millis();
}

void connectRadar() {
  if (radarOnline) return;
  if (lastRadarAttemptAt != 0 &&
      millis() - lastRadarAttemptAt < RADAR_RETRY_MS) {
    return;
  }

  lastRadarAttemptAt = millis();
  if (!radar.begin()) {
    radarOnline = false;
    return;
  }

  radarOnline = true;
  radar.setSensorMode(eSpeedMode);
  delay(250);
  radar.setFrettingDetection(eON);
}

float readUltrasonicOnce() {
  digitalWrite(ULTRASONIC_TRIG_PIN, LOW);
  delayMicroseconds(3);
  digitalWrite(ULTRASONIC_TRIG_PIN, HIGH);
  delayMicroseconds(10);
  digitalWrite(ULTRASONIC_TRIG_PIN, LOW);

  const uint32_t duration =
      pulseIn(ULTRASONIC_ECHO_PIN, HIGH, ECHO_TIMEOUT_US);
  if (duration == 0) return -1.0f;
  return duration * 0.0343f / 2.0f;
}

float readFilteredUltrasonic() {
  float readings[3];
  int count = 0;

  for (int i = 0; i < 3; i++) {
    const float distance = readUltrasonicOnce();
    if (distance >= 2.0f && distance <= 400.0f) {
      readings[count++] = distance;
    }
    delay(30);
  }

  if (count == 0) return -1.0f;

  for (int i = 1; i < count; i++) {
    const float value = readings[i];
    int j = i - 1;
    while (j >= 0 && readings[j] > value) {
      readings[j + 1] = readings[j];
      j--;
    }
    readings[j + 1] = value;
  }
  return readings[count / 2];
}

float pointAndMeasure(int angle) {
  servoAngle = angle;
  blankVibration(400 + VIB_SERVO_BLANK_MS);  // covers the move + settle
  scannerServo.write(angle);
  delay(400);
  const float d = readFilteredUltrasonic();
  blankVibration(VIB_SERVO_BLANK_MS);
  return d;
}

float clearanceScore(float distance) {
  return distance < 0 ? 401.0f : distance;
}

void runObstacleScanner() {
  selectedDirection = "CENTRE";
  leftDistance = -1.0f;
  rightDistance = -1.0f;
  centreDistance = pointAndMeasure(CENTRE_ANGLE);
  obstacleDetected =
      centreDistance >= 0 && centreDistance <= OBSTACLE_LIMIT_CM;

  if (!obstacleDetected) return;

  Serial.println("OBSTACLE DETECTED - scanning left and right");
  leftDistance = pointAndMeasure(LEFT_ANGLE);
  rightDistance = pointAndMeasure(RIGHT_ANGLE);

  if (clearanceScore(leftDistance) >= clearanceScore(rightDistance)) {
    selectedDirection = "LEFT";
    servoAngle = LEFT_ANGLE;
  } else {
    selectedDirection = "RIGHT";
    servoAngle = RIGHT_ANGLE;
  }

  blankVibration(350 + VIB_SERVO_BLANK_MS);
  scannerServo.write(servoAngle);
  delay(350);
  blankVibration(VIB_SERVO_BLANK_MS);
}

void appendNullableDistance(String &json, const char *name, float value) {
  json += ",\"";
  json += name;
  json += "\":";
  if (value < 0) {
    json += "null";
  } else {
    json += String(value, 1);
  }
}

String buildTelemetry() {
  const uint8_t targetCount = radarOnline ? radar.getTargetNumber() : 0;
  const bool humanDetected = radarOnline && targetCount > 0;
  const bool vibrationDetected =
      lastVibrationAt != 0 && millis() - lastVibrationAt <= VIBRATION_HOLD_MS;

  personDetected = humanDetected;
  personRadius = humanDetected ? radar.getTargetRange() : -1.0f;

  String json;
  json.reserve(480);
  json = "{\"deviceId\":\"";
  json += DEVICE_ID;
  json += "\",\"sensorOnline\":";
  json += radarOnline ? "true" : "false";
  json += ",\"detected\":";
  json += humanDetected ? "true" : "false";
  json += ",\"targets\":";
  json += String(humanDetected ? targetCount : 0);

  if (humanDetected) {
    json += ",\"distance\":";
    json += String(radar.getTargetRange(), 3);
    json += ",\"speed\":";
    json += String(radar.getTargetSpeed(), 3);
    json += ",\"energy\":";
    json += String(radar.getTargetEnergy());
  } else {
    json += ",\"distance\":null,\"speed\":null,\"energy\":0";
  }

  json += ",\"wifiRssi\":";
  if (WiFi.status() == WL_CONNECTED) {
    json += String(WiFi.RSSI());
  } else {
    json += "null";
  }

  json += ",\"ultrasonicOnline\":true";
  json += ",\"obstacleDetected\":";
  json += obstacleDetected ? "true" : "false";
  appendNullableDistance(json, "ultrasonicDistance", centreDistance);
  appendNullableDistance(json, "leftDistance", leftDistance);
  appendNullableDistance(json, "rightDistance", rightDistance);
  json += ",\"scanDirection\":\"";
  json += selectedDirection;
  json += "\",\"servoAngle\":";
  json += String(servoAngle);
  json += ",\"vibrationOnline\":true";
  json += ",\"vibrationDetected\":";
  json += vibrationDetected ? "true" : "false";
  json += ",\"vibrationRaw\":";
  json += vibrationRaw ? "1" : "0";
  json += "}";
  return json;
}

void transmitTelemetry(const String &payload) {
  Serial.println(payload);
  if (WiFi.status() != WL_CONNECTED) return;

  HTTPClient http;
  http.setTimeout(1200);
  http.begin(TELEMETRY_URL);
  http.addHeader("Content-Type", "application/json");
  const int responseCode = http.POST(payload);
  http.end();

  if (responseCode >= 200 && responseCode < 300) {
    serverEverOk = true;
    lastServerOkAt = millis();
  }
  if (responseCode <= 0) {
    Serial.println("{\"wifiPost\":false}");
  }
}

LinkState currentLinkState() {
  if (WiFi.status() != WL_CONNECTED) return LINK_DISCONNECTED;
  if (serverEverOk && millis() - lastServerOkAt <= SERVER_TIMEOUT_MS) {
    return LINK_CONNECTED;
  }
  if (serverEverOk) return LINK_DISCONNECTED;  // Server was reachable, now lost.
  return LINK_WAITING;                          // Still making first contact.
}

// Large text when it fits on one 128 px line, otherwise the small font.
void drawHero(const char *text) {
  const uint8_t size = (strlen(text) * 12 <= OLED_WIDTH) ? 2 : 1;
  const int16_t width = strlen(text) * 6 * size;
  display.setTextSize(size);
  display.setCursor((OLED_WIDTH - width) / 2, size == 2 ? 14 : 18);
  display.print(text);
}

void updateOled() {
  if (!oledOnline) return;

  const LinkState link = currentLinkState();
  const char *linkText = link == LINK_CONNECTED ? "CONNECTED"
                         : link == LINK_WAITING ? "WAITING"
                                                : "DISCONNECTED";

  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);

  // Row 1: title and divider.
  display.setTextSize(1);
  display.setCursor(0, 0);
  display.print("RADARHEX");
  display.drawFastHLine(0, 10, OLED_WIDTH, SSD1306_WHITE);

  // Row 2: main status (person state only when the link is up).
  if (link != LINK_CONNECTED) {
    drawHero(linkText);                 // WAITING or DISCONNECTED
  } else if (!radarOnline) {
    drawHero("WAITING");                // Radar retrying every 2 s.
  } else if (personDetected) {
    drawHero("DETECTED");
  } else {
    drawHero("NO PERSON");
  }

  // Row 3: radius.
  display.setTextSize(1);
  display.setCursor(0, 36);
  if (link == LINK_CONNECTED && radarOnline && personDetected && personRadius >= 0) {
    display.print("Radius: ");
    display.print(personRadius, 2);
    display.print(" m");
  } else {
    display.print("Radius: --");
  }

  // Row 4: radar state.
  display.setCursor(0, 46);
  display.print("Radar: ");
  display.print(radarOnline ? "OK" : "DISCONNECTED");

  // Row 5: server state.
  display.setCursor(0, 56);
  display.print("Server: ");
  display.print(linkText);

  display.display();
}

void setup() {
  Serial.begin(USB_BAUD);

  pinMode(ULTRASONIC_TRIG_PIN, OUTPUT);
  pinMode(ULTRASONIC_ECHO_PIN, INPUT);
  digitalWrite(ULTRASONIC_TRIG_PIN, LOW);

  // Pull the pin to its IDLE level so a loose/floating wire can't false-trigger.
  pinMode(VIBRATION_PIN,
          VIB_ACTIVE_LEVEL == HIGH ? INPUT_PULLDOWN : INPUT_PULLUP);

  // OLED on the ESP32 default I2C pins (21/22). If the screen is missing,
  // the rest of the firmware still runs; only the display is skipped.
  Wire.begin(OLED_SDA_PIN, OLED_SCL_PIN);
  oledOnline = display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDRESS);
  if (oledOnline) {
    display.clearDisplay();
    display.display();
  } else {
    Serial.println("WARNING: OLED not found at 0x3C. Check SDA=GPIO21, SCL=GPIO22.");
  }

  scannerServo.setPeriodHertz(50);
  scannerServo.attach(SERVO_PIN, 500, 2400);
  scannerServo.write(CENTRE_ANGLE);

  delay(800);  // let the servo reach centre and settle

  // Ignore anything during boot, then arm the interrupt.
  blankVibration(VIB_BOOT_SETTLE_MS);
  delay(300);
  if (digitalRead(VIBRATION_PIN) == VIB_ACTIVE_LEVEL) {
    Serial.println("WARNING: vibration pin is at its ACTIVE level while still. "
                   "Turn the sensitivity pot counter-clockwise, or set "
                   "VIB_ACTIVE_LEVEL to the opposite level.");
  }
  attachInterrupt(digitalPinToInterrupt(VIBRATION_PIN), onVibrationEdge,
                  VIB_ACTIVE_LEVEL == HIGH ? RISING : FALLING);

  connectRadar();
  beginWiFi();
  Serial.println("RADARHEX all-in-one controller started");
}

void loop() {
  maintainWiFi();
  connectRadar();
  updateVibrationSensor();

  if (millis() - lastTelemetryAt < TELEMETRY_INTERVAL_MS) return;
  lastTelemetryAt = millis();

  runObstacleScanner();
  updateVibrationSensor();
  transmitTelemetry(buildTelemetry());
  updateOled();
}