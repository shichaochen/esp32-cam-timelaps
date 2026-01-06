#include "esp_camera.h"
#include "FS.h"
#include "SD_MMC.h"
#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include <time.h>
#include "esp_sleep.h"

// 配置AP模式（首次配置时使用）
const char* ap_ssid = "ESP32-CAM-Config";
const char* ap_password = "12345678";  // AP密码，至少8位

// 存储配置的命名空间
Preferences preferences;

// Wi-Fi配置变量（从Preferences读取）
String wifi_ssid = "";
String wifi_password = "";

// Web服务器（用于配置界面）
WebServer server(80);

// NTP服务器配置
const char* ntpServer = "pool.ntp.org";
const long gmtOffset_sec = 8 * 3600;  // GMT+8 (北京时间)
const int daylightOffset_sec = 0;

// 相机引脚定义 (ESP32-CAM)
#define PWDN_GPIO_NUM     32
#define RESET_GPIO_NUM    -1
#define XCLK_GPIO_NUM      0
#define SIOD_GPIO_NUM     26
#define SIOC_GPIO_NUM     27
#define Y9_GPIO_NUM       35
#define Y8_GPIO_NUM       34
#define Y7_GPIO_NUM       39
#define Y6_GPIO_NUM       36
#define Y5_GPIO_NUM       21
#define Y4_GPIO_NUM       19
#define Y3_GPIO_NUM       18
#define Y2_GPIO_NUM        5
#define VSYNC_GPIO_NUM    25
#define HREF_GPIO_NUM     23
#define PCLK_GPIO_NUM     22

// 深度睡眠时间（微秒）- 15分钟
#define SLEEP_DURATION_US (15 * 60 * 1000000ULL)

// 配置标志
bool wifiConfigured = false;

void setup() {
  Serial.begin(115200);
  Serial.setDebugOutput(true);
  Serial.println("\n\nESP32-CAM 定时拍摄程序启动");

  // 初始化Preferences
  preferences.begin("wifi-config", false);
  
  // 读取保存的WiFi配置
  wifi_ssid = preferences.getString("ssid", "");
  wifi_password = preferences.getString("password", "");

  // 检查是否已配置WiFi
  if (wifi_ssid.length() == 0) {
    Serial.println("未检测到WiFi配置，进入配置模式...");
    startConfigMode();
    return;  // 配置模式不会返回
  }

  Serial.printf("读取到保存的WiFi配置: %s\n", wifi_ssid.c_str());

  // 初始化相机
  if (!initCamera()) {
    Serial.println("相机初始化失败！");
    goToSleep();
    return;
  }

  // 初始化SD卡
  if (!initSDCard()) {
    Serial.println("SD卡初始化失败！");
    goToSleep();
    return;
  }

  // 连接Wi-Fi
  if (!connectWiFi()) {
    Serial.println("Wi-Fi连接失败！进入配置模式...");
    startConfigMode();
    return;
  }

  // 同步NTP时间
  if (!syncTime()) {
    Serial.println("时间同步失败！");
    goToSleep();
    return;
  }

  // 拍摄照片
  captureAndSavePhoto();

  // 进入深度睡眠
  Serial.println("进入深度睡眠15分钟...");
  goToSleep();
}

void loop() {
  // 如果在配置模式，处理Web服务器请求
  if (!wifiConfigured) {
    server.handleClient();
    delay(10);
  }
  // 正常模式下不会运行到这里，因为setup后会进入深度睡眠
}

bool initCamera() {
  camera_config_t config;
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer = LEDC_TIMER_0;
  config.pin_d0 = Y2_GPIO_NUM;
  config.pin_d1 = Y3_GPIO_NUM;
  config.pin_d2 = Y4_GPIO_NUM;
  config.pin_d3 = Y5_GPIO_NUM;
  config.pin_d4 = Y6_GPIO_NUM;
  config.pin_d5 = Y7_GPIO_NUM;
  config.pin_d6 = Y8_GPIO_NUM;
  config.pin_d7 = Y9_GPIO_NUM;
  config.pin_xclk = XCLK_GPIO_NUM;
  config.pin_pclk = PCLK_GPIO_NUM;
  config.pin_vsync = VSYNC_GPIO_NUM;
  config.pin_href = HREF_GPIO_NUM;
  config.pin_sscb_sda = SIOD_GPIO_NUM;
  config.pin_sscb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn = PWDN_GPIO_NUM;
  config.pin_reset = RESET_GPIO_NUM;
  config.xclk_freq_hz = 20000000;
  config.pixel_format = PIXFORMAT_JPEG;

  // 高分辨率设置 - UXGA (1600x1200)
  config.frame_size = FRAMESIZE_UXGA;
  config.jpeg_quality = 12;  // 0-63，数值越小质量越高
  config.fb_count = 1;

  // 如果PSRAM可用，使用更大的缓冲区
  if (psramFound()) {
    config.fb_count = 2;
    config.grab_mode = CAMERA_GRAB_WHEN_EMPTY;
  } else {
    // 如果没有PSRAM，降低分辨率
    config.frame_size = FRAMESIZE_SVGA;
    config.fb_count = 1;
  }

  // 初始化相机
  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    Serial.printf("相机初始化失败，错误代码: 0x%x\n", err);
    return false;
  }

  // 获取相机传感器信息
  sensor_t *s = esp_camera_sensor_get();
  if (s != NULL) {
    s->set_brightness(s, 0);     // -2 to 2
    s->set_contrast(s, 0);       // -2 to 2
    s->set_saturation(s, 0);     // -2 to 2
    s->set_special_effect(s, 0); // 0 to 6 (0-No Effect, 1-Negative, 2-Grayscale, 3-Red Tint, 4-Green Tint, 5-Blue Tint, 6-Sepia)
    s->set_whitebal(s, 1);       // 0 = disable , 1 = enable
    s->set_awb_gain(s, 1);       // 0 = disable , 1 = enable
    s->set_wb_mode(s, 0);        // 0 to 4 - if awb_gain enabled (0 - Auto, 1 - Sunny, 2 - Cloudy, 3 - Office, 4 - Home)
    s->set_exposure_ctrl(s, 1);  // 0 = disable , 1 = enable
    s->set_aec2(s, 0);           // 0 = disable , 1 = enable
    s->set_ae_level(s, 0);       // -2 to 2
    s->set_aec_value(s, 300);    // 0 to 1200
    s->set_gain_ctrl(s, 1);      // 0 = disable , 1 = enable
    s->set_agc_gain(s, 0);       // 0 to 30
    s->set_gainceiling(s, (gainceiling_t)0);  // 0 to 6
    s->set_bpc(s, 0);            // 0 = disable , 1 = enable
    s->set_wpc(s, 1);            // 0 = disable , 1 = enable
    s->set_raw_gma(s, 1);        // 0 = disable , 1 = enable
    s->set_lenc(s, 1);           // 0 = disable , 1 = enable
    s->set_hmirror(s, 0);        // 0 = disable , 1 = enable
    s->set_vflip(s, 0);          // 0 = disable , 1 = enable
    s->set_dcw(s, 1);            // 0 = disable , 1 = enable
    s->set_colorbar(s, 0);       // 0 = disable , 1 = enable
  }

  Serial.println("相机初始化成功");
  return true;
}

bool initSDCard() {
  Serial.println("初始化SD卡...");
  
  if (!SD_MMC.begin()) {
    Serial.println("SD卡挂载失败");
    return false;
  }

  uint8_t cardType = SD_MMC.cardType();
  if (cardType == CARD_NONE) {
    Serial.println("未检测到SD卡");
    return false;
  }

  Serial.print("SD卡类型: ");
  if (cardType == CARD_MMC) {
    Serial.println("MMC");
  } else if (cardType == CARD_SD) {
    Serial.println("SDSC");
  } else if (cardType == CARD_SDHC) {
    Serial.println("SDHC");
  } else {
    Serial.println("未知");
  }

  uint64_t cardSize = SD_MMC.cardSize() / (1024 * 1024);
  Serial.printf("SD卡大小: %lluMB\n", cardSize);

  return true;
}

bool connectWiFi() {
  if (wifi_ssid.length() == 0) {
    Serial.println("未配置WiFi");
    return false;
  }

  Serial.printf("正在连接Wi-Fi: %s\n", wifi_ssid.c_str());
  WiFi.mode(WIFI_STA);
  WiFi.begin(wifi_ssid.c_str(), wifi_password.c_str());

  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 20) {
    delay(500);
    Serial.print(".");
    attempts++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nWi-Fi连接成功！");
    Serial.print("IP地址: ");
    Serial.println(WiFi.localIP());
    return true;
  } else {
    Serial.println("\nWi-Fi连接失败");
    return false;
  }
}

// 配置模式：创建AP和Web服务器
void startConfigMode() {
  wifiConfigured = false;
  
  Serial.println("启动配置模式...");
  Serial.printf("AP SSID: %s\n", ap_ssid);
  Serial.printf("AP Password: %s\n", ap_password);
  
  // 创建AP
  WiFi.mode(WIFI_AP);
  WiFi.softAP(ap_ssid, ap_password);
  
  IPAddress IP = WiFi.softAPIP();
  Serial.print("AP IP地址: ");
  Serial.println(IP);
  Serial.println("请连接到WiFi网络并访问: http://192.168.4.1");

  // 配置Web服务器路由
  server.on("/", handleRoot);
  server.on("/config", handleConfig);
  server.on("/save", HTTP_POST, handleSave);
  server.onNotFound(handleNotFound);
  
  server.begin();
  Serial.println("Web服务器已启动，等待配置...");
  
  // 在配置模式下保持运行
  while (true) {
    server.handleClient();
    delay(10);
  }
}

// 根路径：显示配置页面
void handleRoot() {
  String html = "<!DOCTYPE html><html><head>";
  html += "<meta charset='UTF-8'>";
  html += "<meta name='viewport' content='width=device-width, initial-scale=1.0'>";
  html += "<title>ESP32-CAM WiFi配置</title>";
  html += "<style>";
  html += "body { font-family: Arial, sans-serif; max-width: 500px; margin: 50px auto; padding: 20px; background: #f5f5f5; }";
  html += "h1 { color: #333; text-align: center; }";
  html += "form { background: white; padding: 30px; border-radius: 10px; box-shadow: 0 2px 10px rgba(0,0,0,0.1); }";
  html += "label { display: block; margin: 15px 0 5px; color: #555; font-weight: bold; }";
  html += "input { width: 100%; padding: 12px; border: 2px solid #ddd; border-radius: 5px; font-size: 16px; box-sizing: border-box; }";
  html += "input:focus { border-color: #4CAF50; outline: none; }";
  html += "button { width: 100%; padding: 12px; background: #4CAF50; color: white; border: none; border-radius: 5px; font-size: 16px; cursor: pointer; margin-top: 20px; }";
  html += "button:hover { background: #45a049; }";
  html += ".info { background: #e3f2fd; padding: 15px; border-radius: 5px; margin-bottom: 20px; color: #1976d2; }";
  html += "</style></head><body>";
  html += "<h1>📷 ESP32-CAM WiFi配置</h1>";
  html += "<div class='info'>";
  html += "<strong>提示：</strong>请填写您的WiFi网络信息，配置后将自动保存并重启设备。";
  html += "</div>";
  html += "<form action='/save' method='POST'>";
  html += "<label for='ssid'>WiFi名称 (SSID):</label>";
  html += "<input type='text' id='ssid' name='ssid' required placeholder='请输入WiFi名称'>";
  html += "<label for='password'>WiFi密码:</label>";
  html += "<input type='password' id='password' name='password' required placeholder='请输入WiFi密码'>";
  html += "<button type='submit'>保存配置</button>";
  html += "</form>";
  html += "</body></html>";
  
  server.send(200, "text/html; charset=UTF-8", html);
}

// 保存配置
void handleSave() {
  if (server.hasArg("ssid") && server.hasArg("password")) {
    String new_ssid = server.arg("ssid");
    String new_password = server.arg("password");
    
    // 保存到Preferences
    preferences.putString("ssid", new_ssid);
    preferences.putString("password", new_password);
    preferences.end();
    
    Serial.printf("WiFi配置已保存: %s\n", new_ssid.c_str());
    
    // 返回成功页面
    String html = "<!DOCTYPE html><html><head>";
    html += "<meta charset='UTF-8'>";
    html += "<meta name='viewport' content='width=device-width, initial-scale=1.0'>";
    html += "<meta http-equiv='refresh' content='5;url=/'>";
    html += "<title>配置成功</title>";
    html += "<style>";
    html += "body { font-family: Arial, sans-serif; max-width: 500px; margin: 50px auto; padding: 20px; text-align: center; background: #f5f5f5; }";
    html += "h1 { color: #4CAF50; }";
    html += ".success { background: #d4edda; padding: 20px; border-radius: 10px; color: #155724; margin: 20px 0; }";
    html += "</style></head><body>";
    html += "<h1>✅ 配置成功！</h1>";
    html += "<div class='success'>";
    html += "<p>WiFi配置已保存</p>";
    html += "<p>设备将在5秒后重启并连接到新网络</p>";
    html += "</div>";
    html += "</body></html>";
    
    server.send(200, "text/html; charset=UTF-8", html);
    
    delay(2000);
    ESP.restart();
  } else {
    server.send(400, "text/plain", "参数错误");
  }
}

// 配置页面（备用）
void handleConfig() {
  handleRoot();
}

// 404处理
void handleNotFound() {
  server.send(404, "text/plain", "页面未找到");
}

bool syncTime() {
  Serial.println("正在同步NTP时间...");
  configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
  
  struct tm timeinfo;
  int attempts = 0;
  while (!getLocalTime(&timeinfo) && attempts < 10) {
    Serial.println("等待时间同步...");
    delay(1000);
    attempts++;
  }

  if (getLocalTime(&timeinfo)) {
    Serial.println("时间同步成功");
    Serial.print("当前时间: ");
    Serial.println(&timeinfo, "%Y-%m-%d %H:%M:%S");
    return true;
  } else {
    Serial.println("时间同步失败");
    return false;
  }
}

String getTimeString() {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) {
    return "unknown";
  }

  char timeStr[20];
  strftime(timeStr, sizeof(timeStr), "%Y_%m_%d_%H:%M", &timeinfo);
  return String(timeStr);
}

void captureAndSavePhoto() {
  Serial.println("正在拍摄照片...");
  
  camera_fb_t *fb = esp_camera_fb_get();
  if (!fb) {
    Serial.println("拍照失败！");
    return;
  }

  Serial.printf("照片大小: %zu 字节\n", fb->len);

  // 生成文件名
  String filename = "/" + getTimeString() + ".jpg";
  Serial.printf("保存文件: %s\n", filename.c_str());

  // 保存到SD卡
  File file = SD_MMC.open(filename.c_str(), FILE_WRITE);
  if (!file) {
    Serial.println("无法创建文件");
    esp_camera_fb_return(fb);
    return;
  }

  file.write(fb->buf, fb->len);
  file.close();
  Serial.println("照片保存成功！");

  // 释放帧缓冲区
  esp_camera_fb_return(fb);
}

void goToSleep() {
  // 断开Wi-Fi以节省功耗
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  
  // 关闭SD卡
  SD_MMC.end();
  
  // 关闭相机
  esp_camera_deinit();
  
  Serial.flush();
  delay(100);
  
  // 进入深度睡眠
  esp_sleep_enable_timer_wakeup(SLEEP_DURATION_US);
  esp_deep_sleep_start();
}

