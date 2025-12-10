#include "uart_esp.h"
#include <avr/io.h>
#include <avr/interrupt.h>

#define F_CPU 16000000UL
#define BAUD 9600
#define UBRR_VAL ((F_CPU / (16UL * BAUD)) - 1)

#define RX_BUF_SIZE 128
static volatile uint8_t rx_buf[RX_BUF_SIZE];
static volatile uint8_t rx_head = 0;
static volatile uint8_t rx_tail = 0;

static inline void rx_push(uint8_t c)
{
    uint8_t next = (uint8_t)((rx_head + 1) % RX_BUF_SIZE);
    if (next != rx_tail) {
        rx_buf[rx_head] = c;
        rx_head = next;
    }
}

static inline uint8_t rx_pop(uint8_t *out)
{
    if (rx_head == rx_tail) return 0;
    *out = rx_buf[rx_tail];
    rx_tail = (uint8_t)((rx_tail + 1) % RX_BUF_SIZE);
    return 1;
}

void uart1_init(void)
{
    DDRB |= (1 << PB3);
    DDRB &= ~(1 << PB4);

    UBRR1H = (uint8_t)(UBRR_VAL >> 8);
    UBRR1L = (uint8_t)(UBRR_VAL & 0xFF);

    UCSR1B = (1 << RXEN1) | (1 << TXEN1) | (1 << RXCIE1);
    UCSR1C = (1 << UCSZ11) | (1 << UCSZ10);
}

void uart1_send_byte(uint8_t d)
{
    while (!(UCSR1A & (1 << UDRE1)));
    UDR1 = d;
}

void uart1_send_string(const char *s)
{
    while (*s) uart1_send_byte((uint8_t)*s++);
}

ISR(USART1_RX_vect)
{
    uint8_t c = UDR1;
    rx_push(c);
}

uint8_t uart1_readline(char *out, uint8_t maxlen)
{
    static uint8_t idx = 0;
    uint8_t b;

    while (rx_pop(&b)) {
        if (b == '\n' || b == '\r') {
            if (idx > 0) {
                out[idx] = '\0';
                idx = 0;
                return 1;
            }
        } else {
            if (idx < maxlen - 1) {
                out[idx++] = (char)b;
            }
        }
    }
    return 0;
}
