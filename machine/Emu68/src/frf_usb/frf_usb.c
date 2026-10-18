/*
 * FRF USB input layer. USB owns devices on CPU1; callbacks publish generic
 * events and inject hardware-shaped state into the virtual Amiga interfaces.
 */
#include <stdint.h>
#include <string.h>
#include "support.h"
#include "frf_usb.h"
#include "frf_usb_mass.h"
#include "frf_pad_diag.h"
extern volatile struct frf_um_shared *frf_um_host;
#include <uspi/types.h>
#include <uspi.h>

extern void frf_vcia_keyboard_raw(uint8_t raw);
extern void frf_uspi_poll(void);
extern void frf_um_init(void);
extern void frf_um_service(void);
extern void aga_virtual_joy_set(int port, uint16_t joydat, int fire);
extern void aga_virtual_mouse_update(int dx, int dy, unsigned buttons);

#define QN 64u
static struct frf_usb_event q[QN];
static volatile uint32_t q_w, q_r, q_drop;
/* Producer and consumer are CPU1; publish to the Z2 uncached board at 10Hz.
 * Avoid doing slow shared-memory writes in the USB interrupt callback. */
static struct frf_pad_diag pd = { .magic = FRF_PAD_DIAG_MAGIC };
void frf_pad_diag_rx(unsigned success, unsigned len, unsigned id,
                     unsigned valid, const unsigned char *b)
{
    if (!success) { pd.rx_fail++; return; }
    pd.rx_ok++;
    if (!valid) pd.rx_bad++;
    pd.rx_len=len; pd.rx_id=id;
    uint32_t a=0,bv=0;
    for (unsigned i=0;i<4;i++) a=(a<<8) | ((b && len>i) ? b[i] : 0u);
    for (unsigned i=4;i<8;i++) bv=(bv<<8) | ((b && len>i) ? b[i] : 0u);
    pd.sample_a=a; pd.sample_b=bv;
    if (valid) pd.reports_decoded++;
}
void frf_pad_diag_storage(unsigned cmd,unsigned lba,int rc)
{
    if (cmd == FRF_UM_READ) pd.storage_reads++;
    if (cmd == FRF_UM_WRITE) {
        pd.storage_writes++;
        if (rc < 0) pd.storage_write_fail++;
    }
    pd.storage_last_cmd=cmd;
    pd.storage_last_lba=lba;
    pd.storage_last_result=(uint32_t)rc;
}
static void frf_pad_diag_publish(void)
{
    volatile struct frf_um_shared *s=
        __atomic_load_n(&frf_um_host,__ATOMIC_ACQUIRE);
    if (!s || s->magic!=FRF_UM_MAGIC) return;
    volatile uint32_t *d=(volatile uint32_t*)((volatile uint8_t*)s+FRF_PAD_DIAG_OFF);
    const uint32_t *v=(const uint32_t*)&pd;
    /* Publish magic last so a reader never accepts an uninitialized record. */
    for (unsigned i=1;i<sizeof(pd)/4;i++) d[i]=v[i];
    __atomic_thread_fence(__ATOMIC_RELEASE);
    d[0]=FRF_PAD_DIAG_MAGIC;
}


static inline uint64_t nowtick(void)
{
    uint64_t v;
    __asm__ volatile("mrs %0, CNTVCT_EL0" : "=r"(v));
    return v;
}

static void push(uint32_t type, uint32_t index,
                 uint32_t a, uint32_t b, uint32_t c, uint32_t d)
{
    uint32_t w = q_w;
    uint32_t n = (w + 1u) & (QN - 1u);
    if (n == q_r) {
        q_r = (q_r + 1u) & (QN - 1u);
        q_drop++;
    }
    q[w] = (struct frf_usb_event){ type, index, a, b, c, d, nowtick() };
    __asm__ volatile("dmb ish" ::: "memory");
    q_w = n;
}

int frf_usb_event_pop(struct frf_usb_event *out)
{
    if (!out) return 0;
    uint32_t r = q_r;
    if (r == q_w) return 0;
    __asm__ volatile("dmb ish" ::: "memory");
    *out = q[r];
    q_r = (r + 1u) & (QN - 1u);
    return 1;
}

/* ------------------------------------------------------------------------- */
/* Keyboard: USB HID usage -> Amiga raw key                                  */
/* ------------------------------------------------------------------------- */

static uint8_t last_mod, last_keys[6];
static volatile uint8_t lock_led_mask;
static volatile uint8_t lock_led_pending;
static volatile uint8_t host_reboot_pending;
/* F12 hotkey is host-only, captured before 68k starts. */
static volatile uint8_t frf_early_requested;
static volatile uint8_t frf_guest_started;
static volatile uint64_t frf_early_deadline;
static inline uint64_t frf_early_now(void) { uint64_t t; __asm__ volatile("mrs %0, CNTVCT_EL0":"=r"(t)); return t; }
static inline uint64_t frf_early_freq(void) { uint64_t t; __asm__ volatile("mrs %0, CNTFRQ_EL0":"=r"(t)); return t; }
void frf_usb_guest_start(void)
{
    if (frf_early_requested) {
        frf_early_deadline = frf_early_now() + frf_early_freq() * 20u;
        kprintf("[USB] Early Startup: virtual left+right mouse buttons for 20 sec\n");
    }
    frf_guest_started = 1u;
}
int frf_usb_early_startup_active(void)
{
    uint64_t deadline = __atomic_load_n(&frf_early_deadline, __ATOMIC_ACQUIRE);
    return deadline && (int64_t)(deadline - frf_early_now()) > 0;
}


/* Boot-time mouse state published to CPU0 before Kickstart starts. */
/* CPU0 authorizes USB startup only AFTER platform + ROM initialization.
 * This prevents concurrent GPU mailbox accesses during main boot init. */
volatile uint8_t frf_usb_host_start_allowed;
volatile uint8_t frf_usb_boot_ready;
volatile uint8_t frf_usb_mouse_present;
volatile uint8_t frf_usb_mouse_seen;
volatile uint8_t frf_usb_mouse_buttons;

static uint8_t hid_to_amiga(uint8_t h)
{
    if (h >= 0x04 && h <= 0x1d) {
        static const uint8_t alpha[26] = {
            0x20,0x35,0x33,0x22,0x12,0x23,0x24,0x25,0x17,0x26,0x27,0x28,0x37,
            0x36,0x18,0x19,0x10,0x13,0x21,0x14,0x16,0x34,0x11,0x32,0x15,0x31
        };
        return alpha[h - 0x04];
    }

    if (h >= 0x1e && h <= 0x26)
        return (uint8_t)(h - 0x1d);               /* 1..9 */

    switch (h) {
    case 0x27: return 0x0a;                       /* 0 */
    case 0x28: return 0x44;                       /* Return */
    case 0x29: return 0x45;                       /* Escape */
    case 0x2a: return 0x41;                       /* Backspace */
    case 0x2b: return 0x42;                       /* Tab */
    case 0x2c: return 0x40;                       /* Space */
    case 0x2d: return 0x0b;                       /* - */
    case 0x2e: return 0x0c;                       /* = */
    case 0x2f: return 0x1a;                       /* [ */
    case 0x30: return 0x1b;                       /* ] */
    case 0x31: return 0x0d;                       /* backslash */
    case 0x32: return 0x30;                       /* ISO/non-US */
    case 0x33: return 0x29;                       /* ; */
    case 0x34: return 0x2a;                       /* apostrophe */
    case 0x35: return 0x00;                       /* grave */
    case 0x36: return 0x38;                       /* , */
    case 0x37: return 0x39;                       /* . */
    case 0x38: return 0x3a;                       /* / */
    case 0x39: return 0x62;                       /* Caps Lock */

    case 0x3a: return 0x50; case 0x3b: return 0x51;
    case 0x3c: return 0x52; case 0x3d: return 0x53;
    case 0x3e: return 0x54; case 0x3f: return 0x55;
    case 0x40: return 0x56; case 0x41: return 0x57;
    case 0x42: return 0x58; case 0x43: return 0x59; /* F1..F10 */

    case 0x49: return 0x5f;                       /* Insert -> Help */
    case 0x4c: return 0x46;                       /* Delete */
    case 0x4f: return 0x4e;                       /* Right */
    case 0x50: return 0x4f;                       /* Left */
    case 0x51: return 0x4d;                       /* Down */
    case 0x52: return 0x4c;                       /* Up */

    case 0x54: return 0x5c;                       /* KP / */
    case 0x55: return 0x5d;                       /* KP * */
    case 0x56: return 0x4a;                       /* KP - */
    case 0x57: return 0x5e;                       /* KP + */
    case 0x58: return 0x43;                       /* KP Enter */
    case 0x59: return 0x1d; case 0x5a: return 0x1e; case 0x5b: return 0x1f;
    case 0x5c: return 0x2d; case 0x5d: return 0x2e; case 0x5e: return 0x2f;
    case 0x5f: return 0x3d; case 0x60: return 0x3e; case 0x61: return 0x3f;
    case 0x62: return 0x0f;                       /* KP 0 */
    case 0x63: return 0x3c;                       /* KP . */
    default:   return 0xff;
    }
}

static int key_in(const uint8_t a[6], uint8_t k)
{
    for (int i = 0; i < 6; ++i)
        if (a[i] == k) return 1;
    return 0;
}

static void emit_key(uint8_t raw, int up)
{
    if (raw != 0xff)
        frf_vcia_keyboard_raw((uint8_t)(raw | (up ? 0x80u : 0u)));
}

static void key_raw(unsigned char mod, const unsigned char keys[6])
{
    /* USB modifier bits: LCtrl,LShift,LAlt,LGUI,RCtrl,RShift,RAlt,RGUI. */
    static const uint8_t modraw[8] = {
        0x63, 0x60, 0x64, 0x66, 0x63, 0x61, 0x65, 0x67
    };

    uint8_t changed = (uint8_t)(mod ^ last_mod);
    for (int b = 0; b < 8; ++b)
        if (changed & (1u << b))
            emit_key(modraw[b], !(mod & (1u << b)));

    for (int i = 0; i < 6; ++i) {
        uint8_t k = last_keys[i];
        if (k < 4 || key_in(keys, k)) continue;

        /* USB Num/Scroll Lock are host keyboard concepts; classic Amiga has no
           corresponding raw keys. Caps Lock is a real Amiga key ($62). */
        if (k == 0x53 || k == 0x47) continue;
        emit_key(hid_to_amiga(k), 1);
    }

    for (int i = 0; i < 6; ++i) {
        uint8_t k = keys[i];
        if (k < 4 || key_in(last_keys, k)) continue;

        /* Ctrl+Shift+F12 during boot: hold both virtual mouse buttons
           through Kickstart's Early Startup sampling window. */
        if (k == 0x45 && !frf_guest_started &&
            (mod & ((1u << 0) | (1u << 4))) &&
            (mod & ((1u << 1) | (1u << 5)))) {
            frf_early_requested = 1u;
            continue;
        }
                /* Host-only hard reboot. This is deliberately NOT an Amiga raw key
           and does not change guest RESET semantics. */
        if (k == 0x44 &&
            (mod & ((1u << 0) | (1u << 4))) &&
            (mod & ((1u << 1) | (1u << 5)))) {
            host_reboot_pending = 1;
            continue;
        }

        if (k == 0x39) {
            lock_led_mask ^= LED_CAPS_LOCK;
            lock_led_pending = 1;
            emit_key(0x62, 0);
            continue;
        }
        if (k == 0x53) {
            lock_led_mask ^= LED_NUM_LOCK;
            lock_led_pending = 1;
            continue;
        }
        if (k == 0x47) {
            lock_led_mask ^= LED_SCROLL_LOCK;
            lock_led_pending = 1;
            continue;
        }

        emit_key(hid_to_amiga(k), 0);
    }

    last_mod = mod;
    memcpy(last_keys, keys, sizeof last_keys);

    uint32_t k0 = (uint32_t)keys[0] | ((uint32_t)keys[1] << 8)
                | ((uint32_t)keys[2] << 16) | ((uint32_t)keys[3] << 24);
    uint32_t k1 = (uint32_t)keys[4] | ((uint32_t)keys[5] << 8);
    push(FRF_USB_EV_KEYBOARD, 0, mod, k0, k1, 0);
}

/* ------------------------------------------------------------------------- */
/* Mouse: JOY0DAT quadrature counters + CIA-A left button + POTGOR right     */
/* ------------------------------------------------------------------------- */

static void mouse_status(unsigned buttons, int dx, int dy)
{
    aga_virtual_mouse_update(dx, dy, buttons);
    frf_usb_mouse_buttons = (uint8_t)buttons;
    frf_usb_mouse_seen = 1;
    __asm__ volatile("dmb ish" ::: "memory");
    push(FRF_USB_EV_MOUSE, 0, buttons, (uint32_t)dx, (uint32_t)dy, 0);
}

/* ------------------------------------------------------------------------- */
/* Gamepad 0 -> Amiga joystick port 2 (JOY1DAT + CIA-A fire)                 */
/* ------------------------------------------------------------------------- */

static int axis_dir(const USPiGamePadState *s, int i)
{
    if (!s || i >= s->naxes) return 0;
    int mn = s->axes[i].minimum;
    int mx = s->axes[i].maximum;
    int v  = s->axes[i].value;
    if (mx <= mn) return 0;

    int mid = mn + (mx - mn) / 2;
    int dead = (mx - mn) / 5;                     /* 20% dead zone */
    return v < mid - dead ? -1 : v > mid + dead ? 1 : 0;
}

static void pad_status(unsigned index, const USPiGamePadState *s)
{
    if (!s) return;

    int x = axis_dir(s, 0);
    int y = axis_dir(s, 1);
    /* Many generic USB controllers expose the D-pad on axes 2/3
       instead of a HID hat. Accept either representation. */
    int dx = axis_dir(s, 2);
    int dy = axis_dir(s, 3);
    int up = 0, down = 0, left = 0, right = 0;

    /* Standard HID hat: 0..7 clockwise from up, >7 neutral. */
    if (s->nhats > 0 && (unsigned)s->hats[0] < 8u) {
        int h = s->hats[0];
        up    = h == 0 || h == 1 || h == 7;
        right = h == 1 || h == 2 || h == 3;
        down  = h == 3 || h == 4 || h == 5;
        left  = h == 5 || h == 6 || h == 7;
    }

    left  |= x < 0 || dx < 0;
    right |= x > 0 || dx > 0;
    up    |= y < 0 || dy < 0;
    down  |= y > 0 || dy > 0;

    /* JOYxDAT quadrature:
       left=Y1, up=Y1 xor Y0, right=X1, down=X1 xor X0. */
    uint16_t joy = 0;
    if (left)  joy |= 0x0200u;
    if (right) joy |= 0x0002u;
    if ((left != 0) ^ (up != 0))    joy |= 0x0100u;
    if ((right != 0) ^ (down != 0)) joy |= 0x0001u;

    if (index == 0) {
        static uint16_t prior_joy=0xffff;
        static uint32_t prior_buttons=~0u;
        static unsigned transitions=0;
        uint32_t fire=s->buttons & 0x0fu;
        if ((joy!=prior_joy || fire!=prior_buttons) && transitions++<32u)
            kprintf("[PAD-RC4] decoded gamepad transition joy1=%04x buttons=%08x hat=%d axes=%d/%d\n",
                    joy, s->buttons, s->nhats?s->hats[0]:-1, s->axes[0].value, s->axes[1].value);
        prior_joy=joy;
        prior_buttons=fire;
        aga_virtual_joy_set(1, joy, fire != 0);
        pd.pad_buttons=s->buttons;
        pd.pad_hat=s->nhats ? (uint32_t)s->hats[0] : 0xffffffffu;
        pd.pad_axis01=((uint32_t)(uint16_t)s->axes[0].value << 16)
                    | (uint16_t)s->axes[1].value;
        pd.joy1dat=joy;
        pd.fire=fire!=0;
        pd.updates++;
    }

    uint32_t axis01 = 0, hats = 0;
    if (s->naxes > 0) axis01 = (uint16_t)s->axes[0].value;
    if (s->naxes > 1) axis01 |= (uint32_t)(uint16_t)s->axes[1].value << 16;
    if (s->nhats > 0) hats = (uint32_t)s->hats[0];

    push(FRF_USB_EV_GAMEPAD, index, s->buttons, axis01, hats,
         ((uint32_t)s->naxes << 16) | (uint32_t)s->nbuttons);
}

void frf_usb_main(void)
{
    extern void frf_boot_stage(unsigned stage);
    /* CPU1 started by firmware before CPU0 completes platform init.
     * Wait until CPU0 finishes firmware mailbox and ROM setup. */
    while (!__atomic_load_n(&frf_usb_host_start_allowed, __ATOMIC_ACQUIRE))
        __asm__ volatile("wfe");
    frf_boot_stage(6);
    kprintf("[USB] FRF v0.10.15 COHERENT-MMU: v0.9.3 keyboard base + real wire timing\n");

    frf_boot_stage(7);
    if (!frf_uspi_env_init()) {
        kprintf("[USB] environment init failed; CPU1 parked\n");
        for (;;) __asm__ volatile("wfe");
    }

    frf_boot_stage(8);
    int ok = USPiInitialize();
    if (!ok) {
        kprintf("[USB] USPiInitialize FAILED; Amiga continues without USB input\n");
        for (;;) __asm__ volatile("wfe");
    }

    frf_um_init();
    frf_boot_stage(9);
    int kb = USPiKeyboardAvailable();
    int ms = USPiMouseAvailable();
    int gp = USPiGamePadAvailable();
    pd.receivers=(uint32_t)gp;

    kprintf("[USB] enumeration complete: keyboard=%d mouse=%d gamepads=%d\n",
            kb, ms, gp);
    kprintf("[MOUSE-DIAG] USB boot mouse present=%d; callback=%s; no receiver required\n",
            ms, ms ? "install" : "MISSING");

    if (kb) {
        lock_led_mask = 0;
        lock_led_pending = 1;
        USPiKeyboardRegisterKeyStatusHandlerRaw(key_raw);
    }
    if (ms) {
        frf_usb_mouse_present = 1;
        USPiMouseRegisterStatusHandler(mouse_status);
    }
    if (gp > 0) USPiGamePadRegisterStatusHandler(pad_status);

    /* Give the interrupt endpoint 100 ms to publish held mouse buttons before
       CPU0 lets Kickstart sample Early Startup Control. */
    {
        uint64_t f, t0, now;
        __asm__ volatile("mrs %0, CNTFRQ_EL0" : "=r"(f));
        __asm__ volatile("mrs %0, CNTVCT_EL0" : "=r"(t0));
        do {
            frf_uspi_poll();
            frf_uspi_timer_service();
            __asm__ volatile("yield");
            __asm__ volatile("mrs %0, CNTVCT_EL0" : "=r"(now));
        } while ((now - t0) < f / 10u);
    }
    __asm__ volatile("dmb ish" ::: "memory");
    frf_boot_stage(10);
    frf_usb_boot_ready = 1;
    __asm__ volatile("sev" ::: "memory");

    kprintf("[USB] Amiga input active: keyboard=CIA-A mouse=port1 gamepad0=port2 boot-buttons=%02x\n",
            (unsigned)frf_usb_mouse_buttons);

    uint64_t pad_diag_next=nowtick();
    for (;;) {
        frf_uspi_poll();
        frf_uspi_timer_service();
        USPiGamePadService();
        frf_um_service();
        uint64_t diag_now=nowtick();
        if ((int64_t)(diag_now-pad_diag_next)>=0) {
            uint64_t hz;
            __asm__ volatile("mrs %0, CNTFRQ_EL0" : "=r"(hz));
            pad_diag_next=diag_now+hz/10u;
            frf_pad_diag_publish();
        }

        if (kb && lock_led_pending) {
            uint8_t leds = lock_led_mask;
            lock_led_pending = 0;
            USPiKeyboardSetLEDs(leds);
        }

        if (host_reboot_pending) {
            extern void aga_reboot(int tryboot);
            kprintf("[USB] Ctrl+Shift+F11: preserving diagnostics and rebooting Pi\n");
            aga_reboot(0);
            __builtin_unreachable();
        }

        for (unsigned i = 0; i < 64; ++i)
            __asm__ volatile("yield");
    }
}
