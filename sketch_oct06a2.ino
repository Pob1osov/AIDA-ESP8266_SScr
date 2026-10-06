#include <U8g2lib.h>
#include <Wire.h>

U8G2_SH1106_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, /* reset=*/ U8X8_PIN_NONE);

void setup() {
  Serial.begin(9600); // 必须和AIDA64设置一致
  u8g2.begin();
  u8g2.setFont(u8g2_font_6x10_tf);
}

void loop() {
  if (Serial.available()) {
    String rawData = "";
    // 读取所有可用的字符，限制长度防止卡死
    while (Serial.available() && rawData.length() < 50) {
      char c = Serial.read();
      // 把不可见字符转成十六进制，方便查看
      if (c < 32 || c > 126) {
        rawData += "[";
        rawData += String(c, HEX);
        rawData += "]";
      } else {
        rawData += c;
      }
      delay(2); // 稍微等待数据接收
    }
    
    // 显示到屏幕上
    u8g2.clearBuffer();
    u8g2.setCursor(0, 12);
    u8g2.print("Received:");
    u8g2.setCursor(0, 28);
    u8g2.print(rawData);
    u8g2.sendBuffer();
  }
}
