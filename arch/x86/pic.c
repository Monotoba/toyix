// arch/x86/pic.c
#include <stdint.h>
#include "arch/x86/io.h"
#include "arch/x86/pic.h"
#include "kernel/console.h"


#define PIC1_COMMAND 	0x20u
#define PIC1_DATA		0x21u
#define PIC2_COMMAND	0xA0u
#define PIC2_DATA		0xA1u


#define PIC_EOI			0x20u


#define ICW1_ICW4		0x20u
#define ICW1_INIT		0x10u
#define ICW4_8086		0x01u


void pic_send_eoi(uint8_t irq_line) {
	if (irq_line >= 8) {
		outb(PIC2_COMMAND, PIC_EOI);
	}
	
	outb(PIC1_COMMAND, PIC_EOI);
}


void pic_set_mask(uint8_t irq_line) {
	uint16_t port;
	uint8_t  value;
	
	if (irq_line < 8) {
		port = PIC1_DATA;
	} else {
		port = PIC2_DATA;
		irq_line = (uint8_t)(irq_line - 8);	
	}
	
	value = inb(port);
	value = (uint8_t)(value | (1u << irq_line));
	outb(port, value);
}


void pic_clear_mask(uint8_t irq_line) {
	uint8_t port;
	uint8_t value;
	uint8_t original_irq = irq_line;
	
	if (irq_line < 8) {
		port = PIC1_DATA;
	} else {
		port = PIC2_DATA;
		irq_line = (uint8_t)(irq_line - 8);
	}
	
	value = inb(port);
	value = (uint8_t)(value & ~(1u << irq_line));
	outb(port, value);
	
	/*
	 * If any slave IRQ is unmasked, the master's cascade line IRQ2 must
	 * also be unmasked or the slave interrupt can never reach the CPU.
	 */
	 if (original_irq >= 8) {
	 	value = inb(PIC1_DATA);
	 	value = (uint8_t)(value & ~(1u << 2));
	 	outb(PIC1_DATA, value);
	 }
}


void pic_init(void) {
    /*
     * Begin initialization and tell both PICs that ICW4 will follow.
     */
    outb(PIC1_COMMAND, ICW1_INIT | ICW1_ICW4);
    io_wait();

    outb(PIC2_COMMAND, ICW1_INIT | ICW1_ICW4);
    io_wait();

    /*
     * ICW2: interrupt-vector offsets.
     *
     * Master IRQ0-IRQ7  -> vectors 0x20-0x27
     * Slave  IRQ8-IRQ15 -> vectors 0x28-0x2F
     */
    outb(PIC1_DATA, PIC_REMAP_MASTER_OFFSET);
    io_wait();

    outb(PIC2_DATA, PIC_REMAP_SLAVE_OFFSET);
    io_wait();

    /*
     * ICW3: configure the master/slave connection.
     *
     * Bit 2 on the master says that a slave is attached to IRQ2.
     * The slave identity value 2 says it is connected to master IRQ2.
     */
    outb(PIC1_DATA, 0x04u);
    io_wait();

    outb(PIC2_DATA, 0x02u);
    io_wait();

    /*
     * ICW4: use 8086/88 interrupt mode.
     */
    outb(PIC1_DATA, ICW4_8086);
    io_wait();

    outb(PIC2_DATA, ICW4_8086);
    io_wait();

    /*
     * Initially mask every IRQ. Individual drivers unmask only the
     * interrupt lines they are prepared to handle.
     */
    outb(PIC1_DATA, 0xFFu);
    outb(PIC2_DATA, 0xFFu);

    console_writeln("PIC: remapped IRQs to vectors 0x20-0x2F");
}










