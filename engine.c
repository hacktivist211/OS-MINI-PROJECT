#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "monitor_ioctl.h"

#define STACK_SIZE (1024 * 1024)
#define CONTAINER_ID_LEN 32
#define CONTROL_PATH "/tmp/mini_runtime.sock"
#define LOG_DIR "logs"
#define CONTROL_MESSAGE_LEN 256
#define CHILD_COMMAND_LEN 256
#define LOG_CHUNK_SIZE 4096
#define LOG_BUFFER_CAPACITY 16
#define DEFAULT_SOFT_LIMIT (40UL << 20)
#define DEFAULT_HARD_LIMIT (64UL << 20)
#define MONITOR_DEV "/dev/container_monitor"

typedef enum {
    CMD_SUPERVISOR = 0,
    CMD_START,
    CMD_RUN,
    CMD_PS,
    CMD_LOGS,
    CMD_STOP
} command_kind_t;

typedef enum {
    CONTAINER_STARTING = 0,
    CONTAINER_RUNNING,
    CONTAINER_STOPPED,
    CONTAINER_KILLED,
    CONTAINER_EXITED
} container_state_t;

typedef struct container_record {
    char id[CONTAINER_ID_LEN];
    pid_t host_pid;
    time_t started_at;
    container_state_t state;
    unsigned long soft_limit_bytes;
    unsigned long hard_limit_bytes;
    int exit_code;
    int exit_signal;
    int stop_requested;
    char termination_reason[32];
    char log_path[PATH_MAX];
    struct container_record *next;
} container_record_t;

typedef struct {
    char container_id[CONTAINER_ID_LEN];
    size_t length;
    char data[LOG_CHUNK_SIZE];
} log_item_t;

typedef struct {
    log_item_t items[LOG_BUFFER_CAPACITY];
    size_t head;
    size_t tail;
    size_t count;
    int shutting_down;
    pthread_mutex_t mutex;
    pthread_cond_t not_empty;
    pthread_cond_t not_full;
} bounded_buffer_t;

typedef struct {
    command_kind_t kind;
    char container_id[CONTAINER_ID_LEN];
    char rootfs[PATH_MAX];
    char command[CHILD_COMMAND_LEN];
    unsigned long soft_limit_bytes;
    unsigned long hard_limit_bytes;
    int nice_value;
} control_request_t;

typedef struct {
    int status;
    char message[CONTROL_MESSAGE_LEN];
} control_response_t;

typedef struct {
    char id[CONTAINER_ID_LEN];
    char rootfs[PATH_MAX];
    char command[CHILD_COMMAND_LEN];
    int nice_value;
    int log_write_fd;
} child_config_t;

typedef struct {
    int server_fd;
    int monitor_fd;
    int should_stop;
    pthread_t logger_thread;
    bounded_buffer_t log_buffer;
    pthread_mutex_t metadata_lock;
    container_record_t *containers;
} supervisor_ctx_t;

static supervisor_ctx_t *g_ctx = NULL;

static void usage(const char *prog)
{
    fprintf(stderr,
            "Usage:\n"
            "  %s supervisor <base-rootfs>\n"
            "  %s start <id> <container-rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]\n"
            "  %s run <id> <container-rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]\n"
            "  %s ps\n"
            "  %s logs <id>\n"
            "  %s stop <id>\n",
            prog, prog, prog, prog, prog, prog);
}

static int parse_mib_flag(const char *flag,
                          const char *value,
                          unsigned long *target_bytes)
{
    char *end = NULL;
    unsigned long mib;

    errno = 0;
    mib = strtoul(value, &end, 10);
    if (errno != 0 || end == value || *end != '\0') {
        fprintf(stderr, "Invalid value for %s: %s\n", flag, value);
        return -1;
    }

    if (mib > ULONG_MAX / (1UL << 20)) {
        fprintf(stderr, "Value for %s is too large: %s\n", flag, value);
        return -1;
    }

    *target_bytes = mib * (1UL << 20);
    return 0;
}

static int parse_optional_flags(control_request_t *req,
                                int argc,
                                char *argv[],
                                int start_index)
{
    int i;

    for (i = start_index; i < argc; i += 2) {
        char *end = NULL;
        long nice_value;

        if (i + 1 >= argc) {
            fprintf(stderr, "Missing value for option: %s\n", argv[i]);
            return -1;
        }

        if (strcmp(argv[i], "--soft-mib") == 0) {
            if (parse_mib_flag("--soft-mib", argv[i + 1], &req->soft_limit_bytes) != 0)
                return -1;
            continue;
        }

        if (strcmp(argv[i], "--hard-mib") == 0) {
            if (parse_mib_flag("--hard-mib", argv[i + 1], &req->hard_limit_bytes) != 0)
                return -1;
            continue;
        }

        if (strcmp(argv[i], "--nice") == 0) {
            errno = 0;
            nice_value = strtol(argv[i + 1], &end, 10);
            if (errno != 0 || end == argv[i + 1] || *end != '\0' ||
                nice_value < -20 || nice_value > 19) {
                fprintf(stderr,
                        "Invalid value for --nice (expected -20..19): %s\n",
                        argv[i + 1]);
                return -1;
            }
            req->nice_value = (int)nice_value;
            continue;
        }

        fprintf(stderr, "Unknown option: %s\n", argv[i]);
        return -1;
    }

    if (req->soft_limit_bytes > req->hard_limit_bytes) {
        fprintf(stderr, "Invalid limits: soft limit cannot exceed hard limit\n");
        return -1;
    }

    return 0;
}

static const char *state_to_string(container_state_t state)
{
    switch (state) {
    case CONTAINER_STARTING:
        return "starting";
    case CONTAINER_RUNNING:
        return "running";
    case CONTAINER_STOPPED:
        return "stopped";
    case CONTAINER_KILLED:
        return "killed";
    case CONTAINER_EXITED:
        return "exited";
    default:
        return "unknown";
    }
}

static int bounded_buffer_init(bounded_buffer_t *buffer)
{
    int rc;

    memset(buffer, 0, sizeof(*buffer));

    rc = pthread_mutex_init(&buffer->mutex, NULL);
    if (rc != 0)
        return rc;

    rc = pthread_cond_init(&buffer->not_empty, NULL);
    if (rc != 0) {
        pthread_mutex_destroy(&buffer->mutex);
        return rc;
    }

    rc = pthread_cond_init(&buffer->not_full, NULL);
    if (rc != 0) {
        pthread_cond_destroy(&buffer->not_empty);
        pthread_mutex_destroy(&buffer->mutex);
        return rc;
    }

    return 0;
}

static void bounded_buffer_destroy(bounded_buffer_t *buffer)
{
    pthread_cond_destroy(&buffer->not_full);
    pthread_cond_destroy(&buffer->not_empty);
    pthread_mutex_destroy(&buffer->mutex);
}

static void bounded_buffer_begin_shutdown(bounded_buffer_t *buffer)
{
    pthread_mutex_lock(&buffer->mutex);
    buffer->shutting_down = 1;
    pthread_cond_broadcast(&buffer->not_empty);
    pthread_cond_broadcast(&buffer->not_full);
    pthread_mutex_unlock(&buffer->mutex);
}

int bounded_buffer_push(bounded_buffer_t *buffer, const log_item_t *item)
{
    pthread_mutex_lock(&buffer->mutex);

    while (buffer->count == LOG_BUFFER_CAPACITY && !buffer->shutting_down)
        pthread_cond_wait(&buffer->not_full, &buffer->mutex);

    if (buffer->shutting_down) {
        pthread_mutex_unlock(&buffer->mutex);
        return -1;
    }

    buffer->items[buffer->tail] = *item;
    buffer->tail = (buffer->tail + 1) % LOG_BUFFER_CAPACITY;
    buffer->count++;

    pthread_cond_signal(&buffer->not_empty);
    pthread_mutex_unlock(&buffer->mutex);
    return 0;
}

int bounded_buffer_pop(bounded_buffer_t *buffer, log_item_t *item)
{
    pthread_mutex_lock(&buffer->mutex);

    while (buffer->count == 0 && !buffer->shutting_down)
        pthread_cond_wait(&buffer->not_empty, &buffer->mutex);

    if (buffer->count == 0) {
        pthread_mutex_unlock(&buffer->mutex);
        return -1;
    }

    *item = buffer->items[buffer->head];
    buffer->head = (buffer->head + 1) % LOG_BUFFER_CAPACITY;
    buffer->count--;

    pthread_cond_signal(&buffer->not_full);
    pthread_mutex_unlock(&buffer->mutex);
    return 0;
}

static container_record_t *find_container_locked(supervisor_ctx_t *ctx,
                                                  const char *id)
{
    container_record_t *rec = ctx->containers;
    while (rec) {
        if (strncmp(rec->id, id, CONTAINER_ID_LEN) == 0)
            return rec;
        rec = rec->next;
    }
    return NULL;
}

void *logging_thread(void *arg)
{
    supervisor_ctx_t *ctx = (supervisor_ctx_t *)arg;
    log_item_t item;
    char log_path[PATH_MAX];
    int fd;
    container_record_t *rec;

    while (1) {
        if (bounded_buffer_pop(&ctx->log_buffer, &item) != 0)
            break;

        pthread_mutex_lock(&ctx->metadata_lock);
        rec = find_container_locked(ctx, item.container_id);
        if (rec)
            strncpy(log_path, rec->log_path, PATH_MAX - 1);
        else
            log_path[0] = '\0';
        pthread_mutex_unlock(&ctx->metadata_lock);

        if (log_path[0] == '\0')
            continue;

        fd = open(log_path, O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (fd < 0)
            continue;

        write(fd, item.data, item.length);
        close(fd);
    }

    while (bounded_buffer_pop(&ctx->log_buffer, &item) == 0) {
        pthread_mutex_lock(&ctx->metadata_lock);
        rec = find_container_locked(ctx, item.container_id);
        if (rec)
            strncpy(log_path, rec->log_path, PATH_MAX - 1);
        else
            log_path[0] = '\0';
        pthread_mutex_unlock(&ctx->metadata_lock);

        if (log_path[0] == '\0')
            continue;

        fd = open(log_path, O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (fd < 0)
            continue;

        write(fd, item.data, item.length);
        close(fd);
    }

    return NULL;
}

typedef struct {
    supervisor_ctx_t *ctx;
    char container_id[CONTAINER_ID_LEN];
    int read_fd;
} log_reader_args_t;

static void *log_reader_thread(void *arg)
{
    log_reader_args_t *args = (log_reader_args_t *)arg;
    log_item_t item;
    ssize_t n;

    memset(&item, 0, sizeof(item));
    strncpy(item.container_id, args->container_id, CONTAINER_ID_LEN - 1);

    while ((n = read(args->read_fd, item.data, LOG_CHUNK_SIZE)) > 0) {
        item.length = (size_t)n;
        bounded_buffer_push(&args->ctx->log_buffer, &item);
    }

    close(args->read_fd);
    free(args);
    return NULL;
}

int child_fn(void *arg)
{
    child_config_t *cfg = (child_config_t *)arg;

    if (sethostname(cfg->id, strlen(cfg->id)) != 0) {
        perror("sethostname");
        return 1;
    }

    if (chroot(cfg->rootfs) != 0) {
        perror("chroot");
        return 1;
    }

    if (chdir("/") != 0) {
        perror("chdir");
        return 1;
    }

    if (mount("proc", "/proc", "proc",
              MS_NOSUID | MS_NODEV | MS_NOEXEC, NULL) != 0) {
        perror("mount proc");
    }

    if (cfg->log_write_fd >= 0) {
        dup2(cfg->log_write_fd, STDOUT_FILENO);
        dup2(cfg->log_write_fd, STDERR_FILENO);
        close(cfg->log_write_fd);
    }

    if (cfg->nice_value != 0)
        nice(cfg->nice_value);

    char *const argv_exec[] = {
        "/bin/sh", "-c", cfg->command, NULL
    };

    execv("/bin/sh", argv_exec);
    perror("execv");
    return 1;
}

int register_with_monitor(int monitor_fd,
                          const char *container_id,
                          pid_t host_pid,
                          unsigned long soft_limit_bytes,
                          unsigned long hard_limit_bytes)
{
    struct monitor_request req;

    memset(&req, 0, sizeof(req));
    req.pid = host_pid;
    req.soft_limit_bytes = soft_limit_bytes;
    req.hard_limit_bytes = hard_limit_bytes;
    strncpy(req.container_id, container_id, sizeof(req.container_id) - 1);

    if (ioctl(monitor_fd, MONITOR_REGISTER, &req) < 0)
        return -1;

    return 0;
}

int unregister_from_monitor(int monitor_fd, const char *container_id, pid_t host_pid)
{
    struct monitor_request req;

    memset(&req, 0, sizeof(req));
    req.pid = host_pid;
    strncpy(req.container_id, container_id, sizeof(req.container_id) - 1);

    if (ioctl(monitor_fd, MONITOR_UNREGISTER, &req) < 0)
        return -1;

    return 0;
}

static void handle_sigchld(int sig)
{
    (void)sig;
    int saved_errno = errno;
    int status;
    pid_t pid;

    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        if (!g_ctx)
            continue;

        pthread_mutex_lock(&g_ctx->metadata_lock);
        container_record_t *crec = g_ctx->containers;
        while (crec) {
            if (crec->host_pid == pid) {
                if (WIFEXITED(status)) {
                    crec->state = CONTAINER_EXITED;
                    crec->exit_code = WEXITSTATUS(status);
                    strncpy(crec->termination_reason, "exited",
                            sizeof(crec->termination_reason) - 1);
                } else if (WIFSIGNALED(status)) {
                    crec->exit_signal = WTERMSIG(status);
                    if (crec->stop_requested) {
                        crec->state = CONTAINER_STOPPED;
                        strncpy(crec->termination_reason, "stopped",
                                sizeof(crec->termination_reason) - 1);
                    } else if (crec->exit_signal == SIGKILL) {
                        crec->state = CONTAINER_KILLED;
                        strncpy(crec->termination_reason, "hard_limit_killed",
                                sizeof(crec->termination_reason) - 1);
                    } else {
                        crec->state = CONTAINER_EXITED;
                        strncpy(crec->termination_reason, "signaled",
                                sizeof(crec->termination_reason) - 1);
                    }
                }
                break;
            }
            crec = crec->next;
        }
        pthread_mutex_unlock(&g_ctx->metadata_lock);

        if (g_ctx->monitor_fd >= 0 && rec) {
            unregister_from_monitor(g_ctx->monitor_fd, rec->id, pid);
        }
    }

    errno = saved_errno;
}

static void handle_sigterm(int sig)
{
    (void)sig;
    if (g_ctx)
        g_ctx->should_stop = 1;
}

static pid_t launch_container(supervisor_ctx_t *ctx,
                               const control_request_t *req,
                               int *log_pipe_read_out)
{
    int pipefd[2];
    if (pipe(pipefd) != 0) {
        perror("pipe");
        return -1;
    }

    child_config_t *cfg = malloc(sizeof(*cfg));
    if (!cfg) {
        close(pipefd[0]);
        close(pipefd[1]);
        return -1;
    }

    memset(cfg, 0, sizeof(*cfg));
    strncpy(cfg->id, req->container_id, CONTAINER_ID_LEN - 1);
    strncpy(cfg->rootfs, req->rootfs, PATH_MAX - 1);
    strncpy(cfg->command, req->command, CHILD_COMMAND_LEN - 1);
    cfg->nice_value = req->nice_value;
    cfg->log_write_fd = pipefd[1];

    void *stack = malloc(STACK_SIZE);
    if (!stack) {
        free(cfg);
        close(pipefd[0]);
        close(pipefd[1]);
        return -1;
    }

    int flags = CLONE_NEWPID | CLONE_NEWUTS | CLONE_NEWNS | SIGCHLD;
    pid_t pid = clone(child_fn, (char *)stack + STACK_SIZE, flags, cfg);

    free(stack);
    close(pipefd[1]);

    if (pid < 0) {
        perror("clone");
        free(cfg);
        close(pipefd[0]);
        return -1;
    }

    free(cfg);

    if (log_pipe_read_out)
        *log_pipe_read_out = pipefd[0];
    else
        close(pipefd[0]);

    log_reader_args_t *largs = malloc(sizeof(*largs));
    if (largs) {
        largs->ctx = ctx;
        strncpy(largs->container_id, req->container_id, CONTAINER_ID_LEN - 1);
        largs->read_fd = pipefd[0];
        pthread_t tid;
        pthread_create(&tid, NULL, log_reader_thread, largs);
        pthread_detach(tid);
        if (log_pipe_read_out)
            *log_pipe_read_out = -1;
    }

    return pid;
}

static int handle_start(supervisor_ctx_t *ctx, const control_request_t *req,
                        control_response_t *resp)
{
    pthread_mutex_lock(&ctx->metadata_lock);
    if (find_container_locked(ctx, req->container_id)) {
        pthread_mutex_unlock(&ctx->metadata_lock);
        resp->status = -1;
        snprintf(resp->message, CONTROL_MESSAGE_LEN,
                 "container %s already exists", req->container_id);
        return 0;
    }
    pthread_mutex_unlock(&ctx->metadata_lock);

    mkdir(LOG_DIR, 0755);

    pid_t pid = launch_container(ctx, req, NULL);
    if (pid < 0) {
        resp->status = -1;
        snprintf(resp->message, CONTROL_MESSAGE_LEN, "failed to launch container");
        return 0;
    }

    container_record_t *rec = calloc(1, sizeof(*rec));
    if (!rec) {
        resp->status = -1;
        snprintf(resp->message, CONTROL_MESSAGE_LEN, "out of memory");
        return 0;
    }

    strncpy(rec->id, req->container_id, CONTAINER_ID_LEN - 1);
    rec->host_pid = pid;
    rec->started_at = time(NULL);
    rec->state = CONTAINER_RUNNING;
    rec->soft_limit_bytes = req->soft_limit_bytes;
    rec->hard_limit_bytes = req->hard_limit_bytes;
    snprintf(rec->log_path, PATH_MAX, "%s/%s.log", LOG_DIR, req->container_id);

    pthread_mutex_lock(&ctx->metadata_lock);
    rec->next = ctx->containers;
    ctx->containers = rec;
    pthread_mutex_unlock(&ctx->metadata_lock);

    if (ctx->monitor_fd >= 0) {
        register_with_monitor(ctx->monitor_fd, req->container_id, pid,
                              req->soft_limit_bytes, req->hard_limit_bytes);
    }

    resp->status = 0;
    snprintf(resp->message, CONTROL_MESSAGE_LEN,
             "started container %s pid=%d", req->container_id, pid);
    return 0;
}

static int handle_ps(supervisor_ctx_t *ctx, control_response_t *resp)
{
    char buf[4096];
    int offset = 0;
    container_record_t *rec;

    offset += snprintf(buf + offset, sizeof(buf) - offset,
                       "%-16s %-8s %-18s %-10s %-20s\n",
                       "ID", "PID", "STATE", "STARTED", "REASON");

    pthread_mutex_lock(&ctx->metadata_lock);
    rec = ctx->containers;
    while (rec && offset < (int)sizeof(buf) - 100) {
        const char *reason = rec->termination_reason[0]
                             ? rec->termination_reason : "-";
        offset += snprintf(buf + offset, sizeof(buf) - offset,
                           "%-16s %-8d %-18s %-10ld %-20s\n",
                           rec->id, rec->host_pid,
                           state_to_string(rec->state),
                           (long)rec->started_at,
                           reason);
        rec = rec->next;
    }
    pthread_mutex_unlock(&ctx->metadata_lock);

    resp->status = 0;
    strncpy(resp->message, buf, CONTROL_MESSAGE_LEN - 1);
    return 0;
}

static int handle_logs(supervisor_ctx_t *ctx, const control_request_t *req,
                       control_response_t *resp, int client_fd)
{
    char log_path[PATH_MAX];
    char chunk[4096];
    ssize_t n;
    int fd;

    pthread_mutex_lock(&ctx->metadata_lock);
    container_record_t *rec = find_container_locked(ctx, req->container_id);
    if (rec)
        strncpy(log_path, rec->log_path, PATH_MAX - 1);
    pthread_mutex_unlock(&ctx->metadata_lock);

    if (!rec) {
        resp->status = -1;
        snprintf(resp->message, CONTROL_MESSAGE_LEN,
                 "no container with id %s", req->container_id);
        return 0;
    }

    resp->status = 0;
    snprintf(resp->message, CONTROL_MESSAGE_LEN, "streaming log %s", log_path);
    write(client_fd, resp, sizeof(*resp));

    fd = open(log_path, O_RDONLY);
    if (fd < 0) {
        return 0;
    }

    while ((n = read(fd, chunk, sizeof(chunk))) > 0)
        write(client_fd, chunk, (size_t)n);

    close(fd);
    resp->status = -2;
    return 0;
}

static int handle_stop(supervisor_ctx_t *ctx, const control_request_t *req,
                       control_response_t *resp)
{
    pthread_mutex_lock(&ctx->metadata_lock);
    container_record_t *rec = find_container_locked(ctx, req->container_id);
    if (!rec) {
        pthread_mutex_unlock(&ctx->metadata_lock);
        resp->status = -1;
        snprintf(resp->message, CONTROL_MESSAGE_LEN,
                 "no container with id %s", req->container_id);
        return 0;
    }

    pid_t pid = rec->host_pid;
    rec->stop_requested = 1;
    rec->state = CONTAINER_STOPPED;
    pthread_mutex_unlock(&ctx->metadata_lock);

    kill(pid, SIGTERM);

    resp->status = 0;
    snprintf(resp->message, CONTROL_MESSAGE_LEN,
             "sent SIGTERM to container %s pid=%d", req->container_id, pid);
    return 0;
}

static int run_supervisor(const char *rootfs)
{
    supervisor_ctx_t ctx;
    struct sockaddr_un addr;
    int rc;

    memset(&ctx, 0, sizeof(ctx));
    ctx.server_fd = -1;
    ctx.monitor_fd = -1;

    g_ctx = &ctx;

    rc = pthread_mutex_init(&ctx.metadata_lock, NULL);
    if (rc != 0) {
        errno = rc;
        perror("pthread_mutex_init");
        return 1;
    }

    rc = bounded_buffer_init(&ctx.log_buffer);
    if (rc != 0) {
        errno = rc;
        perror("bounded_buffer_init");
        pthread_mutex_destroy(&ctx.metadata_lock);
        return 1;
    }

    ctx.monitor_fd = open(MONITOR_DEV, O_RDWR);
    if (ctx.monitor_fd < 0)
        fprintf(stderr, "Warning: could not open %s: %s\n",
                MONITOR_DEV, strerror(errno));

    unlink(CONTROL_PATH);
    ctx.server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (ctx.server_fd < 0) {
        perror("socket");
        goto cleanup;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, CONTROL_PATH, sizeof(addr.sun_path) - 1);

    if (bind(ctx.server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        goto cleanup;
    }

    if (listen(ctx.server_fd, 8) < 0) {
        perror("listen");
        goto cleanup;
    }

    struct sigaction sa_chld, sa_term;
    memset(&sa_chld, 0, sizeof(sa_chld));
    sa_chld.sa_handler = handle_sigchld;
    sa_chld.sa_flags = SA_RESTART | SA_NOCLDSTOP;
    sigaction(SIGCHLD, &sa_chld, NULL);

    memset(&sa_term, 0, sizeof(sa_term));
    sa_term.sa_handler = handle_sigterm;
    sigaction(SIGTERM, &sa_term, NULL);
    sigaction(SIGINT, &sa_term, NULL);

    rc = pthread_create(&ctx.logger_thread, NULL, logging_thread, &ctx);
    if (rc != 0) {
        errno = rc;
        perror("pthread_create logger");
        goto cleanup;
    }

    fprintf(stderr, "supervisor ready (rootfs=%s socket=%s)\n",
            rootfs, CONTROL_PATH);

    while (!ctx.should_stop) {
        int client_fd;
        struct sockaddr_un client_addr;
        socklen_t client_len = sizeof(client_addr);

        fd_set rfds;
        struct timeval tv;
        FD_ZERO(&rfds);
        FD_SET(ctx.server_fd, &rfds);
        tv.tv_sec = 1;
        tv.tv_usec = 0;

        int sel = select(ctx.server_fd + 1, &rfds, NULL, NULL, &tv);
        if (sel < 0) {
            if (errno == EINTR)
                continue;
            perror("select");
            break;
        }
        if (sel == 0)
            continue;

        client_fd = accept(ctx.server_fd,
                           (struct sockaddr *)&client_addr, &client_len);
        if (client_fd < 0) {
            if (errno == EINTR)
                continue;
            perror("accept");
            continue;
        }

        control_request_t req;
        control_response_t resp;
        memset(&req, 0, sizeof(req));
        memset(&resp, 0, sizeof(resp));

        ssize_t nr = read(client_fd, &req, sizeof(req));
        if (nr != (ssize_t)sizeof(req)) {
            close(client_fd);
            continue;
        }

        int send_resp = 1;

        switch (req.kind) {
        case CMD_START:
        case CMD_RUN:
            handle_start(&ctx, &req, &resp);
            break;
        case CMD_PS:
            handle_ps(&ctx, &resp);
            break;
        case CMD_LOGS:
            handle_logs(&ctx, &req, &resp, client_fd);
            if (resp.status == -2) {
                close(client_fd);
                send_resp = 0;
            }
            break;
        case CMD_STOP:
            handle_stop(&ctx, &req, &resp);
            break;
        default:
            resp.status = -1;
            snprintf(resp.message, CONTROL_MESSAGE_LEN, "unknown command");
            break;
        }

        if (send_resp)
            write(client_fd, &resp, sizeof(resp));

        close(client_fd);
    }

    fprintf(stderr, "supervisor shutting down\n");

    pthread_mutex_lock(&ctx.metadata_lock);
    container_record_t *rec = ctx.containers;
    while (rec) {
        if (rec->state == CONTAINER_RUNNING || rec->state == CONTAINER_STARTING)
            kill(rec->host_pid, SIGTERM);
        rec = rec->next;
    }
    pthread_mutex_unlock(&ctx.metadata_lock);

    sleep(1);

    while (waitpid(-1, NULL, WNOHANG) > 0)
        ;

cleanup:
    bounded_buffer_begin_shutdown(&ctx.log_buffer);

    if (ctx.logger_thread)
        pthread_join(ctx.logger_thread, NULL);

    pthread_mutex_lock(&ctx.metadata_lock);
    container_record_t *cur = ctx.containers;
    while (cur) {
        container_record_t *next = cur->next;
        free(cur);
        cur = next;
    }
    ctx.containers = NULL;
    pthread_mutex_unlock(&ctx.metadata_lock);

    bounded_buffer_destroy(&ctx.log_buffer);
    pthread_mutex_destroy(&ctx.metadata_lock);

    if (ctx.server_fd >= 0)
        close(ctx.server_fd);
    if (ctx.monitor_fd >= 0)
        close(ctx.monitor_fd);

    unlink(CONTROL_PATH);
    g_ctx = NULL;
    return 0;
}

static int send_control_request(const control_request_t *req)
{
    int fd;
    struct sockaddr_un addr;
    control_response_t resp;

    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("socket");
        return 1;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, CONTROL_PATH, sizeof(addr.sun_path) - 1);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("connect: is the supervisor running?");
        close(fd);
        return 1;
    }

    if (write(fd, req, sizeof(*req)) != sizeof(*req)) {
        perror("write");
        close(fd);
        return 1;
    }

    if (req->kind == CMD_LOGS) {
        char chunk[4096];
        ssize_t n;
        ssize_t first = read(fd, &resp, sizeof(resp));
        if (first == (ssize_t)sizeof(resp) && resp.status == 0) {
            while ((n = read(fd, chunk, sizeof(chunk))) > 0)
                fwrite(chunk, 1, (size_t)n, stdout);
        } else {
            fprintf(stderr, "logs error: %s\n", resp.message);
        }
        close(fd);
        return 0;
    }

    ssize_t nr = read(fd, &resp, sizeof(resp));
    close(fd);

    if (nr != (ssize_t)sizeof(resp)) {
        fprintf(stderr, "short response from supervisor\n");
        return 1;
    }

    printf("%s\n", resp.message);
    return resp.status == 0 ? 0 : 1;
}

static int cmd_start(int argc, char *argv[])
{
    control_request_t req;

    if (argc < 5) {
        fprintf(stderr,
                "Usage: %s start <id> <container-rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]\n",
                argv[0]);
        return 1;
    }

    memset(&req, 0, sizeof(req));
    req.kind = CMD_START;
    strncpy(req.container_id, argv[2], sizeof(req.container_id) - 1);
    strncpy(req.rootfs, argv[3], sizeof(req.rootfs) - 1);
    strncpy(req.command, argv[4], sizeof(req.command) - 1);
    req.soft_limit_bytes = DEFAULT_SOFT_LIMIT;
    req.hard_limit_bytes = DEFAULT_HARD_LIMIT;

    if (parse_optional_flags(&req, argc, argv, 5) != 0)
        return 1;

    return send_control_request(&req);
}

static int cmd_run(int argc, char *argv[])
{
    control_request_t req;

    if (argc < 5) {
        fprintf(stderr,
                "Usage: %s run <id> <container-rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]\n",
                argv[0]);
        return 1;
    }

    memset(&req, 0, sizeof(req));
    req.kind = CMD_RUN;
    strncpy(req.container_id, argv[2], sizeof(req.container_id) - 1);
    strncpy(req.rootfs, argv[3], sizeof(req.rootfs) - 1);
    strncpy(req.command, argv[4], sizeof(req.command) - 1);
    req.soft_limit_bytes = DEFAULT_SOFT_LIMIT;
    req.hard_limit_bytes = DEFAULT_HARD_LIMIT;

    if (parse_optional_flags(&req, argc, argv, 5) != 0)
        return 1;

    return send_control_request(&req);
}

static int cmd_ps(void)
{
    control_request_t req;

    memset(&req, 0, sizeof(req));
    req.kind = CMD_PS;
    return send_control_request(&req);
}

static int cmd_logs(int argc, char *argv[])
{
    control_request_t req;

    if (argc < 3) {
        fprintf(stderr, "Usage: %s logs <id>\n", argv[0]);
        return 1;
    }

    memset(&req, 0, sizeof(req));
    req.kind = CMD_LOGS;
    strncpy(req.container_id, argv[2], sizeof(req.container_id) - 1);
    return send_control_request(&req);
}

static int cmd_stop(int argc, char *argv[])
{
    control_request_t req;

    if (argc < 3) {
        fprintf(stderr, "Usage: %s stop <id>\n", argv[0]);
        return 1;
    }

    memset(&req, 0, sizeof(req));
    req.kind = CMD_STOP;
    strncpy(req.container_id, argv[2], sizeof(req.container_id) - 1);
    return send_control_request(&req);
}

int main(int argc, char *argv[])
{
    if (argc < 2) {
        usage(argv[0]);
        return 1;
    }

    if (strcmp(argv[1], "supervisor") == 0) {
        if (argc < 3) {
            fprintf(stderr, "Usage: %s supervisor <base-rootfs>\n", argv[0]);
            return 1;
        }
        return run_supervisor(argv[2]);
    }

    if (strcmp(argv[1], "start") == 0)
        return cmd_start(argc, argv);

    if (strcmp(argv[1], "run") == 0)
        return cmd_run(argc, argv);

    if (strcmp(argv[1], "ps") == 0)
        return cmd_ps();

    if (strcmp(argv[1], "logs") == 0)
        return cmd_logs(argc, argv);

    if (strcmp(argv[1], "stop") == 0)
        return cmd_stop(argc, argv);

    usage(argv[0]);
    return 1;
}