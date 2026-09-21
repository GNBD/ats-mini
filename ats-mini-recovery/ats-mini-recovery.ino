// ATS Mini standalone recovery firmware (ota_2)
// Runs from its own partition so BOTH app0 and app1 can be updated safely.
//
// Boot sequence:
//   1. Power ON -> recovery always boots first
//   2. Wait 1 second for encoder press
//   3. No encoder -> auto boot to app0
//   4. Encoder held -> recovery menu (app0/app1 boot, firmware update, etc.)
//   5. Recovery always sets itself as next boot target before jumping to app
#include <LovyanGFX.hpp>
#include <WiFi.h>
#include <WebServer.h>
#include <LittleFS.h>
#include <esp_wifi.h>
#include <esp_partition.h>
#include <esp_ota_ops.h>
#include <nvs_flash.h>
#include "Display.h"
#include "Rotary.h"

#define PIN_LCD_BL          38
#define ENCODER_PIN_A        2
#define ENCODER_PIN_B        1
#define ENCODER_PUSH_BUTTON 21
#define STORAGE_PARTITION    "settings"

static constexpr const lgfx::IFont* FONT_LARGE = &lgfx::fonts::Font4;
static constexpr const lgfx::IFont* FONT_SMALL = &lgfx::fonts::Font2;

#define COL_BG     0x0000
#define COL_TEXT   0xFFFF
#define COL_MUTED  0x8410
#define COL_WARN   0xF800
#define COL_OK     0x07E0

LGFX tft;
WebServer server(80);
Rotary encoder(ENCODER_PIN_A, ENCODER_PIN_B, true);

static String apIP;

static const char *menu[] = {
  "Boot App0",
  "Boot App1",
  "Firmware Update",
  "Factory Reset",
  "WiFi File Manager"
};
#define MENU_COUNT (sizeof(menu) / sizeof(menu[0]))

// ---------------------------------------------------------------------------
// Input helpers
// ---------------------------------------------------------------------------

// Returns -1 / 0 / +1 for CCW / none / CW.
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

// Blocking button read. Returns hold duration in ms, or 0 if not pressed.
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
// Boot helpers
// ---------------------------------------------------------------------------

// Boot to specified app partition.
// The main firmware will set recovery as the next boot target in its own setup().
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
  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(COL_TEXT, COL_BG);
  tft.drawString("ATS-MINI Radio", 10, 6, FONT_LARGE);
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

static void drawUpdateProgress(int percent, const char *status)
{
  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(COL_TEXT, COL_BG);
  tft.drawString(status, 10, 65, FONT_SMALL);

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
// Firmware flashing
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
    {
      names[count++] = name;
    }
    file = root.openNextFile();
  }
  root.close();
  return count;
}

static void drawFileList(String *files, int count, int selected, int targetSlot)
{
  tft.fillScreen(COL_BG);
  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(COL_TEXT, COL_BG);
  tft.drawString("FIRMWARE UPDATE", 10, 10, FONT_LARGE);
  tft.setTextColor(COL_MUTED, COL_BG);
  String target = (targetSlot == 0) ? "App0" : "App1";
  tft.drawString("Target: " + target + "  Select .bin:", 10, 35, FONT_SMALL);

  for(int i = 0; i < count; i++)
  {
    int y = 58 + i * 24;
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

  tft.setTextColor(COL_MUTED, COL_BG);
  tft.drawString("Click=Back Hold=Flash", 10, 155, FONT_SMALL);
}

// Returns the flash offset of the first application partition in a merged
// image's partition table, or 0 when no valid table is found.
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

// Size of an ESP application image starting at `appOffset` (0 on failure).
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

  pos += 1;                              // trailing checksum byte
  if(pos % 16) pos += 16 - (pos % 16);   // pad to 16 bytes
  if(hdr[23] == 1) pos += 32;            // appended SHA-256

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

  // A merged image starts with an ESP image at 0x0 and a partition table at
  // 0x8000. Extract the application image so it can be written to an OTA slot.
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
// Firmware update submenu: select target slot, then select file
// ---------------------------------------------------------------------------

static void runFirmwareUpdate()
{
  // Step 1: select target slot (app0 or app1)
  int slotSel = 0;
  const char *slotNames[] = {"App0", "App1"};

  while(true)
  {
    tft.fillScreen(COL_BG);
    tft.setTextDatum(TL_DATUM);
    tft.setTextColor(COL_TEXT, COL_BG);
    tft.drawString("FIRMWARE UPDATE", 10, 10, FONT_LARGE);
    tft.setTextColor(COL_MUTED, COL_BG);
    tft.drawString("Select target slot:", 10, 35, FONT_SMALL);

    for(int i = 0; i < 2; i++)
    {
      int y = 58 + i * 24;
      if(i == slotSel)
      {
        tft.fillRoundRect(5, y - 2, 310, 22, 4, COL_TEXT);
        tft.setTextColor(COL_BG, COL_TEXT);
      }
      else
      {
        tft.setTextColor(COL_TEXT, COL_BG);
      }
      tft.drawString(slotNames[i], 15, y, FONT_SMALL);
    }
    tft.setTextColor(COL_MUTED, COL_BG);
    tft.drawString("Click=Back Hold=Select", 10, 155, FONT_SMALL);

    while(true)
    {
      int8_t d = readEncoder();
      if(d > 0) { slotSel = (slotSel + 1) % 2; break; }
      if(d < 0) { slotSel = (slotSel + 1) % 2; break; }

      uint32_t h = readButton();
      if(h >= 300) goto select_slot_done;
      if(h > 0) return; // short press = back
      delay(10);
    }
  }

select_slot_done:
  // Step 2: select .bin file
  {
    String files[16];
    int fileCount = listFirmwareFiles(files, 16);
    if(fileCount == 0)
    {
      tft.fillScreen(COL_BG);
      tft.setTextDatum(TL_DATUM);
      tft.setTextColor(COL_WARN, COL_BG);
      tft.drawString("No .bin files found", 10, 10, FONT_LARGE);
      tft.setTextColor(COL_MUTED, COL_BG);
      tft.drawString("Upload via WiFi File", 10, 50, FONT_SMALL);
      tft.drawString("Manager first.", 10, 70, FONT_SMALL);
      delay(2500);
      return;
    }

    int fileSel = 0;
    drawFileList(files, fileCount, fileSel, slotSel);

    while(true)
    {
      int8_t fd = readEncoder();
      if(fd > 0)
      {
        fileSel = (fileSel + 1) % fileCount;
        drawFileList(files, fileCount, fileSel, slotSel);
      }
      else if(fd < 0)
      {
        fileSel = (fileSel + fileCount - 1) % fileCount;
        drawFileList(files, fileCount, fileSel, slotSel);
      }

      uint32_t h = readButton();
      if(h >= 300)
      {
        flashFirmware(files[fileSel], slotSel);
        delay(500);
        ESP.restart();
      }
      else if(h > 0)
      {
        break;
      }
      delay(10);
    }
  }
}

// ---------------------------------------------------------------------------
// Factory reset
// ---------------------------------------------------------------------------

static void factoryReset()
{
  tft.fillScreen(COL_BG);
  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(COL_TEXT, COL_BG);
  tft.drawString("FACTORY RESET", 10, 10, FONT_LARGE);
  tft.setTextColor(COL_WARN, COL_BG);
  tft.drawString("Erasing all settings...", 10, 50, FONT_SMALL);

  nvs_flash_erase();
  nvs_flash_init();
  nvs_flash_erase_partition(STORAGE_PARTITION);
  nvs_flash_init_partition(STORAGE_PARTITION);
  LittleFS.format();

  tft.setTextColor(COL_TEXT, COL_BG);
  tft.drawString("Done. Rebooting...", 10, 80, FONT_SMALL);
  delay(1500);
  ESP.restart();
}

// ---------------------------------------------------------------------------
// WiFi file manager
// ---------------------------------------------------------------------------

static void handleRoot()
{
  String html =
    "<!DOCTYPE html><html><head><meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>ATS-MINI Radio</title>"
    "<style>body{font-family:sans-serif;max-width:480px;margin:0 auto;padding:16px;background:#fafafa}"
    "h1{font-size:18px} .card{background:#fff;border:1px solid #e0e0e0;border-radius:8px;padding:12px;margin:12px 0}"
    ".btn{padding:6px 12px;border:none;border-radius:6px;background:#1a73e8;color:#fff;font-size:14px;cursor:pointer}"
    ".del{background:#d93025} .row{display:flex;justify-content:space-between;align-items:center;padding:6px 0;border-bottom:1px solid #eee}"
    "#pbar{width:100%;height:20px;border:1px solid #ccc;border-radius:4px;overflow:hidden;display:none;margin-top:8px}"
    "#pfill{width:0%;height:100%;background:#1a73e8;transition:width .2s}"
    "#ptxt{font-size:13px;color:#555;margin-top:4px}</style></head><body>"
    "<h1>ATS-MINI Radio</h1>";

  html += "<div class='card'><b>Storage</b><div class='row'><span>"
        + String(LittleFS.usedBytes() / 1024) + " / " + String(LittleFS.totalBytes() / 1024) + " KB</span></div></div>";

  html += "<div class='card'><b>Upload .bin</b>"
          "<input type='file' id='fi' accept='.bin' style='margin:8px 0'>"
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
    tft.setTextDatum(TC_DATUM);
    tft.setTextColor(COL_TEXT, COL_BG);
    tft.drawString("RECEIVING", 160, 30, FONT_LARGE);
    tft.setTextDatum(TL_DATUM);
    tft.drawString(up.filename.c_str(), 10, 60, FONT_SMALL);
    tft.drawRect(10, 90, 300, 20, COL_TEXT);
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
  tft.fillScreen(COL_BG);
  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(COL_TEXT, COL_BG);
  tft.drawString("FILE MANAGER", 10, 10, FONT_LARGE);
  tft.setTextColor(COL_MUTED, COL_BG);
  tft.drawString("SSID: ats-recovery", 10, 45, FONT_SMALL);
  tft.drawString("PASS: 12345678", 10, 62, FONT_SMALL);
  tft.drawString("URL:  http://" + apIP, 10, 79, FONT_SMALL);
  tft.drawString("Hold button to exit", 10, 120, FONT_SMALL);

  while(true)
  {
    server.handleClient();
    if(readButton() >= 300) break;
    delay(1);
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
      case 0:  // Boot App0
        bootToApp0();
        break;

      case 1:  // Boot App1
        bootToApp1();
        break;

      case 2:  // Firmware Update
        runFirmwareUpdate();
        break;

      case 3:  // Factory Reset
        factoryReset();
        break;

      case 4:  // WiFi File Manager
        runFileManager();
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

  esp_ota_mark_app_valid_cancel_rollback();

  // Always set recovery as next boot target so power-cycle returns here
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
  tft.drawString("ATS-MINI Radio", 160, 50, FONT_LARGE);
  tft.setTextColor(COL_WARN, COL_BG);
  tft.drawString("Hold encoder for Recovery", 160, 80, FONT_SMALL);
  ledcWrite(PIN_LCD_BL, 255);

  // 1 second wait: encoder held -> recovery menu, otherwise auto boot app0
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
    Serial.println("step: no encoder -> boot app0");
    delay(300);
    bootToApp0();
  }

  // Encoder held -> recovery menu
  Serial.println("step: encoder held -> recovery menu");
  while(digitalRead(ENCODER_PUSH_BUTTON) == LOW) delay(50);
  delay(100);

  if(!LittleFS.begin(false, "/littlefs", 10, "littlefs"))
  {
    LittleFS.format();
    LittleFS.begin(false, "/littlefs", 10, "littlefs");
  }

  WiFi.mode(WIFI_MODE_NULL);
  WiFi.onEvent([](WiFiEvent_t e){ WiFi.setTxPower(WIFI_POWER_17dBm); }, ARDUINO_EVENT_WIFI_AP_START);
  WiFi.mode(WIFI_AP);
  WiFi.softAP("ats-recovery", "12345678");
  esp_wifi_set_ps(WIFI_PS_NONE);
  WiFi.setSleep(false);
  apIP = WiFi.softAPIP().toString();

  server.on("/", HTTP_GET, handleRoot);
  server.on("/upload", HTTP_POST, handleUploadDone, handleUpload);
  server.on("/delete", HTTP_GET, handleDelete);
  server.begin();

  runRecoveryMenu();
}

void loop() {}
