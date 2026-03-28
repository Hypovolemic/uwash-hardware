#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoOTA.h>

// WiFi credentials
const char* WIFI_SSID     = "shiras 24 ultra";
const char* WIFI_PASSWORD  = "test1234";

// uwash-bot server address (the machine running the bot)
// e.g. "http://192.168.1.100:5001" or your deployed server URL
const char* SERVER_URL = "https://web-production-869a0.up.railway.app/machine/update";

// API key (must match SENSOR_API_KEY in the server's .env file)
// Leave empty "" if the server has no SENSOR_API_KEY set
const char* API_KEY = "acaciahackathon12345";

// Which machine is this sensor attached to?
// Valid houses: "ROC", "Dragon", "Garuda", "Phoenix", "Tulpar"
// Valid machines: "Dryer One", "Dryer Two", "Washer One", "Washer Two"
const char* HOUSE        = "ROC";
const char* MACHINE_NAME = "Washer One";
const char* OTA_HOSTNAME = "uwash-sensor-01";
const char* OTA_PASSWORD = "uwash123";

#define VIBRATION_PIN  4    // GPIO connected to SW420 DO pin

// Detection parameters (tune these for your washing machine)
#define SAMPLE_INTERVAL_MS      100    // How often to read the sensor (ms)
#define SAMPLES_WINDOW          30     // Number of samples in the rolling window (30 x 100ms = 3 sec)
#define VIBRATION_THRESHOLD     10     // Out of 30 samples, >=10 HIGH = vibrating
#define STABLE_COUNT_ON         3      // Consecutive windows above threshold to confirm "in_use"
#define STABLE_COUNT_OFF        4     // Consecutive windows below threshold to confirm "available"
                                       // 20 x 3sec = ~60sec of no vibration before marking available
#define STATUS_RESEND_INTERVAL  300000 // Resend current status every 5 minutes as heartbeat (ms)

int vibrationSamples[SAMPLES_WINDOW];
int sampleIndex = 0;
int consecutiveOn  = 0;
int consecutiveOff = 0;
bool machineInUse = false;
bool lastReportedInUse = false;
unsigned long lastSampleTime = 0;
unsigned long lastSendTime = 0;
bool wifiConnected = false;

void setupOTA() {
  ArduinoOTA.setHostname(OTA_HOSTNAME);
  ArduinoOTA.setPassword(OTA_PASSWORD);
  
  ArduinoOTA.onStart([]() {
    Serial.println("[OTA] Update starting...");
  });
  
  ArduinoOTA.onEnd([]() {
    Serial.println("\n[OTA] Update complete! Rebooting...");
  });
  
  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    Serial.printf("[OTA] Progress: %u%%\r", (progress * 100) / total);
  });
  
  ArduinoOTA.onError([](ota_error_t error) {
    Serial.printf("[OTA] Error[%u]: ", error);
    if (error == OTA_AUTH_ERROR) Serial.println("Auth Failed");
    else if (error == OTA_BEGIN_ERROR) Serial.println("Begin Failed");
    else if (error == OTA_CONNECT_ERROR) Serial.println("Connect Failed");
    else if (error == OTA_RECEIVE_ERROR) Serial.println("Receive Failed");
    else if (error == OTA_END_ERROR) Serial.println("End Failed");
  });
  
  ArduinoOTA.begin();
  Serial.printf("[OTA] Ready! Hostname: %s\n", OTA_HOSTNAME);
  Serial.printf("[OTA] IP: %s\n", WiFi.localIP().toString().c_str());
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  
  pinMode(VIBRATION_PIN, INPUT);
  
  // Initialize sample buffer
  memset(vibrationSamples, 0, sizeof(vibrationSamples));
  
  Serial.println("\n=================================");
  Serial.println("  UWash Vibration Sensor v1.0");
  Serial.printf("  House: %s\n", HOUSE);
  Serial.printf("  Machine: %s\n", MACHINE_NAME);
  Serial.println("=================================\n");
  
  connectWiFi();
  setupOTA();
}

void loop() {
  ArduinoOTA.handle();
  // Reconnect WiFi if dropped
  if (WiFi.status() != WL_CONNECTED) {
    wifiConnected = false;
    connectWiFi();
  }
  
  // Sample vibration sensor at regular intervals
  unsigned long now = millis();
  if (now - lastSampleTime >= SAMPLE_INTERVAL_MS) {
    lastSampleTime = now;
    
    // Read sensor and store in rolling buffer
    vibrationSamples[sampleIndex] = digitalRead(VIBRATION_PIN);
    sampleIndex = (sampleIndex + 1) % SAMPLES_WINDOW;
    
    // Only evaluate when we've filled a complete window
    if (sampleIndex == 0) {
      int vibrationCount = 0;
      for (int i = 0; i < SAMPLES_WINDOW; i++) {
        vibrationCount += vibrationSamples[i];
      }
      
      bool currentlyVibrating = (vibrationCount >= VIBRATION_THRESHOLD);
      
      Serial.printf("[Sensor] Vibrations: %d/%d %s\n", 
                    vibrationCount, SAMPLES_WINDOW,
                    currentlyVibrating ? ">> VIBRATING" : "   quiet");
      
      // State machine with hysteresis
      if (currentlyVibrating) {
        consecutiveOff = 0;
        consecutiveOn++;
        if (consecutiveOn >= STABLE_COUNT_ON && !machineInUse) {
          machineInUse = true;
          Serial.println("[State] Machine is now IN USE");
          sendStatus("in_use");
        }
      } else {
        consecutiveOn = 0;
        consecutiveOff++;
        if (consecutiveOff >= STABLE_COUNT_OFF && machineInUse) {
          machineInUse = false;
          Serial.println("[State] Machine is now AVAILABLE");
          sendStatus("available");
        }
      }
    }
  }
  
  // Periodic heartbeat resend
  if (now - lastSendTime >= STATUS_RESEND_INTERVAL && lastSendTime > 0) {
    Serial.println("[Heartbeat] Resending current status...");
    sendStatus(machineInUse ? "in_use" : "available");
  }
}

void connectWiFi() {
  Serial.printf("[WiFi] Connecting to %s", WIFI_SSID);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 40) {
    delay(500);
    Serial.print(".");
    attempts++;
  }
  
  if (WiFi.status() == WL_CONNECTED) {
    wifiConnected = true;
    Serial.printf("\n[WiFi] Connected! IP: %s\n", WiFi.localIP().toString().c_str());
  } else {
    Serial.println("\n[WiFi] FAILED - will retry on next loop");
  }
}

void sendStatus(const char* status) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[API] No WiFi - skipping send");
    return;
  }
  
  HTTPClient http;
  http.begin(SERVER_URL);
  http.addHeader("Content-Type", "application/json");
  
  // Add API key if configured
  if (strlen(API_KEY) > 0) {
    http.addHeader("X-API-Key", API_KEY);
  }
  
  // Build JSON payload
  String payload = "{";
  payload += "\"house\":\"" + String(HOUSE) + "\",";
  payload += "\"machine_name\":\"" + String(MACHINE_NAME) + "\",";
  payload += "\"status\":\"" + String(status) + "\"";
  payload += "}";
  
  Serial.printf("[API] POST %s\n", SERVER_URL);
  Serial.printf("[API] Body: %s\n", payload.c_str());
  
  int httpCode = http.POST(payload);
  
  if (httpCode > 0) {
    String response = http.getString();
    Serial.printf("[API] Response (%d): %s\n", httpCode, response.c_str());
    lastSendTime = millis();
    lastReportedInUse = (strcmp(status, "in_use") == 0);
  } else {
    Serial.printf("[API] ERROR: %s\n", http.errorToString(httpCode).c_str());
  }
  
  http.end();
}
