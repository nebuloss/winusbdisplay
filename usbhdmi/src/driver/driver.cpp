/* SPDX-License-Identifier: GPL-2.0-only
 *
 * Driver entry point and the indirect display callbacks.
 *
 * There is no DDC/CI here, and its absence is deliberate. The previous
 * driver implemented a complete and correct DDC/CI slave so that brightness
 * tools could drive the panel, and Windows never called it once: monitor
 * control goes from the system straight to the graphics adapter's hardware
 * I2C master, which an indirect display has no place in. Microsoft's own
 * documentation says the OS does not use those callbacks. Brightness is
 * applied during colour conversion instead, from a registry value, which is
 * both simpler and actually works.
 */

#include <windows.h>

#include <wdf.h>

#include <iddcx.h>

#include "device.h"
#include "log.h"

using namespace usbhdmi;

namespace {

IndirectDevice* DeviceFrom(WDFDEVICE wdf_device) {
  auto* wrapper = GetDeviceContext(wdf_device);
  return wrapper ? wrapper->device : nullptr;
}

IndirectDevice* DeviceFrom(IDDCX_ADAPTER adapter) {
  auto* wrapper = GetDeviceContext(adapter);
  return wrapper ? wrapper->device : nullptr;
}

/* Monitors carry a different context type. Asking for the wrong one is not a
 * soft failure: the framework raises a fatal error that kills the host
 * process, which Windows then reports as a hung driver and takes the device
 * offline. */
IndirectDevice* DeviceFrom(IDDCX_MONITOR monitor) {
  auto* wrapper = GetMonitorContext(monitor);
  return wrapper ? wrapper->device : nullptr;
}

EVT_WDF_DEVICE_CONTEXT_CLEANUP EvtDeviceContextCleanup;
EVT_WDF_DEVICE_D0_ENTRY EvtDeviceD0Entry;
EVT_WDF_DEVICE_PREPARE_HARDWARE EvtDevicePrepareHardware;
EVT_WDF_DEVICE_RELEASE_HARDWARE EvtDeviceReleaseHardware;

EVT_IDD_CX_ADAPTER_INIT_FINISHED EvtAdapterInitFinished;
EVT_IDD_CX_ADAPTER_COMMIT_MODES EvtAdapterCommitModes;
EVT_IDD_CX_PARSE_MONITOR_DESCRIPTION EvtParseMonitorDescription;
EVT_IDD_CX_MONITOR_GET_DEFAULT_DESCRIPTION_MODES EvtMonitorGetDefaultModes;
EVT_IDD_CX_MONITOR_QUERY_TARGET_MODES EvtMonitorQueryModes;
EVT_IDD_CX_MONITOR_ASSIGN_SWAPCHAIN EvtMonitorAssignSwapChain;
EVT_IDD_CX_MONITOR_UNASSIGN_SWAPCHAIN EvtMonitorUnassignSwapChain;

void EvtDeviceContextCleanup(WDFOBJECT object) {
  auto* wrapper = GetDeviceContext(object);
  if (wrapper) {
    wrapper->Cleanup();
  }
}

NTSTATUS EvtDevicePrepareHardware(WDFDEVICE wdf_device, WDFCMRESLIST,
                                  WDFCMRESLIST) {
  IndirectDevice* device = DeviceFrom(wdf_device);
  if (!device) {
    return STATUS_DEVICE_CONFIGURATION_ERROR;
  }
  const NTSTATUS status = device->PrepareHardware();
  Log("PrepareHardware -> 0x%08X", status);
  return status;
}

NTSTATUS EvtDeviceReleaseHardware(WDFDEVICE wdf_device, WDFCMRESLIST) {
  IndirectDevice* device = DeviceFrom(wdf_device);
  if (device) {
    device->ReleaseHardware();
  }
  return STATUS_SUCCESS;
}

NTSTATUS EvtDeviceD0Entry(WDFDEVICE wdf_device, WDF_POWER_DEVICE_STATE) {
  IndirectDevice* device = DeviceFrom(wdf_device);
  if (!device) {
    return STATUS_DEVICE_CONFIGURATION_ERROR;
  }

  IDDCX_ADAPTER_CAPS caps = {};
  caps.Size = sizeof(caps);
  caps.MaxMonitorsSupported = 1;

  /* The pixel rate the adapter claims it can drive. Every mode is validated
   * against it, so reporting the real USB bandwidth here disqualifies all of
   * them and the monitor then fails to appear with a status that says only
   * "not ready". Report what the modes need; the real bandwidth is handled
   * by dropping updates, not by lying to the compositor. */
  caps.MaxDisplayPipelineRate =
      static_cast<UINT64>(kMaxFrameWidth) * kMaxFrameHeight * 60;

  caps.EndPointDiagnostics.Size = sizeof(caps.EndPointDiagnostics);
  caps.EndPointDiagnostics.GammaSupport = IDDCX_FEATURE_IMPLEMENTATION_NONE;
  caps.EndPointDiagnostics.TransmissionType =
      IDDCX_TRANSMISSION_TYPE_WIRED_USB;
  caps.EndPointDiagnostics.pEndPointFriendlyName = L"USB HDMI Display";
  caps.EndPointDiagnostics.pEndPointManufacturerName = L"MacroSilicon";
  caps.EndPointDiagnostics.pEndPointModelName = L"MS912x/MS913x";

  /* Not optional: adapter initialisation is rejected outright if either
   * version pointer is null, and the error says only "invalid parameter". */
  IDDCX_ENDPOINT_VERSION hardware_version = {};
  hardware_version.Size = sizeof(hardware_version);
  hardware_version.MajorVer = 1;

  IDDCX_ENDPOINT_VERSION firmware_version = {};
  firmware_version.Size = sizeof(firmware_version);
  firmware_version.MajorVer = 1;

  caps.EndPointDiagnostics.pHardwareVersion = &hardware_version;
  caps.EndPointDiagnostics.pFirmwareVersion = &firmware_version;

  WDF_OBJECT_ATTRIBUTES attributes;
  WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attributes, DeviceContextWrapper);
  attributes.EvtCleanupCallback = EvtDeviceContextCleanup;

  IDARG_IN_ADAPTER_INIT init = {};
  init.WdfDevice = wdf_device;
  init.pCaps = &caps;
  init.ObjectAttributes = &attributes;

  IDARG_OUT_ADAPTER_INIT out = {};
  const NTSTATUS status = IddCxAdapterInitAsync(&init, &out);
  Log("D0Entry: adapter init -> 0x%08X", status);
  if (!NT_SUCCESS(status)) {
    return status;
  }

  /* The adapter object gets its own copy of the pointer, because the
   * callbacks below receive only adapter or monitor handles. */
  auto* wrapper = GetDeviceContext(out.AdapterObject);
  wrapper->device = device;
  return STATUS_SUCCESS;
}

NTSTATUS EvtAdapterInitFinished(IDDCX_ADAPTER adapter,
                                const IDARG_IN_ADAPTER_INIT_FINISHED* args) {
  Log("AdapterInitFinished: 0x%08X", args->AdapterInitStatus);
  if (!NT_SUCCESS(args->AdapterInitStatus)) {
    return STATUS_SUCCESS;
  }
  IndirectDevice* device = DeviceFrom(adapter);
  if (device) {
    device->OnAdapterReady(adapter);
  }
  return STATUS_SUCCESS;
}

NTSTATUS EvtAdapterCommitModes(IDDCX_ADAPTER adapter,
                               const IDARG_IN_COMMITMODES* args) {
  IndirectDevice* device = DeviceFrom(adapter);
  return device ? device->CommitModes(args) : STATUS_DEVICE_NOT_READY;
}

NTSTATUS EvtParseMonitorDescription(
    const IDARG_IN_PARSEMONITORDESCRIPTION* args,
    IDARG_OUT_PARSEMONITORDESCRIPTION* out) {
  /* This callback gets no handle to reach the device, so it answers from the
   * static table. Anything actually unsupported is caught when modes are
   * committed. */
  std::vector<Mode> modes;
  for (size_t i = 0; i < kModeCount; ++i) {
    modes.push_back(kModes[i]);
  }

  out->MonitorModeBufferOutputCount =
      FillMonitorModes(modes, args->MonitorModeBufferInputCount,
                       args->pMonitorModes,
                       IDDCX_MONITOR_MODE_ORIGIN_MONITORDESCRIPTOR);

  if (args->MonitorModeBufferInputCount < modes.size()) {
    return args->MonitorModeBufferInputCount == 0 ? STATUS_SUCCESS
                                                  : STATUS_BUFFER_TOO_SMALL;
  }

  out->PreferredMonitorModeIdx = 0;
  for (size_t i = 0; i < modes.size(); ++i) {
    if (modes[i].width == 1920 && modes[i].height == 1080 &&
        modes[i].hz == 60) {
      out->PreferredMonitorModeIdx = static_cast<UINT>(i);
      break;
    }
  }
  return STATUS_SUCCESS;
}

NTSTATUS EvtMonitorGetDefaultModes(
    IDDCX_MONITOR monitor, const IDARG_IN_GETDEFAULTDESCRIPTIONMODES* args,
    IDARG_OUT_GETDEFAULTDESCRIPTIONMODES* out) {
  IndirectDevice* device = DeviceFrom(monitor);
  if (!device) {
    return STATUS_DEVICE_NOT_READY;
  }
  const std::vector<Mode> modes = device->modes();

  out->DefaultMonitorModeBufferOutputCount = FillMonitorModes(
      modes, args->DefaultMonitorModeBufferInputCount,
      args->pDefaultMonitorModes, IDDCX_MONITOR_MODE_ORIGIN_DRIVER);

  if (args->DefaultMonitorModeBufferInputCount == 0) {
    return STATUS_SUCCESS;
  }
  if (args->DefaultMonitorModeBufferInputCount < modes.size()) {
    return STATUS_BUFFER_TOO_SMALL;
  }
  out->PreferredMonitorModeIdx = 0;
  return STATUS_SUCCESS;
}

NTSTATUS EvtMonitorQueryModes(IDDCX_MONITOR monitor,
                              const IDARG_IN_QUERYTARGETMODES* args,
                              IDARG_OUT_QUERYTARGETMODES* out) {
  IndirectDevice* device = DeviceFrom(monitor);
  if (!device) {
    return STATUS_DEVICE_NOT_READY;
  }
  const std::vector<Mode> modes = device->modes();

  out->TargetModeBufferOutputCount =
      FillTargetModes(modes, args->TargetModeBufferInputCount,
                      args->pTargetModes);

  if (args->TargetModeBufferInputCount == 0) {
    return STATUS_SUCCESS;
  }
  if (args->TargetModeBufferInputCount < modes.size()) {
    return STATUS_BUFFER_TOO_SMALL;
  }
  return STATUS_SUCCESS;
}

NTSTATUS EvtMonitorAssignSwapChain(IDDCX_MONITOR monitor,
                                   const IDARG_IN_SETSWAPCHAIN* args) {
  IndirectDevice* device = DeviceFrom(monitor);
  return device ? device->AssignSwapChain(args) : STATUS_DEVICE_NOT_READY;
}

NTSTATUS EvtMonitorUnassignSwapChain(IDDCX_MONITOR monitor) {
  IndirectDevice* device = DeviceFrom(monitor);
  if (device) {
    device->UnassignSwapChain();
  }
  return STATUS_SUCCESS;
}

EVT_WDF_DRIVER_DEVICE_ADD EvtDriverDeviceAdd;

NTSTATUS EvtDriverDeviceAdd(WDFDRIVER, PWDFDEVICE_INIT device_init) {
  IDD_CX_CLIENT_CONFIG config;
  IDD_CX_CLIENT_CONFIG_INIT(&config);
  config.EvtIddCxAdapterInitFinished = EvtAdapterInitFinished;
  config.EvtIddCxAdapterCommitModes = EvtAdapterCommitModes;
  config.EvtIddCxParseMonitorDescription = EvtParseMonitorDescription;
  config.EvtIddCxMonitorGetDefaultDescriptionModes = EvtMonitorGetDefaultModes;
  config.EvtIddCxMonitorQueryTargetModes = EvtMonitorQueryModes;
  config.EvtIddCxMonitorAssignSwapChain = EvtMonitorAssignSwapChain;
  config.EvtIddCxMonitorUnassignSwapChain = EvtMonitorUnassignSwapChain;

  NTSTATUS status = IddCxDeviceInitConfig(device_init, &config);
  if (!NT_SUCCESS(status)) {
    return status;
  }

  WDF_PNPPOWER_EVENT_CALLBACKS power;
  WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&power);
  power.EvtDevicePrepareHardware = EvtDevicePrepareHardware;
  power.EvtDeviceReleaseHardware = EvtDeviceReleaseHardware;
  power.EvtDeviceD0Entry = EvtDeviceD0Entry;
  WdfDeviceInitSetPnpPowerEventCallbacks(device_init, &power);

  WDF_OBJECT_ATTRIBUTES attributes;
  WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attributes, DeviceContextWrapper);
  attributes.EvtCleanupCallback = EvtDeviceContextCleanup;

  WDFDEVICE wdf_device = nullptr;
  status = WdfDeviceCreate(&device_init, &attributes, &wdf_device);
  if (!NT_SUCCESS(status)) {
    return status;
  }

  status = IddCxDeviceInitialize(wdf_device);
  Log("DeviceAdd: device initialize -> 0x%08X", status);
  if (!NT_SUCCESS(status)) {
    return status;
  }

  auto* wrapper = GetDeviceContext(wdf_device);
  wrapper->device = new IndirectDevice(wdf_device);
  return STATUS_SUCCESS;
}

}  // namespace

extern "C" DRIVER_INITIALIZE DriverEntry;

extern "C" NTSTATUS DriverEntry(PDRIVER_OBJECT driver_object,
                                PUNICODE_STRING registry_path) {
  LogReset();
  Log("DriverEntry");

  WDF_DRIVER_CONFIG config;
  WDF_DRIVER_CONFIG_INIT(&config, EvtDriverDeviceAdd);
  config.DriverPoolTag = 'dhsu';

  WDF_OBJECT_ATTRIBUTES attributes;
  WDF_OBJECT_ATTRIBUTES_INIT(&attributes);

  return WdfDriverCreate(driver_object, registry_path, &attributes, &config,
                         WDF_NO_HANDLE);
}
