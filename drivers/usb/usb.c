/* drivers/usb/usb.c - USB driver (stub) */
#include "usb.h"

APIRET usb_reader(PVOID buffer, ULONG length, PULONG bytes_read)
{
    if (bytes_read == 0)
        return ERROR_INVALID_PARAMETER;
    *bytes_read = 0;

    if (buffer == 0 && length != 0)
        return ERROR_INVALID_PARAMETER;

    /* TODO: no USB host controller driver yet. */
    return ERROR_NOT_SUPPORTED;
}
