cmake_minimum_required(VERSION 3.20.0)

# Custom board directory (placed alongside app sources, not in Zephyr tree)
set(BOARD_ROOT ${CMAKE_CURRENT_SOURCE_DIR})

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(grovenfc_zephyr VERSION 1.0.0 LANGUAGES C)

# Custom drivers
add_subdirectory(drivers/grove_nfc)

# Application sources
target_sources(app PRIVATE
    src/main.c
    src/nfc_worker.c
    src/nfc_reader.c
    src/nfc_emulator.c
    src/ui_main.c
    src/storage.c
    src/piano.c
)

target_include_directories(app PRIVATE
    src
    drivers/grove_nfc
)
