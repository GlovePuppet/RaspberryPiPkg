/** @file

    Copyright (c), 2017, Andrey Warkentin <andrey.warkentin@gmail.com>
    Copyright (c) 2015-2016, Linaro Limited. All rights reserved.
    Copyright (C) 2012 Oleksandr Tymoshenko <gonzo@freebsd.org>
    Copyright (C) 2014 Marek Vasut <marex@denx.de>

    SPDX-License-Identifier:     GPL-2.0+

**/

#include "DwUsbHostDxe.h"
#include "DwcHw.h"

/*
 * Look, ma, some documentation:
 *
 * https://www.quicklogic.com/assets/pdf/data-sheets/QL-Hi-Speed-USB-2.0-OTG-Controller-Data-Sheet.pdf
 */

EFI_USB_PCIIO_DEVICE_PATH DwHcDevicePath =
  {
    {
      { ACPI_DEVICE_PATH, ACPI_DP, { sizeof (ACPI_HID_DEVICE_PATH), 0 } },
      EISA_PNP_ID(0x0A03),  // HID
      0                     // UID
    },
    {
      { HARDWARE_DEVICE_PATH, HW_PCI_DP, { sizeof (PCI_DEVICE_PATH), 0 } },
      0,
      0
    },
    { END_DEVICE_PATH_TYPE, END_ENTIRE_DEVICE_PATH_SUBTYPE, { sizeof (EFI_DEVICE_PATH_PROTOCOL), 0} }
  };

typedef enum {
  XFER_DONE,
  XFER_ERROR,
  XFER_NAK,
  XFER_STALL,
  XFER_RESTART
} CHANNEL_HALT_REASON;

typedef struct {
  BOOLEAN Splitting;
  BOOLEAN SplitStart;
} SPLIT_CONTROL;

VOID DwHcInit (IN DWUSB_OTGHC_DEV *DwHc);
VOID DwCoreInit (IN DWUSB_OTGHC_DEV *DwHc);

UINT32
Wait4Bit (
          IN UINT32     Reg,
          IN UINT32     Mask,
          IN BOOLEAN    Set
          )
{
  UINT32  Timeout = 1000000;
  UINT32  Value;

  while (--Timeout) {
    Value = MmioRead32 (Reg);
    if (!Set)
      Value = ~Value;

    if ((Value & Mask) == Mask)
      return 0;

    MicroSecondDelay (1);
  }

  DEBUG ((EFI_D_ERROR, "Wait4Bit: Timeout (Reg:0x%x, mask:0x%x, wait_set:%d)\n", Reg, Mask, Set));

  return 1;
}

CHANNEL_HALT_REASON
Wait4Chhltd (
             IN DWUSB_OTGHC_DEV    *DwHc,
             IN UINT32             Channel,
             IN UINT32             *Sub,
             IN UINT32             *Toggle,
             IN BOOLEAN            IgnoreAck,
             IN SPLIT_CONTROL     *Split
             )
{
  UINT32  HcintCompHltAck = DWC2_HCINT_XFERCOMP | DWC2_HCINT_CHHLTD;
  UINT32  HcintSplitStartAck = DWC2_HCINT_CHHLTD;
  UINT32  HcintSplitNyet = DWC2_HCINT_CHHLTD | DWC2_HCINT_NYET;
  INT32   Ret;
  UINT32  Hcint, Hctsiz;

  MicroSecondDelay (100);
  Ret = Wait4Bit (DwHc->DwUsbBase + HCINT(Channel), DWC2_HCINT_CHHLTD, 1);
  if (Ret) {
    return XFER_ERROR;
  }

  MicroSecondDelay (100);
  Hcint = MmioRead32 (DwHc->DwUsbBase + HCINT(Channel));

  if ((Hcint & DWC2_HCINT_NAK) != 0) {
    return XFER_NAK;
  }

  if ((Hcint & DWC2_HCINT_STALL) != 0) {
    return XFER_STALL;
  }

  if ((Hcint & DWC2_HCINT_FRMOVRUN) != 0) {
    /*
     * Not much else to do, but try again!
     */
    return XFER_RESTART;
  }

  if (IgnoreAck) {
    Hcint &= ~DWC2_HCINT_ACK;
  } else {
    HcintCompHltAck |= DWC2_HCINT_ACK;
    HcintSplitStartAck |= DWC2_HCINT_ACK;
  }

  if (Split->Splitting) {
    if (Split->SplitStart &&
        Hcint == HcintSplitStartAck) {
      Split->SplitStart = FALSE;
      return XFER_RESTART;
    } else if (!Split->SplitStart &&
               Hcint == HcintSplitNyet) {
      return XFER_RESTART;
    }
  }

  if (Hcint != HcintCompHltAck) {
    DEBUG ((EFI_D_ERROR, "Wait4Chhltd: HCINT Error 0x%x %a\n", Hcint,
            Split->SplitStart ? "split start" :
            (Split->Splitting ? "split complete" : "")));
    return XFER_ERROR;
  }

  Hctsiz = MmioRead32 (DwHc->DwUsbBase + HCTSIZ(Channel));
  *Sub = (Hctsiz & DWC2_HCTSIZ_XFERSIZE_MASK) >> DWC2_HCTSIZ_XFERSIZE_OFFSET;
  *Toggle = (Hctsiz & DWC2_HCTSIZ_PID_MASK) >> DWC2_HCTSIZ_PID_OFFSET;

  return XFER_DONE;
}

VOID
DwOtgHcInit (
             IN DWUSB_OTGHC_DEV    *DwHc,
             IN UINT8              HcNum,
             IN EFI_USB2_HC_TRANSACTION_TRANSLATOR  *Translator,
             IN UINT8              DeviceSpeed,
             IN UINT8              DevAddr,
             IN UINT8              Endpoint,
             IN UINT8              EpDir,
             IN UINT8              EpType,
             IN UINT16             MaxPacket,
             IN SPLIT_CONTROL *SplitControl
             )
{
  UINT32 Split = 0;
  UINT32 Hcchar = (DevAddr << DWC2_HCCHAR_DEVADDR_OFFSET) |
    (Endpoint << DWC2_HCCHAR_EPNUM_OFFSET) |
    (EpDir << DWC2_HCCHAR_EPDIR_OFFSET) |
    (EpType << DWC2_HCCHAR_EPTYPE_OFFSET) |
    (MaxPacket << DWC2_HCCHAR_MPS_OFFSET) |
    ((DeviceSpeed == EFI_USB_SPEED_LOW) ? DWC2_HCCHAR_LSPDDEV : 0);

  MmioWrite32 (DwHc->DwUsbBase + HCINT(HcNum), 0x3FFF);

  MmioWrite32 (DwHc->DwUsbBase + HCCHAR(HcNum), Hcchar);

  if (SplitControl->Splitting) {
    Split = DWC2_HCSPLT_SPLTENA |
      ((Translator->TranslatorPortNumber) << DWC2_HCSPLT_PRTADDR_OFFSET) |
      ((Translator->TranslatorHubAddress) << DWC2_HCSPLT_HUBADDR_OFFSET);

    if (!SplitControl->SplitStart) {
      Split |= DWC2_HCSPLT_COMPSPLT;
    }
  }

  MmioWrite32 (DwHc->DwUsbBase + HCSPLT(HcNum), Split);
}

VOID
DwCoreReset (
             IN DWUSB_OTGHC_DEV *DwHc
             )
{
  UINT32  Status;

  Status = Wait4Bit (DwHc->DwUsbBase + GRSTCTL, DWC2_GRSTCTL_AHBIDLE, 1);
  if (Status)
    DEBUG ((EFI_D_ERROR, "DwCoreReset: Timeout!\n"));

  MmioWrite32 (DwHc->DwUsbBase + GRSTCTL, DWC2_GRSTCTL_CSFTRST);

  Status = Wait4Bit (DwHc->DwUsbBase + GRSTCTL, DWC2_GRSTCTL_CSFTRST, 0);
  if (Status)
    DEBUG ((EFI_D_ERROR, "DwCoreReset: Timeout!\n"));

  MicroSecondDelay (100000);
}

STATIC
EFI_STATUS
DwHcTransfer (
              IN     DWUSB_OTGHC_DEV        *DwHc,
              IN     UINT32                 Channel,
              IN     EFI_USB2_HC_TRANSACTION_TRANSLATOR  *Translator,
              IN     UINT8                  DeviceSpeed,
              IN     UINT8                  DeviceAddress,
              IN     UINTN                  MaximumPacketLength,
              IN OUT UINT32                 *Pid,
              IN     UINT32                 TransferDirection,
              IN OUT VOID                   *Data,
              IN OUT UINTN                  *DataLength,
              IN     UINT32                 EpAddress,
              IN     UINT32                 EpType,
              OUT    UINT32                 *TransferResult,
              IN     BOOLEAN                IgnoreAck
              )
{
  UINT32                          TxferLen;
  UINT32                          Done = 0;
  UINT32                          NumPackets;
  UINT32                          Sub;
  UINT32                          Ret = 0;
  UINT32                          StopTransfer = 0;
  EFI_STATUS                      Status = EFI_SUCCESS;
  SPLIT_CONTROL                   Split = { 0 };

  /* DEBUG((DEBUG_ERROR, "%u:%u> Transfer of size %u, MPL = %u\n", */
  /*        DeviceAddress, */
  /*        EpAddress, */
  /*        *DataLength, */
  /*        MaximumPacketLength)); */

 do {
    if (DeviceSpeed == EFI_USB_SPEED_LOW ||
        DeviceSpeed == EFI_USB_SPEED_FULL) {
      Split.Splitting = TRUE;
      Split.SplitStart = TRUE;
    }

    TxferLen = *DataLength - Done;

    if (TxferLen > DWC2_MAX_TRANSFER_SIZE) {
      TxferLen = DWC2_MAX_TRANSFER_SIZE - MaximumPacketLength + 1;
    }

    if (TxferLen > DWC2_DATA_BUF_SIZE) {
      TxferLen = DWC2_DATA_BUF_SIZE - MaximumPacketLength + 1;
    }

    if (Split.Splitting ||
        TxferLen == 0) {
      NumPackets = 1;
    } else {
      NumPackets = (TxferLen + MaximumPacketLength - 1) / MaximumPacketLength;
      if (NumPackets > DWC2_MAX_PACKET_COUNT) {
        NumPackets = DWC2_MAX_PACKET_COUNT;
        TxferLen = NumPackets * MaximumPacketLength;
      }
    }

    if (TransferDirection) {
      TxferLen = NumPackets * MaximumPacketLength;
    }

    MmioWrite32 (DwHc->DwUsbBase + HCTSIZ(Channel),
                 (TxferLen << DWC2_HCTSIZ_XFERSIZE_OFFSET) |
                 (NumPackets << DWC2_HCTSIZ_PKTCNT_OFFSET) |
                 (*Pid << DWC2_HCTSIZ_PID_OFFSET));

    if (!TransferDirection) {
      CopyMem (DwHc->AlignedBuffer, Data+Done, TxferLen);
      ArmDataSynchronizationBarrier();
    }

    MmioWrite32 (DwHc->DwUsbBase + HCDMA(Channel),
                 (UINTN)DwHc->AlignedBufferBusAddress);

restart_channel:
    DwOtgHcInit (DwHc, Channel, Translator, DeviceSpeed,
                 DeviceAddress, EpAddress,
                 TransferDirection, EpType,
                 MaximumPacketLength, &Split);

    MmioAndThenOr32 (DwHc->DwUsbBase + HCCHAR(Channel),
                     ~(DWC2_HCCHAR_MULTICNT_MASK |
                       DWC2_HCCHAR_CHEN |
                       DWC2_HCCHAR_CHDIS),
                     ((1 << DWC2_HCCHAR_MULTICNT_OFFSET) |
                      DWC2_HCCHAR_CHEN));

    Ret = Wait4Chhltd (DwHc, Channel, &Sub, Pid, IgnoreAck, &Split);
    if (Ret == XFER_RESTART) {
      goto restart_channel;
    } else if (Ret == XFER_STALL) {
      *TransferResult = EFI_USB_ERR_STALL;
      Status = EFI_DEVICE_ERROR;
      break;
    } else if (Ret == XFER_ERROR) {
      *TransferResult = EFI_USB_ERR_SYSTEM;
      Status = EFI_DEVICE_ERROR;
      break;
    } else if (Ret == XFER_NAK) {
      *TransferResult = EFI_USB_ERR_NAK;
      Status = EFI_DEVICE_ERROR;
      break;
    }

    if (TransferDirection) { // out or none
      ArmDataSynchronizationBarrier();
      TxferLen -= Sub;
      CopyMem (Data+Done, DwHc->AlignedBuffer, TxferLen);
      if (Sub) {
        StopTransfer = 1;
      }
    }

    Done += TxferLen;
  } while (Done < *DataLength && !StopTransfer);

  MmioWrite32 (DwHc->DwUsbBase + HCINTMSK(Channel), 0);
  MmioWrite32 (DwHc->DwUsbBase + HCINT(Channel), 0xFFFFFFFF);

  *DataLength = Done;

  return Status;
}

STATIC
DWUSB_DEFERRED_REQ *
DwHcFindDeferredTransfer (
                          IN DWUSB_OTGHC_DEV *DwHc,
                          IN UINT8 DeviceAddress,
                          IN UINT8 EndPointAddress
                          )
{
  LIST_ENTRY *Entry;

  EFI_LIST_FOR_EACH (Entry, &DwHc->DeferredList) {
    DWUSB_DEFERRED_REQ *Req = EFI_LIST_CONTAINER (Entry, DWUSB_DEFERRED_REQ,
                                                  List);

    if (Req->DeviceAddress == DeviceAddress &&
        Req->EpAddress == (EndPointAddress & 0xF) &&
        Req->TransferDirection == ((EndPointAddress >> 7) & 0x01)) {
      return Req;
    }
  }

  return NULL;
}

STATIC
VOID
DwHcCancelDeferredTransfers (
                             DWUSB_OTGHC_DEV *DwHc
                             )
{
  LIST_ENTRY *Entry;
  EFI_TPL PreviousTpl;

  PreviousTpl = gBS->RaiseTPL(TPL_NOTIFY);
  EFI_LIST_FOR_EACH (Entry, &DwHc->DeferredList) {
    DWUSB_DEFERRED_REQ *Req = EFI_LIST_CONTAINER (Entry, DWUSB_DEFERRED_REQ,
                                                  List);
    /*
     * Stay on the safe side. Maybe other drivers will quiesce their
     * periodic requests by now as part of exiting the boot
     * services, but maybe not.
     */
    DEBUG((EFI_D_ERROR, "Cancelling periodic access to 0x%x:0x%x\n",
           Req->DeviceAddress, Req->EpAddress));
    gBS->CloseEvent (Req->Event);
  }
  gBS->RestoreTPL(PreviousTpl);
}

STATIC
VOID
DwHcDeferredTransfer (
                      IN EFI_EVENT Event,
                      IN VOID      *Context
                      )
{
  EFI_STATUS Status;
  DWUSB_DEFERRED_REQ *Req = Context;

  Req->TransferResult = EFI_USB_NOERROR;
  Status = DwHcTransfer (Req->DwHc, Req->Channel, Req->Translator,
                         Req->DeviceSpeed, Req->DeviceAddress,
                         Req->MaximumPacketLength, &Req->Pid,
                         Req->TransferDirection, Req->Data, &Req->DataLength,
                         Req->EpAddress, Req->EpType, &Req->TransferResult,
                         Req->IgnoreAck);

  if (Req->EpType == DWC2_HCCHAR_EPTYPE_INTR &&
      Status == EFI_DEVICE_ERROR &&
      Req->TransferResult == EFI_USB_ERR_NAK) {
    /*
     * Swallow the NAK, the upper layer expects us to resubmit automatically.
     */
    return;
  }

  Req->CallbackFunction (Req->Data, Req->DataLength, Req->CallbackContext,
                         Req->TransferResult);
}

/**
   EFI_USB2_HC_PROTOCOL APIs
**/

EFI_STATUS
EFIAPI
DwHcGetCapability (
                   IN  EFI_USB2_HC_PROTOCOL  *This,
                   OUT UINT8                 *MaxSpeed,
                   OUT UINT8                 *PortNumber,
                   OUT UINT8                 *Is64BitCapable
                   )
{
  if ((MaxSpeed == NULL) || (PortNumber == NULL) || (Is64BitCapable == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  *MaxSpeed = EFI_USB_SPEED_HIGH;
  *PortNumber = 1;
  *Is64BitCapable = 1;

  return EFI_SUCCESS;
}

EFI_STATUS
EFIAPI
DwHcReset (
           IN EFI_USB2_HC_PROTOCOL *This,
           IN UINT16               Attributes
           )
{
  DWUSB_OTGHC_DEV *DwHc;
  DwHc = DWHC_FROM_THIS (This);

  DwCoreInit(DwHc);
  DwHcInit(DwHc);

  MmioAndThenOr32 (DwHc->DwUsbBase + HPRT0,
                   ~(DWC2_HPRT0_PRTENA | DWC2_HPRT0_PRTCONNDET |
                     DWC2_HPRT0_PRTENCHNG | DWC2_HPRT0_PRTOVRCURRCHNG),
                   DWC2_HPRT0_PRTRST);

  MicroSecondDelay (50000);

  MmioAnd32 (DwHc->DwUsbBase + HPRT0, ~(DWC2_HPRT0_PRTENA | DWC2_HPRT0_PRTCONNDET |
                                        DWC2_HPRT0_PRTENCHNG | DWC2_HPRT0_PRTOVRCURRCHNG |
                                        DWC2_HPRT0_PRTRST));

  return EFI_SUCCESS;
}

EFI_STATUS
EFIAPI
DwHcGetState (
              IN   EFI_USB2_HC_PROTOCOL  *This,
              OUT  EFI_USB_HC_STATE      *State
              )
{
  DWUSB_OTGHC_DEV *DwHc;

  DwHc = DWHC_FROM_THIS (This);

  *State = DwHc->DwHcState;

  return EFI_SUCCESS;
}

EFI_STATUS
EFIAPI
DwHcSetState (
              IN EFI_USB2_HC_PROTOCOL *This,
              IN EFI_USB_HC_STATE     State
              )
{
  DWUSB_OTGHC_DEV *DwHc;

  DwHc = DWHC_FROM_THIS (This);

  DwHc->DwHcState = State;

  return EFI_SUCCESS;
}

EFI_STATUS
EFIAPI
DwHcGetRootHubPortStatus (
                          IN   EFI_USB2_HC_PROTOCOL  *This,
                          IN   UINT8                 PortNumber,
                          OUT  EFI_USB_PORT_STATUS   *PortStatus
                          )
{
  DWUSB_OTGHC_DEV *DwHc;
  UINT32          Hprt0;

  if (PortNumber > DWC2_HC_PORT) {
    return EFI_INVALID_PARAMETER;
  }

  if (PortStatus == NULL) {
    return EFI_INVALID_PARAMETER;
  }

  DwHc = DWHC_FROM_THIS (This);

  PortStatus->PortStatus = 0;
  PortStatus->PortChangeStatus = 0;
  Hprt0 = MmioRead32 (DwHc->DwUsbBase + HPRT0);

  if (Hprt0 & DWC2_HPRT0_PRTCONNSTS) {
    PortStatus->PortStatus |= USB_PORT_STAT_CONNECTION;
  }

  if (Hprt0 & DWC2_HPRT0_PRTENA) {
    PortStatus->PortStatus |= USB_PORT_STAT_ENABLE;
  }

  if (Hprt0 & DWC2_HPRT0_PRTSUSP) {
    PortStatus->PortStatus |= USB_PORT_STAT_SUSPEND;
  }

  if (Hprt0 & DWC2_HPRT0_PRTOVRCURRACT) {
    PortStatus->PortStatus |= USB_PORT_STAT_OVERCURRENT;
  }

  if (Hprt0 & DWC2_HPRT0_PRTRST) {
    PortStatus->PortStatus |= USB_PORT_STAT_RESET;
  }

  if (Hprt0 & DWC2_HPRT0_PRTPWR) {
    PortStatus->PortStatus |= USB_PORT_STAT_POWER;
  }

  PortStatus->PortStatus |= USB_PORT_STAT_HIGH_SPEED;

  if (Hprt0 & DWC2_HPRT0_PRTENCHNG) {
    //              PortStatus->PortChangeStatus |= USB_PORT_STAT_C_ENABLE;
  }

  if (Hprt0 & DWC2_HPRT0_PRTCONNDET) {
    PortStatus->PortChangeStatus |= USB_PORT_STAT_C_CONNECTION;
  }

  if (Hprt0 & DWC2_HPRT0_PRTOVRCURRCHNG) {
    PortStatus->PortChangeStatus |= USB_PORT_STAT_C_OVERCURRENT;
  }

  return EFI_SUCCESS;
}

EFI_STATUS
EFIAPI
DwHcSetRootHubPortFeature (
                           IN  EFI_USB2_HC_PROTOCOL  *This,
                           IN  UINT8                 PortNumber,
                           IN  EFI_USB_PORT_FEATURE  PortFeature
                           )
{
  DWUSB_OTGHC_DEV         *DwHc;
  UINT32                  Hprt0;
  EFI_STATUS              Status = EFI_SUCCESS;

  if (PortNumber > DWC2_HC_PORT) {
    Status = EFI_INVALID_PARAMETER;
    goto End;
  }

  DwHc = DWHC_FROM_THIS (This);

  switch (PortFeature) {
  case EfiUsbPortEnable:
    break;
  case EfiUsbPortSuspend:
    Hprt0 = MmioRead32 (DwHc->DwUsbBase + HPRT0);
    Hprt0 &= ~(DWC2_HPRT0_PRTENA | DWC2_HPRT0_PRTCONNDET |
               DWC2_HPRT0_PRTENCHNG | DWC2_HPRT0_PRTOVRCURRCHNG);
    Hprt0 |= DWC2_HPRT0_PRTSUSP;
    MmioWrite32 (DwHc->DwUsbBase + HPRT0, Hprt0);
    break;
  case EfiUsbPortReset:
    MmioAndThenOr32 (DwHc->DwUsbBase + HPRT0,
                     ~(DWC2_HPRT0_PRTENA | DWC2_HPRT0_PRTCONNDET |
                       DWC2_HPRT0_PRTENCHNG | DWC2_HPRT0_PRTOVRCURRCHNG),
                     DWC2_HPRT0_PRTRST);
    MicroSecondDelay (50000);
    MmioAnd32 (DwHc->DwUsbBase + HPRT0, ~DWC2_HPRT0_PRTRST);
    break;
  case EfiUsbPortPower:
    Hprt0 = MmioRead32 (DwHc->DwUsbBase + HPRT0);
    Hprt0 &= ~(DWC2_HPRT0_PRTENA | DWC2_HPRT0_PRTCONNDET |
               DWC2_HPRT0_PRTENCHNG | DWC2_HPRT0_PRTOVRCURRCHNG);
    Hprt0 |= DWC2_HPRT0_PRTPWR;
    MmioWrite32 (DwHc->DwUsbBase + HPRT0, Hprt0);
    break;
  case EfiUsbPortOwner:
    break;
  default:
    Status = EFI_INVALID_PARAMETER;
    break;
  }

 End:
  return Status;
}

EFI_STATUS
EFIAPI
DwHcClearRootHubPortFeature (
                             IN  EFI_USB2_HC_PROTOCOL  *This,
                             IN  UINT8                 PortNumber,
                             IN  EFI_USB_PORT_FEATURE  PortFeature
                             )
{
  DWUSB_OTGHC_DEV         *DwHc;
  UINT32                  Hprt0;
  EFI_STATUS              Status = EFI_SUCCESS;

  if (PortNumber > DWC2_HC_PORT) {
    Status = EFI_INVALID_PARAMETER;
    goto End;
  }

  DwHc = DWHC_FROM_THIS (This);

  switch (PortFeature) {
  case EfiUsbPortEnable:
    Hprt0 = MmioRead32 (DwHc->DwUsbBase + HPRT0);
    Hprt0 &= ~(DWC2_HPRT0_PRTENA | DWC2_HPRT0_PRTCONNDET |
               DWC2_HPRT0_PRTENCHNG | DWC2_HPRT0_PRTOVRCURRCHNG);
    Hprt0 |= DWC2_HPRT0_PRTENA;
    MmioWrite32 (DwHc->DwUsbBase + HPRT0, Hprt0);
    break;
  case EfiUsbPortReset:
    MmioAndThenOr32 (DwHc->DwUsbBase + HPRT0,
                     ~(DWC2_HPRT0_PRTENA | DWC2_HPRT0_PRTCONNDET |
                       DWC2_HPRT0_PRTENCHNG | DWC2_HPRT0_PRTOVRCURRCHNG),
                     DWC2_HPRT0_PRTRST);
    MicroSecondDelay (50000);
    MmioAnd32 (DwHc->DwUsbBase + HPRT0, ~DWC2_HPRT0_PRTRST);
    break;
  case EfiUsbPortSuspend:
    MmioWrite32 (DwHc->DwUsbBase + PCGCCTL, 0);
    MicroSecondDelay (40000);
    Hprt0 = MmioRead32 (DwHc->DwUsbBase + HPRT0);
    Hprt0 &= ~(DWC2_HPRT0_PRTENA | DWC2_HPRT0_PRTCONNDET |
               DWC2_HPRT0_PRTENCHNG | DWC2_HPRT0_PRTOVRCURRCHNG);
    Hprt0 |= DWC2_HPRT0_PRTRES;
    MmioWrite32 (DwHc->DwUsbBase + HPRT0, Hprt0);
    Hprt0 &= ~DWC2_HPRT0_PRTSUSP;
    MicroSecondDelay (150000);
    Hprt0 &= ~DWC2_HPRT0_PRTRES;
    MmioWrite32 (DwHc->DwUsbBase + HPRT0, Hprt0);
    break;
  case EfiUsbPortPower:
    Hprt0 = MmioRead32 (DwHc->DwUsbBase + HPRT0);
    Hprt0 &= ~(DWC2_HPRT0_PRTENA | DWC2_HPRT0_PRTCONNDET |
               DWC2_HPRT0_PRTENCHNG | DWC2_HPRT0_PRTOVRCURRCHNG);
    Hprt0 &= ~DWC2_HPRT0_PRTPWR;
    MmioWrite32 (DwHc->DwUsbBase + HPRT0, Hprt0);
    break;
  case EfiUsbPortOwner:
    break;
  case EfiUsbPortConnectChange:
    Hprt0 = MmioRead32 (DwHc->DwUsbBase + HPRT0);
    Hprt0 &= ~DWC2_HPRT0_PRTCONNDET;
    MmioWrite32 (DwHc->DwUsbBase + HPRT0, Hprt0);
    break;
  case EfiUsbPortResetChange:
    break;
  case EfiUsbPortEnableChange:
    Hprt0 = MmioRead32 (DwHc->DwUsbBase + HPRT0);
    Hprt0 &= ~DWC2_HPRT0_PRTENCHNG;
    MmioWrite32 (DwHc->DwUsbBase + HPRT0, Hprt0);
    break;
  case EfiUsbPortSuspendChange:
    break;
  case EfiUsbPortOverCurrentChange:
    Hprt0 = MmioRead32 (DwHc->DwUsbBase + HPRT0);
    Hprt0 &= ~DWC2_HPRT0_PRTOVRCURRCHNG;
    MmioWrite32 (DwHc->DwUsbBase + HPRT0, Hprt0);
    break;
  default:
    Status = EFI_INVALID_PARAMETER;
    break;
  }

 End:
  return Status;
}

EFI_STATUS
EFIAPI
DwHcControlTransfer (
                     IN  EFI_USB2_HC_PROTOCOL                *This,
                     IN  UINT8                               DeviceAddress,
                     IN  UINT8                               DeviceSpeed,
                     IN  UINTN                               MaximumPacketLength,
                     IN  EFI_USB_DEVICE_REQUEST              *Request,
                     IN  EFI_USB_DATA_DIRECTION              TransferDirection,
                     IN  OUT VOID                            *Data,
                     IN  OUT UINTN                           *DataLength,
                     IN  UINTN                               TimeOut,
                     IN  EFI_USB2_HC_TRANSACTION_TRANSLATOR  *Translator,
                     OUT UINT32                              *TransferResult
                     )
{
  DWUSB_OTGHC_DEV         *DwHc;
  EFI_STATUS              Status;
  UINT32                  Pid;
  UINTN                   Length;
  EFI_USB_DATA_DIRECTION  StatusDirection;
  UINT32                  Direction;

  if ((Request == NULL) || (TransferResult == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  if ((TransferDirection != EfiUsbDataIn) &&
      (TransferDirection != EfiUsbDataOut) &&
      (TransferDirection != EfiUsbNoData)) {
    return EFI_INVALID_PARAMETER;
  }

  if ((TransferDirection == EfiUsbNoData) &&
      ((Data != NULL) || (*DataLength != 0))) {
    return EFI_INVALID_PARAMETER;
  }

  if ((TransferDirection != EfiUsbNoData) &&
      ((Data == NULL) || (*DataLength == 0))) {
    return EFI_INVALID_PARAMETER;
  }

  if ((MaximumPacketLength != 8) && (MaximumPacketLength != 16) &&
      (MaximumPacketLength != 32) && (MaximumPacketLength != 64)) {
    return EFI_INVALID_PARAMETER;
  }

  if ((DeviceSpeed == EFI_USB_SPEED_LOW) && (MaximumPacketLength != 8)) {
    return EFI_INVALID_PARAMETER;
  }

  DwHc  = DWHC_FROM_THIS(This);

  *TransferResult = EFI_USB_ERR_SYSTEM;
  Status          = EFI_DEVICE_ERROR;

  Pid = DWC2_HC_PID_SETUP;
  Length = 8;
  Status = DwHcTransfer (DwHc, DWC2_HC_CHANNEL, Translator, DeviceSpeed,
                         DeviceAddress, MaximumPacketLength, &Pid, 0,
                         Request, &Length, 0, DWC2_HCCHAR_EPTYPE_CONTROL,
                         TransferResult, 1);

  if (EFI_ERROR(Status)) {
    DEBUG ((EFI_D_ERROR, "DwHcControlTransfer: Setup Stage Error\n"));
    goto EXIT;
  }

  if (Data) {
    Pid = DWC2_HC_PID_DATA1;

    if (TransferDirection == EfiUsbDataIn)
      Direction = 1;
    else
      Direction = 0;

    Status = DwHcTransfer (DwHc, DWC2_HC_CHANNEL, Translator, DeviceSpeed,
                           DeviceAddress, MaximumPacketLength, &Pid,
                           Direction, Data, DataLength, 0,
                           DWC2_HCCHAR_EPTYPE_CONTROL,
                           TransferResult, 0);

    if (EFI_ERROR(Status)) {
      DEBUG ((EFI_D_ERROR, "DwHcControlTransfer: Data Stage Error\n"));
      goto EXIT;
    }
  }

  if ((TransferDirection == EfiUsbDataOut) || (TransferDirection == EfiUsbNoData))
    StatusDirection = 1;
  else
    StatusDirection = 0;

  Pid = DWC2_HC_PID_DATA1;
  Length = 0;
  Status = DwHcTransfer (DwHc, DWC2_HC_CHANNEL, Translator, DeviceSpeed,
                         DeviceAddress, MaximumPacketLength, &Pid,
                         StatusDirection, DwHc->StatusBuffer, &Length, 0,
                         DWC2_HCCHAR_EPTYPE_CONTROL, TransferResult, 0);

  if (EFI_ERROR(Status)) {
    DEBUG ((EFI_D_ERROR, "DwHcControlTransfer: Status Stage Error\n"));
    goto EXIT;
  }

 EXIT:
  return Status;
}

EFI_STATUS
EFIAPI
DwHcBulkTransfer (
                  IN  EFI_USB2_HC_PROTOCOL                *This,
                  IN  UINT8                               DeviceAddress,
                  IN  UINT8                               EndPointAddress,
                  IN  UINT8                               DeviceSpeed,
                  IN  UINTN                               MaximumPacketLength,
                  IN  UINT8                               DataBuffersNumber,
                  IN  OUT VOID                            *Data[EFI_USB_MAX_BULK_BUFFER_NUM],
                  IN  OUT UINTN                           *DataLength,
                  IN  OUT UINT8                           *DataToggle,
                  IN  UINTN                               TimeOut,
                  IN  EFI_USB2_HC_TRANSACTION_TRANSLATOR  *Translator,
                  OUT UINT32                              *TransferResult
                  )
{
  DWUSB_OTGHC_DEV         *DwHc;
  EFI_STATUS              Status;
  UINT8                   TransferDirection;
  UINT8                   EpAddress;
  UINT32                  Pid;

  if ((Data == NULL) || (Data[0] == NULL) ||
      (DataLength == NULL) || (*DataLength == 0) ||
      (TransferResult == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  if ((*DataToggle != 0) && (*DataToggle != 1))
    return EFI_INVALID_PARAMETER;

  if ((DeviceSpeed == EFI_USB_SPEED_LOW) || (DeviceSpeed == EFI_USB_SPEED_SUPER))
    return EFI_INVALID_PARAMETER;

  if (((DeviceSpeed == EFI_USB_SPEED_FULL) && (MaximumPacketLength > 64)) ||
      ((DeviceSpeed == EFI_USB_SPEED_HIGH) && (MaximumPacketLength > 512)))
    return EFI_INVALID_PARAMETER;

  DwHc = DWHC_FROM_THIS (This);

  *TransferResult         = EFI_USB_ERR_SYSTEM;
  Status                  = EFI_DEVICE_ERROR;
  TransferDirection       = (EndPointAddress >> 7) & 0x01;
  EpAddress               = EndPointAddress & 0x0F;
  Pid                     = (*DataToggle << 1);

  Status = DwHcTransfer (DwHc, DWC2_HC_CHANNEL, Translator, DeviceSpeed,
                         DeviceAddress, MaximumPacketLength, &Pid,
                         TransferDirection, Data[0], DataLength, EpAddress,
                         DWC2_HCCHAR_EPTYPE_BULK, TransferResult, 1);

  *DataToggle = (Pid >> 1);

  return Status;
}

EFI_STATUS
EFIAPI
DwHcAsyncInterruptTransfer (
                            IN  EFI_USB2_HC_PROTOCOL                  *This,
                            IN  UINT8                                 DeviceAddress,
                            IN  UINT8                                 EndPointAddress,
                            IN  UINT8                                 DeviceSpeed,
                            IN  UINTN                                 MaximumPacketLength,
                            IN  BOOLEAN                               IsNewTransfer,
                            IN  OUT UINT8                             *DataToggle,
                            IN  UINTN                                 PollingInterval,
                            IN  UINTN                                 DataLength,
                            IN  EFI_USB2_HC_TRANSACTION_TRANSLATOR    *Translator,
                            IN  EFI_ASYNC_USB_TRANSFER_CALLBACK       CallbackFunction,
                            IN  VOID                                  *Context OPTIONAL
                            )
{
  DWUSB_OTGHC_DEV                 *DwHc;
  EFI_STATUS                      Status;
  EFI_TPL                         PreviousTpl;
  VOID                            *Data = NULL;
  DWUSB_DEFERRED_REQ              *FoundReq = NULL;
  DWUSB_DEFERRED_REQ              *NewReq = NULL;

  if (!(EndPointAddress & USB_ENDPOINT_DIR_IN)) {
    return EFI_INVALID_PARAMETER;
  }

  DwHc = DWHC_FROM_THIS (This);

  PreviousTpl = gBS->RaiseTPL(TPL_NOTIFY);
  FoundReq = DwHcFindDeferredTransfer(DwHc, DeviceAddress, EndPointAddress);

  if (IsNewTransfer) {
    if (FoundReq != NULL) {
      Status = EFI_INVALID_PARAMETER;
      goto done;
    }

    if (DataLength == 0) {
      Status = EFI_INVALID_PARAMETER;
      goto done;
    }

    if ((*DataToggle != 1) && (*DataToggle != 0)) {
      Status = EFI_INVALID_PARAMETER;
      goto done;
    }

    if ((PollingInterval > 255) || (PollingInterval < 1)) {
      Status = EFI_INVALID_PARAMETER;
      goto done;
    }

    if (CallbackFunction == NULL) {
      Status = EFI_INVALID_PARAMETER;
      goto done;
    }
  }

  if (!IsNewTransfer) {
    if (FoundReq == NULL) {
      DEBUG ((DEBUG_ERROR, "Not ending periodic transfer - not found\n"));
      Status = EFI_INVALID_PARAMETER;
      goto done;
    }

    gBS->CloseEvent (FoundReq->Event);
    *DataToggle = FoundReq->Pid >> 1;
    FreePool (FoundReq->Data);

    RemoveEntryList (&FoundReq->List);
    FreePool (FoundReq);

    Status = EFI_SUCCESS;
    goto done;
  }

  NewReq = AllocateZeroPool (sizeof *NewReq);
  if (NewReq == NULL) {
    DEBUG ((EFI_D_ERROR, "DwHcAsyncInterruptTransfer: failed to allocate req"));
    Status = EFI_OUT_OF_RESOURCES;
    goto done;
  }

  Data = AllocateZeroPool (DataLength);
  if (Data == NULL) {
    DEBUG ((EFI_D_ERROR, "DwHcAsyncInterruptTransfer: failed to allocate buffer\n"));
    Status = EFI_OUT_OF_RESOURCES;
    goto done;
  }

  InitializeListHead (&NewReq->List);
  Status = gBS->CreateEvent (
                             EVT_TIMER | EVT_NOTIFY_SIGNAL,
                             TPL_NOTIFY,
                             DwHcDeferredTransfer,
                             NewReq, &NewReq->Event
                             );
  if (Status != EFI_SUCCESS) {
    DEBUG ((EFI_D_ERROR, "DwHcAsyncInterruptTransfer: failed to create event: %r\n", Status));
    goto done;
  }

  Status = gBS->SetTimer (NewReq->Event, TimerPeriodic,
                          EFI_TIMER_PERIOD_MILLISECONDS(PollingInterval));
  if (Status != EFI_SUCCESS) {
    DEBUG ((EFI_D_ERROR, "DwHcAsyncInterruptTransfer: failed to set timer: %r\n", Status));
    goto done;
  }

  NewReq->DwHc = DwHc;
  NewReq->Channel = DWC2_HC_CHANNEL_PERIODIC;
  NewReq->Translator = Translator;
  NewReq->DeviceSpeed = DeviceSpeed;
  NewReq->DeviceAddress = DeviceAddress;
  NewReq->MaximumPacketLength = MaximumPacketLength;
  NewReq->TransferDirection = (EndPointAddress >> 7) & 0x01;
  NewReq->Data = Data;
  NewReq->DataLength = DataLength;
  NewReq->Pid = *DataToggle << 1;
  NewReq->EpAddress = EndPointAddress & 0x0F;
  NewReq->EpType = DWC2_HCCHAR_EPTYPE_INTR;
  NewReq->IgnoreAck = TRUE;
  NewReq->CallbackFunction = CallbackFunction;
  NewReq->CallbackContext = Context;

  InsertTailList (&DwHc->DeferredList, &NewReq->List);
  gBS->SignalEvent (NewReq->Event);

 done:
  gBS->RestoreTPL(PreviousTpl);

  if (Status != EFI_SUCCESS) {
    if (Data != NULL) {
      FreePool (Data);
    }

    if (NewReq != NULL) {
      FreePool (NewReq);
    }
  }

  return Status;
}

EFI_STATUS
EFIAPI
DwHcSyncInterruptTransfer (
                           IN  EFI_USB2_HC_PROTOCOL                *This,
                           IN  UINT8                               DeviceAddress,
                           IN  UINT8                               EndPointAddress,
                           IN  UINT8                               DeviceSpeed,
                           IN  UINTN                               MaximumPacketLength,
                           IN  OUT VOID                            *Data,
                           IN  OUT UINTN                           *DataLength,
                           IN  OUT UINT8                           *DataToggle,
                           IN  UINTN                               TimeOut,
                           IN  EFI_USB2_HC_TRANSACTION_TRANSLATOR  *Translator,
                           OUT UINT32                              *TransferResult
                           )
{
  return EFI_UNSUPPORTED;
}

EFI_STATUS
EFIAPI
DwHcIsochronousTransfer (
                         IN  EFI_USB2_HC_PROTOCOL                *This,
                         IN  UINT8                               DeviceAddress,
                         IN  UINT8                               EndPointAddress,
                         IN  UINT8                               DeviceSpeed,
                         IN  UINTN                               MaximumPacketLength,
                         IN  UINT8                               DataBuffersNumber,
                         IN  OUT VOID                            *Data[EFI_USB_MAX_ISO_BUFFER_NUM],
                         IN  UINTN                               DataLength,
                         IN  EFI_USB2_HC_TRANSACTION_TRANSLATOR  *Translator,
                         OUT UINT32                              *TransferResult
                         )
{
  return EFI_UNSUPPORTED;
}

EFI_STATUS
EFIAPI
DwHcAsyncIsochronousTransfer (
                              IN  EFI_USB2_HC_PROTOCOL                *This,
                              IN  UINT8                               DeviceAddress,
                              IN  UINT8                               EndPointAddress,
                              IN  UINT8                               DeviceSpeed,
                              IN  UINTN                               MaximumPacketLength,
                              IN  UINT8                               DataBuffersNumber,
                              IN  OUT VOID                            *Data[EFI_USB_MAX_ISO_BUFFER_NUM],
                              IN  UINTN                               DataLength,
                              IN  EFI_USB2_HC_TRANSACTION_TRANSLATOR  *Translator,
                              IN  EFI_ASYNC_USB_TRANSFER_CALLBACK     IsochronousCallBack,
                              IN  VOID                                *Context
                              )
{
  return EFI_UNSUPPORTED;
}

/**
   Supported Functions
**/

VOID
InitFslspClkSel (
                 IN DWUSB_OTGHC_DEV *DwHc
                 )
{
  UINT32  PhyClk;

  PhyClk = DWC2_HCFG_FSLSPCLKSEL_30_60_MHZ;

  MmioAndThenOr32 (DwHc->DwUsbBase + HCFG,
                   ~DWC2_HCFG_FSLSPCLKSEL_MASK,
                   PhyClk << DWC2_HCFG_FSLSPCLKSEL_OFFSET);
}

VOID
DwFlushTxFifo (
               IN DWUSB_OTGHC_DEV *DwHc,
               IN INT32 Num
               )
{
  UINT32 Status;

  MmioWrite32 (DwHc->DwUsbBase + GRSTCTL, DWC2_GRSTCTL_TXFFLSH | (Num << DWC2_GRSTCTL_TXFNUM_OFFSET));

  Status = Wait4Bit (DwHc->DwUsbBase + GRSTCTL, DWC2_GRSTCTL_TXFFLSH, 0);
  if (Status)
    DEBUG ((EFI_D_ERROR, "DwFlushTxFifo: Timeout!\n"));

  MicroSecondDelay (1);
}

VOID
DwFlushRxFifo (
               IN DWUSB_OTGHC_DEV *DwHc
               )
{
  UINT32 Status;

  MmioWrite32 (DwHc->DwUsbBase + GRSTCTL, DWC2_GRSTCTL_RXFFLSH);

  Status = Wait4Bit (DwHc->DwUsbBase + GRSTCTL, DWC2_GRSTCTL_RXFFLSH, 0);
  if (Status)
    DEBUG ((EFI_D_ERROR, "DwFlushRxFifo: Timeout!\n"));

  MicroSecondDelay (1);
}

VOID
DwHcInit (
          IN DWUSB_OTGHC_DEV *DwHc
          )
{
  UINT32 NpTxFifoSz = 0;
  UINT32 pTxFifoSz = 0;
  UINT32 Hprt0 = 0;
  INT32  i, Status, NumChannels;

  MmioWrite32 (DwHc->DwUsbBase + PCGCCTL, 0);

  InitFslspClkSel (DwHc);

  MmioWrite32 (DwHc->DwUsbBase + GRXFSIZ, DWC2_HOST_RX_FIFO_SIZE);

  NpTxFifoSz |= DWC2_HOST_NPERIO_TX_FIFO_SIZE << DWC2_FIFOSIZE_DEPTH_OFFSET;
  NpTxFifoSz |= DWC2_HOST_RX_FIFO_SIZE << DWC2_FIFOSIZE_STARTADDR_OFFSET;
  MmioWrite32 (DwHc->DwUsbBase + GNPTXFSIZ, NpTxFifoSz);

  pTxFifoSz |= DWC2_HOST_PERIO_TX_FIFO_SIZE << DWC2_FIFOSIZE_DEPTH_OFFSET;
  pTxFifoSz |= (DWC2_HOST_RX_FIFO_SIZE + DWC2_HOST_NPERIO_TX_FIFO_SIZE) <<
    DWC2_FIFOSIZE_STARTADDR_OFFSET;
  MmioWrite32 (DwHc->DwUsbBase + HPTXFSIZ, pTxFifoSz);

  MmioAnd32 (DwHc->DwUsbBase + GOTGCTL, ~(DWC2_GOTGCTL_HSTSETHNPEN));

  DwFlushTxFifo (DwHc, 0x10);
  DwFlushRxFifo (DwHc);

  NumChannels = MmioRead32 (DwHc->DwUsbBase + GHWCFG2);
  NumChannels &= DWC2_HWCFG2_NUM_HOST_CHAN_MASK;
  NumChannels >>= DWC2_HWCFG2_NUM_HOST_CHAN_OFFSET;
  NumChannels += 1;
  DEBUG ((DEBUG_INFO, "Host has %u channels\n", NumChannels));

  for (i=0; i<NumChannels; i++)
    MmioAndThenOr32 (DwHc->DwUsbBase + HCCHAR(i),
                     ~(DWC2_HCCHAR_CHEN | DWC2_HCCHAR_EPDIR),
                     DWC2_HCCHAR_CHDIS);

  for (i=0; i<NumChannels; i++) {
    MmioAndThenOr32 (DwHc->DwUsbBase + HCCHAR(i),
                     ~DWC2_HCCHAR_EPDIR,
                     (DWC2_HCCHAR_CHEN | DWC2_HCCHAR_CHDIS));
    Status = Wait4Bit (DwHc->DwUsbBase + HCCHAR(i), DWC2_HCCHAR_CHEN, 0);
    if (Status)
      DEBUG ((EFI_D_ERROR, "DwHcInit: Timeout!\n"));
  }

  if (MmioRead32 (DwHc->DwUsbBase + GINTSTS) & DWC2_GINTSTS_CURMODE_HOST) {
    Hprt0 = MmioRead32 (DwHc->DwUsbBase + HPRT0);
    Hprt0 &= ~(DWC2_HPRT0_PRTENA | DWC2_HPRT0_PRTCONNDET);
    Hprt0 &= ~(DWC2_HPRT0_PRTENCHNG | DWC2_HPRT0_PRTOVRCURRCHNG);

    if (!(Hprt0 & DWC2_HPRT0_PRTPWR)) {
      Hprt0 |= DWC2_HPRT0_PRTPWR;
      MmioWrite32 (DwHc->DwUsbBase + HPRT0, Hprt0);
    }
  }
}

VOID
DwCoreInit (
            IN DWUSB_OTGHC_DEV *DwHc
            )
{
  UINT32          AhbCfg = 0;
  UINT32          UsbCfg = 0;

  UsbCfg = MmioRead32 (DwHc->DwUsbBase + GUSBCFG);

  UsbCfg |= DWC2_GUSBCFG_ULPI_EXT_VBUS_DRV;
  UsbCfg &= ~DWC2_GUSBCFG_TERM_SEL_DL_PULSE;

  MmioWrite32 (DwHc->DwUsbBase + GUSBCFG, UsbCfg);

  DwCoreReset (DwHc);

  UsbCfg &= ~(DWC2_GUSBCFG_ULPI_UTMI_SEL | DWC2_GUSBCFG_PHYIF);
  UsbCfg |= CONFIG_DWC2_PHY_TYPE << DWC2_GUSBCFG_ULPI_UTMI_SEL_OFFSET;
  UsbCfg &= ~DWC2_GUSBCFG_DDRSEL;

  MmioWrite32 (DwHc->DwUsbBase + GUSBCFG, UsbCfg);

  DwCoreReset (DwHc);

  UsbCfg = MmioRead32 (DwHc->DwUsbBase + GUSBCFG);

  UsbCfg &= ~(DWC2_GUSBCFG_ULPI_FSLS | DWC2_GUSBCFG_ULPI_CLK_SUS_M);
  MmioWrite32 (DwHc->DwUsbBase + GUSBCFG, UsbCfg);

  AhbCfg &= ~DWC2_GAHBCFG_AXI_BURST4_MASK;
  AhbCfg |= DWC2_GAHBCFG_DMAENABLE | DWC2_GAHBCFG_WAIT_AXI_WRITES;

  MmioWrite32 (DwHc->DwUsbBase + GAHBCFG, AhbCfg);
  MmioAnd32 (DwHc->DwUsbBase + GUSBCFG, ~(DWC2_GUSBCFG_HNPCAP | DWC2_GUSBCFG_SRPCAP));
}

DWUSB_OTGHC_DEV *
CreateDwUsbHc (
               VOID
               )
{
  DWUSB_OTGHC_DEV *DwHc;
  UINT32          Pages;
  UINTN           BufferSize;
  EFI_STATUS      Status;

  DwHc = AllocateZeroPool (sizeof(DWUSB_OTGHC_DEV));

  if (DwHc == NULL) {
    return NULL;
  }

  DwHc->Signature                                 = DWUSB_OTGHC_DEV_SIGNATURE;
  DwHc->DwUsbOtgHc.GetCapability                  = DwHcGetCapability;
  DwHc->DwUsbOtgHc.Reset                          = DwHcReset;
  DwHc->DwUsbOtgHc.GetState                       = DwHcGetState;
  DwHc->DwUsbOtgHc.SetState                       = DwHcSetState;
  DwHc->DwUsbOtgHc.ControlTransfer                = DwHcControlTransfer;
  DwHc->DwUsbOtgHc.BulkTransfer                   = DwHcBulkTransfer;
  DwHc->DwUsbOtgHc.AsyncInterruptTransfer         = DwHcAsyncInterruptTransfer;
  DwHc->DwUsbOtgHc.SyncInterruptTransfer          = DwHcSyncInterruptTransfer;
  DwHc->DwUsbOtgHc.IsochronousTransfer            = DwHcIsochronousTransfer;
  DwHc->DwUsbOtgHc.AsyncIsochronousTransfer       = DwHcAsyncIsochronousTransfer;
  DwHc->DwUsbOtgHc.GetRootHubPortStatus           = DwHcGetRootHubPortStatus;
  DwHc->DwUsbOtgHc.SetRootHubPortFeature          = DwHcSetRootHubPortFeature;
  DwHc->DwUsbOtgHc.ClearRootHubPortFeature        = DwHcClearRootHubPortFeature;
  DwHc->DwUsbOtgHc.MajorRevision                  = 0x02;
  DwHc->DwUsbOtgHc.MinorRevision                  = 0x00;
  DwHc->DwUsbBase                                 = BCM2836_USB_DW2_BASE_ADDRESS;

  CopyMem (&DwHc->DevicePath, &DwHcDevicePath, sizeof(DwHcDevicePath));

  Pages = EFI_SIZE_TO_PAGES (DWC2_STATUS_BUF_SIZE);
  DwHc->StatusBuffer = AllocatePages(Pages);
  if (DwHc->StatusBuffer == NULL) {
    DEBUG ((EFI_D_ERROR, "CreateDwUsbHc: No pages available for StatusBuffer\n"));
    return NULL;
  }

  Pages = EFI_SIZE_TO_PAGES (DWC2_DATA_BUF_SIZE);
  Status = DmaAllocateBuffer (EfiBootServicesData, Pages, (VOID **) &DwHc->AlignedBuffer);
  if (EFI_ERROR (Status)) {
    DEBUG ((EFI_D_ERROR, "CreateDwUsbHc: No pages available for AlignedBuffer\n"));
    return NULL;
  }

  BufferSize = EFI_PAGES_TO_SIZE (Pages);
  Status = DmaMap (MapOperationBusMasterCommonBuffer, DwHc->AlignedBuffer, &BufferSize,
                   &DwHc->AlignedBufferBusAddress, &DwHc->AlignedBufferMapping);
  if (EFI_ERROR (Status)) {
    return NULL;
  }

  InitializeListHead (&DwHc->DeferredList);

  return DwHc;
}

VOID
EFIAPI
DwUsbHcExitBootService (
                        EFI_EVENT     Event,
                        VOID          *Context
                        )
{
  DWUSB_OTGHC_DEV         *DwHc;

  DwHc = (DWUSB_OTGHC_DEV *) Context;

  DwHcCancelDeferredTransfers(DwHc);

  MmioAndThenOr32 (DwHc->DwUsbBase + HPRT0,
                   ~(DWC2_HPRT0_PRTENA | DWC2_HPRT0_PRTCONNDET |
                     DWC2_HPRT0_PRTENCHNG | DWC2_HPRT0_PRTOVRCURRCHNG),
                   DWC2_HPRT0_PRTRST);

  MicroSecondDelay (50000);

  DwCoreReset (DwHc);
}

/**
   UEFI Driver Entry Point API

   @param  ImageHandle       EFI_HANDLE.
   @param  SystemTable       EFI_SYSTEM_TABLE.

   @return EFI_SUCCESS       Success.
   EFI_DEVICE_ERROR  Fail.
**/

EFI_STATUS
EFIAPI
DwUsbHostEntryPoint (
                     IN EFI_HANDLE           ImageHandle,
                     IN EFI_SYSTEM_TABLE     *SystemTable
                     )
{
  EFI_STATUS                      Status;
  DWUSB_OTGHC_DEV                 *DwHc;
  UINT32                          Pages;
  STATIC RASPBERRY_PI_FIRMWARE_PROTOCOL *FwProtocol;

  Status = gBS->LocateProtocol (&gRaspberryPiFirmwareProtocolGuid, NULL,
                                (VOID **)&FwProtocol);
  ASSERT_EFI_ERROR (Status);

  Status = FwProtocol->SetPowerState(RPI_FW_POWER_STATE_USB_HCD, TRUE, TRUE);
  if (EFI_ERROR (Status)) {
    DEBUG((DEBUG_ERROR, "Couldn't power on USB HCD\n"));
    return Status;
  }

  DwHc = CreateDwUsbHc ();

  if (DwHc == NULL) {
    Status = EFI_OUT_OF_RESOURCES;
    goto EXIT;
  }

  /*
   * UsbBusDxe as of b4e96b82b4e2e47e95014b51787ba5b43abac784 expects
   * the HCD to do this. There is no agent invoking DwHcReset anymore.
   */
  DwHcReset(&DwHc->DwUsbOtgHc, 0);
  DwHcSetState(&DwHc->DwUsbOtgHc, EfiUsbHcStateOperational);

  Status = gBS->InstallMultipleProtocolInterfaces (
                                                   &DwHc->DeviceHandle,
                                                   &gEfiUsb2HcProtocolGuid,        &DwHc->DwUsbOtgHc,
                                                   &gEfiDevicePathProtocolGuid,    &DwHc->DevicePath,
                                                   NULL
                                                   );

  if (EFI_ERROR (Status)) {
    goto FREE_DWUSBHC;
  }

  Status = gBS->CreateEventEx (
                               EVT_NOTIFY_SIGNAL,
                               TPL_NOTIFY,
                               DwUsbHcExitBootService,
                               DwHc,
                               &gEfiEventExitBootServicesGuid,
                               &DwHc->ExitBootServiceEvent
                               );

  if (EFI_ERROR (Status)) {
    goto UNINSTALL_PROTOCOL;
  }

  return Status;

 UNINSTALL_PROTOCOL:
  gBS->UninstallMultipleProtocolInterfaces (
                                            &DwHc->DeviceHandle,
                                            &gEfiUsb2HcProtocolGuid,        &DwHc->DwUsbOtgHc,
                                            &gEfiDevicePathProtocolGuid,    &DwHc->DevicePath,
                                            NULL
                                            );
 FREE_DWUSBHC:
  Pages = EFI_SIZE_TO_PAGES (DWC2_STATUS_BUF_SIZE);
  FreePages (DwHc->StatusBuffer, Pages);
  Pages = EFI_SIZE_TO_PAGES (DWC2_DATA_BUF_SIZE);
  DmaUnmap (DwHc->AlignedBufferMapping);
  DmaFreeBuffer (Pages, DwHc->AlignedBuffer);
  gBS->FreePool (DwHc);
 EXIT:
  return Status;
}

EFI_STATUS
EFIAPI
DwUsbHostExitPoint (
                    IN EFI_HANDLE ImageHandle
                    )
{
  EFI_STATUS              Status;
  EFI_USB2_HC_PROTOCOL    *DwUsbHc;
  DWUSB_OTGHC_DEV         *DwHc;
  UINT32                  Pages;

  Status = EFI_SUCCESS;

  Status = gBS->LocateProtocol (&gEfiUsb2HcProtocolGuid, NULL, (VOID **) &DwUsbHc);

  if (EFI_ERROR (Status)) {
    return Status;
  }

  DwHc  = DWHC_FROM_THIS(DwUsbHc);

  if (DwHc->ExitBootServiceEvent != NULL) {
    gBS->CloseEvent (DwHc->ExitBootServiceEvent);
  }

  gBS->UninstallMultipleProtocolInterfaces (
                                            &DwHc->DeviceHandle,
                                            &gEfiUsb2HcProtocolGuid,        &DwHc->DwUsbOtgHc,
                                            &gEfiDevicePathProtocolGuid,    &DwHc->DevicePath,
                                            NULL
                                            );

  if (EFI_ERROR (Status)) {
    return Status;
  }

  Pages = EFI_SIZE_TO_PAGES (DWC2_STATUS_BUF_SIZE);
  FreePages (DwHc->StatusBuffer, Pages);
  Pages = EFI_SIZE_TO_PAGES (DWC2_DATA_BUF_SIZE);
  DmaUnmap (DwHc->AlignedBufferMapping);
  DmaFreeBuffer (Pages, DwHc->AlignedBuffer);
  FreePool (DwHc);

  return Status;
}
