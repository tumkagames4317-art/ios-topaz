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

/* SPMI PMIC arbiter reset (the "PS_HOLD" style reset used by ABL itself). */
#define SPMI_RESET	(SPMI_ARB_BASE + 0x1000U)

#define REG32(a)	(*(volatile uint32_t *)(a))

static int pmic_ok;

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
	__asm__ volatile("hvc #0" :: "r"(x0) : "memory");

	/* Fallback if PSCI is not there: arbiter reset (best effort). */
	REG32(SPMI_RESET) = 1;
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
	__asm__ volatile("hvc #0" :: "r"(x0) : "memory");

	/* Fallback: arbiter reset, which is what ABL itself uses. */
	REG32(SPMI_RESET) = 1;
	for (;;)
		__asm__ volatile("wfi");
}
