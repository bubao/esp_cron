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