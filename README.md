# ESP-Cron: CRON-like Task Scheduler for ESP-IDF

> [中文版](./README_zh.md)

## Overview

ESP-Cron is a lightweight, CRON-style task scheduler for ESP-IDF, built on `esp_timer` and FreeRTOS. It parses standard 6-field CRON expressions (seconds, minutes, hours, day, month, weekday) and schedules callbacks with second-level precision.

**Key characteristics:**
- Scheduling computes next trigger times from the system clock (`time()`)
- `esp_timer` merely waits for the computed delay — it does **not** determine "what time is it"
- Thread-safe with reference counting (safe `cron_job_destroy` even while jobs are queued)
- Smart low-power: timer stops automatically when no jobs are scheduled
- Per-job runner task — no shared thread pool, no task-pool complexity

**You must synchronize system time (SNTP) before creating jobs.** Otherwise, schedules are computed against the 1970 epoch.

---

## What's Inside

| Document | Purpose |
|----------|---------|
| **This README** | Usage guide, API reference, configuration, gotchas |
| [docs/DESIGN.md](./docs/DESIGN.md) | Architecture, thread-safety model, lifecycle contracts, what NOT to do |

---

## Project Structure

```
esp_cron/
├── esp_cron.c                  # Core scheduler (esp_timer + FreeRTOS queue + refcount)
├── include/
│   ├── esp_cron.h              # Public header (include this one)
│   └── cron.h                  # Internal API + cron_job struct definition
├── library/
│   ├── ccronexpr/              # Third-party CRON expression parser
│   └── jobs/                   # Sorted linked list management
├── Kconfig                     # Build-time configuration
├── examples/
│   └── esp_cron_example/       # Complete runnable example
├── test/                       # Unity test cases (test_cron.c)
├── test_apps/                  # Runnable test app (see "Running the Tests")
└── docs/
    └── DESIGN.md               # Architecture & lifecycle contracts (start here)
```

---

## Prerequisites

1. **System time must be valid.** Use SNTP (or RTC on chips with battery backup) to set the clock before calling `cron_job_create`.

2. **Correct boot order:**
   ```text
   NVS init
     ↓
   Wi-Fi connect
     ↓
   SNTP sync  ← wait here until time is valid
     ↓
   cron_job_create(...)
     ↓
   cron_start()
   ```

---

## Quick Start

### Minimal Example (no SNTP — only for testing; time will be wrong in production)

```c
#include "esp_cron.h"
#include "esp_log.h"
#include <time.h>

static const char *TAG = "cron_example";

void my_callback(cron_job *job) {
    ESP_LOGI(TAG, "Job triggered! data=%p", job->data);
}

void app_main(void) {
    // For testing only — in production, sync SNTP first
    struct timeval tv = { .tv_sec = 1530000000 }; // June 2018
    settimeofday(&tv, NULL);

    // Create a job that fires every second
    cron_job *job = cron_job_create("* * * * * *", my_callback, (void *)42);
    if (!job) {
        ESP_LOGE(TAG, "Failed to create job");
        return;
    }

    // Start the scheduler
    cron_start();

    // Keep main alive (scheduler runs in background tasks)
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
```

### Production Example (with SNTP)

```c
#include "esp_cron.h"
#include "esp_sntp.h"
#include "esp_log.h"
#include <time.h>

static const char *TAG = "cron_app";

void water_pump_callback(cron_job *job) {
    ESP_LOGI(TAG, "Turning pump ON");
    // ... control GPIO / pump ...
}

void sensor_read_callback(cron_job *job) {
    ESP_LOGI(TAG, "Reading soil moisture sensor");
    // ... read ADC / I2C ...
}

static void wait_for_time_sync(void) {
    ESP_LOGI(TAG, "Waiting for SNTP sync...");
    sntp_setoperatingmode(SNTP_OPMODE_POLL);
    sntp_setservername(0, "pool.ntp.org");
    sntp_init();

    int retry = 0;
    const int max_retry = 30;
    while (sntp_get_sync_status() != SNTP_SYNC_STATUS_COMPLETED && retry < max_retry) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        retry++;
    }
    if (retry >= max_retry) {
        ESP_LOGW(TAG, "SNTP sync timed out, scheduling may be incorrect");
    } else {
        ESP_LOGI(TAG, "Time synced successfully");
    }
}

void app_main(void) {
    // 1. Set timezone (affects CRON expression evaluation)
    setenv("TZ", "CST-8", 1);
    tzset();

    // 2. Wait for SNTP — must complete before creating jobs
    wait_for_time_sync();

    // 3. Create jobs AFTER time is valid
    // "0 0 8 * * *" = every day at 08:00
    cron_job *pump_job = cron_job_create("0 0 8 * * *", water_pump_callback, NULL);

    // "0 0 18 * * *" = every day at 18:00
    cron_job *evening_job = cron_job_create("0 0 18 * * *", water_pump_callback, NULL);

    // "*/30 * * * * *" = every 30 seconds
    cron_job *sensor_job = cron_job_create("*/30 * * * * *", sensor_read_callback, NULL);

    // 4. Start scheduler
    cron_start();

    ESP_LOGI(TAG, "Scheduler started with %d jobs",
             pump_job && evening_job && sensor_job ? 3 : 0);

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
```

---

## CRON Expression Format

6 fields with second-level precision:

```
┌────────────── second (0 - 59)
| ┌───────────── minute (0 - 59)
| │ ┌───────────── hour (0 - 23)
| │ │ ┌───────────── day of month (1 - 31)
| │ │ │ ┌───────────── month (1 - 12)
| │ │ │ │ ┌───────────── day of week (0 - 6, Sunday=0)
| │ │ │ │ │
* * * * * *
```

Common patterns:

| Expression | Meaning |
|------------|---------|
| `* * * * * *` | Every second |
| `0 * * * * *` | Every minute (at second 0) |
| `*/10 * * * * *` | Every 10 seconds |
| `0 0 8 * * *` | Every day at 08:00 |
| `0 30 7,19 * * *` | Every day at 07:30 and 19:30 |
| `0 0 8 * * 1-5` | Weekdays at 08:00 |

---

## API Reference

### Task Management

```c
// Create and schedule a new job. Returns NULL on failure.
// The job is automatically added to the scheduler.
cron_job *cron_job_create(const char *schedule,
                          cron_job_callback callback,
                          void *data);

// Destroy a job (removes from schedule, releases memory).
// Thread-safe: safe to call even while jobs are queued.
int cron_job_destroy(cron_job *job);

// Remove all jobs from the schedule.
// Per contract, performs no deallocation — the caller owns each job's memory
// and must cron_job_destroy() every job it created.
int cron_job_clear_all();
```

### Scheduler Control

```c
// Start the scheduler (creates worker task + esp_timer).
// Must be called after at least one job is created.
int cron_start();

// Stop the scheduler (stops timer, drains queue, clears all jobs).
int cron_stop();
```

### Manual Scheduling

```c
// Re-insert a job into the schedule (use after dynamic expression change).
int cron_job_schedule(cron_job *job);

// Remove a job from schedule without freeing it.
int cron_job_unschedule(cron_job *job);
```

### Dynamic Expression Update

```c
// Parse a new CRON expression into an existing job.
// You MUST call cron_job_schedule() afterward to apply the change.
int cron_job_load_expression(cron_job *job, const char *schedule);

// Check if expression was loaded successfully.
int cron_job_has_loaded(cron_job *job);
```

Usage:

```c
// Change a job's schedule
cron_job_unschedule(my_job);
cron_job_load_expression(my_job, "0 0 9 * * *");  // Now at 09:00
cron_job_schedule(my_job);
```

### Time Resync

```c
// Reschedule ALL jobs based on current system time.
// Call this after SNTP sync completes or time jumps.
int cron_job_reschedule_all();
```

### Status

```c
// Seconds until next job triggers.
time_t cron_job_seconds_until_next_execution();
```

---

## Kconfig Configuration

In `menuconfig` (ESP-IDF):

| Option | Default | Description |
|--------|---------|-------------|
| `CONFIG_ESP_CRON_ENABLE` | `y` | Enable/disable the component |
| `CONFIG_ESP_CRON_WORKER_STACK_SIZE` | `4096` | Stack size for worker and runner tasks (bytes) |
| `CONFIG_ESP_CRON_QUEUE_DEPTH` | `10` | Max executions waiting to run (capacity of the pending queue) |
| `CONFIG_ESP_CRON_MAX_DUE_JOBS` | `16` | Max jobs processed per single timer callback |
| `CONFIG_ESP_CRON_MIN_DELAY_US` | `1000` | Minimum timer delay (microseconds) |
| `CONFIG_ESP_CRON_ENABLE_STATS` | `n` | Track execution/cancel/queue-full/resync/skip counters (~40 bytes) |

> `cancelled` counts only executions cancelled by `destroy` (queued but not yet started). Triggers skipped because **the same job was already running** are tracked separately in `skipped_running`.

---

## Running the Tests

The tests live in `test/` (Unity source) and a ready-to-run app in `test_apps/`:

```bash
cd components/esp_cron/test_apps
idf.py set-target esp32s2   # match your board
idf.py flash monitor        # runs all 12 test cases on the device
```

> Note: cron schedules depend on the system clock. The tests call `settimeofday` directly — no SNTP needed.

Covered: list add/remove/boundary cases (remove-first, remove-last, single-node), cron expression scheduling, `clear_all`, `reschedule_all`, `start/stop` idempotency, `cancelled` initialization, expression modification + reschedule.

### 24h soak test (recommended)

To validate long-term stability (heap, task churn, skip/cancel counters), run several jobs of different periods while keeping Wi-Fi + SNTP active, periodically doing dynamic `schedule`/`unschedule`/`destroy`/`create` and `stop/start`, with callbacks simulating 1–3s work. Watch `free heap`, `minimum free heap`, `largest free block`, and the stats counters (enable `CONFIG_ESP_CRON_ENABLE_STATS`).

---

## Time Synchronization

### Why It Matters

The scheduler computes `next_execution` via `cron_next(expression, time())`. If `time()` returns `1970-01-01` (no sync), then `next_execution` will be computed relative to 1970 — the job will fire at the wrong moment.

### Correct Boot Order

```
NVS → Wi-Fi → SNTP (wait) → cron_job_create → cron_start
```

### Time Jumps at Runtime

If the system time jumps (e.g., SNTP correction after initial bad sync), the scheduler auto-detects jumps >1 second and recomputes all schedules.

**You should also register an SNTP sync callback** to reschedule immediately (rather than waiting for the next timer tick):

```c
#include "esp_sntp.h"

void sntp_sync_cb(struct timeval *tv) {
    cron_job_reschedule_all();  // rebuild all next_execution values
}

// Register before sntp_init()
sntp_set_time_sync_notification_cb(sntp_sync_cb);
```

### Timezone

CRON expressions are evaluated in **local time** (local time is enabled by default). Set `TZ` **before** creating jobs:

```c
setenv("TZ", "CST-8", 1);
tzset();
```

---

## Thread Safety

This component uses two mutexes internally:

- **`s_mutex`**: Serializes all linked-list and queue operations. Protects against concurrent `timer_cb`, `cron_job_destroy`, `cron_job_schedule`, and `cron_start/stop`.

- **`s_ref_mutex`**: Protects the reference counter on each `cron_job`. Ensures safe deallocation when a job is being executed by a callback while another thread calls `cron_job_destroy`.

**Safe patterns:**
- Call `cron_job_destroy()` from any task — even if the job is currently queued
- Call `cron_start()` / `cron_stop()` from any task — calls are serialized
- Create and destroy jobs from different tasks concurrently

**Reference counting behavior:**
- `cron_job_create`: refs = 1 (caller owns)
- Queue send: refs += 1 (queue owns, grabbed inside the scheduler lock)
- Worker finishes callback: refs -= 1; if refs == 0 → `free(job)`
- `cron_job_destroy`: refs -= 1; if refs == 0 → `free(job)`

This means `cron_job_destroy()` never blocks waiting for a running callback to finish — the job is freed automatically when the last reference is released.

## Lifecycle Semantics

### destroy cancels a queued job (B semantics)

Once `cron_job_destroy(job)` returns, the job is marked `cancelled`. A job that was already in the queue but has not started running **will not** execute its callback — the worker rewinds and releases it silently.

**`cron_job_destroy()` is terminal.** After destroy, the job handle must not be reused — `cron_job_schedule()`, `cron_job_load_expression()`, `cron_job_unschedule()` reject a cancelled job. Create a new job instead.

### destroy inside a callback is safe

```c
void cb(cron_job *job) {
    cron_job_destroy(job); // safe: runner still holds a ref
    // ... code after destroy also OK, job is freed after cb returns
}
```

The runner owns the final reference; the job is freed only after the callback returns and the runner releases it.

### No overlap of the same job (in_flight guard)

Once a job is **successfully queued** (its runner may not have started yet), its `in_flight` flag is set. If the next scheduled moment arrives while that job is still executing or still in the queue, that trigger is **skipped** (never queued). This prevents concurrent runners for the same job — critical for sequences like `pump ON → delay → pump OFF`. If enqueue fails (queue full), `in_flight` is restored so the job is never permanently skipped.

### start/stop are idempotent

| Pattern | Result |
|---------|--------|
| `cron_start(); cron_start();` | Second call returns `0` (already running, no duplicate timer/worker created) |
| `cron_stop(); cron_stop();` | Second call returns `-1` (not running) |
| `cron_start(); cron_stop(); cron_start();` | Restarts cleanly |

`cron_stop()`:
- Stops the timer (no new queue events)
- **Cooperative shutdown**: sends a stop sentinel to the worker, waits for its ack, then proceeds — the worker is never force-deleted mid-operation
- Drains the queue and `cron_job_clear_all()` (all jobs removed from the schedule; their memory is released by the caller's `cron_job_destroy`, since `clear_all` performs no deallocation per its contract)

Note: `cron_stop()` does **not** wait for callbacks already in-flight. Queued/running jobs finish on their runners (refcount guarantees safe teardown); un-consumed queue references are drained and released without leaking.

> `MAX_DUE_JOBS` is the **number processed per timer callback**, `QUEUE_DEPTH` is the **pending-execution queue capacity** — neither is a limit on how many cron jobs you may create. So `16` does not mean "max 16 jobs". If more jobs are due in one moment than `QUEUE_DEPTH`, the excess is dropped (`queue full` warning) but the offending jobs are rescheduled for their next matching moment.

### Queue full behavior

If the FreeRTOS queue is full when a job fires
(`CONFIG_ESP_CRON_QUEUE_DEPTH` exceeded), the event is dropped, a warning is printed, the job's `in_flight` flag is **restored** (so it fires again on the next matching moment), and the queue reference is released. **Jobs are never silently leaked and never permanently stuck skipped**.

### Missed executions

If a device wakes after a scheduled moment (e.g. Deep Sleep wakes at 08:10 but a job was scheduled for 08:00), the job is **not** run retroactively — the scheduler recomputes from the current time and waits for the next matching moment. This is deliberate: catching up on missed irrigation cycles could be dangerous.

### Callback contract

Callbacks run on a per-job runner task. Keep them short — send an event to a business worker instead of blocking (no `vTaskDelay(10000)`, HTTP requests, or script execution in the callback). Safe to call these APIs from inside a callback:
- `cron_job_schedule(job)` / `cron_job_unschedule(job)`
- `cron_job_load_expression(job, ...)` / `cron_job_destroy(job)`
- `cron_job_reschedule_all()`

**Do NOT call `cron_stop()` / `cron_start()` from inside a callback.** They manipulate the scheduler's worker and timer lifecycle; calling them from a runner can deadlock or tear down the infrastructure mid-execution.

---

## Limitations & Anti-Patterns

**Not supported (by design):**

1. **Deep Sleep recovery.** Jobs are in-memory objects; Deep Sleep destroys all RAM. Persist the schedule to NVS and re-create jobs on wake.

2. **Sub-second precision.** Minimum granularity is 1 second.

3. **Single scheduler instance.** Only one `cron_start()` / `cron_stop()` cycle at a time.

4. **Catching up missed executions.** If the device wakes after a scheduled moment (e.g. Deep Sleep wakes at 08:10 but a job was for 08:00), the job is **not** run retroactively — the scheduler waits for the next matching moment. This prevents dangerous cascading replays (e.g. irrigation pump double-trigger).

**Do NOT use esp_cron for:**

5. **Long-running callbacks.** Callbacks run on a per-job runner task. If your callback blocks for minutes (HTTP, heavy computation, `vTaskDelay`), the job's next trigger will be skipped (`in_flight` guard — no concurrent runners for the same job). Keep callbacks short — send an event to a dedicated worker task instead.

6. **Calling `cron_stop()` / `cron_start()` from inside a callback.** This manipulates the scheduler's worker and timer lifecycle; calling them from a runner can deadlock or tear down infrastructure mid-execution. (All other scheduler APIs — schedule, unschedule, destroy, load_expression, reschedule_all — are safe inside callbacks.)

7. **`day-of-month` AND `day-of-week` simultaneously.** The underlying `ccronexpr` parser follows POSIX: if both are specified, the job triggers when **either** matches (logical OR, not AND).

---

## Acknowledgments

- [esp_cron](https://github.com/DavidMora/esp_cron) by David Mora Rodriguez
- [ccronexpr](https://github.com/staticlibs/ccronexpr) — CRON expression parser

## License

[Apache License 2.0](./LICENSE.txt)
