/* SPDX-License-Identifier: Apache-2.0 */

#include <stdint.h>

#include <zephyr/logging/log_backend.h>
#include <zephyr/logging/log_output.h>
#include <zephyr/sys/barrier.h>
#include <zephyr/sys/util.h>

#define SHMEM_LOG_ADDR 0x6e401000UL
#define SHMEM_LOG_MAGIC 0x474f4c5aU /* "ZLOG" in memory */
#define SHMEM_LOG_VERSION 1U
#define SHMEM_LOG_DATA_SIZE (64U * 1024U)
#define SHMEM_LOG_OUTPUT_SIZE 128U

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
	uint8_t data[SHMEM_LOG_DATA_SIZE];
};

BUILD_ASSERT(offsetof(struct shmem_log, data) == 64);

static volatile struct shmem_log *const shared_log =
	(volatile struct shmem_log *)SHMEM_LOG_ADDR;
static uint8_t output_buffer[SHMEM_LOG_OUTPUT_SIZE];
static uint64_t write_seq;

static unsigned long read_hart_id(void)
{
	unsigned long hart_id;

	__asm__ volatile ("csrr %0, mhartid" : "=r" (hart_id));
	return hart_id;
}

static void publish_write_seq(void)
{
	shared_log->write_seq_inverse = ~write_seq;
	barrier_dmem_fence_full();
	shared_log->write_seq = write_seq;
	barrier_dmem_fence_full();
}

static int write_output(uint8_t *data, size_t length, void *ctx)
{
	ARG_UNUSED(ctx);

	for (size_t i = 0; i < length; i++) {
		shared_log->data[write_seq % SHMEM_LOG_DATA_SIZE] = data[i];
		write_seq++;
	}

	publish_write_seq();
	return length;
}

LOG_OUTPUT_DEFINE(shmem_log_output, write_output, output_buffer,
		  sizeof(output_buffer));

static void process(const struct log_backend *const backend,
		    union log_msg_generic *msg)
{
	ARG_UNUSED(backend);

	log_output_msg_process(&shmem_log_output, &msg->log,
			       LOG_OUTPUT_FLAG_LEVEL |
			       LOG_OUTPUT_FLAG_TIMESTAMP |
			       LOG_OUTPUT_FLAG_FORMAT_TIMESTAMP |
			       LOG_OUTPUT_FLAG_CRLF_LFONLY);
}

static void init(const struct log_backend *const backend)
{
	uint32_t boot_id = 1;

	ARG_UNUSED(backend);

	if (shared_log->magic == SHMEM_LOG_MAGIC &&
	    shared_log->version == SHMEM_LOG_VERSION) {
		boot_id = shared_log->boot_id + 1;
	}

	shared_log->magic = 0;
	shared_log->version = SHMEM_LOG_VERSION;
	shared_log->header_size = offsetof(struct shmem_log, data);
	shared_log->data_size = SHMEM_LOG_DATA_SIZE;
	shared_log->boot_id = boot_id;
	shared_log->hart_id = (uint32_t)read_hart_id();
	write_seq = 0;
	publish_write_seq();
	barrier_dmem_fence_full();
	shared_log->magic = SHMEM_LOG_MAGIC;
	barrier_dmem_fence_full();
}

static const struct log_backend_api shmem_log_backend_api = {
	.process = process,
	.init = init,
};

LOG_BACKEND_DEFINE(shmem_log_backend, shmem_log_backend_api, true);
