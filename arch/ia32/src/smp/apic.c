/*
 * Copyright (C) 2001-2004 Jakub Jermar
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * - Redistributions of source code must retain the above copyright
 *   notice, this list of conditions and the following disclaimer.
 * - Redistributions in binary form must reproduce the above copyright
 *   notice, this list of conditions and the following disclaimer in the
 *   documentation and/or other materials provided with the distribution.
 * - The name of the author may not be used to endorse or promote products
 *   derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <arch/types.h>
#include <arch/smp/apic.h>
#include <arch/smp/ap.h>
#include <arch/smp/mps.h>
#include <mm/page.h>
#include <time/delay.h>
#include <arch/interrupt.h>
#include <print.h>
#include <arch/asm.h>
#include <arch.h>

#ifdef __SMP__

/*
 * This is functional, far-from-general-enough interface to the APIC.
 * Advanced Programmable Interrupt Controller for MP systems.
 * Tested on:
 *	Bochs 2.0.2 - Bochs 2.2 with 2-8 CPUs
 *	Simics 2.0.28 
 *	ASUS P/I-P65UP5 + ASUS C-P55T2D REV. 1.41 with 2x 200Mhz Pentium CPUs
 */

/*
 * These variables either stay configured as initilalized, or are changed by
 * the MP configuration code.
 *
 * Pay special attention to the volatile keyword. Without it, gcc -O2 would
 * optimize the code too much and accesses to l_apic and io_apic, that must
 * always be 32-bit, would use byte oriented instructions.
 */
volatile __u32 *l_apic = (__u32 *) 0xfee00000;
volatile __u32 *io_apic = (__u32 *) 0xfec00000;

__u32 apic_id_mask = 0;

int apic_poll_errors(void);

void apic_init(void)
{
	__u32 tmp, id, i;

	trap_register(VECTOR_APIC_SPUR, apic_spurious);

	enable_irqs_function = io_apic_enable_irqs;
	disable_irqs_function = io_apic_disable_irqs;
	eoi_function = l_apic_eoi;
	
	/*
	 * Configure interrupt routing.
	 * IRQ 0 remains masked as the time signal is generated by l_apic's themselves.
	 * Other interrupts will be forwarded to the lowest priority CPU.
	 */
	io_apic_disable_irqs(0xffff);
	trap_register(VECTOR_CLK, l_apic_timer_interrupt);
	for (i=1; i<16; i++) {
		int pin;
	
		if ((pin = mps_irq_to_pin(i)) != -1)
	    		io_apic_change_ioredtbl(pin,0xf,IVT_IRQBASE+i,LOPRI);
	}
	

	/*
	 * Ensure that io_apic has unique ID.
	 */
	tmp = io_apic_read(IOAPICID);
	id = (tmp >> 24) & 0xf;
	if ((1<<id) & apic_id_mask) {
		int i;
		
		for (i=0; i<15; i++) {
			if (!((1<<i) & apic_id_mask)) {
				io_apic_write(IOAPICID, (tmp & (~(0xf<<24))) | (i<<24));
				break;
			}
		}
	}

	/*
	 * Configure the BSP's lapic.
	 */
	l_apic_init();
	l_apic_debug();	
}

void apic_spurious(__u8 n, __u32 stack[])
{
	printf("cpu%d: APIC spurious interrupt\n", CPU->id);
}

int apic_poll_errors(void)
{
	__u32 esr;
	
	esr = l_apic[ESR] & ~ESRClear;
	
	if ((esr>>0) & 1)
		printf("Send CS Error\n");
	if ((esr>>1) & 1)
		printf("Receive CS Error\n");
	if ((esr>>2) & 1)
		printf("Send Accept Error\n");
	if ((esr>>3) & 1)
		printf("Receive Accept Error\n");
	if ((esr>>5) & 1)
		printf("Send Illegal Vector\n");
	if ((esr>>6) & 1)
		printf("Received Illegal Vector\n");
	if ((esr>>7) & 1)
		printf("Illegal Register Address\n");
    
	return !esr;
}

/*
 * Send all CPUs excluding CPU IPI vector.
 */
int l_apic_broadcast_custom_ipi(__u8 vector)
{
	__u32 lo;

	/*
	 * Read the ICR register in and zero all non-reserved fields.
	 */
	lo = l_apic[ICRlo] & ICRloClear;

	lo |= DLVRMODE_FIXED | DESTMODE_LOGIC | LEVEL_ASSERT | SHORTHAND_EXCL | TRGRMODE_LEVEL | vector;
	
	l_apic[ICRlo] = lo;

	lo = l_apic[ICRlo] & ICRloClear;
	if (lo & SEND_PENDING)
		printf("IPI is pending.\n");

	return apic_poll_errors();
}

/*
 * Universal Start-up Algorithm for bringing up the AP processors.
 */
int l_apic_send_init_ipi(__u8 apicid)
{
	__u32 lo, hi;
	int i;

	/*
	 * Read the ICR register in and zero all non-reserved fields.
	 */
	lo = l_apic[ICRlo] & ICRloClear;
	hi = l_apic[ICRhi] & ICRhiClear;
	
	lo |= DLVRMODE_INIT | DESTMODE_PHYS | LEVEL_ASSERT | SHORTHAND_DEST | TRGRMODE_LEVEL;
	hi |= apicid << 24;
	
	l_apic[ICRhi] = hi;
	l_apic[ICRlo] = lo;

	/*
	 * According to MP Specification, 20us should be enough to
	 * deliver the IPI.
	 */
	delay(20);

	if (!apic_poll_errors()) return 0;

	lo = l_apic[ICRlo] & ICRloClear;
	if (lo & SEND_PENDING)
		printf("IPI is pending.\n");

	l_apic[ICRlo] = lo | DLVRMODE_INIT | DESTMODE_PHYS | LEVEL_DEASSERT | SHORTHAND_DEST | TRGRMODE_LEVEL;

	/*
	 * Wait 10ms as MP Specification specifies.
	 */
	delay(10000);

	if (!is_82489DX_apic(l_apic[LAVR])) {
		/*
		 * If this is not 82489DX-based l_apic we must send two STARTUP IPI's.
		 */
		for (i = 0; i<2; i++) {
			lo = l_apic[ICRlo] & ICRloClear;
			lo |= ((__address) ap_boot) / 4096; /* calculate the reset vector */
			l_apic[ICRlo] = lo | DLVRMODE_STUP | DESTMODE_PHYS | LEVEL_ASSERT | SHORTHAND_DEST | TRGRMODE_LEVEL;
			delay(200);
		}
	}
	
	
	return apic_poll_errors();
}

void l_apic_init(void)
{
	__u32 tmp, t1, t2;

	l_apic[LVT_Err] |= (1<<16);
	l_apic[LVT_LINT0] |= (1<<16);
	l_apic[LVT_LINT1] |= (1<<16);

	tmp = l_apic[SVR] & SVRClear;
	l_apic[SVR] = tmp | (1<<8) | (VECTOR_APIC_SPUR);

	l_apic[TPR] &= TPRClear;

	if (CPU->arch.family >= 6)
		enable_l_apic_in_msr();
	
	tmp = l_apic[ICRlo] & ICRloClear;
	l_apic[ICRlo] = tmp | DLVRMODE_INIT | DESTMODE_PHYS | LEVEL_DEASSERT | SHORTHAND_INCL | TRGRMODE_LEVEL;
	
	/*
	 * Program the timer for periodic mode and respective vector.
	 */

	l_apic[TDCR] &= TDCRClear;
	l_apic[TDCR] |= 0xb;
	tmp = l_apic[LVT_Tm] | (1<<17) | (VECTOR_CLK);
	l_apic[LVT_Tm] = tmp & ~(1<<16);

	t1 = l_apic[CCRT];
	l_apic[ICRT] = 0xffffffff;

	while (l_apic[CCRT] == t1)
		;
		
	t1 = l_apic[CCRT];
	delay(1000);
	t2 = l_apic[CCRT];
	
	l_apic[ICRT] = t1-t2;
	
}

void l_apic_eoi(void)
{
	l_apic[EOI] = 0;
}

void l_apic_debug(void)
{
#ifdef LAPIC_VERBOSE
	int i, lint;

	printf("LVT on cpu%d, LAPIC ID: %d\n", CPU->id, l_apic_id());

	printf("LVT_Tm: ");
	if (l_apic[LVT_Tm] & (1<<17)) printf("periodic"); else printf("one-shot"); putchar(',');	
	if (l_apic[LVT_Tm] & (1<<16)) printf("masked");	else printf("not masked"); putchar(',');
	if (l_apic[LVT_Tm] & (1<<12)) printf("send pending"); else printf("idle"); putchar(',');
	printf("%B\n", l_apic[LVT_Tm] & 0xff);
	
	for (i=0; i<2; i++) {
		lint = i ? LVT_LINT1 : LVT_LINT0;
		printf("LVT_LINT%d: ", i);
		if (l_apic[lint] & (1<<16)) printf("masked"); else printf("not masked"); putchar(',');
		if (l_apic[lint] & (1<<15)) printf("level"); else printf("edge"); putchar(',');
		printf("%d", l_apic[lint] & (1<<14)); putchar(',');
		printf("%d", l_apic[lint] & (1<<13)); putchar(',');
		if (l_apic[lint] & (1<<12)) printf("send pending"); else printf("idle"); putchar(',');
	
		switch ((l_apic[lint]>>8)&7) {
		    case 0: printf("fixed"); break;
		    case 4: printf("NMI"); break;
		    case 7: printf("ExtINT"); break;
		}
		putchar(',');
		printf("%B\n", l_apic[lint] & 0xff);	
	}

	printf("LVT_Err: ");
	if (l_apic[LVT_Err] & (1<<16)) printf("masked"); else printf("not masked"); putchar(',');
	if (l_apic[LVT_Err] & (1<<12)) printf("send pending"); else printf("idle"); putchar(',');
	printf("%B\n", l_apic[LVT_Err] & 0xff);	

	/*
	 * This register is supported only on P6 and higher.
	 */
	if (CPU->arch.family > 5) {
		printf("LVT_PCINT: ");
		if (l_apic[LVT_PCINT] & (1<<16)) printf("masked"); else printf("not masked"); putchar(',');
		if (l_apic[LVT_PCINT] & (1<<12)) printf("send pending"); else printf("idle"); putchar(',');
		switch ((l_apic[LVT_PCINT] >> 8)&7) {
		    case 0: printf("fixed"); break;
		    case 4: printf("NMI"); break;
		    case 7: printf("ExtINT"); break;
		}
		putchar(',');
		printf("%B\n", l_apic[LVT_PCINT] & 0xff);
	}
#endif
}

void l_apic_timer_interrupt(__u8 n, __u32 stack[])
{
	l_apic_eoi();
	clock();
}

__u8 l_apic_id(void)
{
	return (l_apic[L_APIC_ID] >> L_APIC_IDShift)&L_APIC_IDMask;
}

__u32 io_apic_read(__u8 address)
{
	__u32 tmp;
	
	tmp = io_apic[IOREGSEL] & ~0xf;
	io_apic[IOREGSEL] = tmp | address;
	return io_apic[IOWIN];
}

void io_apic_write(__u8 address, __u32 x)
{
	__u32 tmp;

	tmp = io_apic[IOREGSEL] & ~0xf;
	io_apic[IOREGSEL] = tmp | address;
	io_apic[IOWIN] = x;
}

void io_apic_change_ioredtbl(int signal, int dest, __u8 v, int flags)
{
	__u32 reglo, reghi;
	int dlvr = 0;
	
	if (flags & LOPRI)
		dlvr = 1;
	
	reglo = io_apic_read(IOREDTBL + signal*2);
	reghi = io_apic_read(IOREDTBL + signal*2 + 1);
	
	reghi &= ~0x0f000000;
	reghi |= (dest<<24);

	reglo &= (~0x1ffff) | (1<<16); /* don't touch the mask */
	reglo |= (0<<15) | (0<<13) | (0<<11) | (dlvr<<8) | v;

	io_apic_write(IOREDTBL + signal*2, reglo);		
	io_apic_write(IOREDTBL + signal*2 + 1, reghi);
}

void io_apic_disable_irqs(__u16 irqmask)
{
	int i,pin;
	__u32 reglo;
	
	for (i=0;i<16;i++) {
		if ((irqmask>>i) & 1) {
			/*
			 * Mask the signal input in IO APIC if there is a
			 * mapping for the respective IRQ number.
			 */
			pin = mps_irq_to_pin(i);
			if (pin != -1) {
				reglo = io_apic_read(IOREDTBL + pin*2);
				reglo |= (1<<16);
				io_apic_write(IOREDTBL + pin*2,reglo);
			}
			
		}
	}
}

void io_apic_enable_irqs(__u16 irqmask)
{
	int i,pin;
	__u32 reglo;
	
	for (i=0;i<16;i++) {
		if ((irqmask>>i) & 1) {
			/*
			 * Unmask the signal input in IO APIC if there is a
			 * mapping for the respective IRQ number.
			 */
			pin = mps_irq_to_pin(i);
			if (pin != -1) {
				reglo = io_apic_read(IOREDTBL + pin*2);
				reglo &= ~(1<<16);
				io_apic_write(IOREDTBL + pin*2,reglo);
			}
			
		}
	}

}

#endif /* __SMP__ */
