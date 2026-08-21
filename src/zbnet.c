/*
 * ZigBee network thread — see zbnet.h.
 *
 * Why a thread. The radio talks AT over USART3, and every exchange blocks until
 * the Telegesis answers: a unicast waits for its ACK, a join streams its JPAN
 * seconds after the OK. Run on the UI loop that meant the screen froze for the
 * length of each AT call and, because the old state machine was gated to the
 * 10 s report tick, a press waited up to 10 s before it was even sent. Here the
 * radio has its own thread: it blocks all it likes on USART3 while the UI thread
 * keeps painting and feeding the watchdog, and a press is unicast within
 * milliseconds of landing in the queue.
 *
 * Ownership: this thread is the sole reader of the zbx RX rings after boot. The
 * ISR fills them; nothing on the main thread may call zbx_* or em250_at once
 * this is running, or the two readers race.
 */
#include <zephyr/kernel.h>
#include <string.h>
#include <stdio.h>

#include "zbnet.h"
#include "em250.h"
#include "zbx.h"
#include "keypad.h"
#include "updater.h"

#define AT_BRR       0x061A   /* 19200, the Telegesis app's default baud */
#define HEARTBEAT_MS 30000    /* idle keepalive: also how we notice the coord vanished */
#define MISS_LIMIT   3        /* consecutive unacked sends before we assume the mesh moved */

/* Presses waiting to go out. Deep enough to absorb a burst of mashing while a
 * send is in flight; a full queue drops the newest press rather than blocking
 * the key handler. */
K_MSGQ_DEFINE(key_q, sizeof(uint8_t), 16, 1);

static volatile bool joined_flag;

bool zbnet_joined(void)            { return joined_flag; }
void zbnet_send_key(uint8_t code)  { k_msgq_put(&key_q, &code, K_NO_WAIT); }

/* One Telegesis reply on one USB line, control chars shown as ~. Same shape as
 * the old at_log so the CA-1 side reads identically. */
static void log_at(const char *tag, char *reply)
{
	char line[176];

	for (char *p = reply; *p; p++) {
		if (*p == '\r' || *p == '\n') {
			*p = '~';
		}
	}
	snprintf(line, sizeof line, "AT %s -> %s", tag, reply[0] ? reply : "(silent)");
	updater_emit(line);
}

/*
 * Walk the radio onto the CA-1's network. This is the sequence proven during
 * bring-up, moved here verbatim: confirm the radio, set the HA-default trust
 * centre key, pin channel 15, come up as an end device with HA-profile xCAST
 * framing, then join (the CA-1 hands the network key over in the clear).
 * Returns true once joined.
 */
static bool join_once(void)
{
	char reply[160];

	/* Already on a network? Asked *first*, because the setup below is not free to repeat:
	 * `ATS0A` rewrites the module's main function, and doing that to a node that is already
	 * joined drops it off the mesh. That is what turned a single missed ACK into a handset
	 * that unjoined itself and then had nowhere to send the key — every press after it read
	 * `AT+N=NoPAN` and no unicast ever left. The S-registers below live in the module's NVM
	 * and survive a reset, so a node that is on a network has been through this already.
	 */
	em250_at("AT+N", AT_BRR, reply, sizeof reply, 800);
	log_at("AT+N", reply);
	if (strstr(reply, "+N=") && !strstr(reply, "NoPAN")) {
		return true;
	}

	em250_at("ATI", AT_BRR, reply, sizeof reply, 500);
	log_at("ATI", reply);

	/* trust-centre link key = ZigBeeAlliance09 (HA default) */
	em250_at("ATS09=5A6967426565416C6C69616E63653039:password",
		 AT_BRR, reply, sizeof reply, 800);
	log_at("S09 linkkey", reply);

	/* channel mask = channel 15 only (bit 4) */
	em250_at("ATS00=0010", AT_BRR, reply, sizeof reply, 600);
	log_at("S00 chan15", reply);

	/* main function 0x4100: end device (bit E) + preconfigured TC link key
	 * (bit 8). 0x8100 would be a sleepy end device (battery). */
	em250_at("ATS0A=4100:password", AT_BRR, reply, sizeof reply, 600);
	log_at("S0A enddev", reply);

	/* xCAST framing so herdsman surfaces our unicasts to the application: HA
	 * profile 0x0104 (S44), source/dest endpoint 1 (S40), cluster 0x0006
	 * (S42). Without a real profile the frames land on Telegesis's private
	 * 0xC091 and herdsman drops them before any converter. */
	em250_at("ATS44=0104", AT_BRR, reply, sizeof reply, 500);
	log_at("S44 profile", reply);
	em250_at("ATS40=0101", AT_BRR, reply, sizeof reply, 500);
	log_at("S40 endpoints", reply);
	em250_at("ATS42=0006", AT_BRR, reply, sizeof reply, 500);
	log_at("S42 cluster", reply);

	/* join — JPAN streams seconds after the OK, so read the whole window. */
	em250_at_wait("AT+JN", AT_BRR, reply, sizeof reply, 9000);
	log_at("AT+JN", reply);
	return strstr(reply, "JPAN") != NULL;
}

/* Keep trying to join, yielding between attempts. Returns only once joined. */
static void join_until_up(void)
{
	while (!join_once()) {
		k_msleep(500);
	}
	joined_flag = true;
	updater_emit("AT *** JOINED CA-1 ***");
}

/* Unicast one AT payload to the coordinator (0x0000) and report whether it was
 * ACKed. em250_at (gap-terminated), not em250_at_wait (full-window): the SEQ/ACK
 * comes back in a fraction of a second, so this returns promptly and the next
 * queued press is not stuck behind a fixed 3 s wait. */
static bool ucast(const char *payload, const char *tag)
{
	char reply[160];
	char cmd[64];

	snprintf(cmd, sizeof cmd, "AT+UCAST:0000=%s", payload);
	em250_at(cmd, AT_BRR, reply, sizeof reply, 1200);
	log_at(tag, reply);
	return strstr(reply, "ACK") != NULL;
}

static void zbnet_thread(void *a, void *b, void *c)
{
	ARG_UNUSED(a); ARG_UNUSED(b); ARG_UNUSED(c);

	unsigned miss = 0;

	k_msleep(1500);        /* let boot + splash settle before seizing the UART */
	join_until_up();

	for (;;) {
		uint8_t key;
		bool acked;
		char payload[32];

		if (k_msgq_get(&key_q, &key, K_MSEC(HEARTBEAT_MS)) == 0) {
			/* "K<code> <name>": the code drives the Juno mapping, the name
			 * keeps the CA-1 log readable. */
			snprintf(payload, sizeof payload, "K%u %s", key, keypad_name(key));
			acked = ucast(payload, "UCAST key");
		} else {
			acked = ucast("IDLE", "UCAST idle");
		}

		/* Drain any passive frames the ISR queued so done_q does not back up
		 * while we are the only reader. */
		{ const uint8_t *f; while (zbx_poll(&f)) { } }

		/* A run of unacked sends means the coordinator re-formed the mesh
		 * without us; a single miss is just a dropped packet, so tolerate a
		 * few before paying for a full rejoin. */
		if (acked) {
			miss = 0;
		} else if (++miss >= MISS_LIMIT) {
			miss = 0;
			joined_flag = false;
			join_until_up();
		}
	}
}

/* Lower priority than the main (UI) thread so a 9 s join never delays a
 * repaint; the two cooperate anyway, since every AT read yields in zbx_getc. */
#define ZBNET_PRIO 7
K_THREAD_STACK_DEFINE(zbnet_stack, 2048);
static struct k_thread zbnet_data;

void zbnet_start(void)
{
	k_thread_create(&zbnet_data, zbnet_stack, K_THREAD_STACK_SIZEOF(zbnet_stack),
			zbnet_thread, NULL, NULL, NULL,
			ZBNET_PRIO, 0, K_NO_WAIT);
	k_thread_name_set(&zbnet_data, "zbnet");
}
