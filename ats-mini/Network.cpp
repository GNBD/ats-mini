#include "Common.h"
#include "Storage.h"
#include "Themes.h"
#include "Utils.h"
#include "Menu.h"
#include "Draw.h"
#include "Splash.h"
#include "TcpMode.h"
#include "Ota.h"
#include "Remote.h"
#include "Button.h"

#include <WiFi.h>
#include <WiFiMulti.h>
#include <WiFiUdp.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <NTPClient.h>
#include <ESPmDNS.h>
#include <LittleFS.h>
#include <time.h>

#define CONNECT_TIME  3000  // Time of inactivity to start connecting WiFi
#define WIFI_MULTI_TOTAL_TIMEOUT  30000
#define SPLASH_MAX_FILE_SIZE (512U * 1024U)
#define WIFI_PAGE_SIZE       4
#define MAX_SCAN_NETWORKS    20
#define WIFI_CONNECT_TIMEOUT 15000
#define KB_MAX_KEYS          48
// wifi_power_t is 0.25 dBm units; Arduino has no WIFI_POWER_14dBm symbol
#define WIFI_POWER_14DBM_RAW ((wifi_power_t)56)

#ifndef WIFI_POWER_LEVEL
#define WIFI_POWER_LEVEL WIFI_POWER_14DBM_RAW
#endif

WiFiMulti wifiMulti;

//
// Access Point (AP) mode settings
//
static const char *apSSID    = RECEIVER_NAME;
static const char *apPWD     = 0;       // No password
static const int   apChannel = 10;      // WiFi channel number (1..13)
static const bool  apHideMe  = false;   // TRUE: disable SSID broadcast
static const int   apClients = 3;       // Maximum simultaneous connected clients

static uint16_t ajaxInterval = 2500;

volatile int web_event = 0;

static bool itIsTimeToWiFi = false; // TRUE: Need to connect to WiFi
static uint32_t connectTime = 0;

// Settings
String loginUsername = "";
String loginPassword = "";
static bool wifiScanHidden = false;

// AsyncWebServer object on port 80
AsyncWebServer server(80);

// NTP Client to get time
WiFiUDP ntpUDP;
NTPClient ntpClient(ntpUDP, "pool.ntp.org");

static bool wifiInitAP();
static bool wifiConnect();
static void webInit();
static void wifiRegisterPowerLevelCallback();
static void wifiPowerLevelOnEvent(WiFiEvent_t event);

static void webSetConfig(AsyncWebServerRequest *request);
static void webUploadSplash(AsyncWebServerRequest *request, const String &filename,
                            size_t index, uint8_t *data, size_t len, bool final);
static bool webIsAuthenticated(AsyncWebServerRequest *request);
static void webUploadFirmwareComplete(AsyncWebServerRequest *request);
static void webUpdatePage(AsyncWebServerRequest *request, const OtaStatus &status = otaStatus(), int code = 0);
static void webUploadFirmware(AsyncWebServerRequest *request, const String &filename,
                              size_t index, uint8_t *data, size_t len, bool final);
static bool webParseUTCDateTime(const String &text, uint32_t *epoch);

static const String webInputField(const String &name, const String &value, bool pass = false);
static const String webStyleSheet();
static const String webPage(const String &body);
static String webNavigation(const char *activePage);
static const String webUtcOffsetSelector();
static const String webThemeSelector();
static const String webStatusPage();
static const String webRemotePage();
static const String webMemoryPage();
static const String webConfigPage();

// API handlers
static void apiStatus(AsyncWebServerRequest *request);
static void apiCommand(AsyncWebServerRequest *request);
static void apiExport(AsyncWebServerRequest *request);
static void apiImport(AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total);
static void apiScreen(AsyncWebServerRequest *request);

struct SplashUploadState
{
  bool incomplete;
  bool tooLarge;
};

static bool webIsAuthenticated(AsyncWebServerRequest *request)
{
  return(loginUsername == "" || loginPassword == "" ||
         request->authenticate(loginUsername.c_str(), loginPassword.c_str()));
}

//
// Delayed WiFi connection
//
void netRequestConnect()
{
  connectTime = millis();
  itIsTimeToWiFi = true;
}

void netTickTime()
{
  otaTick();

  // Connect to WiFi if requested
  if(itIsTimeToWiFi && ((millis() - connectTime) > CONNECT_TIME))
  {
    netInit(wifiModeIdx);
    itIsTimeToWiFi = false;
  }
}

//
// Get current connection status
// (-1 - not connected, 0 - disabled, 1 - connected, 2 - connected to network)
//
int8_t getWiFiStatus()
{
  wifi_mode_t mode = WiFi.getMode();

  switch(mode)
  {
    case WIFI_MODE_NULL:
      return(0);
    case WIFI_AP:
      return(WiFi.softAPgetStationNum()? 1 : -1);
    case WIFI_STA:
      return(WiFi.status()==WL_CONNECTED? 2 : -1);
    case WIFI_AP_STA:
      return((WiFi.status()==WL_CONNECTED)? 2 : WiFi.softAPgetStationNum()? 1 : -1);
    default:
      return(-1);
  }
}

char *getWiFiIPAddress()
{
  static char ip[16];
  return strcpy(ip, WiFi.status()==WL_CONNECTED ? WiFi.localIP().toString().c_str() : "");
}

//
// Stop WiFi hardware
//
void netStop()
{
  tcpStop();
  wifi_mode_t mode = WiFi.getMode();

  MDNS.end();

  // If network connection up, shut it down
  if((mode==WIFI_STA) || (mode==WIFI_AP_STA))
    WiFi.disconnect(true);

  // If access point up, shut it down
  if((mode==WIFI_AP) || (mode==WIFI_AP_STA))
    WiFi.softAPdisconnect(true);

  WiFi.mode(WIFI_MODE_NULL);
}

//
// Initialize WiFi network and services
//
void netInit(uint8_t netMode)
{
  // Always disable WiFi first
  netStop();
  wifiRegisterPowerLevelCallback();

  switch(netMode)
  {
    case NET_OFF:
      // Do not initialize WiFi if disabled
      return;
    case NET_AP_ONLY:
      // Start WiFi access point if requested
      WiFi.mode(WIFI_AP);
      wifiInitAP();
      break;
    case NET_AP_CONNECT:
      // Start WiFi access point if requested
      WiFi.mode(WIFI_AP_STA);
      wifiInitAP();
      break;
    default:
      // No access point
      WiFi.mode(WIFI_STA);
      break;
  }

  // Initialize WiFi and try connecting to a network
  if(netMode>NET_AP_ONLY && wifiConnect())
  {
    // NTP time updates will happen every 5 minutes
    ntpClient.setUpdateInterval(5*60*1000);

    // Get NTP time from the network
    clockReset();
    for(int j=0 ; j<10 ; j++)
      if(ntpSyncTime()) break; else delay(500);

    // Start the result timeout after the blocking time synchronization.
    if(netMode!=NET_SYNC)
      statusShow(
        ("Connected to WiFi network (" + WiFi.SSID() + ")").c_str(),
        ("IP : " + WiFi.localIP().toString() + " or atsmini.local").c_str()
      );
    else
      statusShow(nullptr);
  }
  else if(netMode==NET_AP_ONLY || netMode==NET_AP_CONNECT)
  {
    // Show the access point details when it is the available connection.
    statusShow(
      ("Use Access Point " + String(apSSID)).c_str(),
      ("IP : " + WiFi.softAPIP().toString() + " or atsmini.local").c_str()
    );
  }
  else
    statusShow("Connecting to WiFi network...", "No WiFi connection");

  // If only connected to sync...
  if(netMode==NET_SYNC)
  {
    // Drop network connection
    WiFi.disconnect(true);
    WiFi.mode(WIFI_MODE_NULL);
  }
  else
  {
    // Initialize web server for remote configuration
    webInit();

    // Initialize mDNS
    MDNS.begin("atsmini"); // Set the hostname to "atsmini.local"
    MDNS.addService("http", "tcp", 80);
  }
}

//
// Returns TRUE if NTP time is available
//
bool ntpIsAvailable()
{
  return(ntpClient.isTimeSet());
}

//
// Update NTP time and synchronize clock with NTP time
//
bool ntpSyncTime()
{
  if(WiFi.status()==WL_CONNECTED)
  {
    ntpClient.update();

    if(ntpClient.isTimeSet())
      return(clockSetEpoch(ntpClient.getEpochTime()));
  }
  return(false);
}

static void wifiRegisterPowerLevelCallback()
{
  static bool registered = false;

  if(registered) return;

  WiFi.onEvent(wifiPowerLevelOnEvent, ARDUINO_EVENT_WIFI_AP_START);
  WiFi.onEvent(wifiPowerLevelOnEvent, ARDUINO_EVENT_WIFI_STA_START);
  registered = true;
}

static void wifiPowerLevelOnEvent(WiFiEvent_t event)
{
  (void)event;
  WiFi.setTxPower(WIFI_POWER_LEVEL);
}

//
// Initialize WiFi access point (AP)
//
static bool wifiInitAP()
{
  // These are our own access point (AP) addresses
  IPAddress ip(10, 1, 1, 1);
  IPAddress gateway(10, 1, 1, 1);
  IPAddress subnet(255, 255, 255, 0);

  // Start as access point (AP)
  WiFi.softAP(apSSID, apPWD, apChannel, apHideMe, apClients);
  WiFi.softAPConfig(ip, gateway, subnet);

  ajaxInterval = 2500;
  return(true);
}

//
// Connect to a WiFi network
//
static bool wifiConnect()
{
  // Clean credentials
  wifiMulti.APlistClean();

  // Get the preferences
  prefs.begin("network", true, STORAGE_PARTITION);
  loginUsername = prefs.getString("loginusername", "");
  loginPassword = prefs.getString("loginpassword", "");
  wifiScanHidden = prefs.getBool("wifiscanhidden", false);

  // Try connecting to known WiFi networks
  for(int j=0 ; (j<3) ; j++)
  {
    char nameSSID[16], namePASS[16];
    sprintf(nameSSID, "wifissid%d", j+1);
    sprintf(namePASS, "wifipass%d", j+1);

    String ssid = prefs.getString(nameSSID, "");
    String password = prefs.getString(namePASS, "");

    if(ssid != "")
      wifiMulti.addAP(ssid.c_str(), password.c_str());
  }

  // Done with preferences
  prefs.end();

  statusShow("Connecting to WiFi network...", nullptr, 0);
  drawScreen();

  consumeAbortPending();
  wl_status_t wifiStatus = WL_NO_SSID_AVAIL;
  uint32_t start = millis();
  while(((millis() - start)<WIFI_MULTI_TOTAL_TIMEOUT) && (wifiStatus!=WL_CONNECTED))
  {
    wifiStatus = (wl_status_t)wifiMulti.run(5000, wifiScanHidden);

    if(consumeAbortPending())
    {
      WiFi.disconnect();
      break;
    }

    if((wifiStatus!=WL_CONNECTED) && ((millis() - start)<WIFI_MULTI_TOTAL_TIMEOUT))
      delay(1000);
  }

  if(wifiStatus == WL_CONNECTED)
    ajaxInterval = 1000;

  return(wifiStatus == WL_CONNECTED);
}

//
// Wi-Fi Scan: list networks, pick one, recovery-style keypad password, connect
//
enum KbType : uint8_t { KB_CHAR = 0, KB_MODE, KB_BSP, KB_SPACE, KB_CANCEL };

struct KbKey
{
  int16_t x, y, w, h;
  char ch;
  uint8_t type;
};

static const char *KB_SETS[4] = {
  "abcdefghijklmnopqrstuvwxyz",
  "ABCDEFGHIJKLMNOPQRSTUVWXYZ",
  "1234567890",
  "!@#$%^&*()-_=+[]{};:'\",.<>/?\\|`~"
};
static const char *KB_MODE_NAMES[4] = { "abc", "ABC", "123", "#!@" };

static KbKey kbKeys[KB_MAX_KEYS];
static int kbKeyCount = 0;
static int kbCursor = 0;
static int kbMode = 0;

static void kbAddKey(int16_t x, int16_t y, int16_t w, int16_t h, char ch, uint8_t type)
{
  if(kbKeyCount >= KB_MAX_KEYS) return;
  kbKeys[kbKeyCount++] = {x, y, w, h, ch, type};
}

static void kbBuild(int mode, int contentBottom)
{
  kbMode = mode;
  kbKeyCount = 0;
  if(kbCursor >= KB_MAX_KEYS) kbCursor = 0;

  const int16_t kbTop = contentBottom + 4;
  const int16_t colW = 31;
  const int16_t rowH = 16;
  const int16_t gap = 1;
  const int16_t ox = 5;

  for(int m = 0; m < 4; m++)
    kbAddKey(ox + m * 79, kbTop, 77, 15, (char)m, KB_MODE);

  const char *set = KB_SETS[mode];
  int len = (int)strlen(set);
  int16_t ky = kbTop + 18;
  for(int i = 0; i < len; i++)
  {
    int row = i / 10;
    int col = i % 10;
    kbAddKey(ox + col * (colW + gap), ky + row * (rowH + gap), colW, rowH, set[i], KB_CHAR);
  }

  int rows = (len + 9) / 10;
  if(rows < 1) rows = 1;
  int16_t by = ky + rows * (rowH + gap) + 1;
  kbAddKey(ox, by, 50, rowH, 0, KB_BSP);
  kbAddKey(ox + 52, by, 50, rowH, 0, KB_CANCEL);
  kbAddKey(ox + 104, by, 210, rowH, ' ', KB_SPACE);

  if(kbCursor >= kbKeyCount) kbCursor = kbKeyCount - 1;
  if(kbCursor < 0) kbCursor = 0;
}

static void wifiSaveCredentials(const String &ssid, const String &pass)
{
  prefs.begin("network", false, STORAGE_PARTITION);
  int slot = 0, empty = 0;
  for(int j = 1; j <= 3; j++)
  {
    String key = "wifissid" + String(j);
    String s = prefs.getString(key.c_str(), "");
    if(s == ssid) { slot = j; break; }
    if(s == "" && !empty) empty = j;
  }
  if(!slot) slot = empty ? empty : 1;
  String ssidKey = "wifissid" + String(slot);
  String passKey = "wifipass" + String(slot);
  prefs.putString(ssidKey.c_str(), ssid);
  prefs.putString(passKey.c_str(), pass);
  prefs.end();
}

static bool wifiConnectBlocking(const String &ssid, const String &pass, uint32_t timeoutMs)
{
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.setTxPower(WIFI_POWER_LEVEL);
  WiFi.begin(ssid.c_str(), pass.c_str());
  uint32_t start = millis();
  while(WiFi.status() != WL_CONNECTED && (millis() - start) < timeoutMs)
    delay(50);
  return WiFi.status() == WL_CONNECTED;
}

static void drawWifiScanHeader(const char *title)
{
  spr.fillSprite(TH.bg);
  spr.fillRoundRect(0, 0, 320, 26, 0, TH.menu_bg);
  spr.drawLine(0, 26, 319, 26, TH.menu_border);
  spr.setTextDatum(MC_DATUM);
  spr.setTextColor(TH.menu_hdr);
  spr.drawString(title, 160, 13, FONT_SMALL);
}

static void drawWifiScanList(int netCount, int selected)
{
  drawWifiScanHeader("WIFI SCAN");

  spr.setTextDatum(TL_DATUM);
  if(netCount == 0)
  {
    spr.setTextColor(TH.text_warn);
    spr.drawString("No networks found", 10, 40, FONT_SMALL);
    spr.setTextColor(TH.text_muted);
    spr.drawString("Click = Rescan  Hold = Exit", 10, 62, FONT_SMALL);
    spr.pushSprite(0, 0);
    return;
  }

  spr.setTextColor(TH.text_muted);
  spr.drawString("Hold = Connect  Click = Exit", 10, 32, FONT_SMALL);

  int page = selected / WIFI_PAGE_SIZE;
  int start = page * WIFI_PAGE_SIZE;
  int end = start + WIFI_PAGE_SIZE;
  if(end > netCount) end = netCount;

  for(int i = start; i < end; i++)
  {
    int row = i - start;
    int y = 54 + row * 24;
    if(i == selected)
    {
      spr.fillRoundRect(5, y - 2, 310, 22, 4, TH.menu_hl_bg);
      spr.setTextColor(TH.menu_hl_text, TH.menu_hl_bg);
    }
    else
    {
      spr.setTextColor(TH.box_text, TH.bg);
    }
    spr.drawString(WiFi.SSID(i), 15, y, FONT_SMALL);
  }

  int pages = (netCount + WIFI_PAGE_SIZE - 1) / WIFI_PAGE_SIZE;
  if(pages < 1) pages = 1;
  char pageBuf[32];
  snprintf(pageBuf, sizeof(pageBuf), "Page %d/%d", page + 1, pages);
  spr.setTextColor(TH.text_muted);
  spr.setTextDatum(TL_DATUM);
  spr.drawString(pageBuf, 10, 152, FONT_SMALL);

  spr.pushSprite(0, 0);
}

static void drawWifiScanKeypad(const String &ssid, const String &password, bool kbOpen)
{
  drawWifiScanHeader("WIFI PASSWORD");

  spr.setTextDatum(TL_DATUM);
  spr.setTextColor(TH.box_text);
  spr.drawString("SSID: " + ssid, 10, 32, FONT_SMALL);
  spr.setTextColor(TH.text_muted);
  spr.drawString("Password:", 10, 52, FONT_SMALL);
  spr.setTextColor(TH.box_text);
  String shown = password;
  if(shown.length() > 24) shown = shown.substring(shown.length() - 24);
  spr.drawString(shown + "_", 95, 52, FONT_SMALL);
  spr.setTextColor(TH.text_muted);
  spr.drawString("Click=Type  Hold=Connect", 10, 72, FONT_SMALL);

  int listBottom = 88;
  kbBuild(kbMode, listBottom);

  for(int i = 0; i < kbKeyCount; i++)
  {
    const KbKey &k = kbKeys[i];
    bool sel = (i == kbCursor);
    uint16_t bg = sel ? TH.menu_hl_bg : TH.menu_bg;
    uint16_t fg = sel ? TH.menu_hl_text : TH.menu_item;
    spr.fillRoundRect(k.x, k.y, k.w, k.h, 3, bg);
    spr.drawRoundRect(k.x, k.y, k.w, k.h, 1, TH.menu_border);
    spr.setTextColor(fg, bg);
    spr.setTextDatum(MC_DATUM);
    int cx = k.x + k.w / 2;
    int cy = k.y + k.h / 2;
    if(k.type == KB_MODE)
      spr.drawString(KB_MODE_NAMES[(int)k.ch], cx, cy, FONT_SMALL);
    else if(k.type == KB_BSP)
      spr.drawString("BSP", cx, cy, FONT_SMALL);
    else if(k.type == KB_CANCEL)
      spr.drawString("X", cx, cy, FONT_SMALL);
    else if(k.type == KB_SPACE)
      spr.drawString("SPACE", cx, cy, FONT_SMALL);
    else
    {
      char s[2] = {k.ch, 0};
      spr.drawString(s, cx, cy, FONT_SMALL);
    }
  }

  (void)kbOpen;
  spr.pushSprite(0, 0);
}

static void drawWifiScanStatus(const char *line1, const char *line2, uint16_t color)
{
  drawWifiScanHeader("WIFI SCAN");
  spr.setTextDatum(TL_DATUM);
  spr.setTextColor(TH.box_text);
  if(line1) spr.drawString(line1, 10, 50, FONT_SMALL);
  spr.setTextColor(color);
  if(line2) spr.drawString(line2, 10, 80, FONT_SMALL);
  spr.pushSprite(0, 0);
}

// Modal: Wi-Fi is off — tell user to enable it, wait for OK
static void wifiScanNeedWiFiPopup()
{
  spr.fillSprite(TH.bg);
  spr.fillRoundRect(20, 40, 280, 90, 6, TH.menu_bg);
  spr.drawRoundRect(20, 40, 280, 90, 6, TH.menu_border);

  spr.setTextDatum(MC_DATUM);
  spr.setTextColor(TH.text_warn);
  spr.drawString("Wi-Fi is OFF", 160, 62, FONT_SMALL);
  spr.setTextColor(TH.text);
  spr.drawString("Turn on Wi-Fi first", 160, 84, FONT_SMALL);
  spr.setTextColor(TH.text_muted);
  spr.drawString("Settings > Wi-Fi", 160, 102, FONT_SMALL);

  spr.fillRoundRect(120, 112, 80, 18, 4, TH.menu_hl_bg);
  spr.drawRoundRect(120, 112, 80, 18, 4, TH.menu_border);
  spr.setTextColor(TH.menu_hl_text, TH.menu_hl_bg);
  spr.drawString("OK", 160, 121, FONT_SMALL);

  spr.pushSprite(0, 0);

  ButtonTracker okBtn;
  while(true)
  {
    ButtonTracker::State st = okBtn.update(digitalRead(ENCODER_PUSH_BUTTON) == LOW);
    if(st.wasClicked || st.wasShortPressed) break;
    delay(10);
  }
}

void wifiScanSettings()
{
  // Do not touch the radio when Wi-Fi was never enabled (avoids crash)
  if(wifiModeIdx == NET_OFF || WiFi.getMode() == WIFI_MODE_NULL)
  {
    wifiScanNeedWiFiPopup();
    return;
  }

  drawWifiScanStatus("Scanning...", nullptr, TH.text_muted);

  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.setTxPower(WIFI_POWER_LEVEL);
  int netCount = WiFi.scanNetworks();
  if(netCount < 0) netCount = 0;
  if(netCount > MAX_SCAN_NETWORKS) netCount = MAX_SCAN_NETWORKS;

  int selected = 0;
  bool kbOpen = false;
  String activeSsid;
  String password = "";

  auto redrawList = [&]() { drawWifiScanList(netCount, selected); };
  auto redrawKb = [&]() { drawWifiScanKeypad(activeSsid, password, kbOpen); };

  redrawList();

  ButtonTracker scanBtn;
  while(true)
  {
    uint32_t encCounts = consumeEncoderCounts();
    int16_t d = (int16_t)(encCounts & 0xFFFF);

    ButtonTracker::State st = scanBtn.update(digitalRead(ENCODER_PUSH_BUTTON) == LOW);

    if(!kbOpen)
    {
      if(netCount > 0 && d)
      {
        if(d > 0)
        {
          selected++;
          if(selected >= netCount) selected = 0;
        }
        else
        {
          selected--;
          if(selected < 0) selected = netCount - 1;
        }
        redrawList();
      }

      if(st.wasShortPressed)
      {
        if(netCount == 0)
        {
          WiFi.scanDelete();
          return;
        }
        activeSsid = WiFi.SSID(selected);
        wifi_auth_mode_t auth = WiFi.encryptionType(selected);
        if(auth == WIFI_AUTH_OPEN)
        {
          drawWifiScanStatus(("Connecting: " + activeSsid).c_str(), nullptr, TH.box_text);
          bool ok = wifiConnectBlocking(activeSsid, "", WIFI_CONNECT_TIMEOUT);
          if(ok)
          {
            wifiSaveCredentials(activeSsid, "");
            drawWifiScanStatus(("Connected: " + activeSsid).c_str(), getWiFiIPAddress(), TH.text);
            delay(1200);
            WiFi.scanDelete();
            return;
          }
          drawWifiScanStatus(("Failed: " + activeSsid).c_str(), "Check password / signal", TH.text_warn);
          delay(1500);
          redrawList();
        }
        else
        {
          password = "";
          kbOpen = true;
          kbMode = 0;
          kbCursor = 0;
          redrawKb();
        }
      }
      else if(st.wasClicked)
      {
        if(netCount == 0)
        {
          drawWifiScanStatus("Scanning...", nullptr, TH.text_muted);
          WiFi.scanDelete();
          WiFi.setTxPower(WIFI_POWER_LEVEL);
          netCount = WiFi.scanNetworks();
          if(netCount < 0) netCount = 0;
          if(netCount > MAX_SCAN_NETWORKS) netCount = MAX_SCAN_NETWORKS;
          selected = 0;
          redrawList();
        }
        else
        {
          WiFi.scanDelete();
          return;
        }
      }
    }
    else
    {
      if(d)
      {
        if(kbKeyCount > 0)
        {
          if(d > 0)
          {
            kbCursor++;
            if(kbCursor >= kbKeyCount) kbCursor = 0;
          }
          else
          {
            kbCursor--;
            if(kbCursor < 0) kbCursor = kbKeyCount - 1;
          }
          redrawKb();
        }
      }

      if(st.wasShortPressed)
      {
        drawWifiScanStatus(("Connecting: " + activeSsid).c_str(), nullptr, TH.box_text);
        bool ok = wifiConnectBlocking(activeSsid, password, WIFI_CONNECT_TIMEOUT);
        if(ok)
        {
          wifiSaveCredentials(activeSsid, password);
          drawWifiScanStatus(("Connected: " + activeSsid).c_str(), getWiFiIPAddress(), TH.text);
          delay(1200);
          WiFi.scanDelete();
          return;
        }
        drawWifiScanStatus("Failed", "Check password", TH.text_warn);
        delay(1500);
        redrawKb();
      }
      else if(st.wasClicked)
      {
        const KbKey &k = kbKeys[kbCursor];
        if(k.type == KB_CHAR)
        {
          if(password.length() < 63) password += k.ch;
          redrawKb();
        }
        else if(k.type == KB_MODE)
        {
          kbMode = (int)k.ch;
          kbCursor = 4;
          redrawKb();
        }
        else if(k.type == KB_BSP)
        {
          if(password.length() > 0) password.remove(password.length() - 1);
          redrawKb();
        }
        else if(k.type == KB_SPACE)
        {
          if(password.length() < 63) password += ' ';
          redrawKb();
        }
        else if(k.type == KB_CANCEL)
        {
          kbOpen = false;
          redrawList();
        }
      }
    }

    delay(10);
  }
}

//
// API: Get current radio status as JSON
//
static void apiStatus(AsyncWebServerRequest *request)
{
  if(!webIsAuthenticated(request)) return request->requestAuthentication();

  char freqStr[16];
  if(currentMode == FM)
    snprintf(freqStr, sizeof(freqStr), "%.2f", currentFrequency / 100.0);
  else
    snprintf(freqStr, sizeof(freqStr), "%.1f", currentFrequency + currentBFO / 1000.0);

  const char *modeNames[] = {"FM", "LSB", "USB", "AM"};

  String json = "{";
  json += "\"frequency\":" + String(freqStr) + ",";
  json += "\"mode\":\"" + String(modeNames[currentMode]) + "\",";
  json += "\"band\":\"" + String(getCurrentBand()->bandName) + "\",";
  json += "\"rssi\":" + String(rssi) + ",";
  json += "\"snr\":" + String(snr) + ",";
  json += "\"volume\":" + String(volume) + ",";
  json += "\"bfo\":" + String(currentBFO) + ",";
  json += "\"brightness\":" + String(currentBrt) + ",";
  json += "\"battery\":" + String(batteryMonitor(), 2) + ",";
  json += "\"fw\":\"" + String(getVersion(true)) + "\",";
  json += "\"bandIdx\":" + String(bandIdx) + ",";
  json += "\"totalBands\":" + String(getTotalBands());
  json += "}";

  AsyncWebServerResponse *response = request->beginResponse(200, "application/json", json);
  response->addHeader("Cache-Control", "no-store");
  request->send(response);
}

//
// API: Send remote command
//
static void apiCommand(AsyncWebServerRequest *request)
{
  if(!webIsAuthenticated(request)) return request->requestAuthentication();
  if(!request->hasParam("cmd", true))
    return request->send(400, "application/json", "{\"error\":\"Missing cmd parameter\"}");

  String cmd = request->getParam("cmd", true)->value();
  if(cmd.length() == 0)
    return request->send(400, "application/json", "{\"error\":\"Empty cmd\"}");

  // Multi-char commands: F<freqHz>, V<0-63>
  if(cmd.length() > 1 && cmd.charAt(0) == 'F')
  {
    long freqHz = cmd.substring(1).toInt();
    if(freqHz > 0)
    {
      Band *band = getCurrentBand();
      uint16_t targetFreq = freqFromHz((uint32_t)freqHz, currentMode);
      int targetBfo = isSSB() ? bfoFromHz((uint32_t)freqHz) : 0;
      if(!isFreqInBand(band, targetFreq) ||
         (isSSB() && targetFreq == band->maximumFreq && targetBfo))
        return request->send(400, "application/json", "{\"error\":\"Frequency out of band range\"}");
      if(!updateFrequency(targetFreq, false))
        return request->send(400, "application/json", "{\"error\":\"Frequency out of range\"}");
      if(isSSB())
        updateBFO(targetBfo, false);
      else if(currentBFO)
        updateBFO(0, true);
      clearStationInfo();
      identifyFrequency(currentFrequency + currentBFO / 1000);
      prefsRequestSave(SAVE_SETTINGS | SAVE_CUR_BAND);
    }
  }
  else if(cmd.length() > 1 && cmd.charAt(0) == 'V')
  {
    int vol = cmd.substring(1).toInt();
    if(vol >= 0 && vol <= 63)
    {
      volume = vol;
      rx.setVolume(volume);
      prefsRequestSave(SAVE_SETTINGS);
    }
  }
  else if(cmd.length() == 1)
  {
    RemoteState state;
    int ev = remoteDoCommand(nullptr, &state, cmd.charAt(0));
    web_event = ev;
  }

  request->send(200, "application/json", "{\"ok\":true}");
}

//
// API: Export all settings as JSON
//
static void apiExport(AsyncWebServerRequest *request)
{
  if(!webIsAuthenticated(request)) return request->requestAuthentication();

  uint32_t squelch = ((uint32_t)currentSquelch[FM]) |
                     ((uint32_t)currentSquelch[AM] << 8) |
                     ((uint32_t)currentSquelch[LSB] << 16) |
                     ((uint32_t)currentSquelch[USB] << 24);

  String json = "{";
  json += "\"settings\":{";
  json += "\"volume\":" + String(volume) + ",";
  json += "\"band\":" + String(bandIdx) + ",";
  json += "\"wifiMode\":" + String(wifiModeIdx) + ",";
  json += "\"brightness\":" + String(currentBrt) + ",";
  json += "\"fmAgc\":" + String(FmAgcIdx) + ",";
  json += "\"amAgc\":" + String(AmAgcIdx) + ",";
  json += "\"ssbAgc\":" + String(SsbAgcIdx) + ",";
  json += "\"amAvc\":" + String(AmAvcIdx) + ",";
  json += "\"ssbAvc\":" + String(SsbAvcIdx) + ",";
  json += "\"amSoftMute\":" + String(AmSoftMuteIdx) + ",";
  json += "\"ssbSoftMute\":" + String(SsbSoftMuteIdx) + ",";
  json += "\"sleep\":" + String(currentSleep) + ",";
  json += "\"theme\":" + String(themeIdx) + ",";
  json += "\"rdsMode\":" + String(rdsModeIdx) + ",";
  json += "\"sleepMode\":" + String(sleepModeIdx) + ",";
  json += "\"zoomMenu\":" + String(zoomMenu ? "true" : "false") + ",";
  json += "\"scrollDir\":" + String(scrollDirection) + ",";
  json += "\"encHalfStep\":" + String(encoderHalfStep ? "true" : "false") + ",";
  json += "\"utcOffset\":" + String(utcOffsetIdx) + ",";
  json += "\"squelch\":" + String(squelch) + ",";
  json += "\"fmRegion\":" + String(FmRegionIdx) + ",";
  json += "\"uiLayout\":" + String(uiLayoutIdx) + ",";
  json += "\"bleMode\":" + String(bleModeIdx) + ",";
  json += "\"usbMode\":" + String(usbModeIdx) + ",";
  json += "\"tcpMode\":" + String(tcpModeIdx);
  json += "},";

  // Bands
  json += "\"bands\":[";
  for(int i = 0; i < getTotalBands(); i++)
  {
    if(i) json += ",";
    json += "{";
    json += "\"name\":\"" + String(bands[i].bandName) + "\",";
    json += "\"mode\":" + String(bands[i].bandMode) + ",";
    json += "\"freq\":" + String(bands[i].currentFreq) + ",";
    json += "\"step\":" + String(bands[i].currentStepIdx) + ",";
    json += "\"bw\":" + String(bands[i].bandwidthIdx) + ",";
    json += "\"usbCal\":" + String(bands[i].usbCal) + ",";
    json += "\"lsbCal\":" + String(bands[i].lsbCal);
    json += "}";
  }
  json += "],";

  // Memories
  json += "\"memories\":[";
  bool first = true;
  for(int i = 0; i < MEMORY_COUNT; i++)
  {
    if(memories[i].freq == 0) continue;
    if(!first) json += ",";
    first = false;
    json += "{";
    json += "\"slot\":" + String(i + 1) + ",";
    json += "\"freq\":" + String(memories[i].freq) + ",";
    json += "\"mode\":" + String(memories[i].mode) + ",";
    json += "\"band\":" + String(memories[i].band);
    json += "}";
  }
  json += "]";

  json += "}";

  AsyncWebServerResponse *response = request->beginResponse(200, "application/json", json);
  response->addHeader("Content-Disposition", "attachment; filename=\"ats-mini-settings.json\"");
  response->addHeader("Cache-Control", "no-store");
  request->send(response);
}

//
// API: Import settings from JSON
//
static void apiImport(AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t, size_t)
{
  if(!webIsAuthenticated(request)) return request->requestAuthentication();

  String body((const char *)data, len);

  auto extractInt = [&](const String &key, int &out) -> bool {
    int idx = body.indexOf("\"" + key + "\":");
    if(idx < 0) return false;
    idx += key.length() + 4;
    out = body.substring(idx).toInt();
    return true;
  };

  auto extractBool = [&](const String &key, bool &out) -> bool {
    int idx = body.indexOf("\"" + key + "\":");
    if(idx < 0) return false;
    idx += key.length() + 4;
    out = body.substring(idx, idx + 4) == "true";
    return true;
  };

  int val;
  bool bval;

  // Apply all settings
  if(extractInt("volume", val) && val >= 0 && val <= 63)
  {
    volume = val;
    rx.setVolume(volume);
  }
  if(extractInt("band", val) && val >= 0 && val < getTotalBands())
    bandIdx = val;
  if(extractInt("wifiMode", val))
    wifiModeIdx = val;
  if(extractInt("brightness", val) && val >= 10 && val <= 255)
    currentBrt = val;
  if(extractInt("fmAgc", val))
    FmAgcIdx = val;
  if(extractInt("amAgc", val))
    AmAgcIdx = val;
  if(extractInt("ssbAgc", val))
    SsbAgcIdx = val;
  if(extractInt("amAvc", val))
    AmAvcIdx = val;
  if(extractInt("ssbAvc", val))
    SsbAvcIdx = val;
  if(extractInt("amSoftMute", val))
    AmSoftMuteIdx = val;
  if(extractInt("ssbSoftMute", val))
    SsbSoftMuteIdx = val;
  if(extractInt("sleep", val))
    currentSleep = val;
  if(extractInt("theme", val))
    themeIdx = val;
  if(extractInt("rdsMode", val))
    rdsModeIdx = val;
  if(extractInt("sleepMode", val))
    sleepModeIdx = val;
  if(extractBool("zoomMenu", bval))
    zoomMenu = bval;
  if(extractInt("scrollDir", val))
    scrollDirection = val;
  if(extractBool("encHalfStep", bval))
    encoderHalfStep = bval;
  if(extractInt("utcOffset", val))
    utcOffsetIdx = val;
  if(extractInt("squelch", val))
  {
    currentSquelch[FM] = val & 0xFF;
    currentSquelch[AM] = (val >> 8) & 0xFF;
    currentSquelch[LSB] = (val >> 16) & 0xFF;
    currentSquelch[USB] = (val >> 24) & 0xFF;
  }
  if(extractInt("fmRegion", val))
    FmRegionIdx = val;
  if(extractInt("uiLayout", val))
    uiLayoutIdx = val;
  if(extractInt("bleMode", val))
    bleModeIdx = val;
  if(extractInt("usbMode", val))
    usbModeIdx = val;
  if(extractInt("tcpMode", val))
    tcpModeIdx = val;

  // Import bands
  int bandIdx_start = body.indexOf("\"bands\":[");
  if(bandIdx_start >= 0)
  {
    for(int i = 0; i < getTotalBands(); i++)
    {
      String marker = "\"name\":\"" + String(bands[i].bandName) + "\"";
      int bIdx = body.indexOf(marker, bandIdx_start);
      if(bIdx < 0) continue;

      int objStart = body.lastIndexOf('{', bIdx);
      int objEnd = body.indexOf('}', bIdx);
      if(objStart < 0 || objEnd < 0) continue;

      String obj = body.substring(objStart, objEnd + 1);

      int fi = obj.indexOf("\"freq\":");
      if(fi >= 0) bands[i].currentFreq = obj.substring(fi + 7).toInt();
      fi = obj.indexOf("\"mode\":");
      if(fi >= 0) bands[i].bandMode = obj.substring(fi + 7).toInt();
      fi = obj.indexOf("\"step\":");
      if(fi >= 0) bands[i].currentStepIdx = obj.substring(fi + 7).toInt();
      fi = obj.indexOf("\"bw\":");
      if(fi >= 0) bands[i].bandwidthIdx = obj.substring(fi + 5).toInt();
      fi = obj.indexOf("\"usbCal\":");
      if(fi >= 0) bands[i].usbCal = obj.substring(fi + 9).toInt();
      fi = obj.indexOf("\"lsbCal\":");
      if(fi >= 0) bands[i].lsbCal = obj.substring(fi + 9).toInt();
      prefsSaveBand(i, false);
    }
  }

  // Import memories
  int memIdx_start = body.indexOf("\"memories\":[");
  if(memIdx_start >= 0)
  {
    int pos = memIdx_start + 13;
    while(pos < (int)body.length())
    {
      int objStart = body.indexOf('{', pos);
      if(objStart < 0) break;
      int objEnd = body.indexOf('}', objStart);
      if(objEnd < 0) break;

      String obj = body.substring(objStart, objEnd + 1);
      pos = objEnd + 1;

      int slot = 0, mfreq = 0, mmode = 0, mband = 0;
      int fi = obj.indexOf("\"slot\":");
      if(fi >= 0) slot = obj.substring(fi + 7).toInt();
      fi = obj.indexOf("\"freq\":");
      if(fi >= 0) mfreq = obj.substring(fi + 7).toInt();
      fi = obj.indexOf("\"mode\":");
      if(fi >= 0) mmode = obj.substring(fi + 7).toInt();
      fi = obj.indexOf("\"band\":");
      if(fi >= 0) mband = obj.substring(fi + 7).toInt();

      if(slot >= 1 && slot <= MEMORY_COUNT && mfreq > 0)
      {
        int idx = slot - 1;
        memories[idx].freq = mfreq;
        memories[idx].mode = mmode;
        memories[idx].band = mband;
        prefsSaveMemory(idx, false);
      }
    }
  }

  prefsRequestSave(SAVE_SETTINGS, true);
  request->send(200, "application/json", "{\"ok\":true}");
}

//
// Screen mirror frame cache for delta updates
//
#define SCREEN_TILE 16

static uint16_t *screenCache = nullptr;
static uint16_t screenCacheW = 0;
static uint16_t screenCacheH = 0;
static uint32_t screenCacheHash = 0;

static uint32_t screenHashPixels(const uint16_t *pixels, size_t count)
{
  uint32_t hash = 2166136261u;
  for(size_t i = 0; i < count; i++)
  {
    hash ^= pixels[i];
    hash *= 16777619u;
  }
  return hash;
}

static bool screenCacheStore(const uint16_t *pixels, uint16_t width, uint16_t height, uint32_t hash)
{
  size_t bytes = (size_t)width * height * sizeof(uint16_t);
  if(screenCache && (screenCacheW != width || screenCacheH != height))
  {
    free(screenCache);
    screenCache = nullptr;
  }
  if(!screenCache)
    screenCache = (uint16_t *)malloc(bytes);
  if(!screenCache) return false;
  memcpy(screenCache, pixels, bytes);
  screenCacheW = width;
  screenCacheH = height;
  screenCacheHash = hash;
  return true;
}

static void screenReadScaled(uint16_t *pixels, uint16_t width, uint16_t height, uint16_t scale)
{
  size_t pos = 0;
  for(uint16_t y = 0; y < height; y++)
    for(uint16_t x = 0; x < width; x++)
      pixels[pos++] = spr.readPixel(x * scale, y * scale);
}

//
// API: Capture screen as BMP (full) or RGB565 delta (delta=1)
//
static void apiScreen(AsyncWebServerRequest *request)
{
  if(!webIsAuthenticated(request)) return request->requestAuthentication();

  uint16_t srcW = spr.width();
  uint16_t srcH = spr.height();
  uint16_t scale = 2;
  if(request->hasParam("scale"))
    scale = request->getParam("scale")->value().toInt();
  if(scale < 1) scale = 1;
  if(scale > 4) scale = 4;
  uint16_t width = srcW / scale;
  uint16_t height = srcH / scale;
  size_t pixelCount = (size_t)width * height;

  bool wantDelta = request->hasParam("delta") && request->getParam("delta")->value() == "1";
  uint32_t reqHash = 0;
  if(request->hasParam("h"))
    reqHash = strtoul(request->getParam("h")->value().c_str(), nullptr, 10);

  uint16_t *cur = (uint16_t *)malloc(pixelCount * sizeof(uint16_t));
  if(!cur) return request->send(500, "text/plain", "Out of memory");
  screenReadScaled(cur, width, height, scale);
  uint32_t curHash = screenHashPixels(cur, pixelCount);

  if(!wantDelta)
  {
    size_t imgSize = 14 + 40 + 12 + pixelCount * 2;
    uint8_t *bmp = (uint8_t *)malloc(imgSize);
    if(!bmp)
    {
      free(cur);
      return request->send(500, "text/plain", "Out of memory");
    }

    int pos = 0;
    bmp[pos++] = 'B'; bmp[pos++] = 'M';
    uint32_t fileSize = imgSize;
    memcpy(bmp + pos, &fileSize, 4); pos += 4;
    uint32_t reserved = 0;
    memcpy(bmp + pos, &reserved, 4); pos += 4;
    uint32_t dataOffset = 14 + 40 + 12;
    memcpy(bmp + pos, &dataOffset, 4); pos += 4;
    uint32_t dibSize = 40;
    memcpy(bmp + pos, &dibSize, 4); pos += 4;
    int32_t w = width;
    memcpy(bmp + pos, &w, 4); pos += 4;
    int32_t h = height;
    memcpy(bmp + pos, &h, 4); pos += 4;
    uint16_t planes = 1;
    memcpy(bmp + pos, &planes, 2); pos += 2;
    uint16_t bpp = 16;
    memcpy(bmp + pos, &bpp, 2); pos += 2;
    uint32_t compression = 3;
    memcpy(bmp + pos, &compression, 4); pos += 4;
    uint32_t imgBytes = pixelCount * 2;
    memcpy(bmp + pos, &imgBytes, 4); pos += 4;
    uint32_t xppm = 0, yppm = 0;
    memcpy(bmp + pos, &xppm, 4); pos += 4;
    memcpy(bmp + pos, &yppm, 4); pos += 4;
    uint32_t colorsUsed = 0, colorsImportant = 0;
    memcpy(bmp + pos, &colorsUsed, 4); pos += 4;
    memcpy(bmp + pos, &colorsImportant, 4); pos += 4;
    uint32_t rMask = 0xF800, gMask = 0x07E0, bMask = 0x001F;
    memcpy(bmp + pos, &rMask, 4); pos += 4;
    memcpy(bmp + pos, &gMask, 4); pos += 4;
    memcpy(bmp + pos, &bMask, 4); pos += 4;

    for(int y = (int)height - 1; y >= 0; y--)
      for(uint16_t x = 0; x < width; x++)
      {
        uint16_t pixel = cur[(size_t)y * width + x];
        memcpy(bmp + pos, &pixel, 2);
        pos += 2;
      }

    AsyncWebServerResponse *response = request->beginResponse(200, "image/bmp", bmp, imgSize);
    response->addHeader("Cache-Control", "no-store");
    request->send(response);
    free(bmp);
    screenCacheStore(cur, width, height, curHash);
    free(cur);
    return;
  }

  // Delta mode: 'S', flags (bit0=full), width, height, ...
  bool cacheOk = screenCache && screenCacheW == width && screenCacheH == height &&
                 reqHash != 0 && screenCacheHash == reqHash;

  uint16_t cols = (width + SCREEN_TILE - 1) / SCREEN_TILE;
  uint16_t rows = (height + SCREEN_TILE - 1) / SCREEN_TILE;
  size_t maxTiles = (size_t)cols * rows;
  uint16_t *dirty = cacheOk ? (uint16_t *)malloc(maxTiles * sizeof(uint16_t)) : nullptr;
  uint16_t tileCount = 0;
  size_t deltaBytes = 0;

  if(dirty)
  {
    for(uint16_t ty = 0; ty < rows; ty++)
      for(uint16_t tx = 0; tx < cols; tx++)
      {
        uint16_t x0 = tx * SCREEN_TILE;
        uint16_t y0 = ty * SCREEN_TILE;
        uint16_t tw = (uint16_t)((x0 + SCREEN_TILE <= width) ? SCREEN_TILE : width - x0);
        uint16_t th = (uint16_t)((y0 + SCREEN_TILE <= height) ? SCREEN_TILE : height - y0);
        bool changed = false;
        for(uint16_t y = 0; y < th && !changed; y++)
        {
          size_t row = (size_t)(y0 + y) * width + x0;
          for(uint16_t x = 0; x < tw; x++)
          {
            if(cur[row + x] != screenCache[row + x])
            {
              changed = true;
              break;
            }
          }
        }
        if(changed)
        {
          dirty[tileCount++] = (uint16_t)(ty * cols + tx);
          deltaBytes += 8 + (size_t)tw * th * 2;
        }
      }
    // Too many tiles: cheaper to send the full frame
    if(deltaBytes >= pixelCount * 2)
    {
      free(dirty);
      dirty = nullptr;
      tileCount = 0;
    }
  }

  bool sendFull = !cacheOk || !dirty || tileCount == 0;
  // tileCount==0 means the frame matches cache; still send a tiny ack with flags=0 and 0 tiles
  if(cacheOk && dirty && tileCount == 0)
    sendFull = false;

  // Build response
  size_t header = 6;
  size_t respSize;
  if(sendFull)
    respSize = header + pixelCount * 2;
  else if(tileCount == 0)
    respSize = header + 2;
  else
    respSize = header + 2 + deltaBytes;

  uint8_t *resp = (uint8_t *)malloc(respSize);
  if(!resp)
  {
    if(dirty) free(dirty);
    free(cur);
    return request->send(500, "text/plain", "Out of memory");
  }

  resp[0] = 'S';
  resp[1] = sendFull ? 1 : 0;
  memcpy(resp + 2, &width, 2);
  memcpy(resp + 4, &height, 2);
  size_t pos = header;

  if(sendFull)
  {
    memcpy(resp + pos, cur, pixelCount * 2);
    pos += pixelCount * 2;
  }
  else
  {
    memcpy(resp + pos, &tileCount, 2);
    pos += 2;
    for(uint16_t i = 0; i < tileCount; i++)
    {
      uint16_t idx = dirty[i];
      uint16_t tx = idx % cols;
      uint16_t ty = idx / cols;
      uint16_t x0 = tx * SCREEN_TILE;
      uint16_t y0 = ty * SCREEN_TILE;
      uint16_t tw = (uint16_t)((x0 + SCREEN_TILE <= width) ? SCREEN_TILE : width - x0);
      uint16_t th = (uint16_t)((y0 + SCREEN_TILE <= height) ? SCREEN_TILE : height - y0);
      memcpy(resp + pos, &x0, 2); pos += 2;
      memcpy(resp + pos, &y0, 2); pos += 2;
      memcpy(resp + pos, &tw, 2); pos += 2;
      memcpy(resp + pos, &th, 2); pos += 2;
      for(uint16_t y = 0; y < th; y++)
      {
        size_t src = (size_t)(y0 + y) * width + x0;
        memcpy(resp + pos, cur + src, (size_t)tw * 2);
        pos += (size_t)tw * 2;
      }
    }
  }

  if(dirty) free(dirty);
  screenCacheStore(cur, width, height, curHash);
  free(cur);

  AsyncWebServerResponse *response = request->beginResponse(200, "application/octet-stream", resp, respSize);
  response->addHeader("Cache-Control", "no-store");
  request->send(response);
  free(resp);
}

//
// Initialize internal web server
//
static void webInit()
{
  server.on("/", HTTP_ANY, [] (AsyncWebServerRequest *request) {
    request->send(200, "text/html", webStatusPage());
  });

  server.on("/remote", HTTP_ANY, [] (AsyncWebServerRequest *request) {
    if(!webIsAuthenticated(request)) return request->requestAuthentication();
    request->send(200, "text/html", webRemotePage());
  });

  server.on("/memory", HTTP_ANY, [] (AsyncWebServerRequest *request) {
    if(!webIsAuthenticated(request)) return request->requestAuthentication();
    request->send(200, "text/html", webMemoryPage());
  });

  server.on("/config", HTTP_ANY, [] (AsyncWebServerRequest *request) {
    if(!webIsAuthenticated(request)) return request->requestAuthentication();
    request->send(200, "text/html", webConfigPage());
  });

  server.on("/splash.png", HTTP_GET, [] (AsyncWebServerRequest *request) {
    if(!LittleFS.exists(SPLASH_PATH))
      return request->send(404, "text/plain", "Not found");
    request->send(LittleFS, SPLASH_PATH, "image/png");
  });

  server.onNotFound([] (AsyncWebServerRequest *request) {
    request->send(404, "text/plain", "Not found");
  });

  // This method saves configuration form contents
  server.on("/setconfig", HTTP_POST, webSetConfig, webUploadSplash);

  // Register subpaths first: the server also matches /update to /update/... .
  server.on("/update/upload", HTTP_POST, webUploadFirmwareComplete, webUploadFirmware);
  server.on("/update", HTTP_GET, [](AsyncWebServerRequest *request) {
    if(!webIsAuthenticated(request)) return request->requestAuthentication();
    webUpdatePage(request, otaStatus(), 200);
  });
  server.on("/update", HTTP_POST, [](AsyncWebServerRequest *request) {
    if(!webIsAuthenticated(request)) return request->requestAuthentication();
    if(!otaRequestLatest(request->hasParam("action", true) && request->getParam("action", true)->value() == "install"))
      return webUpdatePage(request, {OTA_FAILED, "An update is already in progress."}, 409);
    request->redirect("/update");
  });

  // API endpoints
  server.on("/api/status", HTTP_GET, apiStatus);
  server.on("/api/cmd", HTTP_POST, apiCommand);
  server.on("/api/export", HTTP_GET, apiExport);
  server.on("/api/import", HTTP_POST,
    [](AsyncWebServerRequest *request) {},
    NULL,
    apiImport
  );
  server.on("/api/screen", HTTP_GET, apiScreen);

  // Start web server
  server.begin();
}

static void webUploadFirmware(AsyncWebServerRequest *request, const String &filename,
                              size_t index, uint8_t *data, size_t len, bool)
{
  if(!webIsAuthenticated(request) || request->getResponse()) return;

  if(!index)
  {
    if(!filename.endsWith(".bin"))
      return webUpdatePage(request, {OTA_FAILED, "Select a firmware .bin file."});
    const auto *param = request->getParam("size", true);
    if(!param) return webUpdatePage(request, {OTA_FAILED, "Invalid firmware size."});
    size_t imageSize = strtoul(param->value().c_str(), nullptr, 10);
    // Round-trip the number to reject signs, whitespace, suffixes, and overflow.
    if(!imageSize || imageSize > request->contentLength() || String(imageSize) != param->value())
      return webUpdatePage(request, {OTA_FAILED, "Invalid firmware size."});
    if(!otaBegin(imageSize))
      return webUpdatePage(request, {OTA_FAILED, "An update is already in progress."}, 409);
    request->client()->setRxTimeout(15);
    request->onDisconnect([]() { otaEndUpload(); });
  }

  if(!otaWrite(data, len)) return webUpdatePage(request);
}

static void webUploadFirmwareComplete(AsyncWebServerRequest *request)
{
  if(!webIsAuthenticated(request)) return request->requestAuthentication();
  if(request->getResponse()) return; // Preserve an upload error queued above.
  if(!request->hasParam("firmware", true, true))
    return webUpdatePage(request, {OTA_FAILED, "No complete firmware uploaded."});
  otaFinish();
  webUpdatePage(request);
}

static void webUploadSplash(AsyncWebServerRequest *request, const String &filename,
                            size_t index, uint8_t *data, size_t len, bool final)
{
  if(!webIsAuthenticated(request)) return;

  if(index == 0)
  {
    LittleFS.remove(SPLASH_TEMP_PATH);

    SplashUploadState *state = static_cast<SplashUploadState *>(calloc(1, sizeof(SplashUploadState)));
    if(!state) return;

    request->_tempObject = state;
    request->onDisconnect([request, state]() {
      if(state->incomplete)
        request->_tempFile.close();

      // Discard an upload that was not installed by webSetConfig().
      LittleFS.remove(SPLASH_TEMP_PATH);
    });

    // The browser filter is only advisory, so enforce the extension here too.
    if(filename.endsWith(".png"))
    {
      request->_tempFile = LittleFS.open(SPLASH_TEMP_PATH, "w");
      state->incomplete = request->_tempFile;
    }
  }

  SplashUploadState *state = static_cast<SplashUploadState *>(request->_tempObject);

  if(request->_tempFile && len)
  {
    if((index + len) > SPLASH_MAX_FILE_SIZE)
    {
      state->tooLarge = true;
      state->incomplete = false;
      request->_tempFile.close();
      LittleFS.remove(SPLASH_TEMP_PATH);
    }
    else if(request->_tempFile.write(data, len) != len)
    {
      state->incomplete = false;
      request->_tempFile.close();
      LittleFS.remove(SPLASH_TEMP_PATH);
    }
  }

  if(final && request->_tempFile)
    request->_tempFile.close();

  if(final && state)
    state->incomplete = false;
}

void webSetConfig(AsyncWebServerRequest *request)
{
  if(!webIsAuthenticated(request)) return request->requestAuthentication();

  uint32_t prefsSave = 0;
  uint32_t epoch;
  bool setClock = false;

  if(request->hasParam("datetime", true))
  {
    String dateTime = request->getParam("datetime", true)->value();
    if(dateTime != "")
    {
      if(!webParseUTCDateTime(dateTime, &epoch))
        return request->send(400, "text/plain", "Date/time must use the YYYY-mm-dd HH:MM:SS format and contain a valid UTC date and time.");
      setClock = true;
    }
  }

  if(request->hasParam("deletesplash", true))
  {
    LittleFS.remove(SPLASH_TEMP_PATH);
    LittleFS.remove(SPLASH_PATH);
  }
  else if(request->hasParam("splash", true, true))
  {
    String filename = request->getParam("splash", true, true)->value();

    if(filename != "")
    {
      SplashUploadState *state = static_cast<SplashUploadState *>(request->_tempObject);
      if(state && state->tooLarge)
        return request->send(413, "text/plain", "The splash image must not exceed 512 KB.");

      if(!filename.endsWith(".png"))
      {
        LittleFS.remove(SPLASH_TEMP_PATH);
        return request->send(400, "text/plain", "The splash image filename must end in .png.");
      }

      if(!LittleFS.exists(SPLASH_TEMP_PATH))
        return request->send(500, "text/plain", "The splash image could not be stored.");

      String error = splashValidate();
      if(error != "")
      {
        LittleFS.remove(SPLASH_TEMP_PATH);
        return request->send(400, "text/plain", error);
      }

      if(!LittleFS.rename(SPLASH_TEMP_PATH, SPLASH_PATH))
      {
        LittleFS.remove(SPLASH_TEMP_PATH);
        return request->send(500, "text/plain", "The splash image could not be installed.");
      }
    }
  }

  // Start modifying preferences
  prefs.begin("network", false, STORAGE_PARTITION);

  // Save user name and password
  if(request->hasParam("username", true) && request->hasParam("password", true))
  {
    loginUsername = request->getParam("username", true)->value();
    loginPassword = request->getParam("password", true)->value();

    prefs.putString("loginusername", loginUsername);
    prefs.putString("loginpassword", loginPassword);
  }

  // Save SSIDs and their passwords
  bool haveSSID = false;
  for(int j=0 ; j<3 ; j++)
  {
    char nameSSID[16], namePASS[16];

    sprintf(nameSSID, "wifissid%d", j+1);
    sprintf(namePASS, "wifipass%d", j+1);

    if(request->hasParam(nameSSID, true) && request->hasParam(namePASS, true))
    {
      String ssid = request->getParam(nameSSID, true)->value();
      String pass = request->getParam(namePASS, true)->value();
      prefs.putString(nameSSID, ssid);
      prefs.putString(namePASS, pass);
      haveSSID |= ssid != "" && pass != "";
    }
  }

  // Save hidden SSID scanning preference
  wifiScanHidden = request->hasParam("wifiscanhidden", true);
  prefs.putBool("wifiscanhidden", wifiScanHidden);

  // Save time zone
  if(request->hasParam("utcoffset", true))
  {
    int idx = request->getParam("utcoffset", true)->value().toInt();
    if(idx >= 0 && idx < getTotalUTCOffsets())
    {
      utcOffsetIdx = idx;
      prefsSave |= SAVE_SETTINGS;
    }
  }

  // Save theme
  if(request->hasParam("theme", true))
  {
    String theme = request->getParam("theme", true)->value();
    themeIdx = theme.toInt();
    prefsSave |= SAVE_SETTINGS;
  }

  // Save scroll direction and menu zoom
  scrollDirection = request->hasParam("scroll", true)? -1 : 1;
  zoomMenu        = request->hasParam("zoom", true);
  setEncoderHalfStep(request->hasParam("encoderhalfstep", true));
  prefsSave |= SAVE_SETTINGS;

  // Done with the preferences
  prefs.end();

  // Save preferences immediately
  prefsRequestSave(prefsSave, true);

  if(setClock) clockSetEpoch(epoch);

  // Show config page again
  request->redirect("/config");

  // If we are currently in AP mode, and infrastructure mode requested,
  // and there is at least one SSID / PASS pair, request network connection
  if(haveSSID && (wifiModeIdx>NET_AP_ONLY) && (WiFi.status()!=WL_CONNECTED))
    netRequestConnect();
}

static const String webInputField(const String &name, const String &value, bool pass)
{
  String newValue(value);

  newValue.replace("\"", "&quot;");
  newValue.replace("'", "&apos;");

  return(
    "<INPUT TYPE='" + String(pass? "PASSWORD":"TEXT") + "' NAME='" +
    name + "' VALUE='" + newValue + "'>"
  );
}

static bool webParseUTCDateTime(const String &text, uint32_t *epoch)
{
  int year, month, day, hour, minute, second;
  return(epoch && text.length() == 19 &&
         sscanf(text.c_str(), "%4d-%2d-%2d %2d:%2d:%2d",
                &year, &month, &day, &hour, &minute, &second) == 6 &&
         clockUTCDateTimeToEpoch(year, month, day, hour, minute, second, epoch));
}

static const String webStyleSheet()
{
  return
"*{box-sizing:border-box;margin:0;padding:0;}"
"BODY{font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',Roboto,sans-serif;background:#fafafa;color:#333;line-height:1.6;}"
"CONTAINER{max-width:640px;margin:0 auto;padding:24px 16px;}"
"H1{font-size:1.25rem;font-weight:600;text-align:center;margin-bottom:16px;color:#111;}"
"NAV{display:flex;justify-content:center;gap:4px;margin-bottom:24px;background:#fff;border-radius:8px;padding:4px;box-shadow:0 1px 3px rgba(0,0,0,0.08);}"
"NAV A{padding:8px 16px;border-radius:6px;text-decoration:none;color:#666;font-size:0.875rem;font-weight:500;transition:all 0.15s;}"
"NAV A:hover{background:#f0f0f0;color:#333;}"
"NAV A.active{background:#111;color:#fff;}"
"TABLE{width:100%;border-collapse:separate;border-spacing:0;background:#fff;border-radius:12px;overflow:hidden;box-shadow:0 1px 3px rgba(0,0,0,0.06);}"
"TH{padding:12px 16px;font-size:0.75rem;font-weight:600;text-transform:uppercase;letter-spacing:0.5px;color:#999;background:#fafafa;border-bottom:1px solid #eee;text-align:left;}"
"TD{padding:12px 16px;border-bottom:1px solid #f5f5f5;font-size:0.9rem;}"
"TR:last-child TD{border-bottom:none;}"
"TD.LABEL{color:#888;font-weight:500;width:140px;}"
"INPUT[type=text],INPUT[type=password],INPUT[type=file],SELECT{width:100%;padding:10px 12px;border:1px solid #ddd;border-radius:8px;font-size:0.9rem;background:#fafafa;transition:border-color 0.15s;}"
"INPUT[type=text]:focus,INPUT[type=password]:focus,SELECT:focus{outline:none;border-color:#333;background:#fff;}"
"INPUT[type=submit],BUTTON{display:inline-flex;align-items:center;justify-content:center;padding:10px 24px;border:none;border-radius:8px;font-size:0.9rem;font-weight:500;cursor:pointer;transition:all 0.15s;}"
"INPUT[type=submit]{background:#111;color:#fff;width:auto;}"
"INPUT[type=submit]:hover{background:#333;}"
"BUTTON{background:#111;color:#fff;}"
"BUTTON:hover{background:#333;}"
"BUTTON:disabled{background:#ccc;cursor:not-allowed;}"
".CENTER{text-align:center;}"
"SMALL{color:#999;font-size:0.8rem;}"
"CODE{background:#f0f0f0;padding:2px 6px;border-radius:4px;font-size:0.85rem;}"
"DETAILS{margin-top:12px;}"
"SUMMARY{cursor:pointer;color:#666;font-size:0.9rem;}"
"P{margin-bottom:8px;}"
"A{color:#333;text-decoration:none;border-bottom:1px solid #ddd;transition:border-color 0.15s;}"
"A:hover{border-color:#333;}"
;
}

static String webNavigation(const char *activePage)
{
  static const struct { const char *name; const char *path; } pages[] =
  {
    {"Status", "/"},
    {"Remote", "/remote"},
    {"Memory", "/memory"},
    {"Config", "/config"},
    {"Update", "/update"},
  };
  String result = "<NAV>";
  for(size_t i = 0; i < sizeof(pages) / sizeof(pages[0]); i++)
  {
    if(!strcmp(activePage, pages[i].path))
      result += String("<A CLASS='active'>") + pages[i].name + "</A>";
    else
      result += String("<A HREF='") + pages[i].path + "'>" + pages[i].name + "</A>";
  }
  return result + "</NAV>";
}

static const String webPage(const String &body)
{
  return
"<!DOCTYPE HTML>"
"<HTML>"
"<HEAD>"
  "<META CHARSET='UTF-8'>"
  "<META NAME='viewport' CONTENT='width=device-width, initial-scale=1.0'>"
  "<TITLE>ATS-Mini</TITLE>"
  "<STYLE>" + webStyleSheet() + "</STYLE>"
"</HEAD>"
"<BODY>" + body + "</BODY>"
"</HTML>"
;
}

static const String webUtcOffsetSelector()
{
  String result = "";

  for(int i=0 ; i<getTotalUTCOffsets(); i++)
  {
    char text[96];

    sprintf(text,
      "<OPTION VALUE='%d' DATA-MINUTES='%d'%s>%s</OPTION>",
      i, utcOffsets[i].offset * 15, utcOffsetIdx==i? " SELECTED":"",
      utcOffsets[i].desc
    );

    result += text;
  }

  return(result);
}

static const String webThemeSelector()
{
  String result = "";

  for(int i=0 ; i<getTotalThemes(); i++)
  {
    char text[64];

    sprintf(text,
      "<OPTION VALUE='%d'%s>%s</OPTION>",
       i, themeIdx==i? " SELECTED":"", theme[i].name
    );

    result += text;
  }

  return(result);
}

static void webNetInfo(String &ip, String &ssid)
{
  if(WiFi.status()==WL_CONNECTED)
  {
    ip = WiFi.localIP().toString();
    ssid = WiFi.SSID();
  }
  else
  {
    ip = WiFi.softAPIP().toString();
    ssid = String(apSSID);
  }
}

static String webReceiverTime()
{
  String receiverTime = "Not synchronized";
  int offsetMinutes = getCurrentUTCOffset() * 15;
  int offsetMagnitude = abs(offsetMinutes);
  char utcOffset[10];
  snprintf(utcOffset, sizeof(utcOffset), "UTC%c%02d:%02d",
           offsetMinutes < 0? '-' : '+', offsetMagnitude / 60, offsetMagnitude % 60);

  if(clockAvailable())
  {
    time_t localTime = time(NULL) + offsetMinutes * 60;
    struct tm fields;
    gmtime_r(&localTime, &fields);
    char text[20];

    strftime(text, sizeof(text),
             clockGetDate(NULL, NULL, NULL, NULL)? "%Y-%m-%d %H:%M:%S" : "%H:%M:%S",
             &fields);

    receiverTime = text;
  }

  receiverTime += " (" + String(utcOffset) + ")";
  return receiverTime;
}

static const String webStatusPage()
{
  String ip, ssid;
  webNetInfo(ip, ssid);
  String freq = currentMode == FM?
    String(currentFrequency / 100.0) + "MHz "
  : String(currentFrequency + currentBFO / 1000.0) + "kHz ";

  String body = "<DIV CLASS='CONTAINER'>"
    "<H1>ATS-Mini</H1>" + webNavigation("/") +

    "<TABLE>"
    "<TR><TD CLASS='LABEL'>Band</TD><TD>"
    + String(getCurrentBand()->bandName) + "</TD></TR>"
    "<TR><TD CLASS='LABEL'>Freq</TD><TD>"
    + freq + " <SMALL>" + String(bandModeDesc[currentMode]) + "</SMALL></TD></TR>"
    "<TR><TD CLASS='LABEL'>Signal</TD><TD>"
    + String(rssi) + "dBuV SNR " + String(snr) + "dB</TD></TR>"
    "<TR><TD CLASS='LABEL'>Vol</TD><TD>" + String(volume) + "/63</TD></TR>"
    "<TR><TD CLASS='LABEL'>Bat</TD><TD>" + String(batteryMonitor(), 2) + "V</TD></TR>"
    "<TR><TD CLASS='LABEL'>FW</TD><TD>" + String(getVersion(true)) + "</TD></TR>"
    "<TR><TD CLASS='LABEL'>Time</TD><TD>" + webReceiverTime() + "</TD></TR>"
    "<TR><TD CLASS='LABEL'>Network</TD><TD>" + ssid + " (" + ip + ")</TD></TR>"
    "</TABLE>"

    "</DIV>";

  return webPage(body);
}

static const String webRemotePage()
{
  String freq = currentMode == FM?
    String(currentFrequency / 100.0)
  : String(currentFrequency + currentBFO / 1000.0);
  const char *unit = currentMode == FM? "MHz" : "kHz";

  String body = "<DIV CLASS='CONTAINER'>"
    "<H1>Remote</H1>" + webNavigation("/remote") +

    "<DIV CLASS='CENTER' STYLE='margin-bottom:8px;'>"
    "<BUTTON CLASS='btn-q active' ONCLICK='setQ(1)' ID='q1'>HIGH</button>"
    "<BUTTON CLASS='btn-q' ONCLICK='setQ(2)' ID='q2'>MED</button>"
    "<BUTTON CLASS='btn-q' ONCLICK='setQ(3)' ID='q3'>LOW</button>"
    "</DIV>"
    "<DIV CLASS='CENTER' STYLE='margin-bottom:16px;'>"
    "<CANVAS ID='screen' STYLE='max-width:240px;width:100%;border-radius:8px;"
    "border:1px solid #eee;background:#000;image-rendering:pixelated;'></CANVAS>"
    "</DIV>"

    // Direct frequency entry
    "<DIV CLASS='tuner'>"
    "<INPUT TYPE='TEXT' ID='f' INPUTMODE='DECIMAL' AUTOCOMPLETE='off' "
    "PLACEHOLDER='" + String(freq) + "' "
    "ONKEYPRESS=\"if(event.key==='Enter')goFreq();\">"
    "<SPAN CLASS='unit' ID='fu'>" + String(unit) + "</SPAN>"
    "<BUTTON CLASS='btn-go' ONCLICK='goFreq()'>GO</BUTTON>"
    "</DIV>"
    "<DIV CLASS='CENTER'><SMALL ID='err' STYLE='color:#c00;'></SMALL></DIV>"
    "<DIV CLASS='CENTER' STYLE='margin:8px 0 12px;'>"
    "<SPAN CLASS='rstat' ID='rfreq'>" + String(freq) + " " + String(unit) + "</SPAN>"
    " <SMALL ID='rmode'>" + String(bandModeDesc[currentMode]) + "</SMALL>"
    " &middot; <SPAN CLASS='rstat' ID='rvol'>" + String(volume) + "/63</SPAN>"
    "</DIV>"

    // 3-button control
    "<DIV CLASS='ctrl'>"
    "<BUTTON CLASS='btn-left' ONCLICK='cmd(\"r\")'>&#9664;</BUTTON>"
    "<BUTTON CLASS='btn-ok' ONCLICK='cmd(\"e\")'>OK</BUTTON>"
    "<BUTTON CLASS='btn-right' ONCLICK='cmd(\"R\")'>&#9654;</BUTTON>"
    "<BUTTON CLASS='btn-ok' ONCLICK='cmd(\"E\")'>MENU</BUTTON>"
    "</DIV>"
    "<DIV CLASS='ctrl'>"
    "<BUTTON CLASS='btn-sm' ONCLICK='cmd(\"b\")'>B-</BUTTON>"
    "<BUTTON CLASS='btn-sm' ONCLICK='cmd(\"B\")'>B+</BUTTON>"
    "<BUTTON CLASS='btn-sm' ONCLICK='cmd(\"v\")'>V-</BUTTON>"
    "<BUTTON CLASS='btn-sm' ONCLICK='cmd(\"V\")'>V+</BUTTON>"
    "<BUTTON CLASS='btn-sm' ONCLICK='cmd(\"m\")'>M-</BUTTON>"
    "<BUTTON CLASS='btn-sm' ONCLICK='cmd(\"M\")'>M+</BUTTON>"
    "</DIV>"

    "<TABLE>"
    "<TR><TD CLASS='LABEL'>Band</TD><TD ID='band'>"
    + String(getCurrentBand()->bandName) + "</TD></TR>"
    "<TR><TD CLASS='LABEL'>Signal</TD><TD>"
    "<DIV CLASS='sbar'><DIV CLASS='sfill' ID='sf' STYLE='width:"
    + String(constrain(rssi * 100 / 120, 5, 100)) + "%'></DIV></DIV>"
    + String(rssi) + "dBuV SNR " + String(snr) + "dB</TD></TR>"
    "<TR><TD CLASS='LABEL'>Vol</TD><TD ID='vol'>" + String(volume) + "/63</TD></TR>"
    "</TABLE>"

    "</DIV>"

    "<STYLE>"
    ".ctrl{display:flex;justify-content:center;gap:8px;margin-bottom:12px;}"
    ".btn-left,.btn-right{width:64px;height:64px;border:none;border-radius:50%;"
    "background:#111;color:#fff;font-size:24px;cursor:pointer;}"
    ".btn-ok{width:64px;height:64px;border:none;border-radius:50%;"
    "background:#333;color:#fff;font-size:14px;font-weight:600;cursor:pointer;}"
    ".btn-sm{padding:8px 14px;border:none;border-radius:8px;"
    "background:#f0f0f0;color:#333;font-size:13px;font-weight:500;cursor:pointer;}"
    ".btn-left:active,.btn-right:active,.btn-ok:active,.btn-sm:active{background:#666;}"
    ".sbar{width:100%;height:6px;background:#eee;border-radius:3px;margin-bottom:4px;}"
    ".sfill{height:100%;background:#333;border-radius:3px;transition:width .3s;}"
    ".btn-q{padding:4px 12px;border:1px solid #ddd;border-radius:6px;"
    "background:#fafafa;color:#888;font-size:11px;cursor:pointer;margin:0 2px;}"
    ".btn-q.active{background:#111;color:#fff;border-color:#111;}"
    ".tuner{display:flex;align-items:center;gap:8px;margin-bottom:4px;}"
    ".tuner INPUT{flex:1;min-width:0;padding:10px 12px;border:1px solid #ddd;"
    "border-radius:8px;font-size:0.9rem;background:#fafafa;}"
    ".tuner .unit{color:#888;font-size:0.85rem;min-width:28px;}"
    ".btn-go{padding:10px 18px;border:none;border-radius:8px;background:#111;"
    "color:#fff;font-weight:600;cursor:pointer;}"
    ".rstat{font-weight:600;}"
    "</STYLE>"

    "<SCRIPT>"
    "var qs=1,curMode='" + String(bandModeDesc[currentMode]) + "';"
    "var prevPx=null,prevW=0,prevH=0,busy=false;"
    "var cvs=document.getElementById('screen');"
    "var ctx=cvs.getContext('2d');"
    "function hashPx(a){"
    "var h=2166136261;"
    "for(var i=0;i<a.length;i++){h^=a[i];h=Math.imul(h,16777619)>>>0;}"
    "return h>>>0;}"
    "function toRGBA(p,d,o){"
    "var r=(p>>11)&31,g=(p>>5)&63,b=p&31;"
    "d[o]=(r<<3)|(r>>2);d[o+1]=(g<<2)|(g>>4);d[o+2]=(b<<3)|(b>>2);d[o+3]=255;}"
    "function setQ(s){"
    "qs=s;prevPx=null;"
    "document.querySelectorAll('.btn-q').forEach(function(b){b.classList.remove('active');});"
    "document.getElementById('q'+s).classList.add('active');"
    "updateScreen();}"
    "function showErr(m){document.getElementById('err').textContent=m||'';}"
    "function cmd(c){"
    "showErr('');"
    "fetch('/api/cmd',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},"
    "body:'cmd='+encodeURIComponent(c)})"
    ".then(function(r){return r.json().then(function(d){"
    "if(!r.ok||d.error){showErr(d.error||'Error');return;}"
    "refresh();updateScreen();});})"
    ".catch(function(){showErr('Network error');});}"
    "function goFreq(){"
    "var el=document.getElementById('f');"
    "var v=parseFloat(el.value);"
    "if(!(v>0)){showErr('Enter a frequency');return;}"
    "var hz=(curMode==='FM')?Math.round(v*1e6):Math.round(v*1000);"
    "cmd('F'+hz);}"
    "function refresh(){"
    "fetch('/api/status').then(function(r){return r.json();}).then(function(d){"
    "curMode=d.mode;"
    "document.getElementById('band').textContent=d.band;"
    "document.getElementById('vol').textContent=d.volume+'/63';"
    "document.getElementById('rvol').textContent=d.volume+'/63';"
    "document.getElementById('rmode').textContent=d.mode;"
    "document.getElementById('rfreq').textContent=d.frequency+' '+((d.mode==='FM')?'MHz':'kHz');"
    "document.getElementById('fu').textContent=(d.mode==='FM')?'MHz':'kHz';"
    "var w=Math.min(Math.max(d.rssi*100/120,5),100);"
    "document.getElementById('sf').style.width=w+'%';"
    "});}"
    "function updateScreen(){"
    "if(busy)return;busy=true;"
    "var h=prevPx?hashPx(prevPx):0;"
    "fetch('/api/screen?scale='+qs+'&delta=1&h='+h+'&t='+Date.now())"
    ".then(function(r){if(!r.ok)throw 0;return r.arrayBuffer();})"
    ".then(function(buf){"
    "var v=new DataView(buf);"
    "if(v.byteLength<6||v.getUint8(0)!==0x53){busy=false;return;}"
    "var flags=v.getUint8(1);"
    "var w=v.getUint16(2,true),hh=v.getUint16(4,true);"
    "var off=6;"
    "if(cvs.width!==w||cvs.height!==hh){"
    "if(!(flags&1)){busy=false;prevPx=null;return;}"
    "cvs.width=w;cvs.height=hh;}"
    "if(flags&1){"
    "prevPx=new Uint16Array(w*hh);"
    "var img=ctx.createImageData(w,hh);"
    "for(var i=0;i<w*hh;i++){"
    "var p=v.getUint16(off,true);off+=2;prevPx[i]=p;toRGBA(p,img.data,i*4);}"
    "ctx.putImageData(img,0,0);"
    "}else{"
    "if(!prevPx||prevPx.length!==w*hh){busy=false;prevPx=null;return;}"
    "var n=v.getUint16(off,true);off+=2;"
    "for(var t=0;t<n;t++){"
    "var x=v.getUint16(off,true),y=v.getUint16(off+2,true);"
    "var tw=v.getUint16(off+4,true),th=v.getUint16(off+6,true);off+=8;"
    "var img=ctx.createImageData(tw,th);"
    "for(var j=0;j<tw*th;j++){"
    "var p=v.getUint16(off,true);off+=2;"
    "prevPx[(y+((j/tw)|0))*w+x+(j%tw)]=p;"
    "toRGBA(p,img.data,j*4);}"
    "ctx.putImageData(img,x,y);"
    "}}"
    "busy=false;})"
    ".catch(function(){busy=false;});}"
    "refresh();updateScreen();"
    "setInterval(refresh,1000);"
    "setInterval(updateScreen,2000);"
    "</SCRIPT>";

  return webPage(body);
}

static const String webMemoryPage()
{
  String items = "";

  for(int j=0 ; j<MEMORY_COUNT ; j++)
  {
    char text[64];
    sprintf(text, "<TR><TD CLASS='LABEL' WIDTH='50px'>%02d</TD><TD>", j+1);
    items += text;

    if(!memories[j].freq)
      items += "&mdash;</TD></TR>";
    else
    {
      String freq = memories[j].mode == FM?
        String(memories[j].freq / 1000000.0) + " MHz"
      : String(memories[j].freq / 1000.0) + " kHz";
      items += freq + " <SMALL>" + bandModeDesc[memories[j].mode] + "</SMALL></TD></TR>";
    }
  }

  return webPage(
"<DIV CLASS='CONTAINER'>"
"<H1>Memory</H1>" + webNavigation("/memory") +
"<TABLE>" + items + "</TABLE>"
"</DIV>"
);
}

const String webConfigPage()
{
  prefs.begin("network", true, STORAGE_PARTITION);
  String ssid1 = prefs.getString("wifissid1", "");
  String pass1 = prefs.getString("wifipass1", "");
  String ssid2 = prefs.getString("wifissid2", "");
  String pass2 = prefs.getString("wifipass2", "");
  String ssid3 = prefs.getString("wifissid3", "");
  String pass3 = prefs.getString("wifipass3", "");
  bool scanHidden = prefs.getBool("wifiscanhidden", false);
  prefs.end();

  String splashImage = LittleFS.exists(SPLASH_PATH)?
    "<IMG SRC='/splash.png?" + String(millis()) + "' ALT='Current splash screen' STYLE='max-width:100%;height:auto;border-radius:8px;'>"
  : "Not installed";
  String splashResolution = String(spr.width()) + "x" + String(spr.height());

  return webPage(
"<DIV CLASS='CONTAINER'>"
"<H1>Config</H1>" + webNavigation("/config") +
"<FORM ACTION='/setconfig' METHOD='POST' ENCTYPE='multipart/form-data' ONSUBMIT='browserDateTime(true)'>"

  "<TABLE>"
  "<TR><TH COLSPAN=2>WiFi Network 1</TH></TR>"
  "<TR>"
    "<TD CLASS='LABEL'>SSID</TD>"
    "<TD>" + webInputField("wifissid1", ssid1) + "</TD>"
  "</TR>"
  "<TR>"
    "<TD CLASS='LABEL'>Password</TD>"
    "<TD>" + webInputField("wifipass1", pass1, true) + "</TD>"
  "</TR>"
  "</TABLE>"

  "<TABLE>"
  "<TR><TH COLSPAN=2>WiFi Network 2</TH></TR>"
  "<TR>"
    "<TD CLASS='LABEL'>SSID</TD>"
    "<TD>" + webInputField("wifissid2", ssid2) + "</TD>"
  "</TR>"
  "<TR>"
    "<TD CLASS='LABEL'>Password</TD>"
    "<TD>" + webInputField("wifipass2", pass2, true) + "</TD>"
  "</TR>"
  "</TABLE>"

  "<TABLE>"
  "<TR><TH COLSPAN=2>WiFi Network 3</TH></TR>"
  "<TR>"
    "<TD CLASS='LABEL'>SSID</TD>"
    "<TD>" + webInputField("wifissid3", ssid3) + "</TD>"
  "</TR>"
  "<TR>"
    "<TD CLASS='LABEL'>Password</TD>"
    "<TD>" + webInputField("wifipass3", pass3, true) + "</TD>"
  "</TR>"
  "</TABLE>"

  "<TABLE>"
  "<TR><TH COLSPAN=2>Web Login</TH></TR>"
  "<TR>"
    "<TD CLASS='LABEL'>Username</TD>"
    "<TD>" + webInputField("username", loginUsername) + "</TD>"
  "</TR>"
  "<TR>"
    "<TD CLASS='LABEL'>Password</TD>"
    "<TD>" + webInputField("password", loginPassword, true) + "</TD>"
  "</TR>"
  "</TABLE>"

  "<TABLE>"
  "<TR><TH COLSPAN=2>Settings</TH></TR>"
  "<TR>"
    "<TD CLASS='LABEL'>Scan Hidden</TD>"
    "<TD><INPUT TYPE='CHECKBOX' NAME='wifiscanhidden' VALUE='on'" +
    (scanHidden? " CHECKED ":"") + "></TD>"
  "</TR>"
  "<TR>"
    "<TD CLASS='LABEL'>Browser Time</TD>"
    "<TD><INPUT TYPE='CHECKBOX' ID='browserdatetime' ONCHANGE='browserDateTime()'></TD>"
  "</TR>"
  "<TR>"
    "<TD CLASS='LABEL'>UTC Date/Time</TD>"
    "<TD><INPUT TYPE='TEXT' ID='datetime' NAME='datetime' PLACEHOLDER='YYYY-mm-dd HH:MM:SS'></TD>"
  "</TR>"
  "<TR>"
    "<TD CLASS='LABEL'>Time Zone</TD>"
    "<TD>"
      "<SELECT ID='utcoffset' NAME='utcoffset'>" + webUtcOffsetSelector() + "</SELECT>"
    "</TD>"
  "</TR>"
  "<TR>"
    "<TD CLASS='LABEL'>Theme</TD>"
    "<TD>"
      "<SELECT NAME='theme'>" + webThemeSelector() + "</SELECT>"
    "</TD>"
  "</TR>"
  "<TR>"
    "<TD CLASS='LABEL'>Reverse Scroll</TD>"
    "<TD><INPUT TYPE='CHECKBOX' NAME='scroll' VALUE='on'" +
    (scrollDirection<0? " CHECKED ":"") + "></TD>"
  "</TR>"
  "<TR>"
    "<TD CLASS='LABEL'>Half-step</TD>"
    "<TD><INPUT TYPE='CHECKBOX' NAME='encoderhalfstep' VALUE='on'" +
    (encoderHalfStep? " CHECKED ":"") + "></TD>"
  "</TR>"
  "<TR>"
    "<TD CLASS='LABEL'>Zoomed Menu</TD>"
    "<TD><INPUT TYPE='CHECKBOX' NAME='zoom' VALUE='on'" +
    (zoomMenu? " CHECKED ":"") + "></TD>"
  "</TR>"
  "</TABLE>"

  "<TABLE>"
  "<TR><TH COLSPAN=2>Splash Screen</TH></TR>"
  "<TR>"
    "<TD CLASS='LABEL'>Current</TD>"
    "<TD>" + splashImage + "</TD>"
  "</TR>"
  "<TR>"
    "<TD CLASS='LABEL'>Upload</TD>"
    "<TD><INPUT TYPE='FILE' NAME='splash' ACCEPT='.png'>"
    "<SMALL>Resolution: " + splashResolution + "px / Max: 512 KB</SMALL></TD>"
  "</TR>"
  "<TR>"
    "<TD CLASS='LABEL'>Delete</TD>"
    "<TD><INPUT TYPE='CHECKBOX' NAME='deletesplash' VALUE='on'></TD>"
  "</TR>"
  "<TR><TD COLSPAN=2 CLASS='CENTER' STYLE='padding-top:16px;'>"
    "<INPUT TYPE='SUBMIT' VALUE='Save'>"
  "</TD></TR>"
  "</TABLE>"
"</FORM>"

"<TABLE STYLE='margin-top:16px;'><TR><TH COLSPAN=2>Settings Backup</TH></TR>"
"<TR><TD CLASS='LABEL'>Export</TD>"
"<TD><A HREF='/api/export' DOWNLOAD STYLE='display:inline-block;padding:8px 16px;background:#111;color:#fff;border-radius:8px;text-decoration:none;'>Download JSON</A></TD></TR>"
"<TR><TD CLASS='LABEL'>Import</TD>"
"<TD STYLE='display:flex;gap:8px;align-items:center;'>"
"<INPUT TYPE='FILE' ID='import-file' ACCEPT='.json'>"
"<BUTTON ONCLICK='importSettings()'>Upload</BUTTON>"
"</TD></TR>"
"</TABLE>"
"<SCRIPT>"
"function browserDateTime(submit)"
"{"
  "const enabled=document.getElementById('browserdatetime').checked;"
  "const dateTime=document.getElementById('datetime');"
  "const utcOffset=document.getElementById('utcoffset');"
  "if(enabled)"
  "{"
    "const now=new Date();"
    "dateTime.value=now.toISOString().slice(0,19).replace('T',' ');"
    "const minutes=-now.getTimezoneOffset();"
    "for(const option of utcOffset.options)"
      "if(Number(option.dataset.minutes)===minutes) utcOffset.value=option.value;"
  "}"
  "dateTime.disabled=utcOffset.disabled=enabled&&!submit;"
"}"
"</SCRIPT>"
"<SCRIPT>"
"function importSettings(){"
"var f=document.getElementById('import-file').files[0];"
"if(!f){alert('Select a file');return;}"
"var r=new FileReader();"
"r.onload=function(e){"
"fetch('/api/import',{method:'POST',headers:{'Content-Type':'application/json'},body:e.target.result})"
".then(function(r){return r.json();})"
".then(function(d){if(d.ok)alert('Settings imported!');else alert('Error');});};"
"r.readAsText(f);"
"}"
"</SCRIPT>"
"</DIV>"
);
}

// Explicit request errors leave the active operation's status unchanged.
static void webUpdatePage(AsyncWebServerRequest *request, const OtaStatus &status, int code)
{
  const bool busy = status.phase == OTA_CHECK_QUEUED || status.phase == OTA_QUEUED ||
                    status.phase == OTA_CONNECTING || status.phase == OTA_WRITING;
  const bool complete = status.phase == OTA_COMPLETE || status.phase == OTA_REBOOT_PENDING;
  const String refresh = complete? "<SCRIPT>setTimeout(()=>location.replace('/'),20000);</SCRIPT>" :
                         busy? "<SCRIPT>setTimeout(()=>location.replace('/update'),1000);</SCRIPT>" : "";
  const String page = webPage(
"<DIV CLASS='CONTAINER'>"
"<H1>Update</H1>" + webNavigation("/update") +
"<TABLE>"
"<TR><TD CLASS='CENTER'>" + status.message + "</TD></TR>"
"<TR><TD>"
  "<DETAILS OPEN><SUMMARY>Manual upload</SUMMARY>"
  "<FORM METHOD='POST' ACTION='/update/upload' ENCTYPE='multipart/form-data' ONSUBMIT='this.elements.size.value=this.elements.firmware.files[0].size;this.querySelector(\"button\").disabled=true;'>"
  "<INPUT TYPE='HIDDEN' NAME='size'>"
  "<P STYLE='margin-top:12px;'><INPUT TYPE='FILE' NAME='firmware' ARIA-LABEL='Firmware file' ACCEPT='.bin' REQUIRED" + String(busy || complete? " DISABLED" : "") + "></P>"
  "<SMALL>Use the <CODE>-ota.bin</CODE> or <CODE>ats-mini.ino.bin</CODE> for your receiver variant.</SMALL>"
  "<P STYLE='margin-top:12px;'>"
  "<BUTTON TYPE='SUBMIT'" + String(busy || complete? " DISABLED" : "") + ">Upload</BUTTON>"
  "</P>"
  "</FORM>"
  "</DETAILS>"
"</TD></TR>"
"</TABLE>"
"</DIV>" + refresh
);
  if(!code) code = status.phase == OTA_FAILED? 400 : 200;
  AsyncWebServerResponse *response = request->beginResponse(code, "text/html", page);
  response->addHeader("Cache-Control", "no-store");
  response->addHeader("Connection", "close");
  request->send(response);
}
