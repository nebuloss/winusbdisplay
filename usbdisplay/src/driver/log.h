/* SPDX-License-Identifier: GPL-2.0-only
 *
 * A file log, because the alternatives do not work here.
 *
 * WDF collapses almost every initialisation failure into one generic status,
 * so the system event log will tell you "problem code 10" and nothing else.
 * A UMDF driver cannot print to a console and attaching a debugger to
 * WUDFHost is not something to do casually. This writes the exact NTSTATUS of
 * every framework call to a file instead, and it is the first thing to read
 * when the device will not start.
 *
 *     C:\Windows\Temp\usbdisplaydd.log
 */

#pragma once

namespace usbdisplay {

void LogReset();
void Log(const char* format, ...);

}  // namespace usbdisplay
