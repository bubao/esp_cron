# CRON-like component for the ESP-IDF framework

> 中文版请见 [README_zh.md](./README_zh.md)

## Component Features

This component provides CRON-style task scheduling for the ESP-IDF framework, leveraging `esp_timer.h` for high-precision timing. It's optimized for embedded scenarios, supporting CRON syntax with second-level precision and seamless integration with ESP32/ESP8266 hardware.

## Usage Guide

The API design prioritizes simplicity, including core functions for task creation, destruction, and scheduler control. The workflow is straightforward: define at least one task, start the scheduler, then manage tasks dynamically. A smart power-saving mechanism automatically puts the scheduler into low-power mode when no tasks are scheduled, minimizing CPU overhead.

**Key Note**: Timing relies on `esp_timer` for hardware-accelerated precision, eliminating the need for system time management. Ensure proper timer initialization before creating tasks to maintain scheduling accuracy.

### Code Structure

```tree
esp-cron/
├── esp_cron.c            # Core scheduling logic & API implementation
├── include/
│   ├── esp_cron.h        # Public header exposing APIs
│   └── cron_internal.h   # Internal header for scheduler internals
├── library/
│   ├── ccronexpr/       # CRON expression parser (open-source component)
│   └── jobs/            # Task linked list management
└── examples/            # Usage examples
```

## Quick Start Guide

### Include Header

```c
#include "esp_cron.h"
```

### 1. Create a Scheduled Task

```c
cron_job *cron_job_create(const char *schedule, cron_job_callback callback, void *data)
```

- `schedule` supports CRON expressions with second-level precision:
  ```txt
    ┌────────────── second (0 - 59)
    | ┌───────────── minute (0 - 59)
    | │ ┌───────────── hour (0 - 23)
    | │ │ ┌───────────── day of month (1 - 31)
    | │ │ │ ┌───────────── month (1 - 12)
    | │ │ │ │ ┌───────────── day of week (0 - 6, Sunday=0; 7=Sunday in some systems)
    | │ │ │ │ │
    * * * * * *
  ```
- `callback` is a task handler function pointer, defined as:
  ```c
  typedef void (*cron_job_callback)(cron_job *);
  ```
  Callbacks are designed to be lightweight; the scheduler manages execution cycles automatically.
- `data` is a user-defined pointer passed to the callback with the task handle.

### Destroy a Task

Stop an existing task with:

```c
int cron_job_destroy(cron_job *job);
```

### Start the Scheduler

Launch the scheduler after defining at least one task:

```c
int cron_start();
```

### Stop the Scheduler

Shut down the scheduler:

```c
int cron_stop();
```

### Clear All Tasks

Remove all scheduled tasks:

```c
int cron_job_clear_all();
```

### Example Usage

```c
/* Initialize ESP-Timer (project-dependent, not component-required) */
esp_timer_init();

/* Create two scheduled tasks */
cron_job *jobs[2];
jobs[0] = cron_job_create("* * * * * *", on_timer_trigger, (void *)0);
jobs[1] = cron_job_create("*/5 * * * * *", on_timer_trigger, (void *)10000);

/* Start scheduler */
cron_start();

/* Simulate main program execution (replace with business logic) */
vTaskDelay(pdMS_TO_TICKS(60000));  // Run for 60 seconds

/* Cleanup resources */
cron_stop();
cron_job_clear_all();
```

Callback function example:

```c
void on_timer_trigger(cron_job *job)
{
  uint32_t task_id = (uint32_t)job->data;
  printf("Task %u triggered at %lu ms\n", task_id, esp_timer_get_time() / 1000);
}
```

## Core API Reference

### Task Management

| Function                  | Description                          |
|---------------------------|--------------------------------------|
| `cron_job* cron_job_create(const char* schedule, cron_job_callback callback, void* data);` | Create and register a task           |
| `int cron_job_destroy(cron_job* job);` | Destroy a specific task              |
| `int cron_job_clear_all();` | Clear all scheduled tasks            |

### Scheduler Control

| Function          | Description                        |
|-------------------|------------------------------------|
| `int cron_start();` | Start the task scheduler           |
| `int cron_stop();` | Stop the task scheduler            |

### Task Scheduling Operations

| Function                | Description                          |
|-------------------------|--------------------------------------|
| `int cron_job_schedule(cron_job* job);` | Manually schedule a task             |
| `int cron_job_unschedule(cron_job* job);` | Unscheduled a task                   |
| `void cron_schedule_task(void* args);` | Special scheduling (e.g., run once)  |

### Status Queries

| Function                          | Description                          |
|-----------------------------------|--------------------------------------|
| `int cron_job_load_expression(cron_job* job, const char* schedule);` | Dynamically load a CRON expression   |
| `int cron_job_has_loaded(cron_job* job);` | Check if an expression is loaded     |
| `time_t cron_job_seconds_until_next_execution();` | Get seconds until next execution     |

## Time Synchronization & Precision Optimization

### Local Timezone Configuration

For projects requiring timezone-aware time display:

1. **Timezone environment configuration**:
   ```c
   setenv("TZ", "CST-8", 1);  // Beijing Time (UTC+8)
   tzset();  // Apply timezone settings
   ```
   This affects functions like `localtime` but **does not impact scheduling precision**—tasks run based on hardware timer absolute time, independent of system timezone.
2. **SNTP time synchronization (optional)**:
   For standard time in logs or displays:
   ```c
   void init_system_time(void) {
       sntp_setoperatingmode(SNTP_OPMODE_POLL);
       sntp_setservername(0, "pool.ntp.org");  // Use domestic servers like "cn.ntp.org"
       sntp_init();

       int retry = 0;
       const int max_retry = 10;
       while (sntp_get_sync_status() != SNTP_SYNC_STATUS_COMPLETED && retry < max_retry) {
           vTaskDelay(pdMS_TO_TICKS(1000));
           retry++;
       }
       time_t now = 0;
       time(&now);
       printf("System time synced: %s", ctime(&now));
   }
   ```
   **Note**: SNTP syncs only the system time for display; scheduling relies on `esp_timer` hardware timing, unaffected by network latency.

### Precision Optimization Guide

1. **Hardware timer configuration**:
   ```c
   /* Enable high-precision timer mode (ESP32-specific) */
   esp_timer_create_args_t timer_args = {
       .callback = NULL,
       .arg = NULL,
       .flags = ESP_TIMER_FLAG_HIGH_PRECISION,  // Key: enable APB clock source
   };
   esp_timer_init(&timer_args);
   ```
2. **Scheduler parameter tuning**:
   ```c
   /* Configure minimum scheduling interval (default: 1ms, adjust for power consumption) */
   #define MIN_SCHEDULE_INTERVAL_US 100000  // 100ms interval for light sleep

   cron_start();  // Apply configuration on startup
   ```
3. **Low-power scenario adaptation**:
   ```c
   /* Combine with timer wakeup for low-power scheduling */
   esp_sleep_enable_timer_wakeup(5000000);  // Wake up every 5 seconds
   esp_light_sleep_start();
   ```
   The scheduler checks task triggers during wakeup cycles, balancing precision and power consumption.

### Why No Timezone Dependence for Scheduling?

This component uses **direct hardware timer timing** with these advantages:

1. **Precision isolation**: Triggers rely on ESP32 internal timers, unaffected by system time or timezone, with microsecond-level accuracy.
2. **No network dependency**: Stable scheduling without SNTP, ideal for offline embedded scenarios.
3. **Power optimization**: Timers work independently during CPU sleep, avoiding constant wakeups in traditional time systems.

**Actual role of timezone configuration**: Only affects time display formats via functions like `localtime`, fully decoupled from scheduling logic.

## Acknowledgments

- [esp_cron](https://github.com/DavidMora/esp_cron) by David Mora Rodriguez
- [ccronexpr](https://github.com/staticlibs/ccronexpr) expression parser

## License

This project is licensed under the [Apache License 2.0](http://www.apache.org/licenses/LICENSE-2.0).

> English documentation: This page is in English. 中文文档请见 [README_zh.md](./README_zh.md)
