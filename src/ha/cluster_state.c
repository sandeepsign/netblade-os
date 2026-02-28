/*
 * cluster_state.c - HA Cluster State Management
 *
 * NetBlade OS v3.x High Availability Module
 * Copyright (c) 2024-2026 Enterprise Systems Inc. All rights reserved.
 *
 * Manages cluster role elections, heartbeat monitoring, and split-brain
 * detection/recovery for 2-node HA pairs.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <time.h>
#include <pthread.h>
#include "cluster_state.h"
#include "heartbeat.h"
#include "syslog.h"
#include "interface_manager.h"

/* Cluster roles */
#define CLUSTER_ROLE_INIT       0
#define CLUSTER_ROLE_ACTIVE     1
#define CLUSTER_ROLE_STANDBY    2
#define CLUSTER_ROLE_SPLIT      3   /* Both nodes active — error state */

/* Heartbeat settings */
#define HEARTBEAT_INTERVAL_MS   1000    /* 1 second */
#define HEARTBEAT_TIMEOUT_MS    3000    /* 3 missed heartbeats */
#define SPLIT_BRAIN_DELAY_MS    5000    /* Wait before declaring split-brain */

/* Election priority */
#define ELECTION_PRIORITY_SERIAL    0   /* Lower serial number wins */
#define ELECTION_PRIORITY_UPTIME    1   /* Higher uptime wins */

typedef struct {
    uint8_t     local_role;
    uint8_t     peer_role;
    bool        heartbeat_up;
    time_t      last_heartbeat_rx;
    time_t      last_heartbeat_tx;
    uint32_t    cluster_id;
    char        local_serial[32];
    char        peer_serial[32];
    bool        split_brain_detected;
    bool        auto_recovery_enabled;
    uint8_t     election_policy;
    pthread_mutex_t state_lock;
} cluster_state_t;

static cluster_state_t cluster;

/*
 * cluster_state_init - Initialize cluster state machine
 */
int cluster_state_init(uint32_t cluster_id, const char *local_serial)
{
    memset(&cluster, 0, sizeof(cluster_state_t));
    pthread_mutex_init(&cluster.state_lock, NULL);

    cluster.cluster_id = cluster_id;
    strncpy(cluster.local_serial, local_serial, sizeof(cluster.local_serial) - 1);
    cluster.local_role = CLUSTER_ROLE_INIT;
    cluster.peer_role = CLUSTER_ROLE_INIT;
    cluster.heartbeat_up = false;
    cluster.auto_recovery_enabled = false;
    cluster.election_policy = ELECTION_PRIORITY_SERIAL;

    syslog_write(LOG_INFO, "Cluster %d initialized. Local serial: %s",
        cluster_id, local_serial);

    return 0;
}

/*
 * cluster_heartbeat_tick - Process heartbeat state
 *
 * Called every HEARTBEAT_INTERVAL_MS by the heartbeat daemon.
 * Detects heartbeat loss and triggers role transitions.
 */
int cluster_heartbeat_tick(void)
{
    pthread_mutex_lock(&cluster.state_lock);

    time_t now = time(NULL);
    double ms_since_rx = difftime(now, cluster.last_heartbeat_rx) * 1000;

    /* Check if heartbeat is alive */
    if (cluster.heartbeat_up && ms_since_rx > HEARTBEAT_TIMEOUT_MS) {
        syslog_write(LOG_WARNING, "Cluster: Heartbeat lost (last rx: %.0f ms ago)",
            ms_since_rx);
        cluster.heartbeat_up = false;

        /*
         * Heartbeat lost — if we're STANDBY, we need to determine
         * if the ACTIVE node has truly failed or if this is a
         * heartbeat link failure (which could cause split-brain).
         */
        if (cluster.local_role == CLUSTER_ROLE_STANDBY) {
            syslog_write(LOG_WARNING, "Cluster: STANDBY node lost heartbeat. "
                "Assuming ACTIVE node failed. Promoting to ACTIVE.");
            cluster.local_role = CLUSTER_ROLE_ACTIVE;
            cluster_activate_virtual_ips();
            cluster_activate_mac_tables();
        }
    }

    /* Check for split-brain: both nodes claim ACTIVE */
    if (cluster.local_role == CLUSTER_ROLE_ACTIVE &&
        cluster.peer_role == CLUSTER_ROLE_ACTIVE) {

        if (!cluster.split_brain_detected) {
            syslog_write(LOG_CRIT, "CLUSTER SPLIT-BRAIN DETECTED: "
                "Both nodes active! Cluster ID: %d", cluster.cluster_id);
            cluster.split_brain_detected = true;

            /* Attempt auto-recovery if enabled (v3.2.0+) */
            if (cluster.auto_recovery_enabled) {
                cluster_auto_resolve_split_brain();
            }
        }
    }

    /* Send heartbeat to peer */
    heartbeat_msg_t msg = {
        .cluster_id = cluster.cluster_id,
        .sender_role = cluster.local_role,
        .timestamp = now,
    };
    strncpy(msg.sender_serial, cluster.local_serial, sizeof(msg.sender_serial) - 1);
    heartbeat_send(&msg);
    cluster.last_heartbeat_tx = now;

    pthread_mutex_unlock(&cluster.state_lock);
    return 0;
}

/*
 * cluster_heartbeat_received - Process incoming heartbeat from peer
 */
int cluster_heartbeat_received(const heartbeat_msg_t *msg)
{
    pthread_mutex_lock(&cluster.state_lock);

    cluster.last_heartbeat_rx = time(NULL);
    cluster.heartbeat_up = true;
    cluster.peer_role = msg->sender_role;
    strncpy(cluster.peer_serial, msg->sender_serial, sizeof(cluster.peer_serial) - 1);

    /* If split-brain was detected and heartbeat is back, log recovery opportunity */
    if (cluster.split_brain_detected && cluster.heartbeat_up) {
        syslog_write(LOG_INFO, "Cluster: Heartbeat restored during split-brain. "
            "Manual or auto recovery can proceed.");
    }

    pthread_mutex_unlock(&cluster.state_lock);
    return 0;
}

/*
 * cluster_auto_resolve_split_brain - Automatic split-brain resolution
 *
 * Uses election policy to determine which node becomes STANDBY.
 * Available in v3.2.0+ when 'cluster split-brain-recovery automatic' is configured.
 */
static int cluster_auto_resolve_split_brain(void)
{
    syslog_write(LOG_INFO, "Cluster: Auto-resolving split-brain using policy: %s",
        cluster.election_policy == ELECTION_PRIORITY_SERIAL ? "serial-number" : "uptime");

    bool should_demote = false;

    switch (cluster.election_policy) {
        case ELECTION_PRIORITY_SERIAL:
            /* Higher serial number becomes STANDBY */
            should_demote = (strcmp(cluster.local_serial, cluster.peer_serial) > 0);
            break;
        case ELECTION_PRIORITY_UPTIME:
            /* Lower uptime becomes STANDBY (newer boot = likely recovered node) */
            should_demote = (get_system_uptime() < get_peer_uptime());
            break;
    }

    if (should_demote) {
        syslog_write(LOG_WARNING, "Cluster: Auto-demoting local node to STANDBY "
            "(serial: %s > peer: %s)",
            cluster.local_serial, cluster.peer_serial);
        cluster.local_role = CLUSTER_ROLE_STANDBY;
        cluster_release_virtual_ips();
        cluster_flush_mac_tables();
        cluster.split_brain_detected = false;
    } else {
        syslog_write(LOG_INFO, "Cluster: Local node remains ACTIVE "
            "(serial: %s <= peer: %s). Waiting for peer to demote.",
            cluster.local_serial, cluster.peer_serial);
    }

    return 0;
}

/*
 * cluster_force_role - CLI command to force cluster role
 */
int cluster_force_role(uint8_t role)
{
    pthread_mutex_lock(&cluster.state_lock);

    syslog_write(LOG_WARNING, "Cluster: Forcing role to %s (operator command)",
        role == CLUSTER_ROLE_ACTIVE ? "ACTIVE" : "STANDBY");

    if (role == CLUSTER_ROLE_STANDBY) {
        cluster_release_virtual_ips();
        cluster_flush_mac_tables();
    } else if (role == CLUSTER_ROLE_ACTIVE) {
        cluster_activate_virtual_ips();
        cluster_activate_mac_tables();
    }

    cluster.local_role = role;
    cluster.split_brain_detected = false;

    pthread_mutex_unlock(&cluster.state_lock);
    return 0;
}

/*
 * cluster_get_status - Get cluster status for show commands and API
 */
int cluster_get_status(cluster_status_t *status)
{
    if (!status) return -1;

    pthread_mutex_lock(&cluster.state_lock);

    status->cluster_id = cluster.cluster_id;
    status->local_role = cluster.local_role;
    status->peer_role = cluster.peer_role;
    status->heartbeat_up = cluster.heartbeat_up;
    status->split_brain = cluster.split_brain_detected;
    status->last_heartbeat = cluster.last_heartbeat_rx;
    strncpy(status->local_serial, cluster.local_serial, sizeof(status->local_serial) - 1);
    strncpy(status->peer_serial, cluster.peer_serial, sizeof(status->peer_serial) - 1);

    pthread_mutex_unlock(&cluster.state_lock);
    return 0;
}
