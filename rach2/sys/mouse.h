#ifndef _MOUSE_H_
#define _MOUSE_H_

#include "lib/kint.h"
#include "lib/kdef.h"

/*
 * PS/2 Mouse Driver for Rach2
 *
 * The PS/2 mouse shares the 8042 controller with the keyboard.
 * Mouse uses IRQ12 (vector 44) and sends 3-byte packets.
 */

/*
 * Mouse packet structure for userspace.
 * Each read() returns one or more of these 3-byte packets.
 */
struct mouse_packet {
    uint8_t buttons;  /* bit 0=left, 1=right, 2=middle */
    int8_t  dx;       /* X movement delta */
    int8_t  dy;       /* Y movement delta */
};

/*
 * Initialize the PS/2 mouse.
 * Enables the auxiliary port on the 8042 controller,
 * configures the mouse, and registers the IRQ12 handler.
 * Returns 0 on success, -1 on failure.
 */
int init_mouse(void);

/*
 * Read mouse packets from the ring buffer (non-blocking).
 * Returns the number of packets read (0 if buffer empty).
 */
int mouse_read_packets(struct mouse_packet *buf, int max_count);

/*
 * Clear the mouse packet buffer.
 * Call when switching modes or resetting input state.
 */
void mouse_clear_packets(void);

/*
 * Check if mouse was successfully initialized.
 */
bool mouse_is_available(void);

#endif /* _MOUSE_H_ */
