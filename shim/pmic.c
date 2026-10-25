/*
 * PMIC buttons and reset, for the panic park loop.
 *
 * The register recipe is the one that already works on this platform
 * (NOTES.md, patch 0060: "SPMI arbiter v5 observer reads, PON 0x810 bits
 * KPDPWR/RESIN SID0"):
 *
 *   SPMI arbiter v5 @ 0x1c40000
 *     +0x800  => per-slave "observer" window base for SID (slave) 0
 *     +0x810  => PON peripheral, KPDPWR_N / RESIN_N status bits
 *
 * My first attempt used 0x0c000000, which is not the ginkgo arbiter; that
 * address is unmapped, so the keys must be read through the arbiter window.
 *
 * Why this matters: it lets a hung shim leave the park loop by itself, so the
 * whole debug cycle runs without the user pressing anything.
 */
#include "pmic.h"
#include "console.h"
#include "ufslog.h"

/* SPMI arbiter v5: the "observer" registers mirror a slave's registers. */
#define SPMI_ARB_BASE	0x01c40000UL
#define SPMI_OBS_BASE	0x800U		/* window for SID 0 (PM6125) */
#define SPMI_OBS_OFF	0x010U		/* PON status inside the window */

#define PON_RT_STS	(SPMI_ARB_BASE + SPMI_OBS_BASE + SPMI_OBS_OFF)

/* PON_RT_STS bits (PM6125 PON block, same layout as PM6150). */
#define PON_KPDPWR_N	(1u << 0)	/* Power key */
#define PON_RESIN_N	(1u << 1)	/* Volume-Down */
#define PON_BOTH	(PON_KPDPWR_N | PON_RESIN_N)

/*
 * APCS watchdog ("qcom,kpss-wdt", DTB base qcom,wdt@f017000): the last-resort
 * reset path on every Qualcomm SoC. The register offsets are the kpss layout
 * from the upstream qcom-wdt driver, not a linear block starting at 0: EN is at
 * 0x08 and the timeouts at 0x10/0x14, with RST and STS between them. Writing
 * "0" to the base is therefore a *ping*, not a disable -- which is why earlier
 * attempts to arm the watchdog here silently did nothing.
 */
#define APCS_WDT0_BASE		0x0f017000UL
#define APCS_WDT0_RST		(APCS_WDT0_BASE + 0x04)
#define APCS_WDT0_EN		(APCS_WDT0_BASE + 0x08)
#define APCS_WDT0_STS		(APCS_WDT0_BASE + 0x0c)
#define APCS_WDT0_BARK_TIME	(APCS_WDT0_BASE + 0x10)
#define APCS_WDT0_BITE_TIME	(APCS_WDT0_BASE + 0x14)

#define REG32(a)	(*(volatile uint32_t *)(a))

static int pmic_ok;

/*
 * Arm the APCS watchdog. The bark interrupts but does not reset, so it is only
 * useful as a warning; the bite is what reboots the board. Both are written
 * before enabling so the counters start from a known value.
 */
void watchdog_arm(uint32_t bark_ticks, uint32_t bite_ticks)
{
	REG32(APCS_WDT0_EN) = 0;
	REG32(APCS_WDT0_RST) = 1;		/* restart the counter */
	REG32(APCS_WDT0_BARK_TIME) = bark_ticks;
	REG32(APCS_WDT0_BITE_TIME) = bite_ticks;
	__asm__ volatile("dsb sy" ::: "memory");
	REG32(APCS_WDT0_EN) = 1;		/* QCOM_WDT_ENABLE = BIT(0) */
	__asm__ volatile("dsb sy" ::: "memory");
}

void watchdog_disarm(void)
{
	REG32(APCS_WDT0_EN) = 0;
	__asm__ volatile("dsb sy" ::: "memory");
}

/*
 * Arm the watchdog briefly and report whether the writes latch.
 *
 * loader.c calls this immediately before handing over to xnu, to answer one
 * question on the panel: can this block reset the board at all? The answer
 * measured on ginkgo is no -- the block is gated after ABL and the EN bit does
 * not stick -- but the check stays because it is cheap and because "the
 * watchdog does not work here" has to stay an observation rather than a
 * remembered guess.
 *
 * Return values:
 *   1  = EN read back set and BITE_TIME took the value: the block is live
 *   0  = the block decoded but EN did not stick: gated, no reset from here
 *  -1  = the block does not decode at all (reads all ones)
 *
 * The caller only prints the result, it does not branch on it.
 */
int wdt_selftest(void)
{
	uint32_t en_before, bite_before, en_after, bite_after;

	en_before = REG32(APCS_WDT0_EN);
	bite_before = REG32(APCS_WDT0_BITE_TIME);

	/* A block that does not decode reads back all ones on most Qualcomm SoCs. */
	if (en_before == 0xffffffffu)
		return -1;

	/*
	 * Program a long countdown rather than a short one: this runs right
	 * before the handoff, and a bite that fires during xnu's own start-up
	 * would be indistinguishable from an xnu crash. 0x7ffffff seconds worth
	 * of ticks is effectively "never".
	 */
	watchdog_arm(0x3ffffffu, 0x7ffffffu);

	en_after = REG32(APCS_WDT0_EN);
	bite_after = REG32(APCS_WDT0_BITE_TIME);

	watchdog_disarm();

	if ((en_before & 1u) == 0u && (en_after & 1u) == 0u)
		return 0;	/* the enable bit never took */
	if (bite_after != 0x7ffffffu)
		return 0;	/* the counter register is not writable either */
	return (en_after & 1u) ? 1 : 0;
}

static void spin_ms(uint32_t ms)
{
	uint64_t f, start, now;
	__asm__ volatile("mrs %0, cntfrq_el0" : "=r"(f));
	__asm__ volatile("mrs %0, cntpct_el0" : "=r"(start));
	uint64_t want = (f / 1000u) * ms;
	do {
		__asm__ volatile("mrs %0, cntpct_el0" : "=r"(now));
		if (now - start >= want)
			break;
		__asm__ volatile("wfi");
	} while (1);
}

static uint32_t keys(void)
{
	return REG32(PON_RT_STS);
}

int pmic_init(void)
{
	/*
	 * A live PON status register reads a small bitmap. All-ones means we are
	 * reading unmapped space (the arbiter base was wrong), which we must know
	 * before parking forever waiting for a key that can never arrive.
	 */
	uint32_t st = keys();
	pmic_ok = st != 0xffffffffu;
	con_hex("PON", st);
	ufslog_hex("PON-status", st);
	return pmic_ok;
}

uint64_t pmic_wait_key(void)
{
	if (!pmic_ok)
		return PMIC_KEY_NONE;

	for (int i = 0; i < 600; i++) {		/* ~60 s, then report and retry */
		uint32_t st = keys();

		if ((st & PON_BOTH) == PON_BOTH)
			continue;		/* let the PMIC handle a two-key reset */

		if (st & PON_RESIN_N) {
			spin_ms(60);		/* debounce */
			if (keys() & PON_RESIN_N)
				return PMIC_KEY_VOL_DOWN;
		}
		if (st & PON_KPDPWR_N) {
			spin_ms(60);
			if (keys() & PON_KPDPWR_N)
				return PMIC_KEY_POWER;
		}
		spin_ms(100);
		if (i % 100 == 99)
			ufslog_str("HALT", "still parked, waiting for a key");
	}
	return PMIC_KEY_NONE;
}

/*
 * Leave the park without any user input.
 *
 * Reading the PMIC keys turned out not to work this early: the recipe in
 * NOTES.md (SPMI arbiter @ 0x1c40000, PON +0x810) is proven inside xnu, where
 * clocks and the RPM are up. Before that, with the MMU off, the arbiter does
 * not answer, so a key press never reaches us.
 *
 * What does work is the other direction: tell ABL where to go, then reset. ABL
 * reads the IMEM restart reason on start, so this lands in fastboot by itself.
 * That removes the user from the loop entirely -- flash, boot, read the UFS
 * log, repeat.
 */
void reboot_now(uint64_t imem_reason)
{
	REG32(0x0c12565cUL) = (uint32_t)imem_reason;
	__asm__ volatile("dsb sy" ::: "memory");

	register uint64_t x0 __asm__("x0") = 0x84000009u;	/* PSCI SYSTEM_RESET */
	__asm__ volatile("smc #0" :: "r"(x0) : "memory");

	/*
	 * PSCI did not take. Fall back to a watchdog bite, which every Qualcomm
	 * SoC resets on: APCS_WDT0 lives at 0x0f017000 and a zeroed enable
	 * register with a short bark/bite makes the PMIC power-cycle the board.
	 * If even that fails, spin -- a reset is never required for correctness,
	 * only for convenience, and hanging is safer than guessing more writes.
	 */
	REG32(APCS_WDT0_EN) = 0;
	REG32(APCS_WDT0_BARK_TIME) = 0x1000;
	REG32(APCS_WDT0_BITE_TIME) = 0x2000;
	REG32(APCS_WDT0_EN) = 1;
	for (;;)
		__asm__ volatile("wfi");
}

void reboot_to_fastboot(void)
{
	/*
	 * Ask ABL for fastboot through IMEM, which is how Qualcomm bootloaders
	 * have always been told where to go next. From the stock ginkgo DTB:
	 *
	 *   qcom,msm-imem@c125000  reg = <0x0c125000 0x1000>
	 *   imem-restart_reason@65c   -> 0x0c12565c
	 *
	 * 0x77665501 is the "bootloader / fastboot" reason. ABL reads it on the
	 * next start, so this is the documented path rather than a guess: writing
	 * it and resetting is exactly what `adb reboot bootloader` ends up doing.
	 */
#define IMEM_RESTART_REASON	0x0c12565cUL
#define RESTART_FASTBOOT	0x77665501u
	REG32(IMEM_RESTART_REASON) = RESTART_FASTBOOT;
	__asm__ volatile("dsb sy" ::: "memory");

	/* PSCI SYSTEM_RESET first: the standard path, keeps ABL happy. */
	register uint64_t x0 __asm__("x0") = 0x84000009u;
	__asm__ volatile("smc #0" :: "r"(x0) : "memory");

	/* Fallback: watchdog bite, as in reboot_now(). */
	REG32(APCS_WDT0_EN) = 0;
	REG32(APCS_WDT0_BARK_TIME) = 0x1000;
	REG32(APCS_WDT0_BITE_TIME) = 0x2000;
	REG32(APCS_WDT0_EN) = 1;
	for (;;)
		__asm__ volatile("wfi");
}
