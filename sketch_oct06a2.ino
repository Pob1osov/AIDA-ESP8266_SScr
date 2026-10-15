/*
 * ============================================================
 * ESP8266 硬件监控副屏 (AIDA64 RemoteSensor 专用版)
 * 屏幕: 1.3寸 SH1106 (I2C)
 * 版本: 正式发布版 (持久长连接 + 防死机 + 自动重连)
 * ============================================================
 * 使用说明:
 * 1. 在 AIDA64 中开启 RemoteSensor，端口设为 888。
 * 2. 在 AIDA64 的 LCD 项目中，创建 Simple1 到 Simple11 共11个项目。
 * 3. 修改下方“用户配置区”的 WiFi 账号、密码和电脑 IP。
 * 4. 烧录后，必须拔掉开发板，彻底关闭 Arduino IDE，再插回机箱 USB 供电。
 * ============================================================
 */

#include <ESP8266WiFi.h>
#include <U8g2lib.h>
#include <Wire.h>

// ================= 用户配置区 =================
const char* ssid = "WiFi_Name";               // 替换为你的 WiFi 名称 (仅支持2.4G)
const char* password = "WiFi_Password";       // 替换为你的 WiFi 密码
const char* host = "IPv4_Adress of your client"; // 替换为电脑的局域网 IP (cmd -> ipconfig 查看)
const int port = 888;                         // AIDA64 RemoteSensor 端口
// =============================================

U8G2_SH1106_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, /* reset=*/ U8X8_PIN_NONE);
WiFiClient client;
unsigned long lastTime = 0;
unsigned long lastPrintTime = 0;

unsigned long lastDataTime = 0;   
bool isScreenOn = true;           
bool hasPlayedBootAnimation = false; 
int connectFailCount = 0; // 记录连续连接失败次数，用于重置状态

// 存储解析出来的数据
String strDate = "--/--/----";
String strTime = "--:--:--";
String strCpuTemp = "N/A";
String strGpuTemp = "N/A";
String strCpuLoad = "N/A";
String strGpuLoad = "N/A";
String strDdr = "N/A";
String strGdr = "N/A";
String strTdp = "N/A";
String strFreq = "N/A";
String strVolt = "N/A";

// ================= 自定义像素图标 (PROGMEM 存于 Flash，节省内存) =================
const char bitmap_CPU[11][16] PROGMEM = {
  "011000010000110", "111111111111111", "110001000101011", "010111010101010", "010111010101010",
  "110111010101011", "010111000101010", "010111011101010", "010001011100011", "111111111111111", "011000010000110"
};
const char bitmap_GPU[11][16] PROGMEM = {
  "011111111111110", "111111111111111", "110001000101011", "110111010101011", "110111010101011",
  "110101010101011", "110101000101011", "110101011101011", "110001011100011", "111111111111111", "011101111111000"
};
const char bitmap_DDR[11][16] PROGMEM = {
  "111111111111111", "100000000000001", "101100110011001", "101010101010101", "101010101010101",
  "101010101011001", "101010101010101", "101100110010101", "100000000000001", "111111111111111", "011111101111110"
};
const char bitmap_GDR[11][16] PROGMEM = {
  "011111111111110", "100000000000001", "101110110011001", "101000101010101", "101000101010101",
  "101010101011001", "101010101010101", "101010101010101", "101110110010101", "100000000000001", "011111111111110"
};

void setup() {
  Serial.begin(115200);
  u8g2.begin();
  
  playWiFiConnectingAnimation();

  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_6x10_tf);
  u8g2.setCursor(12, 20);
  u8g2.print("WiFi CONNECTED!");
  u8g2.sendBuffer();
  delay(1500);
  
  lastDataTime = millis();
}

void loop() {
  // 每1秒执行一次数据抓取 (配合SSE长连接)
  if (millis() - lastTime > 1000) {
    lastTime = millis();
    fetchData();
    
    if (isScreenOn) {
      displayData();
    }
  }
  
  // 每10秒输出一次诊断日志 (内存、WiFi信号、连接状态)
  if (millis() - lastPrintTime > 10000) {
    lastPrintTime = millis();
    Serial.print("FreeHeap: "); Serial.print(ESP.getFreeHeap());
    Serial.print(" | RSSI: "); Serial.print(WiFi.RSSI());
    Serial.print(" | AIDA64: "); Serial.println(client.connected() ? "OK" : "DISCONNECTED");
  }
  
  // 待机息屏控制 (1分钟无数据自动息屏保护)
  if (isScreenOn && (millis() - lastDataTime > 60000)) {
    u8g2.setPowerSave(1); 
    isScreenOn = false;
    Serial.println("Screen Timeout");
  }
}

// ================= 核心网络逻辑 (持久长连接) =================
void fetchData() {
  if (WiFi.status() != WL_CONNECTED) {
    resetToNA();
    return;
  }

  // 1. 仅在未连接时才尝试建立连接 (杜绝每秒断开重连的致命缺陷)
  if (!client.connected()) {
    if (client.connect(host, port)) {
      // 发送 SSE 长连接请求，保持 keep-alive
      client.print("GET /sse HTTP/1.1\r\nHost: ");
      client.print(host);
      client.print("\r\nAccept: text/event-stream\r\nConnection: keep-alive\r\n\r\n");
      connectFailCount = 0;
      Serial.println("Connected to AIDA64 Server!");
    } else {
      connectFailCount++;
      Serial.print("Connect failed! Count: "); Serial.println(connectFailCount);
      if (connectFailCount > 3) {
        resetToNA();
        hasPlayedBootAnimation = false; // 连续连不上，重置状态等下次连接
        connectFailCount = 0;
      }
      return; 
    }
  }

  // 2. 已连接状态下，持续读取数据
  if (client.connected()) {
    bool gotData = false;

    // 非阻塞读取，有数据就读，没数据立刻跳出
    while (client.available()) {
      String line = client.readStringUntil('\n');
      line.trim();
      
      if (line.startsWith("data: ")) {
        String data = line.substring(6); 
        
        while (data.indexOf("{|}") > -1) {
          int endIdx = data.indexOf("{|}");
          String item = data.substring(0, endIdx); 
          
          int pipeIdx = item.indexOf('|');
          if (pipeIdx > 0) {
            String id = item.substring(0, pipeIdx);
            String value = item.substring(pipeIdx + 1);

            if (id == "Simple1") strGdr = value;
            if (id == "Simple2") strDdr = value;
            if (id == "Simple3") strCpuTemp = value;
            if (id == "Simple4") strDate = value;
            if (id == "Simple5") strTime = value;
            if (id == "Simple6") strTdp = value;
            if (id == "Simple7") strFreq = value;
            if (id == "Simple8") strVolt = value;
            if (id == "Simple9") strGpuLoad = value;
            if (id == "Simple10") strCpuLoad = value;
            if (id == "Simple11") strGpuTemp = value;
            
            gotData = true;
          }
          data = data.substring(endIdx + 3);
        }
      }
      // 【防死机关键】加 delay(1)，让出 CPU 时间给 WiFi 底层，防止看门狗复位
      delay(1); 
    }

    // 3. 数据清洗与状态更新
    if (gotData) {
      lastDataTime = millis(); 
      
      // 提取温度纯数字，强行拼 `C (解决各种编码乱码)
      String cleanCpuTemp = "";
      for (int i = 0; i < strCpuTemp.length(); i++) {
        if (isDigit(strCpuTemp[i]) || strCpuTemp[i] == '.') cleanCpuTemp += strCpuTemp[i];
      }
      if (cleanCpuTemp != "") strCpuTemp = cleanCpuTemp + "`C"; else strCpuTemp = "N/A";

      String cleanGpuTemp = "";
      for (int i = 0; i < strGpuTemp.length(); i++) {
        if (isDigit(strGpuTemp[i]) || strGpuTemp[i] == '.') cleanGpuTemp += strGpuTemp[i];
      }
      if (cleanGpuTemp != "") strGpuTemp = cleanGpuTemp + "`C"; else strGpuTemp = "N/A";

      strDdr.replace("DDR ", ""); strDdr.trim();
      strGdr.replace("GDR ", ""); strGdr.trim();

      // 电压保留两位小数
      if (strVolt != "" && strVolt != "N/A") {
        String rawVolt = "";
        for (int i = 0; i < strVolt.length(); i++) {
          if (isDigit(strVolt[i]) || strVolt[i] == '.') rawVolt += strVolt[i];
        }
        if (rawVolt != "") {
          float v = rawVolt.toFloat();
          strVolt = String(v, 2); 
        } else {
          strVolt = "0.00";
        }
      } else {
        strVolt = "N/A";
      }

      // 频率一律除以1000转为GHz
      String rawFreq = "";
      for (int i = 0; i < strFreq.length(); i++) {
        if (isDigit(strFreq[i]) || strFreq[i] == '.') rawFreq += strFreq[i];
      }
      if (rawFreq != "") {
        float f = rawFreq.toFloat();
        f = f / 1000.0; 
        strFreq = String(f, 1); 
      } else {
        strFreq = "0.0";
      }
      
      // 从息屏唤醒并显示数据
      if (!isScreenOn) {
        u8g2.setPowerSave(0); 
        isScreenOn = true;
        displayData(); 
      }
      
      // 电脑刚开机/刚启动AIDA64时，播放一次开机动画
      if (!hasPlayedBootAnimation) {
        playSystemBootAnimation(); 
        hasPlayedBootAnimation = true; 
        displayData(); 
      }
    } else {
      // 如果 30 秒没有任何数据，主动断开，防止假死
      if (millis() - lastDataTime > 30000) {
        Serial.println("No data for 30s, reconnecting...");
        client.stop();
        resetToNA();
      }
    }
  }
}

// ================= 辅助函数 =================

void resetToNA() {
  strDate = "--/--/----";
  strTime = "--:--:--";
  strCpuTemp = "N/A"; strGpuTemp = "N/A";
  strCpuLoad = "N/A"; strGpuLoad = "N/A";
  strDdr = "N/A"; strGdr = "N/A";
  strTdp = "N/A"; strFreq = "N/A"; strVolt = "N/A";
}

void playSystemBootAnimation() {
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_7x13B_tf); 
  u8g2.setCursor(15, 20);
  u8g2.print("SYSTEM ONLINE");
  
  u8g2.drawFrame(14, 35, 100, 12);
  for (int i = 0; i <= 96; i += 4) {
    u8g2.drawBox(16, 37, i, 8); 
    u8g2.sendBuffer();
    delay(15); 
  }
  delay(400); 
}

void playWiFiConnectingAnimation() {
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_6x10_tf);
  u8g2.setCursor(15, 20);
  u8g2.print("Connecting WiFi");
  
  u8g2.drawFrame(14, 35, 100, 10);
  for (int i = 0; i <= 96; i += 6) {
    u8g2.drawBox(16, 37, i, 6);
    u8g2.sendBuffer();
    delay(20);
  }
}

void drawCustomIcon(int x, int y, const char bitmap[11][16]) {
  for (int row = 0; row < 11; row++) {
    for (int col = 0; col < 15; col++) {
      if (pgm_read_byte(&bitmap[row][col]) == '1') {
        u8g2.drawPixel(x + col, y + row);
      }
    }
  }
}

// ================= 完美 UI 显示函数 =================
void displayData() {
  u8g2.clearBuffer();
  
  // 第1行：日期 + 时间
  u8g2.setFont(u8g2_font_7x13B_tf); 
  u8g2.setCursor(0, 11);
  u8g2.print(strDate);

  u8g2.setFont(u8g2_font_7x13B_tf); 
  u8g2.setCursor(81, 11);
  u8g2.print(strTime);

  // 第2行：CPU
  drawCustomIcon(0, 16, bitmap_CPU);
  u8g2.setFont(u8g2_font_6x10_tf); 
  u8g2.setCursor(20, 27); 
  u8g2.print(strCpuTemp + " " + strCpuLoad); 

  u8g2.setFont(u8g2_font_5x7_tf); 
  u8g2.setCursor(78, 27); 
  u8g2.print(strFreq + "@" + strVolt);

  // 第3行：GPU
  drawCustomIcon(0, 32, bitmap_GPU);
  u8g2.setFont(u8g2_font_6x10_tf); 
  u8g2.setCursor(20, 43);
  u8g2.print(strGpuTemp + " " + strGpuLoad); 

  u8g2.setFont(u8g2_font_5x7_tf); 
  u8g2.setCursor(77, 43);
  u8g2.print(strTdp);

  // 第4行：DDR / GDR
  drawCustomIcon(0, 48, bitmap_DDR);
  drawCustomIcon(78, 48, bitmap_GDR);

  u8g2.setFont(u8g2_font_6x10_tf); 
  u8g2.setCursor(20, 59); 
  u8g2.print(strDdr); 
  
  u8g2.setCursor(98, 59); 
  u8g2.print(strGdr); 

  u8g2.sendBuffer();
}
