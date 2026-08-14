cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(t2i_fw)

target_sources(app PRIVATE src/main.c src/reset_hook.c src/hx8347_fsmc.c src/touch.c)
