#pragma once
#include <stdbool.h>

/* Synchronous diagnostic command, executed only by the serial-config task. */
void watch_imu_capture_run(unsigned seconds, unsigned delay_ms, bool (*write_line)(const char *));
bool watch_imu_capture_active(void);
/* Main UI suspends new captures while asleep and cancels/waits for restoration. */
void watch_imu_capture_suspend(bool suspend);
