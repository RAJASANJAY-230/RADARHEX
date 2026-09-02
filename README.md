#  RADARHEX


### Multi-Sensor Robotic System for Disaster Rescue

RADARHEX is a multi-sensor robotic system designed to assist rescue teams in hazardous and obstructed environments. The system combines mmWave radar, vibration sensing, ultrasonic sensing and servo-based scanning under an ESP32 controller.

## 🚨 Problem

After earthquakes, structural collapses and other disasters, survivors may become trapped under rubble or inside hazardous environments. Manual searching can expose rescuers to unstable structures and difficult terrain.

## 💡 Proposed Solution

RADARHEX aims to provide a robotic sensing platform that can investigate hazardous areas and assist in identifying potential survivor locations using multiple sensing modalities.

## 🔧 Current Prototype

The current prototype includes:

- ESP32 microcontroller
- mmWave radar
- Vibration sensor
- Ultrasonic sensor
- Servo motor
- Embedded firmware
- Real-time serial monitoring

## ⚙️ System Architecture :

```text
        mmWave Radar
             │
             ▼
      Vibration Sensor
             │
             ▼
        ┌─────────┐
        │  ESP32  │
        └────┬────┘
             │
       ┌─────┴─────┐
       ▼           ▼
 Ultrasonic      Servo
  Sensor        Scanning
       │           │
       └─────┬─────┘
             ▼
      Detection Logic
             │
             ▼
       Rescue Alert
