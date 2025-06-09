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
#include "jobs.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define MIN_DELAY_US 1000 // 1ms，或根据需求调整

typedef struct {
    unsigned char running;
    TaskHandle_t handle;
    time_t seconds_until_next_execution;
    QueueHandle_t task_queue;
    esp_timer_handle_t esp_timer;
    int next_id;
} cron_state_t;

static cron_state_t state = {
    .running = 0,
    .handle = NULL,
    .seconds_until_next_execution = -1,
    .task_queue = NULL,
    .esp_timer = NULL,
    .next_id = 1,
};

// ====================== 内部工具 ========================

static void schedule_next_timer();

// 判断 job 是否在链表中
static int cron_job_exists_in_list(int id)
{
    struct cron_job_node* node = cron_job_list_first();
    while (node) {
        if (node->job && node->job->id == id) {
            return 1;
        }
        node = node->next;
    }
    return 0;
}

// 内部：不自动调度定时器的 job schedule
static int cron_job_schedule_nosched(cron_job* job)
{
    cron_job_list_init();
    if (!job || !cron_job_has_loaded(job))
        return -1;
    time_t now;
    time(&now);
    job->next_execution = cron_next(&(job->expression), now);
    job->last_triggered_sec = -1;
    if (cron_job_exists_in_list(job->id)) {
        cron_job_list_remove(job->id);
    }
    cron_job_list_insert(job);
    return 0;
}

// ====================== 定时器回调 ========================

static void timer_cb(void* arg)
{
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    time_t now;
    time(&now);
    time_t now_sec = now;

    cron_job* due_jobs[16];
    int due_count = 0;
    while (1) {
        struct cron_job_node* node = cron_job_list_first();
        if (!node || node->job->next_execution > now)
            break;
        cron_job* job = node->job;
        // 防抖：同一秒只触发一次
        if (job->last_triggered_sec == now_sec) {
            cron_job_list_remove(job->id);
            if (due_count < 16)
                due_jobs[due_count++] = job;
            continue;
        }
        job->last_triggered_sec = now_sec;
        if (state.task_queue) {
            xQueueSendFromISR(state.task_queue, &job, &xHigherPriorityTaskWoken);
        }
        cron_job_list_remove(job->id);
        if (due_count < 16)
            due_jobs[due_count++] = job;
    }
    for (int i = 0; i < due_count; ++i) {
        cron_job_schedule_nosched(due_jobs[i]); // 不自动调度定时器
    }
    schedule_next_timer(); // 只调度一次
    if (xHigherPriorityTaskWoken)
        portYIELD_FROM_ISR();
}

static void schedule_next_timer()
{
    struct cron_job_node* node = cron_job_list_first();
    cron_job* job = node ? node->job : NULL;
    if (!job) {
        if (state.esp_timer)
            esp_timer_stop(state.esp_timer);
        return;
    }

    time_t now;
    time(&now);

    int64_t delay_us = (job->next_execution - now) * 1000000;
    if (delay_us < MIN_DELAY_US)
        delay_us = MIN_DELAY_US;

    esp_timer_stop(state.esp_timer);
    esp_timer_start_once(state.esp_timer, delay_us);

    state.seconds_until_next_execution = job->next_execution - now;
}

// ====================== worker task ========================

static void job_runner_task(void* arg)
{
    cron_job* job = (cron_job*)arg;

    TickType_t start_tick = xTaskGetTickCount();

    if (job && job->callback) {
        job->callback(job);
    }

    TickType_t elapsed = xTaskGetTickCount() - start_tick;
    if (elapsed > pdMS_TO_TICKS(5000)) { // 比如超过 5 秒
        printf("Warning: cron job %d callback took too long \n", job->id);
    }

    vTaskDelete(NULL);
}

static void cron_worker_task(void* arg)
{
    cron_job* job = NULL;
    while (1) {
        if (state.task_queue && xQueueReceive(state.task_queue, &job, portMAX_DELAY)) {
            if (job) {
                // 在单独任务中执行 job.callback
                xTaskCreate(job_runner_task, "job_runner", 4096, job, tskIDLE_PRIORITY + 1, NULL);
            }
        }
    }
}

// ====================== API 实现 ========================

cron_job* cron_job_create(const char* schedule, cron_job_callback callback, void* data)
{
    cron_job* job = calloc(1, sizeof(cron_job));
    if (!job) {
        printf("Failed to allocate memory for cron job\n");
        return NULL;
    }

    job->callback = callback;
    job->data = data;
    job->id = state.next_id++;

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
    cron_job_unschedule(job);
    free(job);
    return 0;
}

int cron_job_clear_all()
{
    cron_job_list_init(); // 确保链表和信号量已初始化
    while (cron_job_list_first()) {
        cron_job_destroy(cron_job_list_first()->job);
    }
    return 0;
}

int cron_stop()
{
    if (!state.running)
        return -1;

    state.running = 0;
    if (state.handle) {
        vTaskDelete(state.handle);
        state.handle = NULL;
    }

    if (state.esp_timer) {
        esp_timer_stop(state.esp_timer);
        esp_timer_delete(state.esp_timer);
        state.esp_timer = NULL;
    }

    if (state.task_queue) {
        vQueueDelete(state.task_queue);
        state.task_queue = NULL;
    }

    cron_job_clear_all();
    return 0;
}

int cron_start()
{
    cron_job_list_init(); // 确保链表和信号量已初始化
    if (state.running || state.handle)
        return -1;

    state.task_queue = xQueueCreate(10, sizeof(cron_job*));
    if (!state.task_queue) {
        printf("Failed to create task queue\n");
        return -1;
    }

    if (xTaskCreate(cron_worker_task, "cron_worker", 4096, NULL, tskIDLE_PRIORITY + 2, &state.handle) != pdPASS) {
        printf("Failed to create cron worker task\n");
        vQueueDelete(state.task_queue);
        return -1;
    }

    esp_timer_create_args_t timer_args = {
        .callback = timer_cb,
        .name = "cron_timer"
    };

    if (esp_timer_create(&timer_args, &state.esp_timer) != ESP_OK) {
        vTaskDelete(state.handle);
        vQueueDelete(state.task_queue);
        return -1;
    }

    state.running = 1;
    schedule_next_timer();
    return 0;
}

int cron_job_schedule(cron_job* job)
{
    cron_job_list_init(); // 确保链表和信号量已初始化
    if (!job || !cron_job_has_loaded(job))
        return -1;

    time_t now;
    time(&now);

    job->next_execution = cron_next(&(job->expression), now);
    job->last_triggered_sec = -1; // 每次重新调度时重置
    // 防止重复插入，先判断是否存在再移除
    if (cron_job_exists_in_list(job->id)) {
        cron_job_list_remove(job->id);
    }
    cron_job_list_insert(job);

    // 如果当前 job 是下一个要执行的，重新调度定时器
    struct cron_job_node* node = cron_job_list_first();
    if (node && node->job == job) {
        schedule_next_timer();
    }

    return 0;
}

int cron_job_unschedule(cron_job* job)
{
    cron_job_list_init(); // 确保链表和信号量已初始化
    if (!job)
        return -1;
    if (cron_job_exists_in_list(job->id)) {
        return cron_job_list_remove(job->id);
    }
    return 0;
}

int cron_job_load_expression(cron_job* job, const char* schedule)
{
    if (!job || !schedule)
        return -1;

    memset(&(job->expression), 0, sizeof(job->expression));
    const char* error = NULL;
    cron_parse_expr(schedule, &(job->expression), &error);

    if (error) {
        printf("Failed to parse cron expression: %s\n", error);
        return -1;
    }

    job->load = &(job->expression);
    return 0;
}

int cron_job_has_loaded(cron_job* job)
{
    return job && (job->load == &(job->expression));
}

time_t cron_job_seconds_until_next_execution()
{
    return state.seconds_until_next_execution;
}

// CRON TASKS

void cron_schedule_job_launcher(void* args)
{
    if (args == NULL) {
        goto end;
    }
    cron_job* job = (cron_job*)args;
    job->callback(job);
    goto end;

end:
    vTaskDelete(NULL);
    return;
}

void cron_schedule_task(void* args)
{
    time_t now;
    cron_job* job = NULL;
    int r1 = 0; // RUN ONCE!!
                // IF ARGS ARE A STRING DEFINED AS R1
    if (args != NULL) {
        if (strncmp(args, "R1", 2) == 0) // OK I ADMIT IT, ITS NOT THE MOST BEAUTIFUL CODE EVER, BUT I NEED IT TO BE TESTABLE... DON'T WANT TO GROW OLD WAITING FOR TIME TO PASS... :P
            r1 = 1;
    }

    while (true) {
        state.running = 1;
        time(&now);
        job = cron_job_list_first()->job;
        if (job == NULL) {
            break; // THIS IS IT!!! THIS WILL
        }
        if (now >= job->next_execution) {
            /* Create the task, IT WILL KILL ITSELF AFTER THE JOB IS DONE. */
            xTaskCreatePinnedToCore(
                cron_schedule_job_launcher, /* Function that implements the task. */
                "cron_schedule_job_launcher", /* Text name for the task. */
                4096, /* Stack size in BYTES, not bytes. */
                (void*)job, /* Job is passed into the task. */
                tskIDLE_PRIORITY + 2, /* Priority at which the task is created. */
                (NULL), /* No need for the handle */
                tskNO_AFFINITY); /* No specific core */
            cron_job_list_remove(job->id); // There is mutex in there that can mess with our timing, but i am not sure if we should move this to the new task.
            cron_job_schedule(job); // There is mutex in there that can mess with our timing, but i am not sure if we should move this to the new task.
        } else {
            state.seconds_until_next_execution = job->next_execution - now;
            vTaskDelay((state.seconds_until_next_execution * 1000) / portTICK_PERIOD_MS);
        }
        if (r1 != 0) {
            break;
        }
    }
    cron_stop();
    return;
}
