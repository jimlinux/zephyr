# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(visionfive2_amp_heartbeat)

target_sources(app PRIVATE
  src/log_backend_shmem.c
  src/main.c
  src/shell_backend_shmem.c
)
