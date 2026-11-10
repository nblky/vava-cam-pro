#ifndef VAVA_POLICY_H
#define VAVA_POLICY_H
#include "registry.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

#define POLICY_PLANS 10
struct plan { unsigned days, start, end; bool enabled, all_day; };
enum arm_mode { ARM_OFF, ARM_ON, ARM_SCHEDULE };
struct policy {
    bool managed, enabled, cloud_upload, nas_backup, camera_duration, confirm_detection;
    enum arm_mode mode;
    unsigned level, record_seconds, idle_seconds;
    int utc_offset_minutes;
    uint32_t revision;
    struct plan plans[POLICY_PLANS];
};
void policy_default(struct policy *p);
void policy_daily_default(struct policy *p);
bool policy_time(const char *s, unsigned *seconds);
bool policy_days(const char *s, unsigned *mask);
bool policy_evaluate(const struct policy *p, time_t utc, bool clock_valid);
bool policy_valid(const struct policy *p);
int policy_load(const char *dir, const struct registration r[4], struct policy p[4]);
int policy_load_mode(const char *dir,const struct registration r[4],struct policy p[4],bool daily);
int policy_save(const char *dir, const struct registration r[4], const struct policy p[4]);
bool policy_json(const struct policy *p, bool armed, bool clock_valid, const char *delivery,
                 unsigned attempts, char *out, size_t size);
#endif
