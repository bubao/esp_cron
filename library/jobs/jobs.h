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

#ifndef _ESP_CRON_JOBS_LINKED_LIST
#define _ESP_CRON_JOBS_LINKED_LIST
#include <time.h>
#include "cron.h"

struct cron_job_node {
  struct cron_job_node * next;
  cron_job * job;
};

/*
*  SUMMARY: Returns the first element in the linked list.
*
*  RETURNS:  first node or NULL
*/
struct cron_job_node * cron_job_list_first();

/*
*  SUMMARY: Adds a job to the list in execution order.
*  The job id must already be assigned by the caller.
*
*  PARAMS: job
*
*  RETURNS: job id or -1 on error
*/
int cron_job_list_insert(cron_job * job);

/*
*  SUMMARY: Removes a node from the list by job id.
*
*  RETURNS: 0 on success, -1 on not found
*/
int cron_job_list_remove(int id);

/*
*  SUMMARY: Counts elements on list. O(n)
*
*  RETURNS: number of nodes on list
*/
int cron_job_node_count();

/*
*  SUMMARY: Initializes the module structures (like mutex). Safe to call multiple times.
*
*  RETURNS: nothing
*/
void cron_job_list_init();

#endif