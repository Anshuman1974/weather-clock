/*
 * WiFi Smart Clock - ESP8266 + MAX7219 LED Matrix
 * Supports 4 modules (8x32) and 8 modules (8x64) exclusively
 * Distribution prohibited for commercial use - Copyright Protected
 * 
 * FIX: Scroll speed now works correctly - 10 is fastest, 100 is slowest
 * DEFAULT: 8 modules, 80% brightness, medium scroll speed (60)
 * FIX: IP address always scrolls at medium speed (60)
 */

#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <DNSServer.h>
#include <WiFiManager.h>
#include <NTPClient.h>
#include <WiFiUdp.h>
#include <TimeLib.h>
#include <MD_Parola.h>
#include <MD_MAX72xx.h>
#include <FS.h>
#include <ArduinoJson.h>
#include <ESP8266HTTPClient.h>

// ==================== Hardware Configuration ====================
#define HARDWARE_TYPE MD_MAX72XX::FC16_HW
#define CLK_PIN   14  // D5 on ESP8266
#define DATA_PIN  13  // D7 on ESP8266
#define CS_PIN    15  // D8 on ESP8266

// ==================== Configuration Management ====================
struct Config {
  int numModules;          // 4 or 8
  int scrollSpeed;         // 10 - 100 (10=fastest, 100=slowest)
  int displayIntensity;    // 0 - 100
  bool use24Hour;
  bool showDate;
  bool showDay;
  bool enableWeather;
  String openWeatherMapApiKey;
  String locationId;       
  float utcOffsetHours;
  bool useDst;             
  bool showCurrentTemp;    
  bool showMinMax;
  bool showHumidity;       
  bool showCondition;      
  bool useFadeEffect;
};

Config config;
ESP8266WebServer server(80);
MD_Parola* P = nullptr; 

// ==================== Time and Weather Variables ====================
WiFiUDP ntpUDP;
NTPClient* timeClient = nullptr;

unsigned long lastWeatherUpdate = 0;
const unsigned long weatherInterval = 1200000; // 20 minutes
unsigned long lastNTPUpdate = 0;

// Weather cache
float currentTemp = 0.0;
float minTemp = 0.0;
float maxTemp = 0.0;
int humidity = 0;
String weatherCondition = "";
bool weatherValid = false;

// Rotational View Enums
enum DisplayMode {
  MODE_TIME,
  MODE_4MOD_DAY,
  MODE_4MOD_DATE,
  MODE_8MOD_COMBINED,
  MODE_CURRENT_TEMP,
  MODE_MIN_MAX_TEMP,
  MODE_HUMIDITY,
  MODE_CONDITION
};
int loopSequenceIndex = 0;
DisplayMode activeMode = MODE_TIME;

#define MAX_MSG_LEN 75
char displayMessage[MAX_MSG_LEN] = "";
bool portalActive = false;
bool ipScrollDone = false; // Flag to ensure IP address only scrolls once at startup
WiFiManager wifiManager;

// ==================== Helper Function: Convert Scroll Speed ====================
// User sees: 10=fastest, 100=slowest
// MD_Parola expects: lower number = faster, higher number = slower
// So we invert: 10->100 (fastest), 100->10 (slowest)
int getInvertedSpeed() {
  // Map from user range (10-100) to internal range (100-10)
  return map(config.scrollSpeed, 10, 100, 100, 10);
}

// ==================== Date/Time String Format Generators ====================
String getDayStringShort() {
  String daysShort[] = {"SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT"};
  time_t localTime = now();
  return daysShort[dayOfWeek(localTime) - 1];
}

String getDateStringLong() {
  String monthNames[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
  time_t localTime = now();
  return String(day(localTime)) + " " + monthNames[month(localTime) - 1];
}

String getFormattedTime() {
  time_t localTime = now();
  char timeBuf[16];
  int hr = hour(localTime);
  
  if (!config.use24Hour) {
    if (hr == 0) hr = 12;
    else if (hr > 12) hr -= 12;
    
    if (config.numModules <= 4) {
      snprintf(timeBuf, sizeof(timeBuf), "%02d:%02d", hr, minute(localTime));
    } else {
      String ampm = (hour(localTime) >= 12) ? " PM" : " AM";
      snprintf(timeBuf, sizeof(timeBuf), "%02d:%02d%s", hr, minute(localTime), ampm.c_str());
    }
  } else {
    snprintf(timeBuf, sizeof(timeBuf), "%02d:%02d", hr, minute(localTime));
  }
  return String(timeBuf);
}

void startScrollingMessage(String msg) {
  if (P == nullptr) return;
  msg.toUpperCase();
  strncpy(displayMessage, msg.c_str(), MAX_MSG_LEN - 1);
  displayMessage[MAX_MSG_LEN - 1] = '\0';
  
  textEffect_t effect = config.useFadeEffect ? PA_FADE : PA_SCROLL_LEFT;
  uint16_t pauseTime = config.useFadeEffect ? 2500 : 0; 
  
  // FIXED: Use inverted speed so user sees 10=fastest, 100=slowest
  P->displayText(displayMessage, PA_CENTER, getInvertedSpeed(), pauseTime, effect, effect);
  P->displayReset();
}

// Static center text utility for solid text status layout display
void displayStaticText(String msg) {
  if (P == nullptr) return;
  msg.toUpperCase();
  strncpy(displayMessage, msg.c_str(), MAX_MSG_LEN - 1);
  displayMessage[MAX_MSG_LEN - 1] = '\0';
  P->displayText(displayMessage, PA_CENTER, 0, 0, PA_PRINT, PA_PRINT);
  P->displayAnimate();
}

// Function to perform a complete, clean blocking scroll of the IP address string
void scrollIPBlocking() {
  if (P == nullptr) return;
  String ipStr = "IP: " + WiFi.localIP().toString();
  ipStr.toUpperCase();
  strncpy(displayMessage, ipStr.c_str(), MAX_MSG_LEN - 1);
  displayMessage[MAX_MSG_LEN - 1] = '\0';

  // FIXED: IP address always scrolls at medium speed (60)
  // Medium speed = 60, which maps to internal speed 60 (since 60 maps to 60 in our inversion)
  int ipSpeed = 60;
  P->displayText(displayMessage, PA_CENTER, ipSpeed, 0, PA_SCROLL_LEFT, PA_SCROLL_LEFT);
  P->displayReset();
  
  // Force loop until the text finishes exiting the screen bounds entirely
  while (!P->displayAnimate()) {
    yield();
    ESP.wdtFeed();
  }
  P->displayClear();
  ipScrollDone = true;
}

// ==================== Data Synchronizer ====================
void updateWeatherAndTime() {
  long targetOffsetSeconds = (config.utcOffsetHours * 3600) + (config.useDst ? 3600 : 0);

  if (timeClient) {
    Serial.println(F("Syncing NTP Time..."));
    timeClient->setTimeOffset(targetOffsetSeconds);
    if(timeClient->forceUpdate()) {
       setTime(timeClient->getEpochTime());
       lastNTPUpdate = millis();
    }
  }

  if (!config.enableWeather || config.openWeatherMapApiKey.length() < 10 || config.locationId.length() < 2) {
    weatherValid = false;
    return;
  }

  WiFiClient client;
  HTTPClient http;
  String url = "http://api.openweathermap.org/data/2.5/weather?id=" + config.locationId + "&appid=" + config.openWeatherMapApiKey + "&units=metric";
  
  if (http.begin(client, url)) {
    int httpCode = http.GET();
    if (httpCode == HTTP_CODE_OK) {
      String payload = http.getString();
      StaticJsonDocument<1024> doc;
      DeserializationError error = deserializeJson(doc, payload);
      
      if (!error) {
        currentTemp = doc["main"]["temp"] | 0.0;
        minTemp = doc["main"]["temp_min"] | 0.0;
        maxTemp = doc["main"]["temp_max"] | 0.0;
        humidity = doc["main"]["humidity"] | 0;
        const char* cond = doc["weather"][0]["main"] | "";
        weatherCondition = String(cond);
        weatherValid = true;
        lastWeatherUpdate = millis();
      } else {
        weatherValid = false;
      }
    }
    http.end();
  }
  client.stop();
}

// ==================== Configuration Persistence Storage ====================
void loadConfiguration() {
  // ===== CHANGED: Factory defaults =====
  config.numModules = 8;           // Default to 8 modules (8x64 display)
  config.scrollSpeed = 60;         // Medium scroll speed (60)
  config.displayIntensity = 80;    // 80% brightness
  config.use24Hour = true;
  config.showDate = true;
  config.showDay = true;
  config.enableWeather = false;
  config.openWeatherMapApiKey = "";
  config.locationId = "2073124"; 
  config.utcOffsetHours = 9.5;
  config.useDst = false;
  config.showCurrentTemp = true;
  config.showMinMax = true;
  config.showHumidity = true;
  config.showCondition = true;
  config.useFadeEffect = false;

  if (!SPIFFS.begin()) return;
  if (!SPIFFS.exists("/config.json")) return;

  File configFile = SPIFFS.open("/config.json", "r");
  if (!configFile) return;

  StaticJsonDocument<768> doc;
  DeserializationError error = deserializeJson(doc, configFile);
  configFile.close();

  if (!error) {
    config.numModules = doc["numModules"] | 8;        // Default to 8
    config.scrollSpeed = doc["scrollSpeed"] | 60;     // Default to medium
    config.displayIntensity = doc["displayIntensity"] | 80;  // Default to 80%
    config.use24Hour = doc["use24Hour"] | true;
    config.showDate = doc["showDate"] | true;
    config.showDay = doc["showDay"] | true;
    config.enableWeather = doc["enableWeather"] | false;
    config.openWeatherMapApiKey = doc["apiKey"] | "";
    config.locationId = doc["locationId"] | "2073124";
    config.utcOffsetHours = doc["utcOffset"] | 9.5;
    config.useDst = doc["useDst"] | false;
    config.showCurrentTemp = doc["showCurrentTemp"] | true;
    config.showMinMax = doc["showMinMax"] | true;
    config.showHumidity = doc["showHumidity"] | true;
    config.showCondition = doc["showCondition"] | true;
    config.useFadeEffect = doc["useFadeEffect"] | false;
  }
}

void saveConfiguration() {
  File configFile = SPIFFS.open("/config.json", "w");
  if (!configFile) return;

  StaticJsonDocument<768> doc;
  doc["numModules"] = config.numModules;
  doc["scrollSpeed"] = config.scrollSpeed;
  doc["displayIntensity"] = config.displayIntensity;
  doc["use24Hour"] = config.use24Hour;
  doc["showDate"] = config.showDate;
  doc["showDay"] = config.showDay;
  doc["enableWeather"] = config.enableWeather;
  doc["apiKey"] = config.openWeatherMapApiKey;
  doc["locationId"] = config.locationId;
  doc["utcOffset"] = config.utcOffsetHours;
  doc["useDst"] = config.useDst;
  doc["showCurrentTemp"] = config.showCurrentTemp;
  doc["showMinMax"] = config.showMinMax;
  doc["showHumidity"] = config.showHumidity;
  doc["showCondition"] = config.showCondition;
  doc["useFadeEffect"] = config.useFadeEffect;

  serializeJson(doc, configFile);
  configFile.close();
}

// ==================== Web Control Panel Server Pages ====================
void handleRootPage() {
  String html = F("<!DOCTYPE html><html><head><meta name='viewport' content='width=device-width, initial-scale=1.0'><title>Smart Clock Control Panel</title>");
  html += F("<style>body{font-family:sans-serif;margin:20px;background:#f0f0f0;} .card{background:white;padding:20px;border-radius:8px;box-shadow:0 2px 4px rgba(0,0,0,0.1);margin-bottom:20px;} input,select{width:100%;padding:8px;margin:8px 0;box-sizing:border-box;} input[type=checkbox]{width:auto;margin-right:10px;} .btn{background:#4CAF50;color:white;border:none;cursor:pointer;padding:10px;text-align:center;text-decoration:none;display:inline-block;font-size:14px;border-radius:4px;margin:5px 2px;} .btn-warn{background:#ff9800;} .btn-danger{background:#f44336;} .btn-sync{background:#008CBA;} .help-link{font-size:12px;color:#008CBA;text-decoration:none;display:inline-block;margin-bottom:8px;} .help-link:hover{text-decoration:underline;} .footer-text{text-align:center;font-size:12px;color:#777;margin-top:30px;border-top:1px solid #ccc;padding-top:15px;line-height:1.6;} .grid-container{display:grid;grid-template-columns:1fr 1fr;gap:10px;} .grid-item{background:#f9f9f9;padding:8px;border-radius:4px;border:1px solid #eee;font-size:13px;}</style></head><body>");
  html += F("<h2>Smart Clock Settings Panel</h2>");
  
  html += F("<div class='card' style='border-left: 5px solid #008CBA;'><h3>Live System & Internet Status</h3>");
  html += F("<div class='grid-container'>");
  html += "<div class='grid-item'><b>Current Synchronized Time:</b> " + getFormattedTime() + " (" + getDayStringShort() + " " + getDateStringLong() + ")</div>";
  html += "<div class='grid-item'><b>Display Setup Scale:</b> " + String(config.numModules) + " Modules (" + String(config.numModules * 8) + "x8 Matrix)</div>";
  html += "<div class='grid-item'><b>Active Baseline Timezone:</b> UTC " + String(config.utcOffsetHours, 1) + " Hours</div>";
  html += "<div class='grid-item'><b>Daylight Saving Time (DST):</b> " + String(config.useDst ? "<span style='color:green;'>Active (+1 Hour)</span>" : "<span style='color:red;'>Inactive</span>") + "</div>";
  
  if (config.enableWeather) {
    html += "<div class='grid-item'><b>Internet Weather Sync:</b> " + String(weatherValid ? "<span style='color:green;'>Connected & Valid</span>" : "<span style='color:orange;'>Awaiting Data / Error</span>") + "</div>";
    html += "<div class='grid-item'><b>Live Temperature Map:</b> Now: " + String(currentTemp, 1) + "C (Min: " + String(minTemp, 0) + "C / Max: " + String(maxTemp, 0) + "C)</div>";
    html += "<div class='grid-item'><b>Ambient Humidity Profile:</b> " + String(humidity) + "% Relative</div>";
    html += "<div class='grid-item'><b>Condition Index:</b> " + (weatherCondition.length() > 0 ? weatherCondition : "N/A") + "</div>";
  } else {
    html += "<div class='grid-item' style='grid-column: span 2; text-align:center; color:#888;'>Weather processing modules are globally disabled.</div>";
  }
  html += F("</div></div>");

  html += F("<div class='card'><form action='/save' method='POST'>");
  html += "Hardware Setup Size: <select name='modules'>";
  html += "<option value='4'" + String(config.numModules==4?" selected":"") + ">4 Modules (8x32 Layout)</option>";
  html += "<option value='8'" + String(config.numModules==8?" selected":"") + ">8 Modules (8x64 Layout)</option></select><br>";
  
  html += "Transition Style: <select name='fadeeffect'>";
  html += "<option value='0'" + String(!config.useFadeEffect?" selected":"") + ">Scrolling Text</option>";
  html += "<option value='1'" + String(config.useFadeEffect?" selected":"") + ">Fade In / Fade Out</option></select><br>";

  // FIXED: Updated help text to show correct speed mapping
  html += "Scroll Speed Recommended-50 (10=fastest, 60=medium, 100=slowest), Fade Speed Recommended-100 (10=slower, 60=medium, 100=faster): <input type='number' name='speed' value='" + String(config.scrollSpeed) + "' min='10' max='100'><br>";
  html += "Brightness Level (0-100): <input type='number' name='intensity' value='" + String(config.displayIntensity) + "'><br>";
  
  html += "Clock Format: <select name='format24'>";
  html += "<option value='1'" + String(config.use24Hour?" selected":"") + ">24-Hour Format</option>";
  html += "<option value='0'" + String(!config.use24Hour?" selected":"") + ">12-Hour Format (AM/PM Indicators)</option></select><br>";

  html += "<label><input type='checkbox' name='showday' value='1'" + String(config.showDay?" checked":"") + ">Enable Day Component</label><br>";
  html += "<label><input type='checkbox' name='showdate' value='1'" + String(config.showDate?" checked":"") + ">Enable Date Component</label><br><hr>";
  
  html += "Enable Weather Component: <input type='checkbox' name='enweather' value='1'" + String(config.enableWeather?" checked":"") + "><br>";
  html += "Weather API Key: <input type='text' name='apikey' value='" + config.openWeatherMapApiKey + "'><br>";
  html += F("<a href='https://openweathermap.org/appid' target='_blank' class='help-link'>&rarr; How to obtain your Weather API Key</a><br>");
  
  html += "Location ID (City ID): <input type='text' name='locationid' value='" + config.locationId + "' placeholder='e.g. 2073124'><br>";
  html += F("<a href='https://openweathermap.org/current#builtin' target='_blank' class='help-link'>&rarr; How to look up your City Location ID</a><br>");
  
  html += "Time Zone UTC Offset: <input type='number' step='0.5' name='utcoffset' value='" + String(config.utcOffsetHours, 1) + "'><br>";
  html += "<label><input type='checkbox' name='usedst' value='1'" + String(config.useDst?" checked":"") + "><b>Daylight Saving Time (DST) +1 Hour</b></label><br><hr>";
  
  html += "<h4>Select Weather Display Rotations:</h4>";
  html += "<label><input type='checkbox' name='showcurr' value='1'" + String(config.showCurrentTemp?" checked":"") + ">Show Current Temperature</label><br>";
  html += "<label><input type='checkbox' name='showminmax' value='1'" + String(config.showMinMax?" checked":"") + ">Show Min / Max Temperature</label><br>";
  html += "<label><input type='checkbox' name='showhumidity' value='1'" + String(config.showHumidity?" checked":"") + ">Show Humidity Profile</label><br>";
  html += "<label><input type='checkbox' name='showcondition' value='1'" + String(config.showCondition?" checked":"") + ">Show Condition String</label><br>";
  
  html += "<br><input type='submit' class='btn' value='Save Settings & Update Display'></form></div>";
  
  html += F("<div class='card'><h3>System Tools</h3>");
  html += F("<a href='/sync' class='btn btn-sync'>Force Internet Sync Now</a>");
  html += F("<a href='/restart' class='btn btn-warn' onclick='return confirm(\"Restart system now?\")'>Restart Clock Device</a>");
  html += F("<a href='/resetwifi' class='btn btn-danger' onclick='return confirm(\"This will clear WiFi credentials and open configuration setup. Proceed?\")'>Reset WiFi Profile</a>");
  html += F("<a href='/factory' class='btn btn-danger' style='background:#b71c1c;' onclick='return confirm(\"Wipe all parameters to default factory specifications?\")'>Factory Reset File System</a>");
  html += F("</div>");
  
  html += F("<div class='footer-text'>&copy; 2026 Designed and Developed by Anshuman.<br>All rights reserved. Unauthorized commercial production, distribution, or sale is strictly prohibited.</div>");
  html += F("</body></html>");
  
  server.send(200, "text/html", html);
}

void handleSaveAction() {
  if (server.hasArg("modules")) config.numModules = server.arg("modules").toInt();
  if (server.hasArg("fadeeffect")) config.useFadeEffect = (server.arg("fadeeffect") == "1");
  if (server.hasArg("speed")) config.scrollSpeed = server.arg("speed").toInt();
  if (server.hasArg("intensity")) config.displayIntensity = server.arg("intensity").toInt();
  if (server.hasArg("format24")) config.use24Hour = (server.arg("format24") == "1");
  
  config.showDay = server.hasArg("showday");
  config.showDate = server.hasArg("showdate");
  config.enableWeather = server.hasArg("enweather");
  
  if (server.hasArg("apikey")) config.openWeatherMapApiKey = server.arg("apikey");
  if (server.hasArg("locationid")) config.locationId = server.arg("locationid");
  if (server.hasArg("utcoffset")) config.utcOffsetHours = server.arg("utcoffset").toFloat();
  
  config.useDst = server.hasArg("usedst"); 
  config.showCurrentTemp = server.hasArg("showcurr");
  config.showMinMax = server.hasArg("showminmax");
  config.showHumidity = server.hasArg("showhumidity");
  config.showCondition = server.hasArg("showcondition");

  saveConfiguration();
  
  if (P != nullptr) {
    delete P;
  }
  P = new MD_Parola(HARDWARE_TYPE, DATA_PIN, CLK_PIN, CS_PIN, config.numModules);
  P->begin();
  P->setIntensity(config.displayIntensity);
  P->displayClear();
  
  server.send(200, "text/html", F("<html><body><h3>Settings Applied Instantly!</h3><script>setTimeout(function(){window.location.href='/';},2000);</script></body></html>"));
  
  updateWeatherAndTime();
  loopSequenceIndex = 0; 
}

// System Operations Handlers
void handleForceSync() {
  server.send(200, "text/html", F("<html><body><h3>Syncing Time and Weather with Internet Servers...</h3><script>setTimeout(function(){window.location.href='/';},2500);</script></body></html>"));
  updateWeatherAndTime();
}

void handleRestart() {
  server.send(200, "text/html", F("<html><body><h3>System Rebooting...</h3></body></html>"));
  delay(1000);
  ESP.restart();
}

void handleResetWiFi() {
  server.send(200, "text/html", F("<html><body><h3>WiFi Profile Wiped. Connecting to AP: SmartCLOCK to reconfigure.</h3></body></html>"));
  delay(1000);
  wifiManager.resetSettings();
  ESP.restart();
}

void handleFactoryReset() {
  server.send(200, "text/html", F("<html><body><h3>Wiping File System Storage... Re-starting</h3></body></html>"));
  delay(1000);
  if (SPIFFS.begin()) {
    SPIFFS.format();
  }
  wifiManager.resetSettings();
  ESP.restart();
}

// ==================== Initialization ====================
void setup() {
  Serial.begin(115200);
  
  loadConfiguration();
  
  P = new MD_Parola(HARDWARE_TYPE, DATA_PIN, CLK_PIN, CS_PIN, config.numModules);
  P->begin();
  P->setIntensity(config.displayIntensity);
  P->displayClear();

  wifiManager.setConfigPortalBlocking(false);
  wifiManager.setConfigPortalTimeout(45);

  Serial.println(F("Quiet connection attempt to saved home router details..."));
  WiFi.mode(WIFI_STA);
  WiFi.begin();
  
  // Show a clean, solid "WIFI" status text immediately at boot
  displayStaticText("WIFI");
  
  unsigned long startAttempt = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - startAttempt < 8000) {
    delay(100);
    yield();
    ESP.wdtFeed();
  }

  if (WiFi.status() == WL_CONNECTED) {
    // Clean scroll execution of the IP address right here on startup connection success
    scrollIPBlocking();
    
    long dynamicSecOffset = (config.utcOffsetHours * 3600) + (config.useDst ? 3600 : 0);
    timeClient = new NTPClient(ntpUDP, "pool.ntp.org", dynamicSecOffset, 86400000);
    timeClient->begin();
    updateWeatherAndTime();
  } else {
    Serial.println(F("Router down at startup. Launching background captive engine..."));
    wifiManager.startConfigPortal("SmartCLOCK", "12345678");
    portalActive = true;
    displayStaticText("AP"); // Show a solid, readable "AP" text waiting for phone connection
  }

  server.on("/", handleRootPage);
  server.on("/save", handleSaveAction);
  server.on("/sync", handleForceSync);
  server.on("/restart", handleRestart);
  server.on("/resetwifi", handleResetWiFi);
  server.on("/factory", handleFactoryReset);
  server.begin();
}

// ==================== Core Program Loop ====================
void loop() {
  wifiManager.process();
  server.handleClient();
  yield();

  // If disconnected from the internet, freeze time processing and display current state blocks
  if (WiFi.status() != WL_CONNECTED) {
    ipScrollDone = false; // Reset the flag so it scrolls again when reconnected
    if (!portalActive) {
      wifiManager.startConfigPortal("SmartCLOCK", "12345678");
      portalActive = true;
    }

    // Static text alerts that fit entirely inside 4 modules without truncation layout issues
    if (WiFi.softAPgetStationNum() > 0) {
      displayStaticText("CONN"); // A phone is connected to the clock's hotspot
    } else {
      displayStaticText("AP");   // Hotspot is empty, waiting for configuration
    }
    return; 
  }

  // Handle sudden re-connection events from portal state to active running state
  if (WiFi.status() == WL_CONNECTED) {
    if (portalActive) {
      portalActive = false;
    }
    
    // If we have connected but haven't scrolled the IP address yet, do it now
    if (!ipScrollDone) {
      scrollIPBlocking();
      
      if (timeClient == nullptr) {
        long dynamicSecOffset = (config.utcOffsetHours * 3600) + (config.useDst ? 3600 : 0);
        timeClient = new NTPClient(ntpUDP, "pool.ntp.org", dynamicSecOffset, 86400000);
        timeClient->begin();
      }
      updateWeatherAndTime();
      loopSequenceIndex = 0;
    }
  }

  // AM/PM Single Pixel Indicator Dots for 4-Module configuration tracking
  if (P != nullptr && !config.use24Hour && config.numModules <= 4 && activeMode == MODE_TIME) {
    MD_MAX72XX* mx = P->getGraphicObject();
    if (mx != nullptr) {
      bool isPM = (hour() >= 12);
      mx->setPoint(isPM ? 7 : 0, 31, true);
    }
  }

  // Standard functional display cycle (Time -> Day -> Date -> Weather)
  if (P != nullptr && P->displayAnimate()) {
    time_t checkTime = now();
    if (year(checkTime) <= 1970) {
      if (millis() - lastNTPUpdate > 15000) {
        updateWeatherAndTime();
        lastNTPUpdate = millis();
      }
    } else {
      if (millis() - lastWeatherUpdate > weatherInterval) {
        updateWeatherAndTime();
        lastWeatherUpdate = millis();
      }
    }

    int enabledModes[10];
    int modeCount = 0;
    
    enabledModes[modeCount++] = MODE_TIME;
    
    if (config.numModules <= 4) {
      if (config.showDay)  enabledModes[modeCount++] = MODE_4MOD_DAY;
      if (config.showDate) enabledModes[modeCount++] = MODE_4MOD_DATE;
    } else {
      if (config.showDay || config.showDate) enabledModes[modeCount++] = MODE_8MOD_COMBINED;
    }

    if (config.enableWeather && weatherValid) {
      if (config.showCurrentTemp) enabledModes[modeCount++] = MODE_CURRENT_TEMP;
      if (config.showMinMax)      enabledModes[modeCount++] = MODE_MIN_MAX_TEMP;
      if (config.showHumidity)    enabledModes[modeCount++] = MODE_HUMIDITY;
      if (config.showCondition)   enabledModes[modeCount++] = MODE_CONDITION;
    }
    
    loopSequenceIndex = (loopSequenceIndex + 1) % modeCount;
    activeMode = (DisplayMode)enabledModes[loopSequenceIndex];
    
    String outMsg = "";
    switch (activeMode) {
      case MODE_TIME:
        outMsg = getFormattedTime();
        break;
      case MODE_4MOD_DAY:
        outMsg = getDayStringShort(); 
        break;
      case MODE_4MOD_DATE:
        outMsg = getDateStringLong(); 
        break;
      case MODE_8MOD_COMBINED:
        if (config.showDay && config.showDate) {
          outMsg = getDayStringShort() + ", " + getDateStringLong();
        } else if (config.showDay) {
          outMsg = getDayStringShort();
        } else if (config.showDate) {
          outMsg = getDateStringLong();
        }
        break;
      case MODE_CURRENT_TEMP: 
        if (config.numModules <= 4) {
          outMsg = String(currentTemp, 1) + "C";
        } else {
          outMsg = "TEMP: " + String(currentTemp, 1) + "C"; 
        }
        break;
      case MODE_MIN_MAX_TEMP: 
        if (config.numModules <= 4) {
          outMsg = String(minTemp, 0) + "/" + String(maxTemp, 0) + "C";
        } else {
          outMsg = "L:" + String(minTemp, 0) + " H:" + String(maxTemp, 0) + "C"; 
        }
        break;
      case MODE_HUMIDITY:     
        if (config.numModules <= 4) {
          outMsg = String(humidity) + "%";
        } else {
          outMsg = "HUM: " + String(humidity) + "%"; 
        }
        break;
      case MODE_CONDITION:    
        if (config.numModules <= 4 && weatherCondition.length() > 5) {
          outMsg = weatherCondition.substring(0, 5);
        } else {
          outMsg = weatherCondition; 
        }
        break;
    }
    
    startScrollingMessage(outMsg);
  }
}