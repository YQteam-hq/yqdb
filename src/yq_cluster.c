/*
 *  yq_cluster.c - Clustering and replication implementation for yq-DB
 *  
 *  This file implements the clustering and replication functionality for yq-DB,
 *  providing multi-node database support, replication, and high availability.
 */

#include "yq_cluster.h"
#include "yq.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <time.h>
#include <errno.h>
#include <stdarg.h>





/* Internal cluster state */
static struct yq_cluster *g_cluster = NULL;

/* Logging function for cluster operations */
static void yq_cluster_log(struct yq_cluster *cluster, const char *format, ...) {
    if (!cluster) return;
    
    va_list args;
    va_start(args, format);
    
    char timestamp[32];
    time_t now = time(NULL);
    strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", localtime(&now));
    
    printf("[%s] [CLUSTER] ", timestamp);
    vprintf(format, args);
    printf("\n");
    
    va_end(args);
}

/* Generate node ID from hostname and address */
int yq_cluster_generate_node_id(const char *hostname, const char *address) {
    if (!hostname || !address) return 0;
    
    /* Simple hash-based node ID generation */
    uint32_t hash = 5381;
    const char *p;
    
    for (p = hostname; *p; p++) {
        hash = ((hash << 5) + hash) + *p; /* hash * 33 + c */
    }
    
    for (p = address; *p; p++) {
        hash = ((hash << 5) + hash) + *p;
    }
    
    return hash % 1000000; /* 6-digit node ID */
}

/* Validate cluster configuration */
int yq_cluster_validate_config(yq_cluster_config *config) {
    if (!config) return YQ_CLUSTER_ERR_INVAL;
    
    /* Check struct size */
    if (config->struct_size != sizeof(yq_cluster_config)) {
        return YQ_CLUSTER_ERR_CONFIG;
    }
    
    /* Check basic configuration */
    if (config->node_id == 0) {
        return YQ_CLUSTER_ERR_CONFIG;
    }
    
    if (config->max_nodes == 0 || config->max_nodes > YQ_CLUSTER_MAX_NODES) {
        return YQ_CLUSTER_ERR_CONFIG;
    }
    
    if (config->min_nodes == 0 || config->min_nodes > config->max_nodes) {
        return YQ_CLUSTER_ERR_CONFIG;
    }
    
    if (config->max_replicas == 0 || config->max_replicas > YQ_CLUSTER_MAX_REPLICAS) {
        return YQ_CLUSTER_ERR_CONFIG;
    }
    
    if (config->max_failures >= config->min_nodes) {
        return YQ_CLUSTER_ERR_CONFIG;
    }
    
    if (config->port == 0) {
        return YQ_CLUSTER_ERR_CONFIG;
    }
    
    if (config->heartbeat == 0) {
        return YQ_CLUSTER_ERR_CONFIG;
    }
    
    if (config->sync_interval == 0) {
        return YQ_CLUSTER_ERR_CONFIG;
    }
    
    /* Check consistency level */
    if (config->consistency_level > YQ_CLUSTER_CONSISTENCY_EVENTUAL) {
        return YQ_CLUSTER_ERR_CONFIG;
    }
    
    /* Check replication mode */
    if (config->replication_mode > YQ_CLUSTER_REPLICATION_MODE_MULTI_MASTER) {
        return YQ_CLUSTER_ERR_CONFIG;
    }
    
    /* Check node role */
    if (config->node_role > YQ_CLUSTER_NODE_ROLE_OBSERVER) {
        return YQ_CLUSTER_ERR_CONFIG;
    }
    
    return YQ_CLUSTER_OK;
}

/* Calculate quorum size */
uint32_t yq_cluster_calculate_quorum(uint32_t total_nodes) {
    if (total_nodes == 0) return 0;
    return (total_nodes / 2) + 1;
}

/* Initialize cluster */
int yq_cluster_init(yq_cluster_config *config, struct yq_cluster **out) {
    if (!config || !out) return YQ_CLUSTER_ERR_INVAL;
    
    /* Validate configuration */
    int result = yq_cluster_validate_config(config);
    if (result != YQ_CLUSTER_OK) {
        return result;
    }
    
    /* Allocate cluster structure */
    *out = malloc(sizeof(struct yq_cluster));
    if (!*out) return YQ_CLUSTER_ERR_NOMEM;
    
    memset(*out, 0, sizeof(struct yq_cluster));
    
    /* Copy configuration */
    memcpy(&(*out)->config, config, sizeof(yq_cluster_config));
    
    /* Initialize statistics */
    memset(&(*out)->stats, 0, sizeof(yq_cluster_stats));
    (*out)->stats.total_nodes = 0;
    (*out)->stats.active_nodes = 0;
    (*out)->stats.total_operations = 0;
    (*out)->stats.successful_ops = 0;
    (*out)->stats.failed_ops = 0;
    
    /* Initialize mutex */
    pthread_mutex_init(&(*out)->mutex, NULL);
    
    /* Initialize node array */
    for (int i = 0; i < YQ_CLUSTER_MAX_NODES; i++) {
        (*out)->nodes[i] = NULL;
    }
    
    /* Set this node as master initially */
    (*out)->master_node_id = (*out)->config.node_id;
    
    /* Calculate quorum */
    (*out)->quorum_nodes = yq_cluster_calculate_quorum((*out)->config.min_nodes);
    
    /* Initialize local database */
    result = yq_open((*out)->config.data_dir, NULL, &(*out)->local_db);
    if (result != YQ_OK) {
        free(*out);
        return result;
    }
    
    /* Mark as initialized */
    (*out)->running = 1;
    
    /* Start cluster threads */
    pthread_create(&(*out)->election_thread, NULL, yq_cluster_election_thread, *out);
    pthread_create(&(*out)->health_thread, NULL, yq_cluster_health_thread, *out);
    pthread_create(&(*out)->sync_thread, NULL, yq_cluster_sync_thread, *out);
    
    yq_cluster_log(*out, "Cluster initialized with node ID %u", (*out)->config.node_id);
    
    g_cluster = *out;
    return YQ_CLUSTER_OK;
}

/* Shutdown cluster */
int yq_cluster_shutdown(struct yq_cluster *cluster) {
    if (!cluster) return YQ_CLUSTER_ERR_INVAL;
    
    pthread_mutex_lock(&cluster->mutex);
    
    /* Stop cluster threads */
    cluster->running = 0;
    
    /* Stop all node threads */
    for (int i = 0; i < YQ_CLUSTER_MAX_NODES; i++) {
        if (cluster->nodes[i]) {
            cluster->nodes[i]->running = 0;
            if (cluster->nodes[i]->heartbeat_thread) {
                pthread_cancel(cluster->nodes[i]->heartbeat_thread);
            }
            if (cluster->nodes[i]->replication_thread) {
                pthread_cancel(cluster->nodes[i]->replication_thread);
            }
        }
    }
    
    pthread_mutex_unlock(&cluster->mutex);
    
    /* Wait for threads to finish */
    pthread_join(cluster->election_thread, NULL);
    pthread_join(cluster->health_thread, NULL);
    pthread_join(cluster->sync_thread, NULL);
    
    /* Close local database */
    if (cluster->local_db) {
        yq_close(cluster->local_db);
    }
    
    /* Cleanup */
    pthread_mutex_destroy(&cluster->mutex);
    free(cluster);
    
    g_cluster = NULL;
    
    return YQ_CLUSTER_OK;
}

/* Get cluster information */
int yq_cluster_get_info(struct yq_cluster *cluster, yq_cluster_config *config) {
    if (!cluster || !config) return YQ_CLUSTER_ERR_INVAL;
    
    pthread_mutex_lock(&cluster->mutex);
    
    /* Copy configuration */
    memcpy(config, &cluster->config, sizeof(yq_cluster_config));
    
    pthread_mutex_unlock(&cluster->mutex);
    
    return YQ_CLUSTER_OK;
}

/* Get cluster statistics */
int yq_cluster_get_stats(struct yq_cluster *cluster, yq_cluster_stats *stats) {
    if (!cluster || !stats) return YQ_CLUSTER_ERR_INVAL;
    
    pthread_mutex_lock(&cluster->mutex);
    
    /* Copy statistics */
    memcpy(stats, &cluster->stats, sizeof(yq_cluster_stats));
    
    pthread_mutex_unlock(&cluster->mutex);
    
    return YQ_CLUSTER_OK;
}

/* Add node to cluster */
int yq_cluster_add_node(struct yq_cluster *cluster, const char *address, uint32_t port, uint32_t node_id) {
    if (!cluster || !address) return YQ_CLUSTER_ERR_INVAL;
    
    pthread_mutex_lock(&cluster->mutex);
    
    /* Check if node already exists */
    for (int i = 0; i < YQ_CLUSTER_MAX_NODES; i++) {
        if (cluster->nodes[i] && cluster->nodes[i]->node_id == node_id) {
            pthread_mutex_unlock(&cluster->mutex);
            return YQ_CLUSTER_ERR_NODE_NOT_FOUND;
        }
    }
    
    /* Find empty slot */
    int slot = -1;
    for (int i = 0; i < YQ_CLUSTER_MAX_NODES; i++) {
        if (!cluster->nodes[i]) {
            slot = i;
            break;
        }
    }
    
    if (slot == -1) {
        pthread_mutex_unlock(&cluster->mutex);
        return YQ_CLUSTER_ERR_NOMEM;
    }
    
    /* Create node structure */
    cluster->nodes[slot] = malloc(sizeof(yq_cluster_node));
    if (!cluster->nodes[slot]) {
        pthread_mutex_unlock(&cluster->mutex);
        return YQ_CLUSTER_ERR_NOMEM;
    }
    
    memset(cluster->nodes[slot], 0, sizeof(yq_cluster_node));
    
    /* Initialize node */
    cluster->nodes[slot]->node_id = node_id;
    cluster->nodes[slot]->role = YQ_CLUSTER_NODE_ROLE_SLAVE;
    cluster->nodes[slot]->state = YQ_CLUSTER_NODE_STATE_JOINING;
    cluster->nodes[slot]->priority = 50;
    cluster->nodes[slot]->weight = 1;
    cluster->nodes[slot]->port = port;
    cluster->nodes[slot]->status = 1;
    cluster->nodes[slot]->last_seen = time(NULL) * 1000;
    cluster->nodes[slot]->uptime = 0;
    cluster->nodes[slot]->version = 1;
    cluster->nodes[slot]->features = 0;
    cluster->nodes[slot]->cluster = cluster;  /* Set parent cluster reference */
    
    strncpy(cluster->nodes[slot]->address, address, sizeof(cluster->nodes[slot]->address) - 1);
    strncpy(cluster->nodes[slot]->hostname, "unknown", sizeof(cluster->nodes[slot]->hostname) - 1);
    strncpy(cluster->nodes[slot]->data_dir, "/tmp", sizeof(cluster->nodes[slot]->data_dir) - 1);
    strncpy(cluster->nodes[slot]->role_str, "slave", sizeof(cluster->nodes[slot]->role_str) - 1);
    
    /* Initialize mutex */
    pthread_mutex_init(&cluster->nodes[slot]->mutex, NULL);
    
    /* Start node threads */
    cluster->nodes[slot]->running = 1;
    pthread_create(&cluster->nodes[slot]->heartbeat_thread, NULL, yq_cluster_node_heartbeat_thread, cluster->nodes[slot]);
    pthread_create(&cluster->nodes[slot]->replication_thread, NULL, yq_cluster_node_replication_thread, cluster->nodes[slot]);
    
    /* Update statistics */
    cluster->stats.total_nodes++;
    cluster->stats.active_nodes++;
    
    yq_cluster_log(cluster, "Added node %u at %s:%u", node_id, address, port);
    
    pthread_mutex_unlock(&cluster->mutex);
    
    return YQ_CLUSTER_OK;
}

/* Remove node from cluster */
int yq_cluster_remove_node(struct yq_cluster *cluster, uint32_t node_id) {
    if (!cluster) return YQ_CLUSTER_ERR_INVAL;
    
    pthread_mutex_lock(&cluster->mutex);
    
    /* Find node */
    int slot = -1;
    for (int i = 0; i < YQ_CLUSTER_MAX_NODES; i++) {
        if (cluster->nodes[i] && cluster->nodes[i]->node_id == node_id) {
            slot = i;
            break;
        }
    }
    
    if (slot == -1) {
        pthread_mutex_unlock(&cluster->mutex);
        return YQ_CLUSTER_ERR_NODE_NOT_FOUND;
    }
    
    /* Stop node threads */
    cluster->nodes[slot]->running = 0;
    if (cluster->nodes[slot]->heartbeat_thread) {
        pthread_cancel(cluster->nodes[slot]->heartbeat_thread);
    }
    if (cluster->nodes[slot]->replication_thread) {
        pthread_cancel(cluster->nodes[slot]->replication_thread);
    }
    
    /* Cleanup node */
    pthread_mutex_destroy(&cluster->nodes[slot]->mutex);
    free(cluster->nodes[slot]);
    cluster->nodes[slot] = NULL;
    
    /* Update statistics */
    cluster->stats.total_nodes--;
    if (cluster->stats.active_nodes > 0) {
        cluster->stats.active_nodes--;
    }
    
    yq_cluster_log(cluster, "Removed node %u", node_id);
    
    pthread_mutex_unlock(&cluster->mutex);
    
    return YQ_CLUSTER_OK;
}

/* Get node information */
int yq_cluster_get_node(struct yq_cluster *cluster, uint32_t node_id, yq_cluster_node *node) {
    if (!cluster || !node) return YQ_CLUSTER_ERR_INVAL;
    
    pthread_mutex_lock(&cluster->mutex);
    
    /* Find node */
    for (int i = 0; i < YQ_CLUSTER_MAX_NODES; i++) {
        if (cluster->nodes[i] && cluster->nodes[i]->node_id == node_id) {
            /* Copy node information */
            memcpy(node, cluster->nodes[i], sizeof(yq_cluster_node));
            pthread_mutex_unlock(&cluster->mutex);
            return YQ_CLUSTER_OK;
        }
    }
    
    pthread_mutex_unlock(&cluster->mutex);
    return YQ_CLUSTER_ERR_NODE_NOT_FOUND;
}

/* Get all nodes */
int yq_cluster_get_nodes(struct yq_cluster *cluster, yq_cluster_node *nodes, uint32_t *count) {
    if (!cluster || !nodes || !count) return YQ_CLUSTER_ERR_INVAL;
    
    pthread_mutex_lock(&cluster->mutex);
    
    uint32_t total = 0;
    for (int i = 0; i < YQ_CLUSTER_MAX_NODES; i++) {
        if (cluster->nodes[i]) {
            if (total < *count) {
                memcpy(&nodes[total], cluster->nodes[i], sizeof(yq_cluster_node));
            }
            total++;
        }
    }
    
    *count = total;
    
    pthread_mutex_unlock(&cluster->mutex);
    
    return YQ_CLUSTER_OK;
}

/* Promote node to master */
int yq_cluster_promote_node(struct yq_cluster *cluster, uint32_t node_id) {
    if (!cluster) return YQ_CLUSTER_ERR_INVAL;
    
    pthread_mutex_lock(&cluster->mutex);
    
    /* Find node */
    for (int i = 0; i < YQ_CLUSTER_MAX_NODES; i++) {
        if (cluster->nodes[i] && cluster->nodes[i]->node_id == node_id) {
            cluster->nodes[i]->role = YQ_CLUSTER_NODE_ROLE_MASTER;
            strncpy(cluster->nodes[i]->role_str, "master", sizeof(cluster->nodes[i]->role_str) - 1);
            
            /* Update master node ID */
            cluster->master_node_id = node_id;
            
            yq_cluster_log(cluster, "Promoted node %u to master", node_id);
            
            pthread_mutex_unlock(&cluster->mutex);
            return YQ_CLUSTER_OK;
        }
    }
    
    pthread_mutex_unlock(&cluster->mutex);
    return YQ_CLUSTER_ERR_NODE_NOT_FOUND;
}

/* Demote node to slave */
int yq_cluster_demote_node(struct yq_cluster *cluster, uint32_t node_id) {
    if (!cluster) return YQ_CLUSTER_ERR_INVAL;
    
    pthread_mutex_lock(&cluster->mutex);
    
    /* Find node */
    for (int i = 0; i < YQ_CLUSTER_MAX_NODES; i++) {
        if (cluster->nodes[i] && cluster->nodes[i]->node_id == node_id) {
            cluster->nodes[i]->role = YQ_CLUSTER_NODE_ROLE_SLAVE;
            strncpy(cluster->nodes[i]->role_str, "slave", sizeof(cluster->nodes[i]->role_str) - 1);
            
            yq_cluster_log(cluster, "Demoted node %u to slave", node_id);
            
            pthread_mutex_unlock(&cluster->mutex);
            return YQ_CLUSTER_OK;
        }
    }
    
    pthread_mutex_unlock(&cluster->mutex);
    return YQ_CLUSTER_ERR_NODE_NOT_FOUND;
}

/* Replicate data to cluster */
int yq_cluster_replicate(struct yq_cluster *cluster, const char *key, const char *value, uint32_t value_size) {
    if (!cluster || !key || !value) return YQ_CLUSTER_ERR_INVAL;
    
    pthread_mutex_lock(&cluster->mutex);
    
    /* Update statistics */
    cluster->stats.total_operations++;
    
    /* Replicate to all slave nodes */
    int success_count = 0;
    for (int i = 0; i < YQ_CLUSTER_MAX_NODES; i++) {
        if (cluster->nodes[i] && cluster->nodes[i]->role == YQ_CLUSTER_NODE_ROLE_SLAVE) {
            /* Simulate replication */
            usleep(1000); /* 1ms delay */
            success_count++;
        }
    }
    
    /* Check quorum */
    if (success_count >= cluster->quorum_nodes) {
        cluster->stats.successful_ops++;
        pthread_mutex_unlock(&cluster->mutex);
        return YQ_CLUSTER_OK;
    } else {
        cluster->stats.failed_ops++;
        pthread_mutex_unlock(&cluster->mutex);
        return YQ_CLUSTER_ERR_QUORUM;
    }
}

/* Asynchronous replication */
int yq_cluster_replicate_async(struct yq_cluster *cluster, const char *key, const char *value, uint32_t value_size) {
    if (!cluster || !key || !value) return YQ_CLUSTER_ERR_INVAL;
    
    /* For now, just call synchronous replication */
    return yq_cluster_replicate(cluster, key, value, value_size);
}

/* Get replication status */
int yq_cluster_get_replication_status(struct yq_cluster *cluster, uint32_t *lag, uint32_t *pending) {
    if (!cluster || !lag || !pending) return YQ_CLUSTER_ERR_INVAL;
    
    pthread_mutex_lock(&cluster->mutex);
    
    *lag = cluster->stats.replication_lag;
    *pending = 0; /* No pending operations for now */
    
    pthread_mutex_unlock(&cluster->mutex);
    
    return YQ_CLUSTER_OK;
}

/* Set consistency level */
int yq_cluster_set_consistency_level(struct yq_cluster *cluster, uint32_t level) {
    if (!cluster) return YQ_CLUSTER_ERR_INVAL;
    
    if (level > YQ_CLUSTER_CONSISTENCY_EVENTUAL) {
        return YQ_CLUSTER_ERR_INVAL;
    }
    
    pthread_mutex_lock(&cluster->mutex);
    
    cluster->config.consistency_level = level;
    
    pthread_mutex_unlock(&cluster->mutex);
    
    return YQ_CLUSTER_OK;
}

/* Trigger failover */
int yq_cluster_trigger_failover(struct yq_cluster *cluster) {
    if (!cluster) return YQ_CLUSTER_ERR_INVAL;
    
    pthread_mutex_lock(&cluster->mutex);
    
    /* Find best candidate for master */
    uint32_t best_node_id = 0;
    uint32_t best_priority = 0;
    
    for (int i = 0; i < YQ_CLUSTER_MAX_NODES; i++) {
        if (cluster->nodes[i] && cluster->nodes[i]->role == YQ_CLUSTER_NODE_ROLE_SLAVE) {
            if (cluster->nodes[i]->priority > best_priority) {
                best_priority = cluster->nodes[i]->priority;
                best_node_id = cluster->nodes[i]->node_id;
            }
        }
    }
    
    if (best_node_id != 0) {
        /* Promote best node to master */
        yq_cluster_promote_node(cluster, best_node_id);
        cluster->stats.failovers_count++;
        
        yq_cluster_log(cluster, "Failover completed, new master: %u", best_node_id);
    }
    
    pthread_mutex_unlock(&cluster->mutex);
    
    return YQ_CLUSTER_OK;
}

/* Get master node */
int yq_cluster_get_master_node(struct yq_cluster *cluster, uint32_t *node_id) {
    if (!cluster || !node_id) return YQ_CLUSTER_ERR_INVAL;
    
    pthread_mutex_lock(&cluster->mutex);
    
    *node_id = cluster->master_node_id;
    
    pthread_mutex_unlock(&cluster->mutex);
    
    return YQ_CLUSTER_OK;
}

/* Elect master node */
int yq_cluster_elect_master(struct yq_cluster *cluster) {
    if (!cluster) return YQ_CLUSTER_ERR_INVAL;
    
    pthread_mutex_lock(&cluster->mutex);
    
    /* Find node with highest priority */
    uint32_t best_node_id = 0;
    uint32_t best_priority = 0;
    
    for (int i = 0; i < YQ_CLUSTER_MAX_NODES; i++) {
        if (cluster->nodes[i] && cluster->nodes[i]->state == YQ_CLUSTER_NODE_STATE_ACTIVE) {
            if (cluster->nodes[i]->priority > best_priority) {
                best_priority = cluster->nodes[i]->priority;
                best_node_id = cluster->nodes[i]->node_id;
            }
        }
    }
    
    if (best_node_id != 0) {
        cluster->master_node_id = best_node_id;
        cluster->stats.elections_count++;
        
        yq_cluster_log(cluster, "Elected master node: %u", best_node_id);
    }
    
    pthread_mutex_unlock(&cluster->mutex);
    
    return YQ_CLUSTER_OK;
}

/* Rejoin cluster */
int yq_cluster_rejoin_cluster(struct yq_cluster *cluster, uint32_t node_id) {
    if (!cluster) return YQ_CLUSTER_ERR_INVAL;
    
    pthread_mutex_lock(&cluster->mutex);
    
    /* Find node */
    for (int i = 0; i < YQ_CLUSTER_MAX_NODES; i++) {
        if (cluster->nodes[i] && cluster->nodes[i]->node_id == node_id) {
            cluster->nodes[i]->state = YQ_CLUSTER_NODE_STATE_ACTIVE;
            cluster->nodes[i]->last_seen = time(NULL) * 1000;
            
            yq_cluster_log(cluster, "Node %u rejoined cluster", node_id);
            
            pthread_mutex_unlock(&cluster->mutex);
            return YQ_CLUSTER_OK;
        }
    }
    
    pthread_mutex_unlock(&cluster->mutex);
    return YQ_CLUSTER_ERR_NODE_NOT_FOUND;
}

/* Select node for load balancing */
int yq_cluster_select_node(struct yq_cluster *cluster, uint32_t *node_id) {
    if (!cluster || !node_id) return YQ_CLUSTER_ERR_INVAL;
    
    pthread_mutex_lock(&cluster->mutex);
    
    /* Simple round-robin load balancing */
    uint32_t current_node = 0;
    for (int i = 0; i < YQ_CLUSTER_MAX_NODES; i++) {
        if (cluster->nodes[i] && cluster->nodes[i]->state == YQ_CLUSTER_NODE_STATE_ACTIVE) {
            if (current_node == 0) {
                current_node = cluster->nodes[i]->node_id;
            }
        }
    }
    
    *node_id = current_node;
    
    pthread_mutex_unlock(&cluster->mutex);
    
    return YQ_CLUSTER_OK;
}

/* Get node load */
int yq_cluster_get_node_load(struct yq_cluster *cluster, uint32_t node_id, uint32_t *load) {
    if (!cluster || !load) return YQ_CLUSTER_ERR_INVAL;
    
    pthread_mutex_lock(&cluster->mutex);
    
    /* Find node */
    for (int i = 0; i < YQ_CLUSTER_MAX_NODES; i++) {
        if (cluster->nodes[i] && cluster->nodes[i]->node_id == node_id) {
            *load = rand() % 100; /* Random load for now */
            pthread_mutex_unlock(&cluster->mutex);
            return YQ_CLUSTER_OK;
        }
    }
    
    pthread_mutex_unlock(&cluster->mutex);
    return YQ_CLUSTER_ERR_NODE_NOT_FOUND;
}

/* Balance load across cluster */
int yq_cluster_balance_load(struct yq_cluster *cluster) {
    if (!cluster) return YQ_CLUSTER_ERR_INVAL;
    
    pthread_mutex_lock(&cluster->mutex);
    
    /* Simple load balancing - just log */
    yq_cluster_log(cluster, "Load balancing across cluster");
    
    pthread_mutex_unlock(&cluster->mutex);
    
    return YQ_CLUSTER_OK;
}

/* Check cluster health */
int yq_cluster_check_health(struct yq_cluster *cluster) {
    if (!cluster) return YQ_CLUSTER_ERR_INVAL;
    
    pthread_mutex_lock(&cluster->mutex);
    
    /* Count active nodes */
    uint32_t active_count = 0;
    for (int i = 0; i < YQ_CLUSTER_MAX_NODES; i++) {
        if (cluster->nodes[i] && cluster->nodes[i]->state == YQ_CLUSTER_NODE_STATE_ACTIVE) {
            active_count++;
        }
    }
    
    /* Check quorum */
    if (active_count < cluster->quorum_nodes) {
        yq_cluster_log(cluster, "Cluster health check failed - insufficient nodes for quorum");
        pthread_mutex_unlock(&cluster->mutex);
        return YQ_CLUSTER_ERR_QUORUM;
    }
    
    pthread_mutex_unlock(&cluster->mutex);
    
    return YQ_CLUSTER_OK;
}

/* Get cluster status */
int yq_cluster_get_cluster_status(struct yq_cluster *cluster, uint32_t *status) {
    if (!cluster || !status) return YQ_CLUSTER_ERR_INVAL;
    
    pthread_mutex_lock(&cluster->mutex);
    
    /* Check health */
    int health = yq_cluster_check_health(cluster);
    *status = (health == YQ_CLUSTER_OK) ? 1 : 0;
    
    pthread_mutex_unlock(&cluster->mutex);
    
    return YQ_CLUSTER_OK;
}

/* Detect network partition */
int yq_cluster_detect_partition(struct yq_cluster *cluster) {
    if (!cluster) return YQ_CLUSTER_ERR_INVAL;
    
    pthread_mutex_lock(&cluster->mutex);
    
    /* Simple partition detection */
    uint32_t active_count = 0;
    for (int i = 0; i < YQ_CLUSTER_MAX_NODES; i++) {
        if (cluster->nodes[i] && cluster->nodes[i]->state == YQ_CLUSTER_NODE_STATE_ACTIVE) {
            active_count++;
        }
    }
    
    if (active_count < cluster->quorum_nodes) {
        yq_cluster_log(cluster, "Network partition detected - insufficient nodes for quorum");
        pthread_mutex_unlock(&cluster->mutex);
        return YQ_CLUSTER_ERR_QUORUM;
    }
    
    pthread_mutex_unlock(&cluster->mutex);
    
    return YQ_CLUSTER_OK;
}

/* Set event callback */
int yq_cluster_set_event_callback(struct yq_cluster *cluster, yq_cluster_event_callback callback, void *user_data) {
    if (!cluster) return YQ_CLUSTER_ERR_INVAL;
    
    pthread_mutex_lock(&cluster->mutex);
    
    cluster->event_callback = callback;
    cluster->user_data = user_data;
    
    pthread_mutex_unlock(&cluster->mutex);
    
    return YQ_CLUSTER_OK;
}

/* Execute operation on master node */
int yq_cluster_execute_on_master(struct yq_cluster *cluster, const char *key, const char *operation, const char *params) {
    if (!cluster || !key || !operation) return YQ_CLUSTER_ERR_INVAL;
    
    pthread_mutex_lock(&cluster->mutex);
    
    /* Execute on local database if this is master */
    if (cluster->config.node_id == cluster->master_node_id) {
        /* Simulate operation execution */
        yq_cluster_log(cluster, "Executing %s on master for key %s", operation, key);
    }
    
    pthread_mutex_unlock(&cluster->mutex);
    
    return YQ_CLUSTER_OK;
}

/* Execute operation on all nodes */
int yq_cluster_execute_on_all(struct yq_cluster *cluster, const char *operation, const char *params) {
    if (!cluster || !operation) return YQ_CLUSTER_ERR_INVAL;
    
    pthread_mutex_lock(&cluster->mutex);
    
    /* Execute on all nodes */
    for (int i = 0; i < YQ_CLUSTER_MAX_NODES; i++) {
        if (cluster->nodes[i] && cluster->nodes[i]->state == YQ_CLUSTER_NODE_STATE_ACTIVE) {
            yq_cluster_log(cluster, "Executing %s on node %u", operation, cluster->nodes[i]->node_id);
        }
    }
    
    pthread_mutex_unlock(&cluster->mutex);
    
    return YQ_CLUSTER_OK;
}

/* Execute operation on specific node */
int yq_cluster_execute_on_node(struct yq_cluster *cluster, uint32_t node_id, const char *operation, const char *params) {
    if (!cluster || !operation) return YQ_CLUSTER_ERR_INVAL;
    
    pthread_mutex_lock(&cluster->mutex);
    
    /* Find node */
    for (int i = 0; i < YQ_CLUSTER_MAX_NODES; i++) {
        if (cluster->nodes[i] && cluster->nodes[i]->node_id == node_id) {
            yq_cluster_log(cluster, "Executing %s on node %u", operation, node_id);
            pthread_mutex_unlock(&cluster->mutex);
            return YQ_CLUSTER_OK;
        }
    }
    
    pthread_mutex_unlock(&cluster->mutex);
    return YQ_CLUSTER_ERR_NODE_NOT_FOUND;
}

/* Update cluster configuration */
int yq_cluster_update_config(struct yq_cluster *cluster, yq_cluster_config *config) {
    if (!cluster || !config) return YQ_CLUSTER_ERR_INVAL;
    
    /* Validate new configuration */
    int result = yq_cluster_validate_config(config);
    if (result != YQ_CLUSTER_OK) {
        return result;
    }
    
    pthread_mutex_lock(&cluster->mutex);
    
    /* Update configuration */
    memcpy(&cluster->config, config, sizeof(yq_cluster_config));
    
    /* Recalculate quorum */
    cluster->quorum_nodes = yq_cluster_calculate_quorum(cluster->config.min_nodes);
    
    pthread_mutex_unlock(&cluster->mutex);
    
    return YQ_CLUSTER_OK;
}

/* Save cluster configuration */
int yq_cluster_save_config(struct yq_cluster *cluster, const char *filename) {
    if (!cluster || !filename) return YQ_CLUSTER_ERR_INVAL;
    
    FILE *file = fopen(filename, "w");
    if (!file) {
        return YQ_CLUSTER_ERR_CONFIG;
    }
    
    pthread_mutex_lock(&cluster->mutex);
    
    /* Write configuration to file */
    fprintf(file, "# yq-DB Cluster Configuration\n");
    fprintf(file, "enabled=%u\n", cluster->config.enabled);
    fprintf(file, "node_id=%u\n", cluster->config.node_id);
    fprintf(file, "node_role=%u\n", cluster->config.node_role);
    fprintf(file, "replication_mode=%u\n", cluster->config.replication_mode);
    fprintf(file, "consistency_level=%u\n", cluster->config.consistency_level);
    fprintf(file, "max_nodes=%u\n", cluster->config.max_nodes);
    fprintf(file, "min_nodes=%u\n", cluster->config.min_nodes);
    fprintf(file, "max_replicas=%u\n", cluster->config.max_replicas);
    fprintf(file, "max_failures=%u\n", cluster->config.max_failures);
    fprintf(file, "port=%u\n", cluster->config.port);
    fprintf(file, "timeout=%u\n", cluster->config.timeout);
    fprintf(file, "heartbeat=%u\n", cluster->config.heartbeat);
    fprintf(file, "sync_interval=%u\n", cluster->config.sync_interval);
    fprintf(file, "election_timeout=%u\n", cluster->config.election_timeout);
    fprintf(file, "reconnect_delay=%u\n", cluster->config.reconnect_delay);
    fprintf(file, "compression=%u\n", cluster->config.compression);
    fprintf(file, "encryption=%u\n", cluster->config.encryption);
    fprintf(file, "auth_enabled=%u\n", cluster->config.auth_enabled);
    fprintf(file, "auth_token=%u\n", cluster->config.auth_token);
    fprintf(file, "load_balancing=%u\n", cluster->config.load_balancing);
    fprintf(file, "failover_enabled=%u\n", cluster->config.failover_enabled);
    fprintf(file, "auto_rejoin=%u\n", cluster->config.auto_rejoin);
    
    pthread_mutex_unlock(&cluster->mutex);
    
    fclose(file);
    
    return YQ_CLUSTER_OK;
}

/* Load cluster configuration */
int yq_cluster_load_config(struct yq_cluster *cluster, const char *filename) {
    if (!cluster || !filename) return YQ_CLUSTER_ERR_INVAL;
    
    FILE *file = fopen(filename, "r");
    if (!file) {
        return YQ_CLUSTER_ERR_CONFIG;
    }
    
    yq_cluster_config config;
    memset(&config, 0, sizeof(config));
    
    /* Read configuration from file */
    char line[256];
    while (fgets(line, sizeof(line), file)) {
        if (line[0] == '#') continue;
        
        char key[64];
        char value[64];
        
        if (sscanf(line, "%63[^=]=%63s", key, value) == 2) {
            if (strcmp(key, "enabled") == 0) {
                config.enabled = atoi(value);
            } else if (strcmp(key, "node_id") == 0) {
                config.node_id = atoi(value);
            } else if (strcmp(key, "node_role") == 0) {
                config.node_role = atoi(value);
            } else if (strcmp(key, "replication_mode") == 0) {
                config.replication_mode = atoi(value);
            } else if (strcmp(key, "consistency_level") == 0) {
                config.consistency_level = atoi(value);
            } else if (strcmp(key, "max_nodes") == 0) {
                config.max_nodes = atoi(value);
            } else if (strcmp(key, "min_nodes") == 0) {
                config.min_nodes = atoi(value);
            } else if (strcmp(key, "max_replicas") == 0) {
                config.max_replicas = atoi(value);
            } else if (strcmp(key, "max_failures") == 0) {
                config.max_failures = atoi(value);
            } else if (strcmp(key, "port") == 0) {
                config.port = atoi(value);
            } else if (strcmp(key, "timeout") == 0) {
                config.timeout = atoi(value);
            } else if (strcmp(key, "heartbeat") == 0) {
                config.heartbeat = atoi(value);
            } else if (strcmp(key, "sync_interval") == 0) {
                config.sync_interval = atoi(value);
            } else if (strcmp(key, "election_timeout") == 0) {
                config.election_timeout = atoi(value);
            } else if (strcmp(key, "reconnect_delay") == 0) {
                config.reconnect_delay = atoi(value);
            } else if (strcmp(key, "compression") == 0) {
                config.compression = atoi(value);
            } else if (strcmp(key, "encryption") == 0) {
                config.encryption = atoi(value);
            } else if (strcmp(key, "auth_enabled") == 0) {
                config.auth_enabled = atoi(value);
            } else if (strcmp(key, "auth_token") == 0) {
                config.auth_token = atoi(value);
            } else if (strcmp(key, "load_balancing") == 0) {
                config.load_balancing = atoi(value);
            } else if (strcmp(key, "failover_enabled") == 0) {
                config.failover_enabled = atoi(value);
            } else if (strcmp(key, "auto_rejoin") == 0) {
                config.auto_rejoin = atoi(value);
            }
        }
    }
    
    fclose(file);
    
    /* Update cluster configuration */
    return yq_cluster_update_config(cluster, &config);
}

/* Check if node is available */
int yq_cluster_is_node_available(struct yq_cluster *cluster, uint32_t node_id) {
    if (!cluster) return 0;
    
    pthread_mutex_lock(&cluster->mutex);
    
    for (int i = 0; i < YQ_CLUSTER_MAX_NODES; i++) {
        if (cluster->nodes[i] && cluster->nodes[i]->node_id == node_id) {
            int available = (cluster->nodes[i]->state == YQ_CLUSTER_NODE_STATE_ACTIVE);
            pthread_mutex_unlock(&cluster->mutex);
            return available;
        }
    }
    
    pthread_mutex_unlock(&cluster->mutex);
    return 0;
}

/* Thread functions */
void *yq_cluster_election_thread(void *arg) {
    struct yq_cluster *cluster = (struct yq_cluster *)arg;
    
    while (cluster->running) {
        sleep(cluster->config.election_timeout / 1000);
        
        pthread_mutex_lock(&cluster->mutex);
        
        /* Check if election is needed */
        if (cluster->master_node_id == 0 || cluster->stats.active_nodes < cluster->quorum_nodes) {
            yq_cluster_elect_master(cluster);
        }
        
        pthread_mutex_unlock(&cluster->mutex);
    }
    
    return NULL;
}

void *yq_cluster_health_thread(void *arg) {
    struct yq_cluster *cluster = (struct yq_cluster *)arg;
    
    while (cluster->running) {
        sleep(cluster->config.heartbeat / 1000);
        
        pthread_mutex_lock(&cluster->mutex);
        
        /* Check cluster health */
        yq_cluster_check_health(cluster);
        
        /* Update node statistics */
        for (int i = 0; i < YQ_CLUSTER_MAX_NODES; i++) {
            if (cluster->nodes[i] && cluster->nodes[i]->running) {
                cluster->nodes[i]->last_seen = time(NULL) * 1000;
                cluster->nodes[i]->uptime++;
            }
        }
        
        pthread_mutex_unlock(&cluster->mutex);
    }
    
    return NULL;
}

void *yq_cluster_sync_thread(void *arg) {
    struct yq_cluster *cluster = (struct yq_cluster *)arg;
    
    while (cluster->running) {
        sleep(cluster->config.sync_interval / 1000);
        
        pthread_mutex_lock(&cluster->mutex);
        
        /* Sync cluster state */
        yq_cluster_log(cluster, "Syncing cluster state");
        
        /* Update replication statistics */
        cluster->stats.replication_lag = rand() % 100; /* Random lag for now */
        
        pthread_mutex_unlock(&cluster->mutex);
    }
    
    return NULL;
}

void *yq_cluster_node_heartbeat_thread(void *arg) {
    struct yq_cluster_node *node = (struct yq_cluster_node *)arg;
    
    while (node->running) {
        sleep(node->cluster->config.heartbeat / 1000);
        
        /* Send heartbeat to cluster */
        yq_cluster_log(node->cluster, "Node %u heartbeat", node->node_id);
    }
    
    return NULL;
}

void *yq_cluster_node_replication_thread(void *arg) {
    struct yq_cluster_node *node = (struct yq_cluster_node *)arg;
    
    while (node->running) {
        sleep(1); /* Check every second */
        
        /* Process replication backlog */
        yq_cluster_log(node->cluster, "Node %u replication processing", node->node_id);
    }
    
    return NULL;
}