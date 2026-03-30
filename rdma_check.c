#define _POSIX_C_SOURCE 200809L

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <infiniband/verbs.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define MAGIC 0x52444d41u
#define READY 0x79
#define STR_BYTES 64

typedef enum { MODE_SERVER, MODE_CLIENT } run_mode_t;
typedef enum { WORKLOAD_CHECK, WORKLOAD_BENCH } workload_t;

typedef struct {
    run_mode_t mode;
    workload_t workload;
    char control_host[STR_BYTES];
    uint16_t control_port;
    char ib_device[STR_BYTES];
    uint8_t ib_port;
    size_t message_size;
    uint64_t iterations;
    uint32_t queue_depth;
} config_t;

typedef struct {
    uint32_t magic;
    uint16_t lid;
    uint8_t mtu;
    uint8_t reserved;
    uint32_t qpn;
    uint32_t psn;
} peer_info_t;

typedef struct {
    uint64_t seq;
    uint32_t bytes;
    uint32_t magic;
} msg_header_t;

typedef struct {
    uint32_t magic;
    uint32_t status;
    uint64_t messages;
    uint64_t bad_seq;
    uint64_t bad_payload;
    uint64_t elapsed_ns;
} summary_t;

typedef struct {
    struct ibv_context *verbs;
    struct ibv_pd *pd;
    struct ibv_cq *cq;
    struct ibv_qp *qp;
    struct ibv_mr *send_mr;
    struct ibv_mr *recv_mr;
    uint8_t *send_buf;
    uint8_t *recv_buf;
    size_t slot_size;
    uint32_t depth;
    peer_info_t local;
} rdma_t;

static const char *mode_name(run_mode_t mode) {
    return mode == MODE_SERVER ? "server" : "client";
}

static char *trim(char *s) {
    char *end;

    while (*s != '\0' && isspace((unsigned char)*s)) {
        s++;
    }
    if (*s == '\0') {
        return s;
    }
    end = s + strlen(s) - 1;
    while (end > s && isspace((unsigned char)*end)) {
        *end-- = '\0';
    }
    return s;
}

static int parse_u32(const char *text, uint32_t *out) {
    char *end = NULL;
    unsigned long value;

    errno = 0;
    value = strtoul(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' || value > UINT32_MAX) {
        return -1;
    }
    *out = (uint32_t)value;
    return 0;
}

static int parse_u64(const char *text, uint64_t *out) {
    char *end = NULL;
    unsigned long long value;

    errno = 0;
    value = strtoull(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0') {
        return -1;
    }
    *out = (uint64_t)value;
    return 0;
}

static int parse_config(const char *path, config_t *cfg) {
    FILE *fp;
    char line[256];
    unsigned int line_no = 0;

    memset(cfg, 0, sizeof(*cfg));
    cfg->mode = MODE_SERVER;
    cfg->workload = WORKLOAD_CHECK;
    snprintf(cfg->control_host, sizeof(cfg->control_host), "0.0.0.0");
    cfg->control_port = 7301;
    cfg->ib_port = 1;
    cfg->message_size = 32;
    cfg->iterations = 1;
    cfg->queue_depth = 1;

    fp = fopen(path, "r");
    if (fp == NULL) {
        perror("fopen config");
        return -1;
    }

    while (fgets(line, sizeof(line), fp) != NULL) {
        char *key;
        char *value;
        char *eq;
        uint32_t u32;
        uint64_t u64;

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
            if (parse_u32(value, &u32) != 0 || u32 > UINT16_MAX) {
                fprintf(stderr, "config:%u invalid control_port\n", line_no);
                fclose(fp);
                return -1;
            }
            cfg->control_port = (uint16_t)u32;
        } else if (strcmp(key, "ib_device") == 0) {
            snprintf(cfg->ib_device, sizeof(cfg->ib_device), "%s", value);
        } else if (strcmp(key, "ib_port") == 0) {
            if (parse_u32(value, &u32) != 0 || u32 > UINT8_MAX) {
                fprintf(stderr, "config:%u invalid ib_port\n", line_no);
                fclose(fp);
                return -1;
            }
            cfg->ib_port = (uint8_t)u32;
        } else if (strcmp(key, "message_size") == 0) {
            if (parse_u64(value, &u64) != 0 || u64 < sizeof(msg_header_t) || u64 > UINT32_MAX) {
                fprintf(stderr, "config:%u invalid message_size\n", line_no);
                fclose(fp);
                return -1;
            }
            cfg->message_size = (size_t)u64;
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
    if (cfg->workload == WORKLOAD_CHECK) {
        cfg->iterations = 1;
        cfg->queue_depth = 1;
    }
    return 0;
}

static void *xmem(size_t bytes) {
    void *ptr = NULL;
    size_t page = (size_t)(sysconf(_SC_PAGESIZE) > 0 ? sysconf(_SC_PAGESIZE) : 4096);

    if (posix_memalign(&ptr, page, bytes) != 0) {
        perror("posix_memalign");
        exit(1);
    }
    memset(ptr, 0, bytes);
    return ptr;
}

static uint64_t now_ns(void) {
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static int write_full(int fd, const void *buf, size_t len) {
    const uint8_t *p = buf;
    size_t done = 0;

    while (done < len) {
        ssize_t rc = write(fd, p + done, len - done);
        if (rc < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        done += (size_t)rc;
    }
    return 0;
}

static int read_full(int fd, void *buf, size_t len) {
    uint8_t *p = buf;
    size_t done = 0;

    while (done < len) {
        ssize_t rc = read(fd, p + done, len - done);
        if (rc == 0) {
            errno = ECONNRESET;
            return -1;
        }
        if (rc < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        done += (size_t)rc;
    }
    return 0;
}

static int tcp_server(const config_t *cfg) {
    struct sockaddr_in addr;
    int listen_fd;
    int conn_fd;
    int one = 1;

    listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        perror("socket");
        return -1;
    }
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(cfg->control_port);
    if (inet_pton(AF_INET, cfg->control_host, &addr.sin_addr) != 1) {
        fprintf(stderr, "invalid control_host '%s'\n", cfg->control_host);
        close(listen_fd);
        return -1;
    }
    if (bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        perror("bind");
        close(listen_fd);
        return -1;
    }
    if (listen(listen_fd, 1) != 0) {
        perror("listen");
        close(listen_fd);
        return -1;
    }

    printf("bootstrap mode=server listening on %s:%u\n", cfg->control_host, cfg->control_port);
    fflush(stdout);
    conn_fd = accept(listen_fd, NULL, NULL);
    if (conn_fd < 0) {
        perror("accept");
    } else {
        printf("bootstrap mode=server accepted client\n");
        fflush(stdout);
    }
    close(listen_fd);
    return conn_fd;
}

static int tcp_client(const config_t *cfg) {
    struct sockaddr_in addr;
    int fd;

    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("socket");
        return -1;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(cfg->control_port);
    if (inet_pton(AF_INET, cfg->control_host, &addr.sin_addr) != 1) {
        fprintf(stderr, "invalid control_host '%s'\n", cfg->control_host);
        close(fd);
        return -1;
    }

    printf("bootstrap mode=client connecting to %s:%u\n", cfg->control_host, cfg->control_port);
    fflush(stdout);
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        perror("connect");
        close(fd);
        return -1;
    }
    printf("bootstrap mode=client connected to %s:%u\n", cfg->control_host, cfg->control_port);
    fflush(stdout);
    return fd;
}

static int open_control(const config_t *cfg) {
    return cfg->mode == MODE_SERVER ? tcp_server(cfg) : tcp_client(cfg);
}

static struct ibv_context *open_device(const char *name) {
    struct ibv_device **list;
    struct ibv_context *verbs = NULL;
    int count = 0;
    int i;

    list = ibv_get_device_list(&count);
    if (list == NULL) {
        perror("ibv_get_device_list");
        return NULL;
    }
    for (i = 0; i < count; i++) {
        if (strcmp(ibv_get_device_name(list[i]), name) == 0) {
            verbs = ibv_open_device(list[i]);
            if (verbs == NULL) {
                perror("ibv_open_device");
            }
            break;
        }
    }
    ibv_free_device_list(list);
    if (verbs == NULL) {
        fprintf(stderr, "IB device '%s' not found\n", name);
    }
    return verbs;
}

static int qp_init(struct ibv_qp *qp, uint8_t port) {
    struct ibv_qp_attr attr;

    memset(&attr, 0, sizeof(attr));
    attr.qp_state = IBV_QPS_INIT;
    attr.port_num = port;
    attr.pkey_index = 0;
    attr.qp_access_flags = 0;
    return ibv_modify_qp(qp, &attr,
        IBV_QP_STATE | IBV_QP_PKEY_INDEX | IBV_QP_PORT | IBV_QP_ACCESS_FLAGS);
}

static int qp_rtr(struct ibv_qp *qp, uint8_t port, const peer_info_t *peer) {
    struct ibv_qp_attr attr;

    memset(&attr, 0, sizeof(attr));
    attr.qp_state = IBV_QPS_RTR;
    attr.path_mtu = (enum ibv_mtu)peer->mtu;
    attr.dest_qp_num = peer->qpn;
    attr.rq_psn = peer->psn;
    attr.max_dest_rd_atomic = 1;
    attr.min_rnr_timer = 12;
    attr.ah_attr.dlid = peer->lid;
    attr.ah_attr.sl = 0;
    attr.ah_attr.port_num = port;
    return ibv_modify_qp(qp, &attr,
        IBV_QP_STATE | IBV_QP_AV | IBV_QP_PATH_MTU | IBV_QP_DEST_QPN |
        IBV_QP_RQ_PSN | IBV_QP_MAX_DEST_RD_ATOMIC | IBV_QP_MIN_RNR_TIMER);
}

static int qp_rts(struct ibv_qp *qp, uint32_t psn) {
    struct ibv_qp_attr attr;

    memset(&attr, 0, sizeof(attr));
    attr.qp_state = IBV_QPS_RTS;
    attr.timeout = 14;
    attr.retry_cnt = 7;
    attr.rnr_retry = 7;
    attr.sq_psn = psn;
    attr.max_rd_atomic = 1;
    return ibv_modify_qp(qp, &attr,
        IBV_QP_STATE | IBV_QP_TIMEOUT | IBV_QP_RETRY_CNT |
        IBV_QP_RNR_RETRY | IBV_QP_SQ_PSN | IBV_QP_MAX_QP_RD_ATOMIC);
}

static int rdma_open(const config_t *cfg, rdma_t *rdma) {
    struct ibv_port_attr port_attr;
    struct ibv_qp_init_attr qp_attr;
    size_t bytes;

    memset(rdma, 0, sizeof(*rdma));
    rdma->slot_size = cfg->message_size;
    rdma->depth = cfg->queue_depth;

    rdma->verbs = open_device(cfg->ib_device);
    if (rdma->verbs == NULL) {
        return -1;
    }
    if (ibv_query_port(rdma->verbs, cfg->ib_port, &port_attr) != 0) {
        perror("ibv_query_port");
        return -1;
    }

    rdma->pd = ibv_alloc_pd(rdma->verbs);
    if (rdma->pd == NULL) {
        perror("ibv_alloc_pd");
        return -1;
    }
    rdma->cq = ibv_create_cq(rdma->verbs, (int)(cfg->queue_depth * 2 + 8), NULL, NULL, 0);
    if (rdma->cq == NULL) {
        perror("ibv_create_cq");
        return -1;
    }

    memset(&qp_attr, 0, sizeof(qp_attr));
    qp_attr.send_cq = rdma->cq;
    qp_attr.recv_cq = rdma->cq;
    qp_attr.qp_type = IBV_QPT_RC;
    qp_attr.cap.max_send_wr = cfg->queue_depth + 1;
    qp_attr.cap.max_recv_wr = cfg->queue_depth + 1;
    qp_attr.cap.max_send_sge = 1;
    qp_attr.cap.max_recv_sge = 1;
    rdma->qp = ibv_create_qp(rdma->pd, &qp_attr);
    if (rdma->qp == NULL) {
        perror("ibv_create_qp");
        return -1;
    }
    if (qp_init(rdma->qp, cfg->ib_port) != 0) {
        perror("ibv_modify_qp INIT");
        return -1;
    }

    bytes = rdma->slot_size * rdma->depth;
    rdma->send_buf = xmem(bytes);
    rdma->recv_buf = xmem(bytes);
    rdma->send_mr = ibv_reg_mr(rdma->pd, rdma->send_buf, bytes, 0);
    rdma->recv_mr = ibv_reg_mr(rdma->pd, rdma->recv_buf, bytes, IBV_ACCESS_LOCAL_WRITE);
    if (rdma->send_mr == NULL || rdma->recv_mr == NULL) {
        perror(rdma->send_mr == NULL ? "ibv_reg_mr send" : "ibv_reg_mr recv");
        return -1;
    }

    memset(&rdma->local, 0, sizeof(rdma->local));
    rdma->local.magic = MAGIC;
    rdma->local.lid = port_attr.lid;
    rdma->local.mtu = (uint8_t)port_attr.active_mtu;
    rdma->local.qpn = rdma->qp->qp_num;
    rdma->local.psn = (uint32_t)(rand() & 0x00ffffffu);
    return 0;
}

static void rdma_close(rdma_t *rdma) {
    if (rdma->qp != NULL) {
        ibv_destroy_qp(rdma->qp);
    }
    if (rdma->send_mr != NULL) {
        ibv_dereg_mr(rdma->send_mr);
    }
    if (rdma->recv_mr != NULL) {
        ibv_dereg_mr(rdma->recv_mr);
    }
    if (rdma->cq != NULL) {
        ibv_destroy_cq(rdma->cq);
    }
    if (rdma->pd != NULL) {
        ibv_dealloc_pd(rdma->pd);
    }
    if (rdma->verbs != NULL) {
        ibv_close_device(rdma->verbs);
    }
    free(rdma->send_buf);
    free(rdma->recv_buf);
}

static int exchange_peer(int fd, const peer_info_t *local, peer_info_t *remote) {
    if (write_full(fd, local, sizeof(*local)) != 0 || read_full(fd, remote, sizeof(*remote)) != 0) {
        perror("peer exchange");
        return -1;
    }
    if (remote->magic != MAGIC) {
        fprintf(stderr, "remote peer magic mismatch\n");
        return -1;
    }
    return 0;
}

static int post_recv(const rdma_t *rdma, uint32_t slot) {
    struct ibv_sge sge;
    struct ibv_recv_wr wr;
    struct ibv_recv_wr *bad = NULL;

    memset(&sge, 0, sizeof(sge));
    memset(&wr, 0, sizeof(wr));
    sge.addr = (uintptr_t)(rdma->recv_buf + (size_t)slot * rdma->slot_size);
    sge.length = (uint32_t)rdma->slot_size;
    sge.lkey = rdma->recv_mr->lkey;
    wr.wr_id = slot;
    wr.sg_list = &sge;
    wr.num_sge = 1;
    return ibv_post_recv(rdma->qp, &wr, &bad);
}

static void fill_send(const rdma_t *rdma, uint32_t slot, uint64_t seq) {
    uint8_t *base = rdma->send_buf + (size_t)slot * rdma->slot_size;
    msg_header_t *h = (msg_header_t *)base;
    size_t i;

    h->seq = seq;
    h->bytes = (uint32_t)rdma->slot_size;
    h->magic = MAGIC;
    for (i = sizeof(*h); i < rdma->slot_size; i++) {
        base[i] = (uint8_t)((seq + i) & 0xffu);
    }
}

static int post_send(const rdma_t *rdma, uint32_t slot) {
    struct ibv_sge sge;
    struct ibv_send_wr wr;
    struct ibv_send_wr *bad = NULL;

    memset(&sge, 0, sizeof(sge));
    memset(&wr, 0, sizeof(wr));
    sge.addr = (uintptr_t)(rdma->send_buf + (size_t)slot * rdma->slot_size);
    sge.length = (uint32_t)rdma->slot_size;
    sge.lkey = rdma->send_mr->lkey;
    wr.wr_id = slot;
    wr.sg_list = &sge;
    wr.num_sge = 1;
    wr.opcode = IBV_WR_SEND;
    wr.send_flags = IBV_SEND_SIGNALED;
    return ibv_post_send(rdma->qp, &wr, &bad);
}

static int poll_wc(struct ibv_cq *cq, struct ibv_wc *wc) {
    int rc;

    do {
        rc = ibv_poll_cq(cq, 1, wc);
    } while (rc == 0);
    if (rc < 0) {
        perror("ibv_poll_cq");
        return -1;
    }
    if (wc->status != IBV_WC_SUCCESS) {
        fprintf(stderr, "CQ error: %s opcode=%d\n", ibv_wc_status_str(wc->status), wc->opcode);
        return -1;
    }
    return 0;
}

static void validate_recv(const rdma_t *rdma, uint32_t slot, uint64_t seq,
    uint64_t *bad_seq, uint64_t *bad_payload) {
    uint8_t *base = rdma->recv_buf + (size_t)slot * rdma->slot_size;
    msg_header_t *h = (msg_header_t *)base;
    size_t i;

    if (h->magic != MAGIC || h->bytes != rdma->slot_size || h->seq != seq) {
        (*bad_seq)++;
        return;
    }
    for (i = sizeof(*h); i < rdma->slot_size; i++) {
        if (base[i] != (uint8_t)((seq + i) & 0xffu)) {
            (*bad_payload)++;
            return;
        }
    }
}

static int run_server(const config_t *cfg, const rdma_t *rdma, int fd) {
    uint64_t posted = 0;
    uint64_t received = 0;
    uint64_t bad_seq = 0;
    uint64_t bad_payload = 0;
    uint64_t start_ns;
    summary_t summary;
    uint8_t ready = READY;
    uint32_t warm = cfg->queue_depth;

    if ((uint64_t)warm > cfg->iterations) {
        warm = (uint32_t)cfg->iterations;
    }
    while (posted < warm) {
        if (post_recv(rdma, (uint32_t)posted) != 0) {
            perror("ibv_post_recv");
            return -1;
        }
        posted++;
    }
    if (write_full(fd, &ready, sizeof(ready)) != 0) {
        perror("write ready");
        return -1;
    }

    start_ns = now_ns();
    while (received < cfg->iterations) {
        struct ibv_wc wc;
        uint32_t slot;

        if (poll_wc(rdma->cq, &wc) != 0) {
            return -1;
        }
        if (wc.opcode != IBV_WC_RECV) {
            fprintf(stderr, "unexpected opcode %d on server\n", wc.opcode);
            return -1;
        }

        slot = (uint32_t)wc.wr_id;
        validate_recv(rdma, slot, received, &bad_seq, &bad_payload);
        received++;
        if (posted < cfg->iterations) {
            if (post_recv(rdma, slot) != 0) {
                perror("ibv_post_recv refill");
                return -1;
            }
            posted++;
        }
    }

    memset(&summary, 0, sizeof(summary));
    summary.magic = MAGIC;
    summary.messages = received;
    summary.bad_seq = bad_seq;
    summary.bad_payload = bad_payload;
    summary.elapsed_ns = now_ns() - start_ns;
    summary.status = (bad_seq == 0 && bad_payload == 0) ? 0u : 1u;

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

static int run_client(const config_t *cfg, const rdma_t *rdma, int fd) {
    uint8_t ready;
    uint64_t posted = 0;
    uint64_t done = 0;
    uint32_t inflight = 0;
    uint64_t start_ns;
    uint64_t elapsed_ns;
    summary_t summary;

    if (read_full(fd, &ready, sizeof(ready)) != 0) {
        perror("read ready");
        return -1;
    }
    if (ready != READY) {
        fprintf(stderr, "remote ready mismatch\n");
        return -1;
    }

    start_ns = now_ns();
    while (done < cfg->iterations) {
        while (posted < cfg->iterations && inflight < cfg->queue_depth) {
            uint32_t slot = (uint32_t)(posted % cfg->queue_depth);
            fill_send(rdma, slot, posted);
            if (post_send(rdma, slot) != 0) {
                perror("ibv_post_send");
                return -1;
            }
            posted++;
            inflight++;
        }
        if (inflight > 0) {
            struct ibv_wc wc;

            if (poll_wc(rdma->cq, &wc) != 0) {
                return -1;
            }
            if (wc.opcode != IBV_WC_SEND) {
                fprintf(stderr, "unexpected opcode %d on client\n", wc.opcode);
                return -1;
            }
            done++;
            inflight--;
        }
    }
    elapsed_ns = now_ns() - start_ns;

    if (read_full(fd, &summary, sizeof(summary)) != 0) {
        perror("read summary");
        return -1;
    }
    if (summary.magic != MAGIC) {
        fprintf(stderr, "summary magic mismatch\n");
        return -1;
    }

    printf("client workload=%s messages=%" PRIu64 " bytes=%zu elapsed_ms=%.3f msg_per_sec=%.2f MiB_per_sec=%.2f remote_status=%u remote_bad_seq=%" PRIu64 " remote_bad_payload=%" PRIu64 "\n",
        cfg->workload == WORKLOAD_CHECK ? "check" : "bench",
        cfg->iterations,
        cfg->message_size,
        (double)elapsed_ns / 1000000.0,
        (double)cfg->iterations * 1000000000.0 / (double)elapsed_ns,
        ((double)cfg->iterations * (double)cfg->message_size * 1000000000.0) / ((double)elapsed_ns * 1024.0 * 1024.0),
        summary.status,
        summary.bad_seq,
        summary.bad_payload);
    return summary.status == 0 ? 0 : -1;
}

int main(int argc, char **argv) {
    config_t cfg;
    rdma_t rdma;
    peer_info_t remote;
    int fd = -1;
    int rc = 1;

    if (argc != 2) {
        fprintf(stderr, "usage: %s <config>\n", argv[0]);
        return 1;
    }
    if (parse_config(argv[1], &cfg) != 0) {
        return 1;
    }

    printf("startup mode=%s workload=%s control=%s:%u ib_device=%s ib_port=%u message_size=%zu iterations=%" PRIu64 " queue_depth=%u\n",
        mode_name(cfg.mode),
        cfg.workload == WORKLOAD_CHECK ? "check" : "bench",
        cfg.control_host,
        cfg.control_port,
        cfg.ib_device,
        (unsigned int)cfg.ib_port,
        cfg.message_size,
        cfg.iterations,
        cfg.queue_depth);
    fflush(stdout);

    srand((unsigned int)(time(NULL) ^ (time_t)getpid()));
    if (rdma_open(&cfg, &rdma) != 0) {
        rdma_close(&rdma);
        return 1;
    }

    fd = open_control(&cfg);
    if (fd < 0) {
        rdma_close(&rdma);
        return 1;
    }
    if (exchange_peer(fd, &rdma.local, &remote) != 0) {
        goto out;
    }
    if (remote.mtu > rdma.local.mtu) {
        remote.mtu = rdma.local.mtu;
    }
    if (qp_rtr(rdma.qp, cfg.ib_port, &remote) != 0 || qp_rts(rdma.qp, rdma.local.psn) != 0) {
        perror("ibv_modify_qp");
        goto out;
    }

    printf("connected mode=%s control=%s:%u local_lid=%u remote_lid=%u local_qpn=%u remote_qpn=%u\n",
        mode_name(cfg.mode),
        cfg.control_host,
        cfg.control_port,
        (unsigned int)rdma.local.lid,
        (unsigned int)remote.lid,
        rdma.local.qpn,
        remote.qpn);
    fflush(stdout);

    rc = cfg.mode == MODE_SERVER ? run_server(&cfg, &rdma, fd) : run_client(&cfg, &rdma, fd);

out:
    if (fd >= 0) {
        close(fd);
    }
    rdma_close(&rdma);
    return rc == 0 ? 0 : 1;
}
