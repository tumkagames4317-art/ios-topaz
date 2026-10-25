/*
 * eMMC block I/O for the ginkgo shim, behind the same interface ufslog uses.
 *
 * ---- Why this file exists -----------------------------------------------
 *
 * The shim was written for topaz/sweet, which boot from UFS, and ufslog.c logs
 * through ufs.c. ginkgo (Redmi Note 8, SM6125) has **no UFS at all**: it boots
 * from eMMC behind a Qualcomm SDHCI-v5 controller. Verified on the running
 * device: `/sys/block` has only `mmcblk0` (Samsung S0J9K8, type MMC), the DT
 * node is `sdhci@4744000` with compatible `qcom,sdhci-msm-v5`, and
 * `androidboot.bootdevice=4744000.sdhci`.
 *
 * So on this phone the log region in userdata was never written by the shim:
 * UFS never came up because the block does not exist. That is why every read of
 * the log returned only the hand-written TESTMARK, and why every failure in
 * this boot had to be diagnosed by photographing a half-second screen.
 *
 * This is the minimum needed to fix that: identify the card, put it in
 * transfer state at 400 kHz, and do single-block PIO reads and writes. No DMA,
 * no high-speed timing, no tuning -- those buy speed the log does not need and
 * add ways to fail.
 *
 * ---- Hardware facts used here -------------------------------------------
 *
 * Controller: SDHCI standard registers in the low 0x100 bytes of 0x04744000.
 * Its own clocks are configured by ABL, which read boot.img from this very
 * card a moment ago, so the controller is already clocked and powered.
 *
 * All MMIO here happens with the MMU off (every access is Device memory), so
 * there is no cache to maintain and DMA would be coherent anyway.
 */
#include <stdint.h>
#include "console.h"
#include "ufs.h"

#define SDHCI_BASE	0x04744000UL

/* SDHCI standard registers (SD Host Controller Simplified Spec 3.0). */
#define SDHCI_DMA_ADDRESS	0x00
#define SDHCI_BLOCK_SIZE	0x04
#define SDHCI_BLOCK_COUNT	0x06
#define SDHCI_ARGUMENT		0x08
#define SDHCI_TRANSFER_MODE	0x0c
#define SDHCI_COMMAND		0x0e
#define SDHCI_RESPONSE		0x10	/* 16 bytes */
#define SDHCI_BUFFER		0x20
#define SDHCI_PRESENT_STATE	0x24
#define SDHCI_HOST_CONTROL	0x28
#define SDHCI_POWER_CONTROL	0x29
#define SDHCI_CLOCK_CONTROL	0x2c
#define SDHCI_TIMEOUT_CONTROL	0x2e
#define SDHCI_SOFTWARE_RESET	0x2f
#define SDHCI_NORMAL_INT_STATUS	0x30
#define SDHCI_ERROR_INT_STATUS	0x32
#define SDHCI_NORMAL_INT_ENABLE	0x34
#define SDHCI_ERROR_INT_ENABLE	0x36
#define SDHCI_NORMAL_INT_SIGNAL	0x38
#define SDHCI_ERROR_INT_SIGNAL	0x3a
#define SDHCI_CAPABILITIES	0x40
#define SDHCI_HOST_VERSION	0xfe

/* PRESENT_STATE */
#define PS_CMD_INHIBIT		(1u << 0)
#define PS_DAT_INHIBIT		(1u << 1)
#define PS_DAT_ACTIVE		(1u << 2)
#define PS_WRITE_PROTECT	(1u << 19)

/* CLOCK_CONTROL */
#define CLK_INTERNAL_EN		(1u << 0)
#define CLK_SD_EN		(1u << 2)
#define CLK_SDCLK_DIV_MASK	(0xffu << 8)
#define CLK_SDCLK_DIV_SHIFT	8

/* HOST_CONTROL */
#define HC_LED_CTRL		(1u << 0)
#define HC_HIGH_SPEED		(1u << 2)
#define HC_8BIT_BUS		(1u << 5)

/* POWER_CONTROL */
#define PC_BUS_POWER		(1u << 0)
#define PC_BUS_VOLT_3_3		(7u << 1)
#define PC_BUS_VOLT_1_8		(0xau << 1)
#define PC_BUS_VOLT_3_0		(6u << 1)

/* NORMAL_INT_STATUS */
#define INT_CMD_COMPLETE	(1u << 0)
#define INT_TRANSFER_COMPLETE	(1u << 1)
#define INT_BUF_WRITE_READY	(1u << 4)
#define INT_BUF_READ_READY	(1u << 5)
#define INT_ERRORS		(1u << 15)

/* ERROR_INT_STATUS */
#define EINT_CMD_TIMEOUT	(1u << 0)
#define EINT_CMD_CRC		(1u << 1)
#define EINT_CMD_END_BIT	(1u << 2)
#define EINT_CMD_INDEX		(1u << 3)
#define EINT_DATA_TIMEOUT	(1u << 4)
#define EINT_DATA_CRC		(1u << 5)
#define EINT_DATA_END_BIT	(1u << 6)
#define EINT_CURRENT_LIMIT	(1u << 7)

/* TRANSFER_MODE */
#define TM_BLOCK_COUNT_EN	(1u << 1)
#define TM_READ			(1u << 4)
#define TM_MULTI_BLOCK		(1u << 5)

/* Software reset bits */
#define SRES_ALL		0x01
#define SRES_CMD		0x02
#define SRES_DAT		0x04

#define BLOCK_SIZE		512u
#define SDHCI_TIMEOUT_MS	2000u

static inline uint32_t rd32(uint32_t off) { return *(volatile uint32_t *)(SDHCI_BASE + off); }
static inline void wr32(uint32_t off, uint32_t v) { *(volatile uint32_t *)(SDHCI_BASE + off) = v; }
static inline uint16_t rd16(uint32_t off) { return *(volatile uint16_t *)(SDHCI_BASE + off); }
static inline void wr16(uint32_t off, uint16_t v) { *(volatile uint16_t *)(SDHCI_BASE + off) = v; }
static inline uint8_t rd8(uint32_t off) { return *(volatile uint8_t *)(SDHCI_BASE + off); }
static inline void wr8(uint32_t off, uint8_t v) { *(volatile uint8_t *)(SDHCI_BASE + off) = v; }

static int emmc_ready;

int emmc_ok(void) { return emmc_ready; }

/* ---- timing ------------------------------------------------------------- */

static uint64_t cntfrq(void)
{
	uint64_t v;
	__asm__ volatile("mrs %0, cntfrq_el0" : "=r"(v));
	return v;
}

static uint64_t cntpct(void)
{
	uint64_t v;
	__asm__ volatile("isb; mrs %0, cntpct_el0" : "=r"(v));
	return v;
}

static void spin_us(uint32_t us)
{
	uint64_t end = cntpct() + (cntfrq() / 1000000) * us;
	while (cntpct() < end)
		;
}

static void spin_ms(uint32_t ms)
{
	spin_us(ms * 1000);
}

/* ---- low-level ---------------------------------------------------------- */

/*
 * Wait until `mask` is set in the 16-bit register at `off`, within `ms`.
 * Returns the final register value > 0 on success, 0 on timeout.
 *
 * Deliberately no interrupt use: the shim runs with interrupts masked on this
 * core, so everything is polled.
 */
static uint16_t wait_bits(uint32_t off, uint16_t mask, uint32_t ms)
{
	uint64_t deadline = cntpct() + (cntfrq() / 1000) * ms;

	for (;;) {
		uint16_t v = rd16(off);
		if (v & mask)
			return v;
		if (cntpct() > deadline)
			return 0;
	}
}

static int clear_reset(void)
{
	/* Reset is self-clearing; a stuck bit means the block is not clocked. */
	uint64_t deadline = cntpct() + (cntfrq() / 1000) * SDHCI_TIMEOUT_MS;

	wr8(SDHCI_SOFTWARE_RESET, SRES_ALL);
	while (rd8(SDHCI_SOFTWARE_RESET) & SRES_ALL) {
		if (cntpct() > deadline)
			return 0;
	}
	return 1;
}

/*
 * The internal clock must run before anything else: the reset above hangs
 * without it, which is the usual way an SDHCI bring-up fails silently.
 *
 * BASE_CLK_FREQ (CAMERABILITIES[15:8]) cannot be trusted on this IP: the
 * Qualcomm driver sets SDHCI_QUIRK_CAP_CLOCK_BASE_BROKEN because the field does
 * not describe the real input clock, and it takes the rate from the clock
 * framework instead. With the field wrong, the divider below is wrong and the
 * card never answers -- a failure that looks exactly like "no card".
 *
 * So the divider is not derived from it. Identification runs at 400 kHz, and
 * the base clock here is at most 200 MHz, where a divider of 255 gives ~784 kHz
 * and of 128 gives ~1.5 MHz. Both are slow enough for eMMC identification, and
 * being a little slow costs nothing -- the log is a few dozen blocks. Pick 255
 * (the slowest) so the divisor is right for any plausible input clock.
 */
static int clock_on(uint32_t khz)
{
	uint32_t caps = rd32(SDHCI_CAPABILITIES);
	uint32_t reported = (caps >> 8) & 0xff;		/* BASE_CLK_FREQ, MHz */

	(void)khz;
	uint32_t div = 255;	/* slowest available: safe for identification */

	uint16_t ctrl = rd16(SDHCI_CLOCK_CONTROL) & ~CLK_SDCLK_DIV_MASK;
	ctrl |= (uint16_t)(div << CLK_SDCLK_DIV_SHIFT);
	ctrl |= CLK_INTERNAL_EN;
	wr16(SDHCI_CLOCK_CONTROL, ctrl);

	if (!wait_bits(SDHCI_CLOCK_CONTROL, CLK_INTERNAL_EN, SDHCI_TIMEOUT_MS))
		return 0;

	/* Report what the CAP register claimed, so a wrong value is visible. */
	con_hex2("EMMC", "capbase_mhz=", reported, "div=", div);
	return 1;
}

static void clock_card(int on)
{
	uint16_t ctrl = rd16(SDHCI_CLOCK_CONTROL);
	if (on)
		ctrl |= CLK_SD_EN;
	else
		ctrl &= (uint16_t)~CLK_SD_EN;
	wr16(SDHCI_CLOCK_CONTROL, ctrl);
}

/*
 * Wait until the CMD line is free. PRESENT_STATE reports "busy" as a set bit,
 * so this waits for the bit to go *clear* -- wait_bits() cannot express that,
 * which is why the earlier version of this function was muddled.
 */
static int wait_cmd_free(void)
{
	uint64_t deadline = cntpct() + (cntfrq() / 1000) * SDHCI_TIMEOUT_MS;

	while (rd16(SDHCI_PRESENT_STATE) & PS_CMD_INHIBIT) {
		if (cntpct() > deadline)
			return 0;
	}
	return 1;
}

/* Same for the DAT line. */
static int wait_dat_free(void)
{
	uint64_t deadline = cntpct() + (cntfrq() / 1000) * SDHCI_TIMEOUT_MS;

	while (rd16(SDHCI_PRESENT_STATE) & PS_DAT_INHIBIT) {
		if (cntpct() > deadline)
			return 0;
	}
	return 1;
}

/*
 * Issue one command. Returns 1 if the command completed with no error bit;
 * `resp` (when non-null) receives the 16-byte response block.
 *
 * resp_type: 0 = none, 1 = R1/R5/R6/R7 (48-bit), 2 = R2 (136-bit).
 */
static int cmd(uint32_t idx, uint32_t arg, uint32_t resp_type, uint32_t *resp)
{
	/* CMD index in [13:8]; RESP_TYPE is bits [1:0]; CRC check is bit 3. */
	uint16_t cmdreg = (uint16_t)(idx << 8);

	if (resp_type == 1)
		cmdreg |= 0x0001u;		/* R1/R5/R6/R7, 48-bit */
	else if (resp_type == 2)
		cmdreg |= 0x0002u;		/* R2, 136-bit */

	if (resp_type != 0)
		cmdreg |= 0x0008u;		/* CRC check enable */

	if (!wait_cmd_free())
		return 0;

	wr16(SDHCI_NORMAL_INT_STATUS, 0xffff);
	wr16(SDHCI_ERROR_INT_STATUS, 0xffff);
	wr32(SDHCI_ARGUMENT, arg);
	wr16(SDHCI_COMMAND, cmdreg);

	if (!wait_bits(SDHCI_NORMAL_INT_STATUS, INT_CMD_COMPLETE, SDHCI_TIMEOUT_MS))
		return 0;

	uint16_t err = rd16(SDHCI_ERROR_INT_STATUS);
	if (err & (EINT_CMD_TIMEOUT | EINT_CMD_CRC | EINT_CMD_END_BIT |
		   EINT_CMD_INDEX | EINT_DATA_TIMEOUT | EINT_DATA_CRC |
		   EINT_DATA_END_BIT | EINT_CURRENT_LIMIT))
		return 0;

	if (resp)
		for (int i = 0; i < 4; i++)
			resp[i] = rd32(SDHCI_RESPONSE + i * 4);
	return 1;
}

/* Single block transfer in PIO mode. dir: 1 = card->host. */
static int pio_block(uint32_t lba, void *buf, int dir)
{
	uint8_t *p = (uint8_t *)buf;
	uint16_t tm = TM_BLOCK_COUNT_EN;
	if (dir)
		tm |= TM_READ;

	if (!wait_cmd_free())
		return 0;
	if (!wait_dat_free())
		return 0;

	wr16(SDHCI_BLOCK_SIZE, BLOCK_SIZE);
	wr16(SDHCI_BLOCK_COUNT, 1);
	wr16(SDHCI_TRANSFER_MODE, tm);

	/* DATA_PRESENT (bit 5) | RESP_TYPE 1 (bit 1) | CRC check (bit 3). */
	uint16_t cmdreg = (uint16_t)(((dir ? 17u : 24u) << 8) | 0x0020u |
				     0x0001u | 0x0008u);
	wr16(SDHCI_NORMAL_INT_STATUS, 0xffff);
	wr16(SDHCI_ERROR_INT_STATUS, 0xffff);
	wr32(SDHCI_ARGUMENT, lba);
	wr16(SDHCI_COMMAND, cmdreg);

	/*
	 * Move the fifo word through a local so the caller's buffer is only ever
	 * touched byte-wise.
	 *
	 * Casting the buffer to uint32_t* and storing would be an unaligned
	 * 32-bit access whenever the caller's buffer is not 4-byte aligned --
	 * and with the MMU off every access is Device memory, where an unaligned
	 * access is an alignment fault, not a slow path. ufslog's buffers happen
	 * to be aligned, but the block-initialise path reads into a plain stack
	 * array, so relying on that would be a trap waiting for the next caller.
	 */
	uint32_t words = BLOCK_SIZE / 4;
	for (uint32_t i = 0; i < words; i++) {
		uint32_t want = dir ? INT_BUF_READ_READY : INT_BUF_WRITE_READY;
		if (!wait_bits(SDHCI_NORMAL_INT_STATUS, want, SDHCI_TIMEOUT_MS))
			return 0;
		wr16(SDHCI_NORMAL_INT_STATUS, (uint16_t)want);

		uint8_t *dst = p + i * 4;
		if (dir) {
			uint32_t w = rd32(SDHCI_BUFFER);
			dst[0] = (uint8_t)(w);
			dst[1] = (uint8_t)(w >> 8);
			dst[2] = (uint8_t)(w >> 16);
			dst[3] = (uint8_t)(w >> 24);
		} else {
			uint32_t w = (uint32_t)dst[0] |
				     ((uint32_t)dst[1] << 8) |
				     ((uint32_t)dst[2] << 16) |
				     ((uint32_t)dst[3] << 24);
			wr32(SDHCI_BUFFER, w);
		}
	}

	if (!wait_bits(SDHCI_NORMAL_INT_STATUS, INT_TRANSFER_COMPLETE,
		       SDHCI_TIMEOUT_MS))
		return 0;

	uint16_t err = rd16(SDHCI_ERROR_INT_STATUS);
	if (err & (EINT_DATA_TIMEOUT | EINT_DATA_CRC | EINT_DATA_END_BIT))
		return 0;
	return 1;
}

/* ---- card bring-up ------------------------------------------------------ */

#define R1_READY_FOR_DATA	(1u << 8)
#define R1_CURRENT_STATE	(0xfu << 9)
#define R1_STATE_SHIFT		9
#define R1_STATE_TRAN		4u

static uint32_t rca;

static int card_init(void)
{
	uint32_t resp[4];

	/* CMD0: reset the card to idle. It does not answer, so the result is
	 * meaningless here. */
	cmd(0, 0, 0, 0);
	spin_ms(2);

	/*
	 * CMD1 (SEND_OP_COND) is eMMC's init command. It must be repeated until
	 * the card sets the "power up complete" bit (OCR[31]); once it returns
	 * 1.65-1.95 V as available (OCR[7]), it has finished initialising.
	 */
	int ok = 0;
	for (int i = 0; i < 1000; i++) {
		ok = cmd(1, 0x40ff8000u, 1, resp);	/* sector mode | 3.3 V */
		if (ok && (resp[0] & (1u << 31)))
			break;
		spin_ms(1);
	}
	if (!ok || !(resp[0] & (1u << 31))) {
		con_str("EMMC", "CMD1 never reported power-up complete");
		return 0;
	}
	/* OCR[30] = card capacity status: 1 means high capacity (block addressed),
	 * which is what CMD17/24 with an LBA need. */
	con_hex("OCR", resp[0]);

	/* CMD2: ask for the CID (needed by nothing here, but proves the card
	 * is alive and in the right state). */
	if (!cmd(2, 0, 2, resp)) {
		con_str("EMMC", "CMD2 failed");
		return 0;
	}

	/* CMD3: the card picks an RCA. */
	if (!cmd(3, 1u << 16, 1, resp)) {
		con_str("EMMC", "CMD3 failed");
		return 0;
	}
	rca = resp[0] & 0xffff0000u;

	/* CMD7: select the card -> transfer state. */
	if (!cmd(7, rca, 1, resp)) {
		con_str("EMMC", "CMD7 failed");
		return 0;
	}

	/* Verify the state so a silent failure is not mistaken for success. */
	if (!cmd(13, rca, 1, resp)) {
		con_str("EMMC", "CMD13 failed");
		return 0;
	}
	uint32_t st = (resp[0] >> R1_STATE_SHIFT) & 0xf;
	con_hex2("EMMC", "rca=", rca >> 16, "state=", st);
	if (st != R1_STATE_TRAN) {
		con_str("EMMC", "card not in transfer state");
		return 0;
	}

	/* CMD16: 512-byte blocks. */
	if (!cmd(16, BLOCK_SIZE, 1, resp)) {
		con_str("EMMC", "CMD16 failed");
		return 0;
	}

	return 1;
}

/* Defined below; test_emmc() calls it once the card is usable. */
static int find_userdata(void);

void test_emmc(void)
{
	emmc_ready = 0;
	con_hex2("EMMC", "base=", (uint32_t)SDHCI_BASE,
		 "caps=", rd32(SDHCI_CAPABILITIES));

	/* Power first: POWER_CONTROL must be non-zero or the card gets no VDD. */
	uint8_t pwr = (uint8_t)(PC_BUS_POWER | PC_BUS_VOLT_3_3);
	wr8(SDHCI_POWER_CONTROL, pwr);
	spin_ms(2);

	if (!clear_reset()) {
		con_str("EMMC", "reset stuck: block not clocked");
		return;
	}
	if (!clock_on(400)) {
		con_str("EMMC", "400 kHz clock did not start");
		return;
	}
	clock_card(1);
	spin_ms(2);

	/* 1-bit bus is what POR gives; keep it, the log does not need width. */
	wr8(SDHCI_HOST_CONTROL, rd8(SDHCI_HOST_CONTROL) & ~(uint8_t)HC_8BIT_BUS);

	/* Timeout: maximum, so a slow card is not read as missing. */
	wr8(SDHCI_TIMEOUT_CONTROL, 0x0e);

	if (!card_init()) {
		emmc_ready = 0;
		return;
	}

	/* Prove a read works before claiming the log can use us. */
	uint8_t blk[BLOCK_SIZE];
	if (!pio_block(0, blk, 1)) {
		con_str("EMMC", "LBA0 read failed");
		return;
	}
	/* An eMMC's LBA0 ends with the 0x55AA boot signature. */
	con_hex2("EMMC", "lba0[510]=", blk[510], "lba0[511]=", blk[511]);
	if (blk[510] != 0x55 || blk[511] != 0xaa) {
		con_str("EMMC", "LBA0 has no boot signature: reads unreliable");
		return;
	}

	emmc_ready = 1;
	con_str("EMMC", "card ready, 512-byte PIO");

	/*
	 * Find where the log actually goes. Until this succeeds the log stays
	 * disabled: ufslog's LBA is relative to userdata, and writing it as a
	 * card address would scatter the log over unrelated partitions.
	 */
	if (!find_userdata())
		con_str("EMMC", "userdata not found: log disabled");
}

/* ---- interface used by ufslog.c ---------------------------------------- */

/*
 * ufslog works in 4096-byte blocks (it was written for UFS, whose LUNs use
 * 4K), while an eMMC here is driven with 512-byte blocks. Translate: one
 * caller block is LOG_SHIFT card blocks, so the LBA and the byte count the
 * caller uses keep their meaning and the log's on-disk layout does not move.
 *
 * Getting this wrong would be silent in the worst way -- a 4096-byte buffer
 * written as a single 512-byte block loses 7/8 of every log line while
 * reporting success -- so the arithmetic is done once, here.
 */
#define LOG_BLOCK	4096u
#define CARD_BLOCK	BLOCK_SIZE
#define LOG_SHIFT	(LOG_BLOCK / CARD_BLOCK)	/* 8 */
#define LOG_SHIFT_BITS	3

/*
 * Where the log lives, in *card* 4096-byte blocks.
 *
 * ufslog's LBA 11641087 is an offset **inside userdata**, not inside the card
 * (userdata is mmcblk0p87 and the card has 122142720 sectors, so that value
 * cannot be a card address at all). Writing it as a card address would put the
 * log somewhere the recovery-side `dd` never looks, and "no log" would again be
 * indistinguishable from "shim did not run".
 *
 * So the offset of userdata is read from the GPT on the card itself: header at
 * LBA1, entries at LBA2, 128 bytes each. Each entry is a type GUID, a unique
 * GUID, first_lba and last_lba, then a UTF-16 name. A partition named
 * "userdata" gives the shift directly.
 *
 * If the GPT cannot be read the log stays disabled rather than writing to a
 * guessed address -- a wrong sector is far more damaging than a missing log.
 */
static uint32_t userdata_first_card_lba;

static uint32_t strlen_local(const char *s)
{
	uint32_t n = 0;
	while (s[n])
		n++;
	return n;
}

/* Compare a UTF-16LE name in a GPT entry against an ASCII literal. */
static int gpt_name_is(const uint8_t *p, const char *want)
{
	uint32_t n = strlen_local(want);

	for (uint32_t i = 0; i < n; i++)
		if (p[i * 2] != (uint8_t)want[i] || p[i * 2 + 1] != 0)
			return 0;
	/* The stored name is NUL-terminated UTF-16. */
	return p[0] != 0 && p[n * 2] == 0 && p[n * 2 + 1] == 0;
}

/*
 * Read the GPT and record userdata's first LBA (in 512-byte sectors).
 * Returns 1 on success.
 *
 * Entries are read one sector at a time and scanned as they arrive rather than
 * buffered whole: the entry array can be 16 KB (128 x 128) or larger if the
 * entry size is not 128, and a fixed buffer would silently truncate the scan
 * exactly when a non-standard layout needs it most.
 */
static int find_userdata(void)
{
	static uint8_t hdr[512] __attribute__((aligned(8)));
	static uint8_t buf[512] __attribute__((aligned(8)));

	if (!pio_block(1, hdr, 1)) {
		con_str("EMMC", "GPT header unreadable");
		return 0;
	}
	if (hdr[0] != 'E' || hdr[1] != 'F' || hdr[2] != 'I' || hdr[3] != ' ' ||
	    hdr[4] != 'P' || hdr[5] != 'A' || hdr[6] != 'R' || hdr[7] != 'T') {
		con_str("EMMC", "no GPT at LBA1");
		return 0;
	}

	/* PartitionEntryLBA is at offset 72 (u64, low word is enough for a card
	 * this size), NumberOfPartitionEntries at 80 (u32), SizeOfPartitionEntry
	 * at 84 (u32). */
	uint32_t ent_lba = *(volatile uint32_t *)(hdr + 72);
	uint32_t ent_num = *(volatile uint32_t *)(hdr + 80);
	uint32_t ent_sz = *(volatile uint32_t *)(hdr + 84);

	if (ent_sz < 128 || ent_sz > 4096 || ent_num == 0 || ent_num > 1024) {
		con_hex2("EMMC", "bad_gpt_ents=", ent_num, "sz=", ent_sz);
		return 0;
	}

	uint64_t total = (uint64_t)ent_num * ent_sz;
	uint32_t sectors = (uint32_t)((total + 511) / 512);

	/* Walk the array sector by sector; an entry may straddle a sector. */
	uint32_t cached_sec = 0xffffffffu;
	for (uint32_t i = 0; i < ent_num; i++) {
		uint64_t off = (uint64_t)i * ent_sz;
		uint32_t sec = (uint32_t)(off / 512);
		uint32_t in = (uint32_t)(off % 512);

		/* An entry never spans more than one sector boundary at these
		 * sizes, but check rather than assume. */
		if (in + ent_sz > 512) {
			con_hex2("EMMC", "entry straddles sector at", i, "sz=", ent_sz);
			return 0;
		}
		if (sec >= sectors || sec >= 4096) {
			break;
		}
		if (sec != cached_sec) {
			if (!pio_block(ent_lba + sec, buf, 1)) {
				con_str("EMMC", "GPT entries unreadable");
				return 0;
			}
			cached_sec = sec;
		}

		const uint8_t *e = buf + in;
		int used = 0;
		for (int b = 0; b < 16; b++)
			if (e[b]) {
				used = 1;
				break;
			}
		if (!used)
			continue;
		/* Name is UTF-16LE at offset 56, 36 code units. */
		if (gpt_name_is(e + 56, "userdata")) {
			userdata_first_card_lba =
				*(volatile uint32_t *)(e + 32);
			con_hex2("EMMC", "userdata_lba=",
				 userdata_first_card_lba, "ent=", i);
			return userdata_first_card_lba != 0;
		}
	}
	con_str("EMMC", "no userdata partition in GPT");
	return 0;
}

int ufs_ready(void) { return emmc_ready; }

uint32_t write_blocks(uint32_t lba, uint32_t count, const void *buf)
{
	if (!emmc_ready || !userdata_first_card_lba)
		return 1;
	if (!LOG_SHIFT || (1u << LOG_SHIFT_BITS) != LOG_SHIFT)
		return 1;	/* only the 512/4096 pair is implemented */

	const uint8_t *p = (const uint8_t *)buf;
	uint32_t card_lba = userdata_first_card_lba + (lba << LOG_SHIFT_BITS);

	/* Write the whole run as consecutive 512-byte transfers. */
	for (uint32_t total = count << LOG_SHIFT_BITS, i = 0; i < total; i++)
		if (!pio_block(card_lba + i, (void *)(p + i * CARD_BLOCK), 0))
			return (i >> LOG_SHIFT_BITS) + 1;
	return 0;
}

uint32_t ufs_write_blocks(uint32_t lba, uint32_t count, const void *buf)
{
	return write_blocks(lba, count, buf);
}

uint32_t read_blocks_pub(uint32_t lba, uint32_t count, void *buf)
{
	if (!emmc_ready || !userdata_first_card_lba)
		return 1;
	if (!LOG_SHIFT || (1u << LOG_SHIFT_BITS) != LOG_SHIFT)
		return 1;

	uint8_t *p = (uint8_t *)buf;
	uint32_t card_lba = userdata_first_card_lba + (lba << LOG_SHIFT_BITS);

	for (uint32_t total = count << LOG_SHIFT_BITS, i = 0; i < total; i++)
		if (!pio_block(card_lba + i, p + i * CARD_BLOCK, 1))
			return (i >> LOG_SHIFT_BITS) + 1;
	return 0;
}
