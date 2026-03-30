#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <infiniband/verbs.h>
#include <inttypes.h>
#include <netdb.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define RDMA_CHECK_MAGIC 0x52444d41u
#define RDMA_CHECK_READY 0x79
#define RDMA_CHECK_MAX_STR 128

typedef enum {
    MODE_SERVER = 0,
    MODE_CLIENT = 1,
} run_mode_t;

typedef enum {
    WORKLOAD_CHECK = 0,
    WORKLOAD_BENCH = 1,
} workload_t;

typedef struct {
    run_mode_t mode;
    workload_t workload;
    char control_host[RDMA_CHECK_MAX_STR];
    uint16_t control_port;
    char ib_device[RDMA_CHECK_MAX_STR];
    uint8_t ib_port;
    int gid_index;
    uint8_t service_level;
    size_t message_size;
    uint64_t iterations;
    uint32_t queue_depth;
    int validate;
} config_t;

typedef struct {
    uint32_t magic;
    uint16_t lid;
    uint8_t mtu;
    uint8_t gid_index;
    uint32_t qpn;
    uint32_t psn;
    uint8_t gid[16];
} peer_info_t;

typedef struct {
    uint32_t magic;
    uint32_t status;
    uint64_t received;
    uint64_t bad_seq;
    uint64_t bad_payload;
    uint64_t elapsed_ns;
} summary_t;

typedef struct {
    uint64_t seq;
    uint32_t bytes;
    uint32_t magic;
} __attribute__((packed)) msg_header_t;

typedef struct {
    struct ibv_context *context;
    struct ibv_pd *pd;
    struct ibv_cq *cq;
    struct ibv_qp *qp;
    struct ibv_mr *send_mr;
    struct ibv_mr *recv_mr;
    uint8_t *send_buf;
    uint8_t *recv_buf;
    size_t slot_size;
    uint32_t queue_depth;
    peer_info_t local;
} rdma_ctx_t;

static void usage(const char *argv0) {
    fprintf(stderr, "usage: %s <config>\n", argv0);
}

static char *trim(char *text) {
    char *end;

    while (*text != '\0' && isspace((unsigned char)*text)) {
        text++;
    }
    if (*text == '\0') {
        return text;
    }
    end = text + strlen(text) - 1;
    while (end > text && isspace((unsigned char)*end)) {
        *end = '\0';
        end--;
    }
    return text;
}

static int parse_u64(const char *text, uint64_t *value) {
    char *end = NULL;
    unsigned long long parsed;

    errno = 0;
    parsed = strtoull(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0') {
        return -1;
    }
    *value = (uint64_t)parsed;
    return 0;
}

static int parse_u32(const char *text, uint32_t *value) {
    uint64_t parsed;

    if (parse_u64(text, &parsed) != 0 || parsed > UINT32_MAX) {
        return -1;
    }
    *value = (uint32_t)parsed;
    return 0;
}

static int parse_bool(const char *text, int *value) {
    if (strcmp(text, "1") == 0 || strcmp(text, "true") == 0 || strcmp(text, "yes") == 0) {
        *value = 1;
        return 0;
    }
    if (strcmp(text, "0") == 0 || strcmp(text, "false") == 0 || strcmp(text, "no") == 0) {
        *value = 0;
        return 0;
    }
    return -1;
}

static int parse_config(const char *path, config_t *cfg) {
    FILE *fp;
    char line[512];
    unsigned int line_no = 0;

    memset(cfg, 0, sizeof(*cfg));
    cfg->mode = MODE_SERVER;
    cfg->workload = WORKLOAD_CHECK;
    snprintf(cfg->control_host, sizeof(cfg->control_host), "0.0.0.0");
    cfg->control_port = 18515;
    cfg->ib_port = 1;
    cfg->gid_index = -1;
    cfg->service_level = 0;
    cfg->message_size = 32;
    cfg->iterations = 1;
    cfg->queue_depth = 1;
    cfg->validate = 1;

    fp = fopen(path, "r");
    if (fp == NULL) {
        perror("fopen config");
        return -1;
    }

    while (fgets(line, sizeof(line), fp) != NULL) {
        char *key;
        char *value;
        char *eq;
        uint32_t u32_value;
        uint64_t u64_value;

        line_no++;
        key = trim(line);
        if (*key == '\0' || *key == '#') {
            continue;
        }
        eq = strchr(key, '=');
        if (eq == NULL) {
            fprintf(stderr, "config:%u invalid line\n", line_no);
            fclose(fp);
            return -1;
        }
        *eq = '\0';
        value = trim(eq + 1);
        key = trim(key);

        if (strcmp(key, "mode") == 0) {
            if (strcmp(value, "server") == 0) {
                cfg->mode = MODE_SERVER;
            } else if (strcmp(value, "client") == 0) {
                cfg->mode = MODE_CLIENT;
            } else {
                fprintf(stderr, "config:%u invalid mode\n", line_no);
                fclose(fp);
                return -1;
            }
        } else if (strcmp(key, "workload") == 0) {
            if (strcmp(value, "check") == 0) {
                cfg->workload = WORKLOAD_CHECK;
            } else if (strcmp(value, "bench") == 0) {
                cfg->workload = WORKLOAD_BENCH;
            } else {
                fprintf(stderr, "config:%u invalid workload\n", line_no);
                fclose(fp);
                return -1;
            }
        } else if (strcmp(key, "control_host") == 0) {
            snprintf(cfg->control_host, sizeof(cfg->control_host), "%s", value);
        } else if (strcmp(key, "control_port") == 0) {
            if (parse_u32(value, &u32_value) != 0 || u32_value > UINT16_MAX) {
                fprintf(stderr, "config:%u invalid control_port\n", line_no);
                fclose(fp);
                return -1;
            }
            cfg->control_port = (uint16_t)u32_value;
        } else if (strcmp(key, "ib_device") == 0) {
            snprintf(cfg->ib_device, sizeof(cfg->ib_device), "%s", value);
        } else if (strcmp(key, "ib_port") == 0) {
            if (parse_u32(value, &u32_value) != 0 || u32_value > UINT8_MAX) {
                fprintf(stderr, "config:%u invalid ib_port\n", line_no);
                fclose(fp);
                return -1;
            }
            cfg->ib_port = (uint8_t)u32_value;
        } else if (strcmp(key, "gid_index") == 0) {
            if (strcmp(value, "-1") == 0) {
                cfg->gid_index = -1;
            } else if (parse_u32(value, &u32_value) == 0) {
                cfg->gid_index = (int)u32_value;
            } else {
                fprintf(stderr, "config:%u invalid gid_index\n", line_no);
                fclose(fp);
                return -1;
            }
        } else if (strcmp(key, "service_level") == 0) {
            if (parse_u32(value, &u32_value) != 0 || u32_value > 15) {
                fprintf(stderr, "config:%u invalid service_level\n", line_no);
                fclose(fp);
                return -1;
            }
            cfg->service_level = (uint8_t)u32_value;
        } else if (strcmp(key, "message_size") == 0) {
            if (parse_u64(value, &u64_value) != 0 || u64_value > UINT32_MAX || u64_value > SIZE_MAX) {
                fprintf(stderr, "config:%u invalid message_size\n", line_no);
                fclose(fp);
                return -1;
            }
            cfg->message_size = (size_t)u64_value;
        } else if (strcmp(key, "iterations") == 0) {
            if (parse_u64(value, &cfg->iterations) != 0 || cfg->iterations == 0) {
                fprintf(stderr, "config:%u invalid iterations\n", line_no);
                fclose(fp);
                return -1;
            }
        } else if (strcmp(key, "queue_depth") == 0) {
            if (parse_u32(value, &cfg->queue_depth) != 0 || cfg->queue_depth == 0) {
                fprintf(stderr, "config:%u invalid queue_depth\n", line_no);
                fclose(fp);
                return -1;
            }
        } else if (strcmp(key, "validate") == 0) {
            if (parse_bool(value, &cfg->validate) != 0) {
                fprintf(stderr, "config:%u invalid validate\n", line_no);
                fclose(fp);
                return -1;
            }
        } else {
            fprintf(stderr, "config:%u unknown key '%s'\n", line_no, key);
            fclose(fp);
            return -1;
        }
    }

    fclose(fp);

    if (cfg->ib_device[0] == '\0') {
        fprintf(stderr, "config missing ib_device\n");
        return -1;
    }
    if (cfg->message_size < sizeof(msg_header_t)) {
        fprintf(stderr, "message_size must be at least %zu\n", sizeof(msg_header_t));
        return -1;
    }
    if (cfg->workload == WORKLOAD_CHECK) {
        cfg->iterations = 1;
        cfg->queue_depth = 1;
        cfg->validate = 1;
    }
    return 0;
}

static uint64_t now_ns(void) {
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static void *xaligned_alloc(size_t alignment, size_t size) {
    void *ptr = NULL;
    int rc;

    rc = posix_memalign(&ptr, alignment, size);
    if (rc != 0) {
        fprintf(stderr, "posix_memalign: %s\n", strerror(rc));
        exit(1);
    }
    memset(ptr, 0, size);
    return ptr;
}

static int write_full(int fd, const void *buf, size_t len) {
    const uint8_t *ptr = buf;
    size_t off = 0;

    while (off < len) {
        ssize_t written = write(fd, ptr + off, len - off);
        if (written < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        off += (size_t)written;
    }
    return 0;
}

static int read_full(int fd, void *buf, size_t len) {
    uint8_t *ptr = buf;
    size_t off = 0;

    while (off < len) {
        ssize_t got = read(fd, ptr + off, len - off);
        if (got == 0) {
            errno = ECONNRESET;
            return -1;
        }
        if (got < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        off += (size_t)got;
    }
    return 0;
}

static int open_server_socket(const config_t *cfg) {
    struct addrinfo hints;
    struct addrinfo *result = NULL;
    struct addrinfo *it;
    char port[16];
    int listen_fd = -1;
    int conn_fd = -1;
    int rc;
    int one = 1;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;
    snprintf(port, sizeof(port), "%u", cfg->control_port);

    rc = getaddrinfo(cfg->control_host, port, &hints, &result);
    if (rc != 0) {
        fprintf(stderr, "getaddrinfo server: %s\n", gai_strerror(rc));
        return -1;
    }

    for (it = result; it != NULL; it = it->ai_next) {
        listen_fd = socket(it->ai_family, it->ai_socktype, it->ai_protocol);
        if (listen_fd < 0) {
            continue;
        }
        setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        if (bind(listen_fd, it->ai_addr, it->ai_addrlen) == 0 && listen(listen_fd, 1) == 0) {
            break;
        }
        close(listen_fd);
        listen_fd = -1;
    }

    freeaddrinfo(result);
    if (listen_fd < 0) {
        perror("listen socket");
        return -1;
    }

    conn_fd = accept(listen_fd, NULL, NULL);
    if (conn_fd < 0) {
        perror("accept");
    }
    close(listen_fd);
    return conn_fd;
}

static int open_client_socket(const config_t *cfg) {
    struct addrinfo hints;
    struct addrinfo *result = NULL;
    struct addrinfo *it;
    char port[16];
    int fd = -1;
    int rc;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    snprintf(port, sizeof(port), "%u", cfg->control_port);

    rc = getaddrinfo(cfg->control_host, port, &hints, &result);
    if (rc != 0) {
        fprintf(stderr, "getaddrinfo client: %s\n", gai_strerror(rc));
        return -1;
    }

    for (it = result; it != NULL; it = it->ai_next) {
        fd = socket(it->ai_family, it->ai_socktype, it->ai_protocol);
        if (fd < 0) {
            continue;
        }
        if (connect(fd, it->ai_addr, it->ai_addrlen) == 0) {
            break;
        }
        close(fd);
        fd = -1;
    }

    freeaddrinfo(result);
    if (fd < 0) {
        perror("connect");
    }
    return fd;
}

static int open_control_socket(const config_t *cfg) {
    if (cfg->mode == MODE_SERVER) {
        return open_server_socket(cfg);
    }
    return open_client_socket(cfg);
}

static struct ibv_context *open_device_by_name(const char *name) {
    struct ibv_device **list;
    struct ibv_context *context = NULL;
    int count = 0;
    int i;

    list = ibv_get_device_list(&count);
    if (list == NULL) {
        perror("ibv_get_device_list");
        return NULL;
    }

    for (i = 0; i < count; i++) {
        if (strcmp(ibv_get_device_name(list[i]), name) == 0) {
            context = ibv_open_device(list[i]);
            if (context == NULL) {
                perror("ibv_open_device");
            }
            break;
        }
    }

    if (context == NULL) {
        fprintf(stderr, "IB device '%s' not found\n", name);
    }

    ibv_free_device_list(list);
    return context;
}

static int qp_to_init(struct ibv_qp *qp, uint8_t port) {
    struct ibv_qp_attr attr;

    memset(&attr, 0, sizeof(attr));
    attr.qp_state = IBV_QPS_INIT;
    attr.port_num = port;
    attr.pkey_index = 0;
    attr.qp_access_flags = 0;
    return ibv_modify_qp(qp, &attr,
        IBV_QP_STATE |
        IBV_QP_PKEY_INDEX |
        IBV_QP_PORT |
        IBV_QP_ACCESS_FLAGS);
}

static int qp_to_rtr(struct ibv_qp *qp, const config_t *cfg, const peer_info_t *peer) {
    struct ibv_qp_attr attr;

    memset(&attr, 0, sizeof(attr));
    attr.qp_state = IBV_QPS_RTR;
    attr.path_mtu = (enum ibv_mtu)peer->mtu;
    attr.dest_qp_num = peer->qpn;
    attr.rq_psn = peer->psn;
    attr.max_dest_rd_atomic = 1;
    attr.min_rnr_timer = 12;
    attr.ah_attr.is_global = cfg->gid_index >= 0 ? 1 : 0;
    attr.ah_attr.dlid = peer->lid;
    attr.ah_attr.sl = cfg->service_level;
    attr.ah_attr.src_path_bits = 0;
    attr.ah_attr.port_num = cfg->ib_port;

    if (cfg->gid_index >= 0) {
        memcpy(&attr.ah_attr.grh.dgid, peer->gid, 16);
        attr.ah_attr.grh.hop_limit = 1;
        attr.ah_attr.grh.sgid_index = cfg->gid_index;
    }

    return ibv_modify_qp(qp, &attr,
        IBV_QP_STATE |
        IBV_QP_AV |
        IBV_QP_PATH_MTU |
        IBV_QP_DEST_QPN |
        IBV_QP_RQ_PSN |
        IBV_QP_MAX_DEST_RD_ATOMIC |
        IBV_QP_MIN_RNR_TIMER);
}

static int qp_to_rts(struct ibv_qp *qp, uint32_t psn) {
    struct ibv_qp_attr attr;

    memset(&attr, 0, sizeof(attr));
    attr.qp_state = IBV_QPS_RTS;
    attr.timeout = 14;
    attr.retry_cnt = 7;
    attr.rnr_retry = 7;
    attr.sq_psn = psn;
    attr.max_rd_atomic = 1;
    return ibv_modify_qp(qp, &attr,
        IBV_QP_STATE |
        IBV_QP_TIMEOUT |
        IBV_QP_RETRY_CNT |
        IBV_QP_RNR_RETRY |
        IBV_QP_SQ_PSN |
        IBV_QP_MAX_QP_RD_ATOMIC);
}

static int setup_rdma(const config_t *cfg, rdma_ctx_t *ctx) {
    struct ibv_port_attr port_attr;
    struct ibv_qp_init_attr qp_init;
    long page_size = sysconf(_SC_PAGESIZE);
    size_t total_bytes;

    memset(ctx, 0, sizeof(*ctx));
    ctx->queue_depth = cfg->queue_depth;
    ctx->slot_size = cfg->message_size;

    ctx->context = open_device_by_name(cfg->ib_device);
    if (ctx->context == NULL) {
        return -1;
    }
    if (ibv_query_port(ctx->context, cfg->ib_port, &port_attr) != 0) {
        perror("ibv_query_port");
        return -1;
    }

    ctx->pd = ibv_alloc_pd(ctx->context);
    if (ctx->pd == NULL) {
        perror("ibv_alloc_pd");
        return -1;
    }

    ctx->cq = ibv_create_cq(ctx->context, (int)(cfg->queue_depth * 2 + 8), NULL, NULL, 0);
    if (ctx->cq == NULL) {
        perror("ibv_create_cq");
        return -1;
    }

    memset(&qp_init, 0, sizeof(qp_init));
    qp_init.send_cq = ctx->cq;
    qp_init.recv_cq = ctx->cq;
    qp_init.qp_type = IBV_QPT_RC;
    qp_init.cap.max_send_wr = cfg->queue_depth + 1;
    qp_init.cap.max_recv_wr = cfg->queue_depth + 1;
    qp_init.cap.max_send_sge = 1;
    qp_init.cap.max_recv_sge = 1;

    ctx->qp = ibv_create_qp(ctx->pd, &qp_init);
    if (ctx->qp == NULL) {
        perror("ibv_create_qp");
        return -1;
    }
    if (qp_to_init(ctx->qp, cfg->ib_port) != 0) {
        perror("ibv_modify_qp INIT");
        return -1;
    }

    total_bytes = ctx->slot_size * ctx->queue_depth;
    ctx->send_buf = xaligned_alloc((size_t)(page_size > 0 ? page_size : 4096), total_bytes);
    ctx->recv_buf = xaligned_alloc((size_t)(page_size > 0 ? page_size : 4096), total_bytes);

    ctx->send_mr = ibv_reg_mr(ctx->pd, ctx->send_buf, total_bytes, IBV_ACCESS_LOCAL_WRITE);
    if (ctx->send_mr == NULL) {
        perror("ibv_reg_mr send");
        return -1;
    }
    ctx->recv_mr = ibv_reg_mr(ctx->pd, ctx->recv_buf, total_bytes, IBV_ACCESS_LOCAL_WRITE);
    if (ctx->recv_mr == NULL) {
        perror("ibv_reg_mr recv");
        return -1;
    }

    memset(&ctx->local, 0, sizeof(ctx->local));
    ctx->local.magic = RDMA_CHECK_MAGIC;
    ctx->local.lid = port_attr.lid;
    ctx->local.mtu = (uint8_t)port_attr.active_mtu;
    ctx->local.gid_index = cfg->gid_index < 0 ? UINT8_MAX : (uint8_t)cfg->gid_index;
    ctx->local.qpn = ctx->qp->qp_num;
    ctx->local.psn = (uint32_t)(lrand48() & 0x00ffffffu);

    if (cfg->gid_index >= 0) {
        union ibv_gid gid;

        if (ibv_query_gid(ctx->context, cfg->ib_port, cfg->gid_index, &gid) != 0) {
            perror("ibv_query_gid");
            return -1;
        }
        memcpy(ctx->local.gid, gid.raw, sizeof(ctx->local.gid));
    }

    return 0;
}

static void cleanup_rdma(rdma_ctx_t *ctx) {
    if (ctx->qp != NULL) {
        ibv_destroy_qp(ctx->qp);
    }
    if (ctx->send_mr != NULL) {
        ibv_dereg_mr(ctx->send_mr);
    }
    if (ctx->recv_mr != NULL) {
        ibv_dereg_mr(ctx->recv_mr);
    }
    if (ctx->cq != NULL) {
        ibv_destroy_cq(ctx->cq);
    }
    if (ctx->pd != NULL) {
        ibv_dealloc_pd(ctx->pd);
    }
    if (ctx->context != NULL) {
        ibv_close_device(ctx->context);
    }
    free(ctx->send_buf);
    free(ctx->recv_buf);
}

static int exchange_peer_info(int fd, const peer_info_t *local, peer_info_t *remote) {
    if (write_full(fd, local, sizeof(*local)) != 0) {
        perror("write peer_info");
        return -1;
    }
    if (read_full(fd, remote, sizeof(*remote)) != 0) {
        perror("read peer_info");
        return -1;
    }
    if (remote->magic != RDMA_CHECK_MAGIC) {
        fprintf(stderr, "remote peer magic mismatch\n");
        return -1;
    }
    return 0;
}

static int post_recv_slot(const rdma_ctx_t *ctx, uint32_t slot) {
    struct ibv_sge sge;
    struct ibv_recv_wr wr;
    struct ibv_recv_wr *bad = NULL;

    memset(&sge, 0, sizeof(sge));
    sge.addr = (uintptr_t)(ctx->recv_buf + ((size_t)slot * ctx->slot_size));
    sge.length = (uint32_t)ctx->slot_size;
    sge.lkey = ctx->recv_mr->lkey;

    memset(&wr, 0, sizeof(wr));
    wr.wr_id = slot;
    wr.sg_list = &sge;
    wr.num_sge = 1;

    return ibv_post_recv(ctx->qp, &wr, &bad);
}

static void fill_send_slot(const rdma_ctx_t *ctx, uint32_t slot, uint64_t seq) {
    uint8_t *base = ctx->send_buf + ((size_t)slot * ctx->slot_size);
    msg_header_t *header = (msg_header_t *)base;
    size_t i;

    header->seq = seq;
    header->bytes = (uint32_t)ctx->slot_size;
    header->magic = RDMA_CHECK_MAGIC;

    for (i = sizeof(*header); i < ctx->slot_size; i++) {
        base[i] = (uint8_t)((seq + i) & 0xffu);
    }
}

static int post_send_slot(const rdma_ctx_t *ctx, uint32_t slot) {
    struct ibv_sge sge;
    struct ibv_send_wr wr;
    struct ibv_send_wr *bad = NULL;

    memset(&sge, 0, sizeof(sge));
    sge.addr = (uintptr_t)(ctx->send_buf + ((size_t)slot * ctx->slot_size));
    sge.length = (uint32_t)ctx->slot_size;
    sge.lkey = ctx->send_mr->lkey;

    memset(&wr, 0, sizeof(wr));
    wr.wr_id = slot;
    wr.sg_list = &sge;
    wr.num_sge = 1;
    wr.opcode = IBV_WR_SEND;
    wr.send_flags = IBV_SEND_SIGNALED;

    return ibv_post_send(ctx->qp, &wr, &bad);
}

static int poll_one_completion(struct ibv_cq *cq, struct ibv_wc *wc) {
    int got;

    do {
        got = ibv_poll_cq(cq, 1, wc);
    } while (got == 0);

    if (got < 0) {
        perror("ibv_poll_cq");
        return -1;
    }
    if (wc->status != IBV_WC_SUCCESS) {
        fprintf(stderr, "CQ error status=%s opcode=%d\n",
            ibv_wc_status_str(wc->status), wc->opcode);
        return -1;
    }
    return 0;
}

static void validate_recv(const config_t *cfg, const rdma_ctx_t *ctx, uint32_t slot,
    uint64_t expected_seq, uint64_t *bad_seq, uint64_t *bad_payload) {
    uint8_t *base = ctx->recv_buf + ((size_t)slot * ctx->slot_size);
    msg_header_t *header = (msg_header_t *)base;
    size_t i;

    if (header->magic != RDMA_CHECK_MAGIC || header->bytes != ctx->slot_size || header->seq != expected_seq) {
        (*bad_seq)++;
        return;
    }
    if (!cfg->validate) {
        return;
    }
    for (i = sizeof(*header); i < ctx->slot_size; i++) {
        uint8_t expected = (uint8_t)((expected_seq + i) & 0xffu);
        if (base[i] != expected) {
            (*bad_payload)++;
            return;
        }
    }
}

static int server_loop(const config_t *cfg, rdma_ctx_t *ctx, int fd) {
    uint64_t posted = 0;
    uint64_t received = 0;
    uint64_t bad_seq = 0;
    uint64_t bad_payload = 0;
    uint64_t start_ns;
    uint64_t end_ns;
    summary_t summary;
    uint8_t ready = RDMA_CHECK_READY;
    uint32_t initial;

    initial = cfg->queue_depth;
    if ((uint64_t)initial > cfg->iterations) {
        initial = (uint32_t)cfg->iterations;
    }
    for (posted = 0; posted < initial; posted++) {
        if (post_recv_slot(ctx, (uint32_t)posted) != 0) {
            perror("ibv_post_recv");
            return -1;
        }
    }

    if (write_full(fd, &ready, sizeof(ready)) != 0) {
        perror("write ready");
        return -1;
    }

    start_ns = now_ns();
    while (received < cfg->iterations) {
        struct ibv_wc wc;
        uint32_t slot;

        if (poll_one_completion(ctx->cq, &wc) != 0) {
            return -1;
        }
        if (wc.opcode != IBV_WC_RECV) {
            fprintf(stderr, "unexpected server opcode %d\n", wc.opcode);
            return -1;
        }

        slot = (uint32_t)wc.wr_id;
        validate_recv(cfg, ctx, slot, received, &bad_seq, &bad_payload);
        received++;

        if (posted < cfg->iterations) {
            if (post_recv_slot(ctx, slot) != 0) {
                perror("ibv_post_recv refill");
                return -1;
            }
            posted++;
        }
    }
    end_ns = now_ns();

    memset(&summary, 0, sizeof(summary));
    summary.magic = RDMA_CHECK_MAGIC;
    summary.status = (bad_seq == 0 && bad_payload == 0) ? 0u : 1u;
    summary.received = received;
    summary.bad_seq = bad_seq;
    summary.bad_payload = bad_payload;
    summary.elapsed_ns = end_ns - start_ns;

    if (write_full(fd, &summary, sizeof(summary)) != 0) {
        perror("write summary");
        return -1;
    }

    printf("server workload=%s messages=%" PRIu64 " bytes=%zu elapsed_ms=%.3f bad_seq=%" PRIu64 " bad_payload=%" PRIu64 "\n",
        cfg->workload == WORKLOAD_CHECK ? "check" : "bench",
        received,
        cfg->message_size,
        (double)summary.elapsed_ns / 1000000.0,
        bad_seq,
        bad_payload);
    return summary.status == 0 ? 0 : -1;
}

static int client_loop(const config_t *cfg, rdma_ctx_t *ctx, int fd) {
    uint8_t ready;
    uint64_t posted = 0;
    uint64_t completed = 0;
    uint32_t inflight = 0;
    uint64_t start_ns;
    uint64_t end_ns;
    summary_t summary;
    double elapsed_sec;
    double mib_per_sec;
    double msg_per_sec;

    if (read_full(fd, &ready, sizeof(ready)) != 0) {
        perror("read ready");
        return -1;
    }
    if (ready != RDMA_CHECK_READY) {
        fprintf(stderr, "remote ready byte mismatch\n");
        return -1;
    }

    start_ns = now_ns();
    while (completed < cfg->iterations) {
        while (posted < cfg->iterations && inflight < cfg->queue_depth) {
            uint32_t slot = (uint32_t)(posted % cfg->queue_depth);
            fill_send_slot(ctx, slot, posted);
            if (post_send_slot(ctx, slot) != 0) {
                perror("ibv_post_send");
                return -1;
            }
            posted++;
            inflight++;
        }

        if (inflight > 0) {
            struct ibv_wc wc;

            if (poll_one_completion(ctx->cq, &wc) != 0) {
                return -1;
            }
            if (wc.opcode != IBV_WC_SEND) {
                fprintf(stderr, "unexpected client opcode %d\n", wc.opcode);
                return -1;
            }
            completed++;
            inflight--;
        }
    }
    end_ns = now_ns();

    if (read_full(fd, &summary, sizeof(summary)) != 0) {
        perror("read summary");
        return -1;
    }
    if (summary.magic != RDMA_CHECK_MAGIC) {
        fprintf(stderr, "summary magic mismatch\n");
        return -1;
    }

    elapsed_sec = (double)(end_ns - start_ns) / 1000000000.0;
    mib_per_sec = elapsed_sec > 0.0
        ? ((double)cfg->message_size * (double)cfg->iterations) / (1024.0 * 1024.0 * elapsed_sec)
        : 0.0;
    msg_per_sec = elapsed_sec > 0.0 ? (double)cfg->iterations / elapsed_sec : 0.0;

    printf("client workload=%s messages=%" PRIu64 " bytes=%zu elapsed_ms=%.3f msg_per_sec=%.2f MiB_per_sec=%.2f remote_status=%u remote_bad_seq=%" PRIu64 " remote_bad_payload=%" PRIu64 "\n",
        cfg->workload == WORKLOAD_CHECK ? "check" : "bench",
        cfg->iterations,
        cfg->message_size,
        (double)(end_ns - start_ns) / 1000000.0,
        msg_per_sec,
        mib_per_sec,
        summary.status,
        summary.bad_seq,
        summary.bad_payload);

    return summary.status == 0 ? 0 : -1;
}

int main(int argc, char **argv) {
    config_t cfg;
    rdma_ctx_t ctx;
    peer_info_t remote;
    int control_fd = -1;
    int rc = 1;

    if (argc != 2) {
        usage(argv[0]);
        return 1;
    }

    if (parse_config(argv[1], &cfg) != 0) {
        return 1;
    }

    srand48((long)(time(NULL) ^ getpid()));

    if (setup_rdma(&cfg, &ctx) != 0) {
        cleanup_rdma(&ctx);
        return 1;
    }

    control_fd = open_control_socket(&cfg);
    if (control_fd < 0) {
        cleanup_rdma(&ctx);
        return 1;
    }

    if (exchange_peer_info(control_fd, &ctx.local, &remote) != 0) {
        goto out;
    }
    if ((cfg.gid_index >= 0) != (remote.gid_index != UINT8_MAX)) {
        fprintf(stderr, "gid_index mode mismatch between local and remote\n");
        goto out;
    }
    if (ctx.local.mtu < remote.mtu) {
        remote.mtu = ctx.local.mtu;
    }
    if (qp_to_rtr(ctx.qp, &cfg, &remote) != 0) {
        perror("ibv_modify_qp RTR");
        goto out;
    }
    if (qp_to_rts(ctx.qp, ctx.local.psn) != 0) {
        perror("ibv_modify_qp RTS");
        goto out;
    }

    if (cfg.mode == MODE_SERVER) {
        rc = server_loop(&cfg, &ctx, control_fd) == 0 ? 0 : 1;
    } else {
        rc = client_loop(&cfg, &ctx, control_fd) == 0 ? 0 : 1;
    }

out:
    if (control_fd >= 0) {
        close(control_fd);
    }
    cleanup_rdma(&ctx);
    return rc;
}
