/* Real session/serial plus resident owner, deterministic worker, no USB device. */
#define PODLINK_SERVICE_TEST
#define DEFAULT_STACK_SIZE 4096
#ifndef MAX
#define MAX(a,b) ((a) > (b) ? (a) : (b))
#endif
#define main transport_harness_main
#include "podlink_firmware_test.c"
#undef main
#undef podlink_open
#undef podlink_state
#undef podlink_max_message
#undef podlink_send
#undef podlink_close
#undef podlink_poll
#undef podlink_wait

static int disk_requests;
void usb_podlink_disk(void) { disk_requests++; }
static int clock_wakes;
void button_queue_post(long id, intptr_t data) { assert(id == 0x20000000 && !data); clock_wakes++; }
#include "podlink_rust_service.h"
#include "herdremote_model.h"
static int traced_client_poll(struct podlink_message *m) {
 int have=podlink_poll(m);trace(9,&have,sizeof have);
 if(have){unsigned meta[]={m->type,m->flags,m->seq,m->length};trace(10,meta,sizeof meta);trace(11,m->data,m->length);}
 return have;
}

static struct hr_model cached;
static int updates, losses;
static unsigned int log_format(const struct remote_log_record *r, char *out,
                                unsigned int size)
{
    return snprintf(out, size, "LOG TEST %lu", r->a);
}
static void background(const struct podlink_message *m)
{
    unsigned meta[]={m->type,m->flags,m->seq,m->length};trace(5,meta,sizeof meta);trace(6,m->data,m->length);
    if (m->type == PODLINK_CLOSE) losses++;
    else if (m->type == PODLINK_TEXT)
    {
        hr_model_apply(&cached, (const char *)m->data, m->length);
        updates++;
    }
}
static void work(void)
{
    static struct podlink_parser peer;
    static int cursor;
    if (output_size < cursor) {cursor=0;memset(&peer,0,sizeof(peer));}
    for (int i = 0; i < 20; i++) {
        service_step();controller();current_tick++;
        unsigned snap[]={podlink_state(),podlink_host_connected(),podlink_generation(),podlink_session_state(),podlink_session_revision()};trace(7,snap,sizeof snap);
        struct podlink_message m;
        while(cursor<output_size)if(podlink_decode(&peer,output[cursor++],&m)==1 && m.type==PODLINK_PING)
            input_frame(PODLINK_PONG,m.seq,m.data,m.length);
    }
}
static int resident_seen, resident_resets;
static unsigned cider_records;
static unsigned int cider_next(char *out,unsigned int capacity) {
 static bool sent;const char *record="cf\t1\tplay";unsigned n=strlen(record);
 if(sent || capacity<n)return 0;
 sent=true;memcpy(out,record,n);return n;
}
static bool dedicated_art(const struct podlink_message *m)
{
 if((m->type==PODLINK_TEXT && m->length>=3 && m->data[0]=='C') ||
    (m->type==PODLINK_BINARY && m->length>=4 && !memcmp(m->data,"CXS1",4))) {
   cider_records++;trace(8,m->data,m->length);return true;
 }

    if (m->type == PODLINK_BINARY && m->length >= 4 &&
        !memcmp(m->data, "CAR1", 4)) { resident_seen++; return true; }
    return false;
}
static void resident_reset(void) { resident_resets++; }

int main(void)
{
    assert(!podlink_host_connected());
    usb_serial_init();podlink_init();lease=true;service_step();
    assert(!podlink_host_connected());
    work();
    insert();work();assert(!podlink_host_connected());
    handshake(PODLINK_MAX_MESSAGE,1);work();assert(podlink_host_connected());
    /* Auth is session-bound and independent of the active screen. */
    const char *auth = "DA\t1\t0123456789abcdef0123456789abcdef";
    const char *disk = "DK\t1\t0123456789abcdef0123456789abcdef";
    input_frame(PODLINK_TEXT,90,disk,strlen(disk));work();assert(!disk_requests);
    input_frame(PODLINK_TEXT,91,"HR\t1\thost",9);work();
    for (int screen=0;screen<5;screen++) {
        if(screen==2)assert(podlink_open()); /* Cider */
        if(screen==3)assert(podlink_open_agents(background));
        input_frame(PODLINK_TEXT,92,auth,strlen(auth));work();
        input_frame(PODLINK_TEXT,93,"DK\t1\tffffffffffffffffffffffffffffffff",37);work();
        assert(disk_requests==screen);
        input_frame(PODLINK_TEXT,94,disk,strlen(disk));work();
        assert(disk_requests==screen+1);
        input_frame(PODLINK_TEXT,95,disk,strlen(disk));work();
        assert(disk_requests==screen+1);
        podlink_close();
    }
    input_frame(PODLINK_TEXT,95,auth,strlen(auth));work();
    assert(podlink_rust_debug(14));
    /* Foreground inbox loss does not invalidate the authenticated wire. */
    assert(podlink_open());
    for(int i=0;i<20;i++){input_frame(PODLINK_TEXT,95,"ignored",7);work();}
    assert(podlink_rust_debug(14) && podlink_rust_debug(15));
    podlink_close();
    podlink_rust_invalidate();
    assert(!podlink_rust_debug(14));
    input_frame(PODLINK_TEXT,96,disk,strlen(disk));work();assert(disk_requests==5);
    /* The input callback must not try either mutex, even if the worker or
     * USB thread was interrupted while holding it. */
    mutex_lock(&service_mutex);mutex_lock(&serial_mtx);
    assert(podlink_host_connected());
    mutex_unlock(&serial_mtx);mutex_unlock(&service_mutex);
    char clock_record[PODLINK_TIME_RECORD];unsigned long clock_tick;
    assert(!podlink_take_time(clock_record,&clock_tick));
    const char *stamp="TS\t1\t2026-11-13T19:02:03\t-18000";
    for(int fg=0;fg<3;fg++) {
        if(fg==1)assert(podlink_open_agents(background));
        if(fg==2)assert(podlink_open());
        int wake=clock_wakes;input_frame(PODLINK_TEXT,11+fg,stamp,strlen(stamp));work();
        assert(clock_wakes==wake+1&&podlink_take_time(clock_record,&clock_tick));
        assert(!strcmp(clock_record,stamp)&&!podlink_take_time(clock_record,&clock_tick));
        podlink_close();work();
    }
    assert(podlink_open());
    input_frame(PODLINK_TEXT,14,stamp,strlen(stamp));work();
    for(int i=0;i<20;i++){input_frame(PODLINK_TEXT,14,"ignored",7);work();}
    assert(podlink_take_time(clock_record,&clock_tick)&&!strcmp(clock_record,stamp));
    podlink_close();work();
    int wakes=clock_wakes;
    input_frame(PODLINK_TEXT,15,stamp,strlen(stamp));work();
    const char *newer="TS\t1\t2026-11-13T19:03:03\t-18000";
    input_frame(PODLINK_TEXT,16,newer,strlen(newer));work();
    assert(clock_wakes==wakes+1&&podlink_take_time(clock_record,&clock_tick));
    assert(!strcmp(clock_record,newer));
    char oversized[80];memset(oversized,'x',sizeof(oversized));memcpy(oversized,"TS\t",3);
    input_frame(PODLINK_TEXT,16,oversized,sizeof(oversized));work();
    assert(podlink_take_time(clock_record,&clock_tick)&&!clock_record[0]);
    input_frame(PODLINK_TEXT,16,"TS\t1\0bad",8);work();
    assert(podlink_take_time(clock_record,&clock_tick)&&!clock_record[0]);
    input_frame(PODLINK_TEXT,17,stamp,strlen(stamp));work();assert(podlink_rust_debug(16));
    podlink_rust_invalidate();assert(!podlink_take_time(clock_record,&clock_tick));
    uint32_t revision=podlink_session_revision();
    input_frame(PODLINK_TEXT,2,"HR\t1\thost",9);work();
    assert(podlink_open_agents(background));work();
    struct podlink_message greeting;
    assert(traced_client_poll(&greeting)&&greeting.type==PODLINK_TEXT&&greeting.length==9);
    assert(podlink_state()==PODLINK_CONNECTED);podlink_close();work();
    assert(podlink_state()==PODLINK_CLOSED&&podlink_host_connected());
    const char *list="W\t1\nw1\t-\t1\t0\t0\t0\tCached";
    input_frame(PODLINK_TEXT,50,list,strlen(list));work();
    assert(updates>=1&&cached.nws==1);
    for(int i=0;i<500;i++) {
        clear_output();assert(podlink_open_agents(background));
        assert(!strcmp(cached.ws[0].label,"Cached")); /* first frame, zero I/O */
        assert(podlink_state()==PODLINK_CONNECTED);
        assert(output_size==0);
        work();podlink_close();work();
        assert(lease&&podlink_session_revision()==revision);
        struct podlink_parser parser={0};struct podlink_message m;
        for(int j=0;j<output_size;j++)if(podlink_decode(&parser,output[j],&m)==1)
            assert(m.type!=PODLINK_HELLO&&m.type!=PODLINK_OPEN&&m.type!=PODLINK_CLOSE);
    }
    assert(podlink_open());assert(!podlink_open_agents(background));work();
    assert(podlink_session_revision()==revision);podlink_close();work();
    /* Same wire owner in menus/WPS (0), Agents (1), and another client (2).
     * Enable/toggle does not attach, reconnect or alter the foreground. */
    for (int foreground=0; foreground<=2; foreground++)
    {
        if (foreground==1) assert(podlink_open_agents(background));
        if (foreground==2) assert(podlink_open());
        work();clear_output();
        remote_log_enable(true,log_format);
        remote_log_event(REMOTE_LOG_TAP,1,42,0,NULL,0);
        work();
        struct podlink_parser parser={0};struct podlink_message frame;
        int logs=0;
        for(int j=0;j<output_size;j++)
            if(podlink_decode(&parser,output[j],&frame)==1)
            {
                if(frame.type==PODLINK_TEXT && frame.length==11 &&
                   !memcmp(frame.data,"LOG TEST 42",11)) logs++;
                assert(frame.type!=PODLINK_HELLO && frame.type!=PODLINK_OPEN &&
                       frame.type!=PODLINK_CLOSE);
            }
        assert(logs==1 && podlink_rust_debug(10)==(unsigned)foreground);
        assert(podlink_session_revision()==revision);
        remote_log_enable(false,log_format);
        clear_output();remote_log_event(REMOTE_LOG_TAP,1,43,0,NULL,0);
        work();
        memset(&parser,0,sizeof(parser));
        for(int j=0;j<output_size;j++)
            if(podlink_decode(&parser,output[j],&frame)==1)
                assert(frame.type!=PODLINK_TEXT);
        podlink_close();work();
    }
    /* Fill the real TX queue and freeze its serial sink for one step. */
    while(podlink_session_send(PODLINK_TEXT,"busy",4)==PODLINK_SENT) {}
    int saved_tx_count=tx_count;tx_count=BUFFER_SIZE;
    remote_log_enable(true,log_format);
    remote_log_event(REMOTE_LOG_TAP,1,44,0,NULL,0);
    service_step();assert(podlink_rust_debug(17)==11);
    assert(!memcmp(log_line,"LOG TEST 44",11));
    remote_log_enable(false,log_format);
    service_step();assert(!podlink_rust_debug(17));
    tx_count=saved_tx_count;work();
    /* A second busy line survives until the transport accepts it. */
    while(podlink_session_send(PODLINK_TEXT,"busy",4)==PODLINK_SENT) {}
    saved_tx_count=tx_count;tx_count=BUFFER_SIZE;
    remote_log_enable(true,log_format);
    remote_log_event(REMOTE_LOG_TAP,1,45,0,NULL,0);
    service_step();assert(podlink_rust_debug(17)==11);
    tx_count=saved_tx_count;clear_output();work();
    assert(!podlink_rust_debug(17));
    struct podlink_parser log_parser={0};struct podlink_message log_frame;
    bool retried=false;
    for(int j=0;j<output_size;j++)
        if(podlink_decode(&log_parser,output[j],&log_frame)==1 &&
           log_frame.type==PODLINK_TEXT && log_frame.length==11 &&
           !memcmp(log_frame.data,"LOG TEST 45",11)) retried=true;
    assert(retried);remote_log_enable(false,log_format);work();
    /* A small peer gets an explicit loss notice, never an INVALID loop. */
    uint32_t saved_peer_max=podlink_rust_debug(5);podlink_rust_set_peer(10);
    remote_log_enable(true,log_format);clear_output();
    remote_log_event(REMOTE_LOG_TAP,1,12345678,0,NULL,0);work();
    memset(&log_parser,0,sizeof(log_parser));bool small_notice=false;
    for(int j=0;j<output_size;j++)
        if(podlink_decode(&log_parser,output[j],&log_frame)==1 &&
           log_frame.type==PODLINK_TEXT && log_frame.length==10 &&
           !memcmp(log_frame.data,"LOG LOST 1",10)) small_notice=true;
    assert(small_notice && !podlink_rust_debug(17));
    podlink_rust_set_peer(9);remote_log_event(REMOTE_LOG_TAP,1,46,0,NULL,0);work();
    assert(remote_log_read(log_line,sizeof(log_line))==11);
    assert(!memcmp(log_line,"LOG TEST 46",11));
    podlink_rust_set_peer(saved_peer_max);remote_log_enable(false,log_format);work();
    /* Payload lifetime survives later worker polls until the next podlink_rust_debug(10) poll. */
    assert(podlink_open_agents(background));
    struct podlink_message replay;while(traced_client_poll(&replay)){}
    input_frame(PODLINK_TEXT,60,"one",3);work();struct podlink_message m;
    assert(traced_client_poll(&m));assert(!memcmp(m.data,"one",3));
    input_frame(PODLINK_TEXT,61,"two",3);work();assert(!memcmp(m.data,"one",3));
    assert(traced_client_poll(&m)&&!memcmp(m.data,"two",3));podlink_close();work();
    assert(podlink_open_agents(background));while(traced_client_poll(&m)){}
    uint32_t before_pressure=podlink_generation();
    for(int i=0;i<80;i++){input_frame(PODLINK_TEXT,100+i,"delta",5);work();}
    assert(podlink_generation()!=before_pressure);
    assert(podlink_session_state()==PODLINK_CONNECTED&&podlink_session_revision()==revision);
    bool greeting_after_pressure=false;
    while(traced_client_poll(&m))if(m.type==PODLINK_TEXT&&m.length>=3&&!memcmp(m.data,"HR\t",3))greeting_after_pressure=true;
    assert(greeting_after_pressure);podlink_close();work();
    /* Dedicated bulk bypasses both foreground byte and frame limits. */
    static const struct podlink_resident consumer = {
        dedicated_art, cider_next, resident_reset, NULL};
    podlink_set_resident(&consumer);
    assert(podlink_open_agents(background));
    while(traced_client_poll(&m)){}
    before_pressure=podlink_generation();
    const char *cider[]={"CA\t1\tplaying","CN\t1\tSong","CL\t1\t0\t1\tSong","CK\t1\tok","CXS1xxxx"};
    for(unsigned i=0;i<5;i++){input_frame(i==4?PODLINK_BINARY:PODLINK_TEXT,700+i,cider[i],strlen(cider[i]));work();}
    assert(cider_records==5);
    unsigned char art_frame[4096]={0};memcpy(art_frame,"CAR1",4);
    for(int i=0;i<80;i++)
    {input_frame(PODLINK_BINARY,1000+i,art_frame,sizeof art_frame);work();}
    assert(resident_seen==80 && !podlink_rust_debug(12) && !podlink_rust_debug(13));
    puts("PASS: 80 dedicated art frames / 327680 bytes bypass the foreground inbox");
    assert(podlink_generation()==before_pressure);
    podlink_close();work();
    /* Disconnect/reset invalidates the snapshot before a worker poll. */
    assert(podlink_host_connected());remove_usb();
    work();assert(resident_resets>0);
    assert(!podlink_host_connected());
    work();
    insert();assert(!podlink_host_connected());
    work();assert(!podlink_host_connected());
    lease=false;assert(!podlink_host_connected());
    work();assert(losses>0&&!podlink_rust_debug(11));
    puts("resident real wire: 500 attach/switch/MENU exits; no OPEN/CLOSE/HELLO; cached first frame zero round trips; background list; client exclusion; payload lifetime; unplug PASS");
    puts("remote log real wire: prefixed TEXT in foreground 0/1/2; default/off no TEXT; no reconnect/client ownership change PASS");
    puts("resident host snapshot: configured current generation + OPEN; background independent; IRQ-safe/no mutex; immediate USB disconnect/reset invalidation PASS");
}
