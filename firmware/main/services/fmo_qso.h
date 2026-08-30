#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "services/config_store.h"

/* FMO QSO call signaling engine (APRS APFMO0 messages).
 *
 * Wire format (reverse-engineered from the stock firmware):
 *   FROM>APFMO0,TCPIP*::<TOCALL padded to 9>:<payload>{<msgId>
 * Replies (QTHANS/CALLANS) echo the incoming msgId; new conversations use an
 * incrementing local sequence.  All text on the wire is UTF-8.
 *
 * Call flow: caller QTHQRY -> callee auto QTHANS -> caller jumps to the
 * callee's server (MQTT switch) -> caller CALL -> callee rings -> ACCEPT.
 * The callee never jumps.  Timeouts: QTHANS 10 s (retry every 3 s), RING 7 s,
 * ACCEPT 60 s, incoming ring 60 s (ends silently, no TIMEOUT is sent). */

typedef enum {
    FMO_QSO_PHASE_IDLE = 0,
    FMO_QSO_PHASE_QUERYING,    /* QTHQRY sent, waiting for QTHANS */
    FMO_QSO_PHASE_JUMPING,     /* switching MQTT to the callee's server */
    FMO_QSO_PHASE_CALLING,     /* CALL sent, waiting for RING */
    FMO_QSO_PHASE_RINGING,     /* peer is ringing, waiting for ACCEPT */
    FMO_QSO_PHASE_INCOMING,    /* we are being called (knob accept/reject) */
    FMO_QSO_PHASE_ESTABLISHED,
    FMO_QSO_PHASE_FAILED,      /* terminal detail shown, auto-idle after 5 s */
} fmo_qso_phase_t;

typedef struct {
    fmo_qso_phase_t phase;
    bool outgoing;         /* established/querying/calling as the caller */
    bool incoming;         /* phase == FMO_QSO_PHASE_INCOMING */
    char peer[16];         /* peer callsign (with SSID as received) */
    uint32_t peer_uid;
    char detail[96];       /* human-readable state/failure text (UTF-8) */
} fmo_qso_status_t;

#define FMO_QSO_LOG_COMMENT_MAX 97

typedef struct {
    int64_t ts;
    char dir[4];   /* "in" / "out" */
    char peer[16];
    uint32_t peer_uid;
    char result[40];
    char comment[FMO_QSO_LOG_COMMENT_MAX]; /* QSO greeting, may be empty */
    char grid[8];
    char relay[64];
} fmo_qso_log_entry_t;

esp_err_t fmo_qso_start(const fmo_config_t *config);
void fmo_qso_update_config(const fmo_config_t *config);

void fmo_qso_get_status(fmo_qso_status_t *status);
/* Newest first.  Returns the number of entries copied. */
size_t fmo_qso_get_log(fmo_qso_log_entry_t *out, size_t capacity);

/* Start an outgoing call.  `peer_uid` is required (the device has no
 * callsign->uid table); on failure returns false and fills `error`. */
bool fmo_qso_call(const char *peer, uint32_t peer_uid, char *error,
                  size_t error_size);
/* Knob handlers for an incoming call. */
void fmo_qso_answer(bool accept);
/* Cancel an outgoing call / reject an incoming one / end an established QSO. */
void fmo_qso_cancel(void);

/* Entry points wired into aprs_service (APFMO0 message bodies) and fmo_link
 * (FMO/QSO/UID/<own uid> record payloads). */
void fmo_qso_handle_aprs_message(const char *from, const char *to,
                                 const char *payload, const char *msg_id);
void fmo_qso_handle_mqtt_record(const char *json, size_t size);

/* WGS-84 microdegrees -> 6-character Maidenhead locator (OM89ev). */
void fmo_qso_maidenhead(int32_t lat_e6, int32_t lon_e6, char out[7]);
