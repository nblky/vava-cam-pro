#ifndef VAVA_LINK_RECOVERY_H
#define VAVA_LINK_RECOVERY_H
#include <stdbool.h>
#include <stdint.h>

/* Stock camera 0x4001e6f4 holds an idle keepalive fd across a station restart.
 * Without a usable keepalive, continuing heartbeat replies feeds its watchdog
 * forever. After idle grace, permit one bounded normal link-loss recovery.
 * No persistent camera setting, factory reset or unverified command is sent. */
struct link_recovery {
    uint64_t missing_since, quiet_until, retry_after;
    unsigned attempts;
};
static inline bool link_recovery_blocked(const struct link_recovery *r,uint64_t now) {
    return r->quiet_until && now<r->quiet_until;
}
static inline bool link_recovery_tick(struct link_recovery *r,uint64_t now,
        bool eligible,bool control,bool alive,bool busy) {
    if(alive){r->missing_since=r->quiet_until=0;r->attempts=0;return false;}
    if(link_recovery_blocked(r,now))return false;
    r->quiet_until=0;
    if(!eligible||!control||busy){r->missing_since=0;return false;}
    if(!r->missing_since){r->missing_since=now;return false;}
    if(now-r->missing_since<30000||now<r->retry_after||r->attempts>=3)return false;
    r->missing_since=0;r->quiet_until=now+35000;r->retry_after=now+300000;++r->attempts;
    return true;
}
#endif
