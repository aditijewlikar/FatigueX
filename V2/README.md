# FatigueX V2 (Next-Gen)

This directory contains the completely redesigned architecture for the FatigueX platform, featuring a robust Node.js backend, a high-performance FreeRTOS ESP32 payload, and a stunning Tailwind CSS frontend.

### Tech Stack
*   **Hardware / IoT (ESP32)**: C++, FreeRTOS (Dual-core task management), HTTPClient, ArduinoJson, MAX30105 library (SparkFun).
*   **Backend**: Node.js, Express.js, `ws` (WebSockets) for sub-100ms real-time data streaming, MongoDB (optional local storage).
*   **Frontend**: HTML5, Vanilla JavaScript, Tailwind CSS (via CDN) for modern, responsive UI design. FontAwesome for iconography.

### Features
*   **Non-Blocking Hardware**: Uses FreeRTOS on the ESP32 to offload Wi-Fi transmissions to Core 0, allowing Core 1 to sample the MAX30102 heart rate sensor uninterrupted at 200 Hz.
*   **Real-Time Dashboard**: Node.js WebSocket server broadcasts biometrics instantly to the browser, updating live UI components (BPM, Pitch, Roll, Eye Status) every 100ms.
*   **Simulation Engine**: Includes an autonomous background simulation engine that realistically models 10 fleet workers to demonstrate the platform at scale alongside the live ESP32 hardware.

### Setup Instructions
1. Navigate to `/V2/backend`.
2. Run `npm install` to install Express, ws, and other dependencies.
3. Run `node server.js` to start the backend.
4. Visit `http://localhost:3000` to interact with the dashboard.
5. Flash `/V2/Latest_code4/Latest_code4.ino` to your ESP32 board. Ensure you update the `ssid`, `password`, and `backendUrl` variables at the top of the file to match your local network configuration.
