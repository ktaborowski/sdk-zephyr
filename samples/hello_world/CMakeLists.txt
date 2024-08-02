# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(hello_world)

target_sources(app PRIVATE src/main.c)

message("ENV SYS_VAR_CUSTOM_PATH: $ENV{SYS_VAR_CUSTOM_PATH}")
message("Kconfig CONFIG_CUSTOM_PATH: ${CONFIG_CUSTOM_PATH}")
