/* SPDX-License-Identifier: Apache-2.0 */

#include <errno.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/shell/shell.h>
#include <zephyr/sys/barrier.h>
#include <zephyr/sys/util.h>

#define SHMEM_SHELL_ADDR 0x6e412000UL
#define SHMEM_SHELL_MAGIC 0x4c48535aU /* "ZSHL" in memory */
#define SHMEM_SHELL_VERSION 1U
#define SHMEM_SHELL_HEADER_SIZE 128U
#define SHMEM_SHELL_RX_SIZE 4096U
#define SHMEM_SHELL_TX_SIZE (32U * 1024U)

#define SHMEM_LOG_ADDR 0x6e401000UL
#define SHMEM_LOG_MAGIC 0x474f4c5aU

LOG_MODULE_REGISTER(vf2_amp_shell, LOG_LEVEL_INF);

struct shmem_shell {
	uint32_t magic;
	uint32_t version;
	uint32_t header_size;
	uint32_t rx_size;
	uint32_t tx_size;
	uint32_t boot_id;
	uint32_t hart_id;
	uint32_t reserved0;
	uint64_t rx_write_seq;
	uint64_t rx_write_seq_inverse;
	uint64_t rx_read_seq;
	uint64_t tx_write_seq;
	uint64_t tx_write_seq_inverse;
	uint8_t reserved1[56];
	uint8_t data[];
};

BUILD_ASSERT(offsetof(struct shmem_shell, data) == SHMEM_SHELL_HEADER_SIZE);

struct shmem_log_status {
	uint32_t magic;
	uint32_t version;
	uint32_t header_size;
	uint32_t data_size;
	uint32_t boot_id;
	uint32_t hart_id;
	uint64_t write_seq;
	uint64_t write_seq_inverse;
};

struct shmem_shell_context {
	shell_transport_handler_t handler;
	void *handler_context;
	struct k_timer poll_timer;
	uint64_t tx_write_seq;
	bool initialized;
};

static volatile struct shmem_shell *const shared_shell =
	(volatile struct shmem_shell *)SHMEM_SHELL_ADDR;
static volatile struct shmem_log_status *const shared_log =
	(volatile struct shmem_log_status *)SHMEM_LOG_ADDR;
static struct shmem_shell_context transport_context;

static unsigned long read_hart_id(void)
{
	unsigned long hart_id;

	__asm__ volatile ("csrr %0, mhartid" : "=r" (hart_id));
	return hart_id;
}

static volatile uint8_t *rx_data(void)
{
	return shared_shell->data;
}

static volatile uint8_t *tx_data(void)
{
	return shared_shell->data + SHMEM_SHELL_RX_SIZE;
}

static bool rx_sequence_get(uint64_t *sequence)
{
	uint64_t first = shared_shell->rx_write_seq;
	uint64_t inverse;
	uint64_t second;

	barrier_dmem_fence_full();
	inverse = shared_shell->rx_write_seq_inverse;
	barrier_dmem_fence_full();
	second = shared_shell->rx_write_seq;

	if (first != second || inverse != ~second) {
		return false;
	}

	*sequence = second;
	return true;
}

static void poll_timer_handler(struct k_timer *timer)
{
	struct shmem_shell_context *context = k_timer_user_data_get(timer);
	uint64_t rx_write_seq;

	if (context->handler != NULL && rx_sequence_get(&rx_write_seq) &&
	    rx_write_seq != shared_shell->rx_read_seq) {
		context->handler(SHELL_TRANSPORT_EVT_RX_RDY,
				 context->handler_context);
	}
}

static int transport_init(const struct shell_transport *transport,
			  const void *config,
			  shell_transport_handler_t handler,
			  void *handler_context)
{
	struct shmem_shell_context *context = transport->ctx;
	uint32_t boot_id = 1;

	ARG_UNUSED(config);

	if (context->initialized) {
		return -EALREADY;
	}

	if (shared_shell->magic == SHMEM_SHELL_MAGIC &&
	    shared_shell->version == SHMEM_SHELL_VERSION) {
		boot_id = shared_shell->boot_id + 1;
	}

	shared_shell->magic = 0;
	shared_shell->version = SHMEM_SHELL_VERSION;
	shared_shell->header_size = SHMEM_SHELL_HEADER_SIZE;
	shared_shell->rx_size = SHMEM_SHELL_RX_SIZE;
	shared_shell->tx_size = SHMEM_SHELL_TX_SIZE;
	shared_shell->boot_id = boot_id;
	shared_shell->hart_id = (uint32_t)read_hart_id();
	shared_shell->rx_write_seq = 0;
	shared_shell->rx_write_seq_inverse = UINT64_MAX;
	shared_shell->rx_read_seq = 0;
	shared_shell->tx_write_seq = 0;
	shared_shell->tx_write_seq_inverse = UINT64_MAX;
	context->tx_write_seq = 0;
	context->handler = handler;
	context->handler_context = handler_context;
	context->initialized = true;
	barrier_dmem_fence_full();
	shared_shell->magic = SHMEM_SHELL_MAGIC;
	barrier_dmem_fence_full();

	k_timer_init(&context->poll_timer, poll_timer_handler, NULL);
	k_timer_user_data_set(&context->poll_timer, context);
	k_timer_start(&context->poll_timer, K_MSEC(10), K_MSEC(10));

	return 0;
}

static int transport_uninit(const struct shell_transport *transport)
{
	struct shmem_shell_context *context = transport->ctx;

	k_timer_stop(&context->poll_timer);
	context->initialized = false;
	context->handler = NULL;
	return 0;
}

static int transport_enable(const struct shell_transport *transport,
			    bool blocking_tx)
{
	struct shmem_shell_context *context = transport->ctx;

	ARG_UNUSED(blocking_tx);
	return context->initialized ? 0 : -ENODEV;
}

static int transport_write(const struct shell_transport *transport,
			   const void *data, size_t length, size_t *count)
{
	struct shmem_shell_context *context = transport->ctx;
	const uint8_t *source = data;
	volatile uint8_t *destination = tx_data();

	if (!context->initialized) {
		*count = 0;
		return -ENODEV;
	}

	for (size_t i = 0; i < length; i++) {
		destination[context->tx_write_seq % SHMEM_SHELL_TX_SIZE] =
			source[i];
		context->tx_write_seq++;
	}

	shared_shell->tx_write_seq_inverse = ~context->tx_write_seq;
	barrier_dmem_fence_full();
	shared_shell->tx_write_seq = context->tx_write_seq;
	barrier_dmem_fence_full();
	*count = length;
	return 0;
}

static int transport_read(const struct shell_transport *transport,
			  void *data, size_t length, size_t *count)
{
	struct shmem_shell_context *context = transport->ctx;
	uint8_t *destination = data;
	uint64_t read_seq = shared_shell->rx_read_seq;
	uint64_t write_seq;
	size_t available;

	if (!context->initialized) {
		*count = 0;
		return -ENODEV;
	}

	if (!rx_sequence_get(&write_seq)) {
		*count = 0;
		return 0;
	}

	available = MIN((uint64_t)length, write_seq - read_seq);
	for (size_t i = 0; i < available; i++) {
		destination[i] = rx_data()[read_seq % SHMEM_SHELL_RX_SIZE];
		read_seq++;
	}

	barrier_dmem_fence_full();
	shared_shell->rx_read_seq = read_seq;
	barrier_dmem_fence_full();
	*count = available;
	return 0;
}

static const struct shell_transport_api shmem_shell_transport_api = {
	.init = transport_init,
	.uninit = transport_uninit,
	.enable = transport_enable,
	.write = transport_write,
	.read = transport_read,
};

static struct shell_transport shmem_shell_transport = {
	.api = &shmem_shell_transport_api,
	.ctx = &transport_context,
};

SHELL_DEFINE(shell_shmem, "zephyr:~$ ", &shmem_shell_transport, 0, 0,
	     SHELL_FLAG_OLF_CRLF);

static int shmem_shell_start(void)
{
	static const struct shell_backend_config_flags flags =
		SHELL_DEFAULT_BACKEND_CONFIG_FLAGS;

	return shell_init(&shell_shmem, NULL, flags, false, LOG_LEVEL_NONE);
}

SYS_INIT(shmem_shell_start, POST_KERNEL, CONFIG_KERNEL_INIT_PRIORITY_DEFAULT);

static int cmd_amp_status(const struct shell *shell, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	shell_print(shell, "hart: %u", shared_shell->hart_id);
	shell_print(shell, "shell boot: %u", shared_shell->boot_id);
	shell_print(shell, "shell rx: write=%llu read=%llu",
		    (unsigned long long)shared_shell->rx_write_seq,
		    (unsigned long long)shared_shell->rx_read_seq);
	shell_print(shell, "shell tx: write=%llu",
		    (unsigned long long)shared_shell->tx_write_seq);
	if (shared_log->magic == SHMEM_LOG_MAGIC) {
		shell_print(shell, "log boot: %u", shared_log->boot_id);
		shell_print(shell, "log bytes: %llu",
			    (unsigned long long)shared_log->write_seq);
	} else {
		shell_warn(shell, "shared log is not ready");
	}

	return 0;
}

static int cmd_amp_log_status(const struct shell *shell, size_t argc,
			      char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	if (shared_log->magic != SHMEM_LOG_MAGIC) {
		shell_error(shell, "shared log is not ready");
		return -ENODEV;
	}

	shell_print(shell, "boot: %u", shared_log->boot_id);
	shell_print(shell, "hart: %u", shared_log->hart_id);
	shell_print(shell, "buffer size: %u", shared_log->data_size);
	shell_print(shell, "bytes written: %llu",
		    (unsigned long long)shared_log->write_seq);
	shell_print(shell, "sequence valid: %s",
		    shared_log->write_seq_inverse == ~shared_log->write_seq ?
		    "yes" : "updating");
	return 0;
}

static int cmd_amp_log_test(const struct shell *shell, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	LOG_INF("test message requested from Zephyr shell");
	shell_print(shell, "test log sent; read it with vf2-zephyr-log.py");
	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(amp_log_commands,
	SHELL_CMD(status, NULL, "Show shared log ring status",
		  cmd_amp_log_status),
	SHELL_CMD(test, NULL, "Send a test message to the Linux log reader",
		  cmd_amp_log_test),
	SHELL_SUBCMD_SET_END);

SHELL_STATIC_SUBCMD_SET_CREATE(amp_commands,
	SHELL_CMD(log, &amp_log_commands, "Shared logging commands", NULL),
	SHELL_CMD(status, NULL, "Show AMP shared-memory status", cmd_amp_status),
	SHELL_SUBCMD_SET_END);
SHELL_CMD_REGISTER(amp, &amp_commands, "VisionFive 2 AMP commands", NULL);
