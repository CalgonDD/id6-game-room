#include "../src/ps1_runtime.h"
#include "../src/ps1_controls.h"
#include <assert.h>
#include <stdio.h>
#include <limits.h>
static ps1_audio_queue q=PS1_AUDIO_INIT;
static void put(unsigned n) {
    unsigned i; pthread_mutex_lock(&q.lock);
    for(i=0;i<n;++i) ps1_audio_put_locked(&q,(int16_t)i,(int16_t)~i);
    pthread_mutex_unlock(&q.lock);
}
static void *produce(void *unused) {
    unsigned i; (void)unused;
    for(i=0;i<200000;++i) {
        pthread_mutex_lock(&q.lock);
        ps1_audio_put_locked(&q,(int16_t)i,(int16_t)~i);
        pthread_mutex_unlock(&q.lock);
    }
    return NULL;
}
int main(void) {
    unsigned i; int16_t out[2048]; ps1_pacer p; pthread_t t; xpad_state s;
    for(i=0;i<2;++i) {
        unsigned period=ps1_frame_period(i?60:50),j;
        uint64_t start=1234567890123ull;
        ps1_pacer_reset(&p,start,period);
        for(j=0;j<10000;++j) { uint64_t due=p.due; ps1_pacer_step(&p,due+8000000); assert(p.due==due+period); }
        assert(p.due==start+10000ull*period);
        { uint64_t end=p.due+250000000; ps1_pacer_step(&p,end); assert(p.due==end && p.late==1); ps1_pacer_step(&p,end+8000000); assert(p.due==end+period); }
    }
    assert(ps1_frame_period(0)==16666667 && ps1_frame_period(50)==20000000);
    ps1_audio_take(&q,out,1024); assert(out[0]==0 && q.gaps==0);
    ps1_audio_active(&q,1); put(1024); ps1_audio_take(&q,out,1024); assert(ps1_audio_depth(&q)==1024);
    put(1024); ps1_audio_take(&q,out,1024);
    for(i=0;i<1024;++i) assert(out[i*2]==(int16_t)i && out[i*2+1]==(int16_t)~i);
    ps1_audio_take(&q,out,1024); ps1_audio_take(&q,out,1024); assert(q.gaps==1024);
    ps1_audio_active(&q,1); put(PS1_AUDIO_CAP+1); assert(ps1_audio_depth(&q)==PS1_AUDIO_CAP && q.drops==1);
    ps1_audio_active(&q,1); q.read=q.write=UINT_MAX-1023u; put(2048); ps1_audio_take(&q,out,1024); assert(ps1_audio_depth(&q)==1024);
    ps1_audio_active(&q,1); assert(pthread_create(&t,NULL,produce,NULL)==0);
    for(i=0;i<4000;++i) {
        unsigned k; ps1_audio_take(&q,out,1024);
        for(k=0;k<1024;++k) assert((out[2*k]==0 && out[2*k+1]==0) || out[2*k+1]==(int16_t)~out[2*k]);
    }
    pthread_join(t,NULL); assert(ps1_audio_depth(&q)<=PS1_AUDIO_CAP);
    assert(ps1_default_controls("Tomb Raider 5.chd")==PS1_ADVENTURE && ps1_default_controls("Medal.chd")==PS1_SHOOTER);
    memset(&s,0,sizeof s); s.rt=255; assert(ps1_control_buttons(&s,PS1_SHOOTER)==(1u<<NS_RP_B));
    s.rt=0;s.lt=255;assert(ps1_control_buttons(&s,PS1_SHOOTER)==(1u<<13));
    memset(&s,0,sizeof s);s.buttons=XPAD_A;assert(ps1_control_buttons(&s,PS1_SHOOTER)==(1u<<NS_RP_X));assert(ps1_control_buttons(&s,PS1_ADVENTURE)==(1u<<NS_RP_Y));
    memset(&s,0,sizeof s);s.lx=11000;s.rx=22000;s.ly=12000;s.ry=23000;
    assert(ps1_control_axis(&s,PS1_SHOOTER,0,0)==22000 && ps1_control_axis(&s,PS1_SHOOTER,1,0)==11000);
    assert(ps1_control_axis(&s,PS1_SHOOTER,0,1)==-12000 && ps1_control_axis(&s,PS1_SHOOTER,1,1)==-23000);
    assert(ps1_control_axis(&s,PS1_ORIGINAL,0,0)==11000);
    assert(ps1_control_axis(&s,PS1_ADVENTURE,0,0)==22000);
    assert(ps1_control_buttons(&s,PS1_ADVENTURE)==((1u<<10)|(1u<<NS_RP_RIGHT)|(1u<<NS_RP_UP)));
    s.buttons=XPAD_START|XPAD_BACK;assert(!(ps1_control_buttons(&s,PS1_SHOOTER)&((1u<<NS_RP_START)|(1u<<NS_RP_SELECT))));
    puts("PS1: 50/60 Hz deadlines and stall recovery, FIFO priming/overflow/wrap/concurrency, modern buttons/axes pass");
    return 0;
}
