#include "Common.h"
#include "Storage.h"
#include "Themes.h"
#include "Utils.h"
#include "Recovery.h"
#include "Rotary.h"
#include <LittleFS.h>
#include <Update.h>
#include <WiFi.h>
#include <WebServer.h>
#include <esp_wifi.h>
#include <esp_partition.h>
#include <esp_ota_ops.h>

extern LGFX tft;
extern LGFX_Sprite spr;
extern Rotary encoder;

static WebServer server(80);

static int8_t readEncoder();

static const char *recoveryMenu[] = {
  "Firmware Update",
  "Factory Reset",
  "WiFi File Manager",
  "Reboot"
};
#define RECOVERY_MENU_COUNT (sizeof(recoveryMenu) / sizeof(recoveryMenu[0]))

static void drawRecoveryMenu(uint8_t selected)
{
  tft.fillScreen(TH.bg);
  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(TH.text, TH.bg);
  tft.drawString("RECOVERY MODE", 10, 6, FONT_LARGE);
  tft.setTextColor(TH.text_muted, TH.bg);
  tft.drawString("v" + String(getVersion(true)), 10, 30, FONT_SMALL);

  for(uint8_t i = 0; i < RECOVERY_MENU_COUNT; i++)
  {
    int y = 48 + i * 18;
    if(i == selected)
    {
      tft.fillRoundRect(5, y - 1, 310, 18, 4, TH.text);
      tft.setTextColor(TH.bg, TH.text);
    }
    else
    {
      tft.setTextColor(TH.text, TH.bg);
    }
    tft.drawString(recoveryMenu[i], 15, y, FONT_SMALL);
  }

  tft.setTextColor(TH.text_muted, TH.bg);
  tft.drawString("Click=Back Hold=Select", 10, 156, FONT_SMALL);
}

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
      names[count] = name;
      count++;
    }
    file = root.openNextFile();
  }
  root.close();
  return count;
}

static void drawFileList(String *files, int count, int selected)
{
  tft.fillScreen(TH.bg);
  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(TH.text, TH.bg);
  tft.drawString("FIRMWARE UPDATE", 10, 10, FONT_LARGE);
  tft.setTextColor(TH.text_muted, TH.bg);
  tft.drawString("Select .bin file:", 10, 35, FONT_SMALL);

  for(int i = 0; i < count; i++)
  {
    int y = 58 + i * 24;
    if(i == selected)
    {
      tft.fillRoundRect(5, y - 2, 310, 22, 4, TH.text);
      tft.setTextColor(TH.bg, TH.text);
    }
    else
    {
      tft.setTextColor(TH.text, TH.bg);
    }
    tft.drawString(files[i], 15, y, FONT_SMALL);
  }

  tft.setTextColor(TH.text_muted, TH.bg);
  tft.drawString("Click=Back Hold=Flash", 10, 155, FONT_SMALL);
}

static void drawUpdateProgress(int percent, const char *status)
{
  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(TH.text, TH.bg);
  tft.drawString(status, 10, 65, FONT_SMALL);

  tft.fillRoundRect(10, 90, 300, 20, 4, 0x4208);
  int w = (percent * 296) / 100;
  if(w > 0) tft.fillRoundRect(12, 92, w, 16, 3, TH.text);

  char buf[16];
  sprintf(buf, "%d%%", percent);
  tft.setTextDatum(MC_DATUM);
  tft.drawString(buf, 160, 100, FONT_SMALL);
  tft.setTextDatum(TL_DATUM);
}

static bool flashFirmware(const String &filename, uint8_t slot)
{
  File file = LittleFS.open("/" + filename, "r");
  if(!file || file.size() == 0)
  {
    tft.setTextColor(TH.text_warn, TH.bg);
    tft.drawString("Failed to open file", 10, 65, FONT_SMALL);
    delay(2000);
    return false;
  }

  const esp_partition_t *target = esp_partition_find_first(
    ESP_PARTITION_TYPE_APP,
    static_cast<esp_partition_subtype_t>(ESP_PARTITION_SUBTYPE_APP_OTA_MIN + slot),
    NULL);

  if(!target)
  {
    tft.setTextColor(TH.text_warn, TH.bg);
    tft.drawString("Target partition missing", 10, 65, FONT_SMALL);
    delay(2000);
    file.close();
    return false;
  }

  if(target == esp_ota_get_running_partition())
  {
    tft.setTextColor(TH.text_warn, TH.bg);
    tft.drawString("Cannot update running slot", 10, 65, FONT_SMALL);
    delay(2000);
    file.close();
    return false;
  }

  size_t fileSize = file.size();
  esp_ota_handle_t handle = 0;
  if(esp_ota_begin(target, fileSize, &handle) != ESP_OK)
  {
    tft.setTextColor(TH.text_warn, TH.bg);
    tft.drawString("Update begin failed", 10, 65, FONT_SMALL);
    delay(2000);
    file.close();
    return false;
  }

  uint8_t buf[1024];
  size_t written = 0;
  int lastPercent = -1;

  while(file.available())
  {
    size_t toRead = file.read(buf, sizeof(buf));
    if(esp_ota_write(handle, buf, toRead) != ESP_OK)
    {
      tft.setTextColor(TH.text_warn, TH.bg);
      tft.drawString("Write error", 10, 65, FONT_SMALL);
      delay(2000);
      esp_ota_abort(handle);
      file.close();
      return false;
    }
    written += toRead;
    int percent = (written * 100) / fileSize;
    if(percent != lastPercent)
    {
      drawUpdateProgress(percent, "Flashing...");
      lastPercent = percent;
    }
  }

  file.close();

  if(esp_ota_end(handle) != ESP_OK)
  {
    tft.setTextColor(TH.text_warn, TH.bg);
    tft.drawString("Update failed", 10, 65, FONT_SMALL);
    delay(2000);
    return false;
  }

  drawUpdateProgress(100, "Done!");
  delay(1000);
  return true;
}

static void handleRoot()
{
  String html = "<!DOCTYPE html><html><head><meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>ATS Mini - File Manager</title>"
    "<style>body{font-family:sans-serif;max-width:480px;margin:0 auto;padding:16px;background:#fafafa}"
    "h1{font-size:18px;margin-bottom:12px}"
    ".card{background:#fff;border:1px solid #e0e0e0;border-radius:8px;padding:12px;margin-bottom:12px}"
    "table{width:100%;border-collapse:collapse}"
    "td{padding:4px 8px;border-bottom:1px solid #eee}"
    ".btn{display:inline-block;padding:8px 16px;border:none;border-radius:6px;"
    "background:#1a73e8;color:#fff;text-decoration:none;font-size:14px;cursor:pointer;margin:4px}"
    ".btn-del{background:#d32f2f}"
    "#progress{display:none;margin-top:12px}"
    "#pbar{width:100%;height:20px;border:1px solid #ccc;border-radius:4px;overflow:hidden}"
    "#pfill{width:0%;height:100%;background:#1a73e8;transition:width 0.2s}"
    "#ptxt{font-size:13px;color:#555;margin-top:4px}"
    "</style></head><body>"
    "<h1>ATS Mini - File Manager</h1>"
    "<div class='card'>"
    "<form id='uploadform'>"
    "<input type='file' id='fileinput' name='file' accept='.bin'>"
    "<input type='button' class='btn' value='Upload' onclick='doUpload()'>"
    "</form>"
    "<div id='progress'><div id='pbar'><div id='pfill'></div></div><div id='ptxt'></div></div>"
    "</div>"
    "<script>"
    "function doUpload(){"
    "var f=document.getElementById('fileinput').files[0];"
    "if(!f)return;"
    "var fd=new FormData();fd.append('file',f);"
    "var xhr=new XMLHttpRequest();"
    "document.getElementById('progress').style.display='block';"
    "xhr.upload.onprogress=function(e){"
    "if(e.lengthComputable){"
    "var pct=Math.round(e.loaded/e.total*100);"
    "document.getElementById('pfill').style.width=pct+'%';"
    "document.getElementById('ptxt').textContent=pct+'% ('+Math.round(e.loaded/1024)+'/'+Math.round(e.total/1024)+' KB)';"
    "}};"
    "xhr.onload=function(){document.getElementById('ptxt').textContent='Done!';location.reload();};"
    "xhr.onerror=function(){document.getElementById('ptxt').textContent='Error!';};"
    "xhr.open('POST','/upload');xhr.send(fd);"
    "}"
    "</script>"
    "<div class='card'><b>Storage:</b> " + String(LittleFS.usedBytes() / 1024) + " KB / " + String(LittleFS.totalBytes() / 1024) + " KB</div>"
    "<div class='card'><b>Files on LittleFS:</b><table>";

  File root = LittleFS.open("/", "r");
  if(root && root.isDirectory())
  {
    File file = root.openNextFile();
    while(file)
    {
      String name = file.name();
      size_t size = file.size();
      String sizeStr;
      if(size > 1024) sizeStr = String(size / 1024) + " KB";
      else sizeStr = String(size) + " B";

      html += "<tr><td>" + name + "</td><td>" + sizeStr + "</td>"
        "<td><a class='btn btn-del' href='/delete?file=" + name + "'>Delete</a></td></tr>";
      file = root.openNextFile();
    }
    root.close();
  }

  html += "</table></div></body></html>";
  server.send(200, "text/html", html);
}

static void handleUpload()
{
  HTTPUpload &upload = server.upload();
  static File uploadFile;
  static size_t uploadTotal = 0;
  static int lastDrawnPct = -1;

  if(upload.status == UPLOAD_FILE_START)
  {
    uploadTotal = 0;
    lastDrawnPct = -1;
    String filename = "/" + String(upload.filename.c_str());
    uploadFile = LittleFS.open(filename, FILE_WRITE);

    tft.fillScreen(TH.bg);
    tft.setTextDatum(TL_DATUM);
    tft.setTextColor(TH.text, TH.bg);
    tft.drawString("UPLOADING", 10, 10, FONT_LARGE);
    tft.setTextColor(TH.text_muted, TH.bg);
    tft.drawString(upload.filename.c_str(), 10, 35, FONT_SMALL);
  }
  else if(upload.status == UPLOAD_FILE_WRITE)
  {
    if(uploadFile) uploadFile.write(upload.buf, upload.currentSize);
    uploadTotal += upload.currentSize;

    int pct = (upload.totalSize > 0) ? (int)((uploadTotal * 100) / upload.totalSize) : 0;
    if(pct != lastDrawnPct)
    {
      tft.fillRoundRect(10, 70, 300, 20, 4, 0x4208);
      int w = (pct * 296) / 100;
      if(w > 0) tft.fillRoundRect(12, 72, w, 16, 3, TH.text);

      char buf[48];
      sprintf(buf, "%d KB / %d KB", uploadTotal / 1024, upload.totalSize / 1024);
      tft.setTextDatum(MC_DATUM);
      tft.drawString(buf, 160, 80, FONT_SMALL);
      tft.setTextDatum(TL_DATUM);
      lastDrawnPct = pct;
    }
  }
  else if(upload.status == UPLOAD_FILE_END)
  {
    if(uploadFile) uploadFile.close();
  }
}

static void handleUploadDone()
{
  server.send(200, "application/json", "{\"ok\":true}");
}

static void handleDelete()
{
  if(server.hasArg("file"))
  {
    String filename = "/" + server.arg("file");
    LittleFS.remove(filename);
  }
  server.sendHeader("Location", "/");
  server.send(303);
}

static void startWiFiFileManager()
{
  tft.fillScreen(TH.bg);
  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(TH.text, TH.bg);
  tft.drawString("WIFI FILE MANAGER", 10, 10, FONT_LARGE);
  tft.setTextColor(TH.text_muted, TH.bg);
  tft.drawString("Starting WiFi AP...", 10, 50, FONT_SMALL);

  WiFi.mode(WIFI_AP);
  WiFi.softAP("ats-recovery", "12345678");

  // Reduce TX power to ~16dBm (64 * 0.25dBm) to avoid voltage spikes
  esp_wifi_set_max_tx_power(64);

  IPAddress ip = WiFi.softAPIP();
  String ipStr = ip.toString();

  tft.setTextColor(TH.text, TH.bg);
  tft.drawString("SSID: ats-recovery", 10, 65, FONT_SMALL);
  tft.drawString("PASS: 12345678", 10, 80, FONT_SMALL);
  tft.drawString("URL:  http://" + ipStr, 10, 95, FONT_SMALL);

  // Show LittleFS usage
  size_t totalBytes = LittleFS.totalBytes();
  size_t usedBytes = LittleFS.usedBytes();
  char usageBuf[40];
  sprintf(usageBuf, "Storage: %d KB / %d KB", usedBytes / 1024, totalBytes / 1024);
  tft.drawString(usageBuf, 10, 115, FONT_SMALL);

  tft.setTextColor(TH.text_muted, TH.bg);
  tft.drawString("Click=Back Hold=Reboot", 10, 155, FONT_SMALL);

  server.on("/", HTTP_GET, handleRoot);
  server.on("/upload", HTTP_POST, handleUploadDone, handleUpload);
  server.on("/delete", HTTP_GET, handleDelete);
  server.begin();

  while(true)
  {
    server.handleClient();
    if(digitalRead(ENCODER_PUSH_BUTTON) == LOW)
    {
      delay(50);
      if(digitalRead(ENCODER_PUSH_BUTTON) == LOW)
      {
        WiFi.mode(WIFI_OFF);
        ESP.restart();
      }
    }
    delay(1);
  }
}

static int8_t readEncoder()
{
  uint8_t status = encoder.process();
  if(status == DIR_CW)  return 1;
  if(status == DIR_CCW) return -1;
  return 0;
}

// Point the ESP32 OTA selector at the recovery partition (ota_2) and reboot.
void bootRecoveryPartition()
{
  const esp_partition_t *target = esp_partition_find_first(
    ESP_PARTITION_TYPE_APP,
    static_cast<esp_partition_subtype_t>(ESP_PARTITION_SUBTYPE_APP_OTA_2),
    NULL);

  if(target)
  {
    esp_ota_set_boot_partition(target);
  }

  ESP.restart();
}

void recoveryMode()
{
  tft.init();
  tft.setRotation(3);
  ledcAttach(PIN_LCD_BL, 16000, 8);
  ledcWrite(PIN_LCD_BL, 255);

  pinMode(ENCODER_PIN_A, INPUT_PULLUP);
  pinMode(ENCODER_PIN_B, INPUT_PULLUP);
  pinMode(ENCODER_PUSH_BUTTON, INPUT_PULLUP);

  if(!LittleFS.begin(false, "/littlefs", 10, "littlefs"))
  {
    LittleFS.format();
    LittleFS.begin(false, "/littlefs", 10, "littlefs");
  }

  uint8_t selected = 0;

  while(digitalRead(ENCODER_PUSH_BUTTON) == LOW) delay(50);
  delay(100);

  drawRecoveryMenu(selected);

  while(true)
  {
    int8_t dir = readEncoder();

    if(dir > 0)
    {
      selected = (selected + 1) % RECOVERY_MENU_COUNT;
      drawRecoveryMenu(selected);
    }
    else if(dir < 0)
    {
      selected = (selected + RECOVERY_MENU_COUNT - 1) % RECOVERY_MENU_COUNT;
      drawRecoveryMenu(selected);
    }

    if(digitalRead(ENCODER_PUSH_BUTTON) == LOW)
    {
      delay(50);
      if(digitalRead(ENCODER_PUSH_BUTTON) == LOW)
      {
        uint32_t pressTime = millis();
        while(digitalRead(ENCODER_PUSH_BUTTON) == LOW) delay(10);
        uint32_t holdTime = millis() - pressTime;

        if(holdTime < 300)
          continue;

        switch(selected)
        {
          case 0:
          {
            String files[16];
            int fileCount = listFirmwareFiles(files, 16);

            if(fileCount == 0)
            {
              tft.fillScreen(TH.bg);
              tft.setTextDatum(TL_DATUM);
              tft.setTextColor(TH.text_warn, TH.bg);
              tft.drawString("No .bin files found", 10, 10, FONT_LARGE);
              tft.setTextColor(TH.text_muted, TH.bg);
              tft.drawString("Upload via WiFi File", 10, 50, FONT_SMALL);
              tft.drawString("Manager first.", 10, 70, FONT_SMALL);
              delay(3000);
              break;
            }

            int fileSelected = 0;
            drawFileList(files, fileCount, fileSelected);

            while(true)
            {
              int8_t fd = readEncoder();

              if(fd > 0)
              {
                fileSelected = (fileSelected + 1) % fileCount;
                drawFileList(files, fileCount, fileSelected);
              }
              else if(fd < 0)
              {
                fileSelected = (fileSelected + fileCount - 1) % fileCount;
                drawFileList(files, fileCount, fileSelected);
              }

              if(digitalRead(ENCODER_PUSH_BUTTON) == LOW)
              {
                delay(50);
                if(digitalRead(ENCODER_PUSH_BUTTON) == LOW)
                {
                  uint32_t pt = millis();
                  while(digitalRead(ENCODER_PUSH_BUTTON) == LOW) delay(10);
                  uint32_t ht = millis() - pt;

                  if(ht >= 300)
                  {
                    flashFirmware(files[fileSelected], 1);
                    delay(500);
                    ESP.restart();
                  }
                  else
                  {
                    break;
                  }
                }
              }
              delay(10);
            }

            break;
          }

          case 1:
          {
            tft.fillScreen(TH.bg);
            tft.setTextDatum(TL_DATUM);
            tft.setTextColor(TH.text, TH.bg);
            tft.drawString("FACTORY RESET", 10, 10, FONT_LARGE);
            tft.setTextColor(TH.text_warn, TH.bg);
            tft.drawString("Erasing all settings...", 10, 50, FONT_SMALL);

            nvsErase();
            LittleFS.format();

            tft.setTextColor(TH.text, TH.bg);
            tft.drawString("Done. Rebooting...", 10, 80, FONT_SMALL);
            delay(1500);
            ESP.restart();
            break;
          }

          case 2:
            startWiFiFileManager();
            break;

          case 3:
            ESP.restart();
            break;
        }

        drawRecoveryMenu(selected);
      }
    }

    delay(10);
  }
}