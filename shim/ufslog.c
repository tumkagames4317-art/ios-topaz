/*
 * Persistent console log written straight into userdata over UFS.
 *
 * Why: on ginkgo the panic screen flashes for half a second, so every
 * diagnosis needed the user to hold a camera at the right moment. Writing the
 * text to a fixed place on UFS means a later recovery boot can `dd` it out and
 * read the whole story, so the shim can be debugged with no human in the loop.
 *
 * Placement: the LAST 1025 blocks of userdata, which is exactly what patch
 * 0047 does to xnu (a 4 MiB ring at the end of the partition). Keeping the same
 * spot means tools/sweet-log-pull.sh and our own pull-logs.sh already read the
 * right place, and it stays clear of any real filesystem data if the iOS root
 * is ever flashed there.
 *
 * The writer is deliberately allocation-free and never blocks: a failure to
 * log must never be the reason a boot fails.
 */
#include "ufslog.h"
#include "console.h"
#include "ufs.h"

/*
 * userdata on ginkgo is 47686090752 bytes = 11642112 blocks of 4096.
 * Keep the last 1025 blocks, i.e. LBA [11641087, 11642112).
 */
#define LOG_FIRST_LBA	11641087u
#define LOG_BLOCKS	1025u
#define LOG_LBA_SPAN	1024u		/* one block for the header, 1024 for text */

#define LOG_MAGIC	"SWEETLOG"
#define LOG_TEXT_BYTES	(LOG_LBA_SPAN * 4096u)

static int log_ready;
static int ufslog_ready_flag;
static uint32_t log_lba;		/* next block to write */
static uint32_t log_used;		/* bytes used in the current block */
static char log_buf[4096] __attribute__((aligned(4096)));

/*
 * The UFS host must be running for these writes. ufs.c brings it up during its
 * probe; ufslog_init is called after that, and is a no-op when the probe said
 * the host is not usable, so a broken UFS can never wedge the boot.
 */
void ufslog_init(void)
{
	if (!ufs_ready()) {
		log_ready = 0;
		return;
	}
	log_lba = LOG_FIRST_LBA + 1;	/* block 0 of the region holds the header */
	log_used = 0;
	log_ready = 1;

	/*
	 * Prove the write path before relying on it: write a block, read it back
	 * and compare. Without this a silently failing WRITE(10) would look
	 * exactly like a shim that printed nothing, and we would chase the wrong
	 * bug for hours.
	 */
	static char probe[4096] __attribute__((aligned(4096)));
	for (unsigned i = 0; i < sizeof(probe); i++)
		probe[i] = 0;
	const char *tag = "SWEETLOG-PROBE";
	for (int i = 0; tag[i]; i++)
		probe[i] = tag[i];
	uint32_t wr = write_blocks(LOG_FIRST_LBA, 1, probe);
	for (unsigned i = 0; i < sizeof(probe); i++)
		probe[i] = 0;
	uint32_t rd = read_blocks_pub(LOG_FIRST_LBA, 1, probe);
	int same = 1;
	for (int i = 0; tag[i]; i++)
		if (probe[i] != tag[i])
			same = 0;
	con_hex2("UFSp", "wr=", wr, "rd=", rd);
	con_str("UFSp", same ? "write verified" : "write NOT verified");
	ufslog_ready_flag = same ? 1 : 0;

	ufslog_write("---- shim log start ----\n");
}

int ufslog_active(void)
{
	return log_ready;
}

/* Flush the current block; called when it fills and at the end of a panic. */
static void flush_block(void)
{
	if (!log_ready || !log_used)
		return;
	for (uint32_t i = log_used; i < sizeof(log_buf); i++)
		log_buf[i] = 0;
	if (write_blocks(log_lba, 1, log_buf) == 0) {
		log_lba++;
		if (log_lba >= LOG_FIRST_LBA + LOG_BLOCKS)
			log_lba = LOG_FIRST_LBA + 1;	/* wrap: keep the newest text */
	}
	log_used = 0;
}

void ufslog_write(const char *s)
{
	if (!log_ready || !s)
		return;
	for (; *s; s++) {
		log_buf[log_used++] = *s;
		if (log_used == sizeof(log_buf))
			flush_block();
	}
}

/* Same output format the on-screen console uses, so a photo and the log match. */
void ufslog_str(const char *label, const char *s)
{
	ufslog_write(label);
	ufslog_write(" ");
	ufslog_write(s ? s : "(null)");
	ufslog_write("\n");
}

static const char hexd[] = "0123456789abcdef";

void ufslog_hex(const char *label, uint64_t v)
{
	char b[17];
	for (int i = 0; i < 16; i++)
		b[i] = hexd[(v >> (4 * (15 - i))) & 0xf];
	b[16] = 0;
	ufslog_str(label, b);
}

void ufslog_hex2(const char *label, const char *a, uint32_t va, const char *b, uint32_t vb)
{
	char h[9];
	ufslog_write(label);
	ufslog_write(" ");
	if (a) {
		ufslog_write(a);
		ufslog_write("=");
		for (int i = 0; i < 8; i++)
			h[i] = hexd[(va >> (4 * (7 - i))) & 0xf];
		h[8] = 0;
		ufslog_write(h);
	}
	if (b) {
		ufslog_write(" ");
		ufslog_write(b);
		ufslog_write("=");
		for (int i = 0; i < 8; i++)
			h[i] = hexd[(vb >> (4 * (7 - i))) & 0xf];
		h[8] = 0;
		ufslog_write(h);
	}
	ufslog_write("\n");
}

void ufslog_flush(void)
{
	flush_block();
}

/*
 * Record why we are about to park. Called from the exception handler, so it
 * must not rely on anything that could itself fault: only UFS writes and the
 * flat buffer above.
 */
void ufslog_panic(uint64_t slot, const char *stage, uint64_t esr, uint64_t elr,
		  uint64_t far, uint64_t spsr, uint64_t x0, uint64_t x1, uint64_t x30)
{
	ufslog_write("==== PANIC ====\n");
	ufslog_hex("SLOT", slot);
	ufslog_str("STAGE", stage ? stage : "?");
	ufslog_hex("ESR", esr);
	ufslog_hex("ELR", elr);
	ufslog_hex("FAR", far);
	ufslog_hex("SPSR", spsr);
	ufslog_hex("X0", x0);
	ufslog_hex("X1", x1);
	ufslog_hex("X30", x30);
	ufslog_write("==== END ====\n");
	ufslog_flush();
}
