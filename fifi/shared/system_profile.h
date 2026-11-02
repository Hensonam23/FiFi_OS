#ifndef FIFI_SHARED_SYSTEM_PROFILE_H
#define FIFI_SHARED_SYSTEM_PROFILE_H

#include <stdbool.h>

typedef enum {
    FIFI_PROFILE_GENERAL = 0,
    FIFI_PROFILE_BUILD,
    FIFI_PROFILE_RESEARCH,
    FIFI_PROFILE_PLAY,
    FIFI_PROFILE_COUNT
} fifi_system_profile_t;

typedef struct {
    const char *name;
    const char *storage_name;
    const char *governor;
    const char *governor_fallback;
    const char *energy_preference;
    const char *gpu_preference;
    const char *transparent_hugepages;
    int swappiness;
    long frame_interval_ns;
    int event_poll_ms;
    bool inhibit_display_blanking;
} fifi_system_profile_policy_t;

static inline bool fifi_system_profile_valid(int profile) {
    return profile >= FIFI_PROFILE_GENERAL && profile < FIFI_PROFILE_COUNT;
}

static inline fifi_system_profile_policy_t
fifi_system_profile_policy(fifi_system_profile_t profile) {
    static const fifi_system_profile_policy_t policies[FIFI_PROFILE_COUNT] = {
        [FIFI_PROFILE_GENERAL] = {
            "General", "general", "schedutil", "powersave",
            "balance_performance", "auto", "madvise", 30,
            16666666L, 4, false
        },
        [FIFI_PROFILE_BUILD] = {
            "Build", "build", "performance", "performance",
            "performance", "auto", "madvise", 10,
            16666666L, 4, false
        },
        [FIFI_PROFILE_RESEARCH] = {
            "Research", "research", "performance", "performance",
            "performance", "high", "always", 5,
            16666666L, 4, false
        },
        [FIFI_PROFILE_PLAY] = {
            "Play", "play", "performance", "performance",
            "performance", "high", "madvise", 10,
            4000000L, 2, true
        }
    };
    if (!fifi_system_profile_valid((int)profile)) profile = FIFI_PROFILE_GENERAL;
    return policies[profile];
}

static inline fifi_system_profile_t
fifi_system_profile_parse(const char *name) {
    if (!name) return FIFI_PROFILE_GENERAL;
    for (int i = 0; i < FIFI_PROFILE_COUNT; i++) {
        const char *a = name;
        const char *b = fifi_system_profile_policy((fifi_system_profile_t)i).storage_name;
        while (*a && *b && *a == *b) { a++; b++; }
        if (!*a && !*b) return (fifi_system_profile_t)i;
    }
    return FIFI_PROFILE_GENERAL;
}

#endif
