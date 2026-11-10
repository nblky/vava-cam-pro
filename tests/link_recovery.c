#include "link_recovery.h"
#undef NDEBUG
#include <assert.h>
int main(void) {
    struct link_recovery r={0};
    assert(!link_recovery_tick(&r,1000,true,true,false,false));
    assert(!link_recovery_tick(&r,30000,true,true,false,false));
    /* Viewing, recording or configuration must restart the idle grace. */
    assert(!link_recovery_tick(&r,31000,true,true,false,true));
    assert(!link_recovery_tick(&r,40000,true,true,false,false));
    assert(link_recovery_tick(&r,70000,true,true,false,false));
    assert(link_recovery_blocked(&r,104999));assert(!link_recovery_blocked(&r,105000));
    assert(!link_recovery_tick(&r,105000,true,true,false,false));
    assert(!link_recovery_tick(&r,369999,true,true,false,false));
    assert(link_recovery_tick(&r,370000,true,true,false,false));
    /* A newly established keepalive cancels the outage immediately. */
    assert(!link_recovery_tick(&r,371000,true,true,true,false));
    assert(!link_recovery_blocked(&r,371000));assert(r.attempts==0);
    assert(!link_recovery_tick(&r,400000,false,true,false,false));
    assert(!link_recovery_tick(&r,450000,true,false,false,false));
    r=(struct link_recovery){0};
    for(unsigned i=0;i<3;++i){uint64_t at=1000+i*400000;
        assert(!link_recovery_tick(&r,at,true,true,false,false));
        assert(link_recovery_tick(&r,at+30000,true,true,false,false));}
    assert(!link_recovery_tick(&r,1300000,true,true,false,false));
    assert(!link_recovery_tick(&r,1500000,true,true,false,false));
    assert(r.attempts==3);
    return 0;
}
