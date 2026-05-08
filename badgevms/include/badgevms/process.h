/* This file is part of BadgeVMS
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <sys/types.h>

// Create a new process from the given filename, with argv, **argc, and a particular stack size.
pid_t process_create(char const *path, size_t stack_size, int argc, char **argv);

// Create a thread with from thead_entry(void* user_data), the user_data to send, and the stack size for the new thread.
pid_t thread_create(void (*thread_entry)(void *user_data), void *user_data, uint16_t stack_size);

// Wait for a child process or thread to terminate. The return value of wait is either -1 if the timeout passed, and
// blocking was requested, or the pid of the child process that terminated.
pid_t wait(bool block, uint32_t timeout_msec);

// Lower my priority
void task_priority_lower();

// Restore to previous priority
void task_priority_restore();

// Get the total number of running tasks.
uint32_t get_num_tasks();

// Translate a task-level fd (the virtual fd returned by accept/open in SDK app context)
// to the underlying real kernel fd. Returns -1 if task_fd is invalid.
int get_dev_fd(int task_fd);
