/* SPDX-License-Identifier: GPL-2.0-only
 *
 * Minimal file logger for the driver.
 *
 * UMDF hosts run as LOCAL SERVICE with no console, and WDF collapses most
 * initialisation failures into a single generic status in the event log
 * (EvtDeviceD0Entry failing shows up only as STATUS_DEVICE_POWER_FAILURE on
 * the start IRP). Without a log of its own there is no way to tell which call
 * actually failed.
 */

#pragma once

namespace ms912x {

/* Appends a timestamped line. Never throws, never fails the caller. */
void Log(const char* format, ...);

/* Starts a fresh log file for this load. */
void LogReset();

}  // namespace ms912x
