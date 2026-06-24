#include "store.h"
#include <Arduino.h>
#include <string.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

namespace store {

static Env             s_envs[MAX_ENVS];
static SemaphoreHandle_t s_mu = nullptr;

static void lock()   { xSemaphoreTake(s_mu, portMAX_DELAY); }
static void unlock() { xSemaphoreGive(s_mu); }

void init() {
    s_mu = xSemaphoreCreateMutex();
    memset(s_envs, 0, sizeof(s_envs));
}

static Env* find_or_create(const char* label) {
    Env* free_slot = nullptr;
    for (int i = 0; i < MAX_ENVS; i++) {
        if (s_envs[i].active && strncmp(s_envs[i].label, label,
                                       sizeof(s_envs[i].label)) == 0) {
            return &s_envs[i];
        }
        if (!s_envs[i].active && !free_slot) free_slot = &s_envs[i];
    }
    if (!free_slot) return nullptr;
    memset(free_slot, 0, sizeof(*free_slot));
    free_slot->active = true;
    strncpy(free_slot->label, label, sizeof(free_slot->label) - 1);
    free_slot->history_bucket_start_ms = millis();
    return free_slot;
}

static ModelRollup* model_slot(Env* e, const char* model) {
    ModelRollup* free_slot = nullptr;
    for (int i = 0; i < MAX_MODELS_PER_ENV; i++) {
        if (e->models[i].name[0] &&
            strncmp(e->models[i].name, model, sizeof(e->models[i].name)) == 0) {
            return &e->models[i];
        }
        if (!e->models[i].name[0] && !free_slot) free_slot = &e->models[i];
    }
    if (!free_slot) return nullptr;
    memset(free_slot, 0, sizeof(*free_slot));
    strncpy(free_slot->name, model, sizeof(free_slot->name) - 1);
    return free_slot;
}

static void rotate_history_if_needed(Env* e, uint32_t now_ms) {
    uint32_t elapsed = now_ms - e->history_bucket_start_ms;
    while (elapsed >= BUCKET_MS) {
        e->history_head = (e->history_head + 1) % HISTORY_BUCKETS;
        e->history[e->history_head] = 0;
        e->history_bucket_start_ms += BUCKET_MS;
        elapsed -= BUCKET_MS;
    }
}

void ingest(const char* env_label, const char* model,
            uint64_t input, uint64_t output,
            uint64_t cache_create, uint64_t cache_read) {
    if (!env_label || !model) return;
    lock();
    Env* e = find_or_create(env_label);
    if (e) {
        uint32_t now = millis();
        rotate_history_if_needed(e, now);
        e->total_input        += input;
        e->total_output       += output;
        e->total_cache_create += cache_create;
        e->total_cache_read   += cache_read;
        e->msg_count++;
        e->last_sample_ms = now;
        e->history[e->history_head] += (input + output + cache_create + cache_read);

        ModelRollup* m = model_slot(e, model);
        if (m) {
            m->input        += input;
            m->output       += output;
            m->cache_create += cache_create;
            m->cache_read   += cache_read;
        }
    }
    unlock();
}

void snapshot(Env* out) {
    lock();
    // Rotate every env first so the snapshot reflects current bucket alignment.
    uint32_t now = millis();
    for (int i = 0; i < MAX_ENVS; i++) {
        if (s_envs[i].active) rotate_history_if_needed(&s_envs[i], now);
    }
    memcpy(out, s_envs, sizeof(s_envs));
    unlock();
}

uint64_t grand_total_tokens() {
    uint64_t t = 0;
    lock();
    for (int i = 0; i < MAX_ENVS; i++) {
        if (s_envs[i].active) {
            t += s_envs[i].total_input + s_envs[i].total_output
               + s_envs[i].total_cache_create + s_envs[i].total_cache_read;
        }
    }
    unlock();
    return t;
}

int active_env_count() {
    int n = 0;
    lock();
    for (int i = 0; i < MAX_ENVS; i++) if (s_envs[i].active) n++;
    unlock();
    return n;
}

}  // namespace store
