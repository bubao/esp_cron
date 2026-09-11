# ESP-Cron 设计文档

## 架构总览

```
┌─────────────────────────────────────────────────┐
│                 esp_cron 内部                    │
│                                                 │
│  time() ──► cron_next() ──► esp_timer (等待)    │
│                                 │                │
│                         timer_cb 唤醒            │
│                                 │                │
│               s_mutex           ▼                │
│              ┌──────────────────────┐            │
│              │  筛选到期 job（≤16）  │            │
│              │  ref → 入队 → 移除   │            │
│              └──────────┬───────────┘            │
│                         │ xQueueSend             │
│                         ▼                        │
│              ┌──────────────────────┐            │
│              │     worker task      │            │
│              │  取出 → 创建 runner  │            │
│              └──────────┬───────────┘            │
│                         │ xTaskCreate            │
│                         ▼                        │
│              ┌──────────────────────┐            │
│              │    runner task       │            │
│              │  callback → unref    │            │
│              └──────────────────────┘            │
│                                                  │
└─────────────────────────────────────────────────┘
                          │
                          ▼
                  你的业务 callback
            （发事件 / 控 GPIO / 记日志）
                          │
                          ▼
              你的业务 worker（不在这里）
```

**esp_cron 只回答一个问题：** "现在这个 job 到时间了吗？"
它不知道 pump、GPIO、Lua、HTTP、MQTT、NVS、WiFi、Sensor。

---

## 核心调度原理

调度由两部分组成：

1. **cron_next()** —— 根据当前 `time()` 和 cron 表达式，算出下一个触发时刻（`next_execution`）
2. **esp_timer** —— 一次性定时器，只负责"等那么多微秒"

```
cron_next("0 8 * * *", time()) → next_execution = 明天 08:00:00
esp_timer_start_once(delay_us)  → 等到那个时刻
```

**esp_timer 不判断"现在几点"**，它只计算时间差。因此：

- 系统时间错了 → `cron_next()` 算出的时刻是错的 → 任务在错误时间触发
- SNTP 纠正后 → 必须调用 `cron_job_reschedule_all()` 重算所有 `next_execution`

---

## 线程安全模型

### 双锁设计

| 锁 | 保护对象 | 必须获取时机 |
|----|---------|-------------|
| `s_mutex` | 链表操作、队列操作、状态机 | 所有链表读写 |
| `s_ref_mutex` | `job->refs` 引用计数、`in_flight`、`cancelled` | `cron_job_ref_locked()`、timer_cb 的 `in_flight` 读写、疑似 UAF 检查 |

**锁顺序：** `s_mutex → s_ref_mutex`（永远如此，不可反向——否则 ABBA 死锁）

### 引用计数

```
refs = 1    cron_job_create() 返回（调用者持有）
refs += 1   timer_cb 入队成功（queue 持有，s_mutex 内 +1）
refs -= 1   destroy / clear_all（释放调用者持有）
refs -= 1   runner task 回调结束（释放 queue 持有）
refs == 0   free(job) —— 自动释放，不需要 join
```

### 链表 insert 失败传播（P1-3）

`cron_job_list_insert()` 在 `calloc` 失败时返回 `-1`。所有调用点必须检查并传播：

| 调用点 | 失败行为 |
|--------|---------|
| `cron_job_create()`（经 schedule） | 释放 job 结构，返回 `NULL` —— 调用者不会拿到半初始化 job |
| `cron_job_schedule()` | 返回 `-1`，打印日志，不再静默成功 |
| `cron_job_schedule_nosched()`（timer_cb 重排） | 返回 `-1` + 日志（内存耗尽无法补救，但明确说了这是丢排期而非静默） |
| `cron_job_reschedule_all()` | 返回 `-1` + 失败计数日志 |

测试钩子：`cron_job_test_force_insert_fail()`（weak 默认 0，测试二进制强覆盖）注入 `calloc` 失败路径。

链表本身**不持有引用**——job 在链表内的安全性由"调用者句柄引用 + s_mutex 串行化"共同保证。

### 为什么 free 可以在锁外做

`s_ref_mutex` 只保护引用计数本身；`free()` 在 `refs == 0` 时立即执行，不等待任何锁。这是因为：

- 持有 `s_mutex` 的代码路径只通过 `cron_job_ref_locked()`（读 refs）访问 job
- 持有 `s_ref_mutex` 的代码路径只做增减计数
- `free()` 不持有任何调度锁 → 不可能死锁

---

## 生命周期契约

### 状态机

```
STOPPED ──start──► RUNNING ──stop──► STOPPING ──清理完──► STOPPED
    ▲                                      │
    │          start（幂等，直接返回0）      │
    └──────────────────────────────────────┘
```

- `cron_start()` 已经在运行 → 返回 0（幂等，不重复创建 worker/timer）
- `cron_stop()` 未运行 → 返回 -1
- `cron_stop()` 返回时调度器**已 STOPPED**（同步语义）；不强杀正在执行的 callback，由 refcount 安全收尾

### destroy 是终态

```
cron_job_destroy(job)
    ├── job->cancelled = true
    ├── 从链表移除
    ├── 释放调用者引用（refs--）
    └── return
```

- 已入队但未执行的 callback **不会执行**（worker 检测 cancelled 后直接释放）
- destroy 之后 `job` 句柄**不得复用**：`schedule / unschedule / load_expression` 会拒绝返回 -1
- destroy 在 callback 内调用是安全的（runner 持有最后一个引用，回调返回后才 free）

### callback 内可安全调用的 API

```
cron_job_schedule(job)        ✅
cron_job_unschedule(job)      ✅
cron_job_load_expression()    ✅
cron_job_destroy(job)         ✅（self-destroy，runner 保护）
cron_job_reschedule_all()     ✅
cron_stop()                   ❌（会删除 worker，死锁）
cron_start()                  ❌（同上）
```

### 同一 job 不重叠执行（in_flight 防重入）

job **成功入队**时 `in_flight` 即置位（先于 runner 启动）。如果下一次触发时刻到期时该 job 仍在执行或仍在队列中，**该次触发被跳过**（不入队）。对 `pump ON → delay → pump OFF` 这类序列至关重要。

### queue 满时的行为

```
queue 满 → 打印警告 → 恢复 in_flight → unref（不泄漏） → stats.queue_full++
```

必须恢复 `in_flight`：否则该 job 再到期时会被当成"仍在执行"永远跳过（dead）。恢复后 job 下次匹配时刻仍会正常触发。

### cron_stop 协作式停机（P0-2）

```
cron_stop()
  ├── state = STOPPING
  ├── 停止 + 删除 esp_timer（全程持 s_mutex，等价 esp_timer_stop_blocking 的 callback 同步）
  ├── 队列发送 NULL 毒丸 → worker 收到即退出循环
  ├── 等待 worker ack（s_worker_done，最多 1s；超时才防御性 vTaskDelete）
  ├── 排空队列（unref 残留引用）
  ├── state = STOPPED
  └── cron_job_clear_all()
```

- 不直接在停机窗口 `vTaskDelete(cron.handle)`——避免 worker 半途退出破坏 refcount 追责链
- `cron_stop()` 不等待正在执行的 callback：runner 由 refcount 安全收尾
- 重复 feed 毒丸需要 queue 有空位，而 worker 只做 receive + xTaskCreate（不取任何锁），空位必然腾出，无死锁

---

## 时间同步

### 正确的启动顺序

```
NVS init
  ↓
Wi-Fi connect
  ↓
SNTP sync（等待同步完成）
  ↓
setenv("TZ", ...)  ← 设置时区（影响 cron 表达式解析）
tzset()
  ↓
cron_job_create()   ← time() 已正确
cron_start()
```

### SNTP 同步后必须调用

```
void sntp_cb(struct timeval *tv) {
    cron_job_reschedule_all();  // 重建所有 next_execution
}
```

自动检测（`|Δwall - Δmono| > 2s`）只是 fallback，不能替代主动通知。

### 时区

CRON 表达式按**本地时间**求值（默认启用本地时间）。`TZ` 环境变量影响 `localtime()`，进而影响触发时刻。**必须在 `cron_job_create()` 之前设置好。**

```
setenv("TZ", "CST-8", 1);  // 或 "EST5EDT"、"UTC" 等
tzset();
```

---

## 什么时候不该用 esp_cron

| 场景 | 为什么不适合 |
|------|-------------|
| Deep Sleep 唤醒后恢复定时 | esp_cron 是内存状态，Deep Sleep 丢失 RAM。应将 schedule 持久化到 NVS，唤醒后重建 job |
| 需要 sub-second 精度 | 最小粒度 1 秒（`ccronexpr` 限制） |
| 长时间阻塞的 callback | callback 在 runner task 上执行，长时间阻塞会导致该 job 跳过下一次触发（in_flight 防重入）。业务逻辑应发事件到专用 worker |
| 一个系统多个独立调度器 | 只支持单实例（一个 `cron_start` / `cron_stop`） |
| 需要"追赶"错过的执行 | 设计上不回溯执行错过的时刻（防止重复灌溉等危险场景） |

---

## 内存与任务开销

> **为什么不用常驻任务池：** ESP32-S2 只有 320KB SRAM。典型场景 2-5 个定时任务，每 job 按需创建 runner task（4096 栈），空闲时自动释放。引入常驻任务池反而更耗内存、复杂度更高。YAGNI。

| 组件 | 开销 |
|------|------|
| scheduler 本体 | ~60 bytes（全局状态 + timer + mutex） |
| 每个 job | ~64 bytes（cron_job 结构 + 链表节点 + cron 表达式） |
| timer_cb | 临时栈帧，不分配堆内存 |
| worker task | 常驻，堆栈 `CONFIG_ESP_CRON_WORKER_STACK_SIZE`（默认 4096） |
| runner task | 每次触发创建，回调结束后释放（默认 4096 栈） |
| 统计（开启时） | ~40 bytes（6 个 uint32） |

5 个 job 的典型场景：~380 bytes RAM + worker 常驻栈 + runner 瞬时栈。

---

## 关键配置项

```
CONFIG_ESP_CRON_WORKER_STACK_SIZE = 4096
    worker + runner task 栈大小。callback 复杂时（多层函数调用）可增大。

CONFIG_ESP_CRON_QUEUE_DEPTH = 10
    worker 队列深度。同时到期超过此数量的 job 会被跳过（打印警告），
    下次 timer_cb 自动重排。一般场景够用。

CONFIG_ESP_CRON_MAX_DUE_JOBS = 16
    单次 timer_cb 处理上限。超过的 job 留到下一次 timer_cb（1秒内）处理。
    不是"最多支持 16 个 job"——job 数量没有上限（RAM 限制除外）。

CONFIG_ESP_CRON_MIN_DELAY_US = 1000
    timer 最小延迟。防止 next_execution ≤ now 时以 0 延迟反复触发。

CONFIG_ESP_CRON_ENABLE_STATS = n
    运行统计。开启后增加 ~40 bytes RAM。
    调试时建议打开，生产环境按需。
```

---

## 文件说明

| 文件 | 职责 |
|------|------|
| `esp_cron.c` | 调度器核心：timer、worker、runner、refcount、状态机 |
| `include/cron.h` | 内部头文件：`cron_job` 结构定义、内部 API |
| `include/esp_cron.h` | 公共头文件：对外 API 声明（只 include 用这个） |
| `library/ccronexpr/` | 第三方：6 字段 cron 表达式解析（`cron_next()`） |
| `library/jobs/` | 有序链表：按 `next_execution` 排序的 job 管理 |
| `Kconfig` | 编译时配置项 |
| `examples/` | 完整可运行示例 |
| `test/` + `test_apps/` | Unity 单元测试（`test_apps` 里 `idf.py flash monitor` 真机运行） |
| `docs/DESIGN.md` | 本文档：架构与生命周期契约 |
