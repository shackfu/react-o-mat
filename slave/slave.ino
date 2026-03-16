#include <Adafruit_NeoPixel.h>
#include <VL6180X.h>
#include <WiFi.h>
#include <Wire.h>
#include <esp_now.h>

#define LED_DATA 10
#define I2C_SDA 8
#define I2C_SCL 9

Adafruit_NeoPixel ring(12, LED_DATA, NEO_GRB + NEO_KHZ800);
VL6180X vl;

uint32_t getSensoColor(uint8_t id, bool dim = false) {
  int div = dim ? 8 : 1;
  switch (id % 6) {
    case 0: return ring.Color(255/div, 0, 0);   // Red
    case 1: return ring.Color(0, 255/div, 0);   // Green
    case 2: return ring.Color(0, 0, 255/div);   // Blue
    case 3: return ring.Color(255/div, 255/div, 0); // Yellow
    case 4: return ring.Color(255/div, 0, 255/div); // Magenta/Pink
    case 5: return ring.Color(0, 255/div, 255/div); // Cyan
  }
  return 0;
}

typedef struct {
  uint8_t msgType; // 1: Sync, 2: Hit
  uint8_t slaveID;
  uint8_t totalCount;
  uint8_t gameMode;
  uint8_t status;
  uint8_t winnerID;
  uint8_t targetID;
  uint8_t targetColor;
} struct_msg;

struct_msg inMsg;
struct_msg outMsg;

uint8_t masterMAC[] = {0xAC, 0xA7, 0x04, 0xAF, 0x82, 0xA4};
unsigned long lastRecv = 0;
unsigned long lastHeartbeat = 0;
unsigned long countdownStart = 0;
unsigned long lastHitTime = 0;
bool sensorOK = false;

void OnDataRecv(const esp_now_recv_info *info, const uint8_t *data, int len) {
  if (len == sizeof(struct_msg)) {
    memcpy(&inMsg, data, sizeof(inMsg));
    lastRecv = millis();
  }
}

void setup() {
  ring.begin();
  WiFi.mode(WIFI_STA);
  esp_now_init();
  esp_now_register_recv_cb(OnDataRecv);

  esp_now_peer_info_t peer = {};
  memcpy(peer.peer_addr, masterMAC, 6);
  peer.channel = 0;
  peer.encrypt = false;
  esp_now_add_peer(&peer);

  Wire.begin(I2C_SDA, I2C_SCL);
  vl.init();
  vl.configureDefault();
  vl.setTimeout(500);
  sensorOK = true;
}

void loop() {
  unsigned long now = millis();

  // Heartbeat an Master
  if (now - lastHeartbeat > 500) {
    uint8_t heartbeat = 0;
    esp_now_send(masterMAC, &heartbeat, 1);
    lastHeartbeat = now;
  }

  // --- Visual & Game Logic ---
  ring.clear();

  if (now - lastRecv > 2500) {
    // ROT bei Verbindungsverlust
    float pulse = (sin(now / 800.0) * 80.0) + 90.0;
    ring.fill(ring.Color((uint8_t)pulse, 0, 0));
  } else if (inMsg.gameMode == 0 ||
             inMsg.gameMode == 1) { // MENU (0) oder SETTINGS (1)
    int currentPos = (now / 150) % 12;
    for (int i = 0; i < inMsg.slaveID; i++) {
      ring.setPixelColor((currentPos + i) % 12, ring.Color(0, 150, 0));
    }
  } else if (inMsg.gameMode == 2) { // SPEED RUN
    if (inMsg.status == 1) {        // COUNTDOWN
      if (countdownStart == 0)
        countdownStart = now;
      // LED ring countdown: 12 LEDs turn off over 3 seconds (250ms per LED)
      int ledsOn = 12 - ((now - countdownStart) / 250);
      if (ledsOn < 0)
        ledsOn = 0;
      for (int i = 0; i < ledsOn; i++) {
        ring.setPixelColor(i, ring.Color(150, 50, 0)); // Orange
      }
    } else if (inMsg.status == 2) {     // ACTIVE / GO
      countdownStart = 0;               // Reset
      ring.fill(ring.Color(0, 150, 0)); // Green

      // Sensor Abfrage
      if (sensorOK) {
        uint16_t range = vl.readRangeSingleMillimeters();
        if (range < 50) {     // Hand nah dran (<5cm)
          outMsg.msgType = 2; // HIT
          outMsg.slaveID = inMsg.slaveID;
          esp_now_send(masterMAC, (uint8_t *)&outMsg, sizeof(outMsg));
        }
      }
    } else if (inMsg.status == 3) { // RESULT
      if (inMsg.winnerID == inMsg.slaveID) {
        // Winning slave flashes
        if ((now / 200) % 2 == 0) {
          ring.fill(ring.Color(255, 255, 255)); // White
        } else {
          ring.fill(ring.Color(0, 0, 255)); // Blue
        }
      } else {
        ring.fill(ring.Color(0, 0, 50)); // Dim blue for losers
      }
    }
  } else if (inMsg.gameMode == 3) { // WHACK-A-MOLE
    if (inMsg.status == 1) {        // COUNTDOWN
      if (countdownStart == 0)
        countdownStart = now;
      int ledsOn = 12 - ((now - countdownStart) / 250);
      if (ledsOn < 0)
        ledsOn = 0;
      for (int i = 0; i < ledsOn; i++) {
        ring.setPixelColor(i, ring.Color(150, 50, 0));
      }
    } else if (inMsg.status == 2) { // ACTIVE
      countdownStart = 0;
      if (inMsg.winnerID == 0xFE) { // Penalty Red Flash
        if ((now / 200) % 2 == 0)
          ring.fill(ring.Color(255, 0, 0));
        else
          ring.clear();
      } else if (inMsg.targetID == inMsg.slaveID) {
        uint32_t colors[] = {0,
                             ring.Color(0, 255, 0),
                             ring.Color(0, 0, 255),
                             ring.Color(255, 255, 0),
                             ring.Color(255, 0, 0),
                             ring.Color(255, 20, 147)};
        ring.fill(colors[inMsg.targetColor]);

        if (sensorOK) {
          uint16_t range = vl.readRangeSingleMillimeters();
          if (range < 50 && now - lastHitTime > 1000) {
            outMsg.msgType = 2; // HIT
            outMsg.slaveID = inMsg.slaveID;
            esp_now_send(masterMAC, (uint8_t *)&outMsg, sizeof(outMsg));
            lastHitTime = now;
          }
        }
      } else if (now - lastHitTime < 250) {
        ring.fill(ring.Color(255, 255, 255)); // Flash White
      }
    } else if (inMsg.status == 3) { // RESULT
      if (inMsg.winnerID == 0xFE) { // penalty flash
        if ((now / 200) % 2 == 0)
          ring.fill(ring.Color(255, 0, 0));
        else
          ring.clear();
      } else {
        ring.fill(ring.Color(80, 0, 0)); // Dim Red
      }
    }
  } else if (inMsg.gameMode == 4) { // DISTANCE TEST
    if (sensorOK) {
      uint16_t range = vl.readRangeSingleMillimeters();
      int ledCount = range / 20; // 1 LED per 20mm
      if (ledCount > 12)
        ledCount = 12;

      for (int i = 0; i < ledCount; i++) {
        if (i < 4)
          ring.setPixelColor(i, ring.Color(0, 150, 0)); // Green
        else if (i < 8)
          ring.setPixelColor(i, ring.Color(150, 150, 0)); // Yellow
        else
          ring.setPixelColor(i, ring.Color(150, 0, 0)); // Red
      }
    }
  } else if (inMsg.gameMode == 5) { // DISCO
    int cycle = (now / 150) % 6;
    uint32_t discoColors[] = {ring.Color(255, 0, 0),   ring.Color(0, 255, 0),
                              ring.Color(0, 0, 255),   ring.Color(255, 255, 0),
                              ring.Color(255, 0, 255), ring.Color(0, 255, 255)};
    ring.fill(discoColors[cycle]);
  } else if (inMsg.gameMode == 6) { // SENSO
    if (inMsg.status == 0) { // LOBBY
      ring.fill(getSensoColor(inMsg.slaveID, false));
    } else if (inMsg.status == 1) { // COUNTDOWN
      if (countdownStart == 0)
        countdownStart = now;
      int ledsOn = 12 - ((now - countdownStart) / 250);
      if (ledsOn < 0) ledsOn = 0;
      for (int i = 0; i < ledsOn; i++) {
        ring.setPixelColor(i, ring.Color(150, 50, 0));
      }
    } else if (inMsg.status == 2) { // ACTIVE
      countdownStart = 0;
      // Slave locally flashes white for 250ms when it detects a sensor hit during its turn
      if (inMsg.targetID == inMsg.slaveID || inMsg.targetID == 0xFE) {
        if (now - lastHitTime < 250) {
          ring.fill(ring.Color(255, 255, 255)); // Flash White immediately after hit
        } else if (inMsg.targetID == inMsg.slaveID) {
          ring.fill(getSensoColor(inMsg.slaveID, false)); // We are showing sequence
        } else if (inMsg.targetID == 0xFE) { // Waiting for input
          ring.fill(getSensoColor(inMsg.slaveID, true)); // Dim base color wait
          if (sensorOK) {
            uint16_t range = vl.readRangeSingleMillimeters();
            if (range < 50 && now - lastHitTime > 1000) {
              outMsg.msgType = 2; // HIT
              outMsg.slaveID = inMsg.slaveID;
              esp_now_send(masterMAC, (uint8_t *)&outMsg, sizeof(outMsg));
              lastHitTime = now;
            }
          }
        }
      } else if (now - lastHitTime < 250) {
        ring.fill(ring.Color(255, 255, 255)); // Flash White on hit detection
      } else {
        ring.clear(); // Gap or not targeted
      }
    } else if (inMsg.status == 3) { // RESULT
      if (inMsg.winnerID == 0xFE) {
        if ((now / 200) % 2 == 0)
          ring.fill(ring.Color(255, 0, 0));
        else
          ring.clear();
      } else {
        ring.clear();
      }
    }
  }

  ring.show();
  delay(20);
}