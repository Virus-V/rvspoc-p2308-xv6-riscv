/*
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2019 Western Digital Corporation or its affiliates.
 *
 * Authors:
 *   Anup Patel <anup.patel@wdc.com>
 */
#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"

#include "uart8250.h"

struct spinlock uart_tx_lock;
#define UART_TX_BUF_SIZE 32
char uart_tx_buf[UART_TX_BUF_SIZE];
uint64 uart_tx_w; // write next to uart_tx_buf[uart_tx_w % UART_TX_BUF_SIZE]
uint64 uart_tx_r; // read next from uart_tx_buf[uart_tx_r % UART_TX_BUF_SIZE]

void uartstart();

/* clang-format off */

static inline void __raw_writeb(uint8 val, volatile void *addr)
{
  asm volatile("sb %0, 0(%1)" : : "r"(val), "r"(addr));
}

static inline void __raw_writew(uint16 val, volatile void *addr)
{
  asm volatile("sh %0, 0(%1)" : : "r"(val), "r"(addr));
}

static inline void __raw_writel(uint32 val, volatile void *addr)
{
  asm volatile("sw %0, 0(%1)" : : "r"(val), "r"(addr));
}

#if __riscv_xlen != 32
static inline void __raw_writeq(uint64 val, volatile void *addr)
{
  asm volatile("sd %0, 0(%1)" : : "r"(val), "r"(addr));
}
#endif

static inline uint8 __raw_readb(const volatile void *addr)
{
  uint8 val;

  asm volatile("lb %0, 0(%1)" : "=r"(val) : "r"(addr));
  return val;
}

static inline uint16 __raw_readw(const volatile void *addr)
{
  uint16 val;

  asm volatile("lh %0, 0(%1)" : "=r"(val) : "r"(addr));
  return val;
}

static inline uint32 __raw_readl(const volatile void *addr)
{
  uint32 val;

  asm volatile("lw %0, 0(%1)" : "=r"(val) : "r"(addr));
  return val;
}

#if __riscv_xlen != 32
static inline uint64 __raw_readq(const volatile void *addr)
{
  uint64 val;

  asm volatile("ld %0, 0(%1)" : "=r"(val) : "r"(addr));
  return val;
}
#endif


#define __io_br() do {} while (0)
#define __io_ar() __asm__ __volatile__ ("fence i,r" : : : "memory");
#define __io_bw() __asm__ __volatile__ ("fence w,o" : : : "memory");
#define __io_aw() do {} while (0)

#define readb(c)  ({ uint8  __v; __io_br(); __v = __raw_readb(c); __io_ar(); __v; })
#define readw(c)  ({ uint16 __v; __io_br(); __v = __raw_readw(c); __io_ar(); __v; })
#define readl(c)  ({ uint32 __v; __io_br(); __v = __raw_readl(c); __io_ar(); __v; })

#define writeb(v,c) ({ __io_bw(); __raw_writeb((v),(c)); __io_aw(); })
#define writew(v,c) ({ __io_bw(); __raw_writew((v),(c)); __io_aw(); })
#define writel(v,c) ({ __io_bw(); __raw_writel((v),(c)); __io_aw(); })

#define UART_RBR_OFFSET   0 /* In:  Recieve Buffer Register */
#define UART_THR_OFFSET   0 /* Out: Transmitter Holding Register */
#define UART_DLL_OFFSET   0 /* Out: Divisor Latch Low */
#define UART_IER_OFFSET   1 /* I/O: Interrupt Enable Register */
#define UART_DLM_OFFSET   1 /* Out: Divisor Latch High */
#define UART_FCR_OFFSET   2 /* Out: FIFO Control Register */
#define UART_IIR_OFFSET   2 /* I/O: Interrupt Identification Register */
#define UART_LCR_OFFSET   3 /* Out: Line Control Register */
#define UART_MCR_OFFSET   4 /* Out: Modem Control Register */
#define UART_LSR_OFFSET   5 /* In:  Line Status Register */
#define UART_MSR_OFFSET   6 /* In:  Modem Status Register */
#define UART_SCR_OFFSET   7 /* I/O: Scratch Register */
#define UART_MDR1_OFFSET  8 /* I/O:  Mode Register */

#define UART_LSR_FIFOE    0x80  /* Fifo error */
#define UART_LSR_TEMT   0x40  /* Transmitter empty */
#define UART_LSR_THRE   0x20  /* Transmit-hold-register empty */
#define UART_LSR_BI   0x10  /* Break interrupt indicator */
#define UART_LSR_FE   0x08  /* Frame error indicator */
#define UART_LSR_PE   0x04  /* Parity error indicator */
#define UART_LSR_OE   0x02  /* Overrun error indicator */
#define UART_LSR_DR   0x01  /* Receiver data ready */
#define UART_LSR_BRK_ERROR_BITS 0x1E  /* BI, FE, PE, OE bits */

/* clang-format on */

static volatile void *uart8250_base;
static uint32 uart8250_in_freq;
static uint32 uart8250_baudrate;
static uint32 uart8250_reg_width;
static uint32 uart8250_reg_shift;

static uint32 get_reg(uint32 num)
{
  uint32 offset = num << uart8250_reg_shift;

  if (uart8250_reg_width == 1)
    return readb(uart8250_base + offset);
  else if (uart8250_reg_width == 2)
    return readw(uart8250_base + offset);
  else
    return readl(uart8250_base + offset);
}

static void set_reg(uint32 num, uint32 val)
{
  uint32 offset = num << uart8250_reg_shift;

  if (uart8250_reg_width == 1)
    writeb(val, uart8250_base + offset);
  else if (uart8250_reg_width == 2)
    writew(val, uart8250_base + offset);
  else
    writel(val, uart8250_base + offset);
}

void uart8250_putc(char ch)
{
  while ((get_reg(UART_LSR_OFFSET) & UART_LSR_THRE) == 0)
    ;

  set_reg(UART_THR_OFFSET, ch);
}

int uart8250_getc(void)
{
  if (get_reg(UART_LSR_OFFSET) & UART_LSR_DR)
    return get_reg(UART_RBR_OFFSET);
  return -1;
}

//static struct sbi_console_device uart8250_console = {
//  .name = "uart8250",
//  .console_putc = uart8250_putc,
//  .console_getc = uart8250_getc
//};

int uart8250_init(unsigned long base, uint32 in_freq, uint32 baudrate, uint32 reg_shift,
      uint32 reg_width)
{
  uint16 bdiv;

  uart8250_base      = (volatile void *)base;
  uart8250_reg_shift = reg_shift;
  uart8250_reg_width = reg_width;
  uart8250_in_freq   = in_freq;
  uart8250_baudrate  = baudrate;

  bdiv = uart8250_in_freq / (16 * uart8250_baudrate);

  /* Disable all interrupts */
  set_reg(UART_IER_OFFSET, 0x00);
  /* Enable DLAB */
  set_reg(UART_LCR_OFFSET, 0x80);

  if (bdiv) {
    /* Set divisor low byte */
    set_reg(UART_DLL_OFFSET, bdiv & 0xff);
    /* Set divisor high byte */
    set_reg(UART_DLM_OFFSET, (bdiv >> 8) & 0xff);
  }

  /* 8 bits, no parity, one stop bit */
  set_reg(UART_LCR_OFFSET, 0x03);
  /* Enable FIFO */
  set_reg(UART_FCR_OFFSET, 0x01);
  /* No modem control DTR RTS */
  set_reg(UART_MCR_OFFSET, 0x00);
  /* Clear line status */
  get_reg(UART_LSR_OFFSET);
  /* Read receive buffer */
  get_reg(UART_RBR_OFFSET);
  /* Set scratchpad */
  set_reg(UART_SCR_OFFSET, 0x00);
  /* Clear interrupt */
  get_reg(UART_IIR_OFFSET);
  get_reg(UART_MSR_OFFSET);
  get_reg(31); // USR

  /* Enable Interrupt */
  set_reg(UART_IER_OFFSET, 0x05);

  return 0;
}

extern volatile int panicked; // from printf.c

void
uartinit(void)
{
  uart8250_init(UART0, 25000000, 115200, 2, 4);
  initlock(&uart_tx_lock, "uart");
}

// add a character to the output buffer and tell the
// UART to start sending if it isn't already.
// blocks if the output buffer is full.
// because it may block, it can't be called
// from interrupts; it's only suitable for use
// by write().
void
uartputc(int c)
{
  acquire(&uart_tx_lock);

  if(panicked){
    for(;;)
      ;
  }

  while(uart_tx_w == uart_tx_r + UART_TX_BUF_SIZE){
    // buffer is full.
    // wait for uartstart() to open up space in the buffer.
    sleep(&uart_tx_r, &uart_tx_lock);
  }
  uart_tx_buf[uart_tx_w % UART_TX_BUF_SIZE] = c;
  uart_tx_w += 1;
  uartstart();

  release(&uart_tx_lock);
}


// alternate version of uartputc() that doesn't
// use interrupts, for use by kernel printf() and
// to echo characters. it spins waiting for the uart's
// output register to be empty.
void
uartputc_sync(int c)
{
  push_off();

  if(panicked){
    for(;;)
      ;
  }

  uart8250_putc(c);
  pop_off();
}

// if the UART is idle, and a character is waiting
// in the transmit buffer, send it.
// caller must hold uart_tx_lock.
// called from both the top- and bottom-half.
void
uartstart()
{
  while(1){
    if(uart_tx_w == uart_tx_r){
      // transmit buffer is empty.
      return;
    }

    if((get_reg(UART_LSR_OFFSET) & UART_LSR_THRE) == 0){
      // the UART transmit holding register is full,
      // so we cannot give it another byte.
      // it will interrupt when it's ready for a new byte.
      return;
    }

    int c = uart_tx_buf[uart_tx_r % UART_TX_BUF_SIZE];
    uart_tx_r += 1;

    // maybe uartputc() is waiting for space in the buffer.
    wakeup(&uart_tx_r);

    set_reg(UART_THR_OFFSET, c);
  }
}

// read one input character from the UART.
// return -1 if none is waiting.
int
uartgetc(void)
{
  return uart8250_getc();
}

// handle a uart interrupt, raised because input has
// arrived, or the uart is ready for more output, or
// both. called from devintr().
void
uartintr(void)
{
  // read and process incoming characters.
  while(1){
    int c = uartgetc();
    if(c == -1)
      break;
    consoleintr(c);
  }

  // send buffered characters.
  acquire(&uart_tx_lock);
  uartstart();
  release(&uart_tx_lock);
}