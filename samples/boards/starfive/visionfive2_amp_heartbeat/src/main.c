/* SPDX-License-Identifier: Apache-2.0 */

#include <stdint.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/barrier.h>
#include <zephyr/toolchain.h>

LOG_MODULE_REGISTER(vf2_amp, LOG_LEVEL_DBG);

#define AMP_HEARTBEAT_ADDR 0x6e400000UL
#define AMP_HEARTBEAT_MAGIC 0x53374842U /* "S7HB" */

struct amp_heartbeat {
	uint32_t magic;
	uint32_t sequence;
	uint32_t hart_id;
	uint32_t sequence_inverse;
};

static volatile struct amp_heartbeat *const heartbeat =
	(volatile struct amp_heartbeat *)AMP_HEARTBEAT_ADDR;

static unsigned long read_hart_id(void)
{
	unsigned long hart_id;

	__asm__ volatile ("csrr %0, mhartid" : "=r" (hart_id));
	return hart_id;
}

int main(void)
{
	uint32_t sequence = 0;

	heartbeat->magic = AMP_HEARTBEAT_MAGIC;
	heartbeat->hart_id = (uint32_t)read_hart_id();
	LOG_INF("Zephyr started on S7 hart %u", heartbeat->hart_id);
	LOG_INF("shared-memory logging is ready");

	while (true) {
		sequence++;
		heartbeat->sequence_inverse = ~sequence;
		barrier_dmem_fence_full();
		heartbeat->sequence = sequence;
		barrier_dmem_fence_full();
		LOG_INF("heartbeat %u", sequence);
		k_sleep(K_SECONDS(1));
	}

	return 0;
}
