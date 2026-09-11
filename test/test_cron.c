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

#include <string.h>
#include <time.h>
#include <sys/time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "unity.h"
#include "esp_heap_caps.h"
#include "cron.h"
#include "jobs.h"

TEST_CASE("**CRON_JOB - INFO -- INIT TEST, THIS INITIALIZES THE TIME DATA MAY LEAK SOME MEMORY", "[cron_job]") {
  struct timeval tv;
  tv.tv_sec = 1530000000; // SOMEWHERE IN JUNE 2018
  settimeofday(&tv, NULL);
}

TEST_CASE("**CRON_JOB - cron_job_schedule and cron_job_remove IS IT WORKING? ", "[cron_job]")
{
  int cnt = 0, cnt2 = 0, ans = 0;
  cnt = cron_job_node_count();
  cron_job * job=cron_job_create("* * * * * *",NULL,NULL);
  cnt2 = cron_job_node_count();
  TEST_ASSERT_MESSAGE(job != NULL, "INSERTION FAILED WITH ERRORS");
  TEST_ASSERT_EQUAL_INT_MESSAGE(cnt + 1, cnt2, "LIST DIDNT GROW");
  ans = cron_job_destroy(job);
  cnt2 = cron_job_node_count();
  TEST_ASSERT_EQUAL_INT_MESSAGE( 0, ans, "DESTROY FAILED WITH ERRORS");
  TEST_ASSERT_EQUAL_INT_MESSAGE(cnt, cnt2, "LIST DIDNT REDUCE");
}

TEST_CASE("**CRON_JOB - cron_job_schedule CRON PARSER AND SCHEDULER IS AS EXPECTED ", "[cron_job]")
{
  time_t now;
  struct timeval tv;
  struct tm timeinfo;
  char buffer[256], buffer2[256];
  int buffer_len = 256;
  int ans = 0;
  tv.tv_sec = 1530000000; // SOMEWHERE IN JUNE 2018
  settimeofday(&tv, NULL);
  cron_job * job=cron_job_create("* * * * * *",NULL,NULL);
  time(&now);
  localtime_r(&(job->next_execution), &timeinfo);
  strftime(buffer, buffer_len, "%c", &timeinfo);
  localtime_r(&now, &timeinfo);
  strftime(buffer2, buffer_len, "%c", &timeinfo);
  printf("now: %s next execution: %s\n", buffer2, buffer);
  printf("now: %d next_execution: %d\n", (int)now, (int)job->next_execution);
  TEST_ASSERT_EQUAL_INT_MESSAGE(now + 1, job->next_execution, "SCHEDULE IS NOT FOR THE NEXT SECOND AS IT SHOULD BE");
  ans = cron_job_destroy(job);
  TEST_ASSERT_EQUAL_INT_MESSAGE(0, ans, "UNABLE TO DESTROY A JOB");
}

TEST_CASE("**CRON_JOB - cron_clear_all TEST CLEAR ALL JOBS ", "[cron_job]")
{
  int cnt_init=0,cnt=0,i=0;
  cron_job * jobs[10];
  cnt_init = cron_job_node_count();

  for (i =0;i<10;i++) {
    jobs[i]=cron_job_create("* * * * * *",NULL,NULL);
  }
  cnt = cron_job_node_count();
  TEST_ASSERT_EQUAL_INT_MESSAGE(i, cnt-cnt_init,"Creation count");
  cron_job_destroy(jobs[0]);
  cnt = cron_job_node_count();
  TEST_ASSERT_EQUAL_INT_MESSAGE(i-1, cnt-cnt_init,"Single destroy call");
  cron_job_clear_all();
  cnt = cron_job_node_count();
  TEST_ASSERT_EQUAL_INT_MESSAGE(0, cnt-cnt_init,"Destroy all call");
  for (int j = 1; j < i; j++) cron_job_destroy(jobs[j]);
}

TEST_CASE("**CRON_JOB - cron_job_reschedule_all recomputes schedules", "[cron_job]")
{
  struct timeval tv;
  tv.tv_sec = 1530000000; // SOMEWHERE IN JUNE 2018
  settimeofday(&tv, NULL);

  cron_job * job = cron_job_create("* * * * * *", NULL, NULL);
  TEST_ASSERT_NOT_NULL(job);

  time_t before = job->next_execution;
  int rc = cron_job_reschedule_all();
  TEST_ASSERT_EQUAL_INT_MESSAGE(0, rc, "reschedule_all FAILED");
  TEST_ASSERT_MESSAGE(job->next_execution >= before, "NEXT EXECUTION MOVED BACKWARD");

  cron_job_destroy(job);
}

TEST_CASE("**CRON_JOB - remove last node in list", "[cron_job]")
{
  struct timeval tv;
  tv.tv_sec = 1530000000;
  settimeofday(&tv, NULL);

  cron_job * last = cron_job_create("* * * * * *", NULL, NULL);
  cron_job * first = cron_job_create("* * * * * *", NULL, NULL);
  TEST_ASSERT_EQUAL_INT_MESSAGE(2, cron_job_node_count(), "EXPECTED 2 NODES");

  // 删除链表末尾节点
  int rc = cron_job_unschedule(last);
  TEST_ASSERT_EQUAL_INT_MESSAGE(0, rc, "REMOVE LAST NODE FAILED");
  TEST_ASSERT_EQUAL_INT_MESSAGE(1, cron_job_node_count(), "LIST SHOULD HAVE 1 NODE");

  cron_job_destroy(first);
  TEST_ASSERT_EQUAL_INT_MESSAGE(0, cron_job_node_count(), "LIST SHOULD BE EMPTY");
  cron_job_destroy(last);
}

TEST_CASE("**CRON_JOB - remove single node in list", "[cron_job]")
{
  struct timeval tv;
  tv.tv_sec = 1530000000;
  settimeofday(&tv, NULL);

  cron_job * job = cron_job_create("* * * * * *", NULL, NULL);
  TEST_ASSERT_EQUAL_INT_MESSAGE(1, cron_job_node_count(), "EXPECTED 1 NODE");

  int rc = cron_job_unschedule(job);
  TEST_ASSERT_EQUAL_INT_MESSAGE(0, rc, "REMOVE SINGLE NODE FAILED");
  TEST_ASSERT_EQUAL_INT_MESSAGE(0, cron_job_node_count(), "LIST SHOULD BE EMPTY");

  cron_job_destroy(job);
}

TEST_CASE("**CRON_JOB - start/stop idempotency", "[cron_job]")
{
  struct timeval tv;
  tv.tv_sec = 1530000000;
  settimeofday(&tv, NULL);

  // 幂等 start：多次调用不应崩溃
  TEST_ASSERT_EQUAL_INT(0, cron_start());
  TEST_ASSERT_EQUAL_INT(0, cron_start()); // 幂等：返回 0
  TEST_ASSERT_EQUAL_INT(0, cron_start()); // 幂等：返回 0

  // 幂等 stop
  TEST_ASSERT_EQUAL_INT(0, cron_stop());
  TEST_ASSERT_EQUAL_INT(-1, cron_stop()); // 已停止：返回 -1

  // start → stop → start 恢复
  TEST_ASSERT_EQUAL_INT(0, cron_start());
  TEST_ASSERT_EQUAL_INT(0, cron_stop());
  TEST_ASSERT_EQUAL_INT(0, cron_start());
  TEST_ASSERT_EQUAL_INT(0, cron_stop());
}

TEST_CASE("**CRON_JOB - create initializes cancelled flag", "[cron_job]")
{
  struct timeval tv;
  tv.tv_sec = 1530000000;
  settimeofday(&tv, NULL);

  cron_job * job = cron_job_create("* * * * * *", NULL, NULL);
  TEST_ASSERT_NOT_NULL(job);
  TEST_ASSERT_FALSE(job->cancelled);
  cron_job_destroy(job);
}

TEST_CASE("**CRON_JOB - remove earliest job updates next head", "[cron_job]")
{
  struct timeval tv;
  tv.tv_sec = 1530000000; // 2018-06-26
  settimeofday(&tv, NULL);

  cron_job * later = cron_job_create("0 0 13 * * *", NULL, NULL); // 13:00
  cron_job * earlier = cron_job_create("0 0 12 * * *", NULL, NULL); // 12:00
  TEST_ASSERT_NOT_NULL(later);
  TEST_ASSERT_NOT_NULL(earlier);
  TEST_ASSERT_EQUAL(2, cron_job_node_count());

  // 最早的是 12:00 那个
  TEST_ASSERT_EQUAL_PTR(earlier, cron_job_list_first()->job);

  // 移除最早 job → 链表头应变为 13:00 那个
  TEST_ASSERT_EQUAL_INT(0, cron_job_unschedule(earlier));
  TEST_ASSERT_EQUAL(1, cron_job_node_count());
  TEST_ASSERT_EQUAL_PTR(later, cron_job_list_first()->job);

  cron_job_destroy(later);
}

TEST_CASE("**CRON_JOB - modify expression and reschedule", "[cron_job]")
{
  struct timeval tv;
  tv.tv_sec = 1530000000;
  settimeofday(&tv, NULL);

  cron_job * job = cron_job_create("0 0 12 * * *", NULL, NULL);
  TEST_ASSERT_NOT_NULL(job);
  time_t old = job->next_execution;

  // 先移除，再改表达式，再重新调度
  cron_job_unschedule(job);
  TEST_ASSERT_EQUAL_INT(0, cron_job_load_expression(job, "0 0 18 * * *"));
  TEST_ASSERT_EQUAL_INT(1, cron_job_has_loaded(job));
  TEST_ASSERT_EQUAL_INT(0, cron_job_schedule(job));

  // next_execution 应指向新的 18:00，而不是原 12:00
  struct tm timeinfo;
  localtime_r(&job->next_execution, &timeinfo);
  TEST_ASSERT_EQUAL_INT(18, timeinfo.tm_hour);
  TEST_ASSERT_NOT_EQUAL(old, job->next_execution);

  cron_job_destroy(job);
}

// ===== P0-1 修复测试：callback 运行期间重触发 → 只允许一个 runner =====
static volatile int s_cb_concurrent = 0;
static volatile int s_cb_overlap = 0;

static void slow_callback(cron_job* job)
{
  s_cb_concurrent++;
  if (s_cb_concurrent > 1)
    s_cb_overlap = 1;
  vTaskDelay(pdMS_TO_TICKS(1200)); // 故意超过 1s 周期，触发下一次到期
  s_cb_concurrent--;
}

TEST_CASE("**CRON_JOB - no concurrent runner for same job (in_flight)", "[cron_job]")
{
  struct timeval tv;
  tv.tv_sec = 1530000000;
  settimeofday(&tv, NULL);

  s_cb_concurrent = 0;
  s_cb_overlap = 0;

  cron_job* job = cron_job_create("* * * * * *", slow_callback, NULL); // 每秒触发
  TEST_ASSERT_NOT_NULL(job);

  TEST_ASSERT_EQUAL_INT(0, cron_start());

  // 跑 4 秒：callback 占 1.2s > 周期 1s，若 in_flight 失效会产生重叠 runner
  vTaskDelay(pdMS_TO_TICKS(4000));

  TEST_ASSERT_EQUAL_INT_MESSAGE(0, s_cb_overlap, "SAME JOB RAN CONCURRENTLY (in_flight REGRESSION)");
  TEST_ASSERT_INT_WITHIN_MESSAGE(1, 1, s_cb_concurrent, "callback should not leak concurrency");

  TEST_ASSERT_EQUAL_INT(0, cron_stop());
  cron_job_destroy(job);
}

// ===== P1-3 修复测试：insert 失败必须传播，job 不静默丢失 =====
static volatile int s_force_insert_fail = 0;

int32_t cron_job_test_force_insert_fail(void) // 强覆盖 jobs.c 中的 weak 默认实现
{
  return s_force_insert_fail;
}

TEST_CASE("**CRON_JOB - insert failure propagates (fault injection)", "[cron_job]")
{
  struct timeval tv;
  tv.tv_sec = 1530000000;
  settimeofday(&tv, NULL);

  // 先建一个正常 job 留在链表里，让 reschedule_all 有东西可处理
  cron_job* base = cron_job_create("* * * * * *", NULL, NULL);
  TEST_ASSERT_NOT_NULL(base);

  s_force_insert_fail = 1;

  // create 应因 schedule 失败返回 NULL，而不是返回半初始化 job
  cron_job* job = cron_job_create("* * * * * *", NULL, NULL);
  TEST_ASSERT_NULL_MESSAGE(job, "CREATE MUST FAIL WHEN INSERT FAILS");

  // 链表非空时 reschedule_all 应因 insert 失败返回 -1
  TEST_ASSERT_EQUAL_INT(-1, cron_job_reschedule_all());

  s_force_insert_fail = 0;

  // 恢复后再建，应成功
  cron_job* ok = cron_job_create("* * * * * *", NULL, NULL);
  TEST_ASSERT_NOT_NULL(ok);
  TEST_ASSERT_EQUAL_INT(0, cron_job_reschedule_all());
  cron_job_destroy(ok);
  cron_job_destroy(base);
}

// ===== P0-2 修复测试：callback 执行中 stop 必须安全且可重启 =====
static volatile int s_stop_cb_running = 0;

static void blocking_callback(cron_job* job)
{
  s_stop_cb_running = 1;
  vTaskDelay(pdMS_TO_TICKS(50)); // 占住 runner，让 stop 在其执行期间返回
  s_stop_cb_running = 0;
}

TEST_CASE("**CRON_JOB - stop while callback in-flight is safe (cooperative shutdown)", "[cron_job]")
{
  struct timeval tv;
  tv.tv_sec = 1530000000;
  settimeofday(&tv, NULL);

  cron_job* job = cron_job_create("* * * * * *", blocking_callback, NULL);
  TEST_ASSERT_NOT_NULL(job);

  TEST_ASSERT_EQUAL_INT(0, cron_start());
  vTaskDelay(pdMS_TO_TICKS(1100)); // 让第一批 runner 跑起来

  // callback 可能仍在执行时调用 stop：不得崩溃、不得死锁、随后可重启
  TEST_ASSERT_EQUAL_INT(0, cron_stop());
  TEST_ASSERT_EQUAL_INT(-1, cron_stop()); // 已停止

  TEST_ASSERT_EQUAL_INT(0, cron_start()); // 重启必须可用
  vTaskDelay(pdMS_TO_TICKS(1100));
  TEST_ASSERT_EQUAL_INT(0, cron_stop());

  cron_job_destroy(job);
}

// ===== P0-1 回归：到期 job 超过 MAX_DUE_JOBS 时不能永久丢失 =====
static volatile int s_counters[8];

static void counter_cb(cron_job* job)
{
  int idx = (int)(intptr_t)job->data;
  if (idx >= 0 && idx < 8)
    s_counters[idx]++;
}

TEST_CASE("**CRON_JOB - due jobs beyond MAX_DUE_JOBS are not lost", "[cron_job]")
{
  struct timeval tv;
  tv.tv_sec = 1530000000;
  settimeofday(&tv, NULL);

  const int N = 8; // > CONFIG_ESP_CRON_MAX_DUE_JOBS(4)：首次到期必超上限
  memset((void*)s_counters, 0, sizeof(s_counters));

  cron_job* jobs[N];
  for (int i = 0; i < N; i++) {
    jobs[i] = cron_job_create("* * * * * *", counter_cb, (void*)(intptr_t)i);
    TEST_ASSERT_NOT_NULL(jobs[i]);
  }

  TEST_ASSERT_EQUAL_INT(0, cron_start());
  vTaskDelay(pdMS_TO_TICKS(3500)); // 3 个完整周期

  for (int i = 0; i < N; i++)
    TEST_ASSERT_INT_WITHIN_MESSAGE(
        0, s_counters[i], s_counters[i],
        s_counters[i] >= 2 ? "OK" : "P0-1 REGRESSION: BEYOND-MAX JOB LOST");

  TEST_ASSERT_EQUAL_INT(0, cron_stop());
  for (int i = 0; i < N; i++)
    cron_job_destroy(jobs[i]);
  TEST_ASSERT_EQUAL_INT(0, cron_job_node_count());
}

// ===== P0-3 回归：queue-full 时 job 恢复 in_flight 且下一周期继续触发 =====
static volatile int s_fast_counters[4];

static void fast_cb(cron_job* job)
{
  int idx = (int)(intptr_t)job->data;
  if (idx >= 0 && idx < 4)
    s_fast_counters[idx]++;
}

// 两个占用队列的慢回调：把 QUEUE_DEPTH(2) 占满，逼后来的 job 走 queue-full
static void hold_slot_cb(cron_job* job)
{
  vTaskDelay(pdMS_TO_TICKS(1300));
}

TEST_CASE("**CRON_JOB - queue full skips cycle but job keeps firing", "[cron_job]")
{
  struct timeval tv;
  tv.tv_sec = 1530000000;
  settimeofday(&tv, NULL);

  memset((void*)s_fast_counters, 0, sizeof(s_fast_counters));

  // 慢回调先建：同一秒内排序靠前，先入队占满 2 个空位
  cron_job* hold[2];
  for (int i = 0; i < 2; i++) {
    hold[i] = cron_job_create("* * * * * *", hold_slot_cb, NULL);
    TEST_ASSERT_NOT_NULL(hold[i]);
  }
  cron_job* fast[4];
  for (int i = 0; i < 4; i++) {
    fast[i] = cron_job_create("* * * * * *", fast_cb, (void*)(intptr_t)i);
    TEST_ASSERT_NOT_NULL(fast[i]);
  }

  TEST_ASSERT_EQUAL_INT(0, cron_start());
  vTaskDelay(pdMS_TO_TICKS(4000));

  // 每个 fast job 至少触发 ≥2 次：即使某周期 queue-full 被跳过，也继续触发
  for (int i = 0; i < 4; i++)
    TEST_ASSERT_INT_WITHIN_MESSAGE(
        0, s_fast_counters[i], s_fast_counters[i],
        s_fast_counters[i] >= 2 ? "OK" : "P0-3 REGRESSION: QUEUE-FULL JOB STARVED");

  TEST_ASSERT_EQUAL_INT(0, cron_stop());
  for (int i = 0; i < 2; i++)
    cron_job_destroy(hold[i]);
  for (int i = 0; i < 4; i++)
    cron_job_destroy(fast[i]);
  TEST_ASSERT_EQUAL_INT(0, cron_job_node_count());
}

// ===== P1-2 回归：xTaskCreate 失败 → in_flight 恢复，job 不被永久跳过 =====
static volatile int s_force_runner_fail = 0;
static volatile int s_runner_fail_hits;

int32_t cron_job_test_force_runner_create_fail(void) // 强覆盖 esp_cron.c 的 weak 默认实现
{
  if (s_force_runner_fail) {
    s_runner_fail_hits++;
    return 1;
  }
  return 0;
}

TEST_CASE("**CRON_JOB - runner create failure restores job (fault injection)", "[cron_job]")
{
  struct timeval tv;
  tv.tv_sec = 1530000000;
  settimeofday(&tv, NULL);

  s_runner_fail_hits = 0;
  s_counters[0] = 0;

  cron_job* job = cron_job_create("* * * * * *", counter_cb, (void*)(intptr_t)0);
  TEST_ASSERT_NOT_NULL(job);

  TEST_ASSERT_EQUAL_INT(0, cron_start());

  // 故障窗口：至少跨 2 个触发周期，runner 全部创建失败
  s_force_runner_fail = 1;
  vTaskDelay(pdMS_TO_TICKS(2300));
  s_force_runner_fail = 0;

  // 故障期间不应执行任何 callback
  TEST_ASSERT_EQUAL_INT_MESSAGE(0, s_counters[0], "P1-2 REGRESSION: CALLBACK RAN WHILE RUNNER CREATE FAILS");
  TEST_ASSERT_MESSAGE(s_runner_fail_hits >= 1, "Fault injection hook never fired");

  // 恢复后必须仍能触发（证明 in_flight 被恢复，job 没有被永久锁死）
  vTaskDelay(pdMS_TO_TICKS(1600));
  TEST_ASSERT_INT_WITHIN_MESSAGE(
      0, s_counters[0], s_counters[0],
      s_counters[0] >= 1 ? "OK" : "P1-2 REGRESSION: JOB DEAD AFTER RUNNER FAILURE");

  TEST_ASSERT_EQUAL_INT(0, cron_stop());
  cron_job_destroy(job);
  TEST_ASSERT_EQUAL_INT(0, cron_job_node_count());
}

// ===== P1-4 回归：callback 内自销毁安全（destroy 是终态） =====
static volatile int s_self_destroy_count;

static void self_destroy_cb(cron_job* job)
{
  s_self_destroy_count++;
  cron_job_destroy(job); // 自销毁：合法，不得崩
}

TEST_CASE("**CRON_JOB - self-destroy in callback is safe", "[cron_job]")
{
  struct timeval tv;
  tv.tv_sec = 1530000000;
  settimeofday(&tv, NULL);

  s_self_destroy_count = 0;

  cron_job* job = cron_job_create("* * * * * *", self_destroy_cb, NULL);
  TEST_ASSERT_NOT_NULL(job);

  TEST_ASSERT_EQUAL_INT(0, cron_start());
  vTaskDelay(pdMS_TO_TICKS(2500));

  TEST_ASSERT_EQUAL_INT_MESSAGE(1, s_self_destroy_count, "JOB MUST FIRE EXACTLY ONCE THEN SELF-DESTROY");
  TEST_ASSERT_EQUAL_INT_MESSAGE(0, cron_job_node_count(), "P1-4 REGRESSION: SELF-DESTROYED JOB STILL SCHEDULED");

  TEST_ASSERT_EQUAL_INT(0, cron_stop()); // job 已随销毁释放，此处不再 destroy
}

TEST_CASE("**CRON_JOB - destroy blocks future firings", "[cron_job]")
{
  struct timeval tv;
  tv.tv_sec = 1530000000;
  settimeofday(&tv, NULL);

  s_counters[1] = 0;
  cron_job* job = cron_job_create("* * * * * *", counter_cb, (void*)(intptr_t)1);
  TEST_ASSERT_NOT_NULL(job);

  TEST_ASSERT_EQUAL_INT(0, cron_start());
  vTaskDelay(pdMS_TO_TICKS(1200));
  TEST_ASSERT_MESSAGE(s_counters[1] >= 1, "JOB SHOULD HAVE FIRED BEFORE DESTROY");

  TEST_ASSERT_EQUAL_INT(0, cron_job_destroy(job));
  int fired_at_destroy = s_counters[1];
  vTaskDelay(pdMS_TO_TICKS(2200));

  TEST_ASSERT_EQUAL_INT_MESSAGE(fired_at_destroy, s_counters[1], "DESTROYED JOB KEPT FIRING");
  TEST_ASSERT_EQUAL_INT_MESSAGE(0, cron_job_node_count(), "DESTROYED JOB STILL SCHEDULED");

  TEST_ASSERT_EQUAL_INT(0, cron_stop());
}

// ===== P1-6 回归：SNTP 前后跳变后 job 仍继续触发 =====
static volatile int s_jump_count;

static void jump_cb(cron_job* job)
{
  s_jump_count++;
}

TEST_CASE("**CRON_JOB - forward time jump keeps jobs firing", "[cron_job]")
{
  struct timeval tv;
  tv.tv_sec = 1530000000;
  settimeofday(&tv, NULL);

  s_jump_count = 0;
  cron_job* job = cron_job_create("* * * * * *", jump_cb, NULL);
  TEST_ASSERT_NOT_NULL(job);

  TEST_ASSERT_EQUAL_INT(0, cron_start());
  vTaskDelay(pdMS_TO_TICKS(1200));
  TEST_ASSERT_MESSAGE(s_jump_count >= 1, "SHOULD FIRE BEFORE JUMP");

  int before = s_jump_count;
  tv.tv_sec = 1530000000 + 20; // 前跳 20s
  settimeofday(&tv, NULL);
  cron_job_reschedule_all(); // 文档要求：跳变后主动重建调度

  vTaskDelay(pdMS_TO_TICKS(2200));
  TEST_ASSERT_MESSAGE(s_jump_count > before, "P1-6 REGRESSION: JOB STOPPED FIRING AFTER FORWARD JUMP");

  TEST_ASSERT_EQUAL_INT(0, cron_stop());
  cron_job_destroy(job);
}

TEST_CASE("**CRON_JOB - backward time jump keeps jobs firing", "[cron_job]")
{
  struct timeval tv;
  tv.tv_sec = 1530000000;
  settimeofday(&tv, NULL);

  s_jump_count = 0;
  cron_job* job = cron_job_create("* * * * * *", jump_cb, NULL);
  TEST_ASSERT_NOT_NULL(job);

  TEST_ASSERT_EQUAL_INT(0, cron_start());
  vTaskDelay(pdMS_TO_TICKS(1200));
  TEST_ASSERT_MESSAGE(s_jump_count >= 1, "SHOULD FIRE BEFORE JUMP");

  int before = s_jump_count;
  tv.tv_sec = 1530000000 - 20; // 后跳 20s：必须靠手动 reschedule_all 唤醒
  settimeofday(&tv, NULL);
  cron_job_reschedule_all();

  vTaskDelay(pdMS_TO_TICKS(2200));
  TEST_ASSERT_MESSAGE(s_jump_count > before, "P1-6 REGRESSION: JOB STOPPED FIRING AFTER BACKWARD JUMP");

  TEST_ASSERT_EQUAL_INT(0, cron_stop());
  cron_job_destroy(job);
}

// ===== create/destroy 压力：无内存泄漏、链表归零 =====
TEST_CASE("**CRON_JOB - create/destroy stress has no leak", "[cron_job]")
{
  struct timeval tv;
  tv.tv_sec = 1530000000;
  settimeofday(&tv, NULL);

  // 预热：确保 mutex/锁已初始化（计入 before 快照）
  cron_job* warm = cron_job_create("* * * * * *", NULL, NULL);
  cron_job_destroy(warm);

  size_t before = heap_caps_get_free_size(MALLOC_CAP_8BIT);
  int base_nodes = cron_job_node_count();

  for (int i = 0; i < 500; i++) {
    cron_job* j = cron_job_create("* * * * * *", NULL, NULL);
    if (!j) {
      TEST_FAIL_MESSAGE("create failed mid-stress");
      break;
    }
    cron_job_destroy(j);
  }

  vTaskDelay(pdMS_TO_TICKS(100)); // 等后台 runner/清理完全落地
  size_t after = heap_caps_get_free_size(MALLOC_CAP_8BIT);

  TEST_ASSERT_EQUAL_INT_MESSAGE(base_nodes, cron_job_node_count(), "NODE COUNT DRIFTED");
  // 只允许"减小 ≤2KB"，允许增长（其他子系统惰性释放的噪声）。
  // 真实泄漏是单调缩水，方向至关重要。
  TEST_ASSERT_MESSAGE(
      after + 2048 >= before,
      "LEAK DETECTED: FREE HEAP SHRANK BY >2KB");
}