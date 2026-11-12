# drivers/usb/CMakeLists.txt - USB drivers (xHCI host controller)

add_library(usb STATIC usb.c xhci.c)

target_include_directories(usb PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_SOURCE_DIR}/kernel/include
)

target_compile_options(usb PRIVATE
        -Wall
        -Wextra
        -Werror=implicit-function-declaration
)

message(STATUS "USB driver (xHCI) configured")
