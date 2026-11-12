# drivers/usb/CMakeLists.txt - USB Driver (stub)

add_library(usb STATIC usb.c)

target_include_directories(usb PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_SOURCE_DIR}/kernel/include
)

target_compile_options(usb PRIVATE
        -Wall
        -Wextra
        -Werror=implicit-function-declaration
)

message(STATUS "USB driver (stub) configured")
