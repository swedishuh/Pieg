#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#include "usb.h"
#include "mem.h"
#include "pci.h"

extern void vga_write(const char *text);
extern uint64_t pd_table[];

/* ---- EHCI registers ---- */
#define OP_CMD     0x00
#define OP_STS     0x04
#define OP_INTR    0x08
#define OP_DSSEG   0x10
#define OP_ASYNC   0x18
#define OP_CFG     0x40
#define OP_PORT(n) (0x44u + 4u * (n))

#define CMD_RS     (1u << 0)
#define CMD_HCRST  (1u << 1)
#define CMD_ASE    (1u << 5)
#define STS_HCH    (1u << 12)
#define STS_ASS    (1u << 15)

#define PORT_CCS   (1u << 0)
#define PORT_CSC   (1u << 1)
#define PORT_PED   (1u << 2)
#define PORT_PEC   (1u << 3)
#define PORT_OCC   (1u << 5)
#define PORT_RESET (1u << 8)
#define PORT_PP    (1u << 12)
#define PORT_OWNER (1u << 13)
#define PORT_RWC   (PORT_CSC | PORT_PEC | PORT_OCC)

#define BOUNCE_SECTORS 32u

/* ---- DMA structures (64-bit capable layouts, zeroed upper pointers) ---- */
typedef struct {
    uint32_t next, alt, token;
    uint32_t buf[5], ebuf[5];
    uint32_t pad[3];
} __attribute__((aligned(64))) qtd_t;

typedef struct {
    uint32_t link, epchar, epcap, cur;
    uint32_t onext, oalt, otoken;
    uint32_t obuf[5], oebuf[5];
    uint32_t pad[7];
} __attribute__((aligned(64))) qh_t;

typedef struct {
    uint8_t addr, in_ep, out_ep, iface;
    uint16_t mps;
    uint8_t tog[2];             /* [0] OUT, [1] IN */
    uint8_t tag_seq;
    uint32_t tag;
    blockdev_t dev;
    char name[8];
} usbdev_t;

static volatile uint8_t *op;
static qh_t qh;
static qtd_t qtd[3];
static uint8_t setup_buf[256] __attribute__((aligned(256)));
static uint8_t ctrl_buf[256] __attribute__((aligned(256)));
static uint8_t cbw_buf[64] __attribute__((aligned(64)));
static uint8_t csw_buf[64] __attribute__((aligned(64)));
static uint8_t bounce[BOUNCE_SECTORS * 512u] __attribute__((aligned(4096)));

static usbdev_t devs[USB_MAX_DISKS];
static unsigned int dev_total;

/* One HID boot-protocol mouse (interrupt IN, polled through the async list). */
static struct {
    bool present;
    uint8_t addr, ep;
    uint16_t mps;
    uint8_t tog;
    uint8_t errors;
} hid;
static uint8_t hid_buf[64] __attribute__((aligned(64)));
static unsigned int next_addr = 1;

/* ================================================================
 * Helpers
 * ================================================================ */

static inline void outb(uint16_t port, uint8_t v)
{
    __asm__ volatile ("outb %0, %1" : : "a"(v), "Nd"(port));
}

/* Port 0x80 writes take ~1 us on real hardware. */
static void udelay(uint32_t us) { while (us--) outb(0x80, 0); }
static void mdelay(uint32_t ms) { udelay(ms * 1000u); }

static inline uint32_t orl(uint32_t off)
{
    return *(volatile uint32_t *)(op + off);
}

static inline void owl(uint32_t off, uint32_t v)
{
    *(volatile uint32_t *)(op + off) = v;
}

static inline uint32_t phys(const void *p) { return (uint32_t)(uintptr_t)p; }

static bool wait_sts(uint32_t mask, bool set, uint32_t ms)
{
    for (uint32_t i = 0; i < ms * 10u; i++) {
        if (((orl(OP_STS) & mask) != 0) == set)
            return true;
        udelay(100);
    }
    return false;
}

static void put_u32(uint32_t v)
{
    char t[11];
    unsigned int n = 0;

    if (!v) { vga_write("0"); return; }
    while (v) { t[n++] = (char)('0' + v % 10); v /= 10; }
    while (n) { char c[2] = { t[--n], 0 }; vga_write(c); }
}

static void map_uncached(uint32_t phys_addr, uint32_t size)
{
    uint32_t first = phys_addr >> 21;
    uint32_t last = (phys_addr + size - 1u) >> 21;

    for (uint32_t i = first; i <= last && i < 2048; i++)
        pd_table[i] |= (1ull << 3) | (1ull << 4);

    uint64_t cr3;
    __asm__ volatile ("mov %%cr3, %0" : "=r"(cr3));
    __asm__ volatile ("mov %0, %%cr3" : : "r"(cr3) : "memory");
}

/* ================================================================
 * Transfers: one QH reused for everything, async schedule toggled
 * around each transfer so the HC never sees a half-built queue.
 * ================================================================ */

static void async_set(bool on)
{
    uint32_t c = orl(OP_CMD);

    owl(OP_CMD, on ? (c | CMD_ASE) : (c & ~CMD_ASE));
    wait_sts(STS_ASS, on, 20);
}

static void qtd_init(qtd_t *t, uint8_t pid, unsigned int dt, void *buf,
                     uint32_t len)
{
    uint32_t a = phys(buf);

    memset(t, 0, sizeof(*t));
    t->next = 1;
    t->alt = 1;
    t->token = ((uint32_t)dt << 31) | (len << 16) | (3u << 10) |
               ((uint32_t)pid << 8) | 0x80u;
    t->buf[0] = a;
    for (unsigned int i = 1; i < 5; i++)
        t->buf[i] = (a & ~0xFFFu) + i * 0x1000u;
}

/* 0 = ok, 1 = STALL, -1 = error/timeout */
static int xfer(uint8_t addr, uint8_t ep, uint16_t mps, unsigned int n,
                uint32_t timeout_ms)
{
    for (unsigned int i = 0; i + 1 < n; i++)
        qtd[i].next = phys(&qtd[i + 1]);

    async_set(false);

    qh.link = phys(&qh) | 2u;
    qh.epchar = addr | ((uint32_t)ep << 8) | (2u << 12) | (1u << 14) |
                (1u << 15) | ((uint32_t)mps << 16);
    qh.epcap = 1u << 30;
    qh.cur = 0;
    qh.oalt = 1;
    qh.otoken = 0;
    qh.onext = phys(&qtd[0]);
    __asm__ volatile ("" : : : "memory");

    owl(OP_STS, 0x3Fu);
    async_set(true);

    int result = -1;

    for (uint32_t t = 0; t < timeout_ms * 10u; t++) {
        bool active = false;
        bool bad = false;
        bool stall = false;

        for (unsigned int i = 0; i < n; i++) {
            uint32_t tok = *(volatile uint32_t *)&qtd[i].token;

            if (tok & 0x40u) {
                if (tok & 0x38u)
                    bad = true;
                else
                    stall = true;
                break;
            }
            if (tok & 0x80u) { active = true; break; }
        }

        if (bad) { result = -1; goto done; }
        if (stall) { result = 1; goto done; }
        if (!active) { result = 0; goto done; }
        udelay(100);
    }

done:
    async_set(false);
    return result;
}

static int control(uint8_t addr, uint8_t bm, uint8_t req, uint16_t val,
                   uint16_t idx, uint16_t len)
{
    unsigned int n = 0;
    bool in = (bm & 0x80u) != 0;

    setup_buf[0] = bm;
    setup_buf[1] = req;
    setup_buf[2] = (uint8_t)val;
    setup_buf[3] = (uint8_t)(val >> 8);
    setup_buf[4] = (uint8_t)idx;
    setup_buf[5] = (uint8_t)(idx >> 8);
    setup_buf[6] = (uint8_t)len;
    setup_buf[7] = (uint8_t)(len >> 8);

    qtd_init(&qtd[n++], 2, 0, setup_buf, 8);

    if (len)
        qtd_init(&qtd[n++], in ? 1 : 0, 1, ctrl_buf, len);

    qtd_init(&qtd[n++], (len && in) ? 0 : 1, 1, ctrl_buf, 0);

    return xfer(addr, 0, 64, n, 1000);
}

static int bulk(usbdev_t *d, uint8_t ep, bool in, void *buf, uint32_t len)
{
    qtd_init(&qtd[0], in ? 1 : 0, d->tog[in], buf, len);

    int r = xfer(d->addr, ep, d->mps, 1, 5000);

    if (r == 0)
        d->tog[in] ^= (uint8_t)(((len + d->mps - 1u) / d->mps) & 1u);

    return r;
}

/* ================================================================
 * Bulk-Only Transport
 * ================================================================ */

static void clear_halt(usbdev_t *d, uint8_t ep, bool in)
{
    control(d->addr, 0x02, 1, 0, (uint16_t)(ep | (in ? 0x80u : 0)), 0);
    d->tog[in] = 0;
}

static void recover(usbdev_t *d)
{
    control(d->addr, 0x21, 0xFF, 0, d->iface, 0);
    clear_halt(d, d->in_ep, true);
    clear_halt(d, d->out_ep, false);
}

/* 0 = good, 1 = command failed, -1 = transport error */
static int bot(usbdev_t *d, const uint8_t *cb, uint8_t cblen, void *data,
               uint32_t len, bool in)
{
    memset(cbw_buf, 0, 31);
    d->tag++;
    cbw_buf[0] = 'U'; cbw_buf[1] = 'S'; cbw_buf[2] = 'B'; cbw_buf[3] = 'C';
    for (unsigned int i = 0; i < 4; i++) {
        cbw_buf[4 + i] = (uint8_t)(d->tag >> (8 * i));
        cbw_buf[8 + i] = (uint8_t)(len >> (8 * i));
    }
    cbw_buf[12] = in ? 0x80 : 0;
    cbw_buf[14] = cblen;
    memcpy(&cbw_buf[15], cb, cblen);

    if (bulk(d, d->out_ep, false, cbw_buf, 31) != 0) {
        recover(d);
        return -1;
    }

    if (len) {
        int r = bulk(d, in ? d->in_ep : d->out_ep, in, data, len);

        if (r == 1)
            clear_halt(d, in ? d->in_ep : d->out_ep, in);
        else if (r < 0) {
            recover(d);
            return -1;
        }
    }

    int r = bulk(d, d->in_ep, true, csw_buf, 13);

    if (r == 1) {
        clear_halt(d, d->in_ep, true);
        r = bulk(d, d->in_ep, true, csw_buf, 13);
    }

    if (r != 0) {
        recover(d);
        return -1;
    }

    uint32_t tag = csw_buf[4] | (csw_buf[5] << 8) | (csw_buf[6] << 16) |
                   ((uint32_t)csw_buf[7] << 24);

    if (csw_buf[0] != 'U' || csw_buf[1] != 'S' || csw_buf[2] != 'B' ||
        csw_buf[3] != 'S' || tag != d->tag) {
        recover(d);
        return -1;
    }

    return csw_buf[12] == 0 ? 0 : 1;
}

static bool usb_rw(blockdev_t *dev, uint64_t lba, uint32_t count,
                   void *buf, bool write)
{
    usbdev_t *d = dev->driver_data;
    uint8_t *p = buf;

    while (count) {
        uint32_t n = count < BOUNCE_SECTORS ? count : BOUNCE_SECTORS;
        uint8_t cb[10] = { write ? 0x2A : 0x28, 0,
                           (uint8_t)(lba >> 24), (uint8_t)(lba >> 16),
                           (uint8_t)(lba >> 8), (uint8_t)lba, 0,
                           (uint8_t)(n >> 8), (uint8_t)n, 0 };

        if (lba >> 32)
            return false;

        if (write)
            memcpy(bounce, p, n * 512u);

        int r = bot(d, cb, 10, bounce, n * 512u, !write);

        if (r != 0)
            r = bot(d, cb, 10, bounce, n * 512u, !write);   /* one retry */

        if (r != 0)
            return false;

        if (!write)
            memcpy(p, bounce, n * 512u);

        p += n * 512u;
        lba += n;
        count -= n;
    }

    return true;
}

static bool usb_read(blockdev_t *dev, uint64_t lba, uint32_t count, void *buf)
{
    return usb_rw(dev, lba, count, buf, false);
}

static bool usb_write(blockdev_t *dev, uint64_t lba, uint32_t count,
                      const void *buf)
{
    return usb_rw(dev, lba, count, (void *)buf, true);
}

/* ================================================================
 * Device enumeration
 * ================================================================ */

static void add_storage(usbdev_t *d)
{
    uint8_t tur[6] = { 0 };
    uint8_t sense[6] = { 0x03, 0, 0, 0, 18, 0 };
    uint8_t cap[10] = { 0x25 };
    bool ready = false;

    for (unsigned int i = 0; i < 30 && !ready; i++) {
        if (bot(d, tur, 6, 0, 0, true) == 0) {
            ready = true;
            break;
        }
        bot(d, sense, 6, bounce, 18, true);
        mdelay(100);
    }

    if (!ready || bot(d, cap, 10, bounce, 8, true) != 0) {
        vga_write("[USB] mass storage device not ready, skipped\n");
        return;
    }

    uint32_t last = ((uint32_t)bounce[0] << 24) | (bounce[1] << 16) |
                    (bounce[2] << 8) | bounce[3];
    uint32_t bsz = ((uint32_t)bounce[4] << 24) | (bounce[5] << 16) |
                   (bounce[6] << 8) | bounce[7];

    if (bsz != 512) {
        vga_write("[USB] unsupported sector size, skipped\n");
        return;
    }

    d->name[0] = 'u'; d->name[1] = 's'; d->name[2] = 'b';
    d->name[3] = (char)('0' + dev_total);
    d->name[4] = 0;

    d->dev.name = d->name;
    d->dev.sector_count = (uint64_t)last + 1u;
    d->dev.sector_size = 512;
    d->dev.driver_data = d;
    d->dev.read = usb_read;
    d->dev.write = usb_write;

    vga_write("[USB] ");
    vga_write(d->name);
    vga_write(": ");
    put_u32((uint32_t)(d->dev.sector_count / 2048u));
    vga_write(" MiB mass storage\n");

    dev_total++;
}

static void add_hid_mouse(uint8_t addr, uint8_t cfg, uint8_t iface,
                          uint8_t ep, uint16_t mps)
{
    if (control(addr, 0x00, 9, cfg, 0, 0) != 0)
        return;

    /* Class requests, both optional: boot protocol, report only on change. */
    control(addr, 0x21, 0x0B, 0, iface, 0);
    control(addr, 0x21, 0x0A, 0, iface, 0);

    hid.addr = addr;
    hid.ep = ep;
    hid.mps = mps;
    hid.tog = 0;
    hid.errors = 0;
    hid.present = true;

    vga_write("[USB] HID boot mouse\n");
}

static void enumerate(uint8_t addr)
{
    usbdev_t *d = &devs[dev_total];

    memset(d, 0, sizeof(*d));

    /* Device descriptor at address 0, then assign the address. */
    if (control(0, 0x80, 6, 0x0100, 0, 18) != 0)
        return;
    if (control(0, 0x00, 5, addr, 0, 0) != 0)
        return;
    mdelay(10);

    if (control(addr, 0x80, 6, 0x0200, 0, 9) != 0)
        return;

    uint16_t total = (uint16_t)(ctrl_buf[2] | (ctrl_buf[3] << 8));
    uint8_t cfg = ctrl_buf[5];

    if (total > 255)
        total = 255;
    if (control(addr, 0x80, 6, 0x0200, 0, total) != 0)
        return;

    bool match = false, found = false;
    bool hid_match = false, hid_found = false;
    uint8_t h_iface = 0, h_ep = 0;
    uint16_t h_mps = 0;

    for (unsigned int o = 0; o + 2 <= total && ctrl_buf[o];
         o += ctrl_buf[o]) {
        uint8_t *p = &ctrl_buf[o];

        if (p[1] == 4 && o + 9 <= total) {
            match = !found && p[5] == 8 && p[6] == 6 && p[7] == 0x50;
            if (match)
                d->iface = p[2];

            /* HID, boot interface subclass, mouse protocol. */
            hid_match = !hid_found && p[5] == 3 && p[6] == 1 && p[7] == 2;
            if (hid_match)
                h_iface = p[2];
        } else if (p[1] == 5 && match && o + 7 <= total && (p[3] & 3) == 2) {
            if (p[2] & 0x80)
                d->in_ep = p[2] & 0xF;
            else
                d->out_ep = p[2] & 0xF;
            d->mps = (uint16_t)((p[4] | (p[5] << 8)) & 0x7FF);
            if (d->in_ep && d->out_ep)
                found = true;
        } else if (p[1] == 5 && hid_match && o + 7 <= total &&
                   (p[3] & 3) == 3 && (p[2] & 0x80)) {
            h_ep = p[2] & 0xF;
            h_mps = (uint16_t)((p[4] | (p[5] << 8)) & 0x7FF);
            hid_found = h_ep && h_mps;
            hid_match = false;
        }
    }

    if (!found && hid_found && !hid.present && h_mps <= sizeof(hid_buf)) {
        add_hid_mouse(addr, cfg, h_iface, h_ep, h_mps);
        return;
    }

    if (!found || !d->mps)
        return;

    d->addr = addr;

    if (control(addr, 0x00, 9, cfg, 0, 0) != 0)
        return;

    add_storage(d);
}

static void init_port(unsigned int n)
{
    uint32_t sc = orl(OP_PORT(n));

    if (!(sc & PORT_CCS))
        return;

    /* K-state: low-speed device, give it to the companion controller. */
    if (((sc >> 10) & 3u) == 1) {
        owl(OP_PORT(n), (sc & ~PORT_RWC) | PORT_OWNER);
        return;
    }

    owl(OP_PORT(n), (sc & ~(PORT_PED | PORT_RWC)) | PORT_RESET);
    mdelay(50);
    sc = orl(OP_PORT(n));
    owl(OP_PORT(n), sc & ~(PORT_RESET | PORT_RWC));

    for (unsigned int i = 0; i < 20 && (orl(OP_PORT(n)) & PORT_RESET); i++)
        mdelay(1);
    mdelay(10);

    sc = orl(OP_PORT(n));
    owl(OP_PORT(n), sc | PORT_RWC);   /* ack change bits, keep enabled */

    if (!(sc & PORT_PED)) {
        /* Full-speed device: not ours. */
        owl(OP_PORT(n), (sc & ~PORT_RWC) | PORT_OWNER);
        return;
    }

    if (dev_total < USB_MAX_DISKS && next_addr < 100)
        enumerate((uint8_t)next_addr++);
}

static void init_controller(const pci_device_t *pd)
{
    uint32_t bar0 = pd->bar[0];

    if ((bar0 & 1u) || (bar0 & ~0xFu) == 0 ||
        ((bar0 & 6u) == 4u && pd->bar[1] != 0)) {
        vga_write("[USB] EHCI without a usable BAR0, skipped\n");
        return;
    }

    uint32_t base = bar0 & ~0xFu;
    uint32_t cmd = pci_config_read32(pd->bus, pd->device, pd->function, 4);

    pci_config_write32(pd->bus, pd->device, pd->function, 4,
                       (cmd & 0xFFFFu) | 0x6u);
    map_uncached(base, 0x1000u);

    volatile uint8_t *cap = (volatile uint8_t *)(uintptr_t)base;
    uint32_t hcs = *(volatile uint32_t *)(cap + 4);
    uint32_t hcc = *(volatile uint32_t *)(cap + 8);

    op = cap + cap[0];

    /* BIOS -> OS ownership handoff (USB legacy support capability). */
    for (uint32_t e = (hcc >> 8) & 0xFFu; e >= 0x40; ) {
        uint32_t v = pci_config_read32(pd->bus, pd->device, pd->function,
                                       (uint8_t)e);

        if ((v & 0xFFu) == 1) {
            if (v & (1u << 16)) {
                pci_config_write32(pd->bus, pd->device, pd->function,
                                   (uint8_t)e, v | (1u << 24));
                for (unsigned int i = 0; i < 1000; i++) {
                    if (!(pci_config_read32(pd->bus, pd->device,
                                            pd->function, (uint8_t)e) &
                          (1u << 16)))
                        break;
                    mdelay(1);
                }
            }
            pci_config_write32(pd->bus, pd->device, pd->function,
                               (uint8_t)(e + 4), 0);
            break;
        }

        e = (v >> 8) & 0xFFu;
        if (!e)
            break;
    }

    /* Reset the controller. */
    owl(OP_CMD, orl(OP_CMD) & ~CMD_RS);
    wait_sts(STS_HCH, true, 50);
    owl(OP_CMD, CMD_HCRST);
    for (unsigned int i = 0; i < 100 && (orl(OP_CMD) & CMD_HCRST); i++)
        mdelay(1);

    if (orl(OP_CMD) & CMD_HCRST) {
        vga_write("[USB] EHCI reset timed out, skipped\n");
        return;
    }

    if (hcc & 1u)
        owl(OP_DSSEG, 0);
    owl(OP_INTR, 0);

    memset(&qh, 0, sizeof(qh));
    qh.link = phys(&qh) | 2u;
    qh.epchar = 1u << 15;
    qh.epcap = 1u << 30;
    qh.onext = 1;
    qh.oalt = 1;
    qh.otoken = 0x40;
    owl(OP_ASYNC, phys(&qh));

    owl(OP_CMD, CMD_RS | (8u << 16));
    wait_sts(STS_HCH, false, 50);
    owl(OP_CFG, 1);                 /* route every port to this controller */
    mdelay(5);

    unsigned int ports = hcs & 0xFu;

    if (hcs & (1u << 4)) {
        for (unsigned int i = 0; i < ports; i++)
            owl(OP_PORT(i), (orl(OP_PORT(i)) & ~PORT_RWC) | PORT_PP);
        mdelay(20);
    }

    mdelay(100);                    /* attach debounce */

    for (unsigned int i = 0; i < ports; i++)
        init_port(i);
}

unsigned int usb_init(void)
{
    dev_total = 0;
    next_addr = 1;
    hid.present = false;

    vga_write("\n[USB] Looking for EHCI (USB 2.0) controllers...\n");

    for (unsigned int i = 0; i < pci_device_count(); i++) {
        const pci_device_t *pd = pci_device_get(i);

        if (pd->class_code == 0x0C && pd->subclass == 0x03 &&
            pd->programming_interface == 0x20)
            init_controller(pd);
    }

    if (dev_total == 0)
        vga_write(hid.present ? "[USB] No USB storage device found (mouse only).\n"
                              : "[USB] No USB storage device found.\n");

    return dev_total;
}

unsigned int usb_disk_count(void)
{
    return dev_total;
}

blockdev_t *usb_disk_get(unsigned int index)
{
    return index < dev_total ? &devs[index].dev : 0;
}

int usb_mouse_present(void)
{
    return hid.present;
}

/*
 * One poll of the mouse's interrupt endpoint, about a millisecond long. The
 * HC retries NAKs by itself, so a quiet mouse just times out. Returns 1 and
 * fills report/len when the mouse sent a report.
 */
int usb_mouse_poll(uint8_t *report, unsigned int *len)
{
    if (!hid.present || !op)
        return 0;

    qtd_init(&qtd[0], 1, hid.tog, hid_buf, hid.mps);
    xfer(hid.addr, hid.ep, hid.mps, 1, 1);

    /*
     * xfer() has stopped the schedule by now. If the transfer finished in the
     * last instant before that, the status byte is clean: take the report so
     * the data toggle stays in step with the device.
     */
    uint32_t tok = *(volatile uint32_t *)&qtd[0].token;

    if (tok & 0x40u) {
        if (++hid.errors > 20)
            hid.present = false;    /* unplugged or broken: stop polling it */
        return 0;
    }

    hid.errors = 0;

    if (tok & 0xFFu)
        return 0;                   /* still active: nothing to report */

    uint32_t left = (tok >> 16) & 0x7FFFu;
    uint32_t got = left <= hid.mps ? hid.mps - left : 0;

    hid.tog ^= 1;

    if (!got)
        return 0;

    for (uint32_t i = 0; i < got && i < 64; i++)
        report[i] = hid_buf[i];

    *len = got;
    return 1;
}
