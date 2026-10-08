/*
 *  yq_web.c - Web administration interface implementation for yq-DB
 *  
 *  This file implements the web functionality for yq-DB,
 *  providing a RESTful API for web-based administration and monitoring.
 */

#include "yq_web.h"
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
#include <sys/time.h>
#include <errno.h>

/* Forward declarations for static functions */
static void *yq_web_worker_thread(void *arg);
#include <stdarg.h>
#include <fcntl.h>
#include <signal.h>

/* Internal web server state */
static struct {
    int server_fd;
    int running;
    yq_web_config config;
    yq_web_stats stats;
    yq_web_session *sessions;
    yq_web_user *users;
    pthread_mutex_t mutex;
    pthread_t *threads;
    uint32_t thread_count;
} g_web_server = {0};

/* HTML templates */
const char *yq_web_template_login = 
    "<!DOCTYPE html>"
    "<html>"
    "<head>"
    "<title>yq-DB Login</title>"
    "<style>"
    "body { font-family: Arial, sans-serif; margin: 100px; background: #f5f5f5; }"
    ".container { max-width: 400px; margin: 0 auto; background: white; padding: 40px; border-radius: 8px; box-shadow: 0 2px 10px rgba(0,0,0,0.1); }"
    "h1 { color: #333; text-align: center; margin-bottom: 30px; }"
    ".form-group { margin-bottom: 20px; }"
    "label { display: block; margin-bottom: 5px; color: #555; }"
    "input[type=text], input[type=password] { width: 100%; padding: 12px; border: 1px solid #ddd; border-radius: 4px; box-sizing: border-box; }"
    "button { width: 100%; padding: 12px; background: #007bff; color: white; border: none; border-radius: 4px; cursor: pointer; font-size: 16px; }"
    "button:hover { background: #0056b3; }"
    ".error { color: #dc3545; margin-top: 10px; text-align: center; }"
    "</style>"
    "</head>"
    "<body>"
    "<div class='container'>"
    "<h1>yq-DB Login</h1>"
    "<form id='loginForm'>"
    "<div class='form-group'>"
    "<label for='username'>Username:</label>"
    "<input type='text' id='username' name='username' required>"
    "</div>"
    "<div class='form-group'>"
    "<label for='password'>Password:</label>"
    "<input type='password' id='password' name='password' required>"
    "</div>"
    "<button type='submit'>Login</button>"
    "</form>"
    "<div id='error' class='error'></div>"
    "</div>"
    "<script>"
    "document.getElementById('loginForm').addEventListener('submit', function(e) {"
    "    e.preventDefault();"
    "    const username = document.getElementById('username').value;"
    "    const password = document.getElementById('password').value;"
    "    const errorDiv = document.getElementById('error');"
    "    "
    "    fetch('/api/login', {"
    "        method: 'POST',"
    "        headers: {"
    "            'Content-Type': 'application/json',"
    "        },"
    "        body: JSON.stringify({ username, password })"
    "    })"
    "    .then(response => response.json())"
    "    .then(data => {"
    "        if (data.success) {"
    "            window.location.href = '/dashboard';"
    "        } else {"
    "            errorDiv.textContent = data.message || 'Login failed';"
    "        }"
    "    })"
    "    .catch(error => {"
    "        errorDiv.textContent = 'Error: ' + error;"
    "    });"
    "});"
    "</script>"
    "</body>"
    "</html>";

const char *yq_web_template_dashboard = 
    "<!DOCTYPE html>"
    "<html>"
    "<head>"
    "<title>yq-DB Dashboard</title>"
    "<style>"
    "body { font-family: Arial, sans-serif; margin: 0; background: #f5f5f5; }"
    ".header { background: #333; color: white; padding: 20px; }"
    ".container { max-width: 1200px; margin: 0 auto; padding: 20px; }"
    ".card { background: white; padding: 20px; margin-bottom: 20px; border-radius: 8px; box-shadow: 0 2px 10px rgba(0,0,0,0.1); }"
    ".stats { display: grid; grid-template-columns: repeat(auto-fit, minmax(250px, 1fr)); gap: 20px; }"
    ".stat-item { text-align: center; }"
    ".stat-value { font-size: 24px; font-weight: bold; color: #007bff; }"
    ".stat-label { color: #666; margin-top: 5px; }"
    ".nav { display: flex; gap: 10px; margin-bottom: 20px; }"
    ".nav button { padding: 10px 20px; border: none; border-radius: 4px; cursor: pointer; background: #007bff; color: white; }"
    ".nav button:hover { background: #0056b3; }"
    ".nav button.active { background: #0056b3; }"
    ".tab { display: none; }"
    ".tab.active { display: block; }"
    ".form-group { margin-bottom: 15px; }"
    "label { display: block; margin-bottom: 5px; color: #555; }"
    "input, textarea { width: 100%; padding: 8px; border: 1px solid #ddd; border-radius: 4px; box-sizing: border-box; }"
    "button { padding: 10px 20px; background: #007bff; color: white; border: none; border-radius: 4px; cursor: pointer; }"
    "button:hover { background: #0056b3; }"
    ".error { color: #dc3545; margin-top: 10px; }"
    ".success { color: #28a745; margin-top: 10px; }"
    ".key-item { background: #f8f9fa; padding: 10px; margin-bottom: 10px; border-radius: 4px; border: 1px solid #e9ecef; }"
    ".key-actions { display: flex; gap: 10px; margin-top: 10px; }"
    ".key-actions button { padding: 5px 10px; font-size: 12px; }"
    "</style>"
    "</head>"
    "<body>"
    "<div class='header'>"
    "<h1>yq-DB Dashboard</h1>"
    "<p>Welcome to yq-DB Web Administration Interface</p>"
    "</div>"
    "<div class='container'>"
    "<div class='nav'>"
    "<button class='nav active' onclick='showTab(\"dashboard\")'>Dashboard</button>"
    "<button class='nav' onclick='showTab(\"query\")'>Query</button>"
    "<button class='nav' onclick='showTab(\"monitoring\")'>Monitoring</button>"
    "<button class='nav' onclick='showTab(\"admin\")'>Admin</button>"
    "<button class='nav' onclick='logout()'>Logout</button>"
    "</div>"
    "<div id='dashboard' class='tab active'>"
    "<div class='card'>"
    "<h2>Database Statistics</h2>"
    "<div class='stats'>"
    "<div class='stat-item'>"
    "<div class='stat-value' id='total_keys'>0</div>"
    "<div class='stat-label'>Total Keys</div>"
    "</div>"
    "<div class='stat-item'>"
    "<div class='stat-value' id='total_size'>0</div>"
    "<div class='stat-label'>Total Size</div>"
    "</div>"
    "<div class='stat-item'>"
    "<div class='stat-value' id='uptime'>0</div>"
    "<div class='stat-label'>Uptime</div>"
    "</div>"
    "</div>"
    "</div>"
    "<div class='card'>"
    "<h2>Recent Operations</h2>"
    "<div id='recent_operations'></div>"
    "</div>"
    "</div>"
    "<div id='query' class='tab'>"
    "<div class='card'>"
    "<h2>Query Interface</h2>"
    "<div class='form-group'>"
    "<label for='query_key'>Key:</label>"
    "<input type='text' id='query_key' placeholder='Enter key to query'>"
    "</div>"
    "<div class='form-group'>"
    "<label for='query_value'>Value:</label>"
    "<textarea id='query_value' rows='4' placeholder='Value will be displayed here'></textarea>"
    "</div>"
    "<button onclick='executeQuery()'>Execute Query</button>"
    "<div id='query_result'></div>"
    "</div>"
    "</div>"
    "<div id='monitoring' class='tab'>"
    "<div class='card'>"
    "<h2>System Monitoring</h2>"
    "<div class='stats'>"
    "<div class='stat-item'>"
    "<div class='stat-value' id='cpu_usage'>0%</div>"
    "<div class='stat-label'>CPU Usage</div>"
    "</div>"
    "<div class='stat-item'>"
    "<div class='stat-value' id='memory_usage'>0%</div>"
    "<div class='stat-label'>Memory Usage</div>"
    "</div>"
    "<div class='stat-item'>"
    "<div class='stat-value' id='active_connections'>0</div>"
    "<div class='stat-label'>Active Connections</div>"
    "</div>"
    "</div>"
    "</div>"
    "</div>"
    "<div id='admin' class='tab'>"
    "<div class='card'>"
    "<h2>Administration</h2>"
    "<div class='form-group'>"
    "<label for='admin_key'>Key:</label>"
    "<input type='text' id='admin_key'>"
    "</div>"
    "<div class='form-group'>"
    "<label for='admin_value'>Value:</label>"
    "<textarea id='admin_value' rows='4'></textarea>"
    "</div>"
    "<button onclick='adminPut()'>Put</button>"
    "<button onclick='adminGet()'>Get</button>"
    "<button onclick='adminDelete()'>Delete</button>"
    "<div id='admin_result'></div>"
    "</div>"
    "</div>"
    "</div>"
    "<script>"
    "function showTab(tabName) {"
    "    document.querySelectorAll('.tab').forEach(tab => tab.classList.remove('active'));"
    "    document.querySelectorAll('.nav').forEach(btn => btn.classList.remove('active'));"
    "    document.getElementById(tabName).classList.add('active');"
    "    event.target.classList.add('active');"
    "}"
    "function logout() {"
    "    fetch('/api/logout', { method: 'POST' })"
    "    .then(() => window.location.href = '/login');"
    "}"
    "function executeQuery() {"
    "    const key = document.getElementById('query_key').value;"
    "    fetch('/api/db/get?key=' + encodeURIComponent(key))"
    "    .then(response => response.json())"
    "    .then(data => {"
    "        document.getElementById('query_result').innerHTML = "
    "            data.success ? `<div class='success'>Success: ${data.value}</div>` : "
    "            `<div class='error'>Error: ${data.message}</div>`;"
    "    });"
    "}"
    "function adminPut() {"
    "    const key = document.getElementById('admin_key').value;"
    "    const value = document.getElementById('admin_value').value;"
    "    fetch('/api/db/put', {"
    "        method: 'POST',"
    "        headers: { 'Content-Type': 'application/json' },"
    "        body: JSON.stringify({ key, value })"
    "    })"
    "    .then(response => response.json())"
    "    .then(data => {"
    "        document.getElementById('admin_result').innerHTML = "
    "            data.success ? `<div class='success'>Success</div>` : "
    "            `<div class='error'>Error: ${data.message}</div>`;"
    "    });"
    "}"
    "function adminGet() {"
    "    const key = document.getElementById('admin_key').value;"
    "    fetch('/api/db/get?key=' + encodeURIComponent(key))"
    "    .then(response => response.json())"
    "    .then(data => {"
    "        document.getElementById('admin_result').innerHTML = "
    "            data.success ? `<div class='success'>Value: ${data.value}</div>` : "
    "            `<div class='error'>Error: ${data.message}</div>`;"
    "    });"
    "}"
    "function adminDelete() {"
    "    const key = document.getElementById('admin_key').value;"
    "    fetch('/api/db/delete?key=' + encodeURIComponent(key), { method: 'POST' })"
    "    .then(response => response.json())"
    "    .then(data => {"
    "        document.getElementById('admin_result').innerHTML = "
    "            data.success ? `<div class='success'>Deleted</div>` : "
    "            `<div class='error'>Error: ${data.message}</div>`;"
    "    });"
    "}"
    "function updateStats() {"
    "    fetch('/api/stats')"
    "    .then(response => response.json())"
    "    .then(data => {"
    "        if (data.db_stats) {"
    "            document.getElementById('total_keys').textContent = data.db_stats.key_count;"
    "            document.getElementById('total_size').textContent = formatSize(data.db_stats.size);"
    "        }"
    "        if (data.web_stats) {"
    "            document.getElementById('active_connections').textContent = data.web_stats.active_connections;"
    "        }"
    "    });"
    "}"
    "function formatSize(bytes) {"
    "    if (bytes === 0) return '0 Bytes';"
    "    const k = 1024;"
    "    const sizes = ['Bytes', 'KB', 'MB', 'GB'];"
    "    const i = Math.floor(Math.log(bytes) / Math.log(k));"
    "    return parseFloat((bytes / Math.pow(k, i)).toFixed(2)) + ' ' + sizes[i];"
    "}"
    "setInterval(updateStats, 5000);"
    "updateStats();"
    "</script>"
    "</body>"
    "</html>";

/* Logging function for web operations */
static void yq_web_log(const char *format, ...) {
    if (!g_web_server.config.enable_logging) return;
    
    va_list args;
    va_start(args, format);
    
    char timestamp[32];
    time_t now = time(NULL);
    strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", localtime(&now));
    
    printf("[%s] [WEB] ", timestamp);
    vprintf(format, args);
    printf("\n");
    
    va_end(args);
}

/* Initialize web server */
int yq_web_init(yq_web_config *config, int *out_server_fd) {
    if (!config || !out_server_fd) return YQ_WEB_ERR_INVAL;
    
    /* Validate configuration */
    if (config->struct_size != sizeof(yq_web_config)) {
        return YQ_WEB_ERR_CONFIG;
    }
    
    if (config->port == 0 || config->port > 65535) {
        return YQ_WEB_ERR_CONFIG;
    }
    
    if (config->max_threads == 0 || config->max_threads > 64) {
        return YQ_WEB_ERR_CONFIG;
    }
    
    /* Copy configuration */
    memcpy(&g_web_server.config, config, sizeof(yq_web_config));
    
    /* Initialize mutex */
    pthread_mutex_init(&g_web_server.mutex, NULL);
    
    /* Create server socket */
    g_web_server.server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (g_web_server.server_fd < 0) {
        return YQ_WEB_ERR_CONN;
    }
    
    /* Set socket options */
    int opt = 1;
    if (setsockopt(g_web_server.server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        close(g_web_server.server_fd);
        return YQ_WEB_ERR_CONN;
    }
    
    /* Bind socket */
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(g_web_server.config.port);
    
    if (bind(g_web_server.server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(g_web_server.server_fd);
        return YQ_WEB_ERR_CONN;
    }
    
    /* Listen for connections */
    if (listen(g_web_server.server_fd, g_web_server.config.max_connections) < 0) {
        close(g_web_server.server_fd);
        return YQ_WEB_ERR_CONN;
    }
    
    /* Initialize statistics */
    memset(&g_web_server.stats, 0, sizeof(g_web_server.stats));
    g_web_server.stats.start_time = time(NULL);
    
    /* Initialize admin user */
    g_web_server.users = malloc(sizeof(yq_web_user));
    if (!g_web_server.users) {
        close(g_web_server.server_fd);
        return YQ_WEB_ERR_NOMEM;
    }
    
    memset(g_web_server.users, 0, sizeof(yq_web_user));
    g_web_server.users->struct_size = sizeof(yq_web_user);
    g_web_server.users->user_id = 1;
    g_web_server.users->created_at = time(NULL);
    g_web_server.users->last_login = time(NULL);
    g_web_server.users->role = 1; /* Admin role */
    g_web_server.users->status = 1; /* Active */
    g_web_server.users->active = 1;
    g_web_server.users->admin = 1;
    
    /* Copy admin credentials (in real implementation, these would be properly hashed) */
    strncpy(g_web_server.users->username, "admin", YQ_WEB_MAX_USERNAME_LEN - 1);
    strncpy(g_web_server.users->password_hash, "admin", 63);
    strncpy(g_web_server.users->email, "admin@yqdb.local", 127);
    
    yq_web_log("Web server initialized on port %d", g_web_server.config.port);
    
    *out_server_fd = g_web_server.server_fd;
    return YQ_WEB_OK;
}

/* Shutdown web server */
int yq_web_shutdown(int server_fd) {
    if (server_fd != g_web_server.server_fd) {
        return YQ_WEB_ERR_INVAL;
    }
    
    g_web_server.running = 0;
    
    /* Close server socket */
    close(g_web_server.server_fd);
    
    /* Cleanup sessions */
    pthread_mutex_lock(&g_web_server.mutex);
    yq_web_session *session = g_web_server.sessions;
    while (session) {
        yq_web_session *next = session->next;
        free(session);
        session = next;
    }
    g_web_server.sessions = NULL;
    
    /* Cleanup users */
    yq_web_user *user = g_web_server.users;
    while (user) {
        yq_web_user *next = user->next;
        free(user);
        user = next;
    }
    g_web_server.users = NULL;
    
    pthread_mutex_unlock(&g_web_server.mutex);
    
    /* Cleanup threads */
    if (g_web_server.threads) {
        free(g_web_server.threads);
        g_web_server.threads = NULL;
    }
    
    /* Destroy mutex */
    pthread_mutex_destroy(&g_web_server.mutex);
    
    yq_web_log("Web server shutdown");
    
    return YQ_WEB_OK;
}

/* Start web server */
int yq_web_start(int server_fd) {
    if (server_fd != g_web_server.server_fd) {
        return YQ_WEB_ERR_INVAL;
    }
    
    g_web_server.running = 1;
    
    /* Create worker threads */
    g_web_server.threads = malloc(g_web_server.config.max_threads * sizeof(pthread_t));
    if (!g_web_server.threads) {
        return YQ_WEB_ERR_NOMEM;
    }
    
    for (uint32_t i = 0; i < g_web_server.config.max_threads; i++) {
        if (pthread_create(&g_web_server.threads[i], NULL, yq_web_worker_thread, NULL) != 0) {
            yq_web_log("Failed to create worker thread %d", i);
            return YQ_WEB_ERR_BUSY;
        }
    }
    
    yq_web_log("Web server started with %d worker threads", g_web_server.config.max_threads);
    
    return YQ_WEB_OK;
}

/* Stop web server */
int yq_web_stop(int server_fd) {
    if (server_fd != g_web_server.server_fd) {
        return YQ_WEB_ERR_INVAL;
    }
    
    g_web_server.running = 0;
    
    /* Wait for all threads to finish */
    for (uint32_t i = 0; i < g_web_server.config.max_threads; i++) {
        if (g_web_server.threads[i]) {
            pthread_join(g_web_server.threads[i], NULL);
        }
    }
    
    yq_web_log("Web server stopped");
    
    return YQ_WEB_OK;
}







/* Process HTTP request */
int yq_web_process_request(yq_web_request *request, yq_web_response *response) {
    if (!request || !response) return YQ_WEB_ERR_INVAL;
    
    /* Set default response */
    response->status = YQ_WEB_STATUS_OK;
    response->content_type = YQ_WEB_CONTENT_TYPE_HTML;
    response->content_length = 0;
    
    /* Handle authentication if enabled */
    if (g_web_server.config.enable_auth) {
        if (!yq_web_auth_validate(request, NULL)) {
            response->status = YQ_WEB_STATUS_UNAUTHORIZED;
            response->content_type = YQ_WEB_CONTENT_TYPE_JSON;
            yq_web_response_set_body(response, "{\"success\":false,\"message\":\"Unauthorized\"}", 0);
            return YQ_WEB_OK;
        }
    }
    
    /* Route request to appropriate handler */
    if (strcmp(request->url, "/") == 0 || strcmp(request->url, "/dashboard") == 0) {
        yq_web_endpoint_dashboard(request, response);
    } else if (strcmp(request->url, "/login") == 0) {
        return yq_web_endpoint_login(request, response);
    } else if (strcmp(request->url, "/logout") == 0) {
        return yq_web_endpoint_logout(request, response);
    } else if (strncmp(request->url, "/api/", 5) == 0) {
        yq_web_api_endpoint(request, response);
    } else if (strcmp(request->url, "/health") == 0) {
        return yq_web_endpoint_health(request, response);
    } else if (strcmp(request->url, "/stats") == 0) {
        return yq_web_endpoint_stats(request, response);
    } else {
        response->status = YQ_WEB_STATUS_NOT_FOUND;
        response->content_type = YQ_WEB_CONTENT_TYPE_HTML;
        yq_web_response_set_body(response, "<html><body><h1>404 Not Found</h1></body></html>", 0);
        return YQ_WEB_OK;
    }
}

/* Send HTTP response */
static void yq_web_send_response(int client_fd, yq_web_response *response) {
    if (!response) return;
    
    /* Build HTTP response */
    char buffer[4096];
    int pos = 0;
    
    /* Status line */
    pos += snprintf(buffer + pos, sizeof(buffer) - pos, "HTTP/1.1 %d %s\r\n", 
                   response->status, yq_web_status_to_string(response->status));
    
    /* Headers */
    pos += snprintf(buffer + pos, sizeof(buffer) - pos, "Content-Type: %s\r\n", 
                   yq_web_content_type_to_string(response->content_type));
    
    if (response->content_length > 0) {
        pos += snprintf(buffer + pos, sizeof(buffer) - pos, "Content-Length: %d\r\n", 
                       response->content_length);
    }
    
    if (response->keep_alive) {
        pos += snprintf(buffer + pos, sizeof(buffer) - pos, "Connection: keep-alive\r\n");
    } else {
        pos += snprintf(buffer + pos, sizeof(buffer) - pos, "Connection: close\r\n");
    }
    
    if (response->cors) {
        pos += snprintf(buffer + pos, sizeof(buffer) - pos, "Access-Control-Allow-Origin: *\r\n");
        pos += snprintf(buffer + pos, sizeof(buffer) - pos, "Access-Control-Allow-Methods: GET, POST, PUT, DELETE, OPTIONS\r\n");
        pos += snprintf(buffer + pos, sizeof(buffer) - pos, "Access-Control-Allow-Headers: Content-Type\r\n");
    }
    
    pos += snprintf(buffer + pos, sizeof(buffer) - pos, "Server: yq-DB Web\r\n");
    
    /* Empty line before body */
    pos += snprintf(buffer + pos, sizeof(buffer) - pos, "\r\n");
    
    /* Send headers */
    send(client_fd, buffer, pos, 0);
    
    /* Send body */
    if (response->content_length > 0) {
        send(client_fd, response->body, response->content_length, 0);
    }
}

/* Send HTTP error */
static void yq_web_send_error(int client_fd, yq_web_status status, const char *message) {
    yq_web_response response;
    memset(&response, 0, sizeof(response));
    
    response.status = status;
    response.content_type = YQ_WEB_CONTENT_TYPE_HTML;
    response.content_length = 0;
    
    char body[1024];
    snprintf(body, sizeof(body), "<html><body><h1>%d %s</h1><p>%s</p></body></html>", 
             status, yq_web_status_to_string(status), message ? message : "Error");
    
    yq_web_response_set_body(&response, body, 0);
    yq_web_send_response(client_fd, &response);
}

/* Dashboard endpoint */
int yq_web_endpoint_dashboard(yq_web_request *request, yq_web_response *response) {
    if (!request || !response) return YQ_WEB_ERR_INVAL;
    
    response->status = YQ_WEB_STATUS_OK;
    response->content_type = YQ_WEB_CONTENT_TYPE_HTML;
    response->content_length = strlen(yq_web_template_dashboard);
    
    strncpy(response->body, yq_web_template_dashboard, YQ_WEB_MAX_BODY_LEN - 1);
    response->body[YQ_WEB_MAX_BODY_LEN - 1] = '\0';
    
    return YQ_WEB_OK;
}

/* Login endpoint */
int yq_web_endpoint_login(yq_web_request *request, yq_web_response *response) {
    if (!request || !response) return YQ_WEB_ERR_INVAL;
    
    if (request->method == YQ_WEB_METHOD_GET) {
        /* Show login form */
        response->status = YQ_WEB_STATUS_OK;
        response->content_type = YQ_WEB_CONTENT_TYPE_HTML;
        response->content_length = strlen(yq_web_template_login);
        
        strncpy(response->body, yq_web_template_login, YQ_WEB_MAX_BODY_LEN - 1);
        response->body[YQ_WEB_MAX_BODY_LEN - 1] = '\0';
        
        return YQ_WEB_OK;
    } else if (request->method == YQ_WEB_METHOD_POST) {
        /* Handle login */
        char *username = request->body;
        char *password = strchr(request->body, '\n');
        if (password) {
            password++;
            char *end = strchr(password, '\n');
            if (end) {
                *end = '\0';
            }
        }
        
        if (username && password && strcmp(username, "admin") == 0 && strcmp(password, "admin") == 0) {
            /* Create session */
            yq_web_session *session = malloc(sizeof(yq_web_session));
            if (!session) {
                response->status = YQ_WEB_STATUS_INTERNAL_SERVER_ERROR;
                response->content_type = YQ_WEB_CONTENT_TYPE_JSON;
                yq_web_response_set_body(response, "{\"success\":false,\"message\":\"Internal server error\"}", 0);
                return YQ_WEB_OK;
            }
            
            memset(session, 0, sizeof(yq_web_session));
            session->struct_size = sizeof(yq_web_session);
            session->session_id = time(NULL);
            session->user_id = 1;
            session->created_at = time(NULL);
            session->last_access = time(NULL);
            session->expires_at = time(NULL) + 3600; /* 1 hour */
            session->access_count = 1;
            session->active = 1;
            session->admin = 1;
            
            snprintf(session->session_id_str, YQ_WEB_MAX_SESSION_LEN, "%u", session->session_id);
            strncpy(session->username, username, YQ_WEB_MAX_USERNAME_LEN - 1);
            strncpy(session->ip_address, "127.0.0.1", 63);
            
            /* Add to session list */
            pthread_mutex_lock(&g_web_server.mutex);
            session->next = g_web_server.sessions;
            if (g_web_server.sessions) {
                g_web_server.sessions->prev = session;
            }
            g_web_server.sessions = session;
            pthread_mutex_unlock(&g_web_server.mutex);
            
            /* Set session cookie */
            yq_web_response_set_cookie(response, "session_id", session->session_id_str, 3600);
            
            response->status = YQ_WEB_STATUS_OK;
            response->content_type = YQ_WEB_CONTENT_TYPE_JSON;
            yq_web_response_set_body(response, "{\"success\":true,\"message\":\"Login successful\"}", 0);
            
            return YQ_WEB_OK;
        } else {
            response->status = YQ_WEB_STATUS_UNAUTHORIZED;
            response->content_type = YQ_WEB_CONTENT_TYPE_JSON;
            yq_web_response_set_body(response, "{\"success\":false,\"message\":\"Invalid credentials\"}", 0);
            return YQ_WEB_OK;
        }
    }
    
    response->status = YQ_WEB_STATUS_METHOD_NOT_ALLOWED;
    return YQ_WEB_OK;
}

/* Logout endpoint */
int yq_web_endpoint_logout(yq_web_request *request, yq_web_response *response) {
    if (!request || !response) return YQ_WEB_ERR_INVAL;
    
    /* Clear session cookie */
    yq_web_response_set_cookie(response, "session_id", "", 0);
    
    response->status = YQ_WEB_STATUS_OK;
    response->content_type = YQ_WEB_CONTENT_TYPE_JSON;
    yq_web_response_set_body(response, "{\"success\":true,\"message\":\"Logout successful\"}", 0);
    
    return YQ_WEB_OK;
}

/* Health check endpoint */
int yq_web_endpoint_health(yq_web_request *request, yq_web_response *response) {
    if (!request || !response) return YQ_WEB_ERR_INVAL;
    
    response->status = YQ_WEB_STATUS_OK;
    response->content_type = YQ_WEB_CONTENT_TYPE_JSON;
    
    char body[1024];
    snprintf(body, sizeof(body), 
             "{\"status\":\"healthy\",\"timestamp\":%ld,\"uptime\":%ld,\"version\":\"1.0.0\"}", 
             time(NULL), time(NULL) - g_web_server.stats.start_time);
    
    yq_web_response_set_body(response, body, 0);
    return YQ_WEB_OK;
}

/* Statistics endpoint */
int yq_web_endpoint_stats(yq_web_request *request, yq_web_response *response) {
    if (!request || !response) return YQ_WEB_ERR_INVAL;
    
    response->status = YQ_WEB_STATUS_OK;
    response->content_type = YQ_WEB_CONTENT_TYPE_JSON;
    
    char body[2048];
    snprintf(body, sizeof(body), 
             "{\"web_stats\":{\"total_requests\":%lu,\"successful_requests\":%lu,\"failed_requests\":%lu,\"active_connections\":%lu,\"bytes_sent\":%lu,\"bytes_received\":%lu,\"uptime\":%ld},\"db_stats\":{\"key_count\":0,\"size\":0,\"memory_usage\":0.0}}",
             g_web_server.stats.total_requests,
             g_web_server.stats.successful_requests,
             g_web_server.stats.failed_requests,
             g_web_server.stats.active_connections,
             g_web_server.stats.bytes_sent,
             g_web_server.stats.bytes_received,
             time(NULL) - g_web_server.stats.start_time);
    
    yq_web_response_set_body(response, body, 0);
    return YQ_WEB_OK;
}





/* Authentication functions */
int yq_web_auth_init(const char *admin_username, const char *admin_password) {
    if (!admin_username || !admin_password) return YQ_WEB_ERR_INVAL;
    
    /* Initialize admin user (already done in yq_web_init) */
    return YQ_WEB_OK;
}

int yq_web_auth_login(const char *username, const char *password, yq_web_session **out_session) {
    if (!username || !password || !out_session) return YQ_WEB_ERR_INVAL;
    
    /* Check credentials against admin user */
    if (strcmp(username, "admin") == 0 && strcmp(password, "admin") == 0) {
        /* Create session */
        yq_web_session *session = malloc(sizeof(yq_web_session));
        if (!session) return YQ_WEB_ERR_NOMEM;
        
        memset(session, 0, sizeof(yq_web_session));
        session->struct_size = sizeof(yq_web_session);
        session->session_id = time(NULL);
        session->user_id = 1;
        session->created_at = time(NULL);
        session->last_access = time(NULL);
        session->expires_at = time(NULL) + 3600; /* 1 hour */
        session->access_count = 1;
        session->active = 1;
        session->admin = 1;
        
        snprintf(session->session_id_str, YQ_WEB_MAX_SESSION_LEN, "%u", session->session_id);
        strncpy(session->username, username, YQ_WEB_MAX_USERNAME_LEN - 1);
        strncpy(session->ip_address, "127.0.0.1", 63);
        
        *out_session = session;
        return YQ_WEB_OK;
    }
    
    return YQ_WEB_ERR_AUTH;
}

int yq_web_auth_logout(yq_web_session *session) {
    if (!session) return YQ_WEB_ERR_INVAL;
    
    /* Remove session from list */
    pthread_mutex_lock(&g_web_server.mutex);
    
    if (session->prev) {
        session->prev->next = session->next;
    } else {
        g_web_server.sessions = session->next;
    }
    
    if (session->next) {
        session->next->prev = session->prev;
    }
    
    pthread_mutex_unlock(&g_web_server.mutex);
    
    free(session);
    return YQ_WEB_OK;
}

int yq_web_auth_validate(yq_web_request *request, yq_web_session **out_session) {
    if (!request) return YQ_WEB_ERR_INVAL;
    
    /* Check session cookie */
    const char *cookie = strstr(request->body, "session_id=");
    if (cookie) {
        cookie += 11; /* Skip "session_id=" */
        const char *end = strchr(cookie, ';');
        if (end) {
            int len = end - cookie;
            char session_id_str[YQ_WEB_MAX_SESSION_LEN];
            strncpy(session_id_str, cookie, len);
            session_id_str[len] = '\0';
            
            /* Find session */
            pthread_mutex_lock(&g_web_server.mutex);
            yq_web_session *session = g_web_server.sessions;
            while (session) {
                if (strcmp(session->session_id_str, session_id_str) == 0) {
                    session->last_access = time(NULL);
                    session->access_count++;
                    
                    if (out_session) {
                        *out_session = session;
                    }
                    
                    pthread_mutex_unlock(&g_web_server.mutex);
                    return YQ_WEB_OK;
                }
                session = session->next;
            }
            pthread_mutex_unlock(&g_web_server.mutex);
        }
    }
    
    /* Check for basic auth */
    const char *auth = strstr(request->body, "Authorization: Basic ");
    if (auth) {
        auth += 21; /* Skip "Authorization: Basic " */
        /* In a real implementation, decode base64 and validate */
        return YQ_WEB_OK;
    }
    
    return YQ_WEB_ERR_AUTH;
}

/* Response functions */
int yq_web_response_set_status(yq_web_response *response, yq_web_status status) {
    if (!response) return YQ_WEB_ERR_INVAL;
    response->status = status;
    return YQ_WEB_OK;
}

int yq_web_response_set_content_type(yq_web_response *response, yq_web_content_type content_type) {
    if (!response) return YQ_WEB_ERR_INVAL;
    response->content_type = content_type;
    return YQ_WEB_OK;
}

int yq_web_response_set_header(yq_web_response *response, const char *name, const char *value) {
    if (!response || !name || !value) return YQ_WEB_ERR_INVAL;
    
    /* Append header to headers buffer */
    char header[YQ_WEB_MAX_HEADER_LEN];
    snprintf(header, sizeof(header), "%s: %s\r\n", name, value);
    
    strncat(response->headers, header, sizeof(response->headers) - strlen(response->headers) - 1);
    response->headers_count++;
    
    return YQ_WEB_OK;
}

int yq_web_response_set_cookie(yq_web_response *response, const char *name, const char *value, int max_age) {
    if (!response || !name || !value) return YQ_WEB_ERR_INVAL;
    
    char cookie[YQ_WEB_MAX_HEADER_LEN];
    snprintf(cookie, sizeof(cookie), "%s=%s; Max-Age=%d; Path=/\r\n", name, value, max_age);
    
    strncat(response->cookies, cookie, sizeof(response->cookies) - strlen(response->cookies) - 1);
    response->cookies_count++;
    
    return YQ_WEB_OK;
}

int yq_web_response_set_body(yq_web_response *response, const char *body, size_t length) {
    if (!response || !body) return YQ_WEB_ERR_INVAL;
    
    if (length == 0) {
        length = strlen(body);
    }
    
    if (length >= YQ_WEB_MAX_BODY_LEN) {
        length = YQ_WEB_MAX_BODY_LEN - 1;
    }
    
    strncpy(response->body, body, length);
    response->body[length] = '\0';
    response->content_length = length;
    
    return YQ_WEB_OK;
}

int yq_web_response_send(yq_web_response *response) {
    if (!response) return YQ_WEB_ERR_INVAL;
    
    /* This would send the response in a real implementation */
    /* For now, just log it */
    yq_web_log("Response: %d %s, Content-Length: %zu", 
               response->status, yq_web_status_to_string(response->status), response->content_length);
    
    return YQ_WEB_OK;
}

/* Utility functions */
const char *yq_web_status_to_string(yq_web_status status) {
    switch (status) {
        case YQ_WEB_STATUS_OK: return "OK";
        case YQ_WEB_STATUS_CREATED: return "Created";
        case YQ_WEB_STATUS_ACCEPTED: return "Accepted";
        case YQ_WEB_STATUS_NO_CONTENT: return "No Content";
        case YQ_WEB_STATUS_BAD_REQUEST: return "Bad Request";
        case YQ_WEB_STATUS_UNAUTHORIZED: return "Unauthorized";
        case YQ_WEB_STATUS_FORBIDDEN: return "Forbidden";
        case YQ_WEB_STATUS_NOT_FOUND: return "Not Found";
        case YQ_WEB_STATUS_METHOD_NOT_ALLOWED: return "Method Not Allowed";
        case YQ_WEB_STATUS_CONFLICT: return "Conflict";
        case YQ_WEB_STATUS_UNPROCESSABLE_ENTITY: return "Unprocessable Entity";
        case YQ_WEB_STATUS_TOO_MANY_REQUESTS: return "Too Many Requests";
        case YQ_WEB_STATUS_INTERNAL_SERVER_ERROR: return "Internal Server Error";
        case YQ_WEB_STATUS_BAD_GATEWAY: return "Bad Gateway";
        case YQ_WEB_STATUS_SERVICE_UNAVAILABLE: return "Service Unavailable";
        case YQ_WEB_STATUS_GATEWAY_TIMEOUT: return "Gateway Timeout";
        default: return "Unknown";
    }
}

const char *yq_web_method_to_string(yq_web_method method) {
    switch (method) {
        case YQ_WEB_METHOD_GET: return "GET";
        case YQ_WEB_METHOD_POST: return "POST";
        case YQ_WEB_METHOD_PUT: return "PUT";
        case YQ_WEB_METHOD_DELETE: return "DELETE";
        case YQ_WEB_METHOD_PATCH: return "PATCH";
        case YQ_WEB_METHOD_HEAD: return "HEAD";
        case YQ_WEB_METHOD_OPTIONS: return "OPTIONS";
        default: return "UNKNOWN";
    }
}

const char *yq_web_content_type_to_string(yq_web_content_type content_type) {
    switch (content_type) {
        case YQ_WEB_CONTENT_TYPE_JSON: return "application/json";
        case YQ_WEB_CONTENT_TYPE_HTML: return "text/html";
        case YQ_WEB_CONTENT_TYPE_TEXT: return "text/plain";
        case YQ_WEB_CONTENT_TYPE_XML: return "application/xml";
        case YQ_WEB_CONTENT_TYPE_CSV: return "text/csv";
        case YQ_WEB_CONTENT_TYPE_JPEG: return "image/jpeg";
        case YQ_WEB_CONTENT_TYPE_PNG: return "image/png";
        case YQ_WEB_CONTENT_TYPE_PDF: return "application/pdf";
        case YQ_WEB_CONTENT_TYPE_ZIP: return "application/zip";
        default: return "application/octet-stream";
    }
}





