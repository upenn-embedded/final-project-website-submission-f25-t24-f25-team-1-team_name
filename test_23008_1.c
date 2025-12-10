/*
 * MCP23008 ULTIMATE DIAGNOSTIC
 * Function: Strictly checks every step of the I2C communication.
 * If grounding pins shows no reaction, this code will reveal the truth.
 */

#include <avr/io.h>
#include <util/delay.h>
#include <stdio.h>
#include "uart.h"

// ==========================================
// Settings
// ==========================================
#define MCP_ADDR_W  0x40 
#define MCP_ADDR_R  0x41 
#define MCP_GPIO    0x09 
#define MCP_IODIR   0x00
#define MCP_GPPU    0x06

#ifndef F_CPU
#define F_CPU 16000000UL
#endif
#define BAUD_PRESCALE 103 

// ==========================================
// I2C Driver (with strict error checking)
// ==========================================
void i2c_init(void) {
    PORTC |= (1 << 4) | (1 << 5); // Enable MCU internal pull-up resistors
    TWSR0 = 0x00;
    TWBR0 = 72;
    TWCR0 = (1<<TWEN);
}

uint8_t i2c_start(void) {
    uint16_t timeout = 0;
    TWCR0 = (1<<TWINT) | (1<<TWSTA) | (1<<TWEN);
    while (!(TWCR0 & (1<<TWINT))) {
        if (timeout++ > 20000) return 1; // Timeout
    }
    // Check status code: 0x08(START) or 0x10(REPEATED START)
    if ((TWSR0 & 0xF8) != 0x08 && (TWSR0 & 0xF8) != 0x10) return 2;
    return 0;
}

void i2c_stop(void) {
    TWCR0 = (1<<TWINT) | (1<<TWSTO) | (1<<TWEN);
}

uint8_t i2c_write(uint8_t data) {
    uint16_t timeout = 0;
    TWDR0 = data;
    TWCR0 = (1<<TWINT) | (1<<TWEN);
    while (!(TWCR0 & (1<<TWINT))) {
        if (timeout++ > 20000) return 1; // Timeout
    }
    // Check ACK: 0x18(SLA+W ACK), 0x28(DATA ACK), 0x40(SLA+R ACK)
    if ((TWSR0 & 0xF8) != 0x18 && (TWSR0 & 0xF8) != 0x28 && (TWSR0 & 0xF8) != 0x40)
        return 3; // No ACK (NACK)
    return 0;
}

uint8_t i2c_read_nack(void) {
    TWCR0 = (1<<TWINT) | (1<<TWEN);
    while (!(TWCR0 & (1<<TWINT)));
    return TWDR0;
}

// ==========================================
// Main Function
// ==========================================
int main(void) {
    char buffer[100];
    uint8_t val, error_code;

    UART_init(BAUD_PRESCALE);
    i2c_init();
    _delay_ms(100);

    UART_putstring("\r\n[DIAGNOSTIC MODE] Checking Connection...\r\n");

    // 1. Configuration Phase (with error messages)
    UART_putstring("Step 1: Setting Inputs... ");
    if (i2c_start() || i2c_write(MCP_ADDR_W) || i2c_write(MCP_IODIR) || i2c_write(0xFF)) {
        UART_putstring("FAIL! (Chip not responding)\r\n");
    } else {
        i2c_stop();
        UART_putstring("OK\r\n");
    }

    UART_putstring("Step 2: Enabling Pull-ups... ");
    if (i2c_start() || i2c_write(MCP_ADDR_W) || i2c_write(MCP_GPPU) || i2c_write(0xFF)) {
        UART_putstring("FAIL! (Chip not responding)\r\n");
    } else {
        i2c_stop();
        UART_putstring("OK\r\n");
    }

    UART_putstring("\r\n--- LIVE MONITOR ---\r\n");
    UART_putstring("Touch GROUND to: Pin 10 (GP0) through Pin 17 (GP7)\r\n");

    while (1) {
        error_code = 0;

        // Manual read sequence, checking every step
        if (i2c_start() != 0) error_code = 1;           // START failed
        else if (i2c_write(MCP_ADDR_W) != 0) error_code = 2; // No ACK for write address
        else if (i2c_write(MCP_GPIO) != 0) error_code = 3;   // No ACK for register
        else if (i2c_start() != 0) error_code = 4;           // REPEATED START failed
        else if (i2c_write(MCP_ADDR_R) != 0) error_code = 5; // No ACK for read address
        
        if (error_code != 0) {
            i2c_stop();
            sprintf(buffer, "READ ERROR: Code %d (Check Wires!)\r\n", error_code);
            UART_putstring(buffer);
        } else {
            // Only read if communication succeeded
            val = i2c_read_nack();
            i2c_stop();

            sprintf(buffer, "Hex: 0x%02X | ", val);
            UART_putstring(buffer);

            // Print which pin is grounded
            if (val == 0xFF) {
                UART_putstring("Status: ALL OPEN (Try grounding Pin 10–17)");
            } else {
                for (int i = 0; i <= 7; i++) {
                    if ((val & (1 << i)) == 0) {
                        sprintf(buffer, "[GP%d GROUNDED!] ", i);
                        UART_putstring(buffer);
                    }
                }
            }
            UART_putstring("\r\n");
        }

        _delay_ms(500);
    }
}
