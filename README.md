# React-o-mat

![React-o-mat Logo](logo.jpg)

**React-o-mat** is an interactive, multi-node ESP32-based arcade game system utilizing ESP-NOW for fast, wireless communication. It consists of a "Master" controller and multiple "Slave" nodes to create engaging reaction and memory games.

## Hardware Setup

The system is built around the **ESP32-C3 SuperMini** microcontroller.

### Master Node
- **Microcontroller**: ESP32-C3 SuperMini
- **Display**: SSD1306 OLED Display (I2C: SDA=8, SCL=9)
- **LEDs**: 12x NeoPixel Ring (Data Pin 10)
- **Sensor**: VL6180X Time-of-Flight distance sensor (I2C: SDA=8, SCL=9)
- **Input**: Analog Joystick (X: GPIO 0, Y: GPIO 1, Button: GPIO 2)

### Slave Node(s)
- **Microcontroller**: ESP32-C3 SuperMini
- **LEDs**: 12x NeoPixel Ring (Data Pin 10)
- **Sensor**: VL6180X Time-of-Flight distance sensor (I2C: SDA=8, SCL=9)

## Features & Game Modes

Navigate through the OLED menu using the joystick to select different game modes:

- **Speed Run**: A pure reaction time test. Wait for the signal and trigger your sensor as fast as possible!
- **Whack-A-Mole**: The classic arcade game. Nodes will light up randomly; you have to trigger their sensors to score points. Includes Normal and Advanced modes with adjustable time strings and speed levels. Watch out for penalty colors!
- **Senso**: A memory sequence game (similar to Simon Says). Watch the sequence of colors on the nodes and recreate it.
- **Disco**: Party mode! All connected nodes flash random colors.
- **Distance Test**: A utility screen to test and calibrate the VL6180X distance sensors.

## Architecture & Communication

- **Protocol**: The nodes communicate using the low-latency **ESP-NOW** protocol on WiFi channel 0/1. 
- **Topology**: Star topology with the Master node acting as the central hub.
- **Addressing**: The Master operates on a fixed base MAC address (`AC:A7:04:AF:82:A4`). Slaves send unicast messages to this MAC address.
- **Presence Detection**: Slaves send a heartbeat every 500ms. The Master automatically registers new slaves and drops inactive slaves after a 10-second timeout.
- **Non-blocking Execution**: The code features asynchronous logic for I2C display updates (150-200ms) and continuous distance sensor readings to ensure zero interference with ESP-NOW interrupts.

## Installation & Setup

1. **Libraries Required**:
   - `Adafruit_GFX`
   - `Adafruit_NeoPixel`
   - `Adafruit_SSD1306`
   - `VL6180X` (by Pololu)

2. **Master Upload**: Open `master/master.ino` in the Arduino IDE, select the ESP32-C3 board, and upload it to the Master microcontroller.
3. **Slave Upload**: Open `slave/slave.ino` in the Arduino IDE and upload it to as many Slave microcontrollers as you want.
4. **Power Up**: Turn on the Master node first, followed by the Slave nodes. The Master's OLED screen will display the number of connected Slaves dynamically.
