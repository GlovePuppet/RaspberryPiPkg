/** @file

    Copyright (c), 2017, Andrey Warkentin <andrey.warkentin@gmail.com>
    Copyright (c) 2015-2016, Linaro Limited. All rights reserved.

    Copyright (C) 2012 Oleksandr Tymoshenko <gonzo@freebsd.org>
    Copyright (C) 2014 Marek Vasut <marex@denx.de>

    SPDX-License-Identifier:     GPL-2.0+

**/

#ifndef _DWUSBHOSTDXE_H_
#define _DWUSBHOSTDXE_H_

#include <Uefi.h>

#include <Protocol/RaspberryPiFirmware.h>
#include <Protocol/Usb2HostController.h>
#include <IndustryStandard/RpiFirmware.h>
#include <IndustryStandard/Bcm2836.h>

#include <Guid/EventGroup.h>

#include <Library/DebugLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/UefiDriverEntryPoint.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Library/BaseLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PcdLib.h>
#include <Library/ReportStatusCodeLib.h>
#include <Library/DevicePathLib.h>
#include <Library/PcdLib.h>
#include <Library/IoLib.h>
#include <Library/TimerLib.h>
#include <Library/DmaLib.h>
#include <Library/ArmLib.h>

#define MAX_DEVICE                      16
#define MAX_ENDPOINT                    16

#define DWUSB_OTGHC_DEV_SIGNATURE       SIGNATURE_32 ('d', 'w', 'h', 'c')
#define DWHC_FROM_THIS(a)               CR(a, DWUSB_OTGHC_DEV, DwUsbOtgHc, DWUSB_OTGHC_DEV_SIGNATURE)

//
// Iterate through the double linked list. NOT delete safe
//
#define EFI_LIST_FOR_EACH(Entry, ListHead)    \
  for(Entry = (ListHead)->ForwardLink; Entry != (ListHead); Entry = Entry->ForwardLink)

//
// Iterate through the double linked list. This is delete-safe.
// Do not touch NextEntry
//
#define EFI_LIST_FOR_EACH_SAFE(Entry, NextEntry, ListHead)            \
  for(Entry = (ListHead)->ForwardLink, NextEntry = Entry->ForwardLink;\
      Entry != (ListHead); Entry = NextEntry, NextEntry = Entry->ForwardLink)

#define EFI_LIST_CONTAINER(Entry, Type, Field) BASE_CR(Entry, Type, Field)

//
// The RequestType in EFI_USB_DEVICE_REQUEST is composed of
// three fields: One bit direction, 2 bit type, and 5 bit
// target.
//
#define USB_REQUEST_TYPE(Dir, Type, Target)                             \
  ((UINT8)((((Dir) == EfiUsbDataIn ? 0x01 : 0) << 7) | (Type) | (Target)))

typedef struct {
  ACPI_HID_DEVICE_PATH          AcpiDevicePath;
  PCI_DEVICE_PATH               PciDevicePath;
  EFI_DEVICE_PATH_PROTOCOL      EndDevicePath;
} EFI_USB_PCIIO_DEVICE_PATH;

typedef struct _DWUSB_DEFERRED_REQ {
  IN OUT LIST_ENTRY                         List;
  IN OUT EFI_EVENT                          Event;
  IN     struct _DWUSB_OTGHC_DEV            *DwHc;
  IN     UINT32                             Channel;
  IN     EFI_USB2_HC_TRANSACTION_TRANSLATOR *Translator;
  IN     UINT8                              DeviceSpeed;
  IN     UINT8                              DeviceAddress;
  IN     UINTN                              MaximumPacketLength;
  IN     UINT32                             TransferDirection;
  IN OUT VOID                               *Data;
  IN OUT UINTN                              DataLength;
  IN OUT UINT32                             Pid;
  IN     UINT32                             EpAddress;
  IN     UINT32                             EpType;
  OUT    UINT32                             TransferResult;
  IN     BOOLEAN                            IgnoreAck;
  IN     EFI_ASYNC_USB_TRANSFER_CALLBACK    CallbackFunction;
  IN     VOID                               *CallbackContext;
} DWUSB_DEFERRED_REQ;

typedef struct _DWUSB_OTGHC_DEV {
  UINTN                           Signature;
  EFI_HANDLE                      DeviceHandle;

  EFI_USB2_HC_PROTOCOL            DwUsbOtgHc;

  EFI_USB_HC_STATE                DwHcState;

  EFI_USB_PCIIO_DEVICE_PATH       DevicePath;

  EFI_EVENT                       ExitBootServiceEvent;

  EFI_PHYSICAL_ADDRESS            DwUsbBase;
  UINT8                           *StatusBuffer;

  UINT8                           *AlignedBuffer;
  VOID *                          AlignedBufferMapping;
  UINTN                           AlignedBufferBusAddress;

  UINT16                          PortStatus;
  UINT16                          PortChangeStatus;

  LIST_ENTRY                      DeferredList;
} DWUSB_OTGHC_DEV;

#endif //_DWUSBHOSTDXE_H_
