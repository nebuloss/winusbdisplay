/* SPDX-License-Identifier: GPL-2.0-only
 *
 * UMDF2 entry points and IddCx client callbacks.
 */

#include <windows.h>

#include <wdf.h>

#include <iddcx.h>

#include "device.h"
#include "log.h"

using namespace ms912x;

namespace {

IndirectDevice* DeviceFrom(WDFDEVICE wdf_device) {
  auto* wrapper = GetIndirectDeviceContext(wdf_device);
  return wrapper ? wrapper->device : nullptr;
}

IndirectDevice* DeviceFrom(IDDCX_ADAPTER adapter) {
  auto* wrapper = GetIndirectDeviceContext(adapter);
  return wrapper ? wrapper->device : nullptr;
}

/* Monitor objects are created with MonitorContextWrapper. Asking for the
 * wrong context type is not a soft failure in WDF: it raises a fatal error
 * that kills the UMDF host, which Windows then reports as a user-mode driver
 * hang and takes the device offline. */
IndirectDevice* DeviceFrom(IDDCX_MONITOR monitor) {
  auto* wrapper = GetMonitorContext(monitor);
  return wrapper ? wrapper->device : nullptr;
}

/* ------------------------------------------------------------------------ */

EVT_WDF_DEVICE_CONTEXT_CLEANUP EvtDeviceContextCleanup;
EVT_WDF_DEVICE_D0_ENTRY EvtDeviceD0Entry;
EVT_WDF_DEVICE_PREPARE_HARDWARE EvtDevicePrepareHardware;
EVT_WDF_DEVICE_RELEASE_HARDWARE EvtDeviceReleaseHardware;

EVT_IDD_CX_ADAPTER_INIT_FINISHED EvtIddCxAdapterInitFinished;
EVT_IDD_CX_ADAPTER_COMMIT_MODES EvtIddCxAdapterCommitModes;
EVT_IDD_CX_PARSE_MONITOR_DESCRIPTION EvtIddCxParseMonitorDescription;
EVT_IDD_CX_MONITOR_GET_DEFAULT_DESCRIPTION_MODES
    EvtIddCxMonitorGetDefaultModes;
EVT_IDD_CX_MONITOR_QUERY_TARGET_MODES EvtIddCxMonitorQueryModes;
EVT_IDD_CX_MONITOR_ASSIGN_SWAPCHAIN EvtIddCxMonitorAssignSwapChain;
EVT_IDD_CX_MONITOR_I2C_TRANSMIT EvtIddCxMonitorI2CTransmit;
EVT_IDD_CX_MONITOR_I2C_RECEIVE EvtIddCxMonitorI2CReceive;
EVT_IDD_CX_MONITOR_UNASSIGN_SWAPCHAIN EvtIddCxMonitorUnassignSwapChain;

void EvtDeviceContextCleanup(WDFOBJECT object) {
  auto* wrapper = GetIndirectDeviceContext(object);
  if (wrapper) {
    wrapper->Cleanup();
  }
}

NTSTATUS EvtDevicePrepareHardware(WDFDEVICE wdf_device, WDFCMRESLIST,
                                  WDFCMRESLIST) {
  Log("PrepareHardware: enter");
  IndirectDevice* device = DeviceFrom(wdf_device);
  if (!device) {
    Log("PrepareHardware: no device context");
    return STATUS_DEVICE_CONFIGURATION_ERROR;
  }
  NTSTATUS status = device->PrepareHardware();
  Log("PrepareHardware: -> 0x%08X", status);
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
  Log("D0Entry: enter");
  IndirectDevice* device = DeviceFrom(wdf_device);
  if (!device) {
    Log("D0Entry: no device context");
    return STATUS_DEVICE_CONFIGURATION_ERROR;
  }

  /* Tell IddCx the adapter is ready. Everything the adapter needs was already
   * read in PrepareHardware. */
  IDDCX_ADAPTER_CAPS caps = {};
  caps.Size = sizeof(caps);
  caps.MaxMonitorsSupported = 1;
  /* This is the pixel rate the adapter claims it can drive, and the OS
   * validates every mode against it. Advertising the real USB 2 bandwidth
   * here disqualifies all of them and monitor arrival then fails with
   * STATUS_DEVICE_NOT_READY. Report what the modes need; actual bandwidth is
   * handled by dropping frames in FrameSender, not by lying to the OS. */
  caps.MaxDisplayPipelineRate =
      static_cast<UINT64>(kMaxWidth) * kMaxHeight * 60;
  caps.EndPointDiagnostics.Size = sizeof(caps.EndPointDiagnostics);
  caps.EndPointDiagnostics.GammaSupport = IDDCX_FEATURE_IMPLEMENTATION_NONE;
  caps.EndPointDiagnostics.TransmissionType =
      IDDCX_TRANSMISSION_TYPE_WIRED_USB;
  caps.EndPointDiagnostics.pEndPointFriendlyName = L"MacroSilicon USB Display";
  caps.EndPointDiagnostics.pEndPointManufacturerName = L"MacroSilicon";
  caps.EndPointDiagnostics.pEndPointModelName = L"MS912x/MS913x";
  /* These two are not optional: IddCxAdapterInitAsync rejects the caps with
   * STATUS_INVALID_PARAMETER if either version pointer is null. */
  IDDCX_ENDPOINT_VERSION hardware_version = {};
  hardware_version.Size = sizeof(hardware_version);
  hardware_version.MajorVer = 1;

  IDDCX_ENDPOINT_VERSION firmware_version = {};
  firmware_version.Size = sizeof(firmware_version);
  firmware_version.MajorVer = 1;

  caps.EndPointDiagnostics.pHardwareVersion = &hardware_version;
  caps.EndPointDiagnostics.pFirmwareVersion = &firmware_version;

  WDF_OBJECT_ATTRIBUTES attributes;
  WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attributes,
                                          IndirectDeviceContextWrapper);
  attributes.EvtCleanupCallback = EvtDeviceContextCleanup;

  IDARG_IN_ADAPTER_INIT init = {};
  init.WdfDevice = wdf_device;
  init.pCaps = &caps;
  init.ObjectAttributes = &attributes;

  IDARG_OUT_ADAPTER_INIT out = {};
  NTSTATUS status = IddCxAdapterInitAsync(&init, &out);
  Log("D0Entry: IddCxAdapterInitAsync -> 0x%08X", status);
  if (!NT_SUCCESS(status)) {
    return status;
  }

  /* The adapter object gets its own copy of the context pointer so the
   * IddCx callbacks, which only receive adapter or monitor handles, can find
   * their way back. */
  auto* wrapper = GetIndirectDeviceContext(out.AdapterObject);
  wrapper->device = device;
  Log("D0Entry: success");
  return STATUS_SUCCESS;
}

NTSTATUS EvtIddCxAdapterInitFinished(
    IDDCX_ADAPTER adapter, const IDARG_IN_ADAPTER_INIT_FINISHED* args) {
  Log("AdapterInitFinished: status 0x%08X", args->AdapterInitStatus);
  if (!NT_SUCCESS(args->AdapterInitStatus)) {
    return STATUS_SUCCESS;
  }
  IndirectDevice* device = DeviceFrom(adapter);
  if (device) {
    /* CreateMonitor sets the monitor's own context; do not reach for it with
     * the device context type here. */
    device->OnAdapterInitFinished(adapter);
  }
  return STATUS_SUCCESS;
}

NTSTATUS EvtIddCxAdapterCommitModes(IDDCX_ADAPTER adapter,
                                    const IDARG_IN_COMMITMODES* args) {
  IndirectDevice* device = DeviceFrom(adapter);
  if (!device) {
    return STATUS_DEVICE_NOT_READY;
  }
  return device->CommitModes(args);
}

NTSTATUS EvtIddCxParseMonitorDescription(
    const IDARG_IN_PARSEMONITORDESCRIPTION* args,
    IDARG_OUT_PARSEMONITORDESCRIPTION* out) {
  /* This callback has no handle to get back to the device, so answer from the
   * static mode table. The commit path validates against the device anyway. */
  std::vector<Mode> modes;
  for (size_t i = 0; i < kModeListLen; ++i) {
    modes.push_back(kModeList[i]);
  }

  Log("ParseMonitorDescription: in=%u buf=%p descType=%u dataSize=%u",
      args->MonitorModeBufferInputCount, (void*)args->pMonitorModes,
      (unsigned)args->MonitorDescription.Type,
      args->MonitorDescription.DataSize);

  out->MonitorModeBufferOutputCount = FillMonitorModes(
      modes, args->MonitorModeBufferInputCount, args->pMonitorModes,
      IDDCX_MONITOR_MODE_ORIGIN_MONITORDESCRIPTOR);

  if (args->MonitorModeBufferInputCount < modes.size()) {
    return args->MonitorModeBufferInputCount == 0 ? STATUS_SUCCESS
                                                  : STATUS_BUFFER_TOO_SMALL;
  }

  /* Prefer 1080p60 when it is in the list. */
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

NTSTATUS EvtIddCxMonitorGetDefaultModes(
    IDDCX_MONITOR monitor, const IDARG_IN_GETDEFAULTDESCRIPTIONMODES* args,
    IDARG_OUT_GETDEFAULTDESCRIPTIONMODES* out) {
  IndirectDevice* device = DeviceFrom(monitor);
  if (!device) {
    return STATUS_DEVICE_NOT_READY;
  }
  const std::vector<Mode>& modes = device->modes();

  Log("GetDefaultModes: in=%u modes=%u",
      args->DefaultMonitorModeBufferInputCount, (unsigned)modes.size());
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

NTSTATUS EvtIddCxMonitorQueryModes(IDDCX_MONITOR monitor,
                                   const IDARG_IN_QUERYTARGETMODES* args,
                                   IDARG_OUT_QUERYTARGETMODES* out) {
  IndirectDevice* device = DeviceFrom(monitor);
  if (!device) {
    return STATUS_DEVICE_NOT_READY;
  }
  const std::vector<Mode>& modes = device->modes();

  Log("QueryTargetModes: in=%u modes=%u",
      args->TargetModeBufferInputCount, (unsigned)modes.size());
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

NTSTATUS EvtIddCxMonitorAssignSwapChain(IDDCX_MONITOR monitor,
                                        const IDARG_IN_SETSWAPCHAIN* args) {
  IndirectDevice* device = DeviceFrom(monitor);
  if (!device) {
    return STATUS_DEVICE_NOT_READY;
  }
  return device->AssignSwapChain(args);
}

/* DDC/CI lives on I2C address 0x37. Everything else is refused so that a
 * probe cannot mistake us for a device we are not. */
NTSTATUS EvtIddCxMonitorI2CTransmit(IDDCX_MONITOR monitor,
                                    const IDARG_IN_I2C_TRANSMIT* args) {
  IndirectDevice* device = DeviceFrom(monitor);
  if (!device) {
    return STATUS_DEVICE_NOT_READY;
  }
  if (!device->ddc()->Transmit(args->SevenBitI2CAddress,
                               static_cast<const uint8_t*>(args->pData),
                               args->DataSizeInBytes)) {
    return STATUS_NOT_SUPPORTED;
  }
  return STATUS_SUCCESS;
}

NTSTATUS EvtIddCxMonitorI2CReceive(IDDCX_MONITOR monitor,
                                   const IDARG_IN_I2C_RECEIVE* args) {
  IndirectDevice* device = DeviceFrom(monitor);
  if (!device) {
    return STATUS_DEVICE_NOT_READY;
  }
  if (!device->ddc()->Receive(args->SevenBitI2CAddress,
                              static_cast<uint8_t*>(args->pData),
                              args->DataSizeInBytes)) {
    return STATUS_NOT_SUPPORTED;
  }
  return STATUS_SUCCESS;
}

NTSTATUS EvtIddCxMonitorUnassignSwapChain(IDDCX_MONITOR monitor) {
  IndirectDevice* device = DeviceFrom(monitor);
  if (device) {
    device->UnassignSwapChain();
  }
  return STATUS_SUCCESS;
}

/* ------------------------------------------------------------------------ */

EVT_WDF_DRIVER_DEVICE_ADD EvtDriverDeviceAdd;

NTSTATUS EvtDriverDeviceAdd(WDFDRIVER, PWDFDEVICE_INIT device_init) {
  IDD_CX_CLIENT_CONFIG config;
  IDD_CX_CLIENT_CONFIG_INIT(&config);
  config.EvtIddCxAdapterInitFinished = EvtIddCxAdapterInitFinished;
  config.EvtIddCxAdapterCommitModes = EvtIddCxAdapterCommitModes;
  config.EvtIddCxParseMonitorDescription = EvtIddCxParseMonitorDescription;
  config.EvtIddCxMonitorGetDefaultDescriptionModes =
      EvtIddCxMonitorGetDefaultModes;
  config.EvtIddCxMonitorQueryTargetModes = EvtIddCxMonitorQueryModes;
  config.EvtIddCxMonitorAssignSwapChain = EvtIddCxMonitorAssignSwapChain;
  config.EvtIddCxMonitorUnassignSwapChain = EvtIddCxMonitorUnassignSwapChain;
  config.EvtIddCxMonitorI2CTransmit = EvtIddCxMonitorI2CTransmit;
  config.EvtIddCxMonitorI2CReceive = EvtIddCxMonitorI2CReceive;

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
  WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attributes,
                                          IndirectDeviceContextWrapper);
  attributes.EvtCleanupCallback = EvtDeviceContextCleanup;

  WDFDEVICE wdf_device = nullptr;
  status = WdfDeviceCreate(&device_init, &attributes, &wdf_device);
  if (!NT_SUCCESS(status)) {
    return status;
  }

  status = IddCxDeviceInitialize(wdf_device);
  Log("DeviceAdd: IddCxDeviceInitialize -> 0x%08X", status);
  if (!NT_SUCCESS(status)) {
    return status;
  }

  auto* wrapper = GetIndirectDeviceContext(wdf_device);
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
  config.DriverPoolTag = 'x219';

  WDF_OBJECT_ATTRIBUTES attributes;
  WDF_OBJECT_ATTRIBUTES_INIT(&attributes);

  return WdfDriverCreate(driver_object, registry_path, &attributes, &config,
                         WDF_NO_HANDLE);
}
