/* Minimal DNS server: answers every A query with the SoftAP IP address.
 * Based on ESP-IDF captive_portal example (CC0). */

#include "dns_server.h"

#include <string.h>
#include <stdlib.h>

#include "esp_log.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"

#define DNS_PORT 53
#define DNS_MAX_LEN 256
#define QR_FLAG (1 << 7)
#define OPCODE_MASK 0x7800
#define QD_TYPE_A 0x0001
#define ANS_TTL_SEC 300

static const char *TAG = "dns_srv";

typedef struct __attribute__((__packed__)) {
    uint16_t id;
    uint16_t flags;
    uint16_t qd_count;
    uint16_t an_count;
    uint16_t ns_count;
    uint16_t ar_count;
} dns_header_t;

typedef struct {
    uint16_t type;
    uint16_t class;
} dns_question_t;

typedef struct __attribute__((__packed__)) {
    uint16_t ptr_offset;
    uint16_t type;
    uint16_t class;
    uint32_t ttl;
    uint16_t addr_len;
    uint32_t ip_addr;
} dns_answer_t;

struct dns_server_handle {
    volatile bool started;
    TaskHandle_t task;
};

static char *parse_dns_name(char *raw, char *out, size_t out_max)
{
    char *label = raw;
    char *itr = out;
    int len = 0;
    do {
        int sub = *label;
        len += sub + 1;
        if (len > (int)out_max) return NULL;
        memcpy(itr, label + 1, sub);
        itr[sub] = '.';
        itr += sub + 1;
        label += sub + 1;
    } while (*label != 0);
    out[len - 1] = '\0';
    return label + 1;
}

static int build_reply(char *req, size_t req_len, char *reply, size_t reply_max,
                       uint32_t ap_ip)
{
    if (req_len > reply_max) return -1;
    memset(reply, 0, reply_max);
    memcpy(reply, req, req_len);

    dns_header_t *hdr = (dns_header_t *)reply;
    if ((hdr->flags & OPCODE_MASK) != 0) return 0;
    hdr->flags |= QR_FLAG;

    uint16_t qd_count = ntohs(hdr->qd_count);
    hdr->an_count = htons(qd_count);

    int reply_len = qd_count * sizeof(dns_answer_t) + req_len;
    if (reply_len > (int)reply_max) return -1;

    char *cur_ans = reply + req_len;
    char *cur_qd = reply + sizeof(dns_header_t);
    char name[128];

    for (int i = 0; i < qd_count; i++) {
        char *end = parse_dns_name(cur_qd, name, sizeof(name));
        if (!end) return -1;
        dns_question_t *q = (dns_question_t *)end;
        if (ntohs(q->type) == QD_TYPE_A) {
            dns_answer_t *ans = (dns_answer_t *)cur_ans;
            ans->ptr_offset = htons(0xC000 | (cur_qd - reply));
            ans->type = q->type;
            ans->class = q->class;
            ans->ttl = htonl(ANS_TTL_SEC);
            ans->addr_len = htons(4);
            ans->ip_addr = ap_ip;
        }
    }
    return reply_len;
}

static void dns_task(void *arg)
{
    dns_server_handle_t h = arg;
    char rx[128];
    char reply[DNS_MAX_LEN];

    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        ESP_LOGE(TAG, "socket: %d", errno);
        vTaskDelete(NULL);
        return;
    }
    struct sockaddr_in bind_addr = {
        .sin_family = AF_INET,
        .sin_port = htons(DNS_PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (bind(sock, (struct sockaddr *)&bind_addr, sizeof(bind_addr)) < 0) {
        ESP_LOGE(TAG, "bind: %d", errno);
        close(sock);
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "listening on :%d", DNS_PORT);

    while (h->started) {
        struct sockaddr_in src;
        socklen_t slen = sizeof(src);
        int len = recvfrom(sock, rx, sizeof(rx) - 1, 0,
                           (struct sockaddr *)&src, &slen);
        if (len < 0) {
            if (h->started) ESP_LOGE(TAG, "recvfrom: %d", errno);
            break;
        }
        rx[len] = 0;

        /* Get AP IP dynamically */
        esp_netif_ip_info_t ip_info;
        esp_netif_t *ap = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
        if (ap == NULL || esp_netif_get_ip_info(ap, &ip_info) != ESP_OK)
            continue;

        int rlen = build_reply(rx, len, reply, sizeof(reply), ip_info.ip.addr);
        if (rlen > 0) {
            sendto(sock, reply, rlen, 0, (struct sockaddr *)&src, sizeof(src));
        }
    }
    close(sock);
    vTaskDelete(NULL);
}

dns_server_handle_t dns_server_start(void)
{
    dns_server_handle_t h = calloc(1, sizeof(struct dns_server_handle));
    if (!h) return NULL;
    h->started = true;
    if (xTaskCreate(dns_task, "dns_srv", 3072, h, 5, &h->task) != pdPASS) {
        free(h);
        return NULL;
    }
    return h;
}

void dns_server_stop(dns_server_handle_t h)
{
    if (!h) return;
    h->started = false;
    vTaskDelete(h->task);
    free(h);
}
