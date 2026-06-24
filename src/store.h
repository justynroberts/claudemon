#pragma once
#include <stdint.h>
#include <stddef.h>

namespace store {

constexpr int  MAX_ENVS         = 12;
constexpr int  HISTORY_BUCKETS  = 60;   // rolling minutes
constexpr int  BUCKET_MS        = 60000;
constexpr int  MAX_MODELS_PER_ENV = 6;

struct ModelRollup {
    char     name[32];
    uint64_t input;
    uint64_t output;
    uint64_t cache_create;
    uint64_t cache_read;
};

struct Env {
    char        label[32];
    bool        active;
    uint32_t    last_sample_ms;   // millis() at last ingest
    uint64_t    total_input;
    uint64_t    total_output;
    uint64_t    total_cache_create;
    uint64_t    total_cache_read;
    uint32_t    msg_count;
    ModelRollup models[MAX_MODELS_PER_ENV];
    // Rolling minute buckets of total tokens (input+output+cache).
    uint32_t    history[HISTORY_BUCKETS];
    uint32_t    history_head;     // index of newest bucket
    uint32_t    history_bucket_start_ms;
};

void init();

// Called from HTTP thread. Looks up or creates the env, adds the sample,
// rotates history buckets as needed.
void ingest(const char* env_label,
            const char* model,
            uint64_t input,
            uint64_t output,
            uint64_t cache_create,
            uint64_t cache_read);

// Snapshot-read for the UI thread. Pass a pointer to MAX_ENVS Env structs;
// fills active ones, sets active=false on the rest.
void snapshot(Env* out);

// Total tokens summed across all active envs.
uint64_t grand_total_tokens();
int      active_env_count();

}  // namespace store
