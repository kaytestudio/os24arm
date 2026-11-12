/**
 * @file usbd.h
 * @brief OS/2-style USB stack interface: USBD, host controller and class
 *        driver IDC (inter-device-driver communication)
 *
 * Modelled on the OS/2 Warp USB device driver stack as documented at
 * https://www.edm2.com/index.php/USB_Device_Driver_Stack_for_OS/2_Warp
 *
 *   class/client drivers (USBKBD)      category 0x92 / 0x93
 *              |  IDC
 *            USBD                      category 0x91
 *              |  IDC
 *   host controller drivers (USBXHCD)  category 0x90
 *
 * Every layer exports one IDC entry point taking a generic IOCtl request
 * packet (Cmd 10h); Category picks the layer, Function the operation.
 * Categories, functions and USB_IDC_* error codes are the documented
 * values. Differences from the 16-bit original, because this kernel is
 * flat 64-bit, single-threaded and polled:
 *   - pointers are flat (no FAR/selector:offset); usbDS/classDriverDS are
 *     opaque context pointers; requestData1..3 are pointer-sized.
 *   - there are no threads: completions are delivered from usbd_daemon(),
 *     never from inside the ACCIO call that queued the request (which is
 *     the documented requirement that notification runs separately).
 *   - USBD polls each HCD with Category 0x90 / Function PRCIRQ, since
 *     there are no hardware interrupts to drive it.
 * Values marked "local" are this implementation's own: the page does not
 * give them.
 */
#ifndef DRIVERS_USB_USBD_H
#define DRIVERS_USB_USBD_H

#include "types.h"

/* ── Request packet (generic IOCtl) ───────────────────────────────────── */
#define CMDGenIOCTL                 0x10
#define STATUS_DONE                 0x0100
#define STERR                       0x8000

typedef struct RP_GENIOCTL {
    uint8_t   Len;
    uint8_t   Unit;
    uint8_t   Cmd;              /* must be CMDGenIOCTL */
    uint16_t  Status;           /* STATUS_DONE [| STERR | error code] */
    uint8_t   Category;
    uint8_t   Function;
    void     *ParmPacket;
    void     *DataPacket;
    uint16_t  sfn;              /* ignored, as in USBUHCD/USBD */
} RP_GENIOCTL;

typedef void (*usb_idc_fn)(RP_GENIOCTL *pRP_GENIOCTL);

/* ── Categories ───────────────────────────────────────────────────────── */
#define USB_IDC_CATEGORY_HOST       0x90
#define USB_IDC_CATEGORY_USBD       0x91
#define USB_IDC_CATEGORY_CLASS      0x92
#define USB_IDC_CATEGORY_CLIENT     0x93

/* ── Functions ────────────────────────────────────────────────────────── */
#define USB_IDC_FUNCTION_ACCIO      0x41
#define USB_IDC_FUNCTION_CANCEL     0x42
#define USB_IDC_FUNCTION_REGISTER   0x43
#define USB_IDC_FUNCTION_PRCIRQ     0x44
#define USB_IDC_FUNCTION_CHKSERV    0x45
#define USB_IDC_FUNCTION_DETDEV     0x46
#define USB_IDC_FUNCTION_RSTHOST    0x47
#define USB_IDC_FUNCTION_SETCONF    0x48
#define USB_IDC_FUNCTION_SETINTF    0x49
#define USB_IDC_FUNCTION_CLRSTALL   0x4a
#define USB_IDC_FUNCTION_CMPL_INI   0x4c
#define USB_IDC_FUNCTION_APM        0x4d
#define USB_IDC_FUNCTION_UNREGISTER 0x4f
#define USB_IDC_FUNCTION_RESET_PORT 0x51
#define USB_IDC_FUNCTION_IDLE       0x52

/* ── Error codes (low 7 bits of Status, with STERR) ───────────────────── */
#define USB_IDC_WRONGCAT            0x10
#define USB_IDC_WRONGFUNC           0x11
#define USB_IDC_PARMERR             0x13
#define USB_IDC_ALLOCERR            0x14   /* name local; 0x8114 documented */
#define USB_IDC_NOBANDWIDTH         0x15
#define USB_IDC_CANCELED            0x17
#define USB_IDC_ADDRINV             0x18
#define USB_IDC_SERVREJCTD          0x19
#define USB_IDC_IOFAILED            0x20

#define USB_APM_SUSPEND             1
#define USB_APM_RESUME              2

/* ── USBRb flags (local values) ───────────────────────────────────────── */
/* Low byte: transfer type. High byte: pipe type. Control transfers use
 * TTYPE_SETUP with buffer1 = 8-byte setup packet, buffer2 = data stage;
 * everything else uses buffer1 for the data. */
#define USRB_FLAGS_TTYPE_SETUP      0x0000
#define USRB_FLAGS_TTYPE_IN         0x0001
#define USRB_FLAGS_TTYPE_OUT        0x0002
#define USRB_FLAGS_TTYPE_MASK       0x0003
#define USRB_FLAGS_DET_LOWSPEED     0x0010  /* set by USBD */
#define USRB_FLAGS_DET_HIGHSPEED    0x0020  /* set by USBD */
#define USRB_FLAGS_DET_ISOHREQ      0x0100
#define USRB_FLAGS_DET_INTRPT       0x0200
#define USRB_FLAGS_DET_BULK         0x0400

/* USBRb.status on completion (local values): 0 = success */
#define USRB_STATUS_STALLED         0x40
#define USRB_STATUS_DATABUF         0x20
#define USRB_STATUS_BABBLE          0x10
#define USRB_STATUS_CRC             0x04

/* ── Request block ────────────────────────────────────────────────────── */
typedef struct USBRb {
    uint16_t   controllerId;
    uint8_t    deviceAddress;   /* 1-127; 0 = not yet addressed */
    uint8_t    endPointId;      /* 0-15 */
    uint8_t    status;          /* USRB_STATUS_*, set on completion */
    uint16_t   flags;           /* USRB_FLAGS_* */
    void      *buffer1;
    uint16_t   buffer1Length;   /* on completion: bytes moved (non-control) */
    void      *buffer2;
    uint16_t   buffer2Length;   /* on completion: bytes moved (control) */
    uint16_t   serviceTime;     /* interrupt pipes: endpoint's bInterval */
    uint16_t   maxPacketSize;   /* 0 on EP0 = USBD fills in the device's */
    usb_idc_fn usbIDC;          /* completion: called with Function PRCIRQ */
    void      *usbDS;
    uint8_t    category;        /* Category for the completion call */
    uintptr_t  requestData1;    /* caller's own, returned unchanged */
    uintptr_t  requestData2;
    uintptr_t  requestData3;
    uint8_t    maxErrorCount;
    uint8_t    altInterface;
    uint16_t   isoFlags;
    uint16_t   isoFrameLength;
    uint16_t   isoBuffers;
    uint8_t    Mult;
    uint8_t    HubAddr;         /* set by USBD: hub the device is on */
    uint8_t    PortNumb;        /* set by USBD: port on that hub */
} USBRb;

/* ── Other parameter packets ──────────────────────────────────────────── */
typedef struct {
    uint16_t controllerId;
    uint8_t  deviceAddress;
    uint8_t  endPointId;        /* 0xff = all endpoints */
} USBCancel;

typedef struct {
    uint16_t controllerId;
    uint8_t  deviceAddress;
} USBDetach;

typedef struct {
    uint16_t controllerId;
    uint8_t  deviceAddress;
    void    *classDriverDS;
    uint8_t  configurationValue; /* 0 = release the device (local; see
                                  * below); SETINTF: alternate setting */
    uint16_t irqSwitchValue;     /* returned as requestData1 on completion */
    uint8_t  category;           /* Category for the completion call */
    uint8_t *setConfiguration;   /* caller's 8-byte setup packet buffer */
} USBSetConf;

typedef struct {
    uint16_t   controllerId;
    uint8_t    deviceAddress;
    uint8_t    endPointId;
    usb_idc_fn clientIDCAddr;
    void      *clientDS;
    uint16_t   irqSwitchValue;
    uint8_t    category;
    uint8_t   *clearStalled;
} USBClearStalled;

#define USB_MAX_HCD_CONTROLLERS 4
typedef struct {
    usb_idc_fn usbIDC;          /* HCD's IDC entry */
    uint16_t   hcdID;           /* set by USBD: first controllerId */
    uint16_t   hcdCount;        /* controllers this HCD drives */
    uint32_t   hDriver;
    uint32_t   hAdapter[USB_MAX_HCD_CONTROLLERS];
} USBHCD;

typedef struct {
    usb_idc_fn usbIDC;          /* class/client driver's IDC entry */
    void      *usbDS;
} USBDClass;

/* SETCONF with configurationValue 0 (local convention): the owning class
 * driver gives the device up - e.g. it turned out not to be usable. USBD
 * cancels its requests and offers it to the other drivers, not this one
 * again. setConfiguration may be NULL. */

/* REGISTER (Category 0x91): ParmPacket = USBHCD* with DataPacket NULL for
 * a host controller driver, or USBDClass* with DataPacket pointing at the
 * driver's category byte (0x92/0x93) for a class/client driver (local). */

typedef struct __attribute__((packed)) {
    uint8_t  bLength;
    uint8_t  bDescriptorType;
    uint16_t bcdUSB;
    uint8_t  bDeviceClass;
    uint8_t  bDeviceSubClass;
    uint8_t  bDeviceProtocol;
    uint8_t  bMaxPacketSize0;
    uint16_t idVendor;
    uint16_t idProduct;
    uint16_t bcdDevice;
    uint8_t  iManufacturer;
    uint8_t  iProduct;
    uint8_t  iSerialNumber;
    uint8_t  bNumConfigurations;
} DeviceDescriptor;

/* DeviceInfo.lowSpeedDevice (local values) */
#define USB_SPEED_FULL   0
#define USB_SPEED_LOW    1
#define USB_SPEED_HIGH   2
#define USB_SPEED_SUPER  3

#define MAX_CONFIG_LENGTH 512
typedef struct {
    uint16_t         ctrlID;
    uint8_t          deviceAddress;
    uint8_t          bConfigurationValue; /* 0 = unclaimed; set to accept */
    uint8_t          bInterfaceNumber;
    uint8_t          lowSpeedDevice;      /* USB_SPEED_* */
    uint8_t          portNum;
    int16_t          parentHubIndex;      /* -1: on the root hub */
    uint32_t         rmDevHandle;
    uint8_t          clearStalled[8];
    DeviceDescriptor descriptor;
    uint16_t         configLength;        /* local: bytes in configurationData */
    uint8_t          configurationData[MAX_CONFIG_LENGTH];
} DeviceInfo;

typedef struct {
    DeviceInfo *pDeviceInfo;
    void       *pFilter;
} USBCDServe;

/* ── USBD entry points ────────────────────────────────────────────────── */
void usbd_idc(RP_GENIOCTL *pRP_GENIOCTL);     /* "USBD$" IDC entry */

int  usbd_basedevinit(int verbose);           /* BASEDEVINIT (1Bh) */
void usbd_init_complete(void);                /* INITIALIZATION COMPLETE (1Fh) */
int  usbd_loaded(void);

/* The daemon: poll host controllers, deliver completions, handle hub
 * port changes (attach/detach). Call it from idle loops. */
void usbd_daemon(void);

/* Poll the host controllers once (deliver completions) without the rest
 * of the daemon. For class drivers doing synchronous I/O (e.g. block
 * reads); safe to call from inside a completion callback. */
void usbd_pump(void);

void usbd_list(void);                         /* shell: USB command */
void usbd_summary(void);                      /* one-line boot summary */

/* Helper for drivers: send an IDC request, return its Status. */
uint16_t usb_idc_call(usb_idc_fn idc, uint8_t category, uint8_t function,
                      void *parm, void *data);

/* Class drivers: give a claimed device back (SETCONF with configuration
 * 0, see above) when it turns out not to be usable. */
static inline void usb_release_device(uint16_t ctrl, uint8_t addr)
{
    USBSetConf sc = { 0 };
    sc.controllerId  = ctrl;
    sc.deviceAddress = addr;
    usb_idc_call(usbd_idc, USB_IDC_CATEGORY_USBD, USB_IDC_FUNCTION_SETCONF, &sc, 0);
}

#endif /* DRIVERS_USB_USBD_H */
