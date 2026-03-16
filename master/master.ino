#include <Adafruit_GFX.h>
#include <Adafruit_NeoPixel.h>
#include <Adafruit_SSD1306.h>
#include <VL6180X.h>
#include <WiFi.h>
#include <Wire.h>
#include <esp_now.h>

#define LED_DATA 10
#define I2C_SDA 8
#define I2C_SCL 9
#define JOY_X 0
#define JOY_Y 1
#define JOY_BT 2
#define SLAVE_TIMEOUT 10000

Adafruit_SSD1306 display(128, 64, &Wire, -1);
Adafruit_NeoPixel ring(12, LED_DATA, NEO_GRB + NEO_KHZ800);
VL6180X vl = VL6180X();
bool sensorOK = false;

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

uint8_t slaveList[10][6];
unsigned long lastSeen[10];
uint8_t activeSlaves = 0;
unsigned long lastUpdate = 0;
uint16_t lastRange = 8190;

typedef struct {
  uint8_t msgType; // 1: Sync, 2: Hit, 3: GameStart, 4: GameOver
  uint8_t slaveID;
  uint8_t totalCount;
  uint8_t gameMode;
  uint8_t status;   // 0: Idle, 1: Countdown, 2: GO, 3: Result
  uint8_t winnerID; // 0xFF: No one, 0: Master, 1+: Slaves
  uint8_t targetID; // 0: Master, 1+: Slaves, 0xFF: None
  uint8_t
      targetColor; // 1: Green, 2: Blue, 3: Yellow, 4: Red, 5: Pink (Forbidden)
} struct_msg;

struct_msg outMsg;
struct_msg inMsg;

enum GameState {
  MENU,
  SETTINGS,
  SPEED_RUN,
  WHACK_A_MOLE,
  DISTANCE_TEST,
  DISCO,
  SENSO
};
GameState currentState = MENU;
enum GameSubState { LOBBY, COUNTDOWN, ACTIVE, RESULT };
GameSubState subState = LOBBY;

int menuSelection = 0;
bool masterIsPlayer = true;
unsigned long gameTimer = 0;
uint8_t winnerID = 0xFF;
long reactionTime = 0;
int whackScore = 0;
int whackMode = 0;      // 0: Normal, 1: Advanced
int whackSpeed = 3;     // 1-5
int lobbySelection = 0; // 0: Mode, 1: Time, 2: Speed
unsigned long moleTimer = 0;
unsigned long moleDuration = 0;
unsigned long whackGameDuration = 30000; // Default 30s
bool moleActive = false;
bool penaltyActive = false;
uint8_t currentMoleColor = 0;
uint8_t currentMoleID = 0xFF;
unsigned long lastWhackHit = 0;
unsigned long whackHitID = 0xFF; // 0=Master, 1+=Slaves
unsigned long penaltyTimer = 0;
unsigned long lastJoyMove = 0;

// SENSO Variables
int sensoDifficulty = 3;
uint8_t sensoSequence[100];
int sensoSeqLength = 1;
int sensoStep = 0;
bool sensoShowingSequence = true;
unsigned long sensoTimer = 0;
unsigned long sensoShowDuration = 1000;
unsigned long sensoGapDuration = 250;
bool sensoIsGap = false;

// Calibration
int joyXCenter = 2048;
int joyYCenter = 2048;
const int deadzone = 500; // Lowered to 500 for better responsiveness

void OnDataRecv(const esp_now_recv_info *info, const uint8_t *data, int len) {
  const uint8_t *mac = info->src_addr;

  if (len == 1 && data[0] == 0) { // Heartbeat
    int idx = -1;
    for (int i = 0; i < activeSlaves; i++)
      if (memcmp(slaveList[i], mac, 6) == 0)
        idx = i;

    if (idx == -1 && activeSlaves < 10) {
      memcpy(slaveList[activeSlaves], mac, 6);
      esp_now_peer_info_t peer = {};
      memcpy(peer.peer_addr, mac, 6);
      peer.channel = 0;
      peer.encrypt = false;
      if (esp_now_add_peer(&peer) == ESP_OK) {
        lastSeen[activeSlaves] = millis();
        activeSlaves++;
      }
    } else if (idx != -1) {
      lastSeen[idx] = millis();
    }
    return;
  }

  if (len == sizeof(struct_msg)) {
    memcpy(&inMsg, data, sizeof(inMsg));
    if (inMsg.msgType == 2 && currentState == SPEED_RUN && subState == ACTIVE) {
      winnerID = inMsg.slaveID;
      reactionTime = millis() - gameTimer;
      subState = RESULT;
    } else if (inMsg.msgType == 2 && currentState == WHACK_A_MOLE &&
               subState == ACTIVE) {
      if (moleActive && inMsg.slaveID == currentMoleID && !penaltyActive) {
        if (currentMoleColor == 5 && whackMode == 1) { // Pink hit!
          penaltyActive = true;
          penaltyTimer = millis();
          moleActive = false;
          winnerID = 0xFE; // Broadcast penalty flash
        } else {
          whackScore++;
          moleActive = false;
          lastWhackHit = millis();
          whackHitID = inMsg.slaveID;
          moleTimer = millis(); // Implement gap by resetting timer
        }
      }
    } else if (inMsg.msgType == 2 && currentState == SENSO && subState == ACTIVE) {
      if (!sensoShowingSequence) {
        if (inMsg.slaveID == sensoSequence[sensoStep]) {
          sensoStep++;
          if (sensoStep >= sensoSeqLength) { // Round won
            sensoSeqLength++;
            sensoStep = 0;
            sensoShowingSequence = true;
            sensoIsGap = true;
            int minID = masterIsPlayer ? 0 : 1;
            sensoSequence[sensoSeqLength - 1] = activeSlaves==0 && minID==1 ? 0xFF : random(minID, activeSlaves + 1);
            sensoTimer = millis(); 
            sensoGapDuration = 1000;
            currentMoleID = 0xFF; // Used as visual toggle
          }
        } else {
          winnerID = 0xFE;
          subState = RESULT;
        }
      } else {
        winnerID = 0xFE; // Hit during show phase = lose
        subState = RESULT;
      }
    }
  }
}

void setup() {
  Wire.begin(I2C_SDA, I2C_SCL);
  display.begin(SSD1306_SWITCHCAPVCC, 0x3C);
  ring.begin();
  outMsg.winnerID = 0xFF; // Initially no winner
  pinMode(JOY_BT, INPUT_PULLUP);
  WiFi.mode(WIFI_STA);
  esp_now_init();
  esp_now_register_recv_cb(OnDataRecv);

  // Calibration
  display.clearDisplay();
  display.setTextColor(WHITE);
  display.setCursor(0, 0);
  display.println("CALIBRATING...");
  display.println("Keep Joystick neutral");
  display.display();

  long xSum = 0, ySum = 0;
  for (int i = 0; i < 50; i++) {
    xSum += analogRead(JOY_X);
    ySum += analogRead(JOY_Y);
    delay(10);
  }
  joyXCenter = xSum / 50;
  joyYCenter = ySum / 50;

  vl.init();
  vl.configureDefault();
  vl.setTimeout(500);
  vl.writeReg(0x024, 83);
  vl.startRangeContinuous(30); // Start continuous non-blocking mode (30ms)
  sensorOK = true;
  randomSeed(analogRead(3));
}

void loop() {
  unsigned long now = millis();

  // Timeout-Check
  for (int i = 0; i < activeSlaves; i++) {
    if (now - lastSeen[i] > SLAVE_TIMEOUT) {
      esp_now_del_peer(slaveList[i]);
      for (int j = i; j < activeSlaves - 1; j++) {
        memcpy(slaveList[j], slaveList[j + 1], 6);
        lastSeen[j] = lastSeen[j + 1];
      }
      activeSlaves--;
    }
  }

  // Menu/Joystick Logic - Non-blocking
  int yVal = analogRead(JOY_Y);
  int xVal = analogRead(JOY_X);
  bool btn = (digitalRead(JOY_BT) == LOW);

  if (now - lastJoyMove > 150) { // Sped up to 150ms
    if (currentState == MENU) {
      if (yVal < joyYCenter - deadzone) {
        menuSelection = (menuSelection == 0) ? 5 : menuSelection - 1;
        lastJoyMove = now;
      } else if (yVal > joyYCenter + deadzone) {
        menuSelection = (menuSelection + 1) % 6;
        lastJoyMove = now;
      }
    } else if (currentState == WHACK_A_MOLE && subState == LOBBY) {
      if (yVal < joyYCenter - deadzone) {
        lobbySelection = (lobbySelection - 1 + 3) % 3;
        lastJoyMove = now;
      } else if (yVal > joyYCenter + deadzone) {
        lobbySelection = (lobbySelection + 1) % 3;
        lastJoyMove = now;
      }
      if (xVal < joyXCenter - deadzone) {
        if (lobbySelection == 0)
          whackMode = (whackMode + 1) % 2;
        else if (lobbySelection == 1 && whackGameDuration > 30000)
          whackGameDuration -= 30000;
        else if (lobbySelection == 2 && whackSpeed > 1)
          whackSpeed--;
        lastJoyMove = now;
      } else if (xVal > joyXCenter + deadzone) {
        if (lobbySelection == 0)
          whackMode = (whackMode + 1) % 2;
        else if (lobbySelection == 1 && whackGameDuration < 300000)
          whackGameDuration += 30000;
        else if (lobbySelection == 2 && whackSpeed < 5)
          whackSpeed++;
        lastJoyMove = now;
      }
    } else if (currentState == SENSO && subState == LOBBY) {
      if (xVal < joyXCenter - deadzone) {
        if (sensoDifficulty > 1) sensoDifficulty--;
        lastJoyMove = now;
      } else if (xVal > joyXCenter + deadzone) {
        if (sensoDifficulty < 5) sensoDifficulty++;
        lastJoyMove = now;
      }
    } else if (currentState == SETTINGS) {
      if (yVal < joyYCenter - deadzone) {
        masterIsPlayer = true;
        lastJoyMove = now;
      } else if (yVal > joyYCenter + deadzone) {
        masterIsPlayer = false;
        lastJoyMove = now;
      }
    }
  }

  // Button handling - simple debounce
  static bool lastBtnState = false;
  bool btnClicked = (btn && !lastBtnState);
  lastBtnState = btn;

  if (btnClicked) {
    if (currentState == MENU) {
      if (menuSelection == 0)
        currentState = SPEED_RUN;
      else if (menuSelection == 1)
        currentState = WHACK_A_MOLE;
      else if (menuSelection == 2)
        currentState = SENSO;
      else if (menuSelection == 3)
        currentState = DISCO;
      else if (menuSelection == 4)
        currentState = DISTANCE_TEST;
      else
        currentState = SETTINGS;
      subState = LOBBY;
      outMsg.winnerID = 0xFF;
      lobbySelection = 0; // Reset lobby cursor
    } else if (currentState == SPEED_RUN) {
      if (subState == LOBBY) {
        subState = COUNTDOWN;
        gameTimer = now;
        winnerID = 0xFF;
      } else if (subState == RESULT) {
        currentState = MENU;
      }
    } else if (currentState == WHACK_A_MOLE) {
      if (subState == LOBBY) {
        subState = COUNTDOWN;
        gameTimer = now;
        whackScore = 0;
        penaltyTimer = 0;
        penaltyActive = false;
        winnerID = 0xFF;
      } else if (subState == RESULT) {
        currentState = MENU;
      }
    } else if (currentState == SENSO) {
      if (subState == LOBBY) {
        subState = COUNTDOWN;
        gameTimer = now;
        winnerID = 0xFF;
        sensoSeqLength = 1;
        sensoStep = 0;
        int minID = masterIsPlayer ? 0 : 1;
        sensoSequence[0] = activeSlaves==0 && minID==1 ? 0xFF : random(minID, activeSlaves + 1);
        sensoShowingSequence = true;
        sensoIsGap = true;
        sensoGapDuration = 1000;
        sensoTimer = now;
      } else if (subState == RESULT) {
        currentState = MENU;
      }
    } else if (currentState == SETTINGS) {
      currentState = MENU;
    } else if (subState == RESULT || currentState == DISTANCE_TEST ||
               currentState == DISCO) {
      currentState = MENU;
    }
  }

  // Common State Logic (Timer based but outside display block)
  if (currentState == WHACK_A_MOLE && subState == ACTIVE) {
    unsigned long gameElapsed = now - gameTimer;
    if (gameElapsed >= whackGameDuration) {
      subState = RESULT;
      moleActive = false;
    }

    if (penaltyActive && now - penaltyTimer > 3000) {
      penaltyActive = false;
      winnerID = 0xFF;
      moleTimer = now;
    }

    if (!moleActive && !penaltyActive) {
      int cooldown = 600 - (whackSpeed * 100);
      if (now - moleTimer > (cooldown > 100 ? cooldown : 100)) {
        int minID = masterIsPlayer ? 0 : 1;
        if (minID == 1 && activeSlaves == 0) {
          currentMoleID = 0xFF; // No one to show moles to
        } else {
          currentMoleID = random(minID, activeSlaves + 1);
        }
        currentMoleColor = (whackMode == 0) ? random(1, 4) : random(1, 6);
        moleTimer = now;
        moleDuration =
            random(2000 - (whackSpeed * 300), 4000 - (whackSpeed * 600));
        moleActive = true;
      }
    } else if (moleActive && now - moleTimer > moleDuration) {
      moleActive = false;
      moleTimer = now; // Implement gap
    }
  } else if (currentState == SENSO && subState == ACTIVE) {
    if (sensoShowingSequence) {
      if (sensoIsGap) {
        if (now - sensoTimer > sensoGapDuration) {
          sensoIsGap = false;
          sensoTimer = now;
          // Calculate show time based on difficulty (1 = slow, 5 = fast)
          sensoShowDuration = 1000 - (sensoDifficulty * 150);
          if (sensoShowDuration < 200) sensoShowDuration = 200;
        }
      } else {
        if (now - sensoTimer > sensoShowDuration) {
          sensoStep++;
          if (sensoStep >= sensoSeqLength) {
            sensoShowingSequence = false;
            sensoStep = 0;
          } else {
            sensoIsGap = true;
            sensoGapDuration = 250;
            sensoTimer = now;
          }
        }
      }
    }
  }

  if (now - lastUpdate > 80) {
    lastUpdate = now;
    display.clearDisplay();
    display.setTextColor(WHITE);

    if (currentState == MENU) {
      display.setCursor(0, 0);
      display.printf("Slaves: %d [%s]", activeSlaves,
                     masterIsPlayer ? "P" : "R");
      display.setCursor(0, 16);
      display.println(menuSelection == 0 ? "> SPEED RUN" : "  SPEED RUN");
      display.println(menuSelection == 1 ? "> WHACK-A-MOLE" : "  WHACK-A-MOLE");
      display.println(menuSelection == 2 ? "> SENSO" : "  SENSO");
      display.println(menuSelection == 3 ? "> DISCO" : "  DISCO");
      display.println(menuSelection == 4 ? "> DISTANCE TEST" : "  DISTANCE TEST");
      display.println(menuSelection == 5 ? "> SETTINGS" : "  SETTINGS");

      ring.clear();
      if (activeSlaves == 0) {
        float pulse = (sin(now / 800.0) * 50.0) + 60.0;
        ring.setPixelColor(0, ring.Color((uint8_t)pulse, 0, 0));
      } else {
        int currentPos = (now / 150) % 12;
        for (int i = 0; i < activeSlaves + (masterIsPlayer ? 1 : 0); i++) {
          ring.setPixelColor((currentPos + i) % 12, ring.Color(0, 150, 0));
        }
      }
    } else if (currentState == SETTINGS) {
      display.setCursor(0, 0);
      display.println("--- SETTINGS ---");
      display.setCursor(0, 20);
      display.printf("%s Master: PLAYER\n", masterIsPlayer ? ">" : " ");
      display.printf("%s Master: REMOTE\n", !masterIsPlayer ? ">" : " ");
    } else if (currentState == SPEED_RUN) {
      if (subState == LOBBY) {
        display.setCursor(0, 0);
        display.println("SPEED RUN: Lobby");
        display.setCursor(0, 20);
        display.println("Press Button to START");
      } else if (subState == COUNTDOWN) {
        int elapsed = (now - gameTimer) / 1000;
        display.setCursor(40, 20);
        display.setTextSize(3);
        display.print(3 - elapsed);
        if (elapsed >= 3) {
          subState = ACTIVE;
          gameTimer = now;
        }
        // LED ring countdown: 12 LEDs turn off over 3 seconds (250ms per LED)
        int ledsOn = 12 - ((now - gameTimer) / 250);
        if (ledsOn < 0)
          ledsOn = 0;
        ring.clear();
        for (int i = 0; i < ledsOn; i++) {
          ring.setPixelColor(i, ring.Color(150, 50, 0)); // Orange
        }
      } else if (subState == ACTIVE) {
        display.setCursor(40, 20);
        display.setTextSize(3);
        display.println("GO");
        ring.fill(ring.Color(0, 150, 0)); // Green
      } else if (subState == RESULT) {
        display.setTextSize(1);
        display.setCursor(0, 0);
        display.println("WINNER!");
        display.setTextSize(2);
        display.setCursor(0, 20);
        if (winnerID == 0)
          display.print("MASTER");
        else
          display.printf("SLAVE %d", winnerID);
        display.setTextSize(1);
        display.setCursor(0, 45);
        display.printf("Time: %ld ms", reactionTime);

        // Flashing logic for Master winner
        if (winnerID == 0) {
          if ((now / 200) % 2 == 0) {
            ring.fill(ring.Color(255, 255, 255)); // White
          } else {
            ring.fill(ring.Color(0, 255, 0)); // Green
          }
        } else {
          ring.fill(ring.Color(0, 0, 150)); // Static Blue for other winners
        }
      }
    } else if (currentState == WHACK_A_MOLE) {
      if (subState == LOBBY) {
        display.setCursor(0, 0);
        display.println("WHACK-A-MOLE: Lobby");
        display.setCursor(0, 16);
        display.printf("%s Mode: %s\n", lobbySelection == 0 ? ">" : " ",
                       whackMode == 0 ? "Normal" : "Advanced");
        display.printf("%s Time: %ds\n", lobbySelection == 1 ? ">" : " ",
                       whackGameDuration / 1000);
        display.printf("%s Speed: Lvl %d\n", lobbySelection == 2 ? ">" : " ",
                       whackSpeed);
        display.println("Press Button to START");
      } else if (subState == COUNTDOWN) {
        int elapsed = (now - gameTimer) / 1000;
        display.setCursor(40, 20);
        display.setTextSize(3);
        display.print(3 - elapsed);
        if (elapsed >= 3) {
          subState = ACTIVE;
          gameTimer = now;
          moleActive = false;
        }
        int ledsOn = 12 - ((now - gameTimer) / 250);
        if (ledsOn < 0)
          ledsOn = 0;
        ring.clear();
        for (int i = 0; i < ledsOn; i++) {
          ring.setPixelColor(i, ring.Color(150, 50, 0));
        }
      } else if (subState == ACTIVE) {
        unsigned long gameElapsed = now - gameTimer;
        if (gameElapsed >= whackGameDuration) {
          subState = RESULT;
          moleActive = false;
        } else {
          display.setTextSize(1);
          display.setCursor(0, 0);
          display.printf("Time: %ds", (whackGameDuration - gameElapsed) / 1000);

          display.setCursor(0, 20);
          display.print("Score: ");
          display.setTextSize(2);
          display.print(whackScore);
          display.setTextSize(1);

          if (penaltyActive) {
            if (now - penaltyTimer > 3000) {
              penaltyActive = false;
              winnerID = 0xFF;
              moleTimer = now; // Resume distribution
            }
          }

          if (now - lastWhackHit < 250 && whackHitID == 0) {
            ring.fill(ring.Color(255, 255, 255)); // Master Hit Flash
          } else if (penaltyActive) {
            if ((now / 200) % 2 == 0)
              ring.fill(ring.Color(255, 0, 0));
            else
              ring.clear();
          } else if (moleActive && currentMoleID == 0) {
            uint32_t colors[] = {0,
                                 ring.Color(0, 255, 0),
                                 ring.Color(0, 0, 255),
                                 ring.Color(255, 255, 0),
                                 ring.Color(255, 0, 0),
                                 ring.Color(255, 20, 147)};
            ring.fill(colors[currentMoleColor]);
          } else {
            ring.clear();
          }
        }

        if (btn && !(moleActive && currentMoleID == 0 && masterIsPlayer)) {
          subState = RESULT;
          moleActive = false;
          delay(300);
        }
      } else if (subState == RESULT) {
        display.setCursor(0, 0);
        display.println("GAME OVER");
        display.setTextSize(2);
        display.setCursor(0, 20);
        display.printf("Score: %d", whackScore);
        display.setTextSize(1);
        display.setCursor(0, 45);
        display.println("Press Button to EXIT");

        if (winnerID == 0xFE && now - penaltyTimer < 3000) {
          // Penalty Flash RED
          if ((now / 200) % 2 == 0)
            ring.fill(ring.Color(255, 0, 0));
          else
            ring.clear();
        } else {
          ring.setBrightness(50);           // Optional: temp lower for dim red
          ring.fill(ring.Color(100, 0, 0)); // Dim Red
          ring.setBrightness(255);
        }
      }
    } else if (currentState == SENSO) {
      if (subState == LOBBY) {
        display.setCursor(0, 0);
        display.println("SENSO: Lobby");
        display.setCursor(0, 16);
        display.printf("> Difficulty: Lvl %d\n", sensoDifficulty);
        display.println("\nAssigning Colors...");
        
        ring.clear();
        int minID = masterIsPlayer ? 0 : 1;
        for(int i = minID; i <= activeSlaves; i++) {
           if(i == 0) {
             ring.fill(getSensoColor(0, false));
           }
        }
      } else if (subState == COUNTDOWN) {
        int elapsed = (now - gameTimer) / 1000;
        display.setCursor(40, 20);
        display.setTextSize(3);
        display.print(3 - elapsed);
        if (elapsed >= 3) {
          subState = ACTIVE;
        }
        int ledsOn = 12 - ((now - gameTimer) / 250);
        if (ledsOn < 0) ledsOn = 0;
        ring.clear();
        for (int i = 0; i < ledsOn; i++) ring.setPixelColor(i, ring.Color(150, 50, 0));
      } else if (subState == ACTIVE) {
        display.setTextSize(1);
        display.setCursor(0, 0);
        display.printf("Score: %d", sensoSeqLength - 1);
        
        ring.clear();
        if (sensoShowingSequence) {
          display.setCursor(0, 20);
          display.println("WATCH SEQUENCE");
          if (!sensoIsGap) {
             if (sensoSequence[sensoStep] == 0) {
                 ring.fill(getSensoColor(0, false));
             }
          }
        } else {
          display.setCursor(0, 20);
          display.println("YOUR TURN");
          int minID = masterIsPlayer ? 0 : 1;
          if (now - lastWhackHit < 250) {
             ring.fill(ring.Color(255, 255, 255)); // Flash White
          } else if (minID == 0) {
             ring.fill(getSensoColor(0, true)); // Dim base color when waiting
          }
        }
        
        if (btn && !sensoShowingSequence && masterIsPlayer) {
          if (0 == sensoSequence[sensoStep]) {
            sensoStep++;
            lastWhackHit = now; // Flash white
            if (sensoStep >= sensoSeqLength) { // Round won
              sensoSeqLength++;
              sensoStep = 0;
              sensoShowingSequence = true;
              sensoIsGap = true;
              int minID = masterIsPlayer ? 0 : 1;
              sensoSequence[sensoSeqLength - 1] = activeSlaves==0 && minID==1 ? 0xFF : random(minID, activeSlaves + 1);
              sensoTimer = millis(); 
              sensoGapDuration = 1000;
              currentMoleID = 0xFF; // visual toggle
            }
          } else {
            winnerID = 0xFE;
            subState = RESULT;
          }
          delay(300);
        }
      } else if (subState == RESULT) {
        display.setCursor(0, 0);
        display.println("GAME OVER");
        display.setTextSize(2);
        display.setCursor(0, 20);
        display.printf("Score: %d", sensoSeqLength - 1);
        display.setTextSize(1);
        display.setCursor(0, 45);
        display.println("Press Button to EXIT");
        
        if ((now / 200) % 2 == 0)
          ring.fill(ring.Color(255, 0, 0));
        else
          ring.clear();
      }
    } else if (currentState == DISTANCE_TEST) {
      display.setCursor(0, 0);
      display.println("--- DISTANCE TEST ---");
      if (sensorOK) {
        display.setCursor(0, 20);
        display.setTextSize(2);
        display.printf("%d mm", lastRange);
        display.setTextSize(1);
        display.setCursor(0, 45);
        display.println("Press Button to EXIT");
        ring.clear();
        int ledCount = lastRange / 20; // 1 LED per 20mm
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
      } else {
        display.println("SENSOR ERROR!");
      }
    } else if (currentState == DISCO) {
      display.setCursor(0, 0);
      display.println("--- DISCO MODE ---");
      display.setCursor(0, 20);
      display.println("pArTy mOdE...");
      display.println("Click to STOP");

      int cycle = (now / 150) % 6;
      uint32_t discoColors[] = {
          ring.Color(255, 0, 0),   ring.Color(0, 255, 0),
          ring.Color(0, 0, 255),   ring.Color(255, 255, 0),
          ring.Color(255, 0, 255), ring.Color(0, 255, 255)};
      ring.fill(discoColors[cycle]);
    }

    display.display();
    ring.show();
  }

  // Sensor reading - Non-blocking (continuous mode)
  if (sensorOK &&
      (currentState == SPEED_RUN || currentState == WHACK_A_MOLE ||
       currentState == DISTANCE_TEST || currentState == SENSO)) {
    // Result ready? Bit 2 of 0x04F is Range Result Ready
    if (vl.readReg(0x04F) & 0x04) {
      lastRange = vl.readRangeContinuousMillimeters();
    }
  }

  // Hit detection logic - Reactive to non-blocking range
  if (currentState == SPEED_RUN && subState == ACTIVE && masterIsPlayer) {
    if ((sensorOK && lastRange < 50) || btn) {
      winnerID = 0;
      reactionTime = now - gameTimer;
      subState = RESULT;
    }
  } else if (currentState == WHACK_A_MOLE && subState == ACTIVE &&
             moleActive && currentMoleID == 0 && masterIsPlayer &&
             !penaltyActive) {
    if ((sensorOK && lastRange < 50) || btn) {
      if (currentMoleColor == 5 && whackMode == 1) {
        penaltyActive = true;
        penaltyTimer = now;
        winnerID = 0xFE;
      } else {
        whackScore++;
        lastWhackHit = now;
        whackHitID = 0;
        moleTimer = now; // Implement gap
      }
      moleActive = false;
    }
  } else if (currentState == SENSO && subState == ACTIVE && masterIsPlayer) {
    if ((sensorOK && lastRange < 50 && now - lastWhackHit > 1000) || (btn && !sensoShowingSequence)) { // Reuse lastWhackHit for debounce
      if (!sensoShowingSequence) {
        if (sensoSequence[sensoStep] == 0) {
          sensoStep++;
          lastWhackHit = now;
          if (sensoStep >= sensoSeqLength) {
            sensoSeqLength++;
            sensoStep = 0;
            sensoShowingSequence = true;
            sensoIsGap = true;
            int minID = masterIsPlayer ? 0 : 1;
            sensoSequence[sensoSeqLength - 1] = activeSlaves==0 && minID==1 ? 0xFF : random(minID, activeSlaves + 1);
            sensoTimer = millis();
            sensoGapDuration = 1000;
            currentMoleID = 0xFF; // visual toggle
          }
        } else {
          winnerID = 0xFE;
          subState = RESULT;
        }
        lastWhackHit = now;
      } else if (lastRange < 50) { // Accidentally hit during show phase
        winnerID = 0xFE;
        subState = RESULT;
      }
    }
  }

  // Broadcast Sync - Throttled 50ms
  static unsigned long lastBroadcast = 0;
  if (now - lastBroadcast > 50) {
    lastBroadcast = now;
    outMsg.msgType = 1;
    outMsg.totalCount = activeSlaves + (masterIsPlayer ? 1 : 0);
    outMsg.gameMode = currentState;
    outMsg.status = subState;
    outMsg.winnerID =
        (subState == RESULT) ? winnerID : (penaltyActive ? 0xFE : 0xFF);
        
    if (currentState == WHACK_A_MOLE && moleActive) {
      outMsg.targetID = currentMoleID;
      outMsg.targetColor = currentMoleColor;
    } else if (currentState == SENSO && subState == ACTIVE) {
      outMsg.targetID = sensoIsGap ? 0xFF : (sensoShowingSequence ? sensoSequence[sensoStep] : 0xFE); 
      // 0xFE TargetID means "waiting for input". 0xFF means gap/off.
      outMsg.targetColor = 0; 
    } else {
      outMsg.targetID = 0xFF;
      outMsg.targetColor = 0;
    }
    for (int i = 0; i < activeSlaves; i++) {
        outMsg.slaveID = i + 1;
        esp_now_send(slaveList[i], (uint8_t *)&outMsg, sizeof(outMsg));
    }
  }
}