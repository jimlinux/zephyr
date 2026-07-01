#!/usr/bin/env python3
"""Stream Zephyr logs from the VisionFive 2 AMP shared-memory ring."""

import mmap
import os
import struct
import sys
import time


SHMEM_LOG_ADDR = 0x6E401000
SHMEM_LOG_MAGIC = 0x474F4C5A
SHMEM_LOG_VERSION = 1
HEADER_SIZE = 64
MAX_DATA_SIZE = 64 * 1024
MAP_SIZE = HEADER_SIZE + MAX_DATA_SIZE
HEADER = struct.Struct("<6I2Q")
MASK64 = (1 << 64) - 1


def read_header(memory):
    first = HEADER.unpack_from(memory, 0)
    second = HEADER.unpack_from(memory, 0)
    if first[6] != second[6]:
        return None

    magic, version, header_size, data_size, boot_id, hart_id, write_seq, inverse = second
    if inverse != ((~write_seq) & MASK64):
        return None
    if (
        magic != SHMEM_LOG_MAGIC
        or version != SHMEM_LOG_VERSION
        or header_size != HEADER_SIZE
        or not 0 < data_size <= MAX_DATA_SIZE
    ):
        return None

    return data_size, boot_id, hart_id, write_seq


def main():
    fd = os.open("/dev/mem", os.O_RDONLY | os.O_SYNC)
    try:
        memory = mmap.mmap(
            fd,
            MAP_SIZE,
            flags=mmap.MAP_SHARED,
            prot=mmap.PROT_READ,
            offset=SHMEM_LOG_ADDR,
        )
    finally:
        os.close(fd)

    boot_id = None
    read_seq = 0

    try:
        while True:
            header = read_header(memory)
            if header is None:
                time.sleep(0.1)
                continue

            data_size, current_boot_id, hart_id, write_seq = header
            if boot_id != current_boot_id:
                boot_id = current_boot_id
                read_seq = max(0, write_seq - data_size)
                print(
                    f"[vf2-zephyr-log] boot={boot_id} hart={hart_id}",
                    file=sys.stderr,
                    flush=True,
                )

            available = write_seq - read_seq
            if available > data_size:
                lost = available - data_size
                print(
                    f"\n[vf2-zephyr-log] lost {lost} bytes",
                    file=sys.stderr,
                    flush=True,
                )
                read_seq = write_seq - data_size

            while read_seq < write_seq:
                offset = read_seq % data_size
                length = min(write_seq - read_seq, data_size - offset)
                chunk = memory[HEADER_SIZE + offset : HEADER_SIZE + offset + length]
                sys.stdout.buffer.write(chunk)
                read_seq += length

            sys.stdout.buffer.flush()
            time.sleep(0.02)
    except KeyboardInterrupt:
        return 0
    finally:
        memory.close()


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except PermissionError:
        print("permission denied: run this program as root", file=sys.stderr)
        raise SystemExit(1)
    except OSError as error:
        print(f"vf2-zephyr-log: {error}", file=sys.stderr)
        raise SystemExit(1)
