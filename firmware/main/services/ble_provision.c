#include "ble_provision.h"

#include "sdkconfig.h"

#ifdef CONFIG_BT_NIMBLE_ENABLED

#include "services/config_store.h"
#include "services/network_manager.h"
#include "version.h"

#include "esp_bt.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "nimble/ble.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "ble_prov";

/* NUS (Nordic UART Service) UUIDs */
static const ble_uuid128_t k_nus_svc_uuid =
    BLE_UUID128_INIT(0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0,
                     0x93, 0xf3, 0xa3, 0xb5, 0x01, 0x00, 0x40, 0x6e);
static const ble_uuid128_t k_nus_rx_uuid =
    BLE_UUID128_INIT(0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0,
                     0x93, 0xf3, 0xa3, 0xb5, 0x02, 0x00, 0x40, 0x6e);
static const ble_uuid128_t k_nus_tx_uuid =
    BLE_UUID128_INIT(0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0,
                     0x93, 0xf3, 0xa3, 0xb5, 0x03, 0x00, 0x40, 0x6e);

#define BLE_DEVICE_NAME "OpenFMO-CFG"
#define CMD_BUF_SIZE 160

/* --- State --- */
static bool s_active;
static bool s_host_synced;
static bool s_client_connected;
static bool s_advertising;
static bool s_web_client_connected;
static uint16_t s_conn_handle;
static uint16_t s_tx_val_handle;
static char s_cmd_buf[CMD_BUF_SIZE];
static size_t s_cmd_len;
static bool s_reboot_pending;
static uint32_t s_reboot_at_ms;
static bool s_scan_pending;
static bool s_wifi_result_pending;

/* WiFi transaction (BEGIN/SET/APPLY) */
static bool s_wifi_txn;
static char s_staged_ssid[33];
static char s_staged_pass[65];

#define BLE_MIN_FREE_INTERNAL          (40U * 1024U)
#define BLE_MIN_LARGEST_INTERNAL_BLOCK (12U * 1024U)

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000ULL);
}

/* --- Send notification over NUS TX --- */
static void send_line(const char *line)
{
    if (!line) return;
    ESP_LOGI(TAG, "tx: %s", line);
    if (!s_client_connected || s_tx_val_handle == 0) return;

    struct os_mbuf *om = ble_hs_mbuf_from_flat(line, strlen(line));
    if (om) {
        ble_gatts_notify_custom(s_conn_handle, s_tx_val_handle, om);
    }
}

/* --- Command handlers --- */
static void send_config(void)
{
    fmo_config_t cfg;
    config_store_load(&cfg);
    char line[128];
    snprintf(line, sizeof(line), "WIFI_SSID=%s", cfg.wifi_ssid);
    send_line(line);
    snprintf(line, sizeof(line), "SERVER_HOST=%s", cfg.nrl_host);
    send_line(line);
    snprintf(line, sizeof(line), "SERVER_PORT=%u", (unsigned)cfg.nrl_port);
    send_line(line);
    snprintf(line, sizeof(line), "CHANNEL=%u", (unsigned)cfg.nrl_channel);
    send_line(line);
    snprintf(line, sizeof(line), "CALLSIGN=%s", cfg.callsign);
    send_line(line);
    snprintf(line, sizeof(line), "CALL_SSID=%u", (unsigned)cfg.callsign_ssid);
    send_line(line);
    snprintf(line, sizeof(line), "RX_MHZ=%.4f", (double)cfg.radio_rx_mhz);
    send_line(line);
    snprintf(line, sizeof(line), "TX_MHZ=%.4f", (double)cfg.radio_tx_mhz);
    send_line(line);
    send_line("OK GET");
}

static void send_help(void)
{
    send_line("OpenFMO BLE config:");
    send_line("GET");
    send_line("SET WIFI_SSID=<ssid>");
    send_line("SET WIFI_PASS=<password>");
    send_line("SET SERVER_HOST=<host>");
    send_line("SET SERVER_PORT=<1..65535>");
    send_line("SET CHANNEL=<0..7>");
    send_line("SET CALLSIGN=<call>");
    send_line("SET CALL_SSID=<0..255>");
    send_line("SCAN");
    send_line("BEGIN");
    send_line("APPLY");
    send_line("REBOOT");
}

static void do_scan(void)
{
    wifi_ap_record_t records[24];
    uint16_t count = 24;
    if (network_manager_scan_records(records, &count) != ESP_OK) {
        send_line("ERR SCAN");
        return;
    }
    char line[80];
    size_t reported = 0;
    for (uint16_t i = 0; i < count && reported < 20; ++i) {
        if (records[i].ssid[0] == '\0') continue;
        /* de-dup */
        bool dup = false;
        for (uint16_t j = 0; j < i; ++j) {
            if (strcmp((char *)records[i].ssid, (char *)records[j].ssid) == 0) {
                dup = true;
                break;
            }
        }
        if (dup) continue;
        snprintf(line, sizeof(line), "WIFI=%s,%d",
                 records[i].ssid, (int)records[i].rssi);
        send_line(line);
        ++reported;
    }
    send_line("OK SCAN");
}

static void set_field(const char *key, const char *value)
{
    fmo_config_t cfg;
    config_store_load(&cfg);
    bool save = true;
    bool restart_wifi = false;

    if (strcmp(key, "WIFI_SSID") == 0 || strcmp(key, "SSID") == 0) {
        if (strlen(value) > 32) { send_line("ERR SET"); return; }
        strlcpy(cfg.wifi_ssid, value, sizeof(cfg.wifi_ssid));
        restart_wifi = true;
    } else if (strcmp(key, "WIFI_PASS") == 0 || strcmp(key, "PASS") == 0) {
        if (strlen(value) > 64) { send_line("ERR SET"); return; }
        strlcpy(cfg.wifi_password, value, sizeof(cfg.wifi_password));
        restart_wifi = true;
    } else if (strcmp(key, "SERVER_HOST") == 0) {
        strlcpy(cfg.nrl_host, value, sizeof(cfg.nrl_host));
    } else if (strcmp(key, "SERVER_PORT") == 0) {
        unsigned long port = strtoul(value, NULL, 10);
        if (port == 0 || port > 65535) { send_line("ERR SET"); return; }
        cfg.nrl_port = (uint16_t)port;
    } else if (strcmp(key, "CHANNEL") == 0) {
        unsigned long ch = strtoul(value, NULL, 10);
        if (ch > 7) { send_line("ERR SET"); return; }
        cfg.nrl_channel = (uint8_t)ch;
    } else if (strcmp(key, "CALLSIGN") == 0 || strcmp(key, "CALL") == 0) {
        strlcpy(cfg.callsign, value, sizeof(cfg.callsign));
    } else if (strcmp(key, "CALL_SSID") == 0) {
        unsigned long ssid = strtoul(value, NULL, 10);
        if (ssid > 255) { send_line("ERR SET"); return; }
        cfg.callsign_ssid = (uint8_t)ssid;
    } else {
        send_line("ERR SET");
        return;
    }

    if (save && config_store_save(&cfg) != ESP_OK) {
        send_line("ERR SAVE");
        return;
    }
    if (restart_wifi) {
        network_manager_update_profiles(&cfg, true);
    }
    send_line("OK SET");
}

static void handle_command(char *cmd)
{
    /* trim */
    while (*cmd == ' ' || *cmd == '\t') ++cmd;
    size_t len = strlen(cmd);
    while (len > 0 && (cmd[len-1] == ' ' || cmd[len-1] == '\t' ||
                       cmd[len-1] == '\r' || cmd[len-1] == '\n'))
        cmd[--len] = '\0';
    if (len == 0) return;

    ESP_LOGI(TAG, "rx: %s", cmd);

    if (strcasecmp(cmd, "HELP") == 0 || strcmp(cmd, "?") == 0) {
        send_help();
    } else if (strcasecmp(cmd, "GET") == 0) {
        send_config();
    } else if (strcasecmp(cmd, "SCAN") == 0) {
        s_scan_pending = true;
    } else if (strcasecmp(cmd, "BEGIN") == 0) {
        fmo_config_t cfg;
        config_store_load(&cfg);
        strlcpy(s_staged_ssid, cfg.wifi_ssid, sizeof(s_staged_ssid));
        strlcpy(s_staged_pass, cfg.wifi_password, sizeof(s_staged_pass));
        s_wifi_txn = true;
        send_line("OK BEGIN");
    } else if (strcasecmp(cmd, "APPLY") == 0) {
        if (s_wifi_txn) {
            if (s_staged_ssid[0] == '\0') {
                send_line("ERR APPLY");
                return;
            }
            fmo_config_t cfg;
            config_store_load(&cfg);
            config_store_wifi_add(&cfg, s_staged_ssid, s_staged_pass, true);
            config_store_save(&cfg);
            network_manager_update_profiles(&cfg, true);
            s_wifi_txn = false;
            s_wifi_result_pending = true;
            send_line("OK APPLY");
            send_line("WIFI_STATE=CONNECTING");
        } else {
            send_line("OK APPLY");
        }
    } else if (strcasecmp(cmd, "REBOOT") == 0) {
        s_reboot_pending = true;
        s_reboot_at_ms = now_ms() + 500;
        send_line("OK REBOOT");
    } else if (strncasecmp(cmd, "SET ", 4) == 0) {
        char *assign = cmd + 4;
        while (*assign == ' ') ++assign;
        char *eq = strchr(assign, '=');
        if (!eq) { send_line("ERR SET"); return; }
        *eq = '\0';
        char *key = assign;
        char *val = eq + 1;
        /* uppercase key */
        for (char *p = key; *p; ++p) *p = toupper((unsigned char)*p);

        if (s_wifi_txn &&
            (strcmp(key, "WIFI_SSID") == 0 || strcmp(key, "SSID") == 0)) {
            if (strlen(val) > 32) { send_line("ERR SET"); return; }
            strlcpy(s_staged_ssid, val, sizeof(s_staged_ssid));
            s_staged_pass[0] = '\0';
            send_line("OK SET");
        } else if (s_wifi_txn &&
                   (strcmp(key, "WIFI_PASS") == 0 || strcmp(key, "PASS") == 0)) {
            if (strlen(val) > 64) { send_line("ERR SET"); return; }
            strlcpy(s_staged_pass, val, sizeof(s_staged_pass));
            send_line("OK SET");
        } else {
            set_field(key, val);
        }
    } else {
        send_line("ERR UNKNOWN");
    }
}

static void feed_data(const uint8_t *data, size_t len)
{
    for (size_t i = 0; i < len; ++i) {
        char ch = (char)data[i];
        if (ch == '\n' || ch == '\r') {
            if (s_cmd_len > 0) {
                s_cmd_buf[s_cmd_len] = '\0';
                handle_command(s_cmd_buf);
                s_cmd_len = 0;
            }
        } else if (s_cmd_len + 1 < CMD_BUF_SIZE) {
            s_cmd_buf[s_cmd_len++] = ch;
        } else {
            s_cmd_len = 0;
            send_line("ERR LINE_TOO_LONG");
        }
    }
}

/* --- GATT service table --- */
static int nus_rx_access(uint16_t conn_handle, uint16_t attr_handle,
                         struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn_handle; (void)attr_handle; (void)arg;
    if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
        const struct os_mbuf *om = ctxt->om;
        while (om) {
            feed_data(om->om_data, om->om_len);
            om = SLIST_NEXT(om, om_next);
        }
        return 0;
    }
    return BLE_ATT_ERR_READ_NOT_PERMITTED;
}

/* NimBLE requires every characteristic definition to have an access callback,
 * including notify-only values. Notifications themselves use the value handle
 * directly, so no ATT operation is accepted here. */
static int nus_tx_access(uint16_t conn_handle, uint16_t attr_handle,
                         struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn_handle;
    (void)attr_handle;
    (void)ctxt;
    (void)arg;
    return BLE_ATT_ERR_READ_NOT_PERMITTED;
}

static const struct ble_gatt_svc_def k_gatt_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &k_nus_svc_uuid.u,
        .characteristics = (struct ble_gatt_chr_def[]){
            {
                /* RX: client writes commands here */
                .uuid = &k_nus_rx_uuid.u,
                .access_cb = nus_rx_access,
                .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP,
            },
            {
                /* TX: device notifies responses here */
                .uuid = &k_nus_tx_uuid.u,
                .access_cb = nus_tx_access,
                .val_handle = &s_tx_val_handle,
                .flags = BLE_GATT_CHR_F_NOTIFY,
            },
            {0},
        },
    },
    {0},
};

/* --- GAP event --- */
static int gap_event(struct ble_gap_event *event, void *arg)
{
    (void)arg;
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            s_client_connected = true;
            s_advertising = false;
            s_conn_handle = event->connect.conn_handle;
            ESP_LOGI(TAG, "client connected");
            char banner[64];
            snprintf(banner, sizeof(banner), "%s v%s BLE ready. Send HELP.",
                     FMO_FIRMWARE_NAME, FMO_FIRMWARE_VERSION);
            send_line(banner);
        } else {
            ble_gap_terminate(event->connect.conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        }
        return 0;

    case BLE_GAP_EVENT_DISCONNECT:
        s_client_connected = false;
        s_advertising = false;
        s_wifi_txn = false;
        ESP_LOGI(TAG, "client disconnected");
        return 0;

    case BLE_GAP_EVENT_ADV_COMPLETE:
        s_advertising = false;
        return 0;

    default:
        return 0;
    }
}

static void start_advertising(void)
{
    if (!s_host_synced || s_advertising || s_client_connected ||
        s_web_client_connected) return;

    struct ble_gap_adv_params params = {0};
    params.conn_mode = BLE_GAP_CONN_MODE_UND;
    params.disc_mode = BLE_GAP_DISC_MODE_GEN;
    /* 100-150 ms is still discovered quickly, without monopolizing RF time
     * needed by the provisioning SoftAP's DHCP exchange. */
    params.itvl_min = 0xa0;
    params.itvl_max = 0xf0;

    int rc = ble_gap_adv_start(BLE_OWN_ADDR_PUBLIC, NULL, BLE_HS_FOREVER,
                               &params, gap_event, NULL);
    if (rc == 0) {
        s_advertising = true;
    } else {
        ESP_LOGW(TAG, "adv start rc=%d", rc);
    }
}

/* --- NimBLE host task --- */
static void ble_host_task(void *param)
{
    (void)param;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

static void on_sync(void)
{
    s_host_synced = true;
    ble_svc_gap_device_name_set(BLE_DEVICE_NAME);
    start_advertising();
    if (s_advertising) {
        ESP_LOGI(TAG, "advertising as %s", BLE_DEVICE_NAME);
    } else if (s_web_client_connected) {
        ESP_LOGI(TAG, "advertising paused for Web provisioning client");
    }
}

static void on_reset(int reason)
{
    ESP_LOGW(TAG, "host reset reason=%d", reason);
    s_host_synced = false;
    s_client_connected = false;
    s_advertising = false;
}

/* --- Public API --- */
esp_err_t ble_provision_start(void)
{
    if (s_active) return ESP_OK;

    ESP_LOGI(TAG, "starting BLE provisioning");
    s_host_synced = false;

    /* NimBLE allocations use PSRAM in this build. Keep enough internal DRAM
     * for the controller, host task metadata and Wi-Fi coexistence, without
     * applying the much larger Bluedroid-oriented safety gate. */
    const size_t free_internal =
        heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    const size_t largest_internal =
        heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (free_internal < BLE_MIN_FREE_INTERNAL ||
        largest_internal < BLE_MIN_LARGEST_INTERNAL_BLOCK) {
        ESP_LOGW(TAG, "BLE skipped: internal free=%u largest=%u (need >=40K / >=12K)",
                 (unsigned)free_internal, (unsigned)largest_internal);
        return ESP_ERR_NO_MEM;
    }

    /* nimble_port_init() initializes BOTH the BT controller and the NimBLE
     * host stack in the correct order. */
    esp_err_t err = nimble_port_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nimble_port_init: %s", esp_err_to_name(err));
        return err;
    }
    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.reset_cb = on_reset;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;

    ble_svc_gap_init();
    ble_svc_gatt_init();

    /* Register NUS service and find TX value handle */
    int rc = ble_gatts_count_cfg(k_gatt_svcs);
    if (rc != 0) {
        ESP_LOGE(TAG, "gatts count rc=%d", rc);
        nimble_port_deinit();
        return ESP_FAIL;
    }
    rc = ble_gatts_add_svcs(k_gatt_svcs);
    if (rc != 0) {
        ESP_LOGE(TAG, "gatts add rc=%d", rc);
        nimble_port_deinit();
        return ESP_FAIL;
    }

    /* Set advertising data: service UUID + name in scan response */
    struct ble_hs_adv_fields fields = {0};
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.uuids128 = &k_nus_svc_uuid;
    fields.num_uuids128 = 1;
    fields.uuids128_is_complete = 1;
    ble_gap_adv_set_fields(&fields);

    struct ble_hs_adv_fields scan_rsp = {0};
    scan_rsp.name = (uint8_t *)BLE_DEVICE_NAME;
    scan_rsp.name_len = strlen(BLE_DEVICE_NAME);
    scan_rsp.name_is_complete = 1;
    ble_gap_adv_rsp_set_fields(&scan_rsp);

    err = nimble_port_freertos_init(ble_host_task);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nimble host task: %s", esp_err_to_name(err));
        nimble_port_deinit();
        return err;
    }

    s_active = true;
    return ESP_OK;
}

void ble_provision_stop(void)
{
    if (!s_active) return;

    ESP_LOGI(TAG, "stopping BLE");
    if (s_client_connected) {
        ble_gap_terminate(s_conn_handle, BLE_ERR_REM_USER_CONN_TERM);
    }
    ble_gap_adv_stop();
    int rc = nimble_port_stop();
    if (rc != 0) {
        ESP_LOGW(TAG, "nimble stop rc=%d", rc);
    }
    esp_err_t err = nimble_port_deinit();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "nimble deinit: %s", esp_err_to_name(err));
    } else {
        /* Provisioning is complete. BLE is not restarted until the next boot,
         * so return its static controller memory to the system as well. */
        err = esp_bt_controller_mem_release(ESP_BT_MODE_BTDM);
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
            ESP_LOGW(TAG, "BT memory release: %s", esp_err_to_name(err));
        }
    }

    s_active = false;
    s_host_synced = false;
    s_advertising = false;
    s_client_connected = false;
    s_wifi_txn = false;
    s_tx_val_handle = 0;
}

void ble_provision_set_web_client_connected(bool connected)
{
    if (s_web_client_connected == connected) return;
    s_web_client_connected = connected;
    if (!s_active || !s_host_synced) return;

    if (connected) {
        if (s_advertising) {
            int rc = ble_gap_adv_stop();
            if (rc != 0 && rc != BLE_HS_EALREADY) {
                ESP_LOGW(TAG, "pause advertising rc=%d", rc);
            }
        }
        ESP_LOGI(TAG, "advertising paused; Web provisioning has RF priority");
    } else {
        ESP_LOGI(TAG, "Web client left; BLE advertising may resume");
        /* ble_provision_poll() restarts advertising after the stop-complete
         * event has updated s_advertising. */
    }
}

bool ble_provision_is_active(void)
{
    return s_active;
}

void ble_provision_poll(void)
{
    if (!s_active) return;

    network_status_t net;
    network_manager_get_status(&net);

    /* Provisioning ends as soon as the new Wi-Fi profile obtains an IP.
     * Send the final result while the BLE link still exists, then tear the
     * stack down even if the phone remains connected. */
    if (net.station_connected) {
        if (s_wifi_result_pending) {
            char line[48];
            snprintf(line, sizeof(line), "WIFI_STATE=GOT_IP,%s", net.ip_address);
            send_line(line);
            s_wifi_result_pending = false;
            vTaskDelay(pdMS_TO_TICKS(100));
        }
        ESP_LOGI(TAG, "WiFi connected, provisioning complete; stopping BLE");
        ble_provision_stop();
        return;
    }

    /* Restart advertising if disconnected */
    if (s_host_synced && !s_client_connected && !s_advertising) {
        start_advertising();
    }

    /* Deferred scan */
    if (s_scan_pending) {
        s_scan_pending = false;
        if (s_client_connected) do_scan();
    }

    /* Deferred reboot */
    if (s_reboot_pending && (int32_t)(now_ms() - s_reboot_at_ms) >= 0) {
        ESP_LOGI(TAG, "reboot now");
        vTaskDelay(pdMS_TO_TICKS(100));
        esp_restart();
    }
}

#else /* !CONFIG_BT_NIMBLE_ENABLED */

esp_err_t ble_provision_start(void) { return ESP_ERR_NOT_SUPPORTED; }
void ble_provision_poll(void) {}
bool ble_provision_is_active(void) { return false; }
void ble_provision_stop(void) {}
void ble_provision_set_web_client_connected(bool connected) { (void)connected; }

#endif /* CONFIG_BT_NIMBLE_ENABLED */
