/* R3_FIFO_GET_AT_LABEL (third_party/rpcs3/rpcs3/Emu/RSX/port_get_at_label.h): GET is past a label release by
 * the time its value lands.
 *
 * Replays the 2026-11-16 R3 hang: the ring held SEMAPHORE_OFFSET 0x400 / RELEASE 0x30000000 / OFFSET 0x410 /
 * RELEASE 0x30000000 at IO 0x039FFD00 with PUT 0x039FFD20. The puller publishes GET as the execution position
 * (R3_FIFO_GET_EXEC); the guest (func_00734A30 at 0x00734C08) sees label 0x41, reads GET once and spins until PUT
 * equals it. Its snapshot was 0x039FFD18, the last RELEASE's own header, and PUT never moved again. */
#include <stdio.h>
#include <stdint.h>
#include "port_get_at_label.h"

static int fails;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

#define BASE 0x039FFD00u
#define PUT  0x039FFD20u

/* GET_EXEC puller over four single-argument packets; returns the GET the guest snapshots right after the
 * label it waits for (the second RELEASE) lands. */
static uint32_t replay(int fix)
{
    uint32_t get = BASE, snapshot = 0;
    for (uint32_t hdr = BASE; hdr < PUT; hdr += 8)
    {
        const uint32_t arg = hdr + 4;              /* m_fifo_pos while the method runs */
        const int release = ((hdr - BASE) / 8) & 1;
        if (release && fix)
        {
            const uint32_t next = port_get_at_label(get, arg);
            if (next) get = next;
        }
        if (release && hdr == BASE + 0x18u)
            snapshot = get;                        /* label 0x41 just landed */
        get = arg + 4;                             /* sync_get after the packet */
    }
    CHECK(get == PUT);                             /* the puller always ends idle at PUT */
    return snapshot;
}

int main(void)
{
    /* without publication the guest's snapshot is the RELEASE header and never equals PUT: the hang */
    CHECK(replay(0) == BASE + 0x18u);
    /* with it, the snapshot is PUT and the guest's wait ends at once */
    CHECK(replay(1) == PUT);

    /* GET at the executing word's header or earlier in the same stream: publish the end of the word */
    CHECK(port_get_at_label(BASE + 0x18u, BASE + 0x1Cu) == PUT);
    CHECK(port_get_at_label(BASE + 0x1Cu, BASE + 0x1Cu) == PUT);
    CHECK(port_get_at_label(BASE, BASE + 0x1Cu) == PUT);
    /* already published (upstream cache-end publication, or this word earlier): nothing, never backwards */
    CHECK(port_get_at_label(PUT, BASE + 0x1Cu) == 0);
    CHECK(port_get_at_label(PUT + 0x80u, BASE + 0x1Cu) == 0);
    /* GET in another region (after a JUMP, a wrapped ring): left alone */
    CHECK(port_get_at_label(0x00100000u, 0x039FFD1Cu) == 0);
    CHECK(port_get_at_label(0x039FFF00u, 0x00000104u) == 0);
    /* a multi-argument packet: GET stops after the release word, not the packet */
    CHECK(port_get_at_label(0x1000u, 0x1008u) == 0x100Cu);

    printf("[fifo_get_at_label] %s (%d failed)\n", fails ? "FAIL" : "PASS", fails);
    return fails ? 1 : 0;
}
