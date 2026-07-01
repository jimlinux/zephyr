#!/usr/bin/env python3
"""Interactive Zephyr shell over the VisionFive 2 AMP shared-memory link."""

import mmap
import os
import select
import struct
import sys
import termios
import time
import tty


SHELL_ADDR = 0x6E412000
SHELL_MAGIC = 0x4C48535A
SHELL_VERSION = 1
HEADER_SIZE = 128
RX_SIZE = 4096
TX_SIZE = 32 * 1024
MAP_SIZE = HEADER_SIZE + RX_SIZE + TX_SIZE
HEADER = struct.Struct("<8I5Q")
MASK64 = (1 << 64) - 1
EXIT_BYTE = b"\x1d"  # Ctrl-]


def read_header(memory):
    first = HEADER.unpack_from(memory, 0)
    second = HEADER.unpack_from(memory, 0)
    if first[8] != second[8] or first[11] != second[11]:
        return None

    (
        magic,
        version,
        header_size,
        rx_size,
        tx_size,
        boot_id,
        hart_id,
        _,
        rx_write,
        rx_inverse,
        rx_read,
        tx_write,
        tx_inverse,
    ) = second
    if (
        magic != SHELL_MAGIC
        or version != SHELL_VERSION
        or header_size != HEADER_SIZE
        or rx_size != RX_SIZE
        or tx_size != TX_SIZE
        or rx_inverse != ((~rx_write) & MASK64)
        or tx_inverse != ((~tx_write) & MASK64)
    ):
        return None

    return boot_id, hart_id, rx_write, rx_read, tx_write


def send_input(memory, pending, rx_write, rx_read):
    free = RX_SIZE - (rx_write - rx_read)
    length = min(len(pending), max(0, free))
    for index in range(length):
        memory[HEADER_SIZE + (rx_write % RX_SIZE)] = pending[index]
        rx_write += 1

    if length:
        struct.pack_into("<Q", memory, 40, (~rx_write) & MASK64)
        struct.pack_into("<Q", memory, 32, rx_write)
    return pending[length:], rx_write


def main():
    if not sys.stdin.isatty():
        print("stdin must be a terminal", file=sys.stderr)
        return 1

    fd = os.open("/dev/mem", os.O_RDWR | os.O_SYNC)
    try:
        memory = mmap.mmap(
            fd,
            MAP_SIZE,
            flags=mmap.MAP_SHARED,
            prot=mmap.PROT_READ | mmap.PROT_WRITE,
            offset=SHELL_ADDR,
        )
    finally:
        os.close(fd)

    old_terminal = termios.tcgetattr(sys.stdin.fileno())
    boot_id = None
    rx_write = 0
    tx_read = 0
    pending = bytearray()

    print("Waiting for Zephyr shared-memory shell...", file=sys.stderr)
    print("Press Ctrl-] to return to Linux.", file=sys.stderr)

    try:
        tty.setraw(sys.stdin.fileno())
        while True:
            header = read_header(memory)
            if header is None:
                time.sleep(0.05)
                continue

            current_boot, hart_id, shared_rx_write, rx_read, tx_write = header
            if boot_id != current_boot:
                boot_id = current_boot
                rx_write = shared_rx_write
                tx_read = max(0, tx_write - TX_SIZE)
                pending.clear()
                message = f"\r\n[connected: boot={boot_id} hart={hart_id}]\r\n"
                os.write(sys.stdout.fileno(), message.encode())

            if tx_write - tx_read > TX_SIZE:
                lost = tx_write - tx_read - TX_SIZE
                tx_read = tx_write - TX_SIZE
                message = f"\r\n[shell output lost {lost} bytes]\r\n"
                os.write(sys.stdout.fileno(), message.encode())

            while tx_read < tx_write:
                offset = tx_read % TX_SIZE
                length = min(tx_write - tx_read, TX_SIZE - offset)
                start = HEADER_SIZE + RX_SIZE + offset
                os.write(sys.stdout.fileno(), memory[start : start + length])
                tx_read += length

            ready, _, _ = select.select([sys.stdin], [], [], 0.02)
            if ready:
                data = os.read(sys.stdin.fileno(), 256)
                if EXIT_BYTE in data:
                    break
                pending.extend(data)

            pending, rx_write = send_input(
                memory, pending, rx_write, rx_read
            )
    finally:
        termios.tcsetattr(sys.stdin.fileno(), termios.TCSADRAIN, old_terminal)
        memory.close()
        print("\nReturned to Linux.")

    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except PermissionError:
        print("permission denied: run this program as root", file=sys.stderr)
        raise SystemExit(1)
    except OSError as error:
        print(f"vf2-zephyr-shell: {error}", file=sys.stderr)
        raise SystemExit(1)
