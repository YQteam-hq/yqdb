/*
 *  yq_cluster.h - Clustering and replication API for yq-DB
 *  
 *  This header defines the clustering and replication functionality for yq-DB,
 *  including support for multi-node databases, replication, and high availability.
 *  
 *  Features:
 *  - Multi-node cluster support
 *  - Asynchronous replication
 *  - Automatic failover
 *  - Load balancing
 *  - Consistency levels
 *  - Health monitoring
 *  - Node discovery
 *  - Cluster management
 *  
 *  Enable with: -DYQ_ENABLE_CLUSTER=1
 */

#ifndef YQ_CLUSTER_H
#define YQ_CLUSTER_H

#include <stdint.h>
#include <stddef.h>
#include <pthread.h>
#include "yq.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Cluster configuration macros */
#define YQ_CLUSTER_MAX_NODES         64
#define YQ_CLUSTER_MAX_REPLICAS      3
#define YQ_CLUSTER_MAX_FAILURES     3
#define YQ_CLUSTER_DEFAULT_PORT     9876
#define YQ_CLUSTER_DEFAULT_TIMEOUT  5000
#define YQ_CLUSTER_DEFAULT_HEARTBEAT 1000
#define YQ_CLUSTER_DEFAULT_SYNC_INTERVAL 5000

/* Cluster node states */
typedef enum yq_cluster_node_state {
    YQ_CLUSTER_NODE_STATE_UNKNOWN    = 0,
    YQ_CLUSTER_NODE_STATE_JOINING    = 1,
    YQ_CLUSTER_NODE_STATE_ACTIVE     = 2,
    YQ_CLUSTER_NODE_STATE_LEAVING    = 3,
    YQ_CLUSTER_NODE_STATE_DEAD       = 4,
    YQ_CLUSTER_NODE_STATE_SUSPECTED  = 5
} yq_cluster_node_state;

/* Cluster node roles */
typedef enum yq_cluster_node_role {
    YQ_CLUSTER_NODE_ROLE_MASTER     = 0,
    YQ_CLUSTER_NODE_ROLE_SLAVE      = 1,
    YQ_CLUSTER_NODE_ROLE_ARBITER    = 2,
    YQ_CLUSTER_NODE_ROLE_OBSERVER   = 3
} yq_cluster_node_role;

/* Cluster consistency levels */
typedef enum yq_cluster_consistency {
    YQ_CLUSTER_CONSISTENCY_ONE       = 0,  /* One node (local) */
    YQ_CLUSTER_CONSISTENCY_QUORUM    = 1,  /* Majority of nodes */
    YQ_CLUSTER_CONSISTENCY_ALL       = 2,  /* All nodes */
    YQ_CLUSTER_CONSISTENCY_EVENTUAL = 3   /* Eventually consistent */
} yq_cluster_consistency;

/* Cluster replication modes */
typedef enum yq_cluster_replication_mode {
    YQ_CLUSTER_REPLICATION_MODE_SYNC    = 0,  /* Synchronous replication */
    YQ_CLUSTER_REPLICATION_MODE_ASYNC   = 1,  /* Asynchronous replication */
    YQ_CLUSTER_REPLICATION_MODE_MASTERSLAVE = 2,  /* Master-slave replication */
    YQ_CLUSTER_REPLICATION_MODE_MULTI_MASTER = 3  /* Multi-master replication */
} yq_cluster_replication_mode;

/* Forward declarations */
struct yq_cluster;
struct yq_cluster_node;

/* Cluster node information */
typedef struct yq_cluster_node {
    uint32_t struct_size;      /* Must be sizeof(yq_cluster_node) */
    uint32_t node_id;          /* Unique node identifier */
    uint32_t role;             /* Node role (yq_cluster_node_role) */
    uint32_t state;            /* Node state (yq_cluster_node_state) */
    uint32_t priority;         /* Node priority (0-100, higher = higher priority) */
    uint32_t weight;           /* Node weight for load balancing */
    uint32_t port;             /* Node port */
    uint32_t status;           /* Node status flags */
    uint32_t last_seen;        /* Last seen timestamp (milliseconds) */
    uint32_t uptime;           /* Node uptime (seconds) */
    uint32_t version;          /* Node protocol version */
    uint32_t features;        /* Node feature flags */
    char hostname[64];        /* Node hostname */
    char address[128];         /* Node IP address */
    char data_dir[256];       /* Node data directory */
    char role_str[16];        /* Role string for display */
    uint32_t reserved[5];     /* Must be 0 */
    int running;              /* Whether node is running */
    struct yq_cluster *cluster;  /* Reference to parent cluster */
    pthread_mutex_t mutex;        /* Node mutex */
    pthread_t heartbeat_thread;  /* Node heartbeat thread */
    pthread_t replication_thread; /* Node replication thread */
} yq_cluster_node;

/* Cluster configuration */
typedef struct yq_cluster_config {
    uint32_t struct_size;      /* Must be sizeof(yq_cluster_config) */
    uint32_t enabled;          /* Whether clustering enabled */
    uint32_t node_id;          /* This node's ID */
    uint32_t node_role;        /* This node's role */
    uint32_t replication_mode;  /* Replication mode */
    uint32_t consistency_level; /* Consistency level */
    uint32_t max_nodes;        /* Maximum number of nodes */
    uint32_t min_nodes;        /* Minimum number of nodes for quorum */
    uint32_t max_replicas;     /* Maximum number of replicas */
    uint32_t max_failures;     /* Maximum tolerated failures */
    uint32_t port;              /* Cluster communication port */
    uint32_t timeout;          /* Operation timeout (milliseconds) */
    uint32_t heartbeat;        /* Heartbeat interval (milliseconds) */
    uint32_t sync_interval;    /* State sync interval (milliseconds) */
    uint32_t election_timeout; /* Election timeout (milliseconds) */
    uint32_t reconnect_delay;  /* Reconnect delay (milliseconds) */
    uint32_t compression;      /* Compression for network traffic */
    uint32_t encryption;       /* Encryption for network traffic */
    uint32_t auth_enabled;     /* Whether authentication enabled */
    uint32_t auth_token;       /* Authentication token */
    uint32_t load_balancing;   /* Load balancing algorithm */
    uint32_t failover_enabled; /* Whether automatic failover enabled */
    uint32_t auto_rejoin;      /* Whether auto-rejoin enabled */
    char data_dir[256];        /* Data directory path */
    uint32_t reserved[7];      /* Must be 0 */
} yq_cluster_config;

/* Cluster statistics */
typedef struct yq_cluster_stats {
    uint32_t struct_size;      /* Must be sizeof(yq_cluster_stats) */
    uint32_t total_nodes;      /* Total number of nodes */
    uint32_t active_nodes;     /* Number of active nodes */
    uint32_t master_nodes;     /* Number of master nodes */
    uint32_t slave_nodes;      /* Number of slave nodes */
    uint32_t failed_nodes;     /* Number of failed nodes */
    uint32_t total_operations; /* Total operations */
    uint32_t successful_ops;    /* Successful operations */
    uint32_t failed_ops;       /* Failed operations */
    uint32_t replication_lag;  /* Average replication lag (ms) */
    uint32_t network_latency;  /* Average network latency (ms) */
    uint32_t last_election;    /* Last election timestamp */
    uint32_t elections_count;  /* Number of elections */
    uint32_t failovers_count;  /* Number of failovers */
    uint32_t sync_events;      /* Number of sync events */
    uint32_t heartbeat_events; /* Number of heartbeat events */
    uint32_t reserved[8];      /* Must be 0 */
} yq_cluster_stats;

/* Cluster error codes */
#define YQ_CLUSTER_OK                    0
#define YQ_CLUSTER_ERR_INVAL            1
#define YQ_CLUSTER_ERR_NOMEM            2
#define YQ_CLUSTER_ERR_TIMEOUT          3
#define YQ_CLUSTER_ERR_NETWORK          4
#define YQ_CLUSTER_ERR_NODE_NOT_FOUND    5
#define YQ_CLUSTER_ERR_QUORUM           6
#define YQ_CLUSTER_ERR_ELECTION         7
#define YQ_CLUSTER_ERR_REPLICATION       8
#define YQ_CLUSTER_ERR_CONFLICT         9
#define YQ_CLUSTER_ERR_AUTH           10
#define YQ_CLUSTER_ERR_CONFIG         11
#define YQ_CLUSTER_ERR_STATE          12
#define YQ_CLUSTER_ERR_BUSY           13
#define YQ_CLUSTER_ERR_SHUTDOWN        14
#define YQ_CLUSTER_ERR_VERSION        15
#define YQ_CLUSTER_ERR_FEATURE        16
#define YQ_CLUSTER_ERR_LOAD_BALANCE   17
#define YQ_CLUSTER_ERR_FAILOVER       18
#define YQ_CLUSTER_ERR_AUTO_REJOIN    19

/* Event callbacks */
typedef void (*yq_cluster_event_callback)(uint32_t event_type, uint32_t node_id, void *user_data);

/* Cluster structure */
struct yq_cluster {
    yq_cluster_config config;
    yq_cluster_stats stats;
    yq_cluster_node *nodes[YQ_CLUSTER_MAX_NODES];
    uint32_t node_count;
    uint32_t master_node_id;
    uint32_t quorum_nodes;
    pthread_mutex_t mutex;
    pthread_t election_thread;
    pthread_t health_thread;
    pthread_t sync_thread;
    int running;
    yq_cluster_event_callback event_callback;
    void *user_data;
    yq_db *local_db;  /* Local database instance */
};

/* Cluster API functions */
int yq_cluster_init(yq_cluster_config *config, struct yq_cluster **out);
int yq_cluster_shutdown(struct yq_cluster *cluster);
int yq_cluster_get_info(struct yq_cluster *cluster, yq_cluster_config *config);
int yq_cluster_get_stats(struct yq_cluster *cluster, yq_cluster_stats *stats);

/* Node management */
int yq_cluster_add_node(struct yq_cluster *cluster, const char *address, uint32_t port, uint32_t node_id);
int yq_cluster_remove_node(struct yq_cluster *cluster, uint32_t node_id);
int yq_cluster_get_node(struct yq_cluster *cluster, uint32_t node_id, yq_cluster_node *node);
int yq_cluster_get_nodes(struct yq_cluster *cluster, yq_cluster_node *nodes, uint32_t *count);
int yq_cluster_promote_node(struct yq_cluster *cluster, uint32_t node_id);
int yq_cluster_demote_node(struct yq_cluster *cluster, uint32_t node_id);

/* Replication functions */
int yq_cluster_replicate(struct yq_cluster *cluster, const char *key, const char *value, uint32_t value_size);
int yq_cluster_replicate_async(struct yq_cluster *cluster, const char *key, const char *value, uint32_t value_size);
int yq_cluster_get_replication_status(struct yq_cluster *cluster, uint32_t *lag, uint32_t *pending);
int yq_cluster_set_consistency_level(struct yq_cluster *cluster, uint32_t level);

/* Failover functions */
int yq_cluster_trigger_failover(struct yq_cluster *cluster);
int yq_cluster_get_master_node(struct yq_cluster *cluster, uint32_t *node_id);
int yq_cluster_elect_master(struct yq_cluster *cluster);
int yq_cluster_rejoin_cluster(struct yq_cluster *cluster, uint32_t node_id);

/* Load balancing */
int yq_cluster_select_node(struct yq_cluster *cluster, uint32_t *node_id);
int yq_cluster_get_node_load(struct yq_cluster *cluster, uint32_t node_id, uint32_t *load);
int yq_cluster_balance_load(struct yq_cluster *cluster);

/* Health monitoring */
int yq_cluster_check_health(struct yq_cluster *cluster);
int yq_cluster_get_cluster_status(struct yq_cluster *cluster, uint32_t *status);
int yq_cluster_detect_partition(struct yq_cluster *cluster);

/* Event callbacks */
int yq_cluster_set_event_callback(struct yq_cluster *cluster, yq_cluster_event_callback callback, void *user_data);

/* Cluster operations */
int yq_cluster_execute_on_master(struct yq_cluster *cluster, const char *key, const char *operation, const char *params);
int yq_cluster_execute_on_all(struct yq_cluster *cluster, const char *operation, const char *params);
int yq_cluster_execute_on_node(struct yq_cluster *cluster, uint32_t node_id, const char *operation, const char *params);

/* Configuration management */
int yq_cluster_update_config(struct yq_cluster *cluster, yq_cluster_config *config);
int yq_cluster_save_config(struct yq_cluster *cluster, const char *filename);
int yq_cluster_load_config(struct yq_cluster *cluster, const char *filename);

/* Utility functions */
int yq_cluster_generate_node_id(const char *hostname, const char *address);
int yq_cluster_validate_config(yq_cluster_config *config);
uint32_t yq_cluster_calculate_quorum(uint32_t total_nodes);
int yq_cluster_is_node_available(struct yq_cluster *cluster, uint32_t node_id);

/* Thread function declarations */
void *yq_cluster_election_thread(void *arg);
void *yq_cluster_health_thread(void *arg);
void *yq_cluster_sync_thread(void *arg);
void *yq_cluster_node_heartbeat_thread(void *arg);
void *yq_cluster_node_replication_thread(void *arg);

#ifdef __cplusplus
}
#endif

#endif /* YQ_CLUSTER_H */