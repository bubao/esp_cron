// Copyright 2018 Insite SAS
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
//
//
// Author: David Mora Rodriguez dmorar (at) insite.com.co
//
#include "esp_cron.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "jobs.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

// 运行统计（CONFIG_ESP_CRON_ENABLE_STATS 可选）
#ifdef CONFIG_ESP_CRON_ENABLE_STATS
typedef struct {
    uint32_t executed;
    uint32_t cancelled;      // 仅统计 destroy 后取消的 queued execution
    uint32_t skipped_running; // 同一 job 正在执行而被跳过的触发
    uint32_t queue_full;
    uint32_t time_resync;
    uint32_t runner_create_failed;
} cron_stats_t;
static cron_stats_t stats = {0};
#endif

// Kconfig 提供，未配置时使用默认值
#ifndef CONFIG_ESP_CRON_WORKER_STACK_SIZE
#define CONFIG_ESP_CRON_WORKER_STACK_SIZE 4096
#endif
#ifndef CONFIG_ESP_CRON_QUEUE_DEPTH
#define CONFIG_ESP_CRON_QUEUE_DEPTH 10
#endif
#ifndef CONFIG_ESP_CRON_MAX_DUE_JOBS
#define CONFIG_ESP_CRON_MAX_DUE_JOBS 16
#endif
#ifndef CONFIG_ESP_CRON_MIN_DELAY_US
#define CONFIG_ESP_CRON_MIN_DELAY_US 1000
#endif

// ====================== 调度器状态 ========================

typedef enum {
    CRON_STATE_STOPPED = 0,
    CRON_STATE_RUNNING,
    CRON_STATE_STOPPING,  // 正在停止：不再接受新 job，已入队的执行完
} cron_state_e;

typedef struct {
    cron_state_e state;
    TaskHandle_t handle;
    time_t seconds_until_next_execution;
    QueueHandle_t task_queue;
    esp_timer_handle_t esp_timer;
    int next_id;
    bool time_baseline_valid; // 首次采样后置 true，避免 wall=0（1970）时误判跳变
    int64_t last_monotonic_us;
    time_t last_wall_time;
} cron_state_t;

static cron_state_t cron = {
    .state = CRON_STATE_STOPPED,
    .handle = NULL,
    .seconds_until_next_execution = -1,
    .task_queue = NULL,
    .esp_timer = NULL,
    .next_id = 1,
    .time_baseline_valid = false,
    .last_monotonic_us = 0,
    .last_wall_time = 0,
};

// ====================== 互斥锁 ========================

// 锁顺序规则（必须遵守，防止 ABBA 死锁）：
//   s_mutex → s_ref_mutex → 链表内部 semaphore
// 永远只能这个方向嵌套获取，禁止反向。

// ===== refcount 所有权模型 =====
//
//   refs = 1    cron_job_create（调用者持有句柄引用）
//   refs += 1   timer_cb 入队成功（queue 持有，s_mutex 内 ref_locked）
//   refs -= 1   destroy / clear_all（释放创建/调度持有）
//   refs -= 1   job_runner_task 回调结束（释放队列引用，所有权已从
//               worker 转移给 runner，worker→runner 之间不重复 +1/-1）
//   refs == 0   free(job)
//
// 关键不变量：
//   - ref 只能在持有 s_mutex 时增加（cron_job_ref_locked），
//     保证 timer_cb 拿到 job 指针后 destroy 无法提前 free
//   - 链表本身不持有独立 ref；job 在链表内的安全性由
//     @创建者句柄引用 + s_mutex 串行化 共同保证
//   - runner 是 job 生命周期终结者：无论 callback 内发生什么
//     （destroy 自己 / stop / clear_all），runner 最后执行 unref

static SemaphoreHandle_t s_mutex = NULL;     // 保护链表、队列、状态
static SemaphoreHandle_t s_ref_mutex = NULL;  // 保护 job->refs / in_flight / cancelled
static SemaphoreHandle_t s_worker_done = NULL; // worker 退出通知（cron_stop 等待用）

static void cron_ensure_mutex(void)
{
    if (s_mutex == NULL)
        s_mutex = xSemaphoreCreateMutex();
    if (s_ref_mutex == NULL)
        s_ref_mutex = xSemaphoreCreateMutex();
    if (s_worker_done == NULL)
        s_worker_done = xSemaphoreCreateBinary();
}

// ====================== 引用计数 ========================
// 规则：s_mutex → s_ref_mutex（先拿大锁再拿小锁）

static void cron_job_ref_locked(cron_job* job)
{
    // 必须在持有 s_mutex 时调用，防止 ref 与 destroy 竞态
    xSemaphoreTake(s_ref_mutex, portMAX_DELAY);
    job->refs++;
    xSemaphoreGive(s_ref_mutex);
}

static void cron_job_unref(cron_job* job)
{
    int refs;
    xSemaphoreTake(s_ref_mutex, portMAX_DELAY);
    refs = --job->refs;
    xSemaphoreGive(s_ref_mutex);
    if (refs <= 0)
        free(job);
}

// ====================== 内部工具 ========================

static void schedule_next_timer_locked();
static int cron_job_reschedule_all_locked(void);
static int cron_job_unschedule_locked(cron_job* job);

static int cron_job_exists_in_list(int id)
{
    struct cron_job_node* node = cron_job_list_first();
    while (node) {
        if (node->job && node->job->id == id)
            return 1;
        node = node->next;
    }
    return 0;
}

// 不自动调度定时器的 job schedule（调用方需持有 s_mutex）
static int cron_job_schedule_nosched(cron_job* job)
{
    cron_job_list_init();
    if (!job || !cron_job_has_loaded(job))
        return -1;
    time_t now;
    time(&now);
    job->next_execution = cron_next(&(job->expression), now);
    job->last_triggered_sec = -1;
    if (cron_job_exists_in_list(job->id))
        cron_job_list_remove(job->id);
    if (cron_job_list_insert(job) < 0) {
        printf("esp_cron: failed to insert job %d into schedule\n", job->id);
        return -1;
    }
    return 0;
}

// ====================== 定时器回调 ========================

static void timer_cb(void* arg)
{
    (void)arg;
    time_t now;
    time(&now);
    time_t now_sec = now;

    cron_job* due_jobs[CONFIG_ESP_CRON_MAX_DUE_JOBS];
    int due_count = 0;

    xSemaphoreTake(s_mutex, portMAX_DELAY);

    while (1) {
        struct cron_job_node* node = cron_job_list_first();
        if (!node || node->job->next_execution > now)
            break;
        cron_job* job = node->job;

        // 防重入：同一 job 已入队或正在执行则跳过本次触发。
        // （灌溉场景：pump ON/OFF 序列不能被并发 runner 打乱）
        xSemaphoreTake(s_ref_mutex, portMAX_DELAY);
        bool in_flight = job->in_flight;
        xSemaphoreGive(s_ref_mutex);
        if (in_flight) {
#ifdef CONFIG_ESP_CRON_ENABLE_STATS
            stats.skipped_running++; // 不计入 cancelled（语义不同）
#endif
            cron_job_list_remove(job->id);
            if (due_count < CONFIG_ESP_CRON_MAX_DUE_JOBS)
                due_jobs[due_count++] = job; // 重算 next_execution 推进到下次
            continue;
        }

        // 防抖：同一秒只触发一次
        if (job->last_triggered_sec == now_sec) {
            cron_job_list_remove(job->id);
            if (due_count < CONFIG_ESP_CRON_MAX_DUE_JOBS)
                due_jobs[due_count++] = job;
            continue;
        }

        job->last_triggered_sec = now_sec;

        // --- P0-1 修复：ref 必须在 s_mutex 保护内完成 ---
        // 先置 in_flight（入队即视为执行中，杜绝入队后、runner 启动前的
        // 重复入队窗口），再 ref，再入队，再从链表移除。顺序不可颠倒。
        xSemaphoreTake(s_ref_mutex, portMAX_DELAY);
        job->in_flight = true;
        xSemaphoreGive(s_ref_mutex);

        cron_job_ref_locked(job);
        if (cron.task_queue && xQueueSend(cron.task_queue, &job, 0) == pdTRUE) {
            // 入队成功：队列持有引用，in_flight 保持 true，回调执行完由 runner 清除
        } else {
            // --- P0-3 修复：Queue 满时恢复 in_flight、unref + 日志，不静默丢弃 ---
            printf("esp_cron: queue full, job %d dropped\n", job->id);
#ifdef CONFIG_ESP_CRON_ENABLE_STATS
            stats.queue_full++;
#endif
            xSemaphoreTake(s_ref_mutex, portMAX_DELAY);
            job->in_flight = false; // 入队失败：必须恢复，否则 job 永远无法再次触发
            xSemaphoreGive(s_ref_mutex);
            cron_job_unref(job); // 入队失败，释放 ref
        }
        cron_job_list_remove(job->id);
        if (due_count < CONFIG_ESP_CRON_MAX_DUE_JOBS)
            due_jobs[due_count++] = job;
    }

    for (int i = 0; i < due_count; ++i) {
        if (cron_job_schedule_nosched(due_jobs[i]) != 0)
            printf("esp_cron: failed to reschedule job %d after firing\n", due_jobs[i]->id);
    }

    schedule_next_timer_locked();
    xSemaphoreGive(s_mutex);
}

// 锁内轻量函数契约：本函数只做「读链表 head + time() + esp_timer 控制」。
// 禁止在其内引入 xQueueSend / callback / free / xTaskCreate，
// 否则 s_mutex 锁内挂载长操作会使锁关系复杂化并阻塞调度。
// (调用方必须持有 s_mutex)
static void schedule_next_timer_locked()
{
    struct cron_job_node* node = cron_job_list_first();
    cron_job* job = node ? node->job : NULL;
    if (!job) {
        if (cron.esp_timer)
            esp_timer_stop(cron.esp_timer);
        return;
    }

    // --- 时间跳变检测：比较墙钟增量与单调时钟增量 ---
    // 正常运行时两者同步前进（Δwall ≈ Δmono，均等于定时器间隔）。
    // SNTP 拨正 / settimeofday 只影响墙钟，导致两者出现明显差值。
    // 首次采样只建立 baseline；SNTP 首次同步从 1970 → 当前时间时，
    // 用户应主动调用 cron_job_reschedule_all() 重建 baseline。
    time_t now;
    time(&now);
    int64_t mono_now_us = esp_timer_get_time();
    if (!cron.time_baseline_valid) {
        cron.time_baseline_valid = true;
        cron.last_wall_time = now;
        cron.last_monotonic_us = mono_now_us;
    } else {
        int64_t dwall_us = (now - cron.last_wall_time) * 1000000LL;
        int64_t dmono_us = mono_now_us - cron.last_monotonic_us;
        int64_t delta_us = dmono_us - dwall_us;
        if (delta_us > 2000000LL || delta_us < -2000000LL) { // |Δ| > 2s
            if (cron_job_reschedule_all_locked() == 0) {
#ifdef CONFIG_ESP_CRON_ENABLE_STATS
                stats.time_resync++;
#endif
                node = cron_job_list_first();
                job = node ? node->job : NULL;
                if (!job) {
                    if (cron.esp_timer)
                        esp_timer_stop(cron.esp_timer);
                    return;
                }
            }
            // 重建 baseline：防止跳变在每次 timer_cb 上重复触发
            cron.last_wall_time = now;
            cron.last_monotonic_us = mono_now_us;
        }
        cron.last_monotonic_us = mono_now_us;
        cron.last_wall_time = now;
    }

    int64_t delay_us = (job->next_execution - now) * 1000000LL;

    // --- P0-4 修复：delay < MIN_DELAY 时强制 clamp ---
    if (delay_us < CONFIG_ESP_CRON_MIN_DELAY_US)
        delay_us = CONFIG_ESP_CRON_MIN_DELAY_US;

    esp_timer_stop(cron.esp_timer);
    esp_timer_start_once(cron.esp_timer, delay_us);

    cron.seconds_until_next_execution = job->next_execution - now;
}

static void schedule_next_timer()
{
    if (!s_mutex)
        return;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    schedule_next_timer_locked();
    xSemaphoreGive(s_mutex);
}

// ====================== worker task ========================

static void job_runner_task(void* arg)
{
    cron_job* job = (cron_job*)arg;

    // runner 执行 callback。in_flight 已在入队时置位（防重入起点是
    // "成功入队"，不是"开始执行"），这里只负责执行完清除。
    // 无论 callback 内发生什么（destroy 自己 / stop / clear_all），
    // 最后都清除 in_flight 并 unref——runner 是生命周期终结者。
    if (job && !job->cancelled && job->callback) {
        job->callback(job);
#ifdef CONFIG_ESP_CRON_ENABLE_STATS
        stats.executed++;
#endif
    } else if (job && job->cancelled) {
#ifdef CONFIG_ESP_CRON_ENABLE_STATS
        stats.cancelled++;
#endif
    }

    xSemaphoreTake(s_ref_mutex, portMAX_DELAY);
    job->in_flight = false;
    xSemaphoreGive(s_ref_mutex);

    cron_job_unref(job);
    vTaskDelete(NULL);
}

static void cron_worker_task(void* arg)
{
    // worker 与队列 1:1 绑定：队列句柄由 cron_start 传入，不读全局状态
    QueueHandle_t queue = (QueueHandle_t)arg;
    cron_job* job = NULL;
    while (1) {
        if (xQueueReceive(queue, &job, portMAX_DELAY)) {
            if (job == NULL)
                break; // P0-2: NULL 即 cron_stop 发来的停止哨兵（不作为 job 处理）
            if (xTaskCreate(job_runner_task, "job_runner",
                            CONFIG_ESP_CRON_WORKER_STACK_SIZE,
                            job, tskIDLE_PRIORITY + 1, NULL) != pdPASS) {
                printf("esp_cron: failed to create runner task, job %d dropped\n", job->id);
#ifdef CONFIG_ESP_CRON_ENABLE_STATS
                stats.runner_create_failed++;
#endif
                // runner 没起来，入队时置的 in_flight 永远不会被清除，
                // 必须恢复，否则该 job 再到期会一直被跳过（dead）。
                xSemaphoreTake(s_ref_mutex, portMAX_DELAY);
                if (job)
                    job->in_flight = false;
                xSemaphoreGive(s_ref_mutex);
                cron_job_unref(job); // 释放队列持有的引用
            }
        }
    }
    xSemaphoreGive(s_worker_done); // P0-2: 通知 cron_stop 本 worker 已退出
    vTaskDelete(NULL);
}

// ====================== API 实现 ========================

cron_job* cron_job_create(const char* schedule, cron_job_callback callback, void* data)
{
    if (!schedule)
        return NULL;

    cron_ensure_mutex();

    cron_job* job = calloc(1, sizeof(cron_job));
    if (!job)
        return NULL;

    job->callback = callback;
    job->data = data;
    job->refs = 1;
    job->cancelled = false;

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    job->id = cron.next_id++;
    xSemaphoreGive(s_mutex);

    if (cron_job_load_expression(job, schedule) != 0) {
        free(job);
        return NULL;
    }

    if (cron_job_schedule(job) != 0) {
        free(job);
        return NULL;
    }

    return job;
}

int cron_job_destroy(cron_job* job)
{
    if (!job)
        return -1;

    cron_ensure_mutex();

    xSemaphoreTake(s_mutex, portMAX_DELAY);

    // --- P0-2 修复：标记 cancelled，阻止已入队 job 执行 callback ---
    job->cancelled = true;

    cron_job_unschedule_locked(job);
    xSemaphoreGive(s_mutex);

    // 调用者释放引用；若在队列中，由 worker 消费后释放
    cron_job_unref(job);
    return 0;
}

int cron_job_clear_all()
{
    cron_job_list_init();
    cron_ensure_mutex();

    xSemaphoreTake(s_mutex, portMAX_DELAY);

    while (cron_job_list_first()) {
        cron_job* job = cron_job_list_first()->job;
        cron_job_list_remove(job->id);
        job->cancelled = true;
        cron_job_unref(job);
    }

    // 运行中清空：立即重设定时器（空链表会停止它）
    if (cron.state == CRON_STATE_RUNNING)
        schedule_next_timer_locked();

    xSemaphoreGive(s_mutex);
    return 0;
}

static int cron_job_reschedule_all_locked(void)
{
    int n = cron_job_node_count();
    if (n == 0)
        return 0;

    cron_job** jobs = calloc(n, sizeof(cron_job*));
    if (!jobs)
        return -1;

    int count = 0;
    struct cron_job_node* node = cron_job_list_first();
    while (node) {
        jobs[count++] = node->job;
        node = node->next;
    }

    int failed = 0;
    for (int i = 0; i < count; ++i) {
        if (cron_job_schedule_nosched(jobs[i]) != 0)
            failed++;
    }

    free(jobs);
    if (failed > 0)
        printf("esp_cron: reschedule_all failed for %d job(s)\n", failed);
    return failed ? -1 : 0;
}

int cron_job_reschedule_all()
{
    cron_job_list_init();
    cron_ensure_mutex();

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    int rc = cron_job_reschedule_all_locked();
    // 手动重排（如 SNTP 同步后）视为"我已知时间变了"：
    // 重置 baseline，由下一次 timer_cb 重建，避免随后自动检测误触发。
    cron.time_baseline_valid = false;
    cron.last_wall_time = 0;
    cron.last_monotonic_us = 0;
    xSemaphoreGive(s_mutex);
    return rc;
}

// --- P1-1 修复：start/stop 幂等性 ---
// cron_start: 已运行则返回 0（幂等）
// cron_stop:  停止后清理所有 job，再次 stop 返回 -1（未运行）
int cron_stop()
{
    if (!s_mutex)
        return -1;

    xSemaphoreTake(s_mutex, portMAX_DELAY);

    if (cron.state != CRON_STATE_RUNNING) {
        xSemaphoreGive(s_mutex);
        return -1;
    }

    // 同步语义：cron_stop() 返回时调度器已 STOPPED——
    // timer/worker/queue 全部清理完成，随后 start 是可靠的。
    // 注意：不等待正在执行的 callback（不强杀 runner task），
    // 由 refcount 保证其安全收尾。
    cron.state = CRON_STATE_STOPPING;

    // --- P0-2 修复：cooperative worker 停机 ---
    // 全程持有 s_mutex（timer_cb 需要 s_mutex，故此刻不可能有 timer_cb 在跑，
    // 等价于 esp_timer_stop_blocking 的 callback 生命周期同步），无直接 vTaskDelete：
    // 向队列发送 NULL 毒丸 → worker 收到后退出循环并自删 → cron_stop 等 ack。
    // 先停 timer，杜绝停机窗口内新 job 入队。
    if (cron.esp_timer) {
        esp_timer_stop(cron.esp_timer);
        esp_timer_delete(cron.esp_timer);
        cron.esp_timer = NULL;
    }

    if (cron.handle && cron.task_queue) {
        cron_job* poison = NULL;
        // worker 只做 receive + xTaskCreate，不取任何锁，空位必然腾出，
        // 不会与 s_mutex 死锁；portMAX_DELAY 安全。
        if (xQueueSend(cron.task_queue, &poison, portMAX_DELAY) == pdTRUE) {
            if (xSemaphoreTake(s_worker_done, pdMS_TO_TICKS(1000)) != pdTRUE) {
                // 防御兜底：worker 未按时 ack（异常）。此刻它必然阻塞在
                // xQueueReceive 或 xTaskCreate 中途，直接删除不会泄漏队列引用。
                printf("esp_cron: worker did not ack stop, force deleting\n");
                vTaskDelete(cron.handle);
            }
        } else {
            // xQueueSend portMAX_DELAY 不会失败，此分支仅做防御
            printf("esp_cron: failed to signal worker stop\n");
            vTaskDelete(cron.handle);
        }
        cron.handle = NULL;
    }

    // 排空队列中未消费的引用（worker 已退出，无人在消费）
    // 清 in_flight：这些 job 从未被 runner 消费过，若调用者仍持有引用，
    // in_flight 必须复位，否则重新 schedule 后会被永久跳过（dead）。
    if (cron.task_queue) {
        cron_job* pending = NULL;
        while (xQueueReceive(cron.task_queue, &pending, 0) == pdTRUE) {
            if (pending) {
                xSemaphoreTake(s_ref_mutex, portMAX_DELAY);
                pending->in_flight = false;
                xSemaphoreGive(s_ref_mutex);
                cron_job_unref(pending);
            }
        }
        vQueueDelete(cron.task_queue);
        cron.task_queue = NULL;
    }

    cron.state = CRON_STATE_STOPPED;
    xSemaphoreGive(s_mutex);

    cron_job_clear_all();
    return 0;
}

int cron_start()
{
    cron_job_list_init();
    cron_ensure_mutex();

    xSemaphoreTake(s_mutex, portMAX_DELAY);

    // 幂等：已运行则直接返回成功
    if (cron.state == CRON_STATE_RUNNING) {
        xSemaphoreGive(s_mutex);
        return 0;
    }

    // 等待 STOPPING 完成（理论上不应发生，但防御性处理）
    if (cron.state == CRON_STATE_STOPPING) {
        xSemaphoreGive(s_mutex);
        return -1;
    }

    cron.task_queue = xQueueCreate(CONFIG_ESP_CRON_QUEUE_DEPTH, sizeof(cron_job*));
    if (!cron.task_queue) {
        xSemaphoreGive(s_mutex);
        return -1;
    }

    // P0-2: 清掉上次 stop 可能残留的 worker ack token（如上次超时强删），
    // 否则本次 stop 会误取旧 ack 直接跳过等待。
    xSemaphoreTake(s_worker_done, 0);

    if (xTaskCreate(cron_worker_task, "cron_worker", CONFIG_ESP_CRON_WORKER_STACK_SIZE,
                    cron.task_queue, tskIDLE_PRIORITY + 2, &cron.handle) != pdPASS) {
        vQueueDelete(cron.task_queue);
        cron.task_queue = NULL;
        xSemaphoreGive(s_mutex);
        return -1;
    }

    esp_timer_create_args_t timer_args = {
        .callback = timer_cb,
        .name = "cron_timer"
    };

    if (esp_timer_create(&timer_args, &cron.esp_timer) != ESP_OK) {
        vTaskDelete(cron.handle);
        cron.handle = NULL;
        vQueueDelete(cron.task_queue);
        cron.task_queue = NULL;
        xSemaphoreGive(s_mutex);
        return -1;
    }

    cron.state = CRON_STATE_RUNNING;
    xSemaphoreGive(s_mutex);

    schedule_next_timer();
    return 0;
}

int cron_job_schedule(cron_job* job)
{
    cron_job_list_init();
    cron_ensure_mutex();

    xSemaphoreTake(s_mutex, portMAX_DELAY);

    if (!job || !cron_job_has_loaded(job) || job->cancelled) {
        xSemaphoreGive(s_mutex);
        return -1;
    }

    time_t now;
    time(&now);

    job->next_execution = cron_next(&(job->expression), now);
    job->last_triggered_sec = -1;
    if (cron_job_exists_in_list(job->id))
        cron_job_list_remove(job->id);
    if (cron_job_list_insert(job) < 0) {
        printf("esp_cron: failed to insert job %d into schedule\n", job->id);
        xSemaphoreGive(s_mutex);
        return -1;
    }

    struct cron_job_node* node = cron_job_list_first();
    if (node && node->job == job)
        schedule_next_timer_locked();

    xSemaphoreGive(s_mutex);
    return 0;
}

// 内部版：调用方须持有 s_mutex
static int cron_job_unschedule_locked(cron_job* job)
{
    struct cron_job_node* head = cron_job_list_first();
    int was_head = (head && head->job == job) ? 1 : 0;

    int rc = 0;
    if (cron_job_exists_in_list(job->id))
        rc = cron_job_list_remove(job->id);

    // 移除的是下一个要触发的 job：立即重设定时器到新的最早时刻，
    // 避免 esp_timer 仍睡到被移除 job 的时间（多一次无谓唤醒）
    if (was_head && rc == 0)
        schedule_next_timer_locked();

    return rc;
}

int cron_job_unschedule(cron_job* job)
{
    cron_job_list_init();
    cron_ensure_mutex();
    if (!job)
        return -1;

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    int rc = cron_job_unschedule_locked(job);
    xSemaphoreGive(s_mutex);
    return rc;
}

int cron_job_load_expression(cron_job* job, const char* schedule)
{
    if (!job || !schedule || job->cancelled) // destroy 是终态
        return -1;

    memset(&(job->expression), 0, sizeof(job->expression));
    const char* error = NULL;
    cron_parse_expr(schedule, &(job->expression), &error);

    if (error) {
        printf("Failed to parse cron expression: %s\n", error);
        return -1;
    }

    job->loaded = true;
    return 0;
}

int cron_job_has_loaded(cron_job* job)
{
    return job && job->loaded;
}

time_t cron_job_seconds_until_next_execution()
{
    return cron.seconds_until_next_execution;
}
