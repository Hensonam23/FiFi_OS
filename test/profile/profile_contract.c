#include <assert.h>
#include <string.h>
#include "../../fifi/shared/system_profile.h"

int main(void) {
    assert(FIFI_PROFILE_COUNT == 4);
    assert(fifi_system_profile_parse("general") == FIFI_PROFILE_GENERAL);
    assert(fifi_system_profile_parse("build") == FIFI_PROFILE_BUILD);
    assert(fifi_system_profile_parse("research") == FIFI_PROFILE_RESEARCH);
    assert(fifi_system_profile_parse("play") == FIFI_PROFILE_PLAY);
    assert(fifi_system_profile_parse("unknown") == FIFI_PROFILE_GENERAL);

    fifi_system_profile_policy_t general =
        fifi_system_profile_policy(FIFI_PROFILE_GENERAL);
    assert(strcmp(general.governor, "schedutil") == 0);
    assert(strcmp(general.governor_fallback, "powersave") == 0);
    assert(strcmp(general.gpu_preference, "auto") == 0);
    assert(general.frame_interval_ns == 16666666L);
    assert(general.event_poll_ms == 4);
    assert(!general.inhibit_display_blanking);

    fifi_system_profile_policy_t build =
        fifi_system_profile_policy(FIFI_PROFILE_BUILD);
    assert(strcmp(build.governor, "performance") == 0);
    assert(strcmp(build.gpu_preference, "auto") == 0);
    assert(build.swappiness == 10);

    fifi_system_profile_policy_t research =
        fifi_system_profile_policy(FIFI_PROFILE_RESEARCH);
    assert(strcmp(research.gpu_preference, "high") == 0);
    assert(strcmp(research.transparent_hugepages, "always") == 0);
    assert(research.swappiness == 5);

    fifi_system_profile_policy_t play =
        fifi_system_profile_policy(FIFI_PROFILE_PLAY);
    assert(strcmp(play.governor, "performance") == 0);
    assert(strcmp(play.gpu_preference, "high") == 0);
    assert(play.frame_interval_ns == 4000000L);
    assert(play.event_poll_ms == 2);
    assert(play.inhibit_display_blanking);
    return 0;
}
