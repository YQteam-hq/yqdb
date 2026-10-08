/*
 *  yq_web.h - Web administration interface API for yq-DB
 *  
 *  This header defines the web functionality for yq-DB,
 *  providing a RESTful API for web-based administration and monitoring.
 *  
 *  Features:
 *  - RESTful API endpoints
 *  - JSON-based request/response
 *  - Authentication and authorization
 *  - Real-time monitoring
 *  - Database management
 *  - Query interface
 *  - Statistics and analytics
 *  - WebSocket support for real-time updates
 *  - File upload/download
 *  - Configuration management
 *  
 *  Enable with: -DYQ_ENABLE_WEB=1
 */

#ifndef YQ_WEB_H
#define YQ_WEB_H

#include <stdint.h>
#include <stddef.h>
#include <pthread.h>
#include <time.h>

#include "yq.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Forward declarations */
struct yq_db;
struct yq_db_stats;

/* Web server configuration macros */
#define YQ_WEB_MAX_URL_LEN        2048
#define YQ_WEB_MAX_METHOD_LEN      16
#define YQ_WEB_MAX_HEADER_LEN      1024
#define YQ_WEB_MAX_BODY_LEN       1024 * 1024  /* 1MB max body size */
#define YQ_WEB_MAX_SESSION_LEN     256
#define YQ_WEB_MAX_USERNAME_LEN    64
#define YQ_WEB_MAX_PASSWORD_LEN    64
#define YQ_WEB_MAX_TOKEN_LEN       256
#define YQ_WEB_DEFAULT_PORT         8080
#define YQ_WEB_MAX_CONNECTIONS     1000
#define YQ_WEB_MAX_THREADS         16
#define YQ_WEB_TIMEOUT             30

/* HTTP methods */
typedef enum yq_web_method {
    YQ_WEB_METHOD_GET = 0,
    YQ_WEB_METHOD_POST = 1,
    YQ_WEB_METHOD_PUT = 2,
    YQ_WEB_METHOD_DELETE = 3,
    YQ_WEB_METHOD_PATCH = 4,
    YQ_WEB_METHOD_HEAD = 5,
    YQ_WEB_METHOD_OPTIONS = 6
} yq_web_method;

/* HTTP status codes */
typedef enum yq_web_status {
    YQ_WEB_STATUS_OK = 200,
    YQ_WEB_STATUS_CREATED = 201,
    YQ_WEB_STATUS_ACCEPTED = 202,
    YQ_WEB_STATUS_NO_CONTENT = 204,
    YQ_WEB_STATUS_BAD_REQUEST = 400,
    YQ_WEB_STATUS_UNAUTHORIZED = 401,
    YQ_WEB_STATUS_FORBIDDEN = 403,
    YQ_WEB_STATUS_NOT_FOUND = 404,
    YQ_WEB_STATUS_METHOD_NOT_ALLOWED = 405,
    YQ_WEB_STATUS_CONFLICT = 409,
    YQ_WEB_STATUS_UNPROCESSABLE_ENTITY = 422,
    YQ_WEB_STATUS_TOO_MANY_REQUESTS = 429,
    YQ_WEB_STATUS_INTERNAL_SERVER_ERROR = 500,
    YQ_WEB_STATUS_BAD_GATEWAY = 502,
    YQ_WEB_STATUS_SERVICE_UNAVAILABLE = 503,
    YQ_WEB_STATUS_GATEWAY_TIMEOUT = 504
} yq_web_status;

/* Content types */
typedef enum yq_web_content_type {
    YQ_WEB_CONTENT_TYPE_JSON = 0,
    YQ_WEB_CONTENT_TYPE_HTML = 1,
    YQ_WEB_CONTENT_TYPE_TEXT = 2,
    YQ_WEB_CONTENT_TYPE_XML = 3,
    YQ_WEB_CONTENT_TYPE_CSV = 4,
    YQ_WEB_CONTENT_TYPE_JPEG = 5,
    YQ_WEB_CONTENT_TYPE_PNG = 6,
    YQ_WEB_CONTENT_TYPE_PDF = 7,
    YQ_WEB_CONTENT_TYPE_ZIP = 8
} yq_web_content_type;

/* Authentication types */
typedef enum yq_web_auth_type {
    YQ_WEB_AUTH_NONE = 0,
    YQ_WEB_AUTH_BASIC = 1,
    YQ_WEB_AUTH_BEARER = 2,
    YQ_WEB_AUTH_API_KEY = 3,
    YQ_WEB_AUTH_OAUTH2 = 4
} yq_web_auth_type;

/* Web server configuration */
typedef struct yq_web_config {
    uint32_t struct_size;          /* Must be sizeof(yq_web_config) */
    uint32_t enabled;              /* Whether web server enabled */
    uint32_t port;                 /* Server port */
    uint32_t max_connections;      /* Maximum connections */
    uint32_t max_threads;          /* Maximum threads */
    uint32_t timeout;              /* Request timeout in seconds */
    uint32_t keep_alive;           /* Keep-alive timeout */
    uint32_t max_body_size;        /* Maximum request body size */
    uint32_t enable_cors;          /* Enable CORS */
    uint32_t enable_compression;   /* Enable response compression */
    uint32_t enable_logging;       /* Enable access logging */
    uint32_t enable_auth;          /* Enable authentication */
    uint32_t auth_type;            /* Authentication type */
    uint32_t enable_websocket;    /* Enable WebSocket support */
    uint32_t enable_file_upload;   /* Enable file upload */
    uint32_t enable_file_download;  /* Enable file download */
    uint32_t enable_admin;         /* Enable admin interface */
    uint32_t enable_monitoring;    /* Enable monitoring endpoints */
    uint32_t reserved[8];          /* Must be 0 */
} yq_web_config;

/* HTTP request */
typedef struct yq_web_request {
    uint32_t struct_size;          /* Must be sizeof(yq_web_request) */
    yq_web_method method;          /* HTTP method */
    yq_web_status status;          /* HTTP status */
    yq_web_content_type content_type; /* Response content type */
    yq_web_auth_type auth_type;    /* Authentication type */
    uint32_t content_length;       /* Request content length */
    uint32_t response_length;      /* Response content length */
    uint32_t connection_id;       /* Connection ID */
    uint32_t session_id;          /* Session ID */
    uint64_t request_id;          /* Unique request ID */
    time_t timestamp;              /* Request timestamp */
    char remote_addr[64];         /* Remote IP address */
    char remote_port[16];         /* Remote port */
    char user_agent[256];         /* User agent string */
    char referer[512];            /* Referer header */
    char url[YQ_WEB_MAX_URL_LEN]; /* Request URL */
    char body[YQ_WEB_MAX_BODY_LEN]; /* Request body */
    char response[YQ_WEB_MAX_BODY_LEN]; /* Response body */
    int authenticated;           /* Whether request is authenticated */
    int websocket;                /* Whether WebSocket connection */
    struct yq_web_session *session; /* User session */
    void *user_data;              /* User data */
} yq_web_request;

/* HTTP response */
typedef struct yq_web_response {
    uint32_t struct_size;          /* Must be sizeof(yq_web_response) */
    yq_web_status status;          /* HTTP status */
    yq_web_content_type content_type; /* Content type */
    uint32_t content_length;       /* Content length */
    uint32_t headers_count;        /* Number of headers */
    uint32_t cookies_count;       /* Number of cookies */
    time_t timestamp;              /* Response timestamp */
    char body[YQ_WEB_MAX_BODY_LEN]; /* Response body */
    char headers[YQ_WEB_MAX_HEADER_LEN * 32]; /* Headers buffer */
    char cookies[YQ_WEB_MAX_HEADER_LEN * 16]; /* Cookies buffer */
    int keep_alive;               /* Keep-alive flag */
    int compressed;               /* Compression flag */
    int cors;                     /* CORS flag */
    struct yq_web_request *request; /* Original request */
    void *user_data;              /* User data */
} yq_web_response;

/* Web session */
typedef struct yq_web_session {
    uint32_t struct_size;          /* Must be sizeof(yq_web_session) */
    uint32_t session_id;          /* Session ID */
    uint32_t user_id;             /* User ID */
    uint32_t created_at;           /* Creation timestamp */
    uint32_t last_access;         /* Last access timestamp */
    uint32_t expires_at;          /* Expiration timestamp */
    uint32_t access_count;        /* Access count */
    uint32_t ip_count;            /* Unique IP count */
    char session_id_str[YQ_WEB_MAX_SESSION_LEN]; /* Session ID string */
    char user_agent[256];         /* User agent */
    char ip_address[64];          /* IP address */
    char username[YQ_WEB_MAX_USERNAME_LEN]; /* Username */
    char token[YQ_WEB_MAX_TOKEN_LEN]; /* Authentication token */
    int active;                  /* Whether session is active */
    int admin;                   /* Whether admin session */
    struct yq_web_session *next; /* Next session in list */
    struct yq_web_session *prev; /* Previous session in list */
} yq_web_session;

/* Web server statistics */
typedef struct yq_web_stats {
    uint32_t struct_size;          /* Must be sizeof(yq_web_stats) */
    uint64_t total_requests;      /* Total requests */
    uint64_t successful_requests;  /* Successful requests */
    uint64_t failed_requests;      /* Failed requests */
    uint64_t bytes_sent;          /* Bytes sent */
    uint64_t bytes_received;      /* Bytes received */
    uint64_t active_connections;  /* Active connections */
    uint64_t total_connections;    /* Total connections */
    uint64_t sessions_created;    /* Sessions created */
    uint64_t sessions_active;      /* Active sessions */
    uint64_t websocket_connections; /* WebSocket connections */
    uint64_t files_uploaded;       /* Files uploaded */
    uint64_t files_downloaded;     /* Files downloaded */
    uint32_t peak_connections;    /* Peak connections */
    uint32_t peak_threads;        /* Peak threads */
    double avg_response_time;     /* Average response time */
    double cpu_usage;            /* CPU usage percentage */
    double memory_usage;         /* Memory usage percentage */
    time_t start_time;           /* Server start time */
    time_t last_request;          /* Last request time */
    time_t last_response;         /* Last response time */
    uint32_t reserved[8];         /* Must be 0 */
} yq_web_stats;

/* User account */
typedef struct yq_web_user {
    uint32_t struct_size;          /* Must be sizeof(yq_web_user) */
    uint32_t user_id;             /* User ID */
    uint32_t created_at;           /* Creation timestamp */
    uint32_t last_login;          /* Last login timestamp */
    uint32_t last_activity;       /* Last activity timestamp */
    uint32_t login_count;         /* Login count */
    uint32_t failed_attempts;     /* Failed login attempts */
    uint32_t role;                /* User role */
    uint32_t status;              /* Account status */
    uint32_t permissions;        /* Permission bitmask */
    char username[YQ_WEB_MAX_USERNAME_LEN]; /* Username */
    char email[128];              /* Email address */
    char password_hash[64];       /* Password hash */
    char salt[32];               /* Password salt */
    char token[YQ_WEB_MAX_TOKEN_LEN]; /* API token */
    char last_ip[64];             /* Last login IP */
    int active;                  /* Whether account is active */
    int admin;                   /* Whether admin account */
    struct yq_web_user *next;    /* Next user in list */
    struct yq_web_user *prev;    /* Previous user in list */
} yq_web_user;

/* Web server error codes */
#define YQ_WEB_OK                 0
#define YQ_WEB_ERR_INVAL         1
#define YQ_WEB_ERR_NOMEM         2
#define YQ_WEB_ERR_CONN          3
#define YQ_WEB_ERR_TIMEOUT       4
#define YQ_WEB_ERR_AUTH          5
#define YQ_WEB_ERR_PERM          6
#define YQ_WEB_ERR_BUSY          7
#define YQ_WEB_ERR_SHUTDOWN      8
#define YQ_WEB_ERR_CONFIG        9
#define YQ_WEB_ERR_STATE         10
#define YQ_WEB_ERR_NOT_FOUND      11
#define YQ_WEB_ERR_EXISTS        12
#define YQ_WEB_ERR_TOO_LARGE      13
#define YQ_WEB_ERR_UNSUPPORTED    14
#define YQ_WEB_ERR_RATE_LIMIT     15

/* Web server API functions */
int yq_web_init(yq_web_config *config, int *out_server_fd);
int yq_web_shutdown(int server_fd);
int yq_web_start(int server_fd);
int yq_web_stop(int server_fd);

/* Request handling */
int yq_web_handle_request(int client_fd, yq_web_request *request, yq_web_response *response);
int yq_web_process_request(yq_web_request *request, yq_web_response *response);

/* Response generation */
int yq_web_response_set_status(yq_web_response *response, yq_web_status status);
int yq_web_response_set_content_type(yq_web_response *response, yq_web_content_type content_type);
int yq_web_response_set_header(yq_web_response *response, const char *name, const char *value);
int yq_web_response_set_cookie(yq_web_response *response, const char *name, const char *value, int max_age);
int yq_web_response_set_body(yq_web_response *response, const char *body, size_t length);
int yq_web_response_send(yq_web_response *response);

/* Authentication */
int yq_web_auth_init(const char *admin_username, const char *admin_password);
int yq_web_auth_login(const char *username, const char *password, yq_web_session **out_session);
int yq_web_auth_logout(yq_web_session *session);
int yq_web_auth_validate(yq_web_request *request, yq_web_session **out_session);
int yq_web_auth_create_user(const char *username, const char *password, const char *email, uint32_t role);
int yq_web_auth_delete_user(uint32_t user_id);
int yq_web_auth_update_user(uint32_t user_id, const char *username, const char *password, const char *email, uint32_t role);

/* Session management */
int yq_web_session_create(uint32_t user_id, yq_web_session **out_session);
int yq_web_session_get(const char *session_id_str, yq_web_session **out_session);
int yq_web_session_update(yq_web_session *session);
int yq_web_session_delete(yq_web_session *session);
int yq_web_session_cleanup(void);
int yq_web_session_get_all(yq_web_session **sessions, uint32_t *count);

/* Database operations */
int yq_web_db_connect(const char *path, yq_db **out_db);
int yq_web_db_disconnect(yq_db *db);
int yq_web_db_get(yq_db *db, const char *key, size_t key_len, char **value, size_t *value_len);
int yq_web_db_put(yq_db *db, const char *key, size_t key_len, const char *value, size_t value_len);
int yq_web_db_delete(yq_db *db, const char *key, size_t key_len);
int yq_web_db_exists(yq_db *db, const char *key, size_t key_len);
int yq_web_db_stats(yq_db *db, yq_stat *stats);

/* Query interface */
int yq_web_query_execute(yq_db *db, const char *query, char **result, size_t *result_len);
int yq_web_query_list_keys(yq_db *db, char **keys, size_t *key_lens, uint32_t max_keys);
int yq_web_query_search(yq_db *db, const char *pattern, char **keys, size_t *key_lens, uint32_t max_keys);
int yq_web_query_range(yq_db *db, const char *start_key, const char *end_key, char **keys, size_t *key_lens, uint32_t max_keys);

/* File operations */
int yq_web_file_upload(yq_web_request *request, const char *filename, const char *content, size_t length);
int yq_web_file_download(yq_web_request *request, const char *filename, char **content, size_t *length);
int yq_web_file_delete(yq_web_request *request, const char *filename);
int yq_web_file_list(yq_web_request *request, char **files, size_t *file_lens, uint32_t max_files);

/* Monitoring and statistics */
int yq_web_stats_get(yq_web_stats *stats);
int yq_web_stats_reset(void);
int yq_web_stats_update(void);

/* WebSocket support */
int yq_web_websocket_upgrade(yq_web_request *request, int *out_websocket_fd);
int yq_web_websocket_send(int websocket_fd, const char *message, size_t length);
int yq_web_websocket_receive(int websocket_fd, char *message, size_t *length);
int yq_web_websocket_close(int websocket_fd);

/* Configuration management */
int yq_web_config_update(yq_web_config *config);
int yq_web_config_save(const char *filename);
int yq_web_config_load(const char *filename);

/* Utility functions */
const char *yq_web_status_to_string(yq_web_status status);
const char *yq_web_method_to_string(yq_web_method method);
const char *yq_web_content_type_to_string(yq_web_content_type content_type);
int yq_web_parse_url(const char *url, char *path, char *query, char *fragment);
int yq_web_url_encode(const char *input, char *output, size_t output_size);
int yq_web_url_decode(const char *input, char *output, size_t output_size);
int yq_web_json_parse(const char *json, char **result);
int yq_web_json_generate(const char *data, char **json);
int yq_web_hash_password(const char *password, const char *salt, char *hash);

/* Web API endpoints */
int yq_web_endpoint_health(yq_web_request *request, yq_web_response *response);
int yq_web_endpoint_info(yq_web_request *request, yq_web_response *response);
int yq_web_endpoint_stats(yq_web_request *request, yq_web_response *response);
int yq_web_endpoint_login(yq_web_request *request, yq_web_response *response);
int yq_web_endpoint_logout(yq_web_request *request, yq_web_response *response);
int yq_web_endpoint_db_status(yq_web_request *request, yq_web_response *response);
int yq_web_endpoint_db_query(yq_web_request *request, yq_web_response *response);
int yq_web_endpoint_db_keys(yq_web_request *request, yq_web_response *response);
int yq_web_endpoint_db_get(yq_web_request *request, yq_web_response *response);
int yq_web_endpoint_db_put(yq_web_request *request, yq_web_response *response);
int yq_web_endpoint_db_delete(yq_web_request *request, yq_web_response *response);
int yq_web_endpoint_admin_users(yq_web_request *request, yq_web_response *response);
int yq_web_endpoint_admin_config(yq_web_request *request, yq_web_response *response);
int yq_web_endpoint_admin_logs(yq_web_request *request, yq_web_response *response);

/* Web interface HTML templates */
extern const char *yq_web_template_login;
extern const char *yq_web_template_dashboard;
extern const char *yq_web_template_admin;
extern const char *yq_web_template_query;
extern const char *yq_web_template_monitoring;

#ifdef __cplusplus
}
#endif

#endif /* YQ_WEB_H */