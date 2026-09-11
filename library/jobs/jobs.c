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
#include "jobs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <stdlib.h>

// --- 故障注入测试钩子（P1-3）---
// 弱符号，默认不注入。测试二进制可通过强覆盖该符号返回非 0，
// 强制让 cron_job_list_insert() 走失败路径，验证 insert 错误被传播。
__attribute__((weak)) int32_t cron_job_test_force_insert_fail(void)
{
    return 0;
}

// STATIC STRUCTS
static struct
{
    struct cron_job_node* first;
    SemaphoreHandle_t semaphore;
    int init;
} linked_list_state = {
    .first = NULL,
    .semaphore = NULL,
    .init = 0
};

void cron_job_list_init()
{
    if (linked_list_state.init == 0) {
        linked_list_state.semaphore = xSemaphoreCreateMutex();
        linked_list_state.init = 1;
    }
}

struct cron_job_node* cron_job_list_first()
{
    return linked_list_state.first;
}

/* 迭代插入，避免递归栈溢出 */
static struct cron_job_node* _cron_job_list_insert(struct cron_job_node* head, struct cron_job_node* new_node)
{
    if (head == NULL || new_node->job->next_execution < head->job->next_execution) {
        new_node->next = head;
        return new_node;
    }

    struct cron_job_node* current = head;
    while (current->next != NULL && current->next->job->next_execution <= new_node->job->next_execution) {
        current = current->next;
    }

    new_node->next = current->next;
    current->next = new_node;
    return head;
}

int cron_job_list_insert(cron_job* job)
{
    if (linked_list_state.semaphore == NULL)
        cron_job_list_init();
    if (job == NULL)
        return -1;
    if (cron_job_test_force_insert_fail())
        return -1;

    struct cron_job_node* new_node = calloc(1, sizeof(struct cron_job_node));
    if (new_node == NULL)
        return -1;

    new_node->job = job;
    if (xSemaphoreTake(linked_list_state.semaphore, (TickType_t)10) == pdTRUE) {
        linked_list_state.first = _cron_job_list_insert(linked_list_state.first, new_node);
        xSemaphoreGive(linked_list_state.semaphore);
    } else {
        free(new_node);
        return -1;
    }
    return new_node->job->id;
}

int cron_job_list_remove(int id)
{
    if (linked_list_state.semaphore == NULL)
        cron_job_list_init();

    if (xSemaphoreTake(linked_list_state.semaphore, (TickType_t)10) != pdTRUE)
        return -1;

    struct cron_job_node* node = linked_list_state.first;
    struct cron_job_node* prev_node = NULL;

    while (node) {
        if (node->job->id == id) {
            if (prev_node == NULL)
                linked_list_state.first = node->next;
            else
                prev_node->next = node->next;
            free(node);
            node = NULL;
            xSemaphoreGive(linked_list_state.semaphore);
            return 0;
        }
        prev_node = node;
        node = node->next;
    }

    xSemaphoreGive(linked_list_state.semaphore);
    return -1;
}

int cron_job_node_count()
{
    int cnt = 0;
    struct cron_job_node* node = cron_job_list_first();
    while (node) {
        cnt++;
        node = node->next;
    }
    return cnt;
}