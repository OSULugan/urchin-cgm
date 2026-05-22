#include "xdrip_comm.h"
#include <pebble.h>
#include <string.h>
#include <stdlib.h>

// ---- Buffer sizes ----
// 60 compact SGV records × ~53 bytes each ≈ 3180 bytes
#define SGV_BUF_SIZE        3200
// 20 compact treatment records × ~60 bytes each ≈ 1200 bytes
#define TREATMENTS_BUF_SIZE 1536
// Maximum SGV records to parse (matches GRAPH_MAX_SGV_COUNT)
#define MAX_PARSE_COUNT     144
// 5-minute interval in seconds
#define INTERVAL_S          300
// 2.5-minute match window in seconds
#define SLOT_WINDOW_S       150

// ---- Parsed SGV record ----
typedef struct {
    int32_t date_s;
    int32_t sgv;
    int32_t trend;
} SgvRecord;

// ---- Per-file reassembly state ----
typedef struct {
    char buf[SGV_BUF_SIZE + 1];
    uint16_t len;
    uint16_t expected_chunks;
    bool complete;
} SgvFileState;

typedef struct {
    char buf[TREATMENTS_BUF_SIZE + 1];
    uint16_t len;
    uint16_t expected_chunks;
    bool complete;
} TreatmentsFileState;

static SgvFileState s_sgv;
static TreatmentsFileState s_treatments;
static DataMessage s_data_msg;
static SgvRecord s_records[MAX_PARSE_COUNT];
static int32_t s_bolus_ts[24];

// ---- Direction string → trend number ----
// Matches NIGHTSCOUT_DIRECTION_TO_TREND in constants.json
static int32_t direction_to_trend(const char *dir) {
    if (strcmp(dir, "DoubleUp") == 0)      return 1;
    if (strcmp(dir, "SingleUp") == 0)      return 2;
    if (strcmp(dir, "FortyFiveUp") == 0)   return 3;
    if (strcmp(dir, "Flat") == 0)          return 4;
    if (strcmp(dir, "FortyFiveDown") == 0) return 5;
    if (strcmp(dir, "SingleDown") == 0)    return 6;
    if (strcmp(dir, "DoubleDown") == 0)    return 7;
    return 0;
}

// ---- Integer parsing helpers ----

// Parse a decimal integer starting at p. Sets *end_out to first non-digit char.
static int32_t parse_int32(const char *p, const char **end_out) {
    int32_t result = 0;
    bool neg = false;
    if (*p == '-') { neg = true; p++; }
    while (*p >= '0' && *p <= '9') {
        result = result * 10 + (*p - '0');
        p++;
    }
    if (end_out) *end_out = p;
    return neg ? -result : result;
}

// Parse a millisecond epoch timestamp; return seconds by reading only the first
// 10 digits (seconds part of a 13-digit ms value). Works for both ms and s.
static int32_t parse_epoch_as_seconds(const char *p) {
    int32_t result = 0;
    int count = 0;
    while (*p >= '0' && *p <= '9' && count < 10) {
        result = result * 10 + (*p - '0');
        p++;
        count++;
    }
    return result;
}

// ---- SGV JSON parser ----
// Extracts records from a compact JSON array:
// [{"sgv":<int>,"date":<ms>,"direction":"<str>"},...]
// Returns the number of records placed in out[].
static uint16_t parse_sgv_json(const char *json, SgvRecord *out, uint16_t max) {
    uint16_t count = 0;
    const char *p = json;

    while (count < max) {
        // Advance to the next object start
        const char *obj = strchr(p, '{');
        if (!obj) break;
        const char *obj_end = strchr(obj, '}');
        if (!obj_end) break;

        SgvRecord rec = {0};

        // Parse "sgv": <integer>
        const char *f = strstr(obj, "\"sgv\":");
        if (f && f < obj_end) {
            f += 6;
            while (*f == ' ') f++;
            rec.sgv = parse_int32(f, NULL);
        }

        // Parse "date": <integer> (ms epoch → keep first 10 digits as seconds)
        f = strstr(obj, "\"date\":");
        if (f && f < obj_end) {
            f += 7;
            while (*f == ' ') f++;
            rec.date_s = parse_epoch_as_seconds(f);
        }

        // Parse "direction": "<string>"
        f = strstr(obj, "\"direction\":\"");
        if (f && f < obj_end) {
            f += 13;
            char dir[32];
            int i = 0;
            while (*f && *f != '"' && i < (int)sizeof(dir) - 1) {
                dir[i++] = *f++;
            }
            dir[i] = '\0';
            rec.trend = direction_to_trend(dir);
        }

        if (rec.sgv > 12 && rec.date_s > 0) { // skip Dexcom error codes (<= 12)
            out[count++] = rec;
        }

        p = obj_end + 1;
    }
    return count;
}

// ---- Treatments JSON parser (bolus tick support) ----
// Expects a compact format: [{"date":<epoch_ms>},...] (only bolus entries).
// Returns the number of timestamps placed in out_s[] (as epoch seconds).
static uint16_t parse_bolus_timestamps(const char *json, int32_t *out_s, uint16_t max) {
    uint16_t count = 0;
    const char *p = json;

    while (count < max) {
        const char *obj = strchr(p, '{');
        if (!obj) break;
        const char *obj_end = strchr(obj, '}');
        if (!obj_end) break;

        const char *f = strstr(obj, "\"date\":");
        if (f && f < obj_end) {
            f += 7;
            while (*f == ' ') f++;
            int32_t ts = parse_epoch_as_seconds(f);
            if (ts > 0) out_s[count++] = ts;
        }

        p = obj_end + 1;
    }
    return count;
}

// ---- DataMessage assembly ----
static void build_data_message(const SgvRecord *records, uint16_t count,
                               const int32_t *bolus_ts, uint16_t bolus_count) {
    DataMessage *out = &s_data_msg;
    memset(out, 0, sizeof(DataMessage));
    out->received_at = time(NULL);

    if (count == 0) return;

    // Records are date-descending (newest first from iOS).
    int32_t end_s = records[0].date_s;

    // Build time-slot-aligned SGV array.
    // sgvs[0] = most recent reading, sgvs[1] = 5 min ago, etc.
    // A slot with no nearby reading stays 0 (= no data point on graph).
    uint16_t sgv_count = 0;
    for (uint16_t i = 0; i < GRAPH_MAX_SGV_COUNT; i++) {
        int32_t slot_s = end_s - (int32_t)i * INTERVAL_S;

        int32_t best_sgv = 0;
        int32_t best_diff = SLOT_WINDOW_S + 1;

        for (uint16_t j = 0; j < count; j++) {
            int32_t diff = records[j].date_s - slot_s;
            if (diff < 0) diff = -diff;
            if (diff < best_diff) {
                best_diff = diff;
                best_sgv = records[j].sgv;
            }
            // Records are sorted newest-first; once we're past the slot window, stop.
            if (records[j].date_s < slot_s - SLOT_WINDOW_S) break;
        }

        int32_t halved = best_sgv > 0 ? (best_sgv / 2) : 0;
        out->sgvs[i] = (uint8_t)(halved > 255 ? 255 : halved);
        if (best_sgv > 0) sgv_count = i + 1;
    }

    out->sgv_count = sgv_count;
    out->last_sgv = records[0].sgv;
    out->trend = records[0].trend;

    // Delta = (most recent slot BG) - (previous slot BG) in full mg/dL
    int32_t y0 = (int32_t)out->sgvs[0] * 2;
    int32_t y1 = (int32_t)out->sgvs[1] * 2;
    out->delta = (y0 > 0 && y1 > 0) ? (y0 - y1) : NO_DELTA_VALUE;

    // Recency in seconds
    out->recency = (int32_t)time(NULL) - records[0].date_s;
    if (out->recency < 0) out->recency = 0;

    out->status_text[0] = '\0';
    out->status_recency = -1;
    out->prediction_length = 0;
    out->prediction_recency = 0;

    // Bolus ticks in graph_extra
    if (bolus_count > 0) {
        for (uint16_t i = 0; i < GRAPH_MAX_SGV_COUNT; i++) {
            int32_t slot_s = end_s - (int32_t)i * INTERVAL_S;
            for (uint16_t k = 0; k < bolus_count; k++) {
                int32_t diff = bolus_ts[k] - slot_s;
                if (diff < 0) diff = -diff;
                if (diff <= SLOT_WINDOW_S) {
                    out->graph_extra[i].bolus = 1;
                    break;
                }
            }
        }
    }
}

// ---- File state helpers ----
static void reset_sgv(void) {
    s_sgv.len = 0;
    s_sgv.expected_chunks = 0;
    s_sgv.complete = false;
}

static void reset_treatments(void) {
    s_treatments.len = 0;
    s_treatments.expected_chunks = 0;
    s_treatments.complete = false;
}

// ---- Dispatch ----
static bool try_dispatch(void (*data_callback)(DataMessage *data)) {
    if (!s_sgv.complete || !s_treatments.complete) return false;

    uint16_t rec_count = parse_sgv_json(s_sgv.buf, s_records, MAX_PARSE_COUNT);
    uint16_t bolus_count = parse_bolus_timestamps(s_treatments.buf, s_bolus_ts, 24);

    if (rec_count > 0) {
        build_data_message(s_records, rec_count, s_bolus_ts, bolus_count);
        save_last_data_message(&s_data_msg);
        data_callback(&s_data_msg);
    }

    reset_sgv();
    reset_treatments();
    return rec_count > 0;
}

// ---- Public API ----
bool xdrip_handle_chunk(
    uint8_t file_type,
    uint16_t total_chunks,
    uint16_t chunk_index,
    const uint8_t *chunk_data,
    uint16_t chunk_len,
    void (*data_callback)(DataMessage *data)
) {
    bool is_sgv = (file_type == XDRIP_FILE_TYPE_SGV);

    char *buf         = is_sgv ? s_sgv.buf         : s_treatments.buf;
    uint16_t buf_max  = is_sgv ? SGV_BUF_SIZE       : TREATMENTS_BUF_SIZE;
    uint16_t *buf_len = is_sgv ? &s_sgv.len         : &s_treatments.len;
    bool *complete    = is_sgv ? &s_sgv.complete    : &s_treatments.complete;

    // If a new SGV transfer begins while we are still awaiting treatments from
    // the previous cycle, dispatch what we have so the display updates, then
    // start fresh.
    if (is_sgv && chunk_index == 0 && s_sgv.complete && !s_treatments.complete) {
        uint16_t n = parse_sgv_json(s_sgv.buf, s_records, MAX_PARSE_COUNT);
        if (n > 0) {
            build_data_message(s_records, n, NULL, 0);
            save_last_data_message(&s_data_msg);
            data_callback(&s_data_msg);
        }
        reset_sgv();
        reset_treatments();
    }

    // Reset this file's state on the first chunk of a new transfer.
    if (chunk_index == 0) {
        *buf_len = 0;
        *complete = false;
        if (is_sgv) s_sgv.expected_chunks = total_chunks;
        else        s_treatments.expected_chunks = total_chunks;
    }

    // Append chunk data, truncating if the buffer is full.
    uint16_t space = buf_max - *buf_len;
    uint16_t to_copy = chunk_len < space ? chunk_len : space;
    if (to_copy > 0) {
        memcpy(buf + *buf_len, chunk_data, to_copy);
        *buf_len += to_copy;
    }

    // Check if all chunks for this file have been received.
    if (chunk_index + 1 == total_chunks) {
        buf[*buf_len] = '\0'; // null-terminate for string scanning
        *complete = true;
        return try_dispatch(data_callback);
    }

    return false;
}
