#include <WiFi.h>
#include <esp_now.h>
#include <Wire.h>
#include <Adafruit_SSD1306.h>
#include <esp_wifi.h>

#define I2C_SDA 8
#define I2C_SCL 9
Adafruit_SSD1306 display(128, 64, &Wire, -1);

void setup() {
  Wire.begin(I2C_SDA, I2C_SCL);
  display.begin(SSD1306_SWITCHCAPVCC, 0x3C);
  
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP("react-o-mat", NULL, 1); // Kanal 1 festlegen
  
  // WICHTIG: Kanal am WiFi-Stack explizit setzen
  esp_wifi_set_promiscuous(true);
  esp_wifi_set_channel(1, WIFI_SECOND_CHAN_NONE);
  esp_wifi_set_promiscuous(false);

  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(WHITE);
  display.setCursor(0,0);
  display.println("CHECK DIESE MAC:");
  display.setCursor(0,20);
  display.println(WiFi.softAPmacAddress()); // Wir brauchen die AP-MAC!
  display.setCursor(0,40);
  display.print("Kanal: "); display.println(WiFi.channel());
  display.display();

  esp_now_init();
}
void loop() {}