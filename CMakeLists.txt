cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(t2i_fw)

target_sources(app PRIVATE src/main.c src/reset_hook.c src/hx8347_fsmc.c src/touch.c
                          src/juno_logo.c src/accel.c src/keypad.c src/wake.c src/lowpower.c src/updater.c src/ui.c src/power.c src/safety.c src/battery.c src/funlight.c src/ir.c src/zbx.c
                          src/em250.c src/etrx2_ebl.c src/zbnet.c)

# Binary assets are generated into C arrays at build time rather than committed as .c blobs, so
# each can be replaced by swapping its source binary:
#   tools/rti/etrx2.ebl        -> the EM250 radio image embedded for on-device reflash
#   assets/juno_logo.rgb565    -> the boot splash (220x60 RGB565, see tools/make_logo.sh)
set(gen_dir ${CMAKE_CURRENT_BINARY_DIR}/gen)
generate_inc_file_for_target(app ${CMAKE_CURRENT_SOURCE_DIR}/tools/rti/etrx2.ebl ${gen_dir}/etrx2_ebl.inc)
generate_inc_file_for_target(app ${CMAKE_CURRENT_SOURCE_DIR}/assets/juno_logo.rgb565 ${gen_dir}/juno_logo.inc)
target_include_directories(app PRIVATE ${gen_dir})

# Firmware version is stamped from git, never hand-tagged: on a USB-only remote the
# boot banner is the only way to confirm which commit is running.
execute_process(
  COMMAND git -C ${CMAKE_CURRENT_SOURCE_DIR} describe --always --dirty
  OUTPUT_VARIABLE FW_GIT_VERSION OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
if(NOT FW_GIT_VERSION)
  set(FW_GIT_VERSION "nogit")
endif()
target_compile_definitions(app PRIVATE FW_GIT_VERSION="${FW_GIT_VERSION}")
