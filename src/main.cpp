/**
 * ESP32-D0WD + GC9A01 电脑状态仪表盘。
 * 数据由 Windows 采集器通过同一路由器的 UDP 单播发送；屏幕可脱离 USB。
 * 中文使用 Arduino_GFX 内置的 U8g2 Unifont 子集，源码统一 UTF-8。
 */
#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <WebServer.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace {
constexpr int kTftSck = 14, kTftMosi = 15, kTftCs = 5, kTftDc = 27;
constexpr int kTftReset = 33, kTftBacklight = 32;
constexpr int kButtonNext = 4, kButtonPrevious = 18;
constexpr int kWidth = 240, kHeight = 240;
constexpr uint16_t kBlack = 0x0000, kWhite = 0xFFFF;
constexpr uint16_t kDim = 0x7BEF, kCyan = 0x3DFF, kBlue = 0x435F;
constexpr uint16_t kViolet = 0xB59F, kAmber = 0xFDA0, kGreen = 0x5F76;
// Time-based frames may be skipped when SPI is busy; the animation never builds a backlog.
constexpr uint32_t kFrameMs = 30, kTransitionMs = 480, kPageDwellMs = 12000;
constexpr uint32_t kUdpPort = 4210, kUdpRetryMs = 3000, kWifiRetryMs = 20000;
constexpr uint32_t kDataTimeoutMs = 5000;
constexpr int32_t kTftSpiHz = 40000000;
constexpr size_t kOrbitDotCount = 90;

Arduino_ESP32SPI tftBus(
    kTftDc, kTftCs, kTftSck, kTftMosi, GFX_NOT_DEFINED, VSPI);
Arduino_GC9A01 panel(&tftBus, kTftReset, 0, true);

class ScreenCanvas final : public Arduino_Canvas {
 public:
  explicit ScreenCanvas(Arduino_GFX *out) : Arduino_Canvas(kWidth, kHeight, out) {}
  const uint16_t *pixels() const { return _framebuffer; }
};
ScreenCanvas canvas(&panel);
Arduino_GFX *screen = &panel;
bool mirrorReady = false;
int16_t orbitDotX[kOrbitDotCount] = {}, orbitDotY[kOrbitDotCount] = {};

Preferences preferences;
WiFiUDP udp;
WebServer portal(80);
DNSServer dns;
bool portalRunning = false;
bool udpReady = false;
bool wifiConnecting = false;
uint32_t lastConnectAttempt = 0;
String savedSsid;
String savedPassword;
String submittedSsid;
String submittedPassword;

struct Metrics {
  float cpu = 0, ram = 0;
  uint32_t ramUsedMb = 0, ramTotalMb = 0;
  uint32_t downBps = 0, upBps = 0;
  uint32_t diskUsedMb = 0, diskTotalMb = 0, uptimeSeconds = 0;
  float cpuTemp = NAN;
  bool valid = false;
  uint32_t receivedAt = 0;
  uint32_t sequence = 0;
} data;

enum Page : uint8_t { CPU_PAGE, MEMORY_PAGE, TEMP_PAGE, NETWORK_PAGE, DISK_PAGE, UPTIME_PAGE, PAGE_COUNT };
constexpr uint16_t kAccent[PAGE_COUNT] = {kCyan, kViolet, kAmber, kBlue, kGreen, kCyan};
constexpr char kPageNames[PAGE_COUNT][20] = {
    "处理器负载", "内存使用", "处理器温度", "网络下行", "系统盘空间", "运行时长"};
uint8_t currentPage = CPU_PAGE;
uint32_t pageChangedAt = 0, animationStartedAt = 0, lastFrameAt = 0;
uint32_t lastUdpAttemptAt = 0, udpPacketsReceived = 0, udpPacketsRejected = 0;
uint32_t lastRenderUs = 0, maxRenderUs = 0;
bool pageAnimating = false;
bool demoMode = false;

const char kPortalHtml[] PROGMEM = R"HTML(
<!doctype html><html lang="zh-CN"><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>ESP32 仪表盘配网</title><style>body{font-family:system-ui;background:#080b12;color:#eaf3ff;max-width:460px;margin:40px auto;padding:20px}h2{color:#51d9ff}input,button{box-sizing:border-box;width:100%;padding:13px;margin:9px 0;border-radius:9px;border:1px solid #39465c;background:#111827;color:#fff;font-size:16px}button{background:#168db1;border:0;font-weight:700}</style>
<h2>电脑状态仪表盘</h2><p>输入电脑所在的 2.4GHz WiFi 名称和密码。屏幕与电脑需要连接到同一个局域网。</p>
<form method="post" action="/save"><label>WiFi 名称</label><input name="ssid" maxlength="32" required><label>WiFi 密码</label><input name="password" type="password" maxlength="63"><button>保存并连接</button></form>
<p>连接成功后，运行电脑端的“局域网数据发送”程序即可。密码只保存在本设备中。</p></html>)HTML";

void setPage(uint8_t next) {
  next %= PAGE_COUNT;
  if (next == currentPage) return;
  currentPage = next;
  animationStartedAt = millis();
  pageAnimating = true;
  pageChangedAt = millis();
  lastFrameAt = pageChangedAt - kFrameMs;
}

void beginPortal() {
  WiFi.mode(WIFI_AP_STA);
  WiFi.setSleep(false);
  WiFi.setScanMethod(WIFI_ALL_CHANNEL_SCAN);
  WiFi.setAutoReconnect(true);
  WiFi.persistent(false);
  WiFi.softAP("D0WD-Dashboard-Setup");
  dns.start(53, "*", WiFi.softAPIP());
  portal.on("/", HTTP_GET, []() { portal.send_P(200, "text/html; charset=utf-8", kPortalHtml); });
  portal.on("/save", HTTP_POST, []() {
    submittedSsid = portal.arg("ssid");
    submittedPassword = portal.arg("password");
    if (submittedSsid.isEmpty()) {
      portal.send(400, "text/plain; charset=utf-8", "WiFi 名称不能为空");
      return;
    }
    preferences.putString("ssid", submittedSsid);
    preferences.putString("password", submittedPassword);
    savedSsid = submittedSsid;
    savedPassword = submittedPassword;
    portal.send(200, "text/html; charset=utf-8",
                "<meta charset='utf-8'><h2>正在连接 WiFi，请稍候观察屏幕。</h2>");
    WiFi.disconnect(false, false);
    WiFi.begin(savedSsid.c_str(), savedPassword.c_str());
    wifiConnecting = true;
    lastConnectAttempt = millis();
  });
  portal.onNotFound([]() {
    portal.sendHeader("Location", "http://192.168.4.1/");
    portal.send(302, "text/plain; charset=utf-8", "正在打开配网页面");
  });
  portal.begin();
  portalRunning = true;
  Serial.println("配网热点已启动：D0WD-Dashboard-Setup，网页地址 http://192.168.4.1");
}

void stopPortal() {
  if (!portalRunning) return;
  portal.stop();
  dns.stop();
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_STA);
  portalRunning = false;
}

void startStation() {
  if (savedSsid.isEmpty()) {
    beginPortal();
    return;
  }
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.setScanMethod(WIFI_ALL_CHANNEL_SCAN);
  WiFi.setAutoReconnect(true);
  WiFi.persistent(false);
  WiFi.setHostname("D0WD-Dashboard");
  WiFi.begin(savedSsid.c_str(), savedPassword.c_str());
  wifiConnecting = true;
  lastConnectAttempt = millis();
}

void drawCenteredAscii(const char *text, int y, uint8_t size, uint16_t color, int xShift = 0) {
  screen->setFont(static_cast<const GFXfont *>(nullptr));
  screen->setTextSize(size);
  int16_t x, boundsY;
  uint16_t w, h;
  screen->getTextBounds(text, 0, y, &x, &boundsY, &w, &h);
  screen->setTextColor(color);
  screen->setCursor((kWidth - static_cast<int>(w)) / 2 + xShift, y);
  screen->print(text);
}

void drawCenteredChinese(const char *text, int baseline, uint16_t color, int xShift = 0) {
  // Chinese glyphs are fixed 16 px; ASCII in this font is 8 px wide.
  int width = 0;
  for (const uint8_t *p = reinterpret_cast<const uint8_t *>(text); *p;) {
    if (*p < 0x80) { width += 8; ++p; }
    else if ((*p & 0xE0) == 0xC0) { width += 16; p += 2; }
    else if ((*p & 0xF0) == 0xE0) { width += 16; p += 3; }
    else { width += 16; p += 4; }
  }
  screen->setFont(u8g2_font_unifont_t_chinese4);
  screen->setTextSize(1);
  screen->setUTF8Print(true);
  screen->setTextColor(color);
  screen->setCursor((kWidth - width) / 2 + xShift, baseline);
  screen->print(text);
  screen->setUTF8Print(false);
  screen->setFont(static_cast<const GFXfont *>(nullptr));
}

void prepareOrbitDots() {
  constexpr float kRadians = 0.0174532925F;
  for (size_t i = 0; i < kOrbitDotCount; ++i) {
    const float angle = (135.0F + static_cast<float>(i) * 3.0F) * kRadians;
    orbitDotX[i] = 120 + static_cast<int16_t>(cosf(angle) * 91.0F);
    orbitDotY[i] = 116 + static_cast<int16_t>(sinf(angle) * 91.0F);
  }
}

float bezierProgress(float progress) {
  progress = constrain(progress, 0.0F, 1.0F);
  if (progress <= 0.0F || progress >= 1.0F) return progress;
  // Match kk_ui's no-overshoot cubic-bezier(0.25, 0, 0.20, 1) timing.
  float low = 0.0F, high = 1.0F, t = progress;
  for (uint8_t i = 0; i < 7; ++i) {
    t = (low + high) * 0.5F;
    const float inverse = 1.0F - t;
    const float x = 3.0F * inverse * inverse * t * 0.25F +
                    3.0F * inverse * t * t * 0.20F + t * t * t;
    if (x < progress) low = t; else high = t;
  }
  const float inverse = 1.0F - t;
  return 3.0F * inverse * t * t + t * t * t;
}

float pageEntrance(uint32_t now, uint16_t delayMs) {
  if (!pageAnimating) return 1.0F;
  const uint32_t elapsed = now - animationStartedAt;
  if (elapsed <= delayMs) return 0.0F;
  return bezierProgress(static_cast<float>(elapsed - delayMs) /
                        static_cast<float>(kTransitionMs - delayMs));
}

void drawOrbit(float phase, uint16_t accent) {
  constexpr int cx = 120, cy = 116, radius = 91;
  for (size_t i = 0; i < kOrbitDotCount; ++i) {
    const int x = orbitDotX[i];
    const int y = orbitDotY[i];
    const uint16_t c = i % 6 == 0 ? 0x2104 : 0x1082;
    // The fixed ring is cached once; only the small orbiting accents move each frame.
    screen->drawPixel(x, y, c);
    if (i % 6 == 0 && y < 168) screen->drawPixel(x + 1, y, c);
  }
  const float a = phase * 0.75F;
  for (int trail = 0; trail < 5; ++trail) {
    const float t = a - trail * 0.065F;
    const int x = cx + static_cast<int>(cosf(t) * radius);
    const int y = cy + static_cast<int>(sinf(t) * radius);
    // Keep moving accents above the title/value labels so they never touch glyphs.
    if (y < 120) screen->fillCircle(x, y, 4 - trail / 2, accent);
  }
  // A second, quieter orbit adds motion without fighting the large reading.
  const float b = -phase * 0.42F + 1.4F;
  const int bx = cx + static_cast<int>(cosf(b) * 72);
  const int by = cy + static_cast<int>(sinf(b) * 72);
  if (by < 124) screen->fillCircle(bx, by, 2, 0x735F);
}

void valueForCurrentPage(char *value, size_t capacity, char *subline, size_t subCapacity,
                         bool fresh) {
  // Never show zero-initialized or stale fields as current readings.
  if (!fresh) {
    snprintf(value, capacity, currentPage == TEMP_PAGE ? "--.-" : "--");
    snprintf(subline, subCapacity, "暂无电脑数据");
    return;
  }
  switch (currentPage) {
    case CPU_PAGE:
      snprintf(value, capacity, data.valid ? "%.0f" : "--", data.cpu);
      snprintf(subline, subCapacity, "全核平均占用");
      break;
    case MEMORY_PAGE:
      snprintf(value, capacity, data.valid ? "%.0f" : "--", data.ram);
      snprintf(subline, subCapacity, "%lu / %lu 兆字节",
               static_cast<unsigned long>(data.ramUsedMb), static_cast<unsigned long>(data.ramTotalMb));
      break;
    case TEMP_PAGE:
      if (isnan(data.cpuTemp)) strcpy(value, "--.-");
      else snprintf(value, capacity, "%.1f", data.cpuTemp);
      strcpy(subline, isnan(data.cpuTemp) ? "未检测到可信温度传感器" : "处理器封装温度");
      break;
    case NETWORK_PAGE:
      snprintf(value, capacity, "%.2f", data.downBps / 1048576.0F);
      snprintf(subline, subCapacity, "上行 %.2f 兆字节/秒", data.upBps / 1048576.0F);
      break;
    case DISK_PAGE: {
      const float percent = data.diskTotalMb ? data.diskUsedMb * 100.0F / data.diskTotalMb : NAN;
      if (isnan(percent)) strcpy(value, "--"); else snprintf(value, capacity, "%.0f", percent);
      snprintf(subline, subCapacity, "%lu / %lu 兆字节",
               static_cast<unsigned long>(data.diskUsedMb), static_cast<unsigned long>(data.diskTotalMb));
      break;
    }
    case UPTIME_PAGE: {
      const uint32_t hours = data.uptimeSeconds / 3600;
      if (hours >= 24) snprintf(value, capacity, "%lu", static_cast<unsigned long>(hours / 24));
      else snprintf(value, capacity, "%lu", static_cast<unsigned long>(hours));
      const uint32_t remain = data.uptimeSeconds % 86400;
      snprintf(subline, subCapacity, "运行 %02lu:%02lu:%02lu",
               static_cast<unsigned long>(remain / 3600), static_cast<unsigned long>((remain / 60) % 60),
               static_cast<unsigned long>(remain % 60));
      break;
  }
  }
}

void drawMetricPage(uint32_t now) {
  // Every frame is fully cleared before drawing. No old glyph can survive a page change.
  screen->fillScreen(kBlack);
  const float titleProgress = pageEntrance(now, 0);
  const float valueProgress = pageEntrance(now, 35);
  const float detailProgress = pageEntrance(now, 75);
  const int titleShift = static_cast<int>((1.0F - titleProgress) * 9.0F);
  const int valueShift = static_cast<int>((1.0F - valueProgress) * 20.0F);
  const int valueDrop = static_cast<int>((1.0F - valueProgress) * 7.0F);
  const int detailShift = static_cast<int>((1.0F - detailProgress) * 9.0F);
  const int detailRise = static_cast<int>((1.0F - detailProgress) * 5.0F);
  const uint16_t accent = kAccent[currentPage];
  const float phase = now * 0.0014F;
  drawOrbit(phase, accent);

  // Center breathing glow and fine ring pulse.
  const int pulse = 70 + static_cast<int>((sinf(phase * 1.7F) + 1.0F) * 3.0F);
  screen->drawCircle(120, 116, pulse, 0x1082);
  screen->drawCircle(120, 116, pulse - 1, 0x0841);
  drawCenteredChinese(kPageNames[currentPage], 44, kDim, titleShift);

  char value[24] = {}, subline[48] = {};
  const bool fresh = data.valid && now - data.receivedAt < kDataTimeoutMs;
  valueForCurrentPage(value, sizeof(value), subline, sizeof(subline), fresh);
  drawCenteredAscii(value, 84 + valueDrop, currentPage == UPTIME_PAGE ? 5 : 6,
                    kWhite, valueShift);

  const char *unit = "百分比";
  if (currentPage == TEMP_PAGE) unit = "摄氏度";
  else if (currentPage == NETWORK_PAGE) unit = "兆字节/秒";
  else if (currentPage == UPTIME_PAGE) unit = fresh && data.uptimeSeconds / 3600 >= 24 ? "天" : "小时";
  // Keep all labels inside the round panel's lower arc (its safe width narrows quickly).
  drawCenteredChinese(unit, 143 + detailRise, accent, detailShift);
  drawCenteredChinese(subline, 162 + detailRise, kDim, -detailShift);

  const bool connected = WiFi.status() == WL_CONNECTED;
  // One concise status line shows both WiFi and host-data state without leaving the circle.
  const char *status = !connected
      ? (portalRunning ? (savedSsid.isEmpty() ? "手机连仪表热点" : "已保存网络重试中")
                       : "无线连接中")
      : (fresh ? "电脑数据已连接" : "无线已连接等数据");
  const uint16_t statusColor = !connected ? kAmber : (fresh ? kGreen : kCyan);
  drawCenteredChinese(status, 198, statusColor);

  // Six position dots and a rotating comet show page position and animation.
  for (uint8_t i = 0; i < PAGE_COUNT; ++i) {
    screen->fillCircle(92 + i * 11, 179, i == currentPage ? 3 : 1,
                       i == currentPage ? accent : 0x39C7);
  }
}

void render(uint32_t now) {
  if (pageAnimating && now - animationStartedAt >= kTransitionMs) pageAnimating = false;
  drawMetricPage(now);
  if (mirrorReady) canvas.flush();
}

bool parsePacket(char *line) {
  unsigned long seq = 0, ramUsed = 0, ramTotal = 0, down = 0, up = 0;
  unsigned long diskUsed = 0, diskTotal = 0, uptime = 0;
  int cpu10 = 0, ram10 = 0, temp10 = -9999;
  const int fields = sscanf(line, "PC2,%lu,%d,%d,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%d",
      &seq, &cpu10, &ram10, &ramUsed, &ramTotal, &down, &up,
      &diskUsed, &diskTotal, &uptime, &temp10);
  if (fields != 11 || cpu10 < 0 || cpu10 > 1000 || ram10 < 0 || ram10 > 1000 ||
      ramTotal == 0 || diskTotal == 0 || down > 1000000000UL ||
      up > 1000000000UL || (temp10 != -9999 && (temp10 < 0 || temp10 > 1250))) return false;
  data.sequence = seq;
  data.cpu = constrain(cpu10 / 10.0F, 0.0F, 100.0F);
  data.ram = constrain(ram10 / 10.0F, 0.0F, 100.0F);
  data.ramUsedMb = ramUsed;
  data.ramTotalMb = ramTotal;
  data.downBps = down;
  data.upBps = up;
  data.diskUsedMb = diskUsed;
  data.diskTotalMb = diskTotal;
  data.uptimeSeconds = uptime;
  data.cpuTemp = temp10 == -9999 ? NAN : temp10 / 10.0F;
  data.valid = true;
  data.receivedAt = millis();
  return true;
}

void processUdp() {
  const int packetSize = udp.parsePacket();
  if (packetSize <= 0) return;
  char line[128];
  if (packetSize >= static_cast<int>(sizeof(line))) {
    while (udp.available()) udp.read();
    ++udpPacketsRejected;
    return;
  }
  const int len = udp.read(line, sizeof(line) - 1);
  if (len <= 0) { ++udpPacketsRejected; return; }
  line[len] = '\0';
  if (parsePacket(line)) ++udpPacketsReceived;
  else ++udpPacketsRejected;
}

void sendFrameCapture() {
  if (!mirrorReady) {
    Serial.println("FRAME_ERROR: full-size mirror unavailable");
    return;
  }
  canvas.flush();
  Serial.println("FRAME_BEGIN_RGB565LE_240x240");
  Serial.flush();
  Serial.write(reinterpret_cast<const uint8_t *>(canvas.pixels()), kWidth * kHeight * 2);
  Serial.flush();
  Serial.println("\nFRAME_END");
}

void receiveSerialCommands() {
  static char command[40];
  static size_t length = 0;
  while (Serial.available()) {
    const char c = static_cast<char>(Serial.read());
    if (c == '\r') continue;
    if (c == '\n') {
      command[length] = 0;
      if (strcmp(command, "CAPTURE") == 0) sendFrameCapture();
      else if (strncmp(command, "PAGE,", 5) == 0) {
        const int requested = atoi(command + 5);
        currentPage = requested % PAGE_COUNT;
        pageAnimating = false;
        pageChangedAt = millis();
      } else if (strcmp(command, "NEXT") == 0) {
        setPage((currentPage + 1) % PAGE_COUNT);
      } else if (strcmp(command, "PREV") == 0) {
        setPage((currentPage + PAGE_COUNT - 1) % PAGE_COUNT);
      } else if (strncmp(command, "DEMO,", 5) == 0) {
        demoMode = true;
        data.cpu = 68.0F;
        data.ram = 74.0F;
        data.ramUsedMb = 11840;
        data.ramTotalMb = 16000;
        data.downBps = 3 * 1024 * 1024 + 640 * 1024;
        data.upBps = 512 * 1024;
        data.diskUsedMb = 350000;
        data.diskTotalMb = 1000000;
        data.uptimeSeconds = 8 * 3600 + 23 * 60 + 14;
        data.cpuTemp = 64.5F;
        data.valid = true;
        data.receivedAt = millis();
        currentPage = atoi(command + 5) % PAGE_COUNT;
        pageAnimating = false;
        pageChangedAt = millis();
      } else if (strcmp(command, "REAL") == 0) {
        demoMode = false;
        data.valid = false;
      } else if (strncmp(command, "SETWIFI,", 8) == 0) {
        char *separator = strchr(command + 8, ',');
        if (!separator) {
          Serial.println("SETWIFI 格式错误；使用 SETWIFI,WiFi名称,WiFi密码");
        } else {
          *separator = '\0';
          const String ssid = command + 8;
          const String password = separator + 1;
          if (ssid.isEmpty() || ssid.length() > 32 || password.length() > 63) {
            Serial.println("Wi-Fi 名称或密码长度无效，未修改已保存配置");
          } else {
            preferences.putString("ssid", ssid);
            preferences.putString("password", password);
            savedSsid = ssid;
            savedPassword = password;
            if (portalRunning) stopPortal();
            WiFi.mode(WIFI_STA);
            WiFi.disconnect(false, false);
            WiFi.setSleep(false);
            WiFi.setScanMethod(WIFI_ALL_CHANNEL_SCAN);
            WiFi.setAutoReconnect(true);
            WiFi.persistent(false);
            WiFi.begin(savedSsid.c_str(), savedPassword.c_str());
            wifiConnecting = true;
            lastConnectAttempt = millis();
            Serial.println("Wi-Fi 凭据已保存到 NVS，正在连接路由器");
          }
        }
      } else if (strcmp(command, "SCAN") == 0) {
        WiFi.disconnect(false, false);
        delay(100);
        Serial.println("WIFI_SCAN_BEGIN");
        const int count = WiFi.scanNetworks(false, true);
        Serial.printf("WIFI_SCAN_COUNT,%d\n", count);
        for (int i = 0; i < count; ++i) {
          Serial.printf("WIFI_AP,%s,%d,%u,%d\n", WiFi.SSID(i).c_str(), WiFi.RSSI(i),
                        static_cast<unsigned>(WiFi.channel(i)),
                        static_cast<int>(WiFi.encryptionType(i)));
        }
        Serial.println("WIFI_SCAN_END");
        WiFi.scanDelete();
        if (!savedSsid.isEmpty()) {
          WiFi.begin(savedSsid.c_str(), savedPassword.c_str());
          wifiConnecting = true;
          lastConnectAttempt = millis();
        }
      } else if (strcmp(command, "INFO") == 0) {
        const String ip = portalRunning ? WiFi.softAPIP().toString() : WiFi.localIP().toString();
        const bool fresh = data.valid && millis() - data.receivedAt < kDataTimeoutMs;
        Serial.printf("PSRAM=%u bytes, free heap=%u, WiFi=%d, IP=%s, UDP=%d, RX=%lu, bad=%lu, frame=%lu/%lu us, sample=%s\n",
                      static_cast<unsigned>(ESP.getPsramSize()),
                      static_cast<unsigned>(ESP.getFreeHeap()), WiFi.status(),
                      ip.c_str(), udpReady, static_cast<unsigned long>(udpPacketsReceived),
                      static_cast<unsigned long>(udpPacketsRejected),
                      static_cast<unsigned long>(lastRenderUs), static_cast<unsigned long>(maxRenderUs),
                      fresh ? "valid" : "waiting");
      }
      length = 0;
    } else if (length + 1 < sizeof(command)) command[length++] = c;
    else length = 0;
  }
}

void handleButtons() {
  static int previous[2] = {HIGH, HIGH}, stable[2] = {HIGH, HIGH};
  static uint32_t changed[2] = {0, 0};
  const int pins[2] = {kButtonNext, kButtonPrevious};
  const uint32_t now = millis();
  for (int i = 0; i < 2; ++i) {
    const int reading = digitalRead(pins[i]);
    if (reading != previous[i]) { previous[i] = reading; changed[i] = now; }
    if (now - changed[i] > 28 && stable[i] != reading) {
      stable[i] = reading;
      if (stable[i] == LOW) setPage((currentPage + (i == 0 ? 1 : PAGE_COUNT - 1)) % PAGE_COUNT);
    }
  }
}

void serviceWiFi(uint32_t now) {
  if (portalRunning) { dns.processNextRequest(); portal.handleClient(); }
  if (WiFi.status() == WL_CONNECTED) {
    wifiConnecting = false;
    if (portalRunning) stopPortal();
    if (!udpReady && (!lastUdpAttemptAt || now - lastUdpAttemptAt >= kUdpRetryMs)) {
      udp.stop();
      lastUdpAttemptAt = now;
      udpReady = udp.begin(kUdpPort);
      if (udpReady) {
        Serial.printf("无线网络已连接，地址 %s，UDP 已启动\n", WiFi.localIP().toString().c_str());
      }
    }
    return;
  }
  if (udpReady) {
    udp.stop();
    udpReady = false;
    lastUdpAttemptAt = 0;
  }
  // If saved credentials are stale or the router is unavailable, reopen setup
  // AP after a short timeout so the board remains recoverable without USB.
  if (!portalRunning && now > 30000) {
    beginPortal();
    return;
  }
  // Keep retrying saved router credentials even while the setup hotspot is up.
  // A temporary handshake failure must not strand the board on the portal page.
  if (!savedSsid.isEmpty() && now - lastConnectAttempt > kWifiRetryMs) {
    WiFi.disconnect(false, false);
    WiFi.begin(savedSsid.c_str(), savedPassword.c_str());
    wifiConnecting = true;
    lastConnectAttempt = now;
  }
}

void clearWiFiAndReprovision() {
  preferences.clear();
  savedSsid = "";
  savedPassword = "";
  WiFi.disconnect(true, true);
  delay(100);
  ESP.restart();
}

void handleResetGesture() {
  static uint32_t bothPressedAt = 0;
  static bool alreadyTriggered = false;
  const bool held = digitalRead(kButtonNext) == LOW && digitalRead(kButtonPrevious) == LOW;
  if (!held) { bothPressedAt = 0; alreadyTriggered = false; return; }
  if (!bothPressedAt) bothPressedAt = millis();
  if (!alreadyTriggered && millis() - bothPressedAt > 5000) {
    alreadyTriggered = true;
    clearWiFiAndReprovision();
  }
}
}  // namespace

void setup() {
  Serial.begin(115200);
  delay(150);
  canvas.begin(kTftSpiHz);
  mirrorReady = canvas.pixels() != nullptr;
  if (mirrorReady) screen = &canvas;
  else panel.begin(kTftSpiHz);
  prepareOrbitDots();
  screen->setRotation(0);
  screen->setUTF8Print(false);
  screen->setFont(static_cast<const GFXfont *>(nullptr));
  pinMode(kTftBacklight, OUTPUT);
  digitalWrite(kTftBacklight, HIGH);
  pinMode(kButtonNext, INPUT_PULLUP);
  pinMode(kButtonPrevious, INPUT_PULLUP);
  preferences.begin("pc-dash", false);
  savedSsid = preferences.getString("ssid", "");
  savedPassword = preferences.getString("password", "");
  startStation();
  pageChangedAt = millis();
  Serial.printf("PSRAM: %s, size=%u bytes, free=%u; full-frame mirror: %s\n",
                psramFound() ? "detected" : "not detected",
                static_cast<unsigned>(ESP.getPsramSize()),
                static_cast<unsigned>(ESP.getFreePsram()),
                mirrorReady ? "ready" : "off (direct TFT drawing)");
  Serial.println("仪表盘已启动，接收端口 UDP 4210；串口支持 PAGE,n / DEMO,n / CAPTURE / INFO / SCAN / SETWIFI,ssid,password");
}

void loop() {
  serviceWiFi(millis());
  if (WiFi.status() == WL_CONNECTED && udpReady) processUdp();
  receiveSerialCommands();
  handleButtons();
  handleResetGesture();
  const uint32_t afterInput = millis();
  if (!pageAnimating && afterInput - pageChangedAt >= kPageDwellMs) {
    setPage((currentPage + 1) % PAGE_COUNT);
  }
  if (afterInput - lastFrameAt >= kFrameMs) {
    lastFrameAt = afterInput;
    const uint32_t renderStarted = micros();
    render(afterInput);
    lastRenderUs = micros() - renderStarted;
    if (lastRenderUs > maxRenderUs) maxRenderUs = lastRenderUs;
  }
  delay(1);
}
