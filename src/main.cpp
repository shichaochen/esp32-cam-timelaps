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
#define LED_GPIO_NUM       4  // 闪光灯引脚

// 深度睡眠时间（微秒）- 10分钟
#define SLEEP_DURATION_US (10 * 60 * 1000000ULL)

// 白平衡模式配置
// 0 = Auto (自动), 1 = Sunny (日光), 2 = Cloudy (阴天), 3 = Office (办公室), 4 = Home (室内)
// 如果照片偏绿，尝试使用 1 (Sunny) 或 2 (Cloudy)
#define WB_MODE 1  // 默认使用日光模式，改善偏绿问题

// JPEG质量配置 (0-63，数值越小质量越高，文件越大)
// 推荐值: 10-12 (平衡质量和大小), 8-10 (高质量), 5-8 (最高质量，文件较大)
// 当前50KB左右，提高质量后预计80-120KB
#define JPEG_QUALITY 10  // 从12提高到10，提升照片质量

// 配置标志
bool wifiConfigured = false;

void setup() {
  Serial.begin(115200);
  delay(1000);  // 等待串口稳定
  Serial.setDebugOutput(true);
  Serial.println("\n\nESP32-CAM 定时拍摄程序启动");
  Serial.flush();

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

  // 所有初始化完成，闪光灯闪烁3次表示就绪
  Serial.println("所有初始化完成，系统就绪！");
  Serial.flush();
  flashLED(3, 200);  // 闪烁3次，每次200ms

  // 启动Web服务器（用于查看状态、浏览照片和重新配置）
  server.on("/", handleStatus);
  server.on("/config", handleRoot);
  server.on("/save", HTTP_POST, handleSave);
  server.on("/reset", handleReset);
  server.on("/photos", handlePhotos);  // 照片列表页面
  server.on("/photo", handlePhoto);    // 查看/下载单个照片
  server.on("/delete", HTTP_GET, handleDelete);  // 删除照片
  server.on("/test", []() {  // 测试路由
    server.send(200, "text/plain", "Web服务器正常工作！");
  });
  server.onNotFound(handleNotFound);
  server.begin();
  Serial.printf("Web服务器已启动！\n");
  Serial.printf("访问地址: http://%s/\n", WiFi.localIP().toString().c_str());
  Serial.printf("照片浏览: http://%s/photos\n", WiFi.localIP().toString().c_str());
  Serial.printf("测试页面: http://%s/test\n", WiFi.localIP().toString().c_str());
  Serial.flush();

  // 检查唤醒原因
  esp_sleep_wakeup_cause_t wakeup_reason = esp_sleep_get_wakeup_cause();
  
  if (wakeup_reason == ESP_SLEEP_WAKEUP_UNDEFINED) {
    // 首次上电（不是深度睡眠唤醒）
    Serial.println("首次上电，拍摄第一张照片...");
    Serial.flush();
    
    // 拍摄第一张照片
    captureAndSavePhoto();
    
    // 保持10分钟不休眠（只是等待，不进入深度睡眠）
    Serial.println("首次上电，保持10分钟不休眠...");
    Serial.println("在此期间，可以通过Web界面访问设备");
    Serial.printf("Web服务器地址: http://%s\n", WiFi.localIP().toString().c_str());
    Serial.flush();
    
    // 等待10分钟，但需要处理Web服务器请求
    unsigned long waitTime = SLEEP_DURATION_US / 1000;  // 转换为毫秒
    unsigned long startTime = millis();
    unsigned long elapsedTime = 0;
    unsigned long lastPrintTime = 0;
    
    while (elapsedTime < waitTime) {
      // 处理Web服务器请求（重要：让Web服务器能够响应）
      server.handleClient();
      
      // 非阻塞延迟
      delay(100);
      
      elapsedTime = millis() - startTime;
      unsigned long remaining = (waitTime - elapsedTime) / 1000;
      
      // 每分钟打印一次剩余时间
      if (remaining != lastPrintTime && remaining % 60 == 0 && remaining > 0) {
        Serial.printf("首次上电等待中，剩余时间: %lu 分钟\n", remaining / 60);
        Serial.flush();
        lastPrintTime = remaining;
      }
    }
    
    Serial.println("首次上电10分钟等待完成，现在进入正常休眠循环模式...");
    Serial.flush();
  } else {
    // 深度睡眠唤醒，正常模式
    Serial.println("深度睡眠唤醒，正常拍摄模式");
    Serial.printf("Web服务器已启动，可以通过以下地址访问:\n");
    Serial.printf("主页: http://%s/\n", WiFi.localIP().toString().c_str());
    Serial.printf("照片浏览: http://%s/photos\n", WiFi.localIP().toString().c_str());
    Serial.flush();
    
    // 深度睡眠唤醒后，给Web服务器一些时间处理请求（30秒）
    Serial.println("深度睡眠唤醒后，Web服务器将运行30秒供访问...");
    unsigned long webTime = 30000;  // 30秒
    unsigned long webStart = millis();
    while (millis() - webStart < webTime) {
      server.handleClient();
      delay(100);
    }
    Serial.println("30秒Web服务器访问时间结束，开始拍摄照片...");
    Serial.flush();
  }

  // 拍摄照片
  captureAndSavePhoto();

  // 进入深度睡眠前，给Web服务器一些时间处理请求
  Serial.println("进入深度睡眠10分钟...");
  Serial.printf("深度睡眠期间Web服务器将停止，唤醒后会重新启动\n");
  Serial.printf("如需访问Web界面，请在设备唤醒后立即访问: http://%s\n", WiFi.localIP().toString().c_str());
  Serial.flush();
  
  // 在进入深度睡眠前，处理一些Web请求（最多等待5秒）
  unsigned long sleepDelay = 5000;  // 给5秒时间处理Web请求
  unsigned long startDelay = millis();
  while (millis() - startDelay < sleepDelay) {
    server.handleClient();
    delay(100);
  }
  
  delay(500);  // 确保所有输出都发送完毕
  goToSleep();
}

void loop() {
  // 处理Web服务器请求（配置模式和正常模式都处理）
  server.handleClient();
  delay(10);
  
  // 注意：正常模式下，setup函数执行完会进入深度睡眠，所以loop不会运行
  // 但在首次上电等待10分钟期间，loop会运行，可以处理web请求
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
  config.jpeg_quality = JPEG_QUALITY;  // 使用宏定义的质量值 (0-63，数值越小质量越高)
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
    // 基础图像参数
    s->set_brightness(s, 0);     // -2 to 2
    s->set_contrast(s, 0);       // -2 to 2
    s->set_saturation(s, 0);    // -2 to 2 (0=正常，负值降低饱和度，正值增加饱和度)
    
    // 确保特殊效果关闭（4=Green Tint，可能是偏绿的原因）
    s->set_special_effect(s, 0); // 0 to 6 (0-No Effect, 1-Negative, 2-Grayscale, 3-Red Tint, 4-Green Tint, 5-Blue Tint, 6-Sepia)
    
    // 白平衡设置 - 改善偏绿问题
    s->set_whitebal(s, 1);       // 0 = disable , 1 = enable (启用白平衡)
    s->set_awb_gain(s, 1);       // 0 = disable , 1 = enable (启用自动白平衡增益)
    // 白平衡模式：0-Auto, 1-Sunny(日光), 2-Cloudy(阴天), 3-Office(办公室), 4-Home(室内)
    // 使用WB_MODE宏定义的值，默认使用日光模式改善偏绿问题
    s->set_wb_mode(s, WB_MODE);
    
    // 曝光控制
    s->set_exposure_ctrl(s, 1);  // 0 = disable , 1 = enable
    s->set_aec2(s, 0);           // 0 = disable , 1 = enable (AEC2通常用于低光环境)
    s->set_ae_level(s, 0);       // -2 to 2
    s->set_aec_value(s, 300);    // 0 to 1200
    
    // 增益控制
    s->set_gain_ctrl(s, 1);      // 0 = disable , 1 = enable
    s->set_agc_gain(s, 0);       // 0 to 30
    s->set_gainceiling(s, (gainceiling_t)0);  // 0 to 6
    
    // 图像处理
    s->set_bpc(s, 0);            // 0 = disable , 1 = enable (黑像素校正)
    s->set_wpc(s, 1);            // 0 = disable , 1 = enable (白像素校正) - 保持启用
    s->set_raw_gma(s, 1);        // 0 = disable , 1 = enable (原始伽马校正)
    s->set_lenc(s, 1);           // 0 = disable , 1 = enable (镜头校正)
    s->set_dcw(s, 1);            // 0 = disable , 1 = enable (DCW - 降噪和色彩校正)
    
    // 其他设置
    s->set_hmirror(s, 0);        // 0 = disable , 1 = enable (水平镜像)
    s->set_vflip(s, 0);          // 0 = disable , 1 = enable (垂直翻转)
    s->set_colorbar(s, 0);       // 0 = disable , 1 = enable (测试条)
    
    Serial.println("相机参数配置完成");
    Serial.printf("JPEG质量: %d (0-63，数值越小质量越高)\n", JPEG_QUALITY);
    Serial.printf("预计文件大小: %s\n", JPEG_QUALITY <= 8 ? "100-150KB (高质量)" : JPEG_QUALITY <= 10 ? "80-120KB (较高质量)" : "50-80KB (标准质量)");
    const char* wbModeNames[] = {"Auto", "Sunny", "Cloudy", "Office", "Home"};
    if (WB_MODE >= 0 && WB_MODE <= 4) {
      Serial.printf("白平衡模式: %d (%s)\n", WB_MODE, wbModeNames[WB_MODE]);
    } else {
      Serial.printf("白平衡模式: %d (自定义)\n", WB_MODE);
    }
    Serial.println("提示: 如需调整质量，修改代码中的JPEG_QUALITY值 (推荐范围: 8-12)");
    Serial.flush();
  }

  // 初始化闪光灯引脚（默认关闭）
  pinMode(LED_GPIO_NUM, OUTPUT);
  digitalWrite(LED_GPIO_NUM, LOW);
  
  Serial.println("相机初始化成功");
  return true;
}

bool initSDCard() {
  Serial.println("初始化SD卡...");
  
  // 尝试挂载SD卡，如果失败则重试一次
  if (!SD_MMC.begin()) {
    Serial.println("SD卡挂载失败，尝试重新挂载...");
    delay(500);
    if (!SD_MMC.begin()) {
      Serial.println("SD卡挂载失败");
      return false;
    }
  }
  
  delay(200);  // 等待SD卡稳定

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

// 状态页面（正常模式）
void handleStatus() {
  struct tm timeinfo;
  String timeStr = "未同步";
  if (getLocalTime(&timeinfo)) {
    char buf[30];
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &timeinfo);
    timeStr = String(buf);
  }
  
  String html = "<!DOCTYPE html><html><head>";
  html += "<meta charset='UTF-8'>";
  html += "<meta name='viewport' content='width=device-width, initial-scale=1.0'>";
  html += "<title>ESP32-CAM 状态</title>";
  html += "<style>";
  html += "body { font-family: Arial, sans-serif; max-width: 600px; margin: 50px auto; padding: 20px; background: #f5f5f5; }";
  html += "h1 { color: #333; text-align: center; }";
  html += ".card { background: white; padding: 20px; border-radius: 10px; box-shadow: 0 2px 10px rgba(0,0,0,0.1); margin: 20px 0; }";
  html += ".info { margin: 10px 0; }";
  html += ".label { font-weight: bold; color: #555; }";
  html += ".value { color: #333; }";
  html += "a { display: inline-block; margin: 10px 5px; padding: 10px 20px; background: #4CAF50; color: white; text-decoration: none; border-radius: 5px; }";
  html += "a:hover { background: #45a049; }";
  html += ".warning { background: #fff3cd; padding: 15px; border-radius: 5px; margin: 20px 0; color: #856404; }";
  html += "</style></head><body>";
  html += "<h1>📷 ESP32-CAM 状态</h1>";
  html += "<div class='card'>";
  html += "<div class='info'><span class='label'>WiFi名称:</span> <span class='value'>" + wifi_ssid + "</span></div>";
  html += "<div class='info'><span class='label'>IP地址:</span> <span class='value'>" + WiFi.localIP().toString() + "</span></div>";
  html += "<div class='info'><span class='label'>当前时间:</span> <span class='value'>" + timeStr + "</span></div>";
  html += "<div class='info'><span class='label'>信号强度:</span> <span class='value'>" + String(WiFi.RSSI()) + " dBm</span></div>";
  html += "</div>";
  html += "<div class='card'>";
  html += "<h2>操作</h2>";
  html += "<a href='/photos'>📷 浏览照片</a>";
  html += "<a href='/config'>⚙️ 重新配置WiFi</a>";
  html += "<a href='/reset' onclick='return confirm(\"确定要清除WiFi配置并重启吗？\")'>🔄 清除配置并重启</a>";
  html += "</div>";
  html += "<div class='warning'>";
  html += "<strong>注意：</strong>设备每10分钟自动拍摄一张照片并进入深度睡眠。";
  html += "</div>";
  html += "</body></html>";
  
  server.send(200, "text/html; charset=UTF-8", html);
}

// 重置配置
void handleReset() {
  preferences.clear();
  preferences.end();
  
  String html = "<!DOCTYPE html><html><head>";
  html += "<meta charset='UTF-8'>";
  html += "<meta http-equiv='refresh' content='3;url=/'>";
  html += "<title>配置已清除</title>";
  html += "<style>body { font-family: Arial, sans-serif; text-align: center; padding: 50px; }</style>";
  html += "</head><body>";
  html += "<h1>配置已清除</h1>";
  html += "<p>设备将在3秒后重启...</p>";
  html += "</body></html>";
  
  server.send(200, "text/html; charset=UTF-8", html);
  delay(1000);
  ESP.restart();
}

// 照片列表页面
void handlePhotos() {
  String html = "<!DOCTYPE html><html><head>";
  html += "<meta charset='UTF-8'>";
  html += "<meta name='viewport' content='width=device-width, initial-scale=1.0'>";
  html += "<title>照片浏览</title>";
  html += "<style>";
  html += "body { font-family: Arial, sans-serif; max-width: 1200px; margin: 20px auto; padding: 20px; background: #f5f5f5; }";
  html += "h1 { color: #333; text-align: center; }";
  html += ".nav { margin: 20px 0; text-align: center; }";
  html += ".nav a, .nav button { display: inline-block; margin: 5px 10px; padding: 10px 20px; background: #4CAF50; color: white; text-decoration: none; border: none; border-radius: 5px; cursor: pointer; }";
  html += ".nav a:hover, .nav button:hover { background: #45a049; }";
  html += ".photo-list { background: white; border-radius: 10px; box-shadow: 0 2px 10px rgba(0,0,0,0.1); margin: 20px 0; overflow: hidden; }";
  html += "table { width: 100%; border-collapse: collapse; }";
  html += "th { background: #4CAF50; color: white; padding: 12px; text-align: left; font-weight: bold; }";
  html += "td { padding: 10px 12px; border-bottom: 1px solid #eee; }";
  html += "tr:hover { background: #f9f9f9; }";
  html += ".photo-name { font-family: monospace; color: #333; word-break: break-all; }";
  html += ".photo-actions { white-space: nowrap; }";
  html += ".photo-actions a { display: inline-block; margin: 0 5px; padding: 6px 12px; background: #2196F3; color: white; text-decoration: none; border-radius: 3px; font-size: 12px; }";
  html += ".photo-actions a:hover { background: #0b7dda; }";
  html += ".photo-actions a.download { background: #ff9800; }";
  html += ".photo-actions a.download:hover { background: #e68900; }";
  html += ".photo-actions a.delete { background: #f44336; }";
  html += ".photo-actions a.delete:hover { background: #d32f2f; }";
  html += ".empty { text-align: center; padding: 50px; color: #999; }";
  html += ".count { margin: 10px 0; padding: 10px; background: #e3f2fd; border-radius: 5px; color: #1976d2; }";
  html += "</style></head><body>";
  html += "<h1>📷 照片浏览</h1>";
  html += "<div class='nav'>";
  html += "<a href='/'>返回首页</a>";
  html += "<button onclick='location.reload()'>🔄 刷新</button>";
  html += "</div>";
  
  // 获取所有照片文件
  int photoCount = 0;
  
  // 开始表格
  html += "<div class='photo-list'>";
  html += "<table>";
  html += "<thead><tr><th>序号</th><th>文件名</th><th>操作</th></tr></thead>";
  html += "<tbody>";
  
  // 扫描根目录
  File root = SD_MMC.open("/");
  if (root && root.isDirectory()) {
    Serial.println("开始扫描根目录...");
    Serial.flush();
    
    File file = root.openNextFile();
    while (file) {
      String fileName = String(file.name());
      bool isDir = file.isDirectory();
      
      Serial.printf("找到: %s (目录: %s)\n", fileName.c_str(), isDir ? "是" : "否");
      Serial.flush();
      
      if (!isDir && fileName.endsWith(".jpg")) {
        // 根目录中的JPG文件
        String displayName = fileName;
        if (displayName.startsWith("/")) {
          displayName = displayName.substring(1);
        }
        
        // URL编码文件路径
        String encodedPath = urlEncode(fileName);
        
        html += "<tr>";
        html += "<td>" + String(photoCount + 1) + "</td>";
        html += "<td class='photo-name'>" + displayName + "</td>";
        html += "<td class='photo-actions'>";
        html += "<a href='/photo?file=" + encodedPath + "' target='_blank'>查看</a>";
        html += "<a href='/photo?file=" + encodedPath + "&download=1' class='download' download='" + displayName + "'>下载</a>";
        html += "<a href='/delete?file=" + encodedPath + "' class='delete' onclick='return confirm(\"确定要删除照片 " + displayName + " 吗？此操作不可恢复！\")'>删除</a>";
        html += "</td>";
        html += "</tr>";
        photoCount++;
        Serial.printf("添加根目录文件: %s\n", displayName.c_str());
        Serial.flush();
      } else if (isDir) {
        // 扫描所有目录（不仅仅是周目录）
        // 确保路径格式正确
        String dirPath = fileName;
        if (!dirPath.startsWith("/")) {
          dirPath = "/" + dirPath;
        }
        
        Serial.printf("扫描目录: %s\n", dirPath.c_str());
        Serial.flush();
        
        File dir = SD_MMC.open(dirPath.c_str());
        if (dir && dir.isDirectory()) {
          File photoFile = dir.openNextFile();
          int dirPhotoCount = 0;
          
          while (photoFile) {
            String photoName = String(photoFile.name());
            bool photoIsDir = photoFile.isDirectory();
            
            Serial.printf("  目录项: %s (目录: %s)\n", photoName.c_str(), photoIsDir ? "是" : "否");
            Serial.flush();
            
            if (!photoIsDir && photoName.endsWith(".jpg")) {
              // 构建完整路径
              String fullPath;
              // file.name() 可能返回完整路径或相对路径
              if (photoName.startsWith("/")) {
                // 已经是完整路径
                fullPath = photoName;
              } else {
                // 相对路径，需要拼接
                fullPath = dirPath;
                if (!fullPath.endsWith("/")) {
                  fullPath += "/";
                }
                // 移除photoName中可能的前导斜杠
                if (photoName.startsWith("/")) {
                  photoName = photoName.substring(1);
                }
                fullPath += photoName;
              }
              
              // 确保路径以 / 开头
              if (!fullPath.startsWith("/")) {
                fullPath = "/" + fullPath;
              }
              
              String displayName = fullPath.substring(1);
              
              // URL编码文件路径
              String encodedPath = urlEncode(fullPath);
              
              html += "<tr>";
              html += "<td>" + String(photoCount + 1) + "</td>";
              html += "<td class='photo-name'>" + displayName + "</td>";
              html += "<td class='photo-actions'>";
              html += "<a href='/photo?file=" + encodedPath + "' target='_blank'>查看</a>";
              html += "<a href='/photo?file=" + encodedPath + "&download=1' class='download' download='" + displayName + "'>下载</a>";
              html += "<a href='/delete?file=" + encodedPath + "' class='delete' onclick='return confirm(\"确定要删除照片 " + displayName + " 吗？此操作不可恢复！\")'>删除</a>";
              html += "</td>";
              html += "</tr>";
              photoCount++;
              dirPhotoCount++;
              Serial.printf("  添加目录文件: %s (完整路径: %s)\n", photoName.c_str(), fullPath.c_str());
              Serial.flush();
            }
            photoFile.close();
            photoFile = dir.openNextFile();
          }
          dir.close();
          Serial.printf("目录 %s 中找到 %d 张照片\n", dirPath.c_str(), dirPhotoCount);
          Serial.flush();
        } else {
          Serial.printf("警告: 无法打开目录 %s\n", dirPath.c_str());
          Serial.flush();
        }
      }
      file.close();
      file = root.openNextFile();
    }
    root.close();
    Serial.printf("扫描完成，共找到 %d 张照片\n", photoCount);
    Serial.flush();
    
    // 结束表格
    html += "</tbody>";
    html += "</table>";
    html += "</div>";
    
    // 显示照片总数
    if (photoCount > 0) {
      html += "<div class='count'>共找到 " + String(photoCount) + " 张照片</div>";
    } else {
      html += "<div class='empty'><p>📷 还没有照片</p><p>设备会自动拍摄照片并保存</p></div>";
    }
  } else {
    html += "</tbody>";
    html += "</table>";
    html += "</div>";
    html += "<div class='empty'><p>❌ 无法访问SD卡</p></div>";
  }
  
  html += "</body></html>";
  server.send(200, "text/html; charset=UTF-8", html);
}

// 删除照片处理函数
void handleDelete() {
  if (!server.hasArg("file")) {
    server.send(400, "text/plain", "缺少file参数");
    return;
  }
  
  // URL解码文件路径
  String filePath = urlDecode(server.arg("file"));
  
  // 确保路径以 / 开头
  if (!filePath.startsWith("/")) {
    filePath = "/" + filePath;
  }
  
  // 安全检查：防止目录遍历攻击
  if (filePath.indexOf("..") >= 0 || !filePath.endsWith(".jpg")) {
    server.send(400, "text/plain", "无效的文件路径: " + filePath);
    Serial.printf("删除失败：无效路径 %s\n", filePath.c_str());
    Serial.flush();
    return;
  }
  
  Serial.printf("尝试删除文件: %s\n", filePath.c_str());
  Serial.flush();
  
  // 检查文件是否存在
  File file = SD_MMC.open(filePath.c_str(), FILE_READ);
  if (!file) {
    Serial.printf("错误: 文件不存在 %s\n", filePath.c_str());
    Serial.flush();
    String html = "<!DOCTYPE html><html><head>";
    html += "<meta charset='UTF-8'>";
    html += "<meta http-equiv='refresh' content='2;url=/photos'>";
    html += "<title>删除失败</title>";
    html += "<style>body { font-family: Arial, sans-serif; text-align: center; padding: 50px; }";
    html += ".error { color: #f44336; }</style></head><body>";
    html += "<h1 class='error'>❌ 删除失败</h1>";
    html += "<p>文件不存在: " + filePath + "</p>";
    html += "<p>2秒后自动返回照片列表...</p>";
    html += "<a href='/photos'>立即返回</a>";
    html += "</body></html>";
    server.send(404, "text/html; charset=UTF-8", html);
    return;
  }
  
  // 确保不是目录
  if (file.isDirectory()) {
    file.close();
    Serial.printf("错误: 路径是目录而不是文件: %s\n", filePath.c_str());
    Serial.flush();
    String html = "<!DOCTYPE html><html><head>";
    html += "<meta charset='UTF-8'>";
    html += "<meta http-equiv='refresh' content='2;url=/photos'>";
    html += "<title>删除失败</title>";
    html += "<style>body { font-family: Arial, sans-serif; text-align: center; padding: 50px; }";
    html += ".error { color: #f44336; }</style></head><body>";
    html += "<h1 class='error'>❌ 删除失败</h1>";
    html += "<p>不能删除目录: " + filePath + "</p>";
    html += "<p>2秒后自动返回照片列表...</p>";
    html += "<a href='/photos'>立即返回</a>";
    html += "</body></html>";
    server.send(400, "text/html; charset=UTF-8", html);
    return;
  }
  
  file.close();
  
  // 删除文件
  if (SD_MMC.remove(filePath.c_str())) {
    Serial.printf("文件删除成功: %s\n", filePath.c_str());
    Serial.flush();
    
    String html = "<!DOCTYPE html><html><head>";
    html += "<meta charset='UTF-8'>";
    html += "<meta http-equiv='refresh' content='2;url=/photos'>";
    html += "<title>删除成功</title>";
    html += "<style>body { font-family: Arial, sans-serif; text-align: center; padding: 50px; }";
    html += ".success { color: #4CAF50; }</style></head><body>";
    html += "<h1 class='success'>✅ 删除成功</h1>";
    html += "<p>文件已删除: " + filePath + "</p>";
    html += "<p>2秒后自动返回照片列表...</p>";
    html += "<a href='/photos'>立即返回</a>";
    html += "</body></html>";
    server.send(200, "text/html; charset=UTF-8", html);
  } else {
    Serial.printf("错误: 文件删除失败 %s\n", filePath.c_str());
    Serial.flush();
    
    String html = "<!DOCTYPE html><html><head>";
    html += "<meta charset='UTF-8'>";
    html += "<meta http-equiv='refresh' content='2;url=/photos'>";
    html += "<title>删除失败</title>";
    html += "<style>body { font-family: Arial, sans-serif; text-align: center; padding: 50px; }";
    html += ".error { color: #f44336; }</style></head><body>";
    html += "<h1 class='error'>❌ 删除失败</h1>";
    html += "<p>无法删除文件: " + filePath + "</p>";
    html += "<p>可能原因：文件被占用或SD卡错误</p>";
    html += "<p>2秒后自动返回照片列表...</p>";
    html += "<a href='/photos'>立即返回</a>";
    html += "</body></html>";
    server.send(500, "text/html; charset=UTF-8", html);
  }
}

// URL编码函数
String urlEncode(String str) {
  String encoded = "";
  char c;
  for (int i = 0; i < str.length(); i++) {
    c = str.charAt(i);
    if (c == ' ') {
      encoded += "%20";
    } else if (c == '+') {
      encoded += "%2B";
    } else if (c == '/') {
      encoded += "%2F";
    } else if (c == '?') {
      encoded += "%3F";
    } else if (c == '%') {
      encoded += "%25";
    } else if (c == '#') {
      encoded += "%23";
    } else if (c == '&') {
      encoded += "%26";
    } else if (c == '=') {
      encoded += "%3D";
    } else {
      encoded += c;
    }
  }
  return encoded;
}

// URL解码函数
String urlDecode(String str) {
  String decoded = "";
  char c;
  char code0, code1;
  for (int i = 0; i < str.length(); i++) {
    c = str.charAt(i);
    if (c == '+') {
      decoded += ' ';
    } else if (c == '%') {
      if (i + 2 < str.length()) {
        code0 = str.charAt(i + 1);
        code1 = str.charAt(i + 2);
        if (isDigit(code0) && isDigit(code1)) {
          c = (code0 - '0') * 16 + (code1 - '0');
          decoded += c;
          i += 2;
        } else {
          decoded += c;
        }
      } else {
        decoded += c;
      }
    } else {
      decoded += c;
    }
  }
  return decoded;
}

// 查看/下载单个照片
void handlePhoto() {
  if (!server.hasArg("file")) {
    server.send(400, "text/plain", "缺少file参数");
    return;
  }
  
  // URL解码文件路径
  String filePath = urlDecode(server.arg("file"));
  
  // 确保路径以 / 开头
  if (!filePath.startsWith("/")) {
    filePath = "/" + filePath;
  }
  
  // 安全检查
  if (filePath.indexOf("..") >= 0 || !filePath.endsWith(".jpg")) {
    server.send(400, "text/plain", "无效的文件路径: " + filePath);
    Serial.printf("无效文件路径: %s\n", filePath.c_str());
    return;
  }
  
  Serial.printf("尝试打开文件: %s\n", filePath.c_str());
  Serial.flush();
  
  // 尝试打开文件
  File file = SD_MMC.open(filePath.c_str(), FILE_READ);
  if (!file) {
    Serial.printf("错误: 无法打开文件 %s\n", filePath.c_str());
    Serial.flush();
    server.send(404, "text/plain", "文件未找到: " + filePath);
    return;
  }
  
  if (file.isDirectory()) {
    file.close();
    Serial.printf("错误: 路径是目录而不是文件: %s\n", filePath.c_str());
    Serial.flush();
    server.send(400, "text/plain", "路径是目录: " + filePath);
    return;
  }
  
  size_t fileSize = file.size();
  Serial.printf("文件大小: %zu 字节\n", fileSize);
  Serial.flush();
  
  // 检查是否是下载请求
  if (server.hasArg("download") && server.arg("download") == "1") {
    String fileName = filePath.substring(filePath.lastIndexOf("/") + 1);
    
    // 设置响应头
    server.setContentLength(fileSize);
    server.sendHeader("Content-Type", "image/jpeg", true);
    server.sendHeader("Content-Disposition", "attachment; filename=\"" + fileName + "\"", true);
    server.sendHeader("Connection", "close", true);
    server.send(200, "image/jpeg", "");
    
    // 分块传输文件（每次4KB，ESP32-CAM内存有限）
    const size_t chunkSize = 4096;
    uint8_t* buffer = (uint8_t*)malloc(chunkSize);
    if (!buffer) {
      file.close();
      Serial.println("错误: 内存分配失败");
      Serial.flush();
      return;
    }
    
    size_t remaining = fileSize;
    size_t totalSent = 0;
    
    while (remaining > 0 && file.available()) {
      size_t toRead = (remaining > chunkSize) ? chunkSize : remaining;
      size_t bytesRead = file.read(buffer, toRead);
      
      if (bytesRead > 0) {
        size_t bytesSent = server.client().write(buffer, bytesRead);
        if (bytesSent != bytesRead) {
          Serial.printf("警告: 只发送了 %zu/%zu 字节\n", bytesSent, bytesRead);
          Serial.flush();
        }
        totalSent += bytesSent;
        remaining -= bytesRead;
      } else {
        Serial.println("错误: 无法从文件读取数据");
        Serial.flush();
        break;
      }
      
      // 每传输一定数据后刷新
      if (totalSent % (chunkSize * 4) == 0) {
        server.client().flush();
        delay(10);
      }
    }
    
    free(buffer);
    file.close();
    server.client().flush();
    Serial.printf("文件下载完成，已发送 %zu 字节\n", totalSent);
    Serial.flush();
    return;
  }
  
  // 检查是否是缩略图请求
  if (server.hasArg("thumb") && server.arg("thumb") == "1") {
    // 设置响应头
    server.setContentLength(fileSize);
    server.sendHeader("Content-Type", "image/jpeg", true);
    server.sendHeader("Cache-Control", "public, max-age=3600", true);
    server.sendHeader("Connection", "close", true);
    server.send(200, "image/jpeg", "");
    
    // 分块传输文件（每次4KB）
    const size_t chunkSize = 4096;
    uint8_t* buffer = (uint8_t*)malloc(chunkSize);
    if (!buffer) {
      file.close();
      Serial.println("错误: 内存分配失败");
      Serial.flush();
      return;
    }
    
    size_t remaining = fileSize;
    size_t totalSent = 0;
    
    while (remaining > 0 && file.available()) {
      size_t toRead = (remaining > chunkSize) ? chunkSize : remaining;
      size_t bytesRead = file.read(buffer, toRead);
      
      if (bytesRead > 0) {
        size_t bytesSent = server.client().write(buffer, bytesRead);
        if (bytesSent != bytesRead) {
          Serial.printf("警告: 只发送了 %zu/%zu 字节\n", bytesSent, bytesRead);
          Serial.flush();
        }
        totalSent += bytesSent;
        remaining -= bytesRead;
      } else {
        Serial.println("错误: 无法从文件读取数据");
        Serial.flush();
        break;
      }
      
      // 每传输一定数据后刷新
      if (totalSent % (chunkSize * 4) == 0) {
        server.client().flush();
        delay(10);
      }
    }
    
    free(buffer);
    file.close();
    server.client().flush();
    Serial.printf("缩略图传输完成，已发送 %zu 字节\n", totalSent);
    Serial.flush();
    return;
  }
  
  // 查看照片页面（返回完整图片，而不是HTML页面）
  // 设置响应头
  server.setContentLength(fileSize);
  server.sendHeader("Content-Type", "image/jpeg", true);
  server.sendHeader("Cache-Control", "public, max-age=3600", true);
  server.sendHeader("Connection", "close", true);
  server.send(200, "image/jpeg", "");
  
  // 分块传输文件（每次4KB）
  const size_t chunkSize = 4096;
  uint8_t* buffer = (uint8_t*)malloc(chunkSize);
  if (!buffer) {
    file.close();
    Serial.println("错误: 内存分配失败");
    Serial.flush();
    return;
  }
  
  size_t remaining = fileSize;
  size_t totalSent = 0;
  
  while (remaining > 0 && file.available()) {
    size_t toRead = (remaining > chunkSize) ? chunkSize : remaining;
    size_t bytesRead = file.read(buffer, toRead);
    
    if (bytesRead > 0) {
      size_t bytesSent = server.client().write(buffer, bytesRead);
      if (bytesSent != bytesRead) {
        Serial.printf("警告: 只发送了 %zu/%zu 字节\n", bytesSent, bytesRead);
        Serial.flush();
      }
      totalSent += bytesSent;
      remaining -= bytesRead;
    } else {
      Serial.println("错误: 无法从文件读取数据");
      Serial.flush();
      break;
    }
    
    // 每传输一定数据后刷新
    if (totalSent % (chunkSize * 4) == 0) {
      server.client().flush();
      delay(10);
    }
  }
  
  free(buffer);
  file.close();
  server.client().flush();
  Serial.printf("照片传输完成，已发送 %zu 字节\n", totalSent);
  Serial.flush();
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
  strftime(timeStr, sizeof(timeStr), "%Y_%m_%d_%H_%M", &timeinfo);  // 使用下划线替代冒号
  return String(timeStr);
}

// 获取ISO周数（1-53）
// ISO 8601标准：每年第一周是包含1月4日的那一周
int getWeekNumber(struct tm* timeinfo) {
  int year = timeinfo->tm_year + 1900;
  int month = timeinfo->tm_mon + 1;
  int day = timeinfo->tm_mday;
  
  // 简化的周数计算：从1月1日开始的周数
  // 更准确的方法需要计算1月1日是星期几，这里使用简化版本
  int dayOfYear = timeinfo->tm_yday;
  int week = (dayOfYear / 7) + 1;
  
  // 确保周数在合理范围内
  if (week < 1) week = 1;
  if (week > 53) week = 53;
  
  return week;
}

// 获取周目录名称，格式：YYYY_WXX
String getWeekDirectory() {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) {
    return "/unknown";
  }

  int year = timeinfo.tm_year + 1900;
  int week = getWeekNumber(&timeinfo);
  
  char dirName[20];
  snprintf(dirName, sizeof(dirName), "/%d_W%02d", year, week);
  return String(dirName);
}

// 确保目录存在，如果不存在则创建
// 使用SD_MMC库的mkdir()方法直接创建目录
bool ensureDirectoryExists(const char* dirPath) {
  // 等待SD卡稳定
  delay(200);
  
  // 先检查目录是否已存在
  File dir = SD_MMC.open(dirPath);
  if (dir && dir.isDirectory()) {
    dir.close();
    Serial.printf("目录已存在: %s\n", dirPath);
    return true;
  }
  if (dir) dir.close();
  
  // 目录不存在，使用mkdir()创建目录
  Serial.printf("正在创建目录: %s\n", dirPath);
  Serial.flush();
  
  // 尝试创建目录（最多重试3次）
  for (int i = 0; i < 3; i++) {
    if (SD_MMC.mkdir(dirPath)) {
      delay(200);  // 等待文件系统更新
      
      // 验证目录是否创建成功
      File verify = SD_MMC.open(dirPath);
      if (verify && verify.isDirectory()) {
        verify.close();
        Serial.printf("目录创建成功: %s\n", dirPath);
        return true;
      }
      if (verify) verify.close();
    }
    
    if (i < 2) {
      Serial.printf("目录创建失败，重试中 (%d/3)...\n", i + 2);
      Serial.flush();
      delay(500);  // 重试前等待
    }
  }
  
  // 如果创建失败，返回false，让程序回退到根目录
  Serial.printf("目录创建失败: %s，将使用根目录\n", dirPath);
  return false;
}

void captureAndSavePhoto() {
  Serial.println("正在拍摄照片...");
  Serial.flush();
  
  // 打开闪光灯
  pinMode(LED_GPIO_NUM, OUTPUT);
  digitalWrite(LED_GPIO_NUM, HIGH);
  Serial.println("闪光灯已打开");
  delay(100);  // 等待闪光灯稳定
  
  // 拍照
  camera_fb_t *fb = esp_camera_fb_get();
  
  // 关闭闪光灯
  digitalWrite(LED_GPIO_NUM, LOW);
  Serial.println("闪光灯已关闭");
  if (!fb) {
    Serial.println("拍照失败！");
    Serial.flush();
    return;
  }

  Serial.printf("照片大小: %zu 字节 (%.2f KB)\n", fb->len, fb->len / 1024.0);
  Serial.flush();

  // 获取周目录和时间字符串（在释放相机资源前获取，避免时间变化）
  Serial.println("正在获取周目录和时间...");
  Serial.flush();
  String weekDir = getWeekDirectory();
  String timeString = getTimeString();
  Serial.printf("周目录: %s, 时间: %s\n", weekDir.c_str(), timeString.c_str());
  Serial.flush();

  // 将照片数据复制到PSRAM缓冲区（如果可用），避免相机和SD卡资源冲突
  Serial.println("正在复制照片数据...");
  Serial.flush();
  uint8_t* imageBuffer = NULL;
  size_t imageSize = fb->len;
  bool usePSRAM = false;
  
  Serial.println("检查PSRAM...");
  Serial.flush();
  
  // 简化策略：直接使用原始缓冲区，不复制到PSRAM
  // 这样可以避免PSRAM分配可能导致的卡顿
  // 在写入SD卡时，相机资源会被占用，但写入完成后会立即释放
  Serial.println("使用原始缓冲区（不复制到PSRAM）");
  Serial.flush();
  imageBuffer = fb->buf;
  usePSRAM = false;
  
  // 注意：不在这里释放相机帧缓冲区，等写入完成后再释放
  // 这样可以避免资源冲突

  // 增加延迟，确保相机资源完全释放
  Serial.println("等待系统稳定...");
  Serial.flush();
  delay(1000);  // 增加延迟到1秒，让系统完全稳定

  // 保存到SD卡
  Serial.println("开始写入SD卡...");
  Serial.flush();
  
  // 尝试重新初始化SD卡连接，避免资源冲突
  Serial.println("重新初始化SD卡连接...");
  Serial.flush();
  SD_MMC.end();
  delay(500);
  if (!SD_MMC.begin()) {
    Serial.println("警告：SD卡重新初始化失败，继续尝试写入...");
    Serial.flush();
  }
  delay(1000);  // 等待SD卡完全准备好
  
  // 尝试创建周目录
  Serial.printf("尝试创建周目录: %s\n", weekDir.c_str());
  Serial.flush();
  bool dirCreated = ensureDirectoryExists(weekDir.c_str());
  
  // 根据目录创建结果决定使用哪个路径
  String filename;
  if (dirCreated) {
    filename = weekDir + "/" + timeString + ".jpg";
    Serial.printf("使用周目录路径: %s\n", filename.c_str());
  } else {
    filename = "/" + timeString + ".jpg";
    Serial.printf("目录创建失败，使用根目录路径: %s\n", filename.c_str());
  }
  Serial.flush();
  
  // 重试机制：最多尝试3次
  bool writeSuccess = false;
  size_t totalWritten = 0;
  int retryCount = 0;
  const int maxRetries = 3;
  const size_t chunkSize = 4096;  // 每次写入4KB，避免一次性写入大文件
  
  bool fallbackToRoot = false;  // 标记是否已回退到根目录
  
  while (!writeSuccess && retryCount < maxRetries) {
    if (retryCount > 0) {
      Serial.printf("重试写入 (第 %d 次)...\n", retryCount);
      delay(1000);  // 重试前等待更长时间
    }
    
    Serial.printf("尝试打开文件进行写入 (尝试 %d/%d)...\n", retryCount + 1, maxRetries);
    Serial.flush();
    delay(200);  // 打开文件前短暂延迟
    
    File file = SD_MMC.open(filename.c_str(), FILE_WRITE);
    if (!file) {
      // 如果使用周目录失败，且是第一次尝试，回退到根目录
      if (retryCount == 0 && !fallbackToRoot && filename.startsWith("/2026_W")) {
        Serial.printf("周目录路径写入失败（目录可能不存在或FAT32不支持自动创建）\n");
        Serial.printf("自动回退到根目录...\n");
        Serial.flush();
        filename = "/" + timeString + ".jpg";
        Serial.printf("新路径: %s\n", filename.c_str());
        Serial.flush();
        fallbackToRoot = true;
        delay(500);
        continue;  // 重试使用根目录，不增加retryCount
      }
      
      Serial.printf("错误：无法创建文件 %s\n", filename.c_str());
      retryCount++;
      if (retryCount >= maxRetries) {
        Serial.println("可能的原因：");
        Serial.println("1. 文件名包含非法字符");
        Serial.println("2. SD卡空间不足");
        Serial.println("3. SD卡文件系统错误");
        Serial.flush();
        // 释放资源
        if (fb) {
          esp_camera_fb_return(fb);
        }
        return;
      }
      delay(2000);  // 重试前等待更长时间
      continue;
    }

    // 分块写入数据，避免一次性写入大文件
    totalWritten = 0;
    size_t remaining = imageSize;
    
    while (remaining > 0) {
      size_t toWrite = (remaining > chunkSize) ? chunkSize : remaining;
      size_t written = file.write(imageBuffer + totalWritten, toWrite);
      
      if (written == 0) {
        Serial.printf("写入中断在位置: %zu\n", totalWritten);
        break;
      }
      
      totalWritten += written;
      remaining -= written;
      
      // 每写入一块后刷新，确保数据及时写入
      if (totalWritten % (chunkSize * 4) == 0) {
        file.flush();
        delay(10);  // 短暂延迟，让SD卡处理
      }
    }
    
    // 最终刷新
    file.flush();
    delay(50);  // 等待刷新完成
    
    // 检查是否写入完整
    if (totalWritten == imageSize) {
      writeSuccess = true;
    }
    
    file.close();
    delay(100);  // 关闭文件后等待
    
    if (writeSuccess) {
      Serial.printf("照片保存成功！文件大小: %zu 字节\n", totalWritten);
      break;
    } else {
      Serial.printf("警告：写入不完整！期望: %zu, 实际: %zu\n", imageSize, totalWritten);
      retryCount++;
    }
  }
  
  if (!writeSuccess) {
    Serial.println("错误：多次尝试后仍无法保存文件！");
  }
  
  // 释放相机帧缓冲区（写入完成后释放）
  if (fb) {
    esp_camera_fb_return(fb);
    Serial.println("相机帧缓冲区已释放");
  }
  
  Serial.flush();
}

// 闪光灯闪烁函数
void flashLED(int times, int duration) {
  pinMode(LED_GPIO_NUM, OUTPUT);
  digitalWrite(LED_GPIO_NUM, LOW);  // 确保初始状态为关闭
  
  for (int i = 0; i < times; i++) {
    digitalWrite(LED_GPIO_NUM, HIGH);  // 打开
    delay(duration);
    digitalWrite(LED_GPIO_NUM, LOW);   // 关闭
    if (i < times - 1) {  // 最后一次不需要延迟
      delay(duration);
    }
  }
  Serial.printf("闪光灯闪烁 %d 次完成\n", times);
  Serial.flush();
}

void goToSleep() {
  Serial.println("准备进入深度睡眠...");
  Serial.flush();
  
  // 断开Wi-Fi以节省功耗
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  Serial.println("WiFi已断开");
  Serial.flush();
  
  // 关闭SD卡
  SD_MMC.end();
  Serial.println("SD卡已关闭");
  Serial.flush();
  
  // 关闭相机
  esp_camera_deinit();
  Serial.println("相机已关闭");
  Serial.flush();
  
  delay(200);
  Serial.println("进入深度睡眠10分钟，10分钟后自动唤醒...");
  Serial.flush();
  delay(500);  // 确保所有输出都发送完毕
  
  // 进入深度睡眠
  esp_sleep_enable_timer_wakeup(SLEEP_DURATION_US);
  esp_deep_sleep_start();
}

