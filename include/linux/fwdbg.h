/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_FWDBG_H
#define _LINUX_FWDBG_H

#include <linux/types.h>

/* Boot progress stripes on the bootloader's splash framebuffer */
#ifdef CONFIG_FWDBG_STRIPES
void fwdbg_stage(int n);
void fwdbg_paint(int row, u8 r, u8 g, u8 b);
void fwdbg_print(int row, const char *s, u8 r, u8 g, u8 b, u8 bg_r, u8 bg_g, u8 bg_b);
void fwdbg_freeze(void);
void fwdbg_reboot_bootloader(void);
void fwdbg_early_reason(void);
void fwdbg_initcall(void);
void fwdbg_point(int n);
#else
static inline void fwdbg_stage(int n) { }
static inline void fwdbg_paint(int row, u8 r, u8 g, u8 b) { }
static inline void fwdbg_print(int row, const char *s, u8 r, u8 g, u8 b, u8 bg_r, u8 bg_g, u8 bg_b) { }
static inline void fwdbg_freeze(void) { }
static inline void fwdbg_reboot_bootloader(void) { }
static inline void fwdbg_early_reason(void) { }
static inline void fwdbg_initcall(void) { }
static inline void fwdbg_point(int n) { }
#endif

#endif
