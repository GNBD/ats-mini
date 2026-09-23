// ATS Mini standalone recovery firmware (beta) - ESP32 BOOTmgr
// Network update + WiFi setting beta. Runs from its own partition.
//
// Boot sequence:
//   1. Power ON -> recovery always boots first
//   2. Wait 1 second for encoder press
//   3. No encoder -> auto boot to app0
//   4. Encoder held -> recovery menu (STA WiFi, WiFi section, update, etc.)
//   5. Recovery always sets itself as next boot target before jumping to app
#include <LovyanGFX.hpp>
#include <WiFi.h>
#include <WebServer.h>
#include <HTTPClient.h>
#include <LittleFS.h>
#include <Preferences.h>
#include <esp_wifi.h>
#include <esp_partition.h>
#include <esp_ota_ops.h>
#include <nvs_flash.h>
#include <qrcode.h>
#include "Display.h"
#include "Rotary.h"

#define PIN_LCD_BL          38
#define ENCODER_PIN_A        2
#define ENCODER_PIN_B        1
#define ENCODER_PUSH_BUTTON 21
#define STORAGE_PARTITION    "settings"

#define WIFI_PAGE_SIZE       4
#define FILE_PAGE_SIZE       4
#define MAX_SCAN_NETWORKS    20
#define MAX_URLS             8
#define WIFI_CONNECT_TIMEOUT 15000
#define BOOT_CONNECT_TIMEOUT 5000
#define KB_MAX_KEYS          48

static constexpr const lgfx::IFont* FONT_LARGE = &lgfx::fonts::Font4;
static constexpr const lgfx::IFont* FONT_SMALL = &lgfx::fonts::Font2;
static constexpr const lgfx::IFont* FONT_TINY  = &lgfx::fonts::Font0;

#define COL_BG      0x0000
#define COL_TEXT    0xFFFF
#define COL_MUTED   0x8410
#define COL_WARN    0xF800
#define COL_OK      0x07E0
#define COL_AP      0xFFE0
#define COL_KEY     0x1082
#define COL_KEYSEL  0xFFFF

#define RECOVERY_VERSION "2.0.0-beta"

// Default: fetch this .txt (one URL per line). Local /update_url.txt and
// DEFAULT_UPDATE_URLS are fallbacks when the remote list is unavailable.
static const char *REMOTE_UPDATE_URL_TXT =
  "https://gnbdatsmini.netlify.app/update_url.txt";

static const char *DEFAULT_UPDATE_URLS[MAX_URLS] = {};

LGFX tft;
WebServer server(80);
Rotary encoder(ENCODER_PIN_A, ENCODER_PIN_B, true);
Preferences prefs;

static String apIP;
static bool apModeActive = false;
static bool serverRunning = false;

static const char *gBootSlot = "App0";
static bool gBootIsApp1 = false;

static const char *menu[] = {
  "Boot App0",
  "Boot App1",
  "Firmware Update",
  "WiFi",
  "Erase",
  "About"
};
#define MENU_COUNT (sizeof(menu) / sizeof(menu[0]))

static const char *wifiMenu[] = {
  "WiFi Setting",
  "WiFi File Manager"
};
#define WIFI_MENU_COUNT (sizeof(wifiMenu) / sizeof(wifiMenu[0]))

// ---------------------------------------------------------------------------
// Input helpers
// ---------------------------------------------------------------------------

static int8_t readEncoder()
{
  static unsigned char last = DIR_NONE;
  unsigned char dir = encoder.process();
  if(dir == last) return 0;
  last = dir;
  if(dir == DIR_CW) return -1;
  if(dir == DIR_CCW) return 1;
  return 0;
}

static uint32_t readButton()
{
  if(digitalRead(ENCODER_PUSH_BUTTON) != LOW) return 0;
  delay(30);
  if(digitalRead(ENCODER_PUSH_BUTTON) != LOW) return 0;

  uint32_t start = millis();
  while(digitalRead(ENCODER_PUSH_BUTTON) == LOW) delay(10);
  return millis() - start;
}

// ---------------------------------------------------------------------------
// WiFi helpers
// ---------------------------------------------------------------------------

static void applyTxPower()
{
  WiFi.setTxPower(WIFI_POWER_13dBm);
}

static void drawNetIcon()
{
  tft.fillRect(290, 4, 30, 18, COL_BG);
  if(apModeActive)
  {
    tft.setTextDatum(TL_DATUM);
    tft.setTextColor(COL_AP, COL_BG);
    tft.drawString("AP", 300, 6, FONT_SMALL);
    return;
  }
  if(WiFi.status() != WL_CONNECTED)
  {
    tft.setTextColor(COL_WARN, COL_BG);
    tft.drawLine(300, 7, 310, 17, COL_WARN);
    tft.drawLine(310, 7, 300, 17, COL_WARN);
    return;
  }
  tft.drawCircle(305, 17, 2, COL_OK);
  tft.drawArc(305, 17, 5, 6, 225, 315, COL_OK);
  tft.drawArc(305, 17, 9, 10, 225, 315, COL_OK);
}

static void drawHeader(const char *title, const char *right = nullptr)
{
  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(COL_TEXT, COL_BG);
  tft.drawString(title, 10, 6, FONT_LARGE);
  drawNetIcon();
  if(right)
  {
    tft.setTextDatum(TR_DATUM);
    tft.setTextColor(COL_OK, COL_BG);
    tft.drawString(right, 294, 8, FONT_SMALL);
    tft.setTextDatum(TL_DATUM);
  }
}

static bool loadWifiCredentials(String &ssid, String &pass)
{
  if(!prefs.begin("wificfg", true, STORAGE_PARTITION)) return false;
  ssid = prefs.getString("ssid", "");
  pass = prefs.getString("pass", "");
  prefs.end();
  return ssid.length() > 0;
}

static void saveWifiCredentials(const String &ssid, const String &pass)
{
  if(!prefs.begin("wificfg", false, STORAGE_PARTITION)) return;
  prefs.putString("ssid", ssid);
  prefs.putString("pass", pass);
  prefs.end();
}

static bool wifiConnectBlocking(const String &ssid, const String &pass, uint32_t timeoutMs)
{
  WiFi.mode(WIFI_STA);
  applyTxPower();
  WiFi.setSleep(false);
  WiFi.begin(ssid.c_str(), pass.c_str());
  uint32_t start = millis();
  while(WiFi.status() != WL_CONNECTED && (millis() - start) < timeoutMs)
    delay(50);
  return WiFi.status() == WL_CONNECTED;
}

static void wifiStartAp()
{
  WiFi.disconnect(true);
  delay(50);
  WiFi.mode(WIFI_AP);
  applyTxPower();
  WiFi.softAP("ats-recovery", "12345678");
  esp_wifi_set_ps(WIFI_PS_NONE);
  WiFi.setSleep(false);
  apIP = WiFi.softAPIP().toString();
  apModeActive = true;
}

static void wifiStopApToSta()
{
  if(serverRunning)
  {
    server.stop();
    serverRunning = false;
  }
  WiFi.softAPdisconnect(true);
  apModeActive = false;

  String ssid, pass;
  WiFi.mode(WIFI_STA);
  applyTxPower();
  if(loadWifiCredentials(ssid, pass))
    WiFi.begin(ssid.c_str(), pass.c_str());
}

// ---------------------------------------------------------------------------
// Boot helpers
// ---------------------------------------------------------------------------

static void bootToApp(esp_partition_subtype_t appSubtype)
{
  const esp_partition_t *app = esp_partition_find_first(
    ESP_PARTITION_TYPE_APP, appSubtype, NULL);
  if(app) esp_ota_set_boot_partition(app);
  ESP.restart();
}

static void bootToApp0()
{
  bootToApp(static_cast<esp_partition_subtype_t>(ESP_PARTITION_SUBTYPE_APP_OTA_0));
}

static void bootToApp1()
{
  bootToApp(static_cast<esp_partition_subtype_t>(ESP_PARTITION_SUBTYPE_APP_OTA_1));
}

// ---------------------------------------------------------------------------
// Drawing
// ---------------------------------------------------------------------------

static void drawMenu(int selected)
{
  tft.fillScreen(COL_BG);
  drawHeader("ESP32 BOOTmgr", gBootSlot);
  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(COL_MUTED, COL_BG);
  tft.drawString("Hold = Select  Click = Back", 10, 30, FONT_SMALL);

  for(uint8_t i = 0; i < MENU_COUNT; i++)
  {
    int y = 48 + i * 18;
    if(i == selected)
    {
      tft.fillRoundRect(5, y - 1, 310, 18, 4, COL_TEXT);
      tft.setTextColor(COL_BG, COL_TEXT);
    }
    else
    {
      tft.setTextColor(COL_TEXT, COL_BG);
    }
    tft.drawString(menu[i], 15, y, FONT_SMALL);
  }
}

static void drawWifiMenu(int selected)
{
  tft.fillScreen(COL_BG);
  drawHeader("WiFi");
  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(COL_MUTED, COL_BG);
  tft.drawString("Hold = Select  Click = Back", 10, 30, FONT_SMALL);

  String status;
  if(apModeActive) status = "AP Mode";
  else if(WiFi.status() == WL_CONNECTED) status = WiFi.SSID();
  else status = "Offline";
  tft.setTextColor(COL_MUTED, COL_BG);
  tft.drawString(status, 10, 150, FONT_SMALL);

  for(uint8_t i = 0; i < WIFI_MENU_COUNT; i++)
  {
    int y = 52 + i * 22;
    if(i == selected)
    {
      tft.fillRoundRect(5, y - 2, 310, 22, 4, COL_TEXT);
      tft.setTextColor(COL_BG, COL_TEXT);
    }
    else
    {
      tft.setTextColor(COL_TEXT, COL_BG);
    }
    tft.drawString(wifiMenu[i], 15, y, FONT_SMALL);
  }
}

static void drawUpdateProgress(int percent, const char *status)
{
  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(COL_TEXT, COL_BG);
  tft.drawString(status, 10, 65, FONT_SMALL);
  drawNetIcon();

  tft.fillRoundRect(10, 90, 300, 20, 4, 0x4208);
  int w = (percent * 296) / 100;
  if(w > 0) tft.fillRoundRect(12, 92, w, 16, 3, COL_TEXT);

  char buf[16];
  sprintf(buf, "%d%%", percent);
  tft.setTextDatum(MC_DATUM);
  tft.drawString(buf, 160, 100, FONT_SMALL);
  tft.setTextDatum(TL_DATUM);
}

// ---------------------------------------------------------------------------
// Firmware flashing (app OTA)
// ---------------------------------------------------------------------------

static int listFirmwareFiles(String *names, int maxCount)
{
  int count = 0;
  File root = LittleFS.open("/", "r");
  if(!root || !root.isDirectory()) return 0;

  File file = root.openNextFile();
  while(file && count < maxCount)
  {
    String name = file.name();
    if(name.endsWith(".bin"))
      names[count++] = name;
    file = root.openNextFile();
  }
  root.close();
  return count;
}

static void drawFilePage(String *files, int count, int selected, const char *targetLabel)
{
  tft.fillScreen(COL_BG);
  drawHeader("FIRMWARE UPDATE");
  tft.setTextColor(COL_MUTED, COL_BG);
  String target = String("Target: ") + targetLabel + "  Select .bin:";
  tft.drawString(target, 10, 35, FONT_SMALL);

  int page = selected / FILE_PAGE_SIZE;
  int start = page * FILE_PAGE_SIZE;
  int end = start + FILE_PAGE_SIZE;
  if(end > count) end = count;

  for(int i = start; i < end; i++)
  {
    int row = i - start;
    int y = 58 + row * 24;
    if(i == selected)
    {
      tft.fillRoundRect(5, y - 2, 310, 22, 4, COL_TEXT);
      tft.setTextColor(COL_BG, COL_TEXT);
    }
    else
    {
      tft.setTextColor(COL_TEXT, COL_BG);
    }
    tft.drawString(files[i], 15, y, FONT_SMALL);
  }

  int pages = (count + FILE_PAGE_SIZE - 1) / FILE_PAGE_SIZE;
  if(pages < 1) pages = 1;
  char pageBuf[16];
  snprintf(pageBuf, sizeof(pageBuf), "Page %d/%d", page + 1, pages);
  tft.setTextColor(COL_MUTED, COL_BG);
  tft.drawString(pageBuf, 10, 155, FONT_SMALL);
  tft.setTextDatum(TR_DATUM);
  tft.drawString("Click=Back Hold=Flash", 312, 155, FONT_SMALL);
  tft.setTextDatum(TL_DATUM);
}

static void drawChoiceList(const char *title, const char *hint,
                           const char **items, int itemCount, int selected)
{
  tft.fillScreen(COL_BG);
  drawHeader(title);
  tft.setTextColor(COL_MUTED, COL_BG);
  tft.drawString(hint, 10, 35, FONT_SMALL);

  int page = selected / FILE_PAGE_SIZE;
  int start = page * FILE_PAGE_SIZE;
  int end = start + FILE_PAGE_SIZE;
  if(end > itemCount) end = itemCount;

  for(int i = start; i < end; i++)
  {
    int row = i - start;
    int y = 58 + row * 24;
    if(i == selected)
    {
      tft.fillRoundRect(5, y - 2, 310, 22, 4, COL_TEXT);
      tft.setTextColor(COL_BG, COL_TEXT);
    }
    else
    {
      tft.setTextColor(COL_TEXT, COL_BG);
    }
    tft.drawString(items[i], 15, y, FONT_SMALL);
  }

  tft.setTextColor(COL_MUTED, COL_BG);
  if(itemCount > FILE_PAGE_SIZE)
  {
    int pages = (itemCount + FILE_PAGE_SIZE - 1) / FILE_PAGE_SIZE;
    char pageBuf[16];
    snprintf(pageBuf, sizeof(pageBuf), "Page %d/%d", page + 1, pages);
    tft.drawString(pageBuf, 10, 155, FONT_SMALL);
  }
  tft.setTextDatum(TR_DATUM);
  tft.drawString("Click=Back Hold=Select", 312, 155, FONT_SMALL);
  tft.setTextDatum(TL_DATUM);
}

// Generic single-page choice: rotate + hold=select, short=back.
// Returns selected index, or -1 on back.
static int runChoice(const char *title, const char *hint,
                     const char **items, int itemCount)
{
  int selected = 0;
  drawChoiceList(title, hint, items, itemCount, selected);

  while(true)
  {
    int8_t d = readEncoder();
    if(d > 0)
    {
      selected = (selected + 1) % itemCount;
      drawChoiceList(title, hint, items, itemCount, selected);
    }
    else if(d < 0)
    {
      selected = (selected + itemCount - 1) % itemCount;
      drawChoiceList(title, hint, items, itemCount, selected);
    }

    uint32_t h = readButton();
    if(h >= 300) return selected;
    if(h > 0) return -1;
    delay(10);
  }
}

static uint32_t findFirstAppOffset(File &file, size_t fileSize)
{
  if(fileSize < (0x8000 + 32)) return 0;

  file.seek(0x8000);
  for(int i = 0; i < 95; i++)
  {
    uint8_t e[32];
    if(file.read(e, 32) != 32) break;
    if(e[0] != 0xAA || e[1] != 0x50) break;

    uint8_t type = e[2];
    uint8_t sub  = e[3];
    uint32_t addr = e[4] | (e[5] << 8) | (e[6] << 16) | ((uint32_t)e[7] << 24);

    if(type == 0x00 && sub >= 0x10 && sub <= 0x1F) return addr;
  }
  return 0;
}

static size_t computeAppImageSize(File &file, uint32_t appOffset, size_t fileSize)
{
  if((appOffset + 24) > fileSize) return 0;

  file.seek(appOffset);
  uint8_t hdr[24];
  if(file.read(hdr, 24) != 24 || hdr[0] != 0xE9) return 0;

  uint8_t segCount = hdr[1];
  uint32_t pos = 24;

  for(uint8_t i = 0; i < segCount; i++)
  {
    uint8_t sh[8];
    if(file.read(sh, 8) != 8) return 0;
    uint32_t dlen = sh[4] | (sh[5] << 8) | (sh[6] << 16) | ((uint32_t)sh[7] << 24);
    pos += 8 + dlen;
    if((appOffset + pos) > fileSize) return 0;
    file.seek(appOffset + pos);
  }

  pos += 1;
  if(pos % 16) pos += 16 - (pos % 16);
  if(hdr[23] == 1) pos += 32;

  return pos;
}

static bool flashFirmware(const String &filename, int targetSlot)
{
  File file = LittleFS.open("/" + filename, "r");
  if(!file || file.size() == 0)
  {
    tft.setTextColor(COL_WARN, COL_BG);
    tft.drawString("Failed to open file", 10, 65, FONT_SMALL);
    delay(2000);
    return false;
  }

  esp_partition_subtype_t targetSubtype = (targetSlot == 0)
    ? static_cast<esp_partition_subtype_t>(ESP_PARTITION_SUBTYPE_APP_OTA_0)
    : static_cast<esp_partition_subtype_t>(ESP_PARTITION_SUBTYPE_APP_OTA_1);

  const esp_partition_t *target = esp_partition_find_first(
    ESP_PARTITION_TYPE_APP, targetSubtype, NULL);

  if(!target)
  {
    tft.setTextColor(COL_WARN, COL_BG);
    tft.drawString("Target partition missing", 10, 65, FONT_SMALL);
    delay(2000);
    file.close();
    return false;
  }

  if(target == esp_ota_get_running_partition())
  {
    tft.setTextColor(COL_WARN, COL_BG);
    tft.drawString("Cannot update running slot", 10, 65, FONT_SMALL);
    delay(2000);
    file.close();
    return false;
  }

  size_t fileSize = file.size();
  uint32_t appOffset = 0;
  size_t appSize = fileSize;
  bool merged = false;

  if(fileSize > 0x8002)
  {
    uint8_t magic[2];
    file.seek(0);
    file.read(magic, 2);
    bool imageAtZero = (magic[0] == 0xE9);
    file.seek(0x8000);
    file.read(magic, 2);
    bool tableAt8000 = (magic[0] == 0xAA && magic[1] == 0x50);

    if(imageAtZero && tableAt8000)
    {
      uint32_t off = findFirstAppOffset(file, fileSize);
      size_t sz = off ? computeAppImageSize(file, off, fileSize) : 0;

      if(off && sz && sz <= fileSize)
      {
        appOffset = off;
        appSize = sz;
        merged = true;
      }
    }
  }

  if(merged)
  {
    tft.setTextColor(COL_MUTED, COL_BG);
    tft.drawString("Merged image -> app only", 10, 45, FONT_SMALL);
  }

  esp_ota_handle_t handle = 0;
  if(esp_ota_begin(target, appSize, &handle) != ESP_OK)
  {
    tft.setTextColor(COL_WARN, COL_BG);
    tft.drawString("Update begin failed", 10, 65, FONT_SMALL);
    delay(2000);
    file.close();
    return false;
  }

  file.seek(appOffset);
  uint8_t buf[1024];
  size_t written = 0;
  int lastPercent = -1;

  while(written < appSize)
  {
    size_t toRead = file.read(buf, sizeof(buf));
    if(toRead == 0) break;
    if((written + toRead) > appSize) toRead = appSize - written;

    if(esp_ota_write(handle, buf, toRead) != ESP_OK)
    {
      tft.setTextColor(COL_WARN, COL_BG);
      tft.drawString("Write error", 10, 65, FONT_SMALL);
      delay(2000);
      esp_ota_abort(handle);
      file.close();
      return false;
    }
    written += toRead;
    int percent = (written * 100) / appSize;
    if(percent != lastPercent)
    {
      drawUpdateProgress(percent, "Flashing...");
      lastPercent = percent;
    }
  }
  file.close();

  if(written != appSize || esp_ota_end(handle) != ESP_OK)
  {
    tft.setTextColor(COL_WARN, COL_BG);
    tft.drawString("Update failed", 10, 65, FONT_SMALL);
    delay(2000);
    return false;
  }

  drawUpdateProgress(100, "Done!");
  delay(1000);
  return true;
}

// ---------------------------------------------------------------------------
// Network download
// ---------------------------------------------------------------------------

static String urlToFileName(const String &url)
{
  String u = url;
  int q = u.indexOf('?');
  if(q > 0) u = u.substring(0, q);
  int hash = u.indexOf('#');
  if(hash > 0) u = u.substring(0, hash);
  u.trim();
  int sl = u.lastIndexOf('/');
  String name = (sl >= 0) ? u.substring(sl + 1) : u;
  if(name.length() == 0) name = "download.bin";
  return name;
}

static void appendUrlLines(const String &text, String *urls, int maxCount, int &n)
{
  int start = 0;
  while(start < (int)text.length() && n < maxCount)
  {
    int nl = text.indexOf('\n', start);
    String line = (nl < 0) ? text.substring(start) : text.substring(start, nl);
    line.trim();
    start = (nl < 0) ? text.length() : nl + 1;
    if(line.length() == 0) continue;
    if(line.startsWith("#")) continue;
    if(line.startsWith("http://") || line.startsWith("https://"))
      urls[n++] = line;
  }
}

static bool fetchTextUrl(const String &url, String &out)
{
  HTTPClient http;
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.setTimeout(15000);
  if(!http.begin(url)) return false;
  int code = http.GET();
  if(code != HTTP_CODE_OK)
  {
    http.end();
    return false;
  }
  out = http.getString();
  http.end();
  return out.length() > 0;
}

static int loadUrlList(String *urls, int maxCount)
{
  int n = 0;

  // Remote update_url.txt takes priority
  if(WiFi.status() == WL_CONNECTED && REMOTE_UPDATE_URL_TXT[0])
  {
    String body;
    if(fetchTextUrl(REMOTE_UPDATE_URL_TXT, body))
      appendUrlLines(body, urls, maxCount, n);
  }

  // Local LittleFS copy
  File f = LittleFS.open("/update_url.txt", "r");
  if(f)
  {
    String body = f.readString();
    f.close();
    appendUrlLines(body, urls, maxCount, n);
  }

  // Compiled fallbacks last
  for(int i = 0; i < MAX_URLS && n < maxCount; i++)
  {
    if(DEFAULT_UPDATE_URLS[i] && DEFAULT_UPDATE_URLS[i][0])
      urls[n++] = DEFAULT_UPDATE_URLS[i];
  }
  return n;
}

static bool downloadToFile(const String &url, const String &fname)
{
  HTTPClient http;
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.setTimeout(20000);
  if(!http.begin(url)) return false;

  int code = http.GET();
  if(code != HTTP_CODE_OK)
  {
    http.end();
    return false;
  }

  int total = http.getSize();
  File out = LittleFS.open("/" + fname, FILE_WRITE);
  if(!out)
  {
    http.end();
    return false;
  }

  uint8_t buf[1024];
  int got = 0;
  int lastPercent = -1;
  uint32_t lastData = millis();
  WiFiClient *stream = http.getStreamPtr();

  tft.fillScreen(COL_BG);
  drawHeader("DOWNLOAD");
  tft.setTextColor(COL_TEXT, COL_BG);
  tft.drawString(fname, 10, 40, FONT_SMALL);

  while(http.connected() || stream->available())
  {
    size_t avail = stream->available();
    if(avail)
    {
      size_t r = stream->readBytes(buf, avail < sizeof(buf) ? avail : sizeof(buf));
      if(r == 0) break;
      out.write(buf, r);
      got += (int)r;
      lastData = millis();
      if(total > 0)
      {
        int percent = (int)((int64_t)got * 100 / total);
        if(percent != lastPercent)
        {
          drawUpdateProgress(percent, "Downloading...");
          lastPercent = percent;
        }
        if(got >= total) break;
      }
    }
    else
    {
      if((millis() - lastData) > 15000) break;
      delay(1);
    }
  }

  out.close();
  http.end();

  if(total > 0) return got == total;
  return got > 0;
}

static int downloadAllFromUrls()
{
  String urls[MAX_URLS];
  int urlCount = loadUrlList(urls, MAX_URLS);

  if(urlCount == 0)
  {
    tft.fillScreen(COL_BG);
    drawHeader("NETWORK");
    tft.setTextColor(COL_WARN, COL_BG);
    tft.drawString("No URLs configured", 10, 50, FONT_SMALL);
    tft.setTextColor(COL_MUTED, COL_BG);
    tft.drawString("Upload update_url.txt", 10, 80, FONT_SMALL);
    tft.drawString("or edit DEFAULT_UPDATE_URLS", 10, 98, FONT_SMALL);
    tft.drawString("Click to go back", 10, 140, FONT_SMALL);
    while(true)
    {
      uint32_t h = readButton();
      if(h > 0) break;
      delay(10);
    }
    return 0;
  }

  const char *pick[MAX_URLS];
  int pickCount = 0;
  for(int i = 0; i < urlCount; i++)
    pick[pickCount++] = urls[i].c_str();

  if(pickCount >= 2)
  {
    int sel = runChoice("Download", "Select URL:", pick, pickCount);
    if(sel < 0) return 0;
    String chosen = pick[sel];
    String fname = urlToFileName(chosen);
    if(downloadToFile(chosen, fname)) return 1;
    tft.fillScreen(COL_BG);
    drawHeader("DOWNLOAD");
    tft.setTextColor(COL_WARN, COL_BG);
    tft.drawString("Failed:", 10, 50, FONT_SMALL);
    tft.drawString(fname, 10, 70, FONT_SMALL);
    tft.setTextColor(COL_MUTED, COL_BG);
    tft.drawString("Click to go back", 10, 140, FONT_SMALL);
    while(true)
    {
      uint32_t h = readButton();
      if(h > 0) break;
      delay(10);
    }
    return 0;
  }

  String fname = urlToFileName(pick[0]);
  bool ok = downloadToFile(pick[0], fname);
  if(!ok)
  {
    tft.fillScreen(COL_BG);
    drawHeader("DOWNLOAD");
    tft.setTextColor(COL_WARN, COL_BG);
    tft.drawString("Failed:", 10, 50, FONT_SMALL);
    tft.drawString(fname, 10, 70, FONT_SMALL);
    tft.setTextColor(COL_MUTED, COL_BG);
    tft.drawString("Click to go back", 10, 140, FONT_SMALL);
    while(true)
    {
      uint32_t h = readButton();
      if(h > 0) break;
      delay(10);
    }
    return 0;
  }
  return 1;
}

// ---------------------------------------------------------------------------
// Firmware update flow: target -> source -> file -> flash
// ---------------------------------------------------------------------------

static void runFirmwareUpdate()
{
  const char *slotNames[] = {"App0", "App1"};
  int slotSel = runChoice("FIRMWARE UPDATE", "Select target slot:", slotNames, 2);
  if(slotSel < 0) return;

  const char *sourceNames[] = {"Local files", "Network"};
  int srcSel = runChoice("FIRMWARE UPDATE", "Select source:", sourceNames, 2);
  if(srcSel < 0) return;

  if(srcSel == 1)
  {
    int got = downloadAllFromUrls();
    if(got == 0)
    {
      tft.fillScreen(COL_BG);
      drawHeader("NETWORK");
      tft.setTextColor(COL_WARN, COL_BG);
      tft.drawString("No files downloaded", 10, 60, FONT_SMALL);
      delay(2000);
      return;
    }
  }

  String files[32];
  int fileCount = listFirmwareFiles(files, 32);
  if(fileCount == 0)
  {
    tft.fillScreen(COL_BG);
    drawHeader("FIRMWARE UPDATE");
    tft.setTextColor(COL_WARN, COL_BG);
    tft.drawString("No .bin files found", 10, 10, FONT_LARGE);
    tft.setTextColor(COL_MUTED, COL_BG);
    tft.drawString("Use Network source or", 10, 50, FONT_SMALL);
    tft.drawString("WiFi File Manager upload.", 10, 70, FONT_SMALL);
    delay(2500);
    return;
  }

  int fileSel = 0;
  drawFilePage(files, fileCount, fileSel, slotNames[slotSel]);

  while(true)
  {
    int8_t fd = readEncoder();
    if(fd > 0)
    {
      fileSel++;
      if(fileSel >= fileCount) fileSel = 0;
      drawFilePage(files, fileCount, fileSel, slotNames[slotSel]);
    }
    else if(fd < 0)
    {
      fileSel--;
      if(fileSel < 0) fileSel = fileCount - 1;
      drawFilePage(files, fileCount, fileSel, slotNames[slotSel]);
    }

    uint32_t h = readButton();
    if(h >= 300)
    {
      bool ok = flashFirmware(files[fileSel], slotSel);
      delay(500);
      if(ok) ESP.restart();
      drawFilePage(files, fileCount, fileSel, slotNames[slotSel]);
    }
    else if(h > 0)
    {
      break;
    }
    delay(10);
  }
}

// ---------------------------------------------------------------------------
// Erase
// ---------------------------------------------------------------------------

static const char *resetItems[] = {"Factory Reset", "App0", "App1", "LittleFS"};
#define RESET_ITEM_COUNT (sizeof(resetItems) / sizeof(resetItems[0]))
#define RESET_FACTORY_INDEX 0

static void drawResetMenu(const bool *checked, int selected)
{
  tft.fillScreen(COL_BG);
  drawHeader("ERASE");
  tft.setTextColor(COL_MUTED, COL_BG);
  tft.drawString("Click=Check  Hold=Erase", 10, 30, FONT_SMALL);

  for(int i = 0; i < (int)RESET_ITEM_COUNT; i++)
  {
    int y = 54 + i * 22;
    if(i == selected)
    {
      tft.fillRoundRect(5, y - 2, 310, 22, 4, COL_TEXT);
      tft.setTextColor(COL_BG, COL_TEXT);
    }
    else
    {
      tft.setTextColor(COL_TEXT, COL_BG);
    }
    char line[24];
    snprintf(line, sizeof(line), "[%c] %s", checked[i] ? 'X' : ' ', resetItems[i]);
    tft.drawString(line, 15, y, FONT_SMALL);
  }

  tft.setTextColor(COL_MUTED, COL_BG);
  tft.drawString("Hold with nothing = Back", 10, 155, FONT_SMALL);
}

static void drawResetProgress(const char *label, int percent, int overall)
{
  tft.fillRect(0, 42, 320, 120, COL_BG);
  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(COL_TEXT, COL_BG);
  tft.drawString(label, 10, 46, FONT_SMALL);

  tft.fillRoundRect(10, 70, 300, 22, 4, 0x4208);
  int w = (percent * 296) / 100;
  if(w > 0) tft.fillRoundRect(12, 72, w, 18, 3, COL_TEXT);

  char buf[16];
  snprintf(buf, sizeof(buf), "%d%%", percent);
  tft.setTextDatum(MC_DATUM);
  tft.setTextColor(COL_TEXT, COL_BG);
  tft.drawString(buf, 160, 81, FONT_SMALL);
  tft.setTextDatum(TL_DATUM);

  tft.setTextColor(COL_MUTED, COL_BG);
  tft.drawString("Overall", 10, 104, FONT_SMALL);
  tft.fillRoundRect(70, 104, 240, 12, 3, 0x4208);
  int ow = (overall * 236) / 100;
  if(ow > 0) tft.fillRoundRect(72, 106, ow, 8, 2, COL_OK);
}

static bool erasePartitionProgress(const esp_partition_t *part, const char *label,
                                   int overallStart, int overallEnd)
{
  const size_t chunk = 0x10000;
  for(size_t done = 0; done < part->size;)
  {
    size_t len = part->size - done;
    if(len > chunk) len = chunk;
    if(esp_partition_erase_range(part, done, len) != ESP_OK) return false;
    done += len;
    int pct = (int)((uint64_t)done * 100 / part->size);
    drawResetProgress(label, pct, overallStart + (overallEnd - overallStart) * pct / 100);
  }
  return true;
}

static void eraseMenu()
{
  bool checked[RESET_ITEM_COUNT] = {false, false, false, false};
  int selected = 0;
  drawResetMenu(checked, selected);

  while(true)
  {
    int8_t d = readEncoder();
    if(d > 0)
    {
      selected = (selected + 1) % (int)RESET_ITEM_COUNT;
      drawResetMenu(checked, selected);
    }
    else if(d < 0)
    {
      selected = (selected + (int)RESET_ITEM_COUNT - 1) % (int)RESET_ITEM_COUNT;
      drawResetMenu(checked, selected);
    }

    uint32_t h = readButton();
    if(h == 0) { delay(10); continue; }

    if(h < 300)
    {
      checked[selected] = !checked[selected];
      drawResetMenu(checked, selected);
      continue;
    }

    int totalUnits = 0;
    for(int i = 0; i < (int)RESET_ITEM_COUNT; i++)
      if(checked[i]) totalUnits += (i == RESET_FACTORY_INDEX) ? 2 : 1;
    if(totalUnits == 0) return;

    const esp_partition_t *nvsPart = NULL, *settingsPart = NULL;
    if(checked[RESET_FACTORY_INDEX])
    {
      nvsPart = esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
        ESP_PARTITION_SUBTYPE_DATA_NVS, "nvs");
      settingsPart = esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
        ESP_PARTITION_SUBTYPE_DATA_NVS, STORAGE_PARTITION);
    }

    const esp_partition_t *app0 = NULL, *app1 = NULL, *fs = NULL;
    if(checked[1]) app0 = esp_partition_find_first(ESP_PARTITION_TYPE_APP,
      static_cast<esp_partition_subtype_t>(ESP_PARTITION_SUBTYPE_APP_OTA_0), NULL);
    if(checked[2]) app1 = esp_partition_find_first(ESP_PARTITION_TYPE_APP,
      static_cast<esp_partition_subtype_t>(ESP_PARTITION_SUBTYPE_APP_OTA_1), NULL);
    if(checked[3]) fs = esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
      ESP_PARTITION_SUBTYPE_DATA_LITTLEFS, NULL);

    if(fs) LittleFS.end();
    if(checked[RESET_FACTORY_INDEX]) nvs_flash_deinit();

    tft.fillScreen(COL_BG);
    drawHeader("ERASE");

    int unit = 0;
    bool ok = true;
    for(int i = 0; i < (int)RESET_ITEM_COUNT && ok; i++)
    {
      if(!checked[i]) continue;
      int units = (i == RESET_FACTORY_INDEX) ? 2 : 1;
      int start = unit * 100 / totalUnits;
      int end = (unit + units) * 100 / totalUnits;

      if(i == RESET_FACTORY_INDEX)
      {
        int mid = start + (end - start) / 2;
        ok = nvsPart && erasePartitionProgress(nvsPart, resetItems[i], start, mid);
        if(ok) ok = settingsPart && erasePartitionProgress(settingsPart, resetItems[i], mid, end);
      }
      else if(i == 1) ok = app0 && erasePartitionProgress(app0, resetItems[i], start, end);
      else if(i == 2) ok = app1 && erasePartitionProgress(app1, resetItems[i], start, end);
      else if(i == 3) ok = fs && erasePartitionProgress(fs, resetItems[i], start, end);

      unit += units;
    }

    if(!ok)
    {
      tft.setTextColor(COL_WARN, COL_BG);
      tft.drawString("Erase failed", 10, 150, FONT_SMALL);
      delay(2500);
      return;
    }

    drawResetProgress("Done", 100, 100);
    delay(1000);
    ESP.restart();
  }
}

// ---------------------------------------------------------------------------
// WiFi Setting: scan (4/page) + encoder keypad + connect/save
// ---------------------------------------------------------------------------

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

static void drawWifiSetting(int *nets, int netCount, int selected,
                            const String &ssid, const String &password,
                            bool kbOpen)
{
  tft.fillScreen(COL_BG);
  drawHeader("WIFI SETTING");

  int listBottom;
  if(kbOpen)
  {
    tft.setTextColor(COL_TEXT, COL_BG);
    tft.drawString("SSID: " + ssid, 10, 32, FONT_SMALL);
    tft.setTextColor(COL_MUTED, COL_BG);
    tft.drawString("Password:", 10, 50, FONT_SMALL);
    tft.setTextColor(COL_TEXT, COL_BG);
    String shown = password;
    if(shown.length() > 24) shown = shown.substring(shown.length() - 24);
    tft.drawString(shown + "_", 90, 50, FONT_SMALL);
    tft.setTextColor(COL_MUTED, COL_BG);
    tft.drawString("Click=Type Hold=OK", 10, 68, FONT_SMALL);

    listBottom = 84;
    kbBuild(kbMode, listBottom);

    for(int i = 0; i < kbKeyCount; i++)
    {
      const KbKey &k = kbKeys[i];
      bool sel = (i == kbCursor);
      uint16_t bg = sel ? COL_KEYSEL : COL_KEY;
      uint16_t fg = sel ? COL_BG : COL_TEXT;
      tft.fillRoundRect(k.x, k.y, k.w, k.h, 3, bg);
      tft.setTextColor(fg, bg);
      tft.setTextDatum(MC_DATUM);
      int cx = k.x + k.w / 2;
      int cy = k.y + k.h / 2;
      if(k.type == KB_MODE)
        tft.drawString(KB_MODE_NAMES[(int)k.ch], cx, cy, FONT_SMALL);
      else if(k.type == KB_BSP)
        tft.drawString("BSP", cx, cy, FONT_SMALL);
      else if(k.type == KB_CANCEL)
        tft.drawString("X", cx, cy, FONT_SMALL);
      else if(k.type == KB_SPACE)
        tft.drawString("SPACE", cx, cy, FONT_SMALL);
      else
      {
        char s[2] = {k.ch, 0};
        tft.drawString(s, cx, cy, FONT_SMALL);
      }
    }
    tft.setTextDatum(TL_DATUM);
  }
  else
  {
    tft.setTextColor(COL_MUTED, COL_BG);
    if(netCount == 0)
      tft.drawString("No networks found", 10, 36, FONT_SMALL);
    else
      tft.drawString("Hold=Connect Click=Rescan", 10, 36, FONT_SMALL);

    int page = selected / WIFI_PAGE_SIZE;
    int start = page * WIFI_PAGE_SIZE;
    int end = start + WIFI_PAGE_SIZE;
    if(end > netCount) end = netCount;

    for(int i = start; i < end; i++)
    {
      int row = i - start;
      int y = 56 + row * 24;
      if(i == selected)
      {
        tft.fillRoundRect(5, y - 2, 310, 22, 4, COL_TEXT);
        tft.setTextColor(COL_BG, COL_TEXT);
      }
      else
      {
        tft.setTextColor(COL_TEXT, COL_BG);
      }
      tft.drawString(WiFi.SSID(i), 15, y, FONT_SMALL);
    }

    int pages = (netCount + WIFI_PAGE_SIZE - 1) / WIFI_PAGE_SIZE;
    if(pages < 1) pages = 1;
    char pageBuf[20];
    snprintf(pageBuf, sizeof(pageBuf), "Page %d/%d", page + 1, pages);
    tft.setTextColor(COL_MUTED, COL_BG);
    tft.drawString(pageBuf, 10, 155, FONT_SMALL);
    listBottom = 150;
  }

  (void)listBottom;
  (void)nets;
}

static int kbHandleEncoder(int8_t d)
{
  if(kbKeyCount == 0) return 0;
  if(d > 0)
  {
    kbCursor++;
    if(kbCursor >= kbKeyCount) kbCursor = 0;
    return 1;
  }
  if(d < 0)
  {
    kbCursor--;
    if(kbCursor < 0) kbCursor = kbKeyCount - 1;
    return 1;
  }
  return 0;
}

static void runWifiSetting()
{
  tft.fillScreen(COL_BG);
  drawHeader("WIFI SETTING");
  tft.setTextColor(COL_MUTED, COL_BG);
  tft.drawString("Scanning...", 10, 50, FONT_SMALL);
  drawNetIcon();

  WiFi.mode(WIFI_STA);
  applyTxPower();
  int netCount = WiFi.scanNetworks();
  if(netCount < 0) netCount = 0;
  if(netCount > MAX_SCAN_NETWORKS) netCount = MAX_SCAN_NETWORKS;

  int selected = 0;
  bool kbOpen = false;
  String activeSsid;
  String password = "";
  int dummyNets[MAX_SCAN_NETWORKS];

  if(netCount == 0) selected = 0;

  auto redraw = [&]() {
    drawWifiSetting(dummyNets, netCount, selected, activeSsid, password, kbOpen);
  };

  redraw();

  while(true)
  {
    if(!kbOpen)
    {
      int8_t d = readEncoder();
      if(netCount > 0)
      {
        if(d > 0)
        {
          selected++;
          if(selected >= netCount) selected = 0;
          redraw();
        }
        else if(d < 0)
        {
          selected--;
          if(selected < 0) selected = netCount - 1;
          redraw();
        }
      }

      uint32_t h = readButton();
      if(h >= 300)
      {
        if(netCount == 0) return;
        activeSsid = WiFi.SSID(selected);
        wifi_auth_mode_t auth = WiFi.encryptionType(selected);
        if(auth == WIFI_AUTH_OPEN)
        {
          tft.fillScreen(COL_BG);
          drawHeader("WIFI SETTING");
          tft.setTextColor(COL_TEXT, COL_BG);
          tft.drawString("Connecting: " + activeSsid, 10, 50, FONT_SMALL);
          bool ok = wifiConnectBlocking(activeSsid, "", WIFI_CONNECT_TIMEOUT);
          if(ok)
          {
            saveWifiCredentials(activeSsid, "");
            tft.setTextColor(COL_OK, COL_BG);
            tft.drawString("Connected!", 10, 80, FONT_SMALL);
            delay(1200);
            return;
          }
          tft.setTextColor(COL_WARN, COL_BG);
          tft.drawString("Failed", 10, 80, FONT_SMALL);
          delay(1500);
          redraw();
        }
        else
        {
          password = "";
          kbOpen = true;
          kbMode = 0;
          kbCursor = 0;
          redraw();
        }
      }
      else if(h > 0)
      {
        WiFi.scanDelete();
        return;
      }
      delay(10);
    }
    else
    {
      int8_t d = readEncoder();
      if(kbHandleEncoder(d)) redraw();

      uint32_t h = readButton();
      if(h >= 300)
      {
        tft.fillScreen(COL_BG);
        drawHeader("WIFI SETTING");
        tft.setTextColor(COL_TEXT, COL_BG);
        tft.drawString("Connecting: " + activeSsid, 10, 50, FONT_SMALL);
        bool ok = wifiConnectBlocking(activeSsid, password, WIFI_CONNECT_TIMEOUT);
        if(ok)
        {
          saveWifiCredentials(activeSsid, password);
          tft.setTextColor(COL_OK, COL_BG);
          tft.drawString("Connected!", 10, 80, FONT_SMALL);
          delay(1200);
          WiFi.scanDelete();
          return;
        }
        tft.setTextColor(COL_WARN, COL_BG);
        tft.drawString("Failed - check password", 10, 80, FONT_SMALL);
        delay(1500);
        redraw();
      }
      else if(h > 0)
      {
        const KbKey &k = kbKeys[kbCursor];
        if(k.type == KB_CHAR)
        {
          if(password.length() < 63) password += k.ch;
          redraw();
        }
        else if(k.type == KB_MODE)
        {
          kbMode = (int)k.ch;
          kbCursor = 4;
          redraw();
        }
        else if(k.type == KB_BSP)
        {
          if(password.length() > 0) password.remove(password.length() - 1);
          redraw();
        }
        else if(k.type == KB_SPACE)
        {
          if(password.length() < 63) password += ' ';
          redraw();
        }
        else if(k.type == KB_CANCEL)
        {
          kbOpen = false;
          redraw();
        }
      }
      delay(10);
    }
  }
}

// ---------------------------------------------------------------------------
// WiFi file manager (AP mode web server)
// ---------------------------------------------------------------------------

static void handleRoot()
{
  String html =
    "<!DOCTYPE html><html><head><meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>ESP32 BOOTmgr</title>"
    "<style>body{font-family:sans-serif;max-width:480px;margin:0 auto;padding:16px;background:#fafafa}"
    "h1{font-size:18px} .card{background:#fff;border:1px solid #e0e0e0;border-radius:8px;padding:12px;margin:12px 0}"
    ".btn{padding:6px 12px;border:none;border-radius:6px;background:#1a73e8;color:#fff;font-size:14px;cursor:pointer}"
    ".del{background:#d93025} .row{display:flex;justify-content:space-between;align-items:center;padding:6px 0;border-bottom:1px solid #eee}"
    "#pbar{width:100%;height:20px;border:1px solid #ccc;border-radius:4px;overflow:hidden;display:none;margin-top:8px}"
    "#pfill{width:0%;height:100%;background:#1a73e8;transition:width .2s}"
    "#ptxt{font-size:13px;color:#555;margin-top:4px}</style></head><body>"
    "<h1>ESP32 BOOTmgr</h1>";

  html += "<div class='card'><b>Storage</b><div class='row'><span>"
        + String(LittleFS.usedBytes() / 1024) + " / " + String(LittleFS.totalBytes() / 1024) + " KB</span></div></div>";

  html += "<div class='card'><b>Upload .bin / update_url.txt</b>"
          "<input type='file' id='fi' accept='.bin,.txt' style='margin:8px 0'>"
          "<button class='btn' id='ubtn' onclick='go()'>Upload</button>"
          "<div id='pbar'><div id='pfill'></div></div><div id='ptxt'></div></div>";

  html += "<div class='card'><b>Files</b>";
  File root = LittleFS.open("/", "r");
  if(root && root.isDirectory())
  {
    File f = root.openNextFile();
    while(f)
    {
      String name = f.name();
      html += "<div class='row'><span>" + name + " (" + String(f.size() / 1024) + " KB)</span>"
              "<a class='btn del' href='/delete?name=" + name + "' onclick=\"return confirm('Delete " + name + "?')\">Del</a></div>";
      f = root.openNextFile();
    }
    root.close();
  }
  html += "</div>";

  html += "<script>"
          "var busy=false;"
          "function go(){if(busy)return;var f=document.getElementById('fi').files[0];if(!f)return;"
          "busy=true;document.getElementById('ubtn').disabled=true;"
          "var fd=new FormData();fd.append('file',f);var x=new XMLHttpRequest();"
          "document.getElementById('pbar').style.display='block';"
          "x.upload.onprogress=function(e){if(e.lengthComputable){"
          "var p=Math.round(e.loaded/e.total*100);"
          "document.getElementById('pfill').style.width=p+'%';"
          "document.getElementById('ptxt').textContent=p+'%';}};"
          "x.onload=function(){document.getElementById('ptxt').textContent='Done!';setTimeout(function(){location.reload()},800);};"
          "x.onerror=function(){document.getElementById('ptxt').textContent='Failed.';busy=false;document.getElementById('ubtn').disabled=false;};"
          "x.open('POST','/upload');x.send(fd);}</script>"
          "</body></html>";

  server.send(200, "text/html", html);
}

static File uploadFile;

static void handleUploadDone()
{
  server.send(200, "text/plain", "OK");
}

static void handleUpload()
{
  HTTPUpload& up = server.upload();
  if(up.status == UPLOAD_FILE_START)
  {
    tft.fillScreen(COL_BG);
    drawHeader("FILE MANAGER");
    tft.setTextDatum(TC_DATUM);
    tft.drawString("RECEIVING", 160, 40, FONT_LARGE);
    tft.setTextDatum(TL_DATUM);
    tft.drawString(up.filename.c_str(), 10, 70, FONT_SMALL);
    tft.drawRect(10, 100, 300, 20, COL_TEXT);
    uploadFile = LittleFS.open("/" + String(up.filename.c_str()), FILE_WRITE);
  }
  else if(up.status == UPLOAD_FILE_WRITE)
  {
    if(uploadFile) uploadFile.write(up.buf, up.currentSize);
  }
  else if(up.status == UPLOAD_FILE_END)
  {
    if(uploadFile) uploadFile.close();
  }
}

static void handleDelete()
{
  if(server.hasArg("name"))
  {
    String name = server.arg("name");
    if(!name.startsWith("/")) name = "/" + name;
    LittleFS.remove(name);
  }
  server.sendHeader("Location", "/");
  server.send(302, "text/plain", "");
}

static void runFileManager()
{
  bool staConnected = (WiFi.status() == WL_CONNECTED);

  if(staConnected)
  {
    apIP = WiFi.localIP().toString();
    apModeActive = false;
  }
  else
  {
    wifiStartAp();
  }

  server.on("/", HTTP_GET, handleRoot);
  server.on("/upload", HTTP_POST, handleUploadDone, handleUpload);
  server.on("/delete", HTTP_GET, handleDelete);
  server.begin();
  serverRunning = true;

  tft.fillScreen(COL_BG);
  drawHeader("FILE MANAGER");
  tft.setTextColor(COL_MUTED, COL_BG);
  if(staConnected)
  {
    tft.drawString("SSID: " + WiFi.SSID(), 10, 45, FONT_SMALL);
    tft.drawString("URL:  http://" + apIP, 10, 79, FONT_SMALL);
  }
  else
  {
    tft.drawString("SSID: ats-recovery", 10, 45, FONT_SMALL);
    tft.drawString("PASS: 12345678", 10, 62, FONT_SMALL);
    tft.drawString("URL:  http://" + apIP, 10, 79, FONT_SMALL);
  }
  tft.drawString("Hold button to exit", 10, 120, FONT_SMALL);

  while(true)
  {
    server.handleClient();
    if(readButton() >= 300) break;
    delay(1);
  }

  if(staConnected)
  {
    server.stop();
    serverRunning = false;
    apModeActive = false;
  }
  else
  {
    wifiStopApToSta();
  }
}

// ---------------------------------------------------------------------------
// About (4 pages)
// ---------------------------------------------------------------------------

#define ABOUT_PAGES 4

static void displayQRCode(esp_qrcode_handle_t qrcode)
{
  int size = esp_qrcode_get_size(qrcode);
  const int scale = 3;
  const int ox = 10, oy = 42;
  int qr = size * scale;

  tft.fillRect(ox - 4, oy - 4, qr + 8, qr + 8, COL_TEXT);
  for(int y = 0; y < size; y++)
    for(int x = 0; x < size; x++)
      if(esp_qrcode_get_module(qrcode, x, y))
        tft.fillRect(ox + x * scale, oy + y * scale, scale, scale, COL_BG);
}

static void drawAboutFooter(int page)
{
  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(COL_MUTED, COL_BG);
  char buf[24];
  snprintf(buf, sizeof(buf), "Page %d/%d", page + 1, ABOUT_PAGES);
  tft.drawString(buf, 8, 153, FONT_SMALL);
  tft.setTextDatum(TR_DATUM);
  tft.drawString("Rotate=Page Hold=Back", 312, 153, FONT_SMALL);
  tft.setTextDatum(TL_DATUM);
}

static void drawAboutPage(int page)
{
  tft.fillScreen(COL_BG);
  drawHeader("ESP32 BOOTmgr");

  switch(page)
  {
    case 0:
    {
      tft.setTextDatum(TR_DATUM);
      tft.setTextColor(COL_OK, COL_BG);
      tft.drawString("v" RECOVERY_VERSION, 294, 10, FONT_SMALL);
      tft.setTextDatum(TL_DATUM);
      esp_qrcode_config_t cfg = ESP_QRCODE_CONFIG_DEFAULT();
      cfg.display_func = displayQRCode;
      esp_qrcode_generate(&cfg, "https://github.com/GNBD/ats-mini");
      tft.setTextColor(COL_TEXT, COL_BG);
      tft.drawString("GitHub", 120, 48, FONT_SMALL);
      tft.setTextColor(COL_MUTED, COL_BG);
      tft.drawString("github.com/", 120, 68, FONT_SMALL);
      tft.drawString("GNBD/ats-mini", 120, 86, FONT_SMALL);
      tft.drawString("Scan for source", 120, 112, FONT_SMALL);
      tft.drawString("and releases.", 120, 130, FONT_SMALL);
      break;
    }
    case 1:
    {
      tft.setTextColor(COL_TEXT, COL_BG);
      tft.drawString("Libraries / Licenses", 8, 38, FONT_SMALL);
      tft.setTextColor(COL_MUTED, COL_BG);
      tft.drawString("LovyanGFX     MIT", 8, 56, FONT_SMALL);
      tft.drawString("ESP32 Core    LGPL-2.1", 8, 72, FONT_SMALL);
      tft.drawString("ESP-IDF       Apache-2.0", 8, 88, FONT_SMALL);
      tft.drawString("LittleFS      MIT", 8, 104, FONT_SMALL);
      tft.drawString("Upstream: esp32-si4732", 8, 126, FONT_SMALL);
      tft.drawString("Fork: GNBD/ats-mini", 8, 142, FONT_SMALL);
      break;
    }
    case 2:
    {
      tft.setTextColor(COL_TEXT, COL_BG);
      tft.drawString("Recovery Help", 8, 38, FONT_SMALL);
      tft.drawString("Boot App0/1", 8, 54, FONT_SMALL);
      tft.drawString("Firmware Upd", 8, 70, FONT_SMALL);
      tft.drawString("WiFi", 8, 86, FONT_SMALL);
      tft.drawString("Setting/Mgr", 8, 102, FONT_SMALL);
      tft.drawString("Erase", 8, 118, FONT_SMALL);
      tft.drawString("About", 8, 134, FONT_SMALL);
      tft.setTextColor(COL_MUTED, COL_BG);
      tft.drawString("boot a slot", 110, 54, FONT_SMALL);
      tft.drawString("Local/Network", 110, 70, FONT_SMALL);
      tft.drawString("scan+password", 110, 86, FONT_SMALL);
      tft.drawString("AP file upload", 110, 102, FONT_SMALL);
      tft.drawString("Factory/partitions", 110, 118, FONT_SMALL);
      tft.drawString("this screen", 110, 134, FONT_SMALL);
      break;
    }
    case 3:
    {
      tft.setTextColor(COL_TEXT, COL_BG);
      tft.drawString("Flash Help (esptool)", 8, 38, FONT_SMALL);
      tft.setTextColor(COL_MUTED, COL_BG);
      tft.drawString("0x0000    bootloader", 8, 56, FONT_SMALL);
      tft.drawString("0x8000    partitions", 8, 72, FONT_SMALL);
      tft.drawString("0x10000   app0", 8, 88, FONT_SMALL);
      tft.drawString("0x210000  app1", 8, 104, FONT_SMALL);
      tft.drawString("0x610000  recovery", 8, 120, FONT_SMALL);
      tft.drawString("0x810000  littlefs", 8, 136, FONT_SMALL);
      tft.drawString("write-flash 0x0 <image>", 8, 152, FONT_SMALL);
      break;
    }
  }

  drawAboutFooter(page);
}

static void runAbout()
{
  int page = 0;
  drawAboutPage(page);

  while(true)
  {
    int8_t d = readEncoder();
    if(d > 0)
    {
      page = (page + 1) % ABOUT_PAGES;
      drawAboutPage(page);
    }
    else if(d < 0)
    {
      page = (page + ABOUT_PAGES - 1) % ABOUT_PAGES;
      drawAboutPage(page);
    }

    uint32_t h = readButton();
    if(h >= 300) return;
    if(h > 0)
    {
      page = (page + 1) % ABOUT_PAGES;
      drawAboutPage(page);
    }
    delay(10);
  }
}

// ---------------------------------------------------------------------------
// WiFi submenu
// ---------------------------------------------------------------------------

static void runWifiMenu()
{
  int selected = 0;
  drawWifiMenu(selected);

  while(true)
  {
    int8_t dir = readEncoder();
    if(dir > 0)
    {
      selected = (selected + 1) % WIFI_MENU_COUNT;
      drawWifiMenu(selected);
    }
    else if(dir < 0)
    {
      selected = (selected + WIFI_MENU_COUNT - 1) % WIFI_MENU_COUNT;
      drawWifiMenu(selected);
    }

    uint32_t held = readButton();
    if(held >= 300)
    {
      if(selected == 0) runWifiSetting();
      else runFileManager();
      drawWifiMenu(selected);
    }
    else if(held > 0)
    {
      return;
    }
    delay(10);
  }
}

// ---------------------------------------------------------------------------
// Main menu
// ---------------------------------------------------------------------------

static void runRecoveryMenu()
{
  int selected = 0;
  drawMenu(selected);

  while(true)
  {
    int8_t dir = readEncoder();
    if(dir > 0)
    {
      selected = (selected + 1) % MENU_COUNT;
      drawMenu(selected);
    }
    else if(dir < 0)
    {
      selected = (selected + MENU_COUNT - 1) % MENU_COUNT;
      drawMenu(selected);
    }

    uint32_t held = readButton();
    if(held == 0 || held < 300) { delay(10); continue; }

    switch(selected)
    {
      case 0:
        bootToApp0();
        break;

      case 1:
        bootToApp1();
        break;

      case 2:
        runFirmwareUpdate();
        break;

      case 3:
        runWifiMenu();
        break;

      case 4:
        eraseMenu();
        break;

      case 5:
        runAbout();
        break;
    }

    drawMenu(selected);
  }
}

// ---------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------

void setup()
{
  Serial.begin(115200);
  delay(300);
  Serial.printf("\n=== RECOVERY BOOT (reset reason %d) ===\n", (int)esp_reset_reason());

  const esp_partition_t *bootPart = esp_ota_get_boot_partition();
  gBootIsApp1 = bootPart && bootPart->subtype == ESP_PARTITION_SUBTYPE_APP_OTA_1;
  gBootSlot = gBootIsApp1 ? "App1" : "App0";
  Serial.printf("active boot slot: %s\n", gBootSlot);

  esp_ota_mark_app_valid_cancel_rollback();

  const esp_partition_t *recovery = esp_partition_find_first(
    ESP_PARTITION_TYPE_APP,
    static_cast<esp_partition_subtype_t>(ESP_PARTITION_SUBTYPE_APP_OTA_2), NULL);
  if(recovery) esp_ota_set_boot_partition(recovery);

  pinMode(ENCODER_PIN_A, INPUT_PULLUP);
  pinMode(ENCODER_PIN_B, INPUT_PULLUP);
  pinMode(ENCODER_PUSH_BUTTON, INPUT_PULLUP);

  ledcAttach(PIN_LCD_BL, 16000, 8);
  ledcWrite(PIN_LCD_BL, 0);

  tft.init();
  tft.setRotation(3);
  tft.fillScreen(COL_BG);
  tft.setTextDatum(TC_DATUM);
  tft.setTextColor(COL_TEXT, COL_BG);
  tft.drawString("ESP32 BOOTmgr", 160, 50, FONT_LARGE);
  tft.setTextColor(COL_WARN, COL_BG);
  tft.drawString("Hold encoder for Recovery", 160, 80, FONT_SMALL);
  tft.setTextColor(COL_MUTED, COL_BG);
  {
    char slotLine[32];
    snprintf(slotLine, sizeof(slotLine), "Current: %s", gBootSlot);
    tft.drawString(slotLine, 160, 105, FONT_SMALL);
  }
  ledcWrite(PIN_LCD_BL, 255);

  Serial.println("step: 1s encoder check");
  bool encoderPressed = false;
  unsigned long start = millis();
  while(millis() - start < 1000)
  {
    if(digitalRead(ENCODER_PUSH_BUTTON) == LOW)
    {
      encoderPressed = true;
      break;
    }
    delay(10);
  }

  if(!encoderPressed)
  {
    Serial.printf("step: no encoder -> boot %s\n", gBootSlot);
    delay(300);
    if(gBootIsApp1) bootToApp1();
    else bootToApp0();
  }

  Serial.println("step: encoder held -> recovery menu");
  while(digitalRead(ENCODER_PUSH_BUTTON) == LOW) delay(50);
  delay(100);

  if(!LittleFS.begin(false, "/littlefs", 10, "littlefs"))
  {
    LittleFS.format();
    LittleFS.begin(false, "/littlefs", 10, "littlefs");
  }

  WiFi.mode(WIFI_MODE_NULL);
  WiFi.onEvent([](WiFiEvent_t e){ applyTxPower(); }, ARDUINO_EVENT_WIFI_STA_START);
  WiFi.onEvent([](WiFiEvent_t e){ applyTxPower(); }, ARDUINO_EVENT_WIFI_AP_START);

  String ssid, pass;
  WiFi.mode(WIFI_STA);
  applyTxPower();
  WiFi.setSleep(false);
  if(loadWifiCredentials(ssid, pass))
  {
    Serial.printf("step: STA connect to %s\n", ssid.c_str());
    WiFi.begin(ssid.c_str(), pass.c_str());
    uint32_t t0 = millis();
    while(WiFi.status() != WL_CONNECTED && (millis() - t0) < BOOT_CONNECT_TIMEOUT)
      delay(50);
    if(WiFi.status() == WL_CONNECTED)
      Serial.printf("step: STA ok %s\n", WiFi.localIP().toString().c_str());
    else
    {
      Serial.println("step: STA failed (ignored)");
      WiFi.disconnect(false);
    }
  }
  else
  {
    Serial.println("step: no saved WiFi");
  }

  runRecoveryMenu();
}

void loop() {}
