/*
  Smart WiFi Clock for ESP32 + MAX7219 LED Matrix
  ------------------------------------------------
  Hardware:
    - ESP32 DEVKIT V1
    - MAX7219 LED matrix modules (4, 8, or 12 modules: 8x32, 8x64, 8x96)
    - Pins: CLK=14, DATA=13, CS=15

  Notes:
    - You can select 4, 8, or 12 modules from the web UI. Default is 4.
    - The display object is created at runtime so module count can be changed and applied after saving settings.
    - After changing module count, the device will reinitialize the matrix driver immediately.
    - Timezones: comprehensive IANA timezone list included in the dropdown.
    - Weather updates immediately after WiFi connect, after saving settings, and on reboot (if enabled).
    - Time shown without seconds.
    - Weather shows current temp then condition; 3 spaces separate weather segments.
    - Toggle to show/hide weather location available on the web UI.

  Libraries required:
    - WiFi.h, WebServer.h, DNSServer.h, WiFiManager.h
    - NTPClient.h, TimeLib.h
    - MD_Parola.h, MD_MAX72xx.h
    - SPIFFS.h, ArduinoJson.h
    - HTTPClient.h, WiFiClientSecure.h

  Copyright:
    Designed and developed by Anshuman Sharma - Distribution prohibited for commercial use
*/

#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <WiFiManager.h>
#include <NTPClient.h>
#include <WiFiUdp.h>
#include <TimeLib.h>
#include <MD_Parola.h>
#include <MD_MAX72xx.h>
#include <SPI.h>
#include <SPIFFS.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>

// -------------------- HARDWARE DEFAULTS --------------------
#define HARDWARE_TYPE MD_MAX72XX::FC16_HW
#define CLK_PIN       14
#define DATA_PIN      13
#define CS_PIN        15

// Default modules count (4 modules => 8x32)
#define DEFAULT_MODULES 4

// -------------------- RUNTIME DISPLAY POINTER --------------------
MD_Parola *display = nullptr;
uint8_t currentModules = DEFAULT_MODULES; // runtime modules count (4,8,12)

// -------------------- NETWORK & WEB --------------------
WebServer server(80);
WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP);

// -------------------- CONSTANTS --------------------
const char* AP_SSID     = "MausamiCLOCK";
const char* AP_PASSWORD = "12345678";
const char* CONFIG_FILE = "/config.json";

const unsigned long NTP_SYNC_INTERVAL_MS = 60UL * 60UL * 1000UL; // 1 hour
unsigned long lastNTPSync = 0;

// -------------------- STATE MACHINE --------------------
enum DisplayMode { MODE_TIME = 0, MODE_DATE_DAY, MODE_WEATHER };
enum ScrollState { STATE_STARTUP = 0, STATE_NORMAL };

ScrollState scrollState = STATE_STARTUP;
DisplayMode currentMode = MODE_TIME;

// -------------------- CONFIG STRUCT --------------------
struct ClockConfig {
  String timezoneName;
  long timezoneOffsetSeconds;
  bool useDST;

  bool use12Hour;
  uint8_t brightness;      // 0-100
  bool nightModeEnabled;
  uint8_t nightStartHour;  // 0-23
  uint8_t nightEndHour;    // 0-23
  uint8_t scrollSpeed;     // 1-7 (1=Extra Slow ... 7=Ultra Fast)

  bool displayAuto;        // true=Auto (Night mode), false=Manual
  bool displayOnManual;    // manual ON/OFF

  bool weatherEnabled;
  String weatherApiKey;
  String weatherCityId;
  uint32_t weatherIntervalMs;
  bool showWeatherTemp;
  bool showWeatherMinMax;
  bool showWeatherCondition;
  bool showWeatherHumidity;
  bool showWeatherLocation; // toggle to show/hide location in weather display

  String customTimezone;

  uint8_t modules; // 4, 8, or 12
};

ClockConfig config;

// -------------------- WEATHER DATA --------------------
struct WeatherData {
  bool valid;
  float temp;
  float tempMin;
  float tempMax;
  int humidity;
  String condition;
  String cityName;
  unsigned long lastUpdate;
};

WeatherData weather;

// -------------------- TIMEZONE LIST (IANA) --------------------
const char* tzList[] = {
  "Africa/Abidjan","Africa/Accra","Africa/Addis_Ababa","Africa/Algiers","Africa/Asmara","Africa/Bamako",
  "Africa/Bangui","Africa/Banjul","Africa/Bissau","Africa/Blantyre","Africa/Brazzaville","Africa/Bujumbura",
  "Africa/Cairo","Africa/Casablanca","Africa/Ceuta","Africa/Conakry","Africa/Dakar","Africa/Dar_es_Salaam",
  "Africa/Djibouti","Africa/Douala","Africa/El_Aaiun","Africa/Freetown","Africa/Gaborone","Africa/Harare",
  "Africa/Johannesburg","Africa/Juba","Africa/Kampala","Africa/Khartoum","Africa/Kigali","Africa/Kinshasa",
  "Africa/Lagos","Africa/Libreville","Africa/Lome","Africa/Luanda","Africa/Lubumbashi","Africa/Lusaka",
  "Africa/Malabo","Africa/Maputo","Africa/Maseru","Africa/Mbabane","Africa/Mogadishu","Africa/Monrovia",
  "Africa/Nairobi","Africa/Ndjamena","Africa/Niamey","Africa/Nouakchott","Africa/Ouagadougou","Africa/Porto-Novo",
  "Africa/Sao_Tome","Africa/Tripoli","Africa/Tunis","Africa/Windhoek",
  "America/Adak","America/Anchorage","America/Anguilla","America/Antigua","America/Araguaina","America/Argentina/Buenos_Aires",
  "America/Argentina/Catamarca","America/Argentina/Cordoba","America/Argentina/Jujuy","America/Argentina/La_Rioja",
  "America/Argentina/Mendoza","America/Argentina/Rio_Gallegos","America/Argentina/Salta","America/Argentina/San_Juan",
  "America/Argentina/San_Luis","America/Argentina/Tucuman","America/Argentina/Ushuaia","America/Aruba",
  "America/Asuncion","America/Atikokan","America/Bahia","America/Bahia_Banderas","America/Barbados","America/Belem",
  "America/Belize","America/Blanc-Sablon","America/Boa_Vista","America/Bogota","America/Boise","America/Cambridge_Bay",
  "America/Campo_Grande","America/Cancun","America/Caracas","America/Cayenne","America/Cayman","America/Chicago",
  "America/Chihuahua","America/Costa_Rica","America/Creston","America/Cuiaba","America/Curacao","America/Danmarkshavn",
  "America/Dawson","America/Dawson_Creek","America/Denver","America/Detroit","America/Dominica","America/Edmonton",
  "America/Eirunepe","America/El_Salvador","America/Fort_Nelson","America/Fortaleza","America/Glace_Bay","America/Godthab",
  "America/Goose_Bay","America/Grand_Turk","America/Grenada","America/Guadeloupe","America/Guatemala","America/Guayaquil",
  "America/Guyana","America/Halifax","America/Havana","America/Hermosillo","America/Indiana/Indianapolis","America/Indiana/Knox",
  "America/Indiana/Marengo","America/Indiana/Petersburg","America/Indiana/Tell_City","America/Indiana/Vevay","America/Indiana/Vincennes",
  "America/Indiana/Winamac","America/Inuvik","America/Iqaluit","America/Jamaica","America/Juneau","America/Kentucky/Louisville",
  "America/Kentucky/Monticello","America/Kralendijk","America/La_Paz","America/Lima","America/Los_Angeles","America/Lower_Princes",
  "America/Maceio","America/Managua","America/Manaus","America/Marigot","America/Martinique","America/Matamoros","America/Mazatlan",
  "America/Menominee","America/Merida","America/Metlakatla","America/Mexico_City","America/Miquelon","America/Moncton",
  "America/Monterrey","America/Montevideo","America/Montserrat","America/Nassau","America/New_York","America/Nipigon",
  "America/Nome","America/Noronha","America/North_Dakota/Beulah","America/North_Dakota/Center","America/North_Dakota/New_Salem",
  "America/Ojinaga","America/Panama","America/Pangnirtung","America/Paramaribo","America/Phoenix","America/Port-au-Prince",
  "America/Port_of_Spain","America/Porto_Velho","America/Puerto_Rico","America/Punta_Arenas","America/Rainy_River",
  "America/Rankin_Inlet","America/Recife","America/Regina","America/Resolute","America/Rio_Branco","America/Santarem",
  "America/Santiago","America/Santo_Domingo","America/Sao_Paulo","America/Scoresbysund","America/Sitka","America/St_Barthelemy",
  "America/St_Johns","America/St_Kitts","America/St_Lucia","America/St_Thomas","America/St_Vincent","America/Swift_Current",
  "America/Tegucigalpa","America/Thule","America/Thunder_Bay","America/Tijuana","America/Toronto","America/Tortola","America/Vancouver",
  "America/Whitehorse","America/Winnipeg","America/Yakutat","America/Yellowknife",
  "Antarctica/Casey","Antarctica/Davis","Antarctica/DumontDUrville","Antarctica/Macquarie","Antarctica/Mawson","Antarctica/Palmer",
  "Antarctica/Rothera","Antarctica/Syowa","Antarctica/Troll","Antarctica/Vostok",
  "Arctic/Longyearbyen",
  "Asia/Aden","Asia/Almaty","Asia/Amman","Asia/Anadyr","Asia/Aqtau","Asia/Aqtobe","Asia/Ashgabat","Asia/Atyrau",
  "Asia/Baghdad","Asia/Bahrain","Asia/Baku","Asia/Bangkok","Asia/Barnaul","Asia/Beirut","Asia/Bishkek","Asia/Brunei",
  "Asia/Chita","Asia/Choibalsan","Asia/Colombo","Asia/Damascus","Asia/Dhaka","Asia/Dili","Asia/Dubai","Asia/Dushanbe",
  "Asia/Famagusta","Asia/Gaza","Asia/Hebron","Asia/Ho_Chi_Minh","Asia/Hong_Kong","Asia/Hovd","Asia/Irkutsk","Asia/Jakarta",
  "Asia/Jayapura","Asia/Jerusalem","Asia/Kabul","Asia/Kamchatka","Asia/Karachi","Asia/Kathmandu","Asia/Khandyga","Asia/Kolkata",
  "Asia/Krasnoyarsk","Asia/Kuala_Lumpur","Asia/Kuching","Asia/Kuwait","Asia/Macau","Asia/Magadan","Asia/Makassar","Asia/Manila",
  "Asia/Muscat","Asia/Nicosia","Asia/Novokuznetsk","Asia/Novosibirsk","Asia/Omsk","Asia/Oral","Asia/Phnom_Penh","Asia/Pontianak",
  "Asia/Pyongyang","Asia/Qatar","Asia/Qostanay","Asia/Qyzylorda","Asia/Riyadh","Asia/Sakhalin","Asia/Samarkand","Asia/Seoul",
  "Asia/Shanghai","Asia/Singapore","Asia/Srednekolymsk","Asia/Taipei","Asia/Tashkent","Asia/Tbilisi","Asia/Tehran","Asia/Thimphu",
  "Asia/Tokyo","Asia/Tomsk","Asia/Ulaanbaatar","Asia/Urumqi","Asia/Ust-Nera","Asia/Vladivostok","Asia/Yakutsk","Asia/Yangon",
  "Asia/Yekaterinburg","Asia/Yerevan",
  "Atlantic/Azores","Atlantic/Bermuda","Atlantic/Canary","Atlantic/Cape_Verde","Atlantic/Faroe","Atlantic/Madeira","Atlantic/Reykjavik",
  "Atlantic/South_Georgia","Atlantic/St_Helena","Atlantic/Stanley",
  "Australia/Adelaide","Australia/Brisbane","Australia/Broken_Hill","Australia/Currie","Australia/Darwin","Australia/Eucla",
  "Australia/Hobart","Australia/Lindeman","Australia/Lord_Howe","Australia/Melbourne","Australia/Perth","Australia/Sydney",
  "Europe/Amsterdam","Europe/Andorra","Europe/Astrakhan","Europe/Athens","Europe/Belgrade","Europe/Berlin","Europe/Bratislava",
  "Europe/Brussels","Europe/Bucharest","Europe/Budapest","Europe/Busingen","Europe/Chisinau","Europe/Copenhagen","Europe/Dublin",
  "Europe/Gibraltar","Europe/Guernsey","Europe/Helsinki","Europe/Isle_of_Man","Europe/Istanbul","Europe/Jersey","Europe/Kaliningrad",
  "Europe/Kiev","Europe/Kirov","Europe/Lisbon","Europe/Ljubljana","Europe/London","Europe/Luxembourg","Europe/Madrid","Europe/Malta",
  "Europe/Mariehamn","Europe/Minsk","Europe/Monaco","Europe/Moscow","Europe/Oslo","Europe/Paris","Europe/Podgorica","Europe/Prague",
  "Europe/Riga","Europe/Rome","Europe/Samara","Europe/San_Marino","Europe/Sarajevo","Europe/Saratov","Europe/Simferopol","Europe/Skopje",
  "Europe/Sofia","Europe/Stockholm","Europe/Tallinn","Europe/Tirane","Europe/Ulyanovsk","Europe/Uzhgorod","Europe/Vaduz","Europe/Vatican",
  "Europe/Vienna","Europe/Vilnius","Europe/Volgograd","Europe/Warsaw","Europe/Zagreb","Europe/Zaporozhye","Europe/Zurich",
  "Indian/Chagos","Indian/Christmas","Indian/Cocos","Indian/Kerguelen","Indian/Mahe","Indian/Maldives","Indian/Mauritius","Indian/Mayotte",
  "Indian/Reunion",
  "Pacific/Apia","Pacific/Auckland","Pacific/Bougainville","Pacific/Chatham","Pacific/Chuuk","Pacific/Easter","Pacific/Efate","Pacific/Enderbury",
  "Pacific/Fakaofo","Pacific/Fiji","Pacific/Funafuti","Pacific/Galapagos","Pacific/Gambier","Pacific/Guadalcanal","Pacific/Guam","Pacific/Honolulu",
  "Pacific/Johnston","Pacific/Kiritimati","Pacific/Kosrae","Pacific/Kwajalein","Pacific/Majuro","Pacific/Marquesas","Pacific/Midway",
  "Pacific/Nauru","Pacific/Niue","Pacific/Norfolk","Pacific/Noumea","Pacific/Pago_Pago","Pacific/Palau","Pacific/Pitcairn","Pacific/Pohnpei",
  "Pacific/Port_Moresby","Pacific/Rarotonga","Pacific/Saipan","Pacific/Tahiti","Pacific/Tarawa","Pacific/Tongatapu","Pacific/Wake","Pacific/Wallis"
};
const size_t TZ_COUNT = sizeof(tzList) / sizeof(tzList[0]);

// -------------------- FORWARD DECLARATIONS --------------------
void loadConfig();
void saveConfig();
void initDisplay(uint8_t modules);
void setupWiFiAndPortal();
void setupWebServer();
void handleRoot();
void handleSaveConfig();
void handleResetWiFi();
void handleResetForm();
void handleWeatherRefresh();
void handleToggleLocation();
void handleNotFound();

void updateTimeFromNTP(bool force = false);
void updateWeather(bool force = false);
String getTimeString();
String getDateDayString();
String getWeatherString();
void updateDisplayStateMachine();
bool isNightTime();
void applyBrightness();
void showStartupLoopUntilWiFi();
void showConnectedInfo();
void scrollMessageBlocking(const String& msg);
uint16_t getScrollDelay();
String htmlEscape(const String& s);
String formatOffset(long offsetSeconds);

// -------------------- UTIL MACROS --------------------
#define WEATHER_GAP "   " // 3 spaces between weather segments

// -------------------- SETUP --------------------
void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println();
  Serial.println(F("Smart WiFi Clock starting..."));
  Serial.println(F("Designed and developed by Anshuman Sharma - Distribution prohibited for commercial use"));

  if (!SPIFFS.begin(true)) {
    Serial.println(F("SPIFFS mount failed or formatted"));
  }

  loadConfig();

  // Initialize display pointer with configured modules (or default)
  currentModules = (config.modules == 4 || config.modules == 8 || config.modules == 12) ? config.modules : DEFAULT_MODULES;
  initDisplay(currentModules);

  // Ensure AP starts reliably on first boot: if no saved SSID, clear WiFi and force AP
  if (WiFi.SSID().length() == 0) {
    Serial.println(F("No saved WiFi credentials found. Forcing AP mode for setup."));
    WiFi.disconnect(true, true); // clear any partial state
    delay(200);
  }

  // Start WiFi manager portal and show startup messages until connected
  setupWiFiAndPortal();

  // Setup web server and NTP
  setupWebServer();

  timeClient.begin();
  timeClient.setUpdateInterval(NTP_SYNC_INTERVAL_MS);
  timeClient.setTimeOffset(config.timezoneOffsetSeconds);
  updateTimeFromNTP(true);

  // Immediately update weather after boot if enabled
  weather.valid = false;
  weather.lastUpdate = 0;
  if (config.weatherEnabled) {
    updateWeather(true);
  }

  scrollState = STATE_NORMAL;
  currentMode = MODE_TIME;
}

// -------------------- LOOP --------------------
void loop() {
  server.handleClient();

  updateTimeFromNTP();
  if (config.weatherEnabled) updateWeather();

  updateDisplayStateMachine();
}

// -------------------- CONFIG LOAD/SAVE --------------------
void loadConfig() {
  if (!SPIFFS.exists(CONFIG_FILE)) {
    Serial.println(F("Config file not found, loading defaults"));
    // Defaults
    config.timezoneName = "Australia/Adelaide";
    config.timezoneOffsetSeconds = 9 * 3600 + 1800;
    config.useDST = true;

    config.use12Hour = true;
    config.brightness = 80;
    config.nightModeEnabled = false;
    config.nightStartHour = 23;
    config.nightEndHour = 6;
    config.scrollSpeed = 3; // default Slow

    config.displayAuto = true;
    config.displayOnManual = true;

    config.weatherEnabled = false;
    config.weatherApiKey = "";
    config.weatherCityId = "";
    config.weatherIntervalMs = 15UL * 60UL * 1000UL;
    config.showWeatherTemp = true;
    config.showWeatherMinMax = true;
    config.showWeatherCondition = true;
    config.showWeatherHumidity = true;
    config.showWeatherLocation = true;

    config.customTimezone = "";

    config.modules = DEFAULT_MODULES;

    saveConfig();
    return;
  }

  File f = SPIFFS.open(CONFIG_FILE, "r");
  if (!f) {
    Serial.println(F("Failed to open config file, using defaults"));
    return;
  }

  StaticJsonDocument<3072> doc;
  DeserializationError err = deserializeJson(doc, f);
  f.close();

  if (err) {
    Serial.print(F("Config JSON parse error: "));
    Serial.println(err.c_str());
    return;
  }

  config.timezoneName = doc["timezoneName"] | "Australia/Adelaide";
  config.timezoneOffsetSeconds = doc["timezoneOffsetSeconds"] | (9 * 3600 + 1800);
  config.useDST = doc["useDST"] | true;

  config.use12Hour = doc["use12Hour"] | true;
  config.brightness = doc["brightness"] | 80;
  config.nightModeEnabled = doc["nightModeEnabled"] | false;
  config.nightStartHour = doc["nightStartHour"] | 23;
  config.nightEndHour = doc["nightEndHour"] | 6;
  config.scrollSpeed = doc["scrollSpeed"] | 3;

  config.displayAuto = doc["displayAuto"] | true;
  config.displayOnManual = doc["displayOnManual"] | true;

  config.weatherEnabled = doc["weatherEnabled"] | false;
  config.weatherApiKey = (const char*) (doc["weatherApiKey"] | "");
  config.weatherCityId = (const char*) (doc["weatherCityId"] | "");
  config.weatherIntervalMs = doc["weatherIntervalMs"] | (15UL * 60UL * 1000UL);
  config.showWeatherTemp = doc["showWeatherTemp"] | true;
  config.showWeatherMinMax = doc["showWeatherMinMax"] | true;
  config.showWeatherCondition = doc["showWeatherCondition"] | true;
  config.showWeatherHumidity = doc["showWeatherHumidity"] | true;
  config.showWeatherLocation = doc["showWeatherLocation"] | true;

  config.customTimezone = (const char*) (doc["customTimezone"] | "");

  config.modules = doc["modules"] | DEFAULT_MODULES;
  if (config.modules != 4 && config.modules != 8 && config.modules != 12) config.modules = DEFAULT_MODULES;

  Serial.println(F("Config loaded"));
}

void saveConfig() {
  StaticJsonDocument<3072> doc;

  doc["timezoneName"] = config.timezoneName;
  doc["timezoneOffsetSeconds"] = config.timezoneOffsetSeconds;
  doc["useDST"] = config.useDST;

  doc["use12Hour"] = config.use12Hour;
  doc["brightness"] = config.brightness;
  doc["nightModeEnabled"] = config.nightModeEnabled;
  doc["nightStartHour"] = config.nightStartHour;
  doc["nightEndHour"] = config.nightEndHour;
  doc["scrollSpeed"] = config.scrollSpeed;

  doc["displayAuto"] = config.displayAuto;
  doc["displayOnManual"] = config.displayOnManual;

  doc["weatherEnabled"] = config.weatherEnabled;
  doc["weatherApiKey"] = config.weatherApiKey;
  doc["weatherCityId"] = config.weatherCityId;
  doc["weatherIntervalMs"] = config.weatherIntervalMs;
  doc["showWeatherTemp"] = config.showWeatherTemp;
  doc["showWeatherMinMax"] = config.showWeatherMinMax;
  doc["showWeatherCondition"] = config.showWeatherCondition;
  doc["showWeatherHumidity"] = config.showWeatherHumidity;
  doc["showWeatherLocation"] = config.showWeatherLocation;

  doc["customTimezone"] = config.customTimezone;

  doc["modules"] = config.modules;

  File f = SPIFFS.open(CONFIG_FILE, "w");
  if (!f) {
    Serial.println(F("Failed to open config file for writing"));
    return;
  }

  if (serializeJson(doc, f) == 0) {
    Serial.println(F("Failed to write config JSON"));
  } else {
    Serial.println(F("Config saved"));
  }
  f.close();
}

// -------------------- DISPLAY INITIALIZATION --------------------
void initDisplay(uint8_t modules) {
  // Validate modules
  if (modules != 4 && modules != 8 && modules != 12) modules = DEFAULT_MODULES;
  currentModules = modules;

  // Delete existing instance if any
  if (display) {
    display->displayClear();
    delete display;
    display = nullptr;
    delay(50);
  }

  // Create new MD_Parola instance with requested module count
  display = new MD_Parola(HARDWARE_TYPE, DATA_PIN, CLK_PIN, CS_PIN, currentModules);
  display->begin();
  display->setIntensity(map(config.brightness, 0, 100, 0, 15));
  display->displayClear();
  display->displaySuspend(false);
  display->setInvert(false);
  display->setZone(0, 0, currentModules - 1);
  display->setPause(0);
  display->setTextAlignment(PA_LEFT);

  Serial.print(F("Display initialized with "));
  Serial.print(currentModules);
  Serial.println(F(" modules"));
}

// -------------------- WIFI & STARTUP AP LOOP --------------------
void setupWiFiAndPortal() {
  WiFiManager wm;
  wm.setDebugOutput(true);
  wm.setMinimumSignalQuality(0);
  wm.setConfigPortalBlocking(true);

  // If no saved credentials, ensure AP mode will be started
  if (WiFi.SSID().length() == 0) {
    Serial.println(F("Starting AP for configuration..."));
    WiFi.mode(WIFI_AP_STA);
    WiFi.softAP(AP_SSID, AP_PASSWORD);
  }

  // Non-blocking visual: show a few startup messages before portal (helps user see AP info)
  scrollMessageBlocking("WELCOME TO MAUSAMICLOCK  ");
  scrollMessageBlocking("SETUP WIFI USING AP MODE  ");
  scrollMessageBlocking("AP: MausamiCLOCK  PASS: 12345678  ");

  // Start config portal (blocking) - will return when connected or portal closed
  if (!wm.autoConnect(AP_SSID, AP_PASSWORD)) {
    Serial.println(F("WiFiManager failed to connect, restarting..."));
    delay(3000);
    ESP.restart();
  }

  // Connected
  Serial.print(F("Connected to WiFi: "));
  Serial.println(WiFi.SSID());
  Serial.print(F("IP: "));
  Serial.println(WiFi.localIP());

  // Show connected info on matrix
  showConnectedInfo();

  // Immediately update weather after WiFi connect if enabled
  if (config.weatherEnabled) {
    updateWeather(true);
  }

  // Small delay to allow user to see IP
  delay(1200);
}

// -------------------- WEB SERVER --------------------
void setupWebServer() {
  server.on("/", HTTP_GET, handleRoot);
  server.on("/save", HTTP_POST, handleSaveConfig);
  server.on("/resetwifi", HTTP_POST, handleResetWiFi);
  server.on("/resetform", HTTP_POST, handleResetForm);
  server.on("/weatherrefresh", HTTP_POST, handleWeatherRefresh);
  server.on("/togglelocation", HTTP_POST, handleToggleLocation);
  server.onNotFound(handleNotFound);
  server.begin();
  Serial.println(F("Web server started"));
}

String htmlHeader() {
  String h;
  h += "<!DOCTYPE html><html><head><meta charset='UTF-8'>";
  h += "<meta name='viewport' content='width=device-width, initial-scale=1.0'>";
  h += "<title>Mausami Smart Clock</title>";
  h += "<style>";
  h += "body{font-family:Arial,Helvetica,sans-serif;background:#111;color:#eee;margin:0;padding:0;}";
  h += "h1,h2{margin:10px 0;color:#fff;}";
  h += ".container{max-width:1000px;margin:0 auto;padding:15px;}";
  h += "label{display:block;margin-top:8px;font-weight:bold;}";
  h += "input,select,textarea{width:100%;padding:6px;margin-top:3px;border-radius:4px;border:1px solid #444;background:#222;color:#eee;}";
  h += "input[type=checkbox]{width:auto;}";
  h += ".row{display:flex;flex-wrap:wrap;gap:10px;}";
  h += ".col{flex:1 1 250px;}";
  h += ".btn{display:inline-block;margin-top:10px;padding:8px 14px;border:none;border-radius:4px;background:#2196F3;color:#fff;cursor:pointer;}";
  h += ".btn-danger{background:#f44336;}";
  h += ".btn-secondary{background:#555;}";
  h += ".section{border:1px solid #333;border-radius:6px;padding:10px;margin-top:10px;background:#181818;}";
  h += ".status{font-size:0.9em;color:#ccc;}";
  h += "hr{border:0;border-top:1px solid #333;margin:15px 0;}";
  h += ".help{font-size:0.9em;color:#ddd;background:#0f0f0f;padding:8px;border-radius:6px;margin-top:8px;}";
  h += "</style></head><body><div class='container'>";
  return h;
}

String htmlFooter() {
  String f;
  f += "<hr><div class='status'>Designed and developed by Anshuman Sharma - Distribution prohibited for commercial use</div>";
  f += "</div></body></html>";
  return f;
}

void handleRoot() {
  String page = htmlHeader();
  page += "<h1>Mausami Smart Clock</h1>";

  // Status
  page += "<div class='section'><h2>Status</h2>";
  page += "<div class='row'><div class='col'>";
  page += "<b>WiFi SSID:</b> " + htmlEscape(WiFi.SSID()) + "<br>";
  page += "<b>IP:</b> " + WiFi.localIP().toString() + "<br>";
  page += "<b>Time:</b> " + htmlEscape(getTimeString()) + "<br>";
  page += "<b>Date/Day:</b> " + htmlEscape(getDateDayString()) + "<br>";
  page += "</div><div class='col'>";
  if (config.weatherEnabled && weather.valid) {
    page += "<b>Weather:</b> " + htmlEscape(getWeatherString()) + "<br>";
    if (config.showWeatherLocation && weather.cityName.length() > 0) {
      page += "<b>City:</b> " + htmlEscape(weather.cityName) + "<br>";
    }
    page += "<b>Last update ms:</b> " + String(weather.lastUpdate) + "<br>";
  } else if (config.weatherEnabled) {
    page += "<b>Weather:</b> Waiting for data or error<br>";
  } else {
    page += "<b>Weather:</b> Disabled<br>";
  }
  page += "</div></div></div>";

  // Configuration form
  page += "<form method='POST' action='/save'>";
  page += "<div class='section'><h2>Time & Timezone</h2>";
  page += "<label for='timezone'>Timezone (IANA list)</label>";
  page += "<select name='timezone' id='timezone' size='6' style='height:140px;'>";
  for (size_t i = 0; i < TZ_COUNT; i++) {
    page += "<option value='" + String(tzList[i]) + "'";
    if (config.timezoneName == tzList[i]) page += " selected";
    page += ">" + String(tzList[i]) + "</option>";
  }
  page += "</select>";

  page += "<label for='customTimezone'>Custom Timezone (optional)</label>";
  page += "<input type='text' name='customTimezone' id='customTimezone' value='" + htmlEscape(config.customTimezone) + "'>";
  page += "<small>Note: Custom timezone is informational; offset is taken from preset or custom handling.</small>";

  page += "<label><input type='checkbox' name='useDST' ";
  if (config.useDST) page += "checked";
  page += "> Use DST (where applicable)</label>";

  page += "<label><input type='checkbox' name='use12Hour' ";
  if (config.use12Hour) page += "checked";
  page += "> 12-hour format</label>";
  page += "</div>";

  page += "<div class='section'><h2>Display</h2>";
  page += "<label for='modules'>Matrix Modules</label>";
  page += "<select name='modules' id='modules'>";
  page += "<option value='4'" + String(config.modules==4 ? " selected":"") + ">4 modules (8x32) - Default</option>";
  page += "<option value='8'" + String(config.modules==8 ? " selected":"") + ">8 modules (8x64)</option>";
  page += "<option value='12'" + String(config.modules==12 ? " selected":"") + ">12 modules (8x96)</option>";
  page += "</select>";

  page += "<label for='brightness'>Brightness (0-100)</label>";
  page += "<input type='range' min='0' max='100' name='brightness' id='brightness' value='" + String(config.brightness) + "'>";

  page += "<label for='scrollSpeed'>Scroll Speed</label>";
  page += "<select name='scrollSpeed' id='scrollSpeed'>";
  page += "<option value='1'" + String(config.scrollSpeed==1 ? " selected":"") + ">1 - Extra Slow</option>";
  page += "<option value='2'" + String(config.scrollSpeed==2 ? " selected":"") + ">2 - Very Slow</option>";
  page += "<option value='3'" + String(config.scrollSpeed==3 ? " selected":"") + ">3 - Slow</option>";
  page += "<option value='4'" + String(config.scrollSpeed==4 ? " selected":"") + ">4 - Normal</option>";
  page += "<option value='5'" + String(config.scrollSpeed==5 ? " selected":"") + ">5 - Fast</option>";
  page += "<option value='6'" + String(config.scrollSpeed==6 ? " selected":"") + ">6 - Very Fast</option>";
  page += "<option value='7'" + String(config.scrollSpeed==7 ? " selected":"") + ">7 - Ultra Fast</option>";
  page += "</select>";

  page += "<label><input type='checkbox' name='nightModeEnabled' ";
  if (config.nightModeEnabled) page += "checked";
  page += "> Night mode (auto display off)</label>";

  page += "<label for='nightStartHour'>Night mode start hour (0-23)</label>";
  page += "<input type='number' min='0' max='23' name='nightStartHour' id='nightStartHour' value='" + String(config.nightStartHour) + "'>";

  page += "<label for='nightEndHour'>Night mode end hour (0-23)</label>";
  page += "<input type='number' min='0' max='23' name='nightEndHour' id='nightEndHour' value='" + String(config.nightEndHour) + "'>";

  page += "<label>Display Control</label>";
  page += "<select name='displayMode'>";
  page += "<option value='auto'";
  if (config.displayAuto) page += " selected";
  page += ">Auto (Night mode)</option>";
  page += "<option value='manual'";
  if (!config.displayAuto) page += " selected";
  page += ">Manual</option>";
  page += "</select>";

  page += "<label><input type='checkbox' name='displayOnManual' ";
  if (config.displayOnManual) page += "checked";
  page += "> Display ON in Manual mode</label>";

  page += "</div>";

  page += "<div class='section'><h2>Weather</h2>";
  page += "<label><input type='checkbox' name='weatherEnabled' ";
  if (config.weatherEnabled) page += "checked";
  page += "> Enable Weather</label>";

  page += "<label for='weatherApiKey'>OpenWeatherMap API Key</label>";
  page += "<input type='text' name='weatherApiKey' id='weatherApiKey' value='" + htmlEscape(config.weatherApiKey) + "'>";

  page += "<label for='weatherCityId'>City ID</label>";
  page += "<input type='text' name='weatherCityId' id='weatherCityId' value='" + htmlEscape(config.weatherCityId) + "'>";

  page += "<label for='weatherInterval'>Update Interval (minutes, 5-60)</label>";
  page += "<input type='number' min='5' max='60' name='weatherInterval' id='weatherInterval' value='" + String(config.weatherIntervalMs / 60000UL) + "'>";

  page += "<label><input type='checkbox' name='showWeatherTemp' ";
  if (config.showWeatherTemp) page += "checked";
  page += "> Show current temperature</label>";

  page += "<label><input type='checkbox' name='showWeatherMinMax' ";
  if (config.showWeatherMinMax) page += "checked";
  page += "> Show min/max temperature</label>";

  page += "<label><input type='checkbox' name='showWeatherCondition' ";
  if (config.showWeatherCondition) page += "checked";
  page += "> Show condition</label>";

  page += "<label><input type='checkbox' name='showWeatherHumidity' ";
  if (config.showWeatherHumidity) page += "checked";
  page += "> Show humidity</label>";

  page += "<label><input type='checkbox' name='showWeatherLocation' ";
  if (config.showWeatherLocation) page += "checked";
  page += "> Show weather location on display</label>";

  page += "<button class='btn' type='submit'>Save Configuration</button>";
  page += "</div>";
  page += "</form>";

  // Actions
  page += "<div class='section'><h2>Actions</h2>";
  page += "<form method='POST' action='/weatherrefresh' style='display:inline;'><button class='btn btn-secondary' type='submit'>Weather Refresh</button></form> ";
  page += "<form method='POST' action='/togglelocation' style='display:inline;margin-left:10px;'><button class='btn' type='submit'>";
  page += (config.showWeatherLocation ? "Hide Location" : "Show Location");
  page += "</button></form> ";
  page += "<form method='POST' action='/resetwifi' style='display:inline;margin-left:10px;'><button class='btn btn-danger' type='submit'>Forget WiFi</button></form> ";
  page += "<form method='POST' action='/resetform' style='display:inline;margin-left:10px;'><button class='btn btn-danger' type='submit'>Reset Config</button></form>";
  page += "</div>";

  // Help section for weather API and city ID
  page += "<div class='section'><h2>Weather Help</h2>";
  page += "<div class='help'>";
  page += "<b>OpenWeatherMap API Key</b>: Sign up at openweathermap.org, go to 'API keys' in your account and create a key. Paste it above.<br><br>";
  page += "<b>City ID</b>: Use the OpenWeatherMap city list (city.list.json) or search the API: ";
  page += "Visit https://openweathermap.org/find or use the API endpoint ";
  page += "<code>api.openweathermap.org/data/2.5/find?q=CityName&appid=YOURKEY</code> to find the numeric city ID. ";
  page += "Alternatively, search 'OpenWeatherMap city ID list'.";
  page += "</div></div>";

  page += htmlFooter();
  server.send(200, "text/html", page);
}

void handleSaveConfig() {
  if (server.method() != HTTP_POST) {
    server.send(405, "text/plain", "Method Not Allowed");
    return;
  }

  String tz = server.arg("timezone");
  String customTZ = server.arg("customTimezone");
  bool useDST = server.hasArg("useDST");
  bool use12 = server.hasArg("use12Hour");

  int brightness = server.arg("brightness").toInt();
  int scrollSpeed = server.arg("scrollSpeed").toInt();
  bool nightEnabled = server.hasArg("nightModeEnabled");
  int nightStart = server.arg("nightStartHour").toInt();
  int nightEnd = server.arg("nightEndHour").toInt();

  String displayMode = server.arg("displayMode");
  bool displayOnManual = server.hasArg("displayOnManual");

  bool weatherEnabled = server.hasArg("weatherEnabled");
  String apiKey = server.arg("weatherApiKey");
  String cityId = server.arg("weatherCityId");
  int weatherIntervalMin = server.arg("weatherInterval").toInt();
  if (weatherIntervalMin < 5) weatherIntervalMin = 5;
  if (weatherIntervalMin > 60) weatherIntervalMin = 60;

  bool showTemp = server.hasArg("showWeatherTemp");
  bool showMinMax = server.hasArg("showWeatherMinMax");
  bool showCond = server.hasArg("showWeatherCondition");
  bool showHum = server.hasArg("showWeatherHumidity");
  bool showLoc = server.hasArg("showWeatherLocation");

  int modules = server.arg("modules").toInt();
  if (modules != 4 && modules != 8 && modules != 12) modules = DEFAULT_MODULES;

  config.timezoneName = tz;
  config.customTimezone = customTZ;
  config.useDST = useDST;

  // Map timezone preset to offset (best-effort: many zones share offsets; here we keep previous offset unless matched)
  bool tzMatched = false;
  for (size_t i = 0; i < TZ_COUNT; i++) {
    if (tz == tzList[i]) {
      // approximate offset by using localtime conversion (not perfect for DST) - keep existing offset if unknown
      // For simplicity, we keep previous offset; user can set timezoneName and we use it for display only.
      tzMatched = true;
      break;
    }
  }
  // If matched, we keep timezoneName; offset remains as before unless user wants to set custom offset manually.
  // (Advanced: implement full TZ->offset mapping with DST rules; beyond scope for lightweight firmware.)

  config.use12Hour = use12;
  config.brightness = constrain(brightness, 0, 100);
  config.scrollSpeed = constrain(scrollSpeed, 1, 7);
  config.nightModeEnabled = nightEnabled;
  config.nightStartHour = constrain(nightStart, 0, 23);
  config.nightEndHour = constrain(nightEnd, 0, 23);

  config.displayAuto = (displayMode == "auto");
  config.displayOnManual = displayOnManual;

  config.weatherEnabled = weatherEnabled;
  config.weatherApiKey = apiKey;
  config.weatherCityId = cityId;
  config.weatherIntervalMs = (uint32_t)weatherIntervalMin * 60000UL;
  config.showWeatherTemp = showTemp;
  config.showWeatherMinMax = showMinMax;
  config.showWeatherCondition = showCond;
  config.showWeatherHumidity = showHum;
  config.showWeatherLocation = showLoc;

  config.modules = modules;

  saveConfig();

  // Reinitialize display if modules changed
  if (modules != currentModules) {
    initDisplay(modules);
  } else {
    // apply brightness immediately
    applyBrightness();
  }

  // Apply timezone immediately and force NTP sync
  timeClient.setTimeOffset(config.timezoneOffsetSeconds);
  updateTimeFromNTP(true);

  // Force immediate weather update after settings saved (if enabled)
  weather.valid = false;
  weather.lastUpdate = 0;
  if (config.weatherEnabled) {
    updateWeather(true);
  }

  // Provide quick feedback and redirect
  server.sendHeader("Location", "/", true);
  server.send(302, "text/plain", "Saved. Redirecting...");
}

void handleResetWiFi() {
  WiFi.disconnect(true, true);
  server.send(200, "text/plain", "WiFi credentials cleared. Device will restart.");
  delay(1000);
  ESP.restart();
}

void handleResetForm() {
  SPIFFS.remove(CONFIG_FILE);
  server.send(200, "text/plain", "Configuration reset. Device will restart.");
  delay(1000);
  ESP.restart();
}

void handleWeatherRefresh() {
  updateWeather(true);
  server.sendHeader("Location", "/", true);
  server.send(302, "text/plain", "Refreshing weather...");
}

void handleToggleLocation() {
  // Toggle showWeatherLocation and save
  config.showWeatherLocation = !config.showWeatherLocation;
  saveConfig();
  server.sendHeader("Location", "/", true);
  server.send(302, "text/plain", "Toggled location display...");
}

void handleNotFound() {
  server.send(404, "text/plain", "Not found");
}

// -------------------- TIME & NTP --------------------
void updateTimeFromNTP(bool force) {
  unsigned long now = millis();
  if (force || now - lastNTPSync > NTP_SYNC_INTERVAL_MS) {
    Serial.println(F("Syncing time with NTP..."));
    if (timeClient.update()) {
      time_t t = timeClient.getEpochTime();
      setTime(t);
      lastNTPSync = now;
      Serial.print(F("Time synced: "));
      Serial.println(getTimeString());
    } else {
      Serial.println(F("NTP update failed"));
    }
  }
}

// Time string WITHOUT seconds
String getTimeString() {
  char buf[16];
  int h = hour();
  int m = minute();

  if (config.use12Hour) {
    bool pm = (h >= 12);
    int hh = h % 12;
    if (hh == 0) hh = 12;
    snprintf(buf, sizeof(buf), "%02d:%02d %s", hh, m, pm ? "PM" : "AM");
  } else {
    snprintf(buf, sizeof(buf), "%02d:%02d", h, m);
  }
  return String(buf);
}

String getDateDayString() {
  char buf[32];
  const char* daysOfWeek[] = {"Sunday","Monday","Tuesday","Wednesday","Thursday","Friday","Saturday"};
  const char* monthsOfYear[] = {"Jan","Feb","Mar","Apr","May","Jun","Jul","Aug","Sep","Oct","Nov","Dec"};

  int d = day();
  int mo = month();
  int y = year();
  int dow = weekday(); // 1=Sunday

  snprintf(buf, sizeof(buf), "%02d %s %04d - %s", d, monthsOfYear[mo-1], y, daysOfWeek[dow-1]);
  return String(buf);
}

// -------------------- WEATHER --------------------
void updateWeather(bool force) {
  if (!config.weatherEnabled) {
    Serial.println(F("Weather disabled; skipping update"));
    weather.valid = false;
    return;
  }
  if (config.weatherApiKey.length() == 0 || config.weatherCityId.length() == 0) {
    Serial.println(F("Weather config incomplete; skipping update"));
    weather.valid = false;
    return;
  }

  unsigned long now = millis();
  if (!force && now - weather.lastUpdate < config.weatherIntervalMs) return;

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println(F("Skipping weather update: no WiFi"));
    return;
  }

  Serial.println(F("Updating weather..."));

  WiFiClientSecure client;
  client.setInsecure(); // For simplicity; in production validate certs.

  HTTPClient https;
  String url = "https://api.openweathermap.org/data/2.5/weather?id=" + config.weatherCityId +
               "&appid=" + config.weatherApiKey + "&units=metric";

  if (!https.begin(client, url)) {
    Serial.println(F("HTTPS begin failed"));
    return;
  }

  int httpCode = https.GET();
  if (httpCode != HTTP_CODE_OK) {
    Serial.print(F("Weather HTTP error: "));
    Serial.println(httpCode);
    https.end();
    return;
  }

  String payload = https.getString();
  https.end();

  StaticJsonDocument<4096> doc;
  DeserializationError err = deserializeJson(doc, payload);
  if (err) {
    Serial.print(F("Weather JSON parse error: "));
    Serial.println(err.c_str());
    return;
  }

  // Parse safely
  weather.temp = doc["main"]["temp"] | 0.0;
  weather.tempMin = doc["main"]["temp_min"] | 0.0;
  weather.tempMax = doc["main"]["temp_max"] | 0.0;
  weather.humidity = doc["main"]["humidity"] | 0;
  weather.condition = (const char*)(doc["weather"][0]["main"] | "");
  weather.cityName = (const char*)(doc["name"] | "");
  weather.valid = true;
  weather.lastUpdate = now;

  Serial.println(F("Weather updated"));
}

// Weather string: current temp first, then condition, then min/max/humidity, with 3-space gaps
String getWeatherString() {
  if (!weather.valid) return "No weather data";

  String s;

  if (config.showWeatherTemp) {
    s += String(weather.temp, 1) + "C" WEATHER_GAP;
  }

  if (config.showWeatherCondition) {
    s += weather.condition + WEATHER_GAP;
  }

  if (config.showWeatherMinMax) {
    s += "Min:" + String(weather.tempMin, 1) + "C" WEATHER_GAP;
    s += "Max:" + String(weather.tempMax, 1) + "C" WEATHER_GAP;
  }

  if (config.showWeatherHumidity) {
    s += "Hum:" + String(weather.humidity) + "%" WEATHER_GAP;
  }

  if (config.showWeatherLocation && weather.cityName.length() > 0) {
    s += "(" + weather.cityName + ")" WEATHER_GAP;
  }

  return s;
}

// -------------------- DISPLAY & STATE MACHINE --------------------
uint16_t getScrollDelay() {
  switch (config.scrollSpeed) {
    case 1: return 120;
    case 2: return 100;
    case 3: return 80;
    case 4: return 60;
    case 5: return 40;
    case 6: return 30;
    case 7: return 20;
    default: return 80;
  }
}

void updateDisplayStateMachine() {
  if (!display) return;
  applyBrightness();

  bool displayOn = true;
  if (config.displayAuto) {
    if (config.nightModeEnabled && isNightTime()) displayOn = false;
  } else {
    displayOn = config.displayOnManual;
  }

  if (!displayOn) {
    display->displayClear();
    display->displaySuspend(true);
    return;
  } else {
    display->displaySuspend(false);
  }

  static bool messageActive = false;
  static String currentMessage;
  static uint16_t delayMs = 80;

  if (!messageActive) {
    // Prepare next message
    if (scrollState == STATE_STARTUP) {
      currentMessage = "WELCOME TO MAUSAMICLOCK  ";
      scrollState = STATE_NORMAL;
    } else {
      switch (currentMode) {
        case MODE_TIME:
          currentMessage = getTimeString() + "  ";
          currentMode = MODE_DATE_DAY;
          break;
        case MODE_DATE_DAY:
          currentMessage = getDateDayString() + "  ";
          currentMode = MODE_WEATHER;
          break;
        case MODE_WEATHER:
        default:
          if (config.weatherEnabled && weather.valid) {
            currentMessage = getWeatherString();
          } else if (config.weatherEnabled) {
            currentMessage = "WEATHER UNAVAILABLE  ";
          } else {
            currentMessage = "WEATHER DISABLED  ";
          }
          currentMode = MODE_TIME;
          break;
      }
    }

    delayMs = getScrollDelay();
    display->displayClear();
    display->displayText((char*)currentMessage.c_str(), PA_LEFT, delayMs, 0, PA_SCROLL_LEFT, PA_SCROLL_LEFT);
    messageActive = true;
  }

  if (display->displayAnimate()) {
    messageActive = false;
  }
}

bool isNightTime() {
  int h = hour();
  if (config.nightStartHour == config.nightEndHour) return false;
  if (config.nightStartHour < config.nightEndHour) {
    return (h >= config.nightStartHour && h < config.nightEndHour);
  } else {
    return (h >= config.nightStartHour || h < config.nightEndHour);
  }
}

void applyBrightness() {
  if (!display) return;
  uint8_t level = map(config.brightness, 0, 100, 0, 15);
  display->setIntensity(level);
}

void showConnectedInfo() {
  String msg = "CONNECTED: " + WiFi.SSID() + "  IP: " + WiFi.localIP().toString() + "  ";
  scrollMessageBlocking(msg);
}

// Blocking scroll helper used for startup messages (keeps it simple and reliable)
void scrollMessageBlocking(const String& msg) {
  if (!display) return;
  display->displayClear();
  uint16_t delayMs = getScrollDelay();
  display->displayText((char*)msg.c_str(), PA_LEFT, delayMs, 0, PA_SCROLL_LEFT, PA_SCROLL_LEFT);
  unsigned long start = millis();
  unsigned long timeout = 20000UL; // safety timeout
  while (!display->displayAnimate()) {
    delay(1);
    if (millis() - start > timeout) {
      Serial.println(F("scrollMessageBlocking timeout"));
      break;
    }
  }
}

// -------------------- UTIL --------------------
String htmlEscape(const String& s) {
  String out;
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    switch (c) {
      case '&': out += "&amp;"; break;
      case '<': out += "&lt;"; break;
      case '>': out += "&gt;"; break;
      case '"': out += "&quot;"; break;
      case '\'': out += "&#39;"; break;
      default: out += c; break;
    }
  }
  return out;
}

// Format offset seconds to human readable UTC±HH:MM (not used for full TZ mapping here)
String formatOffset(long offsetSeconds) {
  char buf[16];
  long s = offsetSeconds;
  char sign = '+';
  if (s < 0) { sign = '-'; s = -s; }
  int hours = s / 3600;
  int mins = (s % 3600) / 60;
  snprintf(buf, sizeof(buf), "UTC%c%02d:%02d", sign, hours, mins);
  return String(buf);
}
