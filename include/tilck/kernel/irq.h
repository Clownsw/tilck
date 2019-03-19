/* SPDX-License-Identifier: BSD-2-Clause */

#pragma once

#include <tilck/common/basic_defs.h>
#include <tilck/kernel/hal.h>
#include <tilck/kernel/interrupts.h>

void setup_irq_handling();

void irq_install_handler(u8 irq, irq_handler_node *n);
void irq_uninstall_handler(u8 irq, irq_handler_node *n);

void irq_set_mask(int irq);
void irq_clear_mask(int irq);

void debug_show_spurious_irq_count(void);
