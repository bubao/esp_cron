# esp_cron – CRON-style Scheduler for ESP32 (Arduino Port)

This is an **Arduino-compatible port** of the original  
**CRON-like component for ESP-IDF**, created by **Bubao**:  
https://github.com/bubao/esp_cron

It enables ESP32 boards to schedule repeated actions using **CRON expressions** with **second-level resolution**, built on top of `esp_timer` and FreeRTOS.timer

This port preserves the original logic while adapting it for the Arduino build system.

---

## ✨ Features

- Full CRON syntax including **seconds**
- High-precision scheduling using `esp_timer`
- Supports multiple concurrent cron jobs
- Independent of system clock, SNTP or timezone
- Lightweight callbacks
- Works on ESP32, ESP32-S2, ESP32-S3, ESP32-C3 (Arduino Core)

---

## 📦 Credits (Upstream Authors)

This Arduino library is based on:

### **Original ESP-IDF Component**
- **Author:** Bubao  
- **Repository:** https://github.com/bubao/esp_cron  
- **License:** Apache 2.0

### **CRON Expression Parser**
- **Project:** ccronexpr  
- **Repository:** https://github.com/staticlibs/ccronexpr  
- **License:** Apache 2.0

All upstream copyright and licenses are preserved.

---

## 📁 Library Structure (Arduino Format)

```
esp_cron/
├── library.properties
└── src/
    ├── esp_cron.c
    ├── esp_cron.h
    ├── jobs.c
    ├── jobs.h
    ├── ccronexpr.c
    └── ccronexpr.h
```

---

## 🔧 Installation

### **A) Arduino IDE**
1. Download this repository as ZIP  
2. Open Arduino IDE → **Sketch → Include Library → Add .ZIP Library…**  
3. Select ZIP  
4. Done

### **B) PlatformIO**

Add to `platformio.ini`:

```ini
lib_deps = <your-github-username>/esp_cron
```

---

## 🚀 Quick Start Example

```cpp
#include <esp_cron.h>

void myJob(cron_job_struct *job) {
  Serial.println("Cron job executed!");
}

void setup() {
  Serial.begin(115200);
  delay(500);

  // Execute every 5 seconds
  cron_job_create("*/5 * * * * *", myJob, NULL);
  cron_start();
}

void loop() {}
```

---

## 🧩 API Overview

### **Create a Job**
```c
cron_job_struct* cron_job_create(const char* expr,
                                 cron_job_callback callback,
                                 void* data);
```

### **Start Scheduler**
```c
int cron_start();
```

### **Stop Scheduler**
```c
int cron_stop();
```

### **Destroy a Job**
```c
int cron_job_destroy(cron_job_struct* job);
```

### **Clear All Jobs**
```c
int cron_job_clear_all();
```

---

## ⏱ CRON Syntax (with seconds)

```
┌──── second (0–59)
│ ┌── minute (0–59)
│ │ ┌── hour (0–23)
│ │ │ ┌── day of month (1–31)
│ │ │ │ ┌── month (1–12)
│ │ │ │ │ ┌── day of week (0–6)
* * * * * *
```

### Examples

| Expression       | Meaning                  |
|------------------|--------------------------|
| `*/5 * * * * *`  | Every 5 seconds          |
| `0 */1 * * * *`  | Every minute at second 0 |
| `0 0 8 * * *`    | Every day at 08:00:00    |

---

## ⚡ Notes

- Scheduling is based on **esp_timer**, not system time  
- CRON jobs run accurately even without WiFi or NTP  
- Callbacks should be short and non-blocking  
- This library does **not** modify or depend on timezone settings  

---

## 🙏 Acknowledgements

Special thanks to:

- [esp_cron](https://github.com/bubao/esp_cron) by bubao, for the original ESP-IDF esp_cron component  
- [esp_cron](https://github.com/DavidMora/esp_cron) by David Mora Rodriguez
- [ccronexpr](https://github.com/staticlibs/ccronexpr) expression parser

---

## 📄 License

This project is licensed under the **Apache License 2.0**.  
See `LICENSE.txt` for details.

