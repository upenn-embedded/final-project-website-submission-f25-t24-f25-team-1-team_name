/*
 * File: main.c
 * ATmega328PB ? UART1 full-duplex with Timer4 1s tick
 * RX: interrupt-driven ring buffer
 * TX: polled in main loop
 *
 */

#include <xc.h>
#include <avr/io.h>
#include <avr/interrupt.h>
#include <stdio.h>
#include <stdint.h>
#include "uart.h"
#include "uart_esp.h"

volatile uint8_t flag_1s = 0;

void Timer4_Init(void) {
    cli();
    TCCR4B |= (1 << WGM42);
    OCR4A = 15625;
    TIMSK4 |= (1 << OCIE4A);
    TCCR4B |= (1 << CS42) | (1 << CS40);
    sei();
}

ISR(TIMER4_COMPA_vect) {
    flag_1s = 1;
}

int main(void)
{
    uart_init();
    uart1_init();
    Timer4_Init();
    sei();

    printf("ATmega328PB UART1 RX + Timer4 started.\n");

    char line[64];

    while (1)
    {
        if (uart1_readline(line, sizeof(line))) {
            printf("Received from ESP32: %s\n", line);
        }

        if (flag_1s) {
            flag_1s = 0;
            uart1_send_string("asdfqwerasdfqwen\n");
        }
    }

    return 0;
}
