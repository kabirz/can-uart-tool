/*
 * Copyright (c) 2026 Kabirz.
 * SPDX-License-Identifier: Apache-2.0
 *
 * lora_sdk_at.h — Shared AT command helpers (internal)
 *
 * Common utilities used by both UDP and serial AT transport layers.
 */

#ifndef LORA_SDK_AT_H
#define LORA_SDK_AT_H

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "lora_sdk.h"

/* Forward declaration — full struct is in lora_sdk_internal.h */
struct lora_sdk;

/* ================================================================
 * AT command string helpers
 * ================================================================ */

/* Ensure AT command string has \r\n termination.
 * If cmd already ends with \r\n, copies as-is.
 * Otherwise appends \r\n.
 * Returns total length (including \r\n), or -1 on overflow. */
int sdk_at_ensure_crlf(const char *cmd, char *out, int out_size);

/* Check if an AT command is a query (trailing '?' after stripping \r\n).
 * cmd should already have \r\n termination. */
int sdk_at_is_query(const char *cmd);

/* ================================================================
 * AT response helpers
 * ================================================================ */

/* Trim trailing \r, \n, and space from an AT response in place.
 * Returns new length after trimming. */
int sdk_at_trim_response(char *buf, int len);

/* Dispatch AT response to callbacks: on_at_response + on_log.
 * 'response' should be a null-terminated string. */
void sdk_at_dispatch_response(struct lora_sdk *sdk, const char *response,
                               enum lora_sdk_log_source source);

/* ================================================================
 * Worker thread helper
 * ================================================================ */

/* Launch a background worker thread.
 * Allocates a copy of work_data and passes it to the worker.
 * The thread handle is tracked by 'sdk' so that sdk_at_join_workers()
 * can wait for it before resources (serial handle, sdk itself) are freed.
 * The worker function MUST free the work data when done.
 * NOTE: workers must not block indefinitely — they are joined on close/cleanup.
 * Returns 0 on success, -1 on allocation/thread-creation failure. */
int sdk_at_launch_worker(struct lora_sdk *sdk, LPTHREAD_START_ROUTINE worker,
                          const void *work_data, size_t work_size);

/* Block until every worker launched via sdk_at_launch_worker() has exited.
 * Uses a draining loop so workers that themselves launch nested workers
 * (e.g. sdk_at_reboot) are also collected. Safe to call when no workers
 * are active. */
void sdk_at_join_workers(struct lora_sdk *sdk);

/* Send AT+Z to reboot gateway. Handles 2s NV-save delay.
 * For serial transport, clears AT mode flag after reboot. */
void sdk_at_reboot(lora_sdk_t *sdk);

#endif /* LORA_SDK_AT_H */
