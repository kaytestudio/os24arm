# drivers/usb/CMakeLists.txt - OS/2-style USB stack: USBD + USBXHCD + USBKBD

add_library(usb STATIC usb.c usbd.c xhci.c usbkbd.c)

target_include_directories(usb PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_SOURCE_DIR}/kernel/include
)

target_compile_options(usb PRIVATE
        -Wall
        -Wextra
        -Werror=implicit-function-declaration
)

message(STATUS "USB stack (USBD, USBXHCD, USBKBD) configured")
