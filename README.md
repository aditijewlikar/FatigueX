# FatigueX - IoT Safety Helmet

FatigueX is an IoT-enabled safety helmet designed for industrial environments to monitor worker fatigue, predict microsleep, and track cardiovascular health in real-time. It uses an ESP32 microcontroller, a MAX30102 heart rate sensor, an MPU6050 accelerometer/gyroscope, and an IR sensor for blink detection.

## Project Structure

This repository is split into two distinct versions to preserve the original deployment while showcasing the completely redesigned, modern architecture:

*   **`/V1`**: Contains the legacy/initial deployed version of the site (static HTML/JS dashboard).
*   **`/V2`**: Contains the vastly updated, redesigned full-stack architecture built using Antigravity. This features a beautiful dark-mode Tailwind UI, a robust Node.js/Express backend, high-speed WebSocket streaming, and a dedicated multi-threaded FreeRTOS C++ program for the ESP32.

## How to Run

### Previewing V1 (Legacy)
1. Navigate to the `/V1` directory.
2. Open `index.html` or `single_worker.html` directly in any web browser. 

### Running V2 (Next-Gen Antigravity)
1. Navigate to `/V2/backend`.
2. Run `npm install` to install the required Node.js dependencies.
3. Run `npm start` (or `node server.js`) to boot the WebSocket server.
4. Navigate your browser to `http://localhost:3000` to view the live dashboard.
5. Upload `/V2/Latest_code4/Latest_code4.ino` to your ESP32 via the Arduino IDE to begin streaming live biometrics to the dashboard.
