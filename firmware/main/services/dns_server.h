#pragma once

#include "esp_err.h"

typedef struct dns_server_handle *dns_server_handle_t;

/** Start a DNS server that answers ALL A queries with the AP interface IP. */
dns_server_handle_t dns_server_start(void);

/** Stop and free the DNS server. */
void dns_server_stop(dns_server_handle_t handle);
