# ESP-Cron: 适用于 ESP-IDF 的 CRON 定时任务调度器

> [English](./README.md)

## 概述

ESP-Cron 是基于 `esp_timer` 和 FreeRTOS 的轻量级 CRON 定时任务调度器。支持 6 字段 CRON 表达式（秒、分、时、日、月、星期），精度到秒级。

**核心特性：**
- 调度时刻基于系统时钟 `time()` 计算，`esp_timer` 仅负责等待到该时刻
- 引用计数机制：即使任务正在队列中等待执行，调用 `cron_job_destroy` 也是安全的
- 线程安全：全局互斥保护所有链表与队列操作
- 智能低功耗：无任务时自动停止定时器
- 每个 job 有独立的 runner task，无需共享线程池

**启动前必须同步系统时间（SNTP），否则调度将基于 1970 年起始时间计算。**

---

## 文档导航

| 文档 | 用途 |
|------|------|
| **本 README** | 使用指南、API 参考、配置、注意事项 |
| [docs/DESIGN.md](./docs/DESIGN.md) | 架构、线程安全模型、生命周期契约、不该做什么 |

---

## 项目结构

```
esp_cron/
├── esp_cron.c                  # 核心调度逻辑（esp_timer + 队列 + 引用计数）
├── include/
│   ├── esp_cron.h              # 公共头文件（include 这个）
│   └── cron.h                  # 内部 API + cron_job 结构体定义
├── library/
│   ├── ccronexpr/              # 第三方 CRON 表达式解析器
│   └── jobs/                   # 有序链表管理
├── Kconfig                     # 编译时配置项
├── examples/
│   └── esp_cron_example/       # 完整可运行示例
├── test/                       # Unity 测试用例（test_cron.c）
├── test_apps/                  # 可直接运行的测试工程（见"运行测试"）
└── docs/
    └── DESIGN.md               # 架构与生命周期契约（从这里开始）
```

---

## 前置条件

1. **系统时间必须有效。** 在调用 `cron_job_create` 之前，通过 SNTP（或带电池备份的 RTC）设置时钟。

2. **正确的启动顺序：**
   ```text
   NVS 初始化
     ↓
   Wi-Fi 连接
     ↓
   SNTP 同步  ← 等待直到时间有效
     ↓
   cron_job_create(...)
     ↓
   cron_start()
   ```

---

## 快速开始

### 最小示例（无 SNTP —— 仅测试用，生产环境时间会不正确）

```c
#include "esp_cron.h"
#include "esp_log.h"
#include <time.h>

static const char *TAG = "cron_example";

void my_callback(cron_job *job) {
    ESP_LOGI(TAG, "Job triggered! data=%p", job->data);
}

void app_main(void) {
    // 仅测试用 —— 生产环境应先同步 SNTP
    struct timeval tv = { .tv_sec = 1530000000 }; // 2018年6月
    settimeofday(&tv, NULL);

    // 创建每秒触发的任务
    cron_job *job = cron_job_create("* * * * * *", my_callback, (void *)42);
    if (!job) {
        ESP_LOGE(TAG, "Failed to create job");
        return;
    }

    // 启动调度器
    cron_start();

    // 保持主线程运行（调度器在后台任务中运行）
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
```

### 生产示例（带 SNTP）

```c
#include "esp_cron.h"
#include "esp_sntp.h"
#include "esp_log.h"
#include <time.h>

static const char *TAG = "cron_app";

void water_pump_callback(cron_job *job) {
    ESP_LOGI(TAG, "Turning pump ON");
    // ... 控制 GPIO / 水泵 ...
}

void sensor_read_callback(cron_job *job) {
    ESP_LOGI(TAG, "Reading soil moisture sensor");
    // ... 读取 ADC / I2C ...
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
    // 1. 设置时区（影响 CRON 表达式的求值）
    setenv("TZ", "CST-8", 1);
    tzset();

    // 2. 等待 SNTP 同步 —— 创建任务前必须完成
    wait_for_time_sync();

    // 3. 时间有效后创建任务
    // "0 0 8 * * *" = 每天 08:00
    cron_job *pump_job = cron_job_create("0 0 8 * * *", water_pump_callback, NULL);

    // "0 0 18 * * *" = 每天 18:00
    cron_job *evening_job = cron_job_create("0 0 18 * * *", water_pump_callback, NULL);

    // "*/30 * * * * *" = 每 30 秒
    cron_job *sensor_job = cron_job_create("*/30 * * * * *", sensor_read_callback, NULL);

    // 4. 启动调度器
    cron_start();

    ESP_LOGI(TAG, "Scheduler started with %d jobs",
             pump_job && evening_job && sensor_job ? 3 : 0);

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
```

---

## CRON 表达式格式

6 字段，精度到秒：

```
┌────────────── 秒 (0 - 59)
| ┌───────────── 分 (0 - 59)
| │ ┌───────────── 时 (0 - 23)
| │ │ ┌───────────── 日 (1 - 31)
| │ │ │ ┌───────────── 月 (1 - 12)
| │ │ │ │ ┌───────────── 星期 (0 - 6, 0=周日)
| │ │ │ │ │
* * * * * *
```

常用表达式：

| 表达式 | 含义 |
|--------|------|
| `* * * * * *` | 每秒 |
| `0 * * * * *` | 每分钟（第 0 秒） |
| `*/10 * * * * *` | 每 10 秒 |
| `0 0 8 * * *` | 每天 08:00 |
| `0 30 7,19 * * *` | 每天 07:30 和 19:30 |
| `0 0 8 * * 1-5` | 工作日 08:00 |

---

## API 参考

### 任务管理

```c
// 创建并调度新任务，失败返回 NULL。
// 任务创建后自动加入调度器。
cron_job *cron_job_create(const char *schedule,
                          cron_job_callback callback,
                          void *data);

// 销毁任务（从调度中移除并释放内存）。
// 线程安全：即使任务正在队列中等待，调用也是安全的。
int cron_job_destroy(cron_job *job);

// 将所有任务移出调度表。
// 按契约本调用不执行任何释放——每个任务的内存归调用者，
// 必须对创建的每个任务逐一调用 cron_job_destroy()。
int cron_job_clear_all();
```

### 调度器控制

```c
// 启动调度器（创建 worker 任务 + esp_timer）。
// 必须在至少创建一个任务后调用。
int cron_start();

// 停止调度器（停止定时器、清空队列、清除所有任务）。
int cron_stop();
```

### 手动调度

```c
// 重新将任务加入调度（修改表达式后使用）。
int cron_job_schedule(cron_job *job);

// 从调度中移除任务但不释放内存。
int cron_job_unschedule(cron_job *job);
```

### 动态修改表达式

```c
// 解析新的 CRON 表达式到已有任务。
// 修改后必须调用 cron_job_schedule() 使更改生效。
int cron_job_load_expression(cron_job *job, const char *schedule);

// 检查表达式是否已成功加载。
int cron_job_has_loaded(cron_job *job);
```

用法：

```c
// 修改任务的调度时间
cron_job_unschedule(my_job);
cron_job_load_expression(my_job, "0 0 9 * * *");  // 改为每天 09:00
cron_job_schedule(my_job);
```

### 时间重同步

```c
// 按当前系统时间重新计算所有任务的调度时刻。
// SNTP 同步完成或时间跳变后调用。
int cron_job_reschedule_all();
```

### 状态查询

```c
// 距离下一个任务触发的秒数。
time_t cron_job_seconds_until_next_execution();
```

---

## Kconfig 配置

在 `menuconfig`（ESP-IDF）中配置：

| 选项 | 默认值 | 说明 |
|------|--------|------|
| `CONFIG_ESP_CRON_ENABLE` | `y` | 启用/禁用本组件 |
| `CONFIG_ESP_CRON_WORKER_STACK_SIZE` | `4096` | Worker 和 Runner 任务的栈大小（字节） |
| `CONFIG_ESP_CRON_QUEUE_DEPTH` | `10` | 待执行 execution 的队列容量（可排队的触发数） |
| `CONFIG_ESP_CRON_MAX_DUE_JOBS` | `16` | 单次 timer 回调处理的最大 job 数 |
| `CONFIG_ESP_CRON_MIN_DELAY_US` | `1000` | 定时器最小延迟（微秒） |
| `CONFIG_ESP_CRON_ENABLE_STATS` | `n` | 统计执行/取消/队列满/时间重同步/skip 计数器（~40 字节） |

> `cancelled` 仅统计 destroy 后取消的 queued（入队但未执行）**execution**。因**同一 job 正在运行**而被跳过的触发单独计入 `skipped_running`，不混入 `cancelled`。

---

## 运行测试

测试代码在 `test/`（Unity 源码），`test_apps/` 是可直接运行的测试工程：

```bash
cd components/esp_cron/test_apps
idf.py set-target esp32s2   # 按你的板子设置
idf.py flash monitor        # 真机运行全部 12 个测试用例
```

> 注意：cron 调度依赖系统时钟。测试直接调用 `settimeofday`，无需 SNTP。

覆盖：链表增删/边界（删除开头、删除末尾、单节点）、cron 表达式调度、`clear_all`、`reschedule_all`、`start/stop` 幂等、`cancelled` 初始化、修改表达式后重排。

### 24 小时 soak 测试（建议）

验证长期稳定性（堆、任务数、skip/cancel 计数器）：让若干不同周期的 job 同时运行，保持 WiFi + SNTP，定时做动态 `schedule`/`unschedule`/`destroy`/`create` 和 `stop/start`，callback 模拟 1~3 秒工作负载。观察 `free heap`、`minimum free heap`、`largest free block` 和统计计数器（开启 `CONFIG_ESP_CRON_ENABLE_STATS`）。

---

## 时间同步

### 为什么重要

调度器通过 `cron_next(expression, time())` 计算 `next_execution`。如果 `time()` 返回 `1970-01-01`（未同步），则 `next_execution` 将基于 1970 年计算 —— 任务会在错误的时间触发。

### 正确的启动顺序

```
NVS → Wi-Fi → SNTP（等待） → cron_job_create → cron_start
```

### 运行时时间跳变

如果系统时间发生跳变（例如首次同步后 SNTP 修正），调度器会自动检测 >1 秒的跳变并重新计算所有任务。

**建议在 SNTP 回调中主动通知**（比等待下一次 timer tick 更及时）：

```c
#include "esp_sntp.h"

void sntp_sync_cb(struct timeval *tv) {
    cron_job_reschedule_all();  // 立即重建所有 next_execution
}

// 在 sntp_init() 之前注册
sntp_set_time_sync_notification_cb(sntp_sync_cb);
```

### 时区

CRON 表达式按**本地时间**求值（默认启用本地时间）。**必须在创建任务前**设置 `TZ`：

```c
setenv("TZ", "CST-8", 1);
tzset();
```

---

## 线程安全

本组件内部使用两个互斥锁：

- **`s_mutex`**：串行化所有链表和队列操作。保护 `timer_cb`、`cron_job_destroy`、`cron_job_schedule`、`cron_start/stop` 之间的并发访问。

- **`s_ref_mutex`**：保护每个 `cron_job` 的引用计数。确保当一个任务正在执行回调时，另一个线程调用 `cron_job_destroy` 能安全释放内存。

**安全模式：**
- 从任何任务调用 `cron_job_destroy()` —— 即使任务正在队列中等待
- 从任何任务调用 `cron_start()` / `cron_stop()` —— 调用会被序列化
- 从不同任务并发创建和销毁任务

**引用计数行为：**
- `cron_job_create`：refs = 1（调用者持有）
- 入队：refs += 1（在调度锁保护内获取引用）
- Worker 完成回调：refs -= 1；若 refs == 0 → `free(job)`
- `cron_job_destroy`：refs -= 1；若 refs == 0 → `free(job)`

这意味着 `cron_job_destroy()` 永远不会阻塞等待正在运行的回调完成 —— 对象在最后一个引用释放时自动释放。

## 生命周期语义

### destroy 取消已入队任务（B 语义）

`cron_job_destroy(job)` 返回后，任务被标记为 `cancelled`。已经进入队列但尚未开始执行的任务**不会**执行其回调 —— worker 将其直接释放。

**`cron_job_destroy()` 是终态。** destroy 之后 job 句柄不得复用 —— `cron_job_schedule()`、`cron_job_load_expression()`、`cron_job_unschedule()` 会拒绝已取消的 job。请创建新任务代替。

### callback 内 destroy 自己是安全的

```c
void cb(cron_job *job) {
    cron_job_destroy(job); // 安全：runner 仍持有引用
    // ... 之后的代码也可执行，job 在回调返回后才释放
}
```

runner 持有最后一个引用；job 在回调返回、runner 释放引用后才被 free。

### 同一 job 不重叠执行（in_flight 防重入）

job 一旦**成功入队**（runner 可能尚未启动），`in_flight` 即置位。如果下一个调度时刻到达时该 job 仍在执行或仍在队列中，本次触发会被**跳过**（不会入队）。这防止同一 job 产生并发 runner —— 对 `pump ON → delay → pump OFF` 这类序列至关重要。入队失败（如队列满）会恢复 `in_flight`，job 不会永远被跳过。

### start/stop 幂等

| 模式 | 结果 |
|------|------|
| `cron_start(); cron_start();` | 第二次返回 `0`（已在运行，不会重复创建 timer/worker） |
| `cron_stop(); cron_stop();` | 第二次返回 `-1`（未运行） |
| `cron_start(); cron_stop(); cron_start();` | 干净地重新启动 |

`cron_stop()` 执行：
- 停止定时器（不再产生新的队列事件）
- **协作式停机**：向 worker 发送停止哨兵，worker 退出并 ack 后才继续，不在停机窗口内强制 `vTaskDelete` worker
- 排空队列并 `cron_job_clear_all()`（所有任务从调度表中移除；内存由调用者的 `cron_job_destroy` 释放，因为 `clear_all` 按契约不执行释放）

与 callback 的关系：`cron_stop()` **不等待正在执行的 callback**。已入队/service 的 job 会继续在 runner 上执行完，由引用计数安全收尾；未消费的队列引用被协议化排空，不泄漏。

> `MAX_DUE_JOBS` 是**单次 timer 回调处理数量**，`QUEUE_DEPTH` 是**待执行队列容量** —— 两者都不是系统支持的最大 cron job 数量。所以 `16` 不代表"最多 16 个任务"。若同一时刻到期任务超过 `QUEUE_DEPTH`，超出的会被丢弃（打印 queue full 警告），但对应 job 会重新排到下一个匹配时刻。

### 队列满时的行为

如果任务触发时 FreeRTOS 队列已满（超过 `CONFIG_ESP_CRON_QUEUE_DEPTH`），事件被丢弃、打印警告、恢复该 job 的 `in_flight` 标志并释放队列引用——因此该 job **不会**永久跳过，下次匹配时刻仍会正常触发。**任务永远不会静默泄漏**。

### 错过执行

如果设备唤醒时已错过调度时刻（例如 Deep Sleep 在 08:10 唤醒但任务计划 08:00），任务**不会**追溯执行 —— 调度器从当前时间重新计算并等待下一个匹配时刻。这是刻意为之：自动补浇灌可能造成用户完全没预期的灌溉。

### 回调契约

回调在每次任务的 runner task 中运行。请保持简短 —— 通过发送事件给业务 worker 而非阻塞（不要在回调中执行 `vTaskDelay(10000)`、HTTP 请求或脚本执行）。允许在回调内安全调用这些 API：
- `cron_job_schedule(job)` / `cron_job_unschedule(job)`
- `cron_job_load_expression(job, ...)` / `cron_job_destroy(job)`
- `cron_job_reschedule_all()`

**禁止在回调内调用 `cron_stop()` / `cron_start()`。** 它们操作调度器的 worker 与定时器生命周期，从 runner 中调用可能导致死锁或在执行中途拆毁基础设施。

---

## 局限性与反模式

**不支持（设计上）：**

1. **Deep Sleep 恢复。** 任务是内存中的对象，Deep Sleep 会销毁所有 RAM。需将调度保存到 NVS，唤醒时重新创建 job。

2. **亚秒精度。** 最小粒度为 1 秒。

3. **多调度器实例。** 同一时间只能有一个 `cron_start()` / `cron_stop()` 周期。

4. **追赶错过的执行。** 设备在预定时刻之后唤醒（如 Deep Sleep 在 08:10 醒来，任务定在 08:00），任务**不会**回溯执行——调度器会等到下一个匹配时刻。这防止重复灌溉等危险场景。

**不要用 esp_cron 做：**

5. **长时间阻塞的 callback。** callback 在 runner task 上执行。如果 callback 阻塞数分钟（HTTP、大量计算、`vTaskDelay`），该 job 的下一次触发会被跳过（`in_flight` 防重入保护：从入队到回调结束期间不会再次入队）。callback 应保持简短——发事件给专用业务 worker。

6. **在 callback 内调用 `cron_stop()` / `cron_start()`。** 这会操作 worker 和定时器生命周期，可能导致死锁或在执行中途拆毁基础设施。（其他所有调度 API——schedule、unschedule、destroy、load_expression、reschedule_all——在 callback 内均可安全调用。）

7. **同时指定"日"和"星期"。** 底层 `ccronexpr` 遵循 POSIX 标准：两者同时指定时，任一条件满足即触发（逻辑 OR，非 AND）。

---

## 致谢

- [esp_cron](https://github.com/DavidMora/esp_cron) by David Mora Rodriguez
- [ccronexpr](https://github.com/staticlibs/ccronexpr) — CRON 表达式解析器

## 许可证

[Apache License 2.0](./LICENSE.txt)
