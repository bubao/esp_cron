#include <Arduino.h>

  #include "esp_cron.h"


static void cron_callback(cron_job* arg) {
  const char* msg = (const char*)arg->data;
  Serial.printf("[callback] Cron job triggered! arg = %s\n", msg);
}

void setup() {
  Serial.begin(115200);
  while (!Serial) { delay(10); }

  static const char* msg = "Hello ESP-CRON";

  // Initialize ESP timer (needed by esp_cron internally)
  esp_timer_init();

  // Create a job that triggers every minute ("* * * * * *")
  cron_job_create("* * * * * *", cron_callback, (void*)msg);

  // Start the cron scheduler
  cron_start();

  Serial.println("esp_cron example started. Waiting for cron job...");
}

void loop() {
  // Keep main loop alive; cron runs in background
  delay(1000);
}
