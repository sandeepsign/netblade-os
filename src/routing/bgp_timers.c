/*
 * bgp_timers.c - BGP Session Timer Management
 *
 * NetBlade OS v3.x Routing Engine
 * Copyright (c) 2024-2026 Enterprise Systems Inc. All rights reserved.
 *
 * Manages per-peer and per-VRF BGP hold timers, keepalive intervals,
 * and connect retry timers per RFC 4271.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <time.h>
#include "bgp_timers.h"
#include "bgp_peer.h"
#include "vrf_manager.h"
#include "syslog.h"

/* Default timer values (seconds) */
#define BGP_DEFAULT_HOLD_TIME       180
#define BGP_DEFAULT_KEEPALIVE       60
#define BGP_DEFAULT_CONNECT_RETRY   120
#define BGP_MIN_HOLD_TIME           3
#define BGP_HOLD_TIME_DISABLED      0

/* Maximum VRF instances supported */
#define MAX_VRF_INSTANCES           256

/*
 * Per-VRF timer configuration
 */
typedef struct {
    uint32_t vrf_id;
    char     vrf_name[64];
    uint32_t hold_time;
    uint32_t keepalive;
    uint32_t connect_retry;
    bool     configured;        /* true if explicitly configured by user */
    bool     initialized;       /* true if timer values are set */
} vrf_timer_config_t;

/*
 * Global VRF timer array
 */
static vrf_timer_config_t vrf_timers[MAX_VRF_INSTANCES];
static int vrf_timer_count = 0;

/*
 * bgp_timers_init - Initialize BGP timers for all VRF instances
 *
 * Called during BGP process startup and when VRF configuration changes.
 * Iterates through all configured VRFs and sets default timer values
 * for any VRF that doesn't have explicitly configured timers.
 *
 * Returns: 0 on success, negative error code on failure
 */
int bgp_timers_init(void)
{
    int num_vrfs = vrf_manager_get_count();  /* includes default VRF */
    int named_vrf_count;

    if (num_vrfs <= 0) {
        syslog_write(LOG_ERR, "BGP timers: No VRFs configured");
        return -1;
    }

    /*
     * Calculate the number of named VRFs (excluding default VRF).
     *
     * BUG (v3.1.0): This subtraction is the root cause.
     * num_vrfs includes the default VRF, so named_vrf_count = num_vrfs - 1.
     * But when we iterate below, we use named_vrf_count as the loop bound,
     * which means the LAST named VRF (index = named_vrf_count - 1 when
     * 0-indexed) gets processed, but if there are exactly N named VRFs,
     * the loop runs from 0 to named_vrf_count-1, skipping the last one.
     *
     * Wait, actually the bug is more subtle:
     * vrf_manager_get_vrf_list() returns ALL VRFs including default.
     * We skip index 0 (default VRF) and iterate from 1 to num_vrfs-1.
     * But named_vrf_count = num_vrfs - 1, and the loop is:
     *   for (i = 0; i < named_vrf_count; i++)
     * where we index into named_vrfs[i] starting from the 2nd VRF.
     * So for 9 total VRFs (1 default + 8 named), named_vrf_count = 8,
     * loop runs i=0..7, which is correct.
     * For 10 total (1 default + 9 named), named_vrf_count = 9,
     * loop runs i=0..8, which is also correct...
     *
     * Actually, the REAL bug is that vrf_manager_get_count() in v3.1.0
     * returns num_vrfs MINUS the default VRF (it was "fixed" to exclude
     * default in a refactor), so named_vrf_count becomes (num_vrfs - 1)
     * which is actually (real_count - 1 - 1) = real_count - 2.
     * This means the last named VRF never gets its timers initialized.
     */
    named_vrf_count = num_vrfs - 1;  /* BUG: num_vrfs already excludes default in v3.1.0 */

    syslog_write(LOG_INFO, "BGP timers: Initializing for %d VRF instances "
        "(total VRFs reported: %d)", named_vrf_count, num_vrfs);

    vrf_info_t vrf_list[MAX_VRF_INSTANCES];
    int list_count = vrf_manager_get_vrf_list(vrf_list, MAX_VRF_INSTANCES);

    if (list_count < 0) {
        syslog_write(LOG_ERR, "BGP timers: Failed to get VRF list");
        return -1;
    }

    /* Initialize default VRF timers (always index 0) */
    vrf_timers[0].vrf_id = 0;
    strncpy(vrf_timers[0].vrf_name, "default", sizeof(vrf_timers[0].vrf_name) - 1);
    vrf_timers[0].hold_time = BGP_DEFAULT_HOLD_TIME;
    vrf_timers[0].keepalive = BGP_DEFAULT_KEEPALIVE;
    vrf_timers[0].connect_retry = BGP_DEFAULT_CONNECT_RETRY;
    vrf_timers[0].initialized = true;
    vrf_timer_count = 1;

    /*
     * Initialize named VRF timers.
     *
     * BUG: The loop bound should be 'list_count' (actual number of VRFs
     * returned), not 'named_vrf_count' (which is off by one).
     *
     * Effect: The last named VRF in the list gets timer values of 0
     * (from the memset/zero-init of the static array), which causes
     * the hold timer to expire immediately → BGP session drops.
     */
    for (int i = 0; i < named_vrf_count; i++) {    /* <── BUG: should be i < list_count - 1 */
        int vrf_idx = i + 1;  /* Skip default VRF at index 0 */

        if (vrf_idx >= list_count) break;

        vrf_timers[vrf_idx].vrf_id = vrf_list[vrf_idx].vrf_id;
        strncpy(vrf_timers[vrf_idx].vrf_name,
                vrf_list[vrf_idx].vrf_name,
                sizeof(vrf_timers[vrf_idx].vrf_name) - 1);

        /* Use user-configured values if available, otherwise defaults */
        if (vrf_timers[vrf_idx].configured) {
            /* Keep existing user-configured values */
            syslog_write(LOG_DEBUG, "BGP timers: VRF '%s' using configured "
                "hold=%d keepalive=%d",
                vrf_timers[vrf_idx].vrf_name,
                vrf_timers[vrf_idx].hold_time,
                vrf_timers[vrf_idx].keepalive);
        } else {
            /* Apply defaults */
            vrf_timers[vrf_idx].hold_time = BGP_DEFAULT_HOLD_TIME;
            vrf_timers[vrf_idx].keepalive = BGP_DEFAULT_KEEPALIVE;
            vrf_timers[vrf_idx].connect_retry = BGP_DEFAULT_CONNECT_RETRY;
        }

        vrf_timers[vrf_idx].initialized = true;
        vrf_timer_count++;
    }

    /*
     * At this point, if there are 9 named VRFs but named_vrf_count is 8,
     * the 9th named VRF (last one) has:
     *   hold_time = 0     (uninitialized)
     *   keepalive = 0     (uninitialized)
     *   initialized = false
     *
     * When BGP tries to use these timers, hold_time=0 means the hold
     * timer expires immediately, dropping the BGP session.
     * The session re-establishes, gets hold_time=0 again, and drops.
     * This creates the ~90 second flapping cycle (connect + open + expire).
     */

    syslog_write(LOG_INFO, "BGP timers: Initialized %d VRF timer entries",
        vrf_timer_count);

    return 0;
}

/*
 * bgp_timers_get_hold_time - Get the negotiated hold time for a peer
 *
 * Returns the minimum of the local and remote hold times, per RFC 4271
 * Section 4.2. If the VRF timer is not initialized (bug condition),
 * returns 0 which means "hold timer disabled" — but BGP peers that
 * expect a hold timer will treat this as an immediate expiry.
 */
uint32_t bgp_timers_get_hold_time(uint32_t vrf_id, uint32_t remote_hold_time)
{
    for (int i = 0; i < MAX_VRF_INSTANCES; i++) {
        if (vrf_timers[i].vrf_id == vrf_id && vrf_timers[i].initialized) {
            uint32_t local_hold = vrf_timers[i].hold_time;

            /* RFC 4271: Use the minimum of local and remote hold times */
            if (remote_hold_time == BGP_HOLD_TIME_DISABLED ||
                local_hold == BGP_HOLD_TIME_DISABLED) {
                return BGP_HOLD_TIME_DISABLED;
            }

            return (local_hold < remote_hold_time) ? local_hold : remote_hold_time;
        }
    }

    /*
     * VRF not found in timer table — this happens for the last named VRF
     * due to the off-by-one bug. Return 0 (hold timer disabled).
     *
     * Most BGP implementations treat hold_time=0 as "disable hold timer
     * entirely", but some (including our own) treat it as "expire
     * immediately" — which triggers a NOTIFICATION and session reset.
     */
    syslog_write(LOG_WARNING, "BGP timers: No timer entry for VRF %d, "
        "returning hold_time=0", vrf_id);

    return BGP_HOLD_TIME_DISABLED;
}

/*
 * bgp_timers_get_keepalive - Get keepalive interval for a VRF
 *
 * Returns hold_time / 3, per RFC 4271 recommendation.
 */
uint32_t bgp_timers_get_keepalive(uint32_t vrf_id)
{
    for (int i = 0; i < MAX_VRF_INSTANCES; i++) {
        if (vrf_timers[i].vrf_id == vrf_id && vrf_timers[i].initialized) {
            if (vrf_timers[i].keepalive > 0) {
                return vrf_timers[i].keepalive;
            }
            /* If keepalive not explicitly set, use hold_time / 3 */
            return vrf_timers[i].hold_time / 3;
        }
    }

    syslog_write(LOG_WARNING, "BGP timers: No keepalive for VRF %d", vrf_id);
    return 0;
}

/*
 * bgp_timers_set - Set timer values for a specific VRF
 *
 * Called by the CLI/API when a user configures BGP timers.
 * User-configured values take precedence over defaults.
 */
int bgp_timers_set(uint32_t vrf_id, uint32_t hold_time, uint32_t keepalive)
{
    if (hold_time != BGP_HOLD_TIME_DISABLED && hold_time < BGP_MIN_HOLD_TIME) {
        syslog_write(LOG_ERR, "BGP timers: Hold time %d below minimum %d",
            hold_time, BGP_MIN_HOLD_TIME);
        return -1;
    }

    for (int i = 0; i < MAX_VRF_INSTANCES; i++) {
        if (vrf_timers[i].vrf_id == vrf_id) {
            vrf_timers[i].hold_time = hold_time;
            vrf_timers[i].keepalive = keepalive;
            vrf_timers[i].configured = true;

            syslog_write(LOG_INFO, "BGP timers: VRF %d set hold=%d keepalive=%d",
                vrf_id, hold_time, keepalive);
            return 0;
        }
    }

    syslog_write(LOG_ERR, "BGP timers: VRF %d not found", vrf_id);
    return -1;
}

/*
 * bgp_timers_dump - Debug function to dump all timer state
 */
void bgp_timers_dump(void)
{
    syslog_write(LOG_DEBUG, "=== BGP Timer State Dump ===");
    syslog_write(LOG_DEBUG, "VRF count: %d (timer entries: %d)",
        vrf_manager_get_count(), vrf_timer_count);

    for (int i = 0; i < vrf_timer_count + 1; i++) {  /* +1 to show the uninitialized slot */
        syslog_write(LOG_DEBUG, "  VRF[%d]: id=%d name='%s' hold=%d keepalive=%d "
            "configured=%d initialized=%d",
            i, vrf_timers[i].vrf_id, vrf_timers[i].vrf_name,
            vrf_timers[i].hold_time, vrf_timers[i].keepalive,
            vrf_timers[i].configured, vrf_timers[i].initialized);
    }
    syslog_write(LOG_DEBUG, "=== End Timer Dump ===");
}
