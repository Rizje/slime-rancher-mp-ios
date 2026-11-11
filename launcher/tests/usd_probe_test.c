/* SR2 Launcher: ios_usd_read_needs_no_probe (patch 0001) at its boundaries.
 *
 * run.sh copies the helper, the code between the sr2:usd-probe markers, into helper.c. This file
 * gives it what it takes from signal_arm64_ios.c (ios_exc_usd: where user_shared_data is mapped)
 * and checks which accesses skip the region query: only reads wholly inside the shared-data page
 * at 0x7ffe0000 that translate to exactly that mapping, and nothing while MADEIRA_USD_PROBE=1. */
#include <stdint.h>
#include <stdio.h>

static uintptr_t ios_exc_usd;
#include "helper.c"

static int failures;

static void expect( int got, int want, const char *what )
{
    printf( "%s: %s\n", got == want ? "ok" : "FAIL", what );
    if (got != want) failures++;
}

int main( void )
{
    static unsigned char page[0x4000];   /* user_shared_data: one 16 KB host page */
    const uintptr_t usd = (uintptr_t)page;
    const uint64_t g = 0x7ffe0000ull;     /* KUSER_SHARED_DATA in the guest */
    unsigned long long before;

    ios_exc_usd = 0;
    expect( ios_usd_read_needs_no_probe( g + 0x320, usd + 0x320, 8, 0 ), 0, "mapping not known yet: probe" );
    ios_exc_usd = usd;

    before = ios_usd_probe_skips;
    expect( ios_usd_read_needs_no_probe( g + 0x320, usd + 0x320, 8, 0 ), 1, "tick count, 0x7ffe0320, 8 bytes: no probe" );
    expect( ios_usd_read_needs_no_probe( g + 0x328, usd + 0x328, 4, 0 ), 1, "0x7ffe0328, 4 bytes: no probe" );
    expect( ios_usd_read_needs_no_probe( g, usd, 1, 0 ), 1, "the page's first byte: no probe" );
    expect( ios_usd_read_needs_no_probe( g + 0xff8, usd + 0xff8, 8, 0 ), 1, "the page's last 8 bytes: no probe" );
    expect( ios_usd_read_needs_no_probe( g + 0xfff, usd + 0xfff, 1, 0 ), 1, "the page's last byte: no probe" );
    expect( ios_usd_probe_skips - before == 5, 1, "each skip is counted" );

    before = ios_usd_probe_skips;
    expect( ios_usd_read_needs_no_probe( g + 0xffc, usd + 0xffc, 8, 0 ), 0, "a read across the page's end: probe" );
    expect( ios_usd_read_needs_no_probe( g + 0x1000, usd + 0x1000, 4, 0 ), 0, "just above the page: probe" );
    expect( ios_usd_read_needs_no_probe( g - 4, usd - 4, 4, 0 ), 0, "just below the page: probe" );
    expect( ios_usd_read_needs_no_probe( g - 4, usd - 4, 8, 0 ), 0, "from below into the page: probe" );
    /* addr + bytes wraps to 4 here, and the translation is the one the page's own offset arithmetic
     * gives, so only a bound that cannot overflow refuses it */
    expect( ios_usd_read_needs_no_probe( 0xfffffffffffffffcull, usd + (uintptr_t)(0xfffffffffffffffcull - g), 8, 0 ), 0,
            "an address near 2^64 whose end wraps: probe" );
    expect( ios_usd_read_needs_no_probe( g + 0x320, usd + 0x321, 8, 0 ), 0, "translated one byte off: probe" );
    expect( ios_usd_read_needs_no_probe( g + 0x320, usd + 0x4320, 8, 0 ), 0, "translated to another mapping: probe" );
    expect( ios_usd_read_needs_no_probe( g + 0x320, 0, 8, 0 ), 0, "no translation: probe" );
    expect( ios_usd_read_needs_no_probe( g + 0x320, usd + 0x320, 8, 1 ), 0, "a write: probe" );
    expect( ios_usd_read_needs_no_probe( g + 0x320, usd + 0x320, 0, 0 ), 0, "zero bytes: probe" );
    expect( ios_usd_read_needs_no_probe( g + 0x320, usd + 0x320, 0x1000, 0 ), 0, "a page-sized read from 0x320: probe" );
    expect( ios_usd_probe_skips == before, 1, "no skip counted for any of these" );

    ios_usd_probe_keep = 1;   /* MADEIRA_USD_PROBE=1 */
    expect( ios_usd_read_needs_no_probe( g + 0x320, usd + 0x320, 8, 0 ), 0, "MADEIRA_USD_PROBE=1: probe as before" );
    ios_usd_probe_keep = 0;

    if (failures) printf( "%d check(s) failed\n", failures );
    else printf( "PASS: shared-data reads skip the region query exactly when they cannot reach unmapped memory\n" );
    return failures ? 1 : 0;
}
