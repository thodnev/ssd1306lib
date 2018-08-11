/* MIT License
 * 
 * Copyright 2018, Tymofii Khodniev <thodnev @ github>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to 
 * deal in the Software without restriction, including without limitation the 
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the Software is 
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in 
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR 
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, 
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE 
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER 
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN 
 * THE SOFTWARE.
 */

#include <avr/io.h>
#include <avr/interrupt.h>
#include <stdint.h>
#include <stdbool.h>
#include <assert.h>

#ifndef F_CPU
	#define F_CPU 16000000UL
	#warning "OLED: F_CPU not set. Using default 16 MHz as fallback"
#endif

#if !defined(OLED_NO_I2C) && !defined(OLED_CMDBUFFER_LEN)
	#define OLED_CMDBUFFER_LEN 30
	#warning "OLED: OLED_CMDBUFFER_LEN not set. Using " ##OLED_CMDBUFFER_LEN " as fallback"
#endif

#if defined(OLED_NO_I2C)
	#warning "OLED: building without I2C"
	#define OLED_I2CWRAP(BLOCK)
#else
	#define OLED_I2CWRAP(BLOCK) BLOCK
#endif

#if !(defined(TWBR) && defined(TWSR) && defined(TWAR) && defined(TWDR)) && !defined(OLED_NO_I2C)
	#error "OLED: AVR target has no TWI peripheral. I2C is required by lib"
#endif

/* GCC provides special attribute, indicating that function is not only tried */
/* to be inlined, but must be ALWAYS inlined instead.			      */
#define ALWAYSINLINE __attribute__((__always_inline__))

#define OLED_ARR_SIZE(arr) (sizeof (arr) / sizeof *(arr))

typedef enum OLED_err_e_ {
	OLED_EOK = 0,
	OLED_EBOUNDS,	/* Pixel is out of display bounds 	*/
	OLED_EPARAMS,	/* Wrong parameters specified		*/
	OLED_EBUSY	/* Indicates display is busy (locked)	*/
} OLED_err;

enum OLED_params {
	/* Bits in mask. Could be combined: (OLED_FILL | OLED_BLACK)  */
	OLED_WHITE = 0x00,		/* Alias for 0 as color	      */
	OLED_BLACK = 0x01,		/* Alias for 1 as color	      */
	OLED_NO_FILL = 0x00,		/* Do not fill the drawn area */
	OLED_FILL = 0x02		/* Fill the area	      */
};

/* Lock type. Need to be volatile to prevent optimizations */
/* 1 means unlocked, 0 means locked */
typedef volatile uint8_t	lock_t;


typedef struct OLED_s_ {
	uint8_t width;
	uint8_t height;
	lock_t busy_lock;	/* Locks when operations on OLED are in process */
	uint8_t *frame_buffer;	/* A *flat* array which contents are displayed */
	OLED_I2CWRAP(		/* Included only if no OLED_NO_I2C defined */
		uint8_t i2c_addr;
		uint8_t cur_page;
		uint8_t num_pages;
	)
} OLED;


#if !defined(OLED_NO_I2C)
/* Buffer used to store commands being emmitted to display */
extern uint8_t OLED_cmdbuffer[OLED_CMDBUFFER_LEN];
#endif


/* Inlines should be declared in headers */
/* For more: https://gcc.gnu.org/onlinedocs/gcc/Inline.htm */

/* Unlocks the busy lock (independent of its original condition) */
inline ALWAYSINLINE void OLED_unlock(OLED *oled)
{
	oled->busy_lock = 1;
	/* This is translated to: 	*/
	/*	ldi r0, lo8(1)		*/
	/* 	movw r30, r24	  	*/
	/*	std Z+6, r0		*/
}


/* Tries to atomically acquire lock. On success captures lock and returns
 * true. On failure does nothing and returns false.
 * Usage is like:
 * if (OLED_trylock(&myoled)) {
 *     ...do something...
 *     OLED_unlock(&myoled)
 * } else {
 *     ...OLED is busy. Do other stuff...
 * }
 */
#ifdef __AVR_XMEGA__
inline ALWAYSINLINE bool OLED_trylock(OLED *oled)
{
	/* Relies on assembly directives. For more, see:		    */
	/* https://www.nongnu.org/avr-libc/user-manual/inline_asm.html 	    */
	/* And LAC in AVR Instruction Set Manual 			    */
	/* %0 is Z register (ptr is read-only, destination is read-written) */
	/* %1 is r0...r31 (read-write)					    */
	/* Works like:   (but is atomic, single instruction)		    */
	/* lock_t old = busy_lock; 				            */
	/* busy_lock = busy_lock & ~val;				    */
	/* val = old;							    */

	bool val = true;
	asm volatile("lac %0, %A1" :
		     "z" (&oled->busy_lock) :
		     "+r" (val));
	return !val;
}
#else
inline ALWAYSINLINE bool OLED_trylock(OLED *oled)
{
	/* We do not have atomic instructions on non-XMEGA cores, so try to */
	/* stick with what's available					    */

	bool val;
	asm volatile(
		/* Read SREG containing Interrupts Enabled flag to tmp reg */
			"in __tmp_reg__, __SREG__\n\t"
		/* Disable interrupts. Interrupt can occur between in and  */
		/* cli. It couldn't be fixed because of cpu architecture   */
			"cli\n\t"
			"ld %0, %a1\n\t"
		/* Write 0 to lock		   			   */
			"st %a1, __zero_reg__\n\t"
		/* Restore original SREG with Interrupts Enabled flag 	   */
			"out __SREG__, __tmp_reg__" :
		/* Attributes below */
			"=&r" (val) :
			"z" (&oled->busy_lock)
		);
	return !val;
}
#endif


/* Cycles till the lock is unlocked, acquires it and only then exits
 * (!) Warning: may cause deadlock (infinite wait for resource to free)
 */
inline ALWAYSINLINE bool OLED_spinlock(OLED *oled)
{
	while (!OLED_trylock(oled));
	return true;
}


/* Used as a header of block to execute its contents with OLED_spinlock
 * (!) Warning: may cause deadlock (infinite wait for resource to free)
 * Usage example:
 * 	OLED_WITH_SPINLOCK(&oled) {
 *		...do something...
 *	}
 * In a for(a; b; c) { ...body... } first a is executed. Then b is tested. Then
 * body is run. Then c is executed. After this, b is tested (and this time)
 * it will evaluate as false.
 * For more see: https://www.tutorialspoint.com/cprogramming/c_for_loop.htm
 */
#define OLED_WITH_SPINLOCK(oled) for(bool __tmp = OLED_spinlock((oled)); __tmp; OLED_unlock((oled)), __tmp = false)


/* The same for OLED_trylock
 * Usage example:
 *	bool isrun = false;
 * 	OLED_WITH_TRYLOCK(&oled, isrun) {
 *		...do something...
 *	}
 *      // now isrun is true if trylock was successful
 * Or:
 * 	OLED_WITH_TRYLOCK(&oled) {
 *		...do something...
 *	}
 * (!) Warning: in second case block could be run or not run, so
 *              in this case use flag *inside* block and test busy_lock
 */
#define OLED_WITH_TRYLOCK_1(oled)  for(bool __tmp = OLED_trylock((oled)); __tmp; OLED_unlock((oled)), __tmp = false)
#define OLED_WITH_TRYLOCK_2(oled, flag)  for(bool __tmp = (flag) = OLED_trylock((oled)); __tmp && (flag); OLED_unlock((oled)), __tmp = false)
#define OLED_WITH_TRYLOCK_N(a0, a1, a2, FUNC, ...) FUNC
#define OLED_WITH_TRYLOCK(...) OLED_WITH_TRYLOCK_N(, ##__VA_ARGS__, OLED_WITH_TRYLOCK_2(__VA_ARGS__), OLED_WITH_TRYLOCK_1(__VA_ARGS__))


OLED_err __OLED_init(OLED *oled, uint8_t width, uint8_t height, uint8_t *frame_buffer, uint32_t i2c_freq_hz, uint8_t i2c_addr);
#ifdef OLED_NO_I2C
#define OLED_init(o, w, h, fb, ...) ({								  \
	_Static_assert(!((w) % 8) && !((h) % 8),							  \
		       "OLED_init: Both width and height MUST BE a multiple of 8");		  \
	OLED_err __err = __OLED_init((o), (w), (h), (fb), ##__VA_ARGS__);			  \
	__err; })
#else
#define OLED_init(o, w, h, fb, freq, addr) ({							  \
	_Static_assert(!((w) % 8) && !((h) % 8),							  \
		       "OLED_init: Both width and height MUST BE a multiple of 8");		  \
	_Static_assert(((freq) > F_CPU / 32656 + 1) && ((freq) <= F_CPU / 16),			  \
		       "OLED_init: I2C hz freq must be in range [1+F_CPU/32656...F_CPU/16]");	  \
	_Static_assert(((addr) & 0x80) == 0,							  \
		       "OLED_init: I2C address must be 7-bit wide");				  \
	OLED_err __err = __OLED_init((o), (w), (h), (fb), (freq), (addr));			  \
	__err; })
#endif


#if !defined(OLED_NO_I2C)
// TODO: document these

/* Sets display brightness. Uses spinlock */
void OLED_cmd_setbrightness(OLED *oled, uint8_t level);


/* Output frame_buffer contents to display. Uses spinlock */
void OLED_refresh(OLED *oled);
#endif

/* Inline put pixel, without checks. See the full method below		     */
/* Used to allow GCC to optimize other draw routines which use put_pixel     */
inline ALWAYSINLINE void OLED_put_pixel_(OLED *oled, uint8_t x, uint8_t y, bool pixel_state)
{
	/* Find byte index in flat array */
	uint16_t byte_num = (y / 8) * (uint16_t)oled->width + x;
	uint8_t bit_y = y % 8;
	if (pixel_state)
		oled->frame_buffer[byte_num] |= (1 << bit_y);
	else
		oled->frame_buffer[byte_num] &= ~(1 << bit_y);
}


/* OLED_put_pixel() - puts pixel at specified coordinates
 * @oled:	OLED object
 * @x:		horizonal coordinate (starting at 0, left-to-right)
 * @y:		vertical coordinate (starting at 0, top-to-bottom)
 * @pixel_state	value of the pixel (0 or 1)
 *
 * Use inline OLED_put_pixel_ for faster output, but without checks
 *
 * These methods are not atomic. If required, protect them with lock, i.e.:
 * OLED_WITH_SPINLOCK(&oled) {
 * 	OLED_put_pixel(&oled, 1, 4, 1);
 * 	...
 * 	OLED_put_pixel(&oled, 10, 20, 1);
 * }
 */
OLED_err OLED_put_pixel(OLED *oled, uint8_t x, uint8_t y, bool pixel_state);


/* OLED_put_rectangle() - ...
 * ...
 *
 * (!) Notice: method is not atomic. If required, protect it with lock
 */
OLED_err OLED_put_rectangle(OLED *oled, uint8_t x_from, uint8_t y_from, uint8_t x_to, uint8_t y_to, enum OLED_params params);