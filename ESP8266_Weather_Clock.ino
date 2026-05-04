/*
 * WiFi Smart Clock - ESP8266 + MAX7219 LED Matrix
 * Supports 8x32 (4 modules), 8x64 (8 modules), 8x96 (12 modules)
 * ALL text scrolls right-to-left continuously
 * Designed and developed by Anshuman Sharma
 * Distribution prohibited for commercial use - Copyright Protected
 * 
 * ESP8266 Version - Adapted from ESP32 code
 * Weather updates every 20 minutes
 * Displays: Current Temp, Min/Max Temp (combined), Humidity, Condition
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

// ==================== ESP8266 Hardware Configuration ====================
// Try MD_MAX72XX::GENERIC_HW if FC16_HW shows issues
#define HARDWARE_TYPE MD_MAX72XX::FC16_HW
#define CLK_PIN   14  // D5 on ESP8266
#define DATA_PIN  13  // D7 on ESP8266
#define CS_PIN    15  // D8 on ESP8266

// Weather update interval (20 minutes in milliseconds)
#define WEATHER_UPDATE_INTERVAL 1200000

// ==================== Forward Declarations ====================
enum DisplayMode {
  MODE_TIME,
  MODE_DATE_DAY,      // Combined date and day
  MODE_CURRENT_TEMP,
  MODE_MIN_MAX_TEMP,  // Combined min/max temp
  MODE_HUMIDITY,
  MODE_CONDITION
};

// ==================== Animation State Machine ====================
enum AnimState {
  ANIM_IDLE,
  ANIM_SCROLLING,
  ANIM_COMPLETE
};

struct MessageAnimator {
  AnimState state = ANIM_IDLE;
  String currentMessage = "";
  int scrollSpeed = 50;
  bool messageSet = false;
  bool continuous = false;
  DisplayMode nextMode = MODE_TIME;
};

// ==================== Global Objects ====================
MD_Parola* P = nullptr;
WiFiUDP ntpUDP;
NTPClient* timeClient = nullptr;
ESP8266WebServer server(80);
WiFiClient wifiClient;  // For HTTP requests

MessageAnimator animator;

// Queue for startup messages
String startupMessages[20];
int startupMsgCount = 0;
int currentStartupMsg = 0;
bool inStartupPhase = true;

DisplayMode currentDisplayMode = MODE_TIME;

// ==================== Weather Variables ====================
char currentWeatherTemp[8] = "--";
char currentWeatherMinTemp[8] = "--";
char currentWeatherMaxTemp[8] = "--";
char currentWeatherCondition[30] = "N/A";
char currentWeatherCity[32] = "";
char currentWeatherHumidity[8] = "--";
char currentWeatherFeelsLike[8] = "--";
char currentWeatherPressure[8] = "--";
char currentWeatherWindSpeed[8] = "--";
unsigned long lastWeatherUpdate = 0;
bool weatherDataValid = false;
bool displayActive = true;

// ==================== Configuration Structure ====================
struct Config {
  char timezone[40];
  bool dstEnabled;
  bool format24Hour;
  int scrollSpeed;
  int brightness;
  int numModules;
  bool weatherEnabled;
  char weatherApiKey[65];
  char weatherCityId[20];
  int weatherUpdateInterval;
  bool nightModeEnabled;
  int nightOffHour;
  int nightOnHour;
  bool displayPowerOverride;
  bool manualPowerState;
  bool wifiConfigured;
  
  // Weather component selection
  bool showCurrentTemp;
  bool showMinMaxTemp;
  bool showHumidity;
  bool showCondition;
  bool showFeelsLike;
  bool showWindSpeed;
  
  Config() {
    strcpy(timezone, "Australia/Sydney");
    dstEnabled = false;
    format24Hour = false;
    scrollSpeed = 3;
    brightness = 100;
    numModules = 4;
    weatherEnabled = false;
    memset(weatherApiKey, 0, sizeof(weatherApiKey));
    strcpy(weatherCityId, "");
    weatherUpdateInterval = 20;  // Default 20 minutes
    nightModeEnabled = false;
    nightOffHour = 23;
    nightOnHour = 7;
    displayPowerOverride = false;
    manualPowerState = true;
    wifiConfigured = false;
    
    // Default weather components
    showCurrentTemp = true;
    showMinMaxTemp = true;
    showHumidity = true;
    showCondition = true;
    showFeelsLike = false;
    showWindSpeed = false;
  }
};

Config config;

// ==================== Timezone Data ====================
struct TimezoneInfo {
  const char* name;
  const char* displayName;
  int offsetHours;
  int offsetMinutes;
  bool supportsDST;
};

const TimezoneInfo timezones[] = {
  {"Australia/Sydney", "Australia - Sydney", 10, 0, true},
  {"Australia/Melbourne", "Australia - Melbourne", 10, 0, true},
  {"Australia/Brisbane", "Australia - Brisbane", 10, 0, false},
  {"Australia/Perth", "Australia - Perth", 8, 0, false},
  {"Australia/Adelaide", "Australia - Adelaide", 9, 30, true},
  {"Pacific/Auckland", "New Zealand - Auckland", 12, 0, true},
  {"Asia/Kolkata", "India - IST", 5, 30, false},
  {"Asia/Tokyo", "Japan - Tokyo", 9, 0, false},
  {"Asia/Shanghai", "China - Beijing", 8, 0, false},
  {"Asia/Singapore", "Singapore", 8, 0, false},
  {"Asia/Dubai", "UAE - Dubai", 4, 0, false},
  {"Europe/London", "UK - London", 0, 0, true},
  {"Europe/Paris", "France - Paris", 1, 0, true},
  {"Europe/Berlin", "Germany - Berlin", 1, 0, true},
  {"America/New_York", "USA - New York", -5, 0, true},
  {"America/Chicago", "USA - Chicago", -6, 0, true},
  {"America/Denver", "USA - Denver", -7, 0, true},
  {"America/Los_Angeles", "USA - Los Angeles", -8, 0, true},
  {"America/Toronto", "Canada - Toronto", -5, 0, true},
  {"America/Vancouver", "Canada - Vancouver", -8, 0, true}
};

const int timezoneCount = sizeof(timezones) / sizeof(timezones[0]);

// ==================== Utility Functions ====================
int getTimezoneOffset() {
  for (int i = 0; i < timezoneCount; i++) {
    if (strcmp(config.timezone, timezones[i].name) == 0) {
      int offset = timezones[i].offsetHours * 3600 + timezones[i].offsetMinutes * 60;
      
      if (config.dstEnabled && timezones[i].supportsDST && timeClient) {
        time_t now = timeClient->getEpochTime();
        struct tm* timeinfo = localtime(&now);
        int currentMonth = timeinfo->tm_mon + 1;
        
        if ((currentMonth >= 10 || currentMonth <= 3) && strstr(config.timezone, "Australia") != nullptr) {
          offset += 3600;
        } else if ((currentMonth >= 9 || currentMonth <= 4) && strstr(config.timezone, "Auckland") != nullptr) {
          offset += 3600;
        } else if (currentMonth >= 3 && currentMonth <= 11 && 
                   (strstr(config.timezone, "America") != nullptr || 
                    strstr(config.timezone, "Europe") != nullptr)) {
          offset += 3600;
        }
      }
      return offset;
    }
  }
  return 0;
}

String getFormattedTime() {
  if (!timeClient) return "00:00";
  
  time_t rawTime = timeClient->getEpochTime() + getTimezoneOffset();
  int hours = hour(rawTime);
  int minutes = minute(rawTime);
  
  if (!config.format24Hour) {
    String ampm = (hours >= 12) ? "PM" : "AM";
    hours = hours % 12;
    if (hours == 0) hours = 12;
    char buffer[20];
    sprintf(buffer, "%d:%02d %s", hours, minutes, ampm.c_str());
    return String(buffer);
  } else {
    char buffer[10];
    sprintf(buffer, "%02d:%02d", hours, minutes);
    return String(buffer);
  }
}

String getFormattedDate() {
  if (!timeClient) return "1 Jan 2024";
  
  time_t rawTime = timeClient->getEpochTime() + getTimezoneOffset();
  const char* months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                          "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
  
  char buffer[20];
  sprintf(buffer, "%d %s %d", day(rawTime), months[month(rawTime)-1], year(rawTime));
  return String(buffer);
}

String getDayName() {
  if (!timeClient) return "Monday";
  
  time_t rawTime = timeClient->getEpochTime() + getTimezoneOffset();
  const char* days[] = {"Sunday", "Monday", "Tuesday", "Wednesday",
                        "Thursday", "Friday", "Saturday"};
  
  return String(days[weekday(rawTime)-1]);
}

String getCombinedDateDay() {
  return getFormattedDate() + " - " + getDayName();
}

String getWeatherConditionIcon() {
  String condition = String(currentWeatherCondition);
  condition.toLowerCase();
  
  if (condition.indexOf("clear") >= 0) return "Sunny";
  if (condition.indexOf("cloud") >= 0) {
    if (condition.indexOf("few") >= 0) return "Partly Cloudy";
    if (condition.indexOf("scattered") >= 0) return "Cloudy";
    return "Cloudy";
  }
  if (condition.indexOf("rain") >= 0) {
    if (condition.indexOf("light") >= 0) return "Light Rain";
    if (condition.indexOf("heavy") >= 0) return "Heavy Rain";
    return "Rain";
  }
  if (condition.indexOf("thunder") >= 0) return "Thunderstorm";
  if (condition.indexOf("snow") >= 0) return "Snow";
  if (condition.indexOf("mist") >= 0) return "Mist";
  if (condition.indexOf("fog") >= 0) return "Fog";
  if (condition.indexOf("haze") >= 0) return "Haze";
  return String(currentWeatherCondition);
}

String getFormattedCurrentTemp() {
  if (!config.weatherEnabled) return "Weather off";
  if (!weatherDataValid) return "Loading weather...";
  return String(currentWeatherCity) + " " + String(currentWeatherTemp) + "C";
}

String getFormattedMinMaxTemp() {
  if (!config.weatherEnabled || !weatherDataValid) return "";
  return "Min " + String(currentWeatherMinTemp) + "C  Max " + String(currentWeatherMaxTemp) + "C";
}

String getFormattedHumidity() {
  if (!config.weatherEnabled || !weatherDataValid) return "";
  return "Humidity " + String(currentWeatherHumidity) + "%";
}

String getFormattedCondition() {
  if (!config.weatherEnabled || !weatherDataValid) return "";
  return getWeatherConditionIcon();
}

String getFormattedFeelsLike() {
  if (!config.weatherEnabled || !weatherDataValid) return "";
  return "Feels like " + String(currentWeatherFeelsLike) + "C";
}

String getFormattedWindSpeed() {
  if (!config.weatherEnabled || !weatherDataValid) return "";
  return "Wind " + String(currentWeatherWindSpeed) + " m/s";
}

String getFormattedPressure() {
  if (!config.weatherEnabled || !weatherDataValid) return "";
  return "Pressure " + String(currentWeatherPressure) + " hPa";
}

int getScrollSpeedValue() {
  const int speeds[] = {120, 90, 60, 40, 25};
  return speeds[constrain(config.scrollSpeed, 0, 4)];
}

bool isNightTime() {
  if (!config.nightModeEnabled || !timeClient) return false;
  
  time_t rawTime = timeClient->getEpochTime() + getTimezoneOffset();
  int currentHour = hour(rawTime);
  
  if (config.nightOffHour < config.nightOnHour) {
    return (currentHour >= config.nightOffHour && currentHour < config.nightOnHour);
  } else {
    return (currentHour >= config.nightOffHour || currentHour < config.nightOnHour);
  }
}

bool shouldDisplayBeOn() {
  if (config.displayPowerOverride) return config.manualPowerState;
  return !isNightTime();
}

int getDisplayWidth() {
  return config.numModules * 8;
}

String getDisplayResolution() {
  return String(getDisplayWidth()) + "x8";
}

int countEnabledWeatherModes() {
  int count = 0;
  if (config.showCurrentTemp) count++;
  if (config.showMinMaxTemp) count++;
  if (config.showHumidity) count++;
  if (config.showCondition) count++;
  if (config.showFeelsLike) count++;
  if (config.showWindSpeed) count++;
  return count;
}

// ==================== Weather Functions ====================
bool fetchWeatherData() {
  if (!config.weatherEnabled || strlen(config.weatherApiKey) == 0 || strlen(config.weatherCityId) == 0) {
    return false;
  }
  
  Serial.println("Fetching weather data...");
  
  HTTPClient http;
  
  String url = "http://api.openweathermap.org/data/2.5/weather?id=";
  url += String(config.weatherCityId);
  url += "&appid=" + String(config.weatherApiKey);
  url += "&units=metric";
  
  http.begin(wifiClient, url);
  http.setTimeout(10000);
  
  int httpCode = http.GET();
  
  if (httpCode != HTTP_CODE_OK) {
    Serial.printf("Weather HTTP error: %d\n", httpCode);
    http.end();
    return false;
  }
  
  String payload = http.getString();
  http.end();
  
  DynamicJsonDocument doc(4096);
  DeserializationError error = deserializeJson(doc, payload);
  
  if (error) {
    Serial.println("Weather JSON parse failed");
    return false;
  }
  
  // Parse main weather data
  JsonObject main = doc["main"];
  if (!main.isNull()) {
    // Current temperature
    snprintf(currentWeatherTemp, sizeof(currentWeatherTemp), "%.0f", main["temp"].as<float>());
    // Min temperature
    snprintf(currentWeatherMinTemp, sizeof(currentWeatherMinTemp), "%.0f", main["temp_min"].as<float>());
    // Max temperature
    snprintf(currentWeatherMaxTemp, sizeof(currentWeatherMaxTemp), "%.0f", main["temp_max"].as<float>());
    // Humidity
    snprintf(currentWeatherHumidity, sizeof(currentWeatherHumidity), "%d", main["humidity"].as<int>());
    // Pressure
    if (main.containsKey("pressure")) {
      snprintf(currentWeatherPressure, sizeof(currentWeatherPressure), "%d", main["pressure"].as<int>());
    }
    // Feels like
    if (main.containsKey("feels_like")) {
      snprintf(currentWeatherFeelsLike, sizeof(currentWeatherFeelsLike), "%.0f", main["feels_like"].as<float>());
    }
  }
  
  // Parse weather condition
  JsonArray weather = doc["weather"];
  if (weather.size() > 0) {
    const char* mainWeather = weather[0]["main"];
    const char* description = weather[0]["description"];
    if (mainWeather) {
      String condition = String(mainWeather);
      if (description) {
        condition += " (" + String(description) + ")";
      }
      strncpy(currentWeatherCondition, condition.c_str(), sizeof(currentWeatherCondition) - 1);
    }
  }
  
  // Parse wind data
  JsonObject wind = doc["wind"];
  if (!wind.isNull() && wind.containsKey("speed")) {
    snprintf(currentWeatherWindSpeed, sizeof(currentWeatherWindSpeed), "%.1f", wind["speed"].as<float>());
  }
  
  // Get city name
  const char* cityName = doc["name"];
  if (cityName) strncpy(currentWeatherCity, cityName, sizeof(currentWeatherCity) - 1);
  
  weatherDataValid = true;
  lastWeatherUpdate = millis();
  
  Serial.printf("Weather for %s:\n", currentWeatherCity);
  Serial.printf("  Current: %sC\n", currentWeatherTemp);
  Serial.printf("  Min: %sC, Max: %sC\n", currentWeatherMinTemp, currentWeatherMaxTemp);
  Serial.printf("  Humidity: %s%%\n", currentWeatherHumidity);
  Serial.printf("  Condition: %s\n", currentWeatherCondition);
  Serial.printf("  Feels like: %sC\n", currentWeatherFeelsLike);
  Serial.printf("  Wind: %s m/s\n", currentWeatherWindSpeed);
  
  return true;
}

// ==================== Display Management ====================
void initializeDisplay() {
  if (P != nullptr) {
    P->displayClear();
    delete P;
  }
  
  P = new MD_Parola(HARDWARE_TYPE, DATA_PIN, CLK_PIN, CS_PIN, config.numModules);
  
  if (P == nullptr) {
    Serial.println("Display init failed!");
    return;
  }
  
  P->begin();
  P->setZone(0, 0, config.numModules - 1);
  P->setIntensity(map(config.brightness, 0, 100, 0, 15));
  P->displayClear();
  P->displaySuspend(false);
  P->setTextEffect(PA_SCROLL_LEFT, PA_SCROLL_LEFT);
  
  Serial.printf("Display: %d modules (%dx8)\n", config.numModules, getDisplayWidth());
}

void startScrollingMessage(String message, bool isContinuous = false) {
  if (!P) return;
  
  animator.currentMessage = message;
  animator.scrollSpeed = getScrollSpeedValue();
  animator.state = ANIM_SCROLLING;
  animator.messageSet = false;
  animator.continuous = isContinuous;
  
  Serial.println("Starting: " + message);
}

void updateAnimation() {
  if (!P) return;
  
  static unsigned long lastPowerCheck = 0;
  if (millis() - lastPowerCheck > 1000) {
    bool shouldBeOn = shouldDisplayBeOn();
    if (shouldBeOn != displayActive) {
      displayActive = shouldBeOn;
      P->displaySuspend(!shouldBeOn);
      if (!shouldBeOn) {
        P->displayClear();
        animator.state = ANIM_IDLE;
        return;
      } else {
        animator.state = ANIM_IDLE;
      }
    }
    lastPowerCheck = millis();
  }
  
  if (!displayActive) return;
  
  P->setIntensity(map(config.brightness, 0, 100, 0, 15));
  
  switch (animator.state) {
    case ANIM_IDLE:
      break;
      
    case ANIM_SCROLLING:
      if (!animator.messageSet) {
        P->displayClear();
        P->displayText(animator.currentMessage.c_str(), PA_CENTER, 
                      animator.scrollSpeed, 0,
                      PA_SCROLL_LEFT, PA_SCROLL_LEFT);
        animator.messageSet = true;
      }
      
      if (P->getZoneStatus(0)) {
        animator.state = ANIM_COMPLETE;
        P->displayClear();
      }
      break;
      
    case ANIM_COMPLETE:
      break;
  }
  
  P->displayAnimate();
}

// ==================== Configuration Management ====================
bool loadConfig() {
  if (!SPIFFS.begin()) return false;
  
  File configFile = SPIFFS.open("/config.json", "r");
  if (!configFile) return false;
  
  DynamicJsonDocument doc(2048);
  DeserializationError error = deserializeJson(doc, configFile);
  configFile.close();
  
  if (error) return false;
  
  strlcpy(config.timezone, doc["tz"] | "Australia/Sydney", sizeof(config.timezone));
  config.dstEnabled = doc["dst"] | false;
  config.format24Hour = doc["fmt"] | false;
  config.scrollSpeed = doc["spd"] | 3;
  config.brightness = doc["bri"] | 100;
  config.numModules = doc["mod"] | 4;
  config.weatherEnabled = doc["we"] | false;
  strlcpy(config.weatherApiKey, doc["wkey"] | "", sizeof(config.weatherApiKey));
  strlcpy(config.weatherCityId, doc["wcid"] | "", sizeof(config.weatherCityId));
  config.weatherUpdateInterval = doc["wint"] | 20;
  config.nightModeEnabled = doc["nm"] | false;
  config.nightOffHour = doc["noff"] | 23;
  config.nightOnHour = doc["non"] | 7;
  config.displayPowerOverride = doc["povr"] | false;
  config.manualPowerState = doc["pman"] | true;
  config.wifiConfigured = doc["wifi"] | false;
  
  // Weather components
  config.showCurrentTemp = doc["wtemp"] | true;
  config.showMinMaxTemp = doc["wminmax"] | true;
  config.showHumidity = doc["whum"] | true;
  config.showCondition = doc["wcond"] | true;
  config.showFeelsLike = doc["wfeel"] | false;
  config.showWindSpeed = doc["wwind"] | false;
  
  if (config.numModules != 4 && config.numModules != 8 && config.numModules != 12) {
    config.numModules = 4;
  }
  
  return true;
}

bool saveConfig() {
  if (!SPIFFS.begin()) return false;
  
  DynamicJsonDocument doc(2048);
  
  doc["tz"] = config.timezone;
  doc["dst"] = config.dstEnabled;
  doc["fmt"] = config.format24Hour;
  doc["spd"] = config.scrollSpeed;
  doc["bri"] = config.brightness;
  doc["mod"] = config.numModules;
  doc["we"] = config.weatherEnabled;
  doc["wkey"] = config.weatherApiKey;
  doc["wcid"] = config.weatherCityId;
  doc["wint"] = config.weatherUpdateInterval;
  doc["nm"] = config.nightModeEnabled;
  doc["noff"] = config.nightOffHour;
  doc["non"] = config.nightOnHour;
  doc["povr"] = config.displayPowerOverride;
  doc["pman"] = config.manualPowerState;
  doc["wifi"] = config.wifiConfigured;
  
  // Weather components
  doc["wtemp"] = config.showCurrentTemp;
  doc["wminmax"] = config.showMinMaxTemp;
  doc["whum"] = config.showHumidity;
  doc["wcond"] = config.showCondition;
  doc["wfeel"] = config.showFeelsLike;
  doc["wwind"] = config.showWindSpeed;
  
  File configFile = SPIFFS.open("/config.json", "w");
  if (!configFile) return false;
  
  serializeJson(doc, configFile);
  configFile.close();
  return true;
}

void resetToDefaults() {
  config = Config();
  saveConfig();
}

// ==================== Web Server Handlers ====================
String generateTimezoneOptions() {
  String options = "";
  for (int i = 0; i < timezoneCount; i++) {
    options += "<option value='" + String(timezones[i].name) + "'";
    if (strcmp(config.timezone, timezones[i].name) == 0) options += " selected";
    options += ">" + String(timezones[i].displayName) + "</option>";
  }
  return options;
}

String generateBrightnessOptions() {
  String options = "";
  int values[] = {100, 80, 60, 40, 20, 0};
  for (int i = 0; i < 6; i++) {
    options += "<option value='" + String(values[i]) + "'";
    if (config.brightness == values[i]) options += " selected";
    options += ">" + String(values[i]) + "%</option>";
  }
  return options;
}

String generateSpeedOptions() {
  String options = "";
  const char* speeds[] = {"Very Slow", "Slow", "Normal", "Fast", "Very Fast"};
  for (int i = 0; i < 5; i++) {
    options += "<option value='" + String(i) + "'";
    if (config.scrollSpeed == i) options += " selected";
    options += ">" + String(speeds[i]) + "</option>";
  }
  return options;
}

String generateModuleOptions() {
  String options = "";
  int modules[] = {4, 8, 12};
  const char* names[] = {"4 Modules (8x32)", "8 Modules (8x64)", "12 Modules (8x96)"};
  
  for (int i = 0; i < 3; i++) {
    options += "<option value='" + String(modules[i]) + "'";
    if (config.numModules == modules[i]) options += " selected";
    options += ">" + String(names[i]) + "</option>";
  }
  return options;
}

String generateCheckbox(const char* name, const char* label, bool checked) {
  String html = "<div style='display:flex; align-items:center; margin:5px 0;'>";
  html += "<input type='checkbox' name='";
  html += name;
  html += "' value='1'";
  if (checked) html += " checked";
  html += " style='width:20px; margin-right:10px;'>";
  html += "<label style='width:auto;'>";
  html += label;
  html += "</label></div>";
  return html;
}

void handleRoot() {
  String weatherStatus = "Disabled";
  if (config.weatherEnabled) {
    if (strlen(config.weatherApiKey) > 0 && strlen(config.weatherCityId) > 0) {
      if (weatherDataValid) {
        weatherStatus = String(currentWeatherCity) + ": " + currentWeatherTemp + "C (Min:" + currentWeatherMinTemp + "C Max:" + currentWeatherMaxTemp + "C) " + currentWeatherHumidity + "% " + getWeatherConditionIcon();
      } else {
        weatherStatus = "Loading...";
      }
    } else {
      weatherStatus = "Config Required";
    }
  }
  
  String html = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
    <meta name="viewport" content="width=device-width, initial-scale=1">
    <title>WiFi Smart Clock</title>
    <style>
        body { font-family: Arial; margin: 20px; background: #f0f0f0; }
        .container { max-width: 900px; margin: 0 auto; background: white; padding: 20px; border-radius: 10px; }
        h1 { color: #333; } h2 { color: #666; margin-top: 20px; }
        .designer { color: #888; font-style: italic; }
        .copyright { color: #999; font-size: 0.8em; margin: 20px 0; padding: 10px; background: #f9f9f9; }
        .status { background: #e8f4f8; padding: 15px; border-radius: 5px; }
        .status-grid { display: grid; grid-template-columns: repeat(2, 1fr); gap: 10px; }
        .form-group { margin: 15px 0; display: flex; align-items: center; }
        label { width: 220px; font-weight: bold; }
        select, input { padding: 8px; width: 250px; border: 1px solid #ddd; border-radius: 4px; }
        button { padding: 10px 20px; margin: 5px; background: #007bff; color: white; border: none; border-radius: 5px; cursor: pointer; }
        button.danger { background: #dc3545; }
        button.warning { background: #ffc107; color: #333; }
        button.success { background: #28a745; }
        .info-box { background: #fff3cd; padding: 15px; border-radius: 5px; margin: 20px 0; }
        .section { border-top: 1px solid #ddd; margin-top: 20px; padding-top: 20px; }
        .hardware-info { background: #d4edda; padding: 10px; border-radius: 5px; }
        .weather-details { font-size: 0.9em; color: #666; margin-top: 5px; }
        .checkbox-group { background: #f8f9fa; padding: 15px; border-radius: 5px; margin: 10px 0; }
        .ap-info { background: #d1ecf1; padding: 15px; border-radius: 5px; margin: 15px 0; border: 1px solid #17a2b8; }
    </style>
</head>
<body>
    <div class="container">
        <h1>WiFi Smart Clock</h1>
        <div class="designer">Designed and developed by Anshuman Sharma</div>
        <div class="copyright">Distribution prohibited for commercial use - Copyright Protected</div>
        
        <div class="ap-info">
            <strong>📡 Access Point Information (First Boot):</strong><br>
            SSID: <strong>SmartCLOCK</strong><br>
            Password: <strong>12345678</strong><br>
            IP Address: <strong>192.168.4.1</strong>
        </div>
        
        <div class="hardware-info">
            ESP8266 | Modules: )rawliteral" + String(config.numModules) + R"rawliteral( | Resolution: )rawliteral" + getDisplayResolution() + R"rawliteral(
        </div>
        
        <div class="status">
            <h2>Status</h2>
            <div class="status-grid">
                <div><strong>Time:</strong> )rawliteral" + getFormattedTime() + R"rawliteral(</div>
                <div><strong>Date/Day:</strong> )rawliteral" + getCombinedDateDay() + R"rawliteral(</div>
                <div><strong>Weather:</strong> )rawliteral" + weatherStatus + R"rawliteral(</div>
                <div><strong>WiFi:</strong> )rawliteral" + WiFi.SSID() + R"rawliteral(</div>
                <div><strong>IP:</strong> )rawliteral" + WiFi.localIP().toString() + R"rawliteral(</div>
                <div><strong>Display:</strong> )rawliteral" + String(config.displayPowerOverride ? (config.manualPowerState ? "ON" : "OFF") : "Auto") + R"rawliteral(</div>
            </div>
        </div>
        
        <form action="/save" method="POST">
            <h2>Time Settings</h2>
            <div class="form-group">
                <label>Time Zone:</label>
                <select name="timezone">)rawliteral" + generateTimezoneOptions() + R"rawliteral(</select>
            </div>
            <div class="form-group">
                <label>DST:</label>
                <select name="dst">
                    <option value="0")rawliteral" + String(config.dstEnabled ? "" : " selected") + R"rawliteral(>OFF</option>
                    <option value="1")rawliteral" + String(config.dstEnabled ? " selected" : "") + R"rawliteral(>ON</option>
                </select>
            </div>
            <div class="form-group">
                <label>Format:</label>
                <select name="format">
                    <option value="0")rawliteral" + String(config.format24Hour ? "" : " selected") + R"rawliteral(>12-hour</option>
                    <option value="1")rawliteral" + String(config.format24Hour ? " selected" : "") + R"rawliteral(>24-hour</option>
                </select>
            </div>
            
            <h2 class="section">Display Settings</h2>
            <div class="form-group">
                <label>Number of Modules:</label>
                <select name="modules">)rawliteral" + generateModuleOptions() + R"rawliteral(</select>
            </div>
            <div class="form-group">
                <label>Scroll Speed:</label>
                <select name="speed">)rawliteral" + generateSpeedOptions() + R"rawliteral(</select>
            </div>
            <div class="form-group">
                <label>Brightness:</label>
                <select name="brightness">)rawliteral" + generateBrightnessOptions() + R"rawliteral(</select>
            </div>
            
            <h2 class="section">Weather Settings (Updates every 20 min)</h2>
            <div class="info-box">
                <strong>Weather Information Displayed:</strong><br>
                Current Temperature<br>
                Min / Max Temperature (combined on one screen)<br>
                Humidity<br>
                Weather Condition (Sunny, Rain, Cloudy, etc.)<br><br>
                Get free API key: <a href="https://openweathermap.org/api" target="_blank">OpenWeatherMap</a><br>
                City IDs: Sydney=2147714, Melbourne=2158177, Brisbane=2174003, London=2643743, New York=5128581
            </div>
            <div class="form-group">
                <label>Weather:</label>
                <select name="weatherEnabled">
                    <option value="0")rawliteral" + String(config.weatherEnabled ? "" : " selected") + R"rawliteral(>Disabled</option>
                    <option value="1")rawliteral" + String(config.weatherEnabled ? " selected" : "") + R"rawliteral(>Enabled</option>
                </select>
            </div>
            <div class="form-group">
                <label>API Key:</label>
                <input type="password" name="apiKey" value=")rawliteral" + String(config.weatherApiKey) + R"rawliteral(" maxlength="64">
            </div>
            <div class="form-group">
                <label>City ID:</label>
                <input type="text" name="cityId" value=")rawliteral" + String(config.weatherCityId) + R"rawliteral(" maxlength="19">
            </div>
            <div class="form-group">
                <label>Update Interval (min):</label>
                <input type="number" name="weatherInterval" min="5" max="60" value=")rawliteral" + String(config.weatherUpdateInterval) + R"rawliteral(">
                <span style="margin-left:10px;font-size:0.9em;">(Default: 20 min)</span>
            </div>
            
            <div class="checkbox-group">
                <strong>Select Weather Components to Display:</strong>
                )rawliteral" + generateCheckbox("showCurrentTemp", "Show Current Temperature", config.showCurrentTemp) + R"rawliteral(
                )rawliteral" + generateCheckbox("showMinMaxTemp", "Show Min/Max Temperature", config.showMinMaxTemp) + R"rawliteral(
                )rawliteral" + generateCheckbox("showHumidity", "Show Humidity", config.showHumidity) + R"rawliteral(
                )rawliteral" + generateCheckbox("showCondition", "Show Weather Condition", config.showCondition) + R"rawliteral(
                )rawliteral" + generateCheckbox("showFeelsLike", "Show 'Feels Like' Temperature", config.showFeelsLike) + R"rawliteral(
                )rawliteral" + generateCheckbox("showWindSpeed", "Show Wind Speed", config.showWindSpeed) + R"rawliteral(
            </div>
            
            <h2 class="section">Night Mode</h2>
            <div class="form-group">
                <label>Night Mode:</label>
                <select name="nightMode">
                    <option value="0")rawliteral" + String(config.nightModeEnabled ? "" : " selected") + R"rawliteral(>Disabled</option>
                    <option value="1")rawliteral" + String(config.nightModeEnabled ? " selected" : "") + R"rawliteral(>Enabled</option>
                </select>
            </div>
            <div class="form-group">
                <label>Turn OFF at (hour):</label>
                <input type="number" name="offHour" min="0" max="23" value=")rawliteral" + String(config.nightOffHour) + R"rawliteral(">
            </div>
            <div class="form-group">
                <label>Turn ON at (hour):</label>
                <input type="number" name="onHour" min="0" max="23" value=")rawliteral" + String(config.nightOnHour) + R"rawliteral(">
            </div>
            
            <button type="submit" class="success">Save & Reboot</button>
        </form>
        
        <div style="margin-top:20px">
            <form action="/power" method="POST" style="display:inline">
                <input type="hidden" name="state" value="on">
                <button type="submit" class="success">Display ON</button>
            </form>
            <form action="/power" method="POST" style="display:inline">
                <input type="hidden" name="state" value="off">
                <button type="submit" class="warning">Display OFF</button>
            </form>
            <form action="/power" method="POST" style="display:inline">
                <input type="hidden" name="state" value="auto">
                <button type="submit">Auto Mode</button>
            </form>
            <form action="/weather/refresh" method="POST" style="display:inline">
                <button type="submit">Refresh Weather</button>
            </form>
        </div>
        
        <div style="margin-top:20px">
            <form action="/forget" method="POST" style="display:inline">
                <button type="submit" class="warning">Forget WiFi</button>
            </form>
            <form action="/reset" method="POST" style="display:inline">
                <button type="submit" class="danger">Reset All Settings</button>
            </form>
        </div>
    </div>
</body>
</html>
)rawliteral";

  server.send(200, "text/html", html);
}

void handleSave() {
  if (server.hasArg("timezone")) strlcpy(config.timezone, server.arg("timezone").c_str(), sizeof(config.timezone));
  if (server.hasArg("dst")) config.dstEnabled = (server.arg("dst") == "1");
  if (server.hasArg("format")) config.format24Hour = (server.arg("format") == "1");
  if (server.hasArg("speed")) config.scrollSpeed = server.arg("speed").toInt();
  if (server.hasArg("brightness")) config.brightness = server.arg("brightness").toInt();
  if (server.hasArg("modules")) {
    int m = server.arg("modules").toInt();
    if (m == 4 || m == 8 || m == 12) config.numModules = m;
  }
  if (server.hasArg("weatherEnabled")) config.weatherEnabled = (server.arg("weatherEnabled") == "1");
  if (server.hasArg("apiKey")) strlcpy(config.weatherApiKey, server.arg("apiKey").c_str(), sizeof(config.weatherApiKey));
  if (server.hasArg("cityId")) strlcpy(config.weatherCityId, server.arg("cityId").c_str(), sizeof(config.weatherCityId));
  if (server.hasArg("weatherInterval")) config.weatherUpdateInterval = server.arg("weatherInterval").toInt();
  if (server.hasArg("nightMode")) config.nightModeEnabled = (server.arg("nightMode") == "1");
  if (server.hasArg("offHour")) config.nightOffHour = server.arg("offHour").toInt();
  if (server.hasArg("onHour")) config.nightOnHour = server.arg("onHour").toInt();
  
  // Weather components
  config.showCurrentTemp = server.hasArg("showCurrentTemp");
  config.showMinMaxTemp = server.hasArg("showMinMaxTemp");
  config.showHumidity = server.hasArg("showHumidity");
  config.showCondition = server.hasArg("showCondition");
  config.showFeelsLike = server.hasArg("showFeelsLike");
  config.showWindSpeed = server.hasArg("showWindSpeed");
  
  saveConfig();
  server.send(200, "text/html", "<html><body><h2>Saved!</h2><p>Rebooting...</p></body></html>");
  delay(1000);
  ESP.restart();
}

void handlePower() {
  if (server.hasArg("state")) {
    String state = server.arg("state");
    if (state == "on") { config.displayPowerOverride = true; config.manualPowerState = true; }
    else if (state == "off") { config.displayPowerOverride = true; config.manualPowerState = false; }
    else { config.displayPowerOverride = false; }
    saveConfig();
  }
  server.send(200, "text/html", "<html><body><h2>OK</h2><a href='/'>Back</a></body></html>");
}

void handleWeatherRefresh() {
  String msg = fetchWeatherData() ? "Weather Updated!" : "Failed!";
  server.send(200, "text/html", "<html><body><h2>" + msg + "</h2><a href='/'>Back</a></body></html>");
}

void handleForget() {
  WiFiManager wm;
  wm.resetSettings();
  server.send(200, "text/html", "<html><body><h2>WiFi Cleared</h2><p>Rebooting...</p></body></html>");
  delay(1000);
  ESP.restart();
}

void handleReset() {
  resetToDefaults();
  server.send(200, "text/html", "<html><body><h2>Reset!</h2><p>Rebooting...</p></body></html>");
  delay(1000);
  ESP.restart();
}

// ==================== Setup ====================
void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("\n=== WiFi Smart Clock - ESP8266 ===\n");
  
  if (!loadConfig()) resetToDefaults();
  
  initializeDisplay();
  
  // Setup startup messages
  startupMessages[0] = "Welcome";
  startupMessages[1] = "WiFi Smart Clock";
  startupMsgCount = 2;
  currentStartupMsg = 0;
  inStartupPhase = true;
  
  // Start first message
  startScrollingMessage(startupMessages[0]);
  
  // WiFi Manager
  WiFiManager wifiManager;
  wifiManager.setConfigPortalTimeout(180);
  
  if (!config.wifiConfigured) {
    // Display AP information prominently
    startupMessages[startupMsgCount++] = "Connect: SmartCLOCK";
    startupMessages[startupMsgCount++] = "Pass: 12345678";
    startupMessages[startupMsgCount++] = "IP: 192.168.4.1";
    
    // Also print to serial for easy reference
    Serial.println("\n=== ACCESS POINT INFORMATION ===");
    Serial.println("SSID: SmartCLOCK");
    Serial.println("Password: 12345678");
    Serial.println("IP Address: 192.168.4.1");
    Serial.println("================================\n");
    
    wifiManager.autoConnect("SmartCLOCK", "12345678");
    config.wifiConfigured = true;
    saveConfig();
  } else {
    startupMessages[startupMsgCount++] = "Connecting WiFi...";
    WiFi.begin();
    int attempts = 0;
    while (WiFi.status() != WL_CONNECTED && attempts < 30) {
      delay(500);
      attempts++;
    }
    if (WiFi.status() != WL_CONNECTED) {
      Serial.println("\n=== ACCESS POINT INFORMATION ===");
      Serial.println("SSID: SmartCLOCK");
      Serial.println("Password: 12345678");
      Serial.println("IP Address: 192.168.4.1");
      Serial.println("================================\n");
      
      wifiManager.startConfigPortal("SmartCLOCK", "12345678");
    }
  }
  
  startupMessages[startupMsgCount++] = "WiFi Connected!";
  startupMessages[startupMsgCount++] = "IP: " + WiFi.localIP().toString();
  
  // NTP
  startupMessages[startupMsgCount++] = "Syncing Time...";
  timeClient = new NTPClient(ntpUDP, "pool.ntp.org", 0, 3600000);
  timeClient->begin();
  for (int i = 0; i < 10 && !timeClient->update(); i++) {
    timeClient->forceUpdate();
    delay(1000);
  }
  startupMessages[startupMsgCount++] = "Time Synced!";
  
  // Weather
  if (config.weatherEnabled && strlen(config.weatherApiKey) > 0) {
    startupMessages[startupMsgCount++] = "Fetching Weather...";
    if (fetchWeatherData()) {
      startupMessages[startupMsgCount++] = "Weather Ready!";
    }
  }
  
  startupMessages[startupMsgCount++] = "Done!";
  
  // Web server
  server.on("/", handleRoot);
  server.on("/save", HTTP_POST, handleSave);
  server.on("/power", HTTP_POST, handlePower);
  server.on("/weather/refresh", HTTP_POST, handleWeatherRefresh);
  server.on("/forget", HTTP_POST, handleForget);
  server.on("/reset", HTTP_POST, handleReset);
  server.begin();
  
  Serial.println("HTTP: http://" + WiFi.localIP().toString());
  Serial.printf("Display: %dx8\n", getDisplayWidth());
  Serial.printf("Weather updates every %d minutes\n", config.weatherUpdateInterval);
}

// ==================== Main Loop ====================
unsigned long lastNTPUpdate = 0;
int currentMsgIndex = 0;
int weatherModeIndex = 0;

void loop() {
  server.handleClient();
  
  // NTP update every 3 hours
  if (millis() - lastNTPUpdate > 10800000) {
    if (timeClient) timeClient->update();
    lastNTPUpdate = millis();
  }
  
  // Weather update based on configured interval (default 20 minutes)
  if (config.weatherEnabled && strlen(config.weatherApiKey) > 0) {
    unsigned long updateIntervalMs = (unsigned long)config.weatherUpdateInterval * 60000UL;
    if (millis() - lastWeatherUpdate >= updateIntervalMs) {
      Serial.printf("Updating weather (every %d minutes)...\n", config.weatherUpdateInterval);
      fetchWeatherData();
    }
  }
  
  // Handle message sequencing
  if (animator.state == ANIM_IDLE || animator.state == ANIM_COMPLETE) {
    if (inStartupPhase) {
      // Move to next startup message
      currentMsgIndex++;
      if (currentMsgIndex < startupMsgCount) {
        startScrollingMessage(startupMessages[currentMsgIndex]);
      } else {
        // Startup phase complete - begin normal operation
        inStartupPhase = false;
        currentDisplayMode = MODE_TIME;
        String firstMsg = getFormattedTime();
        startScrollingMessage(firstMsg, true);
      }
    } else {
      // Normal operation - cycle through enabled modes
      if (config.weatherEnabled && weatherDataValid) {
        // Build array of enabled modes
        DisplayMode enabledModes[10];
        int modeCount = 0;
        
        enabledModes[modeCount++] = MODE_TIME;
        enabledModes[modeCount++] = MODE_DATE_DAY;
        
        if (config.showCurrentTemp) enabledModes[modeCount++] = MODE_CURRENT_TEMP;
        if (config.showMinMaxTemp) enabledModes[modeCount++] = MODE_MIN_MAX_TEMP;
        if (config.showHumidity) enabledModes[modeCount++] = MODE_HUMIDITY;
        if (config.showCondition) enabledModes[modeCount++] = MODE_CONDITION;
        
        // Cycle through modes
        weatherModeIndex = (weatherModeIndex + 1) % modeCount;
        
        String msg;
        switch (enabledModes[weatherModeIndex]) {
          case MODE_TIME:
            msg = getFormattedTime();
            break;
          case MODE_DATE_DAY:
            msg = getCombinedDateDay();
            break;
          case MODE_CURRENT_TEMP:
            msg = getFormattedCurrentTemp();
            break;
          case MODE_MIN_MAX_TEMP:
            msg = getFormattedMinMaxTemp();
            break;
          case MODE_HUMIDITY:
            msg = getFormattedHumidity();
            break;
          case MODE_CONDITION:
            msg = getFormattedCondition();
            break;
          default:
            msg = getFormattedTime();
            break;
        }
        
        startScrollingMessage(msg, true);
      } else {
        // Weather disabled or invalid - just show time and date
        currentDisplayMode = (currentDisplayMode == MODE_TIME) ? MODE_DATE_DAY : MODE_TIME;
        
        String msg;
        if (currentDisplayMode == MODE_TIME) {
          msg = getFormattedTime();
        } else {
          msg = getCombinedDateDay();
        }
        
        startScrollingMessage(msg, true);
      }
    }
  }
  
  // Update animation
  updateAnimation();
  
  delay(5);
}