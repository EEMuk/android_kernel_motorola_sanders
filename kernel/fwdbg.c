// SPDX-License-Identifier: GPL-2.0
/*
 * Boot progress stripes & on-screen text for bring-up without a serial console.
 *
 * The bootloader leaves the display scanning out its continuous-splash
 * framebuffer at physical address 0x90001000. We map each band through
 * early_ioremap (FIXMAP_PAGE_IO), write the pixels, and early_iounmap.
 * FIXMAP_PAGE_IO bypasses CPU cache, ensuring the MDP5 display controller
 * immediately scans out the updated pixels from physical RAM.
 */
#include <linux/fwdbg.h>
#include <linux/string.h>
#include <linux/export.h>
#include <linux/kthread.h>
#include <linux/delay.h>
#include <asm/setup.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/io.h>
#include <linux/timex.h>
#include <linux/font.h>
#include <asm/early_ioremap.h>
#include <linux/notifier.h>
#include <linux/reboot.h>
#include <linux/moduleparam.h>
#include <linux/string.h>
#include <linux/input/qpnp-power-on.h>

#define FWDBG_FB_BASE	0x90001000UL	/* MIPI_FB_ADDR of the msm8953 LK */
#define FWDBG_FB_WIDTH	1080
#define FWDBG_FB_BPP	3
#define FWDBG_HEIGHT	25		/* 25 rows tall per stripe */

extern const struct font_desc font_vga_8x16;

/*
 * early_ioremap() is __init code: once free_initmem() has run it is gone,
 * so after early boot all drawing goes through one permanent mapping.
 */
#define FWDBG_FB_SIZE	(FWDBG_FB_WIDTH * FWDBG_FB_BPP * 1920)
static u8 __iomem *fwdbg_fb;
static void __iomem *fwdbg_wdt;

static u8 __iomem *fwdbg_map(phys_addr_t pa, size_t len)
{
	if (fwdbg_fb)
		return fwdbg_fb + (pa - FWDBG_FB_BASE);
	if (system_state != SYSTEM_BOOTING)
		return NULL;
	return early_ioremap(pa, len);
}

static void fwdbg_unmap(u8 __iomem *p, size_t len)
{
	if (!fwdbg_fb)
		early_iounmap(p, len);
}

static int __init fwdbg_map_fb(void)
{
	/* the splash framebuffer is ordinary RAM: memremap, not ioremap */
	fwdbg_fb = (u8 __iomem *)memremap(FWDBG_FB_BASE, FWDBG_FB_SIZE,
					   MEMREMAP_WC);
	fwdbg_wdt = ioremap(0x0b017000UL, PAGE_SIZE);
	return 0;
}
early_initcall(fwdbg_map_fb);

/*
 * fwdbg.stall=<row>: spin 20 s at that stripe. With no screen to look at,
 * the extra time the phone needs to come back to fastboot shows the kernel
 * got that far.
 */
static void fwdbg_maybe_stall(int row)
{
	const char *p = strstr(boot_command_line, "fwdbg.stall=");
	cycles_t start;
	long want;

	if (row != -1 && (!p || (want = simple_strtol(p + 12, NULL, 10)) != row))
		return;
	start = get_cycles();
	while ((get_cycles() - start) < (cycles_t)20000 * 19200) {
		if (fwdbg_wdt)
			writel_relaxed(0, fwdbg_wdt + 0x08);	/* WDT0_EN */
		cpu_relax();
	}
}

void fwdbg_paint(int row, u8 r, u8 g, u8 b)
{
	size_t stride = FWDBG_FB_WIDTH * FWDBG_FB_BPP;
	size_t len = stride * FWDBG_HEIGHT;
	phys_addr_t pa = FWDBG_FB_BASE + (phys_addr_t)row * stride;
	u8 __iomem *p;
	size_t i;

	p = fwdbg_map(pa, len);
	if (!p)
		return;
	for (i = 0; i + 2 < len; i += 3) {
		writeb_relaxed(r, p + i);
		writeb_relaxed(g, p + i + 1);
		writeb_relaxed(b, p + i + 2);
	}
	wmb();
	fwdbg_unmap(p, len);

	/* Delay ~80ms using the ARM architected timer */
	{
		cycles_t start = get_cycles();
		cycles_t wait_cycles = (cycles_t)80 * 19200; /* 19.2 MHz counter */
		while ((get_cycles() - start) < wait_cycles)
			cpu_relax();
	}
	fwdbg_maybe_stall(row);
}

void fwdbg_print(int row, const char *str, u8 r, u8 g, u8 b, u8 bg_r, u8 bg_g, u8 bg_b)
{
	const int scale = 3; /* 24x48 px per character */
	const int char_w = 8 * scale;
	const int char_h = 16 * scale;
	size_t stride = FWDBG_FB_WIDTH * FWDBG_FB_BPP;
	size_t len = stride * char_h;
	phys_addr_t pa = FWDBG_FB_BASE + (phys_addr_t)row * stride;
	u8 __iomem *fb;
	int c_idx = 0;
	size_t i;

	fb = fwdbg_map(pa, len);
	if (!fb)
		return;

	/* Fill line with background */
	for (i = 0; i + 2 < len; i += 3) {
		writeb_relaxed(bg_r, fb + i);
		writeb_relaxed(bg_g, fb + i + 1);
		writeb_relaxed(bg_b, fb + i + 2);
	}

	while (*str && (c_idx + 1) * char_w <= FWDBG_FB_WIDTH) {
		unsigned char ch = (unsigned char)*str++;
		const u8 *font_char = font_vga_8x16.data + (ch * 16);
		int font_y, font_x, sy, sx;

		for (font_y = 0; font_y < 16; font_y++) {
			u8 bits = font_char[font_y];
			for (font_x = 0; font_x < 8; font_x++) {
				if (bits & (0x80 >> font_x)) {
					for (sy = 0; sy < scale; sy++) {
						int y = font_y * scale + sy;
						size_t row_offset = y * stride;
						for (sx = 0; sx < scale; sx++) {
							int x = c_idx * char_w + font_x * scale + sx;
							size_t px = row_offset + x * 3;
							writeb_relaxed(r, fb + px);
							writeb_relaxed(g, fb + px + 1);
							writeb_relaxed(b, fb + px + 2);
						}
					}
				}
			}
		}
		c_idx++;
	}
	wmb();
	fwdbg_unmap(fb, len);
}

void fwdbg_freeze(void)
{
	/*
	 * Keep the screen frozen permanently so the user can read the crash text.
	 * Periodically reset and disable the Qualcomm MSM8953 hardware watchdog
	 * (base 0x0b017000, WDT0_RST at +0x04, WDT0_EN at +0x08).
	 */
	void __iomem *wdt = fwdbg_wdt;

	while (1) {
		if (wdt) {
			writel_relaxed(1, wdt + 0x04); /* WDT0_RST: reset watchdog counter */
			writel_relaxed(0, wdt + 0x08); /* WDT0_EN: disable watchdog */
			wmb();
		}
		{
			cycles_t start = get_cycles();
			cycles_t wait_cycles = (cycles_t)50 * 19200; /* 50ms */
			while ((get_cycles() - start) < wait_cycles)
				cpu_relax();
		}
	}
}

void fwdbg_stage(int n)
{
	fwdbg_paint(n * 40, 0xff, 0xff, 0xff);
}

/*
 * Unattended bring-up: after a crash, reboot (warm, so ramoops keeps the
 * log) into the bootloader's fastboot mode instead of the installed OS.
 */
#define FWDBG_IMEM_RESTART_REASON	0x0860065cUL
#define FWDBG_REASON_BOOTLOADER		0x77665500

static void __init fwdbg_set_imem_reason(u32 reason)
{
	void __iomem *p = early_ioremap(FWDBG_IMEM_RESTART_REASON & PAGE_MASK,
					PAGE_SIZE);

	if (!p)
		return;
	writel_relaxed(reason, p + (FWDBG_IMEM_RESTART_REASON & ~PAGE_MASK));
	wmb();
	early_iounmap(p, PAGE_SIZE);
}

/* fwdbg.selftest=1: reboot to fastboot once init is done (path test) */
static bool fwdbg_selftest;
module_param_named(selftest, fwdbg_selftest, bool, 0444);

void fwdbg_reboot_bootloader(void)
{
	cycles_t start = get_cycles();

	/* The MSM watchdog would bite (cold reset, RAM lost) while we wait */
	if (fwdbg_wdt) {
		writel_relaxed(0, fwdbg_wdt + 0x08);	/* WDT0_EN */
		wmb();
	}
	/* ~2 s so the on-screen markers can be seen */
	while ((get_cycles() - start) < (cycles_t)2000 * 19200)
		cpu_relax();
	qpnp_pon_set_restart_reason(PON_RESTART_REASON_BOOTLOADER);
	machine_restart("bootloader");
}

/*
 * After all drivers have probed: point both the PMIC PON reason (survives
 * even a cold reset) and the IMEM reason at fastboot, so a hang that ends
 * in a watchdog bite also lands in the bootloader.
 */
static int __init fwdbg_arm_reboot(void)
{
	void __iomem *imem = ioremap(FWDBG_IMEM_RESTART_REASON & PAGE_MASK,
				     PAGE_SIZE);
	int ret;

	if (imem) {
		writel_relaxed(FWDBG_REASON_BOOTLOADER,
			       imem + (FWDBG_IMEM_RESTART_REASON & ~PAGE_MASK));
		wmb();
		iounmap(imem);
	}
	ret = qpnp_pon_set_restart_reason(PON_RESTART_REASON_BOOTLOADER);
	pr_info("fwdbg: crash reboot armed (pon %d)\n", ret);
	if (fwdbg_selftest) {
		cycles_t start = get_cycles();

		if (fwdbg_wdt) {
			writel_relaxed(0, fwdbg_wdt + 0x08);
			wmb();
		}
		/* 20 s: visible in the time the phone takes to come back */
		while ((get_cycles() - start) < (cycles_t)20000 * 19200)
			cpu_relax();
		machine_restart("bootloader");
	}
	return 0;
}
late_initcall_sync(fwdbg_arm_reboot);

static int fwdbg_panic_notify(struct notifier_block *nb, unsigned long ev,
			      void *data)
{
	fwdbg_reboot_bootloader();
	return NOTIFY_DONE;
}

static struct notifier_block fwdbg_panic_nb = {
	.notifier_call = fwdbg_panic_notify,
	.priority = INT_MIN,	/* after everyone else has logged */
};

static int __init fwdbg_panic_init(void)
{
	atomic_notifier_chain_register(&panic_notifier_list, &fwdbg_panic_nb);
	return 0;
}
early_initcall(fwdbg_panic_init);

/* Called from setup_arch(): any reset from here on lands in fastboot */
void __init fwdbg_early_reason(void)
{
	fwdbg_set_imem_reason(FWDBG_REASON_BOOTLOADER);
	/* fwdbg.early: 20 s stall here, a timing probe that 4.9 runs at all */
	if (0) {
		cycles_t start = get_cycles();

		while ((get_cycles() - start) < (cycles_t)20000 * 19200)
			cpu_relax();
	}
}

/* fwdbg.ic=<n>: stall before the n-th initcall (1-based, link order) */
void fwdbg_initcall(void)
{
	static int count;
	const char *p;

	/* also runs for insmod, when boot_command_line (__initdata) is gone */
	if (system_state != SYSTEM_BOOTING)
		return;
	p = strstr(boot_command_line, "fwdbg.ic=");
	count++;
	if (p && simple_strtol(p + 9, NULL, 10) == count)
		fwdbg_maybe_stall(-1);
}

/* fwdbg.pt=<n>: stall at fwdbg_point(n) */
void fwdbg_point(int n)
{
	const char *p;

	if (system_state != SYSTEM_BOOTING)
		return;
	p = strstr(boot_command_line, "fwdbg.pt=");
	if (p && simple_strtol(p + 9, NULL, 10) == n)
		fwdbg_maybe_stall(-1);
}
EXPORT_SYMBOL(fwdbg_point);

/* fwdbg.reboot_after=<s>: reboot to fastboot <s> seconds after init */
static int fwdbg_reboot_after;
module_param_named(reboot_after, fwdbg_reboot_after, int, 0444);

static int fwdbg_reboot_thread(void *unused)
{
	msleep(fwdbg_reboot_after * 1000);
	pr_info("fwdbg: reboot_after %d s expired\n", fwdbg_reboot_after);
	/* straight to the hardware: a driver stuck in shutdown can't block it */
	qpnp_pon_set_restart_reason(PON_RESTART_REASON_BOOTLOADER);
	machine_restart("bootloader");
	return 0;
}

static int __init fwdbg_reboot_after_init(void)
{
	if (fwdbg_reboot_after > 0)
		kthread_run(fwdbg_reboot_thread, NULL, "fwdbg_reboot");
	return 0;
}
late_initcall_sync(fwdbg_reboot_after_init);
