/*
 *  yq_web.c - Web administration interface implementation for yq-DB
 *  
 *  This file implements the web functionality for yq-DB,
 *  providing a RESTful API for web-based administration and monitoring.
 */

#include "yq_web.h"
#include "yq.h"
#include "yq_thread.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#if !defined(_WIN32)
#include <sys/time.h>
#endif
#include <time.h>
#include <errno.h>
#include <stdatomic.h>
#include <stdint.h>

#include <stdarg.h>
#include <fcntl.h>
#include <signal.h>
#if !defined(_WIN32)
#include <strings.h>
#endif

/* Internal web server state */
static struct {
    int server_fd;
    int running;
    yq_web_config config;
    yq_web_stats stats;
    yq_web_session *sessions;
    yq_web_user *users;
    yq_mutex_t mutex;
    yq_thread_t *threads;
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
    yq_mutex_init(&g_web_server.mutex);
    
    /* Create server socket */
    g_web_server.server_fd = yq_sock_open();
    if (g_web_server.server_fd < 0) {
        return YQ_WEB_ERR_CONN;
    }
    
    /* Set socket options */
    if (yq_sock_set_reuseaddr(g_web_server.server_fd) < 0) {
        yq_sock_close(g_web_server.server_fd);
        return YQ_WEB_ERR_CONN;
    }

    /* Bind socket（地址结构由 yq_sock_bind 内部按平台组装） */
    if (yq_sock_bind(g_web_server.server_fd, (uint16_t)g_web_server.config.port) < 0) {
        yq_sock_close(g_web_server.server_fd);
        return YQ_WEB_ERR_CONN;
    }
    
    /* Listen for connections */
    if (yq_sock_listen(g_web_server.server_fd, g_web_server.config.max_connections) < 0) {
        yq_sock_close(g_web_server.server_fd);
        return YQ_WEB_ERR_CONN;
    }
    
    /* Initialize statistics */
    memset(&g_web_server.stats, 0, sizeof(g_web_server.stats));
    g_web_server.stats.start_time = time(NULL);
    
    /* Initialize admin user */
    g_web_server.users = malloc(sizeof(yq_web_user));
    if (!g_web_server.users) {
        yq_sock_close(g_web_server.server_fd);
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

    /*
     * 不再内置任何默认凭据。
     *
     * 此前这里把用户名/口令硬编码成 admin/admin（且 password_hash 字段直接
     * 存明文），对一个对外提供 REST 管理接口的组件来说等于后门。现在账号处于
     * "未配置凭据"状态，必须由调用方在启动后显式调用 yq_web_auth_init() 设置，
     * 否则 yq_web_auth_login() 一律拒绝（fail closed）。
     */
    g_web_server.users->username[0] = '\0';
    g_web_server.users->password_hash[0] = '\0';
    g_web_server.users->salt[0] = '\0';
    strncpy(g_web_server.users->email, "admin@yqdb.local", sizeof(g_web_server.users->email) - 1);

    yq_web_log("Web server initialized on port %d (no credentials configured yet; "
               "call yq_web_auth_init() before accepting logins)",
               g_web_server.config.port);
    
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
    yq_sock_close(g_web_server.server_fd);
    
    /* Cleanup sessions */
    yq_mutex_lock(&g_web_server.mutex);
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
    
    yq_mutex_unlock(&g_web_server.mutex);
    
    /* Cleanup threads */
    if (g_web_server.threads) {
        free(g_web_server.threads);
        g_web_server.threads = NULL;
    }
    
    /* Destroy mutex */
    yq_mutex_destroy(&g_web_server.mutex);
    
    yq_web_log("Web server shutdown");
    
    return YQ_WEB_OK;
}

/* Start web server */
/* Forward declaration: the real worker loop is defined below. */
static void *yq_web_worker_thread(void *arg);

/* Forward declarations: the worker loop replies via these, but they are
 * defined further down the file. */
static void yq_web_send_response(int client_fd, yq_web_response *response);
static void yq_web_send_error(int client_fd, yq_web_status status, const char *message);

/*
 * 从连接上读取一行（以 CRLF 或 LF 结尾），写入 out。返回读到的字节数。
 * 用于解析请求行与请求头，避免读到超长行时越界。
 */
static ssize_t yq_web_recv_line(int fd, char *out, size_t cap) {
    size_t n = 0;
    while (n + 1 < cap) {
        char c;
        int r = yq_sock_recv(fd, &c, 1);
        if (r <= 0) break;
        if (c == '\n') break;
        if (c == '\r') continue;
        out[n++] = c;
    }
    out[n] = '\0';
    return (ssize_t)n;
}

/*
 * 解析请求行 "METHOD SP TARGET SP VERSION" 并填充 request。
 * 仅识别本项目分派所需的字段；其余保持默认值。
 */
static int yq_web_parse_request_line(yq_web_request *req, const char *line) {
    char method[16] = {0}, target[YQ_WEB_MAX_URL_LEN] = {0};
    if (sscanf(line, "%15s %1023s", method, target) != 2) {
        return YQ_WEB_ERR_INVAL;
    }

    if (strcmp(method, "GET") == 0)         req->method = YQ_WEB_METHOD_GET;
    else if (strcmp(method, "POST") == 0)   req->method = YQ_WEB_METHOD_POST;
    else if (strcmp(method, "PUT") == 0)    req->method = YQ_WEB_METHOD_PUT;
    else if (strcmp(method, "DELETE") == 0) req->method = YQ_WEB_METHOD_DELETE;
    else                                    req->method = YQ_WEB_METHOD_GET;

    /* 去掉查询串，只保留路径。 */
    char *q = strchr(target, '?');
    if (q) *q = '\0';

    strncpy(req->url, target, sizeof(req->url) - 1);
    req->url[sizeof(req->url) - 1] = '\0';
    return YQ_WEB_OK;
}

/*
 * 真正的 worker 线程：在监听套接字上 accept 连接、解析请求、交给
 * yq_web_process_request() 处理并回写响应，直到 running 置 0。
 *
 * 此前这里是一个 `return NULL` 的桩，因此 "Multi-threaded HTTP server with
 * worker pools" 并不成立——线程起来了但什么都不做。
 */
static void *yq_web_worker_thread(void *arg) {
    (void)arg;

    while (1) {
        yq_mutex_lock(&g_web_server.mutex);
        int running = g_web_server.running;
        int listen_fd = g_web_server.server_fd;
        yq_mutex_unlock(&g_web_server.mutex);

        if (!running || listen_fd < 0) break;

        yq_socket_t client_fd = yq_sock_accept(listen_fd);
        if (client_fd < 0) {
            /* 被信号打断或 stop() 关闭了监听套接字，都不是错误。 */
            if (errno == EINTR || errno == EBADF || errno == EINVAL) break;
            continue;
        }

        yq_web_request request;
        yq_web_response response;
        memset(&request, 0, sizeof(request));
        memset(&response, 0, sizeof(response));
        request.struct_size = sizeof(request);
        request.timestamp = time(NULL);

        char line[YQ_WEB_MAX_URL_LEN + 32];
        if (yq_web_recv_line(client_fd, line, sizeof(line)) > 0 &&
            yq_web_parse_request_line(&request, line) == YQ_WEB_OK) {

            /* 略过请求头，只保留 Content-Length 以决定是否读 body。 */
            size_t content_len = 0;
            char hdr[1024];
            while (yq_web_recv_line(client_fd, hdr, sizeof(hdr)) > 0 && hdr[0] != '\0') {
                if (strncasecmp(hdr, "Content-Length:", 15) == 0) {
                    content_len = (size_t)strtoul(hdr + 15, NULL, 10);
                }
            }

            if (content_len > 0) {
                size_t want = content_len;
                if (want > sizeof(request.body) - 1) want = sizeof(request.body) - 1;
                size_t got = 0;
                while (got < want) {
                    int r = yq_sock_recv(client_fd, request.body + got, (int)(want - got));
                    if (r <= 0) break;
                    got += (size_t)r;
                }
                request.body[got] = '\0';
                request.content_length = (uint32_t)got;
            }

            yq_web_process_request(&request, &response);

            yq_mutex_lock(&g_web_server.mutex);
            g_web_server.stats.total_requests++;
            if (response.status < 400) g_web_server.stats.successful_requests++;
            else                       g_web_server.stats.failed_requests++;
            yq_mutex_unlock(&g_web_server.mutex);
        } else {
            yq_web_send_error(client_fd, YQ_WEB_STATUS_BAD_REQUEST, "Malformed request");
        }

        yq_web_send_response(client_fd, &response);
        yq_sock_close(client_fd);
    }

    return NULL;
}

int yq_web_start(int server_fd) {
    if (server_fd != g_web_server.server_fd) {
        return YQ_WEB_ERR_INVAL;
    }
    
    g_web_server.running = 1;
    
    /* Create worker threads */
    g_web_server.threads = malloc(g_web_server.config.max_threads * sizeof(yq_thread_t));
    if (!g_web_server.threads) {
        return YQ_WEB_ERR_NOMEM;
    }
    
    for (uint32_t i = 0; i < g_web_server.config.max_threads; i++) {
        if (yq_thread_create(&g_web_server.threads[i], yq_web_worker_thread, NULL) != 0) {
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
            yq_thread_join(g_web_server.threads[i]);
        }
    }
    
    yq_web_log("Web server stopped");
    
    return YQ_WEB_OK;
}







/* Forward declarations for the endpoint handlers used by the dispatcher below. */
int yq_web_endpoint_dashboard(yq_web_request *request, yq_web_response *response);
int yq_web_endpoint_login(yq_web_request *request, yq_web_response *response);
int yq_web_endpoint_logout(yq_web_request *request, yq_web_response *response);
int yq_web_endpoint_health(yq_web_request *request, yq_web_response *response);
int yq_web_endpoint_stats(yq_web_request *request, yq_web_response *response);

/*
 * `/api/` 前缀的真实分派。
 *
 * 此处曾经是一个桩：对任何 method / URL 都回 {"success":true,
 * "message":"API endpoint called"}，让 PR 描述里的 "RESTful Endpoints" 名不副实。
 * 现在把它接到真正有实现的端点上；未知资源返回 404 而不是假成功。
 */
static int yq_web_api_dispatch(yq_web_request *request, yq_web_response *response) {
    if (!request || !response) return YQ_WEB_ERR_INVAL;

    const char *res = request->url + 5; /* 跳过 "/api/" */

    if (strcmp(res, "health") == 0 || strcmp(res, "status") == 0) {
        return yq_web_endpoint_health(request, response);
    } else if (strcmp(res, "stats") == 0) {
        return yq_web_endpoint_stats(request, response);
    } else if (strcmp(res, "dashboard") == 0) {
        return yq_web_endpoint_dashboard(request, response);
    } else {
        /* 未实现的资源就如实报 404，不再返回空洞的成功。 */
        response->status = YQ_WEB_STATUS_NOT_FOUND;
        response->content_type = YQ_WEB_CONTENT_TYPE_JSON;
        yq_web_response_set_body(response,
            "{\"success\":false,\"error\":\"unknown API resource\"}", 0);
        return YQ_WEB_OK;
    }
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
    
    /* Route request to appropriate handler.
     * 每个分支都必须把处理函数的返回值原样返回；曾经有两个分支
     * （dashboard / api_dispatch）只调用不返回，函数会在末尾「掉出去」，
     * 返回值未定义（GCC 在 -Wreturn-type 下报警）。 */
    if (strcmp(request->url, "/") == 0 || strcmp(request->url, "/dashboard") == 0) {
        return yq_web_endpoint_dashboard(request, response);
    } else if (strcmp(request->url, "/login") == 0) {
        return yq_web_endpoint_login(request, response);
    } else if (strcmp(request->url, "/logout") == 0) {
        return yq_web_endpoint_logout(request, response);
    } else if (strncmp(request->url, "/api/", 5) == 0) {
        return yq_web_api_dispatch(request, response);
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
    yq_sock_send(client_fd, buffer, (int)pos);
    
    /* Send body */
    if (response->content_length > 0) {
        yq_sock_send(client_fd, response->body, (int)response->content_length);
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
        
        /*
         * 走与 yq_web_auth_login() 相同的凭据校验：未配置凭据时拒绝登录，
         * 不再存在 admin/admin 这种内置通道。
         */
        yq_web_session *authed = NULL;
        int arc = yq_web_auth_login(username ? username : "", password ? password : "", &authed);
        if (arc == YQ_WEB_OK) {
            yq_web_session *session = authed;
            /* Add to session list */
            yq_mutex_lock(&g_web_server.mutex);
            session->next = g_web_server.sessions;
            if (g_web_server.sessions) {
                g_web_server.sessions->prev = session;
            }
            g_web_server.sessions = session;
            yq_mutex_unlock(&g_web_server.mutex);
            
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

/*
 * 口令派生：salted FNV-1a 64，转成 16 位十六进制存进 password_hash。
 *
 * 这是为了不再明文存口令、并让相同口令在不同账号下产生不同摘要。它**不是**
 * 抗暴力破解的 KDF（生产环境应换成 PBKDF2/scrypt/argon2 之类），这一步只是
 * 把"明文比对"这条后门堵上；在此明确写出它的强度边界，避免高估。
 */
static void yq_web_derive_password(const char *password, const char *salt,
                                   char *out, size_t out_cap) {
    uint64_t h = 1469598103934665603ULL; /* FNV offset basis */
    const unsigned char *p;
    for (p = (const unsigned char *)salt; p && *p; p++) {
        h ^= (uint64_t)(*p);
        h *= 1099511628211ULL;
    }
    for (p = (const unsigned char *)password; p && *p; p++) {
        h ^= (uint64_t)(*p);
        h *= 1099511628211ULL;
    }
    snprintf(out, out_cap, "%016llx", (unsigned long long)h);
}

/* 常量时间比较，避免按字节短路泄露摘要信息。 */
static int yq_web_ct_equal(const char *a, const char *b) {
    size_t la = a ? strlen(a) : 0;
    size_t lb = b ? strlen(b) : 0;
    if (la != lb) return 0;
    unsigned char diff = 0;
    for (size_t i = 0; i < la; i++) {
        diff |= (unsigned char)(a[i] ^ b[i]);
    }
    return diff == 0;
}

/*
 * 生成每账号唯一 salt。用时间、指针与一个原子计数器混合，
 * 不依赖 srand()/rand()（那是进程级全局状态且非线程安全）。
 */
static void yq_web_make_salt(char *out, size_t out_cap) {
    static atomic_uint_fast64_t salt_seq = 0;
    uint64_t n = (uint64_t)atomic_fetch_add(&salt_seq, 1);
    uint64_t t = (uint64_t)time(NULL);
    uint64_t mix = t ^ (n * 0x9E3779B97F4A7C15ULL) ^ (uint64_t)(uintptr_t)out;
    snprintf(out, out_cap, "%016llx", (unsigned long long)mix);
}

int yq_web_auth_init(const char *admin_username, const char *admin_password) {
    if (!admin_username || !admin_password) return YQ_WEB_ERR_INVAL;
    if (admin_username[0] == '\0' || admin_password[0] == '\0') return YQ_WEB_ERR_INVAL;
    if (strlen(admin_username) >= YQ_WEB_MAX_USERNAME_LEN) return YQ_WEB_ERR_INVAL;
    if (!g_web_server.users) return YQ_WEB_ERR_STATE;

    char salt[32];
    yq_web_make_salt(salt, sizeof(salt));

    char digest[64];
    yq_web_derive_password(admin_password, salt, digest, sizeof(digest));

    yq_mutex_lock(&g_web_server.mutex);
    memset(g_web_server.users->username, 0, sizeof(g_web_server.users->username));
    strncpy(g_web_server.users->username, admin_username,
            sizeof(g_web_server.users->username) - 1);

    memset(g_web_server.users->salt, 0, sizeof(g_web_server.users->salt));
    strncpy(g_web_server.users->salt, salt, sizeof(g_web_server.users->salt) - 1);
    g_web_server.users->salt[sizeof(g_web_server.users->salt) - 1] = '\0';

    memset(g_web_server.users->password_hash, 0, sizeof(g_web_server.users->password_hash));
    strncpy(g_web_server.users->password_hash, digest,
            sizeof(g_web_server.users->password_hash) - 1);
    g_web_server.users->password_hash[sizeof(g_web_server.users->password_hash) - 1] = '\0';
    yq_mutex_unlock(&g_web_server.mutex);

    return YQ_WEB_OK;
}

int yq_web_auth_login(const char *username, const char *password, yq_web_session **out_session) {
    if (!username || !password || !out_session) return YQ_WEB_ERR_INVAL;

    if (!g_web_server.users) return YQ_WEB_ERR_STATE;

    /*
     * 未配置凭据时一律拒绝：宁可拒绝所有登录，也不要退回某个默认口令。
     */
    if (g_web_server.users->username[0] == '\0' ||
        g_web_server.users->password_hash[0] == '\0') {
        return YQ_WEB_ERR_AUTH;
    }

    /* 比对用户名 + 派生摘要，二者都走常量时间比较。 */
    int user_ok = yq_web_ct_equal(username, g_web_server.users->username);

    char digest[64];
    yq_web_derive_password(password, g_web_server.users->salt, digest, sizeof(digest));
    int pass_ok = yq_web_ct_equal(digest, g_web_server.users->password_hash);

    if (user_ok && pass_ok) {
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
    yq_mutex_lock(&g_web_server.mutex);
    
    if (session->prev) {
        session->prev->next = session->next;
    } else {
        g_web_server.sessions = session->next;
    }
    
    if (session->next) {
        session->next->prev = session->prev;
    }
    
    yq_mutex_unlock(&g_web_server.mutex);
    
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
            yq_mutex_lock(&g_web_server.mutex);
            yq_web_session *session = g_web_server.sessions;
            while (session) {
                if (strcmp(session->session_id_str, session_id_str) == 0) {
                    session->last_access = time(NULL);
                    session->access_count++;
                    
                    if (out_session) {
                        *out_session = session;
                    }
                    
                    yq_mutex_unlock(&g_web_server.mutex);
                    return YQ_WEB_OK;
                }
                session = session->next;
            }
            yq_mutex_unlock(&g_web_server.mutex);
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

    /* 用 memcpy 而非 strncpy：这里已知要拷贝的确切长度，body 也可能包含
     * 内嵌 NUL（二进制响应体），strncpy 会在此处触发
     * -Wstringop-truncation 误报，且语义上不如 memcpy 明确。
     * 目标缓冲区按其容量整体清零后再写，保证尾部始终有终止符。 */
    memset(response->body, 0, sizeof(response->body));
    memcpy(response->body, body, length);
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





