#include "fmo_link.h"

#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include "audio/audio_passthrough.h"
#include "audio/ima_adpcm.h"
#include "audio/opus_voice.h"
#include "esp_log.h"
#include "esp_random.h"
#include "services/fmo_aprs.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "mqtt_client.h"
#include "mqtt5_client.h"
#include "sodium.h"
#include "services/fmo_cert_store.h"
#include "services/fmo_frame.h"
#include "services/fmo_qso.h"
#include "services/fmo_station_beacon.h"
#include "services/network_manager.h"
#include "services/server_directory.h"

#define RAW_MAX_SIZE 8192U

static const char *TAG = "fmo_link";
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static TaskHandle_t s_task;
static esp_mqtt_client_handle_t s_client;
static fmo_link_status_t s_status;
static opus_voice_decoder_t *s_opus_decoder;
static opus_voice_encoder_t *s_tx_encoder;
static SemaphoreHandle_t s_client_mutex;
static uint8_t *s_raw;
static bool s_no_local = true;
static size_t s_raw_size;
static size_t s_raw_expected;
static char s_active_key[FMO_SERVER_KEY_MAX];
static char s_requested_key[FMO_SERVER_KEY_MAX];
static char s_pending_key[FMO_SERVER_KEY_MAX];
static char s_pending_role[8] = "user";
static char s_super_denied_key[FMO_SERVER_KEY_MAX];
static char s_configured_callsign[16] = "NOCALL";
static uint16_t s_client_suffix;
static int64_t s_voice_until_us;
static bool s_recreate_client;

#define TX_PACKETS_PER_FRAME 6U
static bool s_tx_active;
static int16_t s_tx_pcm[320];
static size_t s_tx_pcm_count;
static uint8_t s_tx_packets[TX_PACKETS_PER_FRAME][OPUS_VOICE_MAX_FRAME_BYTES];
static size_t s_tx_packet_sizes[TX_PACKETS_PER_FRAME];
static size_t s_tx_packet_count;
static uint32_t s_tx_packet_total;
static uint32_t s_tx_frame_count;
static uint16_t s_tx_session;
static uint32_t s_tx_started_ms;
static char s_tx_callsign[16];

static bool decode_block(void *context, fmo_frame_codec_t codec,
                         const uint8_t *data, size_t data_size,
                         int16_t adpcm_sample, uint8_t adpcm_index)
{
    (void)context;
    int16_t pcm[960];
    int samples = -1;
    if (codec == FMO_FRAME_ADPCM) {
        samples = (int)ima_adpcm_decode_fmo(data, data_size, adpcm_sample,
                                            adpcm_index, pcm,
                                            sizeof(pcm) / sizeof(pcm[0]));
    } else if (codec == FMO_FRAME_OPUS && s_opus_decoder != NULL) {
        samples = opus_voice_decode(s_opus_decoder, data, data_size, pcm,
                                    sizeof(pcm) / sizeof(pcm[0]));
    }
    if (samples <= 0) return true;
    audio_passthrough_queue_fmo_output(pcm, (size_t)samples, 8000);
    portENTER_CRITICAL(&s_lock);
    snprintf(s_status.voice_codec, sizeof(s_status.voice_codec), "%s",
             codec == FMO_FRAME_ADPCM ? "ADPCM" : "OPUS");
    s_voice_until_us = esp_timer_get_time() + 900000;
    portEXIT_CRITICAL(&s_lock);
    return true;
}

static void process_raw(const uint8_t *data, size_t size)
{
    fmo_frame_info_t info;
    if (!fmo_frame_parse(data, size, &info, decode_block, NULL)) {
        portENTER_CRITICAL(&s_lock);
        ++s_status.parse_errors;
        portEXIT_CRITICAL(&s_lock);
        return;
    }
    audio_passthrough_note_network_voice(
        AUDIO_NETWORK_FMO, info.callsign,
        s_status.voice_codec[0] != '\0' ? s_status.voice_codec : "FMO");
    portENTER_CRITICAL(&s_lock);
    snprintf(s_status.voice_callsign, sizeof(s_status.voice_callsign), "%s",
             info.callsign);
    s_status.receiving = true;
    ++s_status.rx_frames;
    s_voice_until_us = esp_timer_get_time() + 900000;
    portEXIT_CRITICAL(&s_lock);
}

static bool topic_is_raw(const esp_mqtt_event_t *event)
{
    static const char topic[] = "FMO/RAW";
    return event->topic_len == (int)(sizeof(topic) - 1) &&
           event->topic != NULL &&
           memcmp(event->topic, topic, sizeof(topic) - 1) == 0;
}

/* QSO record topic FMO/QSO/UID/<uid>: the peer publishes its side of an
 * established QSO here; payloads are small JSON documents. */
#define QSO_RECORD_MAX_SIZE 2048U
static uint8_t s_qso[QSO_RECORD_MAX_SIZE];
static size_t s_qso_size;
static size_t s_qso_expected;

static bool topic_is_qso_record(const esp_mqtt_event_t *event)
{
    static const char prefix[] = "FMO/QSO/UID/";
    return event->topic != NULL &&
           event->topic_len > (int)(sizeof(prefix) - 1) &&
           memcmp(event->topic, prefix, sizeof(prefix) - 1) == 0;
}

/* Heartbeat topic FMO/LATE/UID_V1/<uid>: feeds the online roster.  The uid
 * is the decimal topic tail; uid 0 is a valid device and counts too. */
static bool topic_is_heartbeat(const esp_mqtt_event_t *event, uint32_t *uid)
{
    static const char prefix[] = "FMO/LATE/UID_V1/";
    if (event->topic == NULL ||
        event->topic_len <= (int)(sizeof(prefix) - 1) ||
        memcmp(event->topic, prefix, sizeof(prefix) - 1) != 0) return false;
    uint32_t value = 0;
    for (int i = (int)(sizeof(prefix) - 1); i < event->topic_len; ++i) {
        const char c = event->topic[i];
        if (c < '0' || c > '9' ||
            value > (UINT32_MAX - 9U) / 10U) return false;
        value = value * 10U + (uint32_t)(c - '0');
    }
    *uid = value;
    return true;
}

static void mqtt_event(void *argument, esp_event_base_t base,
                       int32_t event_id, void *event_data)
{
    (void)argument;
    (void)base;
    esp_mqtt_event_t *event = event_data;
    if (event_id == MQTT_EVENT_CONNECTED) {
        bool no_local;
        portENTER_CRITICAL(&s_lock);
        no_local = s_no_local;
        portEXIT_CRITICAL(&s_lock);
        esp_mqtt5_subscribe_property_config_t property = {
            .no_local_flag = no_local,
        };
        esp_err_t property_error =
            esp_mqtt5_client_set_subscribe_property(event->client, &property);
        if (property_error != ESP_OK) {
            ESP_LOGW(TAG, "failed to set MQTT 5 No Local: %s",
                     esp_err_to_name(property_error));
        }
        esp_mqtt_client_subscribe(event->client, "FMO/RAW", 0);
        /* Per-device heartbeats, ~1/min each: source of the auto online
         * count for the STATION broadcast. */
        esp_mqtt_client_subscribe(event->client, "FMO/LATE/UID_V1/#", 0);
        /* Peer QSO records addressed to this device. */
        fmo_identity_status_t identity;
        if (fmo_cert_store_status(&identity) == ESP_OK && identity.ready) {
            char qso_topic[40];
            snprintf(qso_topic, sizeof(qso_topic), "FMO/QSO/UID/%lu",
                     (unsigned long)identity.uid);
            esp_mqtt_client_subscribe(event->client, qso_topic, 0);
        }
        portENTER_CRITICAL(&s_lock);
        s_status.connected = true;
        s_status.last_error = ESP_OK;
        /* The SAS accepted this connection: record the role it was made
         * with.  The STATION broadcast gate reads exactly this value (the
         * role the broker granted), never a guess. */
        snprintf(s_status.role, sizeof(s_status.role), "%s", s_pending_role);
        portEXIT_CRITICAL(&s_lock);
        ESP_LOGI(TAG, "connected and subscribed FMO/RAW + FMO/LATE/UID_V1/# "
                 "(No Local=%u, role=%s)",
                 no_local ? 1u : 0u, s_pending_role);
    } else if (event_id == MQTT_EVENT_DISCONNECTED) {
        fmo_station_clear_online();
        portENTER_CRITICAL(&s_lock);
        s_status.connected = false;
        s_status.receiving = false;
        s_status.role[0] = '\0';
        if (s_status.last_error == ESP_OK) {
                s_status.last_error = ESP_FAIL;
        }
        s_recreate_client = true;
        portEXIT_CRITICAL(&s_lock);
    } else if (event_id == MQTT_EVENT_DATA) {
        uint32_t uid;
        if (topic_is_heartbeat(event, &uid)) {
            fmo_station_note_uid(uid);
            return;
        }
        if (topic_is_qso_record(event)) {
            if (event->current_data_offset == 0) {
                s_qso_size = 0;
                s_qso_expected = event->total_data_len > 0 &&
                                 event->total_data_len <= QSO_RECORD_MAX_SIZE
                    ? (size_t)event->total_data_len : 0;
            }
            if (s_qso_expected == 0 || event->data_len <= 0 ||
                (size_t)event->current_data_offset != s_qso_size ||
                s_qso_size + (size_t)event->data_len > s_qso_expected) {
                s_qso_expected = 0;
                s_qso_size = 0;
                return;
            }
            memcpy(s_qso + s_qso_size, event->data, (size_t)event->data_len);
            s_qso_size += (size_t)event->data_len;
            if (s_qso_size == s_qso_expected) {
                fmo_qso_handle_mqtt_record((const char *)s_qso, s_qso_size);
                s_qso_expected = 0;
                s_qso_size = 0;
            }
            return;
        }
        if (!topic_is_raw(event)) return;
        if (event->current_data_offset == 0) {
            s_raw_size = 0;
            s_raw_expected = topic_is_raw(event) &&
                             event->total_data_len > 0 &&
                             event->total_data_len <= RAW_MAX_SIZE
                ? (size_t)event->total_data_len : 0;
        }
        if (s_raw_expected == 0 || event->data_len <= 0 ||
            (size_t)event->current_data_offset != s_raw_size ||
            s_raw_size + (size_t)event->data_len > s_raw_expected) {
            s_raw_expected = 0;
            s_raw_size = 0;
            return;
        }
        memcpy(s_raw + s_raw_size, event->data, (size_t)event->data_len);
        s_raw_size += (size_t)event->data_len;
        if (s_raw_size == s_raw_expected) {
            process_raw(s_raw, s_raw_size);
            s_raw_expected = 0;
            s_raw_size = 0;
        }
    } else if (event_id == MQTT_EVENT_ERROR && event->error_handle != NULL) {
        const int return_code = event->error_handle->connect_return_code;
        portENTER_CRITICAL(&s_lock);
        s_status.last_error = ESP_FAIL;
        if ((return_code == 4 || return_code == 5) &&
            strcmp(s_pending_role, "super") == 0 &&
            s_pending_key[0] != '\0') {
            /* SAS rejected "super" on our own server: remember it and let
             * the reconnect fall back to "user".  "admin" is deliberately
             * not tried -- it is unverified whether admin may broadcast, so
             * it is not treated as super (open point). */
            strlcpy(s_super_denied_key, s_pending_key,
                    sizeof(s_super_denied_key));
        }
        portEXIT_CRITICAL(&s_lock);
        ESP_LOGW(TAG, "MQTT error type=%d return=%d socket=%d",
                 event->error_handle->error_type,
                 event->error_handle->connect_return_code,
                 event->error_handle->esp_transport_sock_errno);
    }
}

static void stop_client(void)
{
    if (s_client_mutex != NULL) {
        xSemaphoreTake(s_client_mutex, portMAX_DELAY);
    }
    if (s_client != NULL) {
        esp_mqtt_client_stop(s_client);
        esp_mqtt_client_destroy(s_client);
        s_client = NULL;
    }
    if (s_client_mutex != NULL) xSemaphoreGive(s_client_mutex);
    portENTER_CRITICAL(&s_lock);
    s_status.connected = false;
    s_status.receiving = false;
    s_status.role[0] = '\0';
    s_recreate_client = false;
    portEXIT_CRITICAL(&s_lock);
}

static bool selected_server(fmo_server_t *server)
{
    char key[FMO_SERVER_KEY_MAX];
    portENTER_CRITICAL(&s_lock);
    snprintf(key, sizeof(key), "%s", s_requested_key);
    portEXIT_CRITICAL(&s_lock);
    if (key[0] == '\0') return false;
    size_t index = fmo_server_directory_find(key);
    const fmo_server_t *found =
        index == SIZE_MAX ? NULL : fmo_server_directory_get(index);
    if (found == NULL) return false;
    *server = *found;
    return true;
}

static esp_err_t start_client(const fmo_server_t *server)
{
    portENTER_CRITICAL(&s_lock);
    s_status.configured = true;
    s_status.connected = false;
    s_status.receiving = false;
    snprintf(s_status.server_name, sizeof(s_status.server_name), "%s",
             server->name);
    snprintf(s_status.server_callsign, sizeof(s_status.server_callsign), "%s",
             server->callsign);
    s_status.role[0] = '\0';
    portEXIT_CRITICAL(&s_lock);
    /* Own-server detection: the operator's own FMO server carries the same
     * callsign as the userCert.  Only there can the account hold the "super"
     * role (required for the FMO-V4 STATION broadcast), so try "super" first
     * and fall back to "user" once the SAS rejects it (remembered per server
     * key for this boot).  Compare base callsigns only — the server list may
     * carry an "-SSID" suffix while the certificate never does. */
    const char *role = "user";
    fmo_identity_status_t identity;
    if (fmo_cert_store_status(&identity) == ESP_OK && identity.ready &&
        fmo_aprs_base_callsign_eq(identity.callsign, server->callsign)) {
        portENTER_CRITICAL(&s_lock);
        const bool super_denied =
            strcmp(s_super_denied_key, server->key) == 0;
        portEXIT_CRITICAL(&s_lock);
        if (!super_denied) role = "super";
    }
    char username[16], client_id[48];
    char *password = NULL;
    esp_err_t error = fmo_cert_store_build_credentials(
        server, role, username, sizeof(username), &password);
    if (error != ESP_OK) {
        portENTER_CRITICAL(&s_lock);
        s_status.last_error = error;
        portEXIT_CRITICAL(&s_lock);
        return error;
    }
    snprintf(client_id, sizeof(client_id), "FMO-%s-%lu-%04X", username,
             (unsigned long)server->uid, (unsigned)s_client_suffix);
    portENTER_CRITICAL(&s_lock);
    snprintf(s_status.client_id, sizeof(s_status.client_id), "%s", client_id);
    snprintf(s_pending_role, sizeof(s_pending_role), "%s", role);
    snprintf(s_pending_key, sizeof(s_pending_key), "%s", server->key);
    portEXIT_CRITICAL(&s_lock);
    esp_mqtt_client_config_t mqtt_config = {
        .broker.address.hostname = server->host,
        .broker.address.port = server->port,
        .broker.address.transport = MQTT_TRANSPORT_OVER_TCP,
        .credentials.username = username,
        .credentials.client_id = client_id,
        .credentials.authentication.password = password,
        .session.keepalive = 60,
        .session.protocol_ver = MQTT_PROTOCOL_V_5,
        .network.reconnect_timeout_ms = 10000,
        .network.timeout_ms = 10000,
        .network.disable_auto_reconnect = true,
        .task.stack_size = 8192,
        .buffer.size = RAW_MAX_SIZE,
        .buffer.out_size = RAW_MAX_SIZE,
        .outbox.limit = 16U * 1024U,
    };
    xSemaphoreTake(s_client_mutex, portMAX_DELAY);
    s_client = esp_mqtt_client_init(&mqtt_config);
    sodium_memzero(password, strlen(password));
    free(password);
    if (s_client == NULL) {
        xSemaphoreGive(s_client_mutex);
        portENTER_CRITICAL(&s_lock);
        s_status.last_error = ESP_ERR_NO_MEM;
        portEXIT_CRITICAL(&s_lock);
        return ESP_ERR_NO_MEM;
    }
    error = esp_mqtt_client_register_event(s_client, ESP_EVENT_ANY_ID,
                                           mqtt_event, NULL);
    if (error == ESP_OK) error = esp_mqtt_client_start(s_client);
    if (error != ESP_OK) {
        esp_mqtt_client_destroy(s_client);
        s_client = NULL;
        xSemaphoreGive(s_client_mutex);
        portENTER_CRITICAL(&s_lock);
        s_status.last_error = error;
        portEXIT_CRITICAL(&s_lock);
        return error;
    }
    xSemaphoreGive(s_client_mutex);
    snprintf(s_active_key, sizeof(s_active_key), "%s", server->key);
    ESP_LOGI(TAG, "connecting %s (%s:%u, client id=%s, role=%s)", server->name,
             server->host, (unsigned)server->port, client_id, role);
    return ESP_OK;
}

static void control_task(void *argument)
{
    (void)argument;
    for (;;) {
        if (s_voice_until_us != 0 && esp_timer_get_time() > s_voice_until_us) {
            portENTER_CRITICAL(&s_lock);
            s_status.receiving = false;
            s_status.voice_callsign[0] = '\0';
            s_status.voice_codec[0] = '\0';
            s_voice_until_us = 0;
            portEXIT_CRITICAL(&s_lock);
        }
        network_status_t network;
        network_manager_get_status(&network);
        bool recreate;
        portENTER_CRITICAL(&s_lock);
        recreate = s_recreate_client;
        portEXIT_CRITICAL(&s_lock);
        if (recreate && s_client != NULL) {
            stop_client();
            s_active_key[0] = '\0';
        }
        fmo_server_t server;
        bool have_server = selected_server(&server);
        const char *wanted_key = have_server ? server.key : "";
        if (!network.station_connected || wanted_key[0] == '\0') {
            if (s_client != NULL) stop_client();
            s_active_key[0] = '\0';
            portENTER_CRITICAL(&s_lock);
            s_status.configured = s_requested_key[0] != '\0';
            if (network.station_connected && s_requested_key[0] != '\0') {
                s_status.last_error = ESP_ERR_NOT_FOUND;
            }
            portEXIT_CRITICAL(&s_lock);
        } else if (strcmp(wanted_key, s_active_key) != 0) {
            stop_client();
            s_active_key[0] = '\0';
            esp_err_t error = start_client(&server);
            if (error != ESP_OK && error != ESP_ERR_INVALID_STATE) {
                ESP_LOGW(TAG, "connect setup failed: %s", esp_err_to_name(error));
            }
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

esp_err_t fmo_link_start(const fmo_config_t *config)
{
    if (s_task != NULL) return ESP_OK;
    s_client_suffix = (uint16_t)esp_random();
    if (config != NULL) {
        snprintf(s_requested_key, sizeof(s_requested_key), "%s",
                 config->fmo_server_key);
        snprintf(s_configured_callsign, sizeof(s_configured_callsign), "%s",
                 config->fmo_callsign);
        s_no_local = config->fmo_mqtt_no_local;
    }
    s_raw = malloc(RAW_MAX_SIZE);
    if (s_raw == NULL) return ESP_ERR_NO_MEM;
    s_opus_decoder = opus_voice_decoder_open_ex(8000, 40);
    s_tx_encoder = opus_voice_encoder_open_ex(8000, 40, 12000);
    s_client_mutex = xSemaphoreCreateMutex();
    if (s_opus_decoder == NULL || s_tx_encoder == NULL ||
        s_client_mutex == NULL) {
        free(s_raw);
        s_raw = NULL;
        return ESP_ERR_NOT_SUPPORTED;
    }
    return xTaskCreate(control_task, "fmo_link", 8192, NULL, 4, &s_task) ==
        pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

void fmo_link_update_config(const fmo_config_t *config)
{
    if (config == NULL) return;
    portENTER_CRITICAL(&s_lock);
    if (strcmp(s_requested_key, config->fmo_server_key) != 0) {
        s_status.last_error = ESP_OK;
    }
    snprintf(s_requested_key, sizeof(s_requested_key), "%s",
             config->fmo_server_key);
    snprintf(s_configured_callsign, sizeof(s_configured_callsign), "%s",
             config->fmo_callsign);
    if (s_no_local != config->fmo_mqtt_no_local) {
        s_no_local = config->fmo_mqtt_no_local;
        s_recreate_client = true;
    }
    portEXIT_CRITICAL(&s_lock);
}

void fmo_link_get_status(fmo_link_status_t *status)
{
    if (status == NULL) return;
    portENTER_CRITICAL(&s_lock);
    *status = s_status;
    portEXIT_CRITICAL(&s_lock);
}

static bool tx_flush(void)
{
    if (s_tx_packet_count == 0) return true;
    const uint8_t *packets[TX_PACKETS_PER_FRAME];
    for (size_t i = 0; i < s_tx_packet_count; ++i) {
        packets[i] = s_tx_packets[i];
    }
    uint8_t frame[RAW_MAX_SIZE];
    size_t frame_size = fmo_frame_build_opus(
        frame, sizeof(frame), s_tx_callsign, s_tx_session, s_tx_started_ms,
        s_tx_started_ms + s_tx_packet_total * 40U, packets,
        s_tx_packet_sizes, s_tx_packet_count,
        s_tx_frame_count < 3 ? 0 : 9);
    if (frame_size == 0) return false;
    bool published = false;
    bool connected;
    portENTER_CRITICAL(&s_lock);
    connected = s_status.connected;
    portEXIT_CRITICAL(&s_lock);
    xSemaphoreTake(s_client_mutex, portMAX_DELAY);
    if (s_client != NULL && connected) {
        published = esp_mqtt_client_publish(
            s_client, "FMO/RAW", (const char *)frame, (int)frame_size,
            0, 0) >= 0;
    }
    xSemaphoreGive(s_client_mutex);
    s_tx_packet_total += (uint32_t)s_tx_packet_count;
    ++s_tx_frame_count;
    s_tx_packet_count = 0;
    return published;
}

static bool tx_encode_packet(void)
{
    if (s_tx_packet_count >= TX_PACKETS_PER_FRAME) return false;
    int encoded = opus_voice_encode(
        s_tx_encoder, s_tx_pcm, sizeof(s_tx_pcm) / sizeof(s_tx_pcm[0]),
        s_tx_packets[s_tx_packet_count],
        sizeof(s_tx_packets[s_tx_packet_count]));
    s_tx_pcm_count = 0;
    if (encoded <= 0) return false;
    s_tx_packet_sizes[s_tx_packet_count++] = (size_t)encoded;
    return s_tx_packet_count < TX_PACKETS_PER_FRAME || tx_flush();
}

bool fmo_link_tx_begin(void)
{
    fmo_identity_status_t identity;
    fmo_link_status_t link;
    char configured_callsign[sizeof(s_configured_callsign)];
    fmo_link_get_status(&link);
    portENTER_CRITICAL(&s_lock);
    strlcpy(configured_callsign, s_configured_callsign,
            sizeof(configured_callsign));
    portEXIT_CRITICAL(&s_lock);
    if (!link.connected ||
        fmo_cert_store_status(&identity) != ESP_OK || !identity.ready) {
        return false;
    }
    if (configured_callsign[0] == '\0' ||
        strcasecmp(configured_callsign, identity.callsign) != 0) {
        ESP_LOGE(TAG, "FMO callsign %s does not match userCert %s",
                 configured_callsign[0] != '\0' ? configured_callsign : "(empty)",
                 identity.callsign);
        return false;
    }
    s_tx_active = true;
    s_tx_pcm_count = 0;
    s_tx_packet_count = 0;
    s_tx_packet_total = 0;
    s_tx_frame_count = 0;
    s_tx_session = (uint16_t)esp_random();
    if (s_tx_session == 0) s_tx_session = 1;
    uint64_t epoch_ms = (uint64_t)time(NULL) * 1000ULL +
                        (uint64_t)(esp_timer_get_time() / 1000) % 1000ULL;
    s_tx_started_ms = (uint32_t)epoch_ms;
    snprintf(s_tx_callsign, sizeof(s_tx_callsign), "%s", configured_callsign);
    return true;
}

bool fmo_link_tx_feed_pcm16(const int16_t *samples, size_t sample_count)
{
    if (!s_tx_active || samples == NULL) return false;
    size_t offset = 0;
    while (offset + 1 < sample_count) {
        int32_t sum = (int32_t)samples[offset] + samples[offset + 1];
        s_tx_pcm[s_tx_pcm_count++] = (int16_t)(sum / 2);
        offset += 2;
        if (s_tx_pcm_count == sizeof(s_tx_pcm) / sizeof(s_tx_pcm[0]) &&
            !tx_encode_packet()) {
            return false;
        }
    }
    return true;
}

void fmo_link_tx_end(void)
{
    if (!s_tx_active) return;
    if (s_tx_pcm_count > 0) {
        int16_t last = s_tx_pcm[s_tx_pcm_count - 1];
        while (s_tx_pcm_count < sizeof(s_tx_pcm) / sizeof(s_tx_pcm[0])) {
            s_tx_pcm[s_tx_pcm_count++] = last;
        }
        (void)tx_encode_packet();
    }
    (void)tx_flush();
    s_tx_active = false;
}

bool fmo_link_get_selected_server(fmo_server_t *server)
{
    if (server == NULL) return false;
    return selected_server(server);
}

bool fmo_link_connected_to(const char *key)
{
    if (key == NULL || key[0] == '\0') return false;
    portENTER_CRITICAL(&s_lock);
    const bool connected = s_status.connected &&
                           strcmp(s_active_key, key) == 0;
    portEXIT_CRITICAL(&s_lock);
    return connected;
}

bool fmo_link_jump_to_key(const char *key)
{
    if (key == NULL || fmo_server_directory_find(key) == SIZE_MAX) {
        return false;
    }
    portENTER_CRITICAL(&s_lock);
    if (strcmp(s_requested_key, key) != 0) {
        s_status.last_error = ESP_OK;
    }
    snprintf(s_requested_key, sizeof(s_requested_key), "%s", key);
    portEXIT_CRITICAL(&s_lock);
    ESP_LOGI(TAG, "QSO jump: switching to server %s (runtime only)", key);
    return true;
}

bool fmo_link_publish(const char *topic, const char *payload, int length)
{
    if (topic == NULL || payload == NULL || length <= 0 ||
        s_client_mutex == NULL) return false;
    bool connected;
    portENTER_CRITICAL(&s_lock);
    connected = s_status.connected;
    portEXIT_CRITICAL(&s_lock);
    if (!connected) return false;
    xSemaphoreTake(s_client_mutex, portMAX_DELAY);
    const bool published = s_client != NULL &&
        esp_mqtt_client_publish(s_client, topic, payload, length, 0, 0) >= 0;
    xSemaphoreGive(s_client_mutex);
    return published;
}
