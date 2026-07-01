/* SPDX-License-Identifier: Apache-2.0 */

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define SHMEM_LOG_ADDR 0x6e401000UL
#define SHMEM_LOG_MAGIC 0x474f4c5aU
#define SHMEM_LOG_VERSION 1U
#define SHMEM_LOG_MAP_SIZE (64U + 64U * 1024U)

struct shmem_log {
	uint32_t magic;
	uint32_t version;
	uint32_t header_size;
	uint32_t data_size;
	uint32_t boot_id;
	uint32_t hart_id;
	uint64_t write_seq;
	uint64_t write_seq_inverse;
	uint8_t reserved[24];
	uint8_t data[];
};

static volatile sig_atomic_t running = 1;

static void stop(int signal_number)
{
	(void)signal_number;
	running = 0;
}

static bool read_write_seq(const volatile struct shmem_log *log,
			   uint64_t *sequence)
{
	uint64_t first;
	uint64_t inverse;
	uint64_t second;

	first = log->write_seq;
	__atomic_thread_fence(__ATOMIC_ACQUIRE);
	inverse = log->write_seq_inverse;
	__atomic_thread_fence(__ATOMIC_ACQUIRE);
	second = log->write_seq;

	if (first != second || inverse != ~second) {
		return false;
	}

	*sequence = second;
	return true;
}

int main(void)
{
	volatile struct shmem_log *log;
	uint32_t boot_id = 0;
	uint64_t read_seq = 0;
	int fd;

	signal(SIGINT, stop);
	signal(SIGTERM, stop);

	fd = open("/dev/mem", O_RDONLY | O_SYNC);
	if (fd < 0) {
		fprintf(stderr, "open /dev/mem: %s\n", strerror(errno));
		return 1;
	}

	log = mmap(NULL, SHMEM_LOG_MAP_SIZE, PROT_READ, MAP_SHARED, fd,
		   SHMEM_LOG_ADDR);
	close(fd);
	if (log == MAP_FAILED) {
		fprintf(stderr, "mmap: %s\n", strerror(errno));
		return 1;
	}

	while (running) {
		uint64_t write_seq;

		if (log->magic != SHMEM_LOG_MAGIC ||
		    log->version != SHMEM_LOG_VERSION ||
		    log->header_size != 64 || log->data_size == 0 ||
		    log->data_size > 64U * 1024U) {
			usleep(100000);
			continue;
		}

		if (!read_write_seq(log, &write_seq)) {
			continue;
		}

		if (boot_id != log->boot_id) {
			boot_id = log->boot_id;
			read_seq = write_seq > log->data_size ?
				   write_seq - log->data_size : 0;
			fprintf(stderr, "[vf2-zephyr-log] boot=%u hart=%u\n",
				boot_id, log->hart_id);
		}

		if (write_seq - read_seq > log->data_size) {
			uint64_t lost = write_seq - read_seq - log->data_size;

			fprintf(stderr,
				"\n[vf2-zephyr-log] lost %" PRIu64 " bytes\n",
				lost);
			read_seq = write_seq - log->data_size;
		}

		while (read_seq < write_seq) {
			uint64_t offset = read_seq % log->data_size;
			size_t length = log->data_size - offset;

			if (length > write_seq - read_seq) {
				length = write_seq - read_seq;
			}
			fwrite((const void *)&log->data[offset], 1, length, stdout);
			read_seq += length;
		}
		fflush(stdout);
		usleep(20000);
	}

	munmap((void *)log, SHMEM_LOG_MAP_SIZE);
	return 0;
}
