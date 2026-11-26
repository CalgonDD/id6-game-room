/* gr_pad.c: Game Room's wired Xbox controller reader (#included by each game program on QNX).
 *
 * Connects to io-usb through libusbdi (as NBT-AAD's usb_direct does on this unit), logs EVERY USB device the
 * stack reports (vendor/product/class), and attaches only to a wired Xbox pad:
 *   - Xbox 360 (XInput, interface FF/5D/01): reads 20-byte reports, lights player-1 LED;
 *   - Xbox One / Series (GIP, FF/47/D0): runs the GIP host handshake without authentication (see below).
 * Each change of state goes to the display test (UDP GR_SHOW_PORT, "p ..."), which moves its square.
 * The HMI trace gets: devices seen, the pad's endpoints, the first raw reports, every button change and a
 * stick summary twice a second. Ends after [seconds] (default 20, at most 60) or on SIGTERM.
 * Phones and every other device are only listed, never attached. Nothing here talks to a car module.
 * QNX only. libusbdi.so.2 is opened at run time (it is in /fs/sda0/lib on this unit), so a missing library can
 * only fail this mode, never the trace writer or the display test in the same binary. The decoding is
 * host-tested through xpad.h. */
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <dlfcn.h>
#include <sys/usbdi.h>
#include "gr_log.h"        /* mps_log, mps_now_ms, mps_sleep_ms */
#include "gr_show_port.h"  /* where the display test listens */
#include "xpad.h"
#include "gip.h"

/* ---- libusbdi, bound at run time ---- */
#define PAD_FN(name) static __typeof__(name) *p_##name
PAD_FN(usbd_connect); PAD_FN(usbd_disconnect); PAD_FN(usbd_attach); PAD_FN(usbd_detach);
PAD_FN(usbd_interface_descriptor); PAD_FN(usbd_parse_descriptors); PAD_FN(usbd_open_pipe);
PAD_FN(usbd_close_pipe); PAD_FN(usbd_alloc_urb); PAD_FN(usbd_free_urb); PAD_FN(usbd_alloc); PAD_FN(usbd_free);
PAD_FN(usbd_setup_interrupt); PAD_FN(usbd_io); PAD_FN(usbd_urb_status); PAD_FN(usbd_abort_pipe);
#undef PAD_FN

static int pad_load_usbdi(void) {
    static const char *const paths[] = {"libusbdi.so.2", "/lib/libusbdi.so.2", "/fs/sda0/lib/libusbdi.so.2",
                                        "/usr/lib/libusbdi.so.2"};
    void *lib = 0;
    unsigned i;
    int missing = 0;
    for (i = 0; i < sizeof paths / sizeof paths[0] && !lib; ++i) lib = dlopen(paths[i], RTLD_NOW);
    if (!lib) {
        const char *why = dlerror();
        mps_log("pad fail step=libusbdi_open error=%s", why ? why : "?");
        return 0;
    }
    /* memcpy, not a cast through void**: QNX gcc -Werror rejects the type-punned store. */
#define PAD_BIND(name) do { void *f_ = dlsym(lib, #name); if (!f_) { mps_log("pad fail step=libusbdi_symbol name=%s", #name); ++missing; } memcpy(&p_##name, &f_, sizeof f_); } while (0)
    PAD_BIND(usbd_connect); PAD_BIND(usbd_disconnect); PAD_BIND(usbd_attach); PAD_BIND(usbd_detach);
    PAD_BIND(usbd_interface_descriptor); PAD_BIND(usbd_parse_descriptors); PAD_BIND(usbd_open_pipe);
    PAD_BIND(usbd_close_pipe); PAD_BIND(usbd_alloc_urb); PAD_BIND(usbd_free_urb); PAD_BIND(usbd_alloc);
    PAD_BIND(usbd_free); PAD_BIND(usbd_setup_interrupt); PAD_BIND(usbd_io); PAD_BIND(usbd_urb_status); PAD_BIND(usbd_abort_pipe);
#undef PAD_BIND
    return missing == 0;
}
#define usbd_connect p_usbd_connect
#define usbd_disconnect p_usbd_disconnect
#define usbd_attach p_usbd_attach
#define usbd_detach p_usbd_detach
#define usbd_interface_descriptor p_usbd_interface_descriptor
#define usbd_parse_descriptors p_usbd_parse_descriptors
#define usbd_open_pipe p_usbd_open_pipe
#define usbd_close_pipe p_usbd_close_pipe
#define usbd_alloc_urb p_usbd_alloc_urb
#define usbd_free_urb p_usbd_free_urb
#define usbd_alloc p_usbd_alloc
#define usbd_free p_usbd_free
#define usbd_setup_interrupt p_usbd_setup_interrupt
#define usbd_io p_usbd_io
#define usbd_urb_status p_usbd_urb_status
#define usbd_abort_pipe p_usbd_abort_pipe


/* ---- I/O model (car result 2026-11-19, pull 01-10-17) ----
 * usbd_io() does NOT wait, even with no callback: it returns EOK at once and the URB stays
 * USBD_STATUS_INPROG (status 00000000). The first version read that as an error 155 times, re-armed the same
 * URB while it was still queued (so the GIP power-on packet was very likely overwritten before it went out),
 * and the Turtle Beach pad (10f5:7113) then dropped off the bus and re-enumerated; the reader also hung.
 * Now, as NBT-AAD's usb_direct does: every transfer has its own URB and buffer and a completion callback
 * (io-usb's thread), the main loop waits on a condition variable, a URB is only re-armed after its callback,
 * and the IN pipe is aborted before it is closed. A SIGALRM backstop _exits if anything still blocks. */

static volatile sig_atomic_t stop_flag = 0;
static void on_stop(int sig) { (void)sig; stop_flag = 1; }
static void on_alarm(int sig) { (void)sig; _exit(5); }   /* hard backstop: the deadline already passed */
/* Where pad states go: the running game (pad_run), or the screen test pattern. */
static unsigned short pad_port = GR_SHOW_PORT;

static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv = PTHREAD_COND_INITIALIZER;
static usbd_device_instance_t pending;
static int have_pending = 0, attached_devno = -1, removed = 0;

static const unsigned pad_vendors[] = {0x045E, 0x0E6F, 0x24C6, 0x0F0D, 0x046D, 0x0738, 0x1532, 0x20D6,
                                       0x2DC8, 0x3537, 0x2563, 0x1BAD, 0x12AB, 0x0079, 0x2E24, 0x10F5};

static int maybe_pad(const usbd_device_instance_t *i) {
    unsigned k;
    if (xpad_kind(i->ident.dclass, i->ident.subclass, i->ident.protocol) != XPAD_NONE) return 1;
    for (k = 0; k < sizeof pad_vendors / sizeof pad_vendors[0]; ++k)
        if (i->ident.vendor == pad_vendors[k]) return 1;
    return 0;
}

static void on_insert(struct usbd_connection *c, usbd_device_instance_t *i) {
    (void)c;
    mps_log("pad usb_device_in devno=%u vid=%04x pid=%04x class=%02x/%02x/%02x config=%u iface=%u alt=%u%s",
            i->devno, (unsigned)i->ident.vendor, (unsigned)i->ident.device, (unsigned)i->ident.dclass,
            (unsigned)i->ident.subclass, (unsigned)i->ident.protocol, (unsigned)i->config, (unsigned)i->iface,
            (unsigned)i->alternate, maybe_pad(i) ? " candidate" : "");
    pthread_mutex_lock(&mu);
    if (maybe_pad(i) && attached_devno < 0 && !have_pending && i->iface == 0) {
        pending = *i;
        have_pending = 1;
        pthread_cond_signal(&cv);
    }
    pthread_mutex_unlock(&mu);
}

static void on_remove(struct usbd_connection *c, usbd_device_instance_t *i) {
    (void)c;
    mps_log("pad usb_device_out devno=%u vid=%04x pid=%04x", i->devno, (unsigned)i->ident.vendor,
            (unsigned)i->ident.device);
    pthread_mutex_lock(&mu);
    if ((int)i->devno == attached_devno) removed = 1;
    if (have_pending && pending.devno == i->devno) have_pending = 0;
    pthread_cond_signal(&cv);
    pthread_mutex_unlock(&mu);
}

static void send_show(int fd, const char *text) {
    struct sockaddr_in a;
#ifdef RETRO_CORE_PS1
    char timed[160];
    if(!strncmp(text,"p ",2)) { snprintf(timed,sizeof timed,"%s t=%llu",text,(unsigned long long)mps_now_ms()); text=timed; }
#endif
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = htons(pad_port);
    sendto(fd, text, strlen(text), 0, (struct sockaddr *)&a, sizeof a);
}

static void hex(const uint8_t *b, unsigned n, char *out, unsigned size) {
    unsigned i;
    out[0] = 0;
    for (i = 0; i < n && i * 2 + 3 <= size; ++i) snprintf(out + i * 2, 3, "%02x", b[i]);
}

static const char *const button_names[XPAD_BUTTON_COUNT] = {"up", "down", "left", "right", "start", "back", "ls",
                                                          "rs", "lb", "rb", "guide", "a", "b", "x", "y"};


/* ---- GIP handshake probe (owner's choice 2026-11-19 after the Turtle Beach 10f5:7113 went silent) ----
 * The pad answered the xpad-style power-on with one empty input report and then stopped. Similar Turtle Beach
 * pads need the full GIP host handshake (and some, authentication). This version does everything except
 * authentication and logs every packet so the next pull shows what the pad asks for:
 *   1. two reads queued at all times;
 *   2. a descriptor request (04 20 seq 00) first; power-on (05 20 seq 01 00) when the descriptor is complete,
 *      or after 1 s without one; Microsoft pads (045e) then also get 05 20 seq 0f 06, PDP (0e6f) led+auth;
 *   3. every packet with needs-ack answered (chunk-aware), an arrival (0x02) re-starts the handshake;
 *   4. authentication packets (0x06) are logged in full, not answered (no crypto here);
 *   5. no packet for 3 s: logged as "quiet" and the handshake is repeated (at most 3 times). */

typedef struct {
    struct usbd_urb *urb;
    uint8_t *buf;
    int busy, done;
    _Uint32t status, len;
} pad_xfer;

typedef struct {
    struct usbd_device *dev;
    struct usbd_pipe *in, *out;
    pad_xfer rx[2], tx;
    int kind;
    unsigned seq;                 /* next host sequence number (1..255) */
    unsigned vendor;
    uint64_t desc_asked_ms;       /* when the descriptor was requested (0 = not pending) */
    int powered, pdp;
    unsigned inputs;              /* input reports since the last power-on */
    gip_chunks chunks;
} pad_link;

static void xfer_done(struct usbd_urb *urb, struct usbd_pipe *pipe, void *handle) {
    pad_xfer *x = (pad_xfer *)handle;
    _Uint32t status = 0, len = 0;
    (void)pipe;
    usbd_urb_status(urb, &status, &len);
    pthread_mutex_lock(&mu);
    x->status = status;
    x->len = len;
    x->busy = 0;
    x->done = 1;
    pthread_cond_broadcast(&cv);
    pthread_mutex_unlock(&mu);
}

static int xfer_alloc(pad_xfer *x) {
    memset(x, 0, sizeof *x);
    x->urb = usbd_alloc_urb(0);
    x->buf = (uint8_t *)usbd_alloc(64);
    return x->urb && x->buf;
}

static void xfer_free(pad_xfer *x) {
    if (x->urb) usbd_free_urb(x->urb);
    if (x->buf) usbd_free(x->buf);
    memset(x, 0, sizeof *x);
}

/* Waits (mutex held by caller) until *busy is 0, a signal, or the time is up. */
static void wait_flag(volatile int *busy, unsigned ms) {
    struct timespec until;
    clock_gettime(CLOCK_REALTIME, &until);
    until.tv_nsec += (long)(ms % 1000u) * 1000000L;
    until.tv_sec += ms / 1000u + until.tv_nsec / 1000000000L;
    until.tv_nsec %= 1000000000L;
    while (*busy && !stop_flag)
        if (pthread_cond_timedwait(&cv, &mu, &until) == ETIMEDOUT) break;
}

/* Sends one packet on the OUT pipe and waits up to 500 ms for it. A packet whose byte 2 is 0 gets the next
 * host sequence number; an ack keeps the device's sequence number it answers. */
static int pad_send(pad_link *p, const uint8_t *msg, unsigned n, const char *what) {
    int r;
    if (!p->out || n > 64) return 0;
    pthread_mutex_lock(&mu);
    if (p->tx.busy) wait_flag(&p->tx.busy, 500);
    if (p->tx.busy) {
        pthread_mutex_unlock(&mu);
        mps_log("pad %s skipped: previous packet still queued", what);
        return 0;
    }
    memcpy(p->tx.buf, msg, n);
    if (p->kind == XPAD_ONE && n >= 3 && p->tx.buf[2] == 0) {
        if (p->seq == 0 || p->seq > 255) p->seq = 1;
        p->tx.buf[2] = (uint8_t)p->seq++;
    }
    p->tx.busy = 1;
    p->tx.done = 0;
    pthread_mutex_unlock(&mu);
    usbd_setup_interrupt(p->tx.urb, URB_DIR_OUT, p->tx.buf, n);
    r = usbd_io(p->tx.urb, p->out, xfer_done, &p->tx, 1000);
    pthread_mutex_lock(&mu);
    if (r != EOK) p->tx.busy = 0;
    else wait_flag(&p->tx.busy, 500);
    {
        char h[2 * 16 + 1];
        hex(p->tx.buf, n > 16 ? 16 : n, h, sizeof h);
        mps_log("pad send %s rc=%d status=%08x%s hex=%s", what, r, (unsigned)p->tx.status,
                p->tx.busy ? " (still queued)" : "", h);
    }
    r = r == EOK && !p->tx.busy && (p->tx.status & USBD_URB_STATUS_MASK) == USBD_STATUS_CMP;
    pthread_mutex_unlock(&mu);
    return r;
}

/* Queues read slot k on the IN pipe (caller must not hold mu). */
static int pad_arm(pad_link *p, int k) {
    int r;
    pad_xfer *x = &p->rx[k];
    pthread_mutex_lock(&mu);
    x->busy = 1;
    x->done = 0;
    pthread_mutex_unlock(&mu);
    usbd_setup_interrupt(x->urb, URB_DIR_IN | URB_SHORT_XFER_OK, x->buf, 64);
    r = usbd_io(x->urb, p->in, xfer_done, x, USBD_TIME_INFINITY);
    if (r != EOK) {
        pthread_mutex_lock(&mu);
        x->busy = 0;
        pthread_mutex_unlock(&mu);
        mps_log("pad arm_failed slot=%d rc=%d", k, r);
    }
    return r == EOK;
}

static void pad_close(pad_link *p) {
    if (p->in) {
        usbd_abort_pipe(p->in);
        pthread_mutex_lock(&mu);
        wait_flag(&p->rx[0].busy, 300);
        wait_flag(&p->rx[1].busy, 300);
        pthread_mutex_unlock(&mu);
        usbd_close_pipe(p->in);
    }
    if (p->out) {
        usbd_abort_pipe(p->out);
        pthread_mutex_lock(&mu);
        wait_flag(&p->tx.busy, 300);
        pthread_mutex_unlock(&mu);
        usbd_close_pipe(p->out);
    }
    xfer_free(&p->rx[0]);
    xfer_free(&p->rx[1]);
    xfer_free(&p->tx);
    if (p->dev) usbd_detach(p->dev);
    memset(p, 0, sizeof *p);
}

static const uint8_t gip_desc_request[4] = {0x04, 0x20, 0x00, 0x00};
static const uint8_t gip_power_on[5] = {0x05, 0x20, 0x00, 0x01, 0x00};
static const uint8_t gip_s_init[5] = {0x05, 0x20, 0x00, 0x0f, 0x06};
static const uint8_t gip_pdp_led[7] = {0x0a, 0x20, 0x00, 0x03, 0x00, 0x01, 0x14};
static const uint8_t gip_pdp_auth[6] = {0x06, 0x20, 0x00, 0x02, 0x01, 0x00};
static const uint8_t x360_led1[3] = {0x01, 0x03, 0x06};

static void gip_start_handshake(pad_link *p, const char *why) {
    mps_log("pad handshake_start reason=%s", why);
    memset(&p->chunks, 0, sizeof p->chunks);
    p->powered = 0;
    pad_send(p, gip_desc_request, sizeof gip_desc_request, "gip_descriptor_request");
    p->desc_asked_ms = mps_now_ms();
}

static void gip_power(pad_link *p, const char *why) {
    mps_log("pad power_on reason=%s", why);
    pad_send(p, gip_power_on, sizeof gip_power_on, "gip_power_on");
    if (p->vendor == 0x045Eu) pad_send(p, gip_s_init, sizeof gip_s_init, "gip_s_init");
    /* PDP pads only start sending input after the "authentication done" packet (Linux xpad: xboxone_pdp_auth).
     * Car result 2026-11-19: Turtle Beach 10f5:7113 names itself "Pdp.SeriesX" in its descriptor (Turtle Beach
     * owns PDP), so PDP's packets go to 0e6f, to 10f5, and to anything whose descriptor says Pdp. */
    if (p->vendor == 0x0E6Fu || p->vendor == 0x10F5u || p->pdp) {
        pad_send(p, gip_pdp_led, sizeof gip_pdp_led, "gip_pdp_led");
        pad_send(p, gip_pdp_auth, sizeof gip_pdp_auth, "gip_pdp_auth");
    }
    p->powered = 1;
    p->inputs = 0;
    p->desc_asked_ms = 0;
}

/* Attaches, finds the Xbox interface and its interrupt endpoints, queues two reads and starts the handshake. */
static int pad_attach_error;   /* the last attach's error (EBUSY: another program still holds the pad) */
static int pad_open(struct usbd_connection *conn, usbd_device_instance_t *inst, pad_link *p) {
    unsigned iface, config = inst->config ? inst->config : 1u;
    int r;
    memset(p, 0, sizeof *p);
    r = usbd_attach(conn, inst, 0, &p->dev);
    pad_attach_error = r == EOK ? 0 : r;
    if (r != EOK) {
        mps_log("pad attach_failed devno=%u error=%d", inst->devno, r);
        return 0;
    }
    for (iface = 0; iface < 8 && !p->kind; ++iface) {
        struct usbd_desc_node *ifn = 0;
        usbd_interface_descriptor_t *id = usbd_interface_descriptor(p->dev, (uint8_t)config, (uint8_t)iface, 0, &ifn);
        int k, e;
        if (!id) continue;
        k = xpad_kind(id->bInterfaceClass, id->bInterfaceSubClass, id->bInterfaceProtocol);
        mps_log("pad interface n=%u class=%02x/%02x/%02x endpoints=%u%s", iface, id->bInterfaceClass,
                id->bInterfaceSubClass, id->bInterfaceProtocol, id->bNumEndpoints,
                k == XPAD_360 ? " xbox360" : k == XPAD_ONE ? " xbox_one" : "");
        if (k == XPAD_NONE) continue;
        for (e = 0; e < 8; ++e) {
            struct usbd_desc_node *epn = 0;
            usbd_descriptors_t *d = usbd_parse_descriptors(p->dev, ifn, USB_DESC_ENDPOINT, e, &epn);
            if (!d) break;
            mps_log("pad endpoint addr=%02x attr=%02x max=%u interval=%u", d->endpoint.bEndpointAddress,
                    d->endpoint.bmAttributes, (unsigned)d->endpoint.wMaxPacketSize, d->endpoint.bInterval);
            if ((d->endpoint.bmAttributes & 3u) != USB_ATTRIB_INTERRUPT) continue;
            if ((d->endpoint.bEndpointAddress & 0x80u) && !p->in) {
                if ((r = usbd_open_pipe(p->dev, d, &p->in)) != EOK) mps_log("pad open_in_failed error=%d", r);
            } else if (!(d->endpoint.bEndpointAddress & 0x80u) && !p->out) {
                if ((r = usbd_open_pipe(p->dev, d, &p->out)) != EOK) mps_log("pad open_out_failed error=%d", r);
            }
        }
        if (p->in) p->kind = k;
    }
    if (!p->kind) {
        mps_log("pad not_an_xbox_pad devno=%u (no FF/5D/01 or FF/47/D0 interface with an interrupt IN endpoint)",
                inst->devno);
        pad_close(p);
        return 0;
    }
    if (!xfer_alloc(&p->rx[0]) || !xfer_alloc(&p->rx[1]) || !xfer_alloc(&p->tx)) {
        mps_log("pad no_memory");
        pad_close(p);
        return 0;
    }
    p->vendor = inst->ident.vendor;
    attached_devno = inst->devno;
    removed = 0;
    if (!pad_arm(p, 0) || !pad_arm(p, 1)) {    /* listen before saying anything */
        pad_close(p);
        attached_devno = -1;
        return 0;
    }
    mps_log("pad ready kind=%s devno=%u vid=%04x pid=%04x reads_queued=2", p->kind == XPAD_ONE ? "xbox_one" : "xbox360",
            inst->devno, (unsigned)inst->ident.vendor, (unsigned)inst->ident.device);
    if (p->kind == XPAD_ONE) {
        if (!p->out) mps_log("pad warning=no_out_endpoint (an Xbox One pad stays silent without the power-on packet)");
        gip_start_handshake(p, "attach");
    } else {
        pad_send(p, x360_led1, sizeof x360_led1, "led_player1");
    }
    return 1;
}

/* seconds = 0: no deadline (the game stops it through stop_flag); own_signals = 0: the caller owns the
 * process signals (the game runs this in a thread). */
static int pad_run(unsigned seconds, unsigned short port, int own_signals) {
    static usbd_device_ident_t ident = {USBD_CONNECT_WILDCARD, USBD_CONNECT_WILDCARD, USBD_CONNECT_WILDCARD,
                                        USBD_CONNECT_WILDCARD, USBD_CONNECT_WILDCARD};
    static usbd_funcs_t funcs = {_USBDI_NFUNCS, on_insert, on_remove, 0};
    usbd_connect_parm_t parm;
    struct usbd_connection *conn = 0;
    pad_link pad;
    xpad_state st, sent;
    unsigned reports = 0, logged = 0, changes = 0, errors = 0, acks = 0, auth = 0, retries = 0, late_sends = 0;
    unsigned per_cmd[256];
    uint64_t deadline, last_send = 0, last_summary = 0, last_rx = 0;
    int fd, r;
    struct sigaction a;

    pad_port = port;
    if (own_signals) {
        memset(&a, 0, sizeof a);
        a.sa_handler = on_stop;
        sigemptyset(&a.sa_mask);
        sigaction(SIGTERM, &a, 0);
        sigaction(SIGINT, &a, 0);
        sigaction(SIGHUP, &a, 0);
        a.sa_handler = on_alarm;
        sigaction(SIGALRM, &a, 0);
        signal(SIGPIPE, SIG_IGN);
        if (seconds) alarm(seconds + 6u);
    } else {
        (void)a;
    }
    memset(&pad, 0, sizeof pad);
    memset(per_cmd, 0, sizeof per_cmd);
    if (!pad_load_usbdi()) return 4;
    memset(&st, 0, sizeof st);
    memset(&sent, 0, sizeof sent);
    fd = socket(AF_INET, SOCK_DGRAM, 0);

    memset(&parm, 0, sizeof parm);
    parm.vusb = USB_VERSION;
    parm.vusbd = USBD_VERSION;
    parm.ident = &ident;
    parm.funcs = &funcs;
    parm.connect_wait = USBD_CONNECT_WAIT;
    r = usbd_connect(&parm, &conn);
    if (r != EOK) {
        mps_log("pad usbd_connect_failed error=%d (%s)", r, strerror(r));
        return 3;
    }
    mps_log("pad begin seconds=%u io=async handshake=gip-no-auth+pdp (devices already plugged in are listed next)", seconds);
    deadline = seconds ? mps_now_ms() + seconds * 1000u : (uint64_t)-1;

    unsigned busy_retries = 0;
    while (!stop_flag && mps_now_ms() < deadline) {
        usbd_device_instance_t inst;
        int take = 0, gone, k, n = 0, slots[2];
        _Uint32t status[2], len[2];
        uint8_t data[2][64];
        uint64_t now;

        pthread_mutex_lock(&mu);
        if (!(pad.dev && (pad.rx[0].done || pad.rx[1].done)) && !have_pending && !removed) {
            int forever = 1;
            /* until a completion/insertion signals, or 100 ms; 16 ms while a stick change is still held back */
            wait_flag(&forever, memcmp(&st, &sent, sizeof st) ? 16 : 100);
        }
        if (!pad.dev && have_pending) { inst = pending; have_pending = 0; take = 1; }
        gone = removed;
        for (k = 0; pad.dev && k < 2; ++k) {
            if (!pad.rx[k].done) continue;
            status[n] = pad.rx[k].status;
            len[n] = pad.rx[k].len > 64 ? 64 : pad.rx[k].len;
            memcpy(data[n], pad.rx[k].buf, len[n]);
            pad.rx[k].done = 0;
            slots[n] = k;
            ++n;
        }
        pthread_mutex_unlock(&mu);

        if (pad.dev && gone) {
            mps_log("pad unplugged");
            pad_close(&pad);
            attached_devno = -1;
            memset(&st, 0, sizeof st);
            send_show(fd, "p 0 0 0 0 0 0 0");
            continue;
        }
        if (take) {
            if (pad_open(conn, &inst, &pad)) last_rx = mps_now_ms();
            else if (pad_attach_error == EBUSY && busy_retries < 20) {
                /* still held by the program before us (Retro's hand-over between emulators): try again shortly */
                ++busy_retries;
                mps_sleep_ms(250);
                pthread_mutex_lock(&mu);
                if (!have_pending) { pending = inst; have_pending = 1; }
                pthread_mutex_unlock(&mu);
            }
            continue;
        }
        now = mps_now_ms();

        /* handshake timers */
        if (pad.dev && pad.kind == XPAD_ONE && !pad.powered && pad.desc_asked_ms && now - pad.desc_asked_ms >= 1000u)
            gip_power(&pad, "no_descriptor_after_1s");
        if (pad.dev && now - last_rx >= 3000u && retries < 3 && pad.inputs < 2) {
            ++retries;
            mps_log("pad quiet ms=%u retry=%u", (unsigned)(now - last_rx), retries);
            last_rx = now;
            if (pad.kind == XPAD_ONE) gip_start_handshake(&pad, "quiet");
        }

        /* A stick change held back by the 15 ms limit below goes out now. The pad reports only changes, so
         * without this the stick's return to centre could be the last report and never be sent: the game
         * kept turning or walking after the stick was let go (car, 2026-11-20). */
        if (pad.dev && memcmp(&st, &sent, sizeof st) && now - last_send >= 15u) {
            char msg[96];
            xpad_format(&st, msg, sizeof msg);
            send_show(fd, msg);
            sent = st;
            last_send = now;
            ++late_sends;
        }

        for (k = 0; k < n; ++k) {
            int slot = slots[k];
            _Uint32t st_code = status[k];
            uint8_t *d = data[k];
            unsigned dl = len[k];
            if ((st_code & USBD_URB_STATUS_MASK) != USBD_STATUS_CMP) {
                ++errors;
                if (errors <= 10) mps_log("pad read_status slot=%d status=%08x len=%u", slot, (unsigned)st_code, dl);
                if ((st_code & USBD_USB_STATUS_MASK) == USBD_STATUS_DEV_NOANSWER ||
                    (st_code & USBD_URB_STATUS_MASK) == USBD_STATUS_ABORTED) continue;
                mps_sleep_ms(20);
                pad_arm(&pad, slot);
                continue;
            }
            pad_arm(&pad, slot);            /* re-arm at once; d[] is our own copy */
            ++reports;
            last_rx = now;

            if (pad.kind == XPAD_ONE) {
                gip_header h;
                char hx[129];
                int ok = gip_parse(d, dl, &h);
                if (ok) ++per_cmd[h.cmd];
                /* every packet that is not plain input, and the first 20 of everything */
                if (logged < 20 || (ok && h.cmd != GIP_INPUT && logged < 80)) {
                    hex(d, dl > 48 ? 48 : dl, hx, sizeof hx);
                    if (ok)
                        mps_log("pad rx cmd=%02x flags=%02x seq=%u len=%u%s chunk=%u bytes=%u hex=%s", h.cmd, h.flags,
                                h.seq, (unsigned)h.len, (h.flags & GIP_F_NEEDS_ACK) ? " ack_wanted" : "",
                                (unsigned)h.chunk, dl, hx);
                    else
                        mps_log("pad rx unparsed bytes=%u hex=%s", dl, hx);
                    ++logged;
                }
                if (!ok) continue;
                if (h.cmd == GIP_AUTH) {
                    ++auth;
                    mps_log("pad auth_packet n=%u (authentication asked for; this probe does not answer it)", auth);
                }
                if (h.flags & GIP_F_CHUNKED) gip_chunk_feed(&pad.chunks, &h);
                if (h.cmd == GIP_DESCRIPTOR && !pad.pdp) {
                    unsigned i;
                    for (i = 0; i + 3 <= dl; ++i)
                        if (d[i] == 'P' && d[i + 1] == 'd' && d[i + 2] == 'p') { pad.pdp = 1; break; }
                    if (pad.pdp) mps_log("pad descriptor_names_pdp (PDP start-up packets will be sent)");
                }
                if (h.cmd == GIP_INPUT) ++pad.inputs;
                if (h.flags & GIP_F_NEEDS_ACK) {
                    uint8_t ack[13];
                    uint32_t received = (h.flags & GIP_F_CHUNKED) ? pad.chunks.received : h.len;
                    uint32_t remaining = (h.flags & GIP_F_CHUNKED) && pad.chunks.total > received
                                             ? pad.chunks.total - received : 0;
                    gip_ack(&h, received, remaining, ack);
                    pad_send(&pad, ack, sizeof ack, "gip_ack");
                    ++acks;
                }
                if (h.cmd == GIP_ARRIVAL) gip_start_handshake(&pad, "arrival");
                if (h.cmd == GIP_DESCRIPTOR && pad.chunks.complete && !pad.powered) {
                    mps_log("pad descriptor_complete bytes=%u", (unsigned)pad.chunks.received);
                    gip_power(&pad, "descriptor_complete");
                }
                if (h.cmd == GIP_DESCRIPTOR && !(h.flags & GIP_F_CHUNKED) && !pad.powered)
                    gip_power(&pad, "descriptor_unchunked");
            } else if (logged < 12 && dl) {
                char hx[129];
                hex(d, dl > 48 ? 48 : dl, hx, sizeof hx);
                mps_log("pad raw len=%u hex=%s", dl, hx);
                ++logged;
            }

            {
                xpad_state before = st;
                if (!xpad_decode(pad.kind, d, dl, &st)) continue;
                if (st.buttons != before.buttons) {
                    unsigned b;
                    char line[200];
                    line[0] = 0;
                    for (b = 0; b < XPAD_BUTTON_COUNT; ++b) {
                        unsigned bit = 1u << b;
                        if ((st.buttons ^ before.buttons) & bit) {
                            strncat(line, button_names[b], sizeof line - strlen(line) - 1);
                            strncat(line, (st.buttons & bit) ? "+ " : "- ", sizeof line - strlen(line) - 1);
                        }
                    }
                    ++changes;
                    mps_log("pad buttons %s", line);
                }
            }
            if (memcmp(&st, &sent, sizeof st) && (st.buttons != sent.buttons || now - last_send >= 15u)) {
                char msg[96];
                xpad_format(&st, msg, sizeof msg);
                send_show(fd, msg);
                sent = st;
                last_send = now;
            }
            if (now - last_summary >= 500u) {
                mps_log("pad sticks lx=%d ly=%d rx=%d ry=%d lt=%u rt=%u", st.lx, st.ly, st.rx, st.ry, st.lt, st.rt);
                last_summary = now;
            }
        }
    }
    if (pad.dev) pad_close(&pad);
    usbd_disconnect(conn);
    if (fd >= 0) close(fd);
    {
        char counts[200];
        unsigned c;
        counts[0] = 0;
        for (c = 0; c < 256; ++c)
            if (per_cmd[c]) {
                char one[24];
                snprintf(one, sizeof one, "%02x:%u ", c, per_cmd[c]);
                strncat(counts, one, sizeof counts - strlen(counts) - 1);
            }
        mps_log("pad end reports=%u button_changes=%u read_errors=%u acks=%u auth_packets=%u quiet_retries=%u "
                "late_sends=%u per_cmd=%s", reports, changes, errors, acks, auth, retries, late_sends,
                counts[0] ? counts : "-");
    }
    return 0;
}

/* Test mode: 20 s (or as asked), feeding the screen test pattern. */
__attribute__((unused)) static int pad_main(unsigned seconds) { return pad_run(seconds, GR_SHOW_PORT, 1); }
