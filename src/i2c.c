#include "i2c.h"
#include "uart.h"
#include <avr/io.h>
#include <util/delay.h>
#include <stdint.h>


void TWI_init(void) {
    // Set TWI clock to 100kHz (for 16MHz F_CPU and prescaler = 1)
    // Formula: SCL frequency = CPU clock frequency / (16 + 2 * TWBR * PrescalerValue)
    // TWBR = ((F_CPU / 100000UL) - 16) / 2 = ((16000000 / 100000) - 16) / 2 = (160 - 16) / 2 = 72
    PORTC |= (1 << PC4) | (1 << PC5);
    TWBR0 = 72;
    TWSR0 = 0x00; // Prescaler set to 1 (TWPS bits 0 and 1 are 0)
    TWCR0 = (1 << TWEN); // Enable TWI
}

twi_error_t TWI_start(void) {
    uint16_t timeout = 0;
    TWCR0 = (1 << TWINT) | (1 << TWSTA) | (1 << TWEN); // Send START condition
    while (!(TWCR0 & (1<<TWINT))) {
        if (timeout++ > 20000) {
            return TWI_ERR_START; // Timeout
        }
    }
    // Check status code: 0x08(START) or 0x10(REPEATED START)
    if ((TWSR0 & 0xF8) != TWI_START_SENT && (TWSR0 & 0xF8) != TWI_REP_START_SENT){
        return TWI_ERR_START;
    }
    return TWI_SUCCESS;
}

void TWI_stop(void) {
    TWCR0 = (1 << TWINT) | (1 << TWSTO) | (1 << TWEN); // Send STOP condition
}

// Sends a byte of data over TWI.
twi_error_t TWI_write(uint8_t data) {
    uint16_t timeout = 0;
    TWDR0 = data; // Load data into TWI Data Register
    TWCR0 = (1 << TWINT) | (1 << TWEN); // Start transmission
    while (!(TWCR0 & (1 << TWINT))){
        if (timeout++ > 20000) return 1; // Timeout
    } // Wait for TWINT flag set
    // Check ACK: 0x18(SLA+W ACK), 0x28(DATA ACK), 0x40(SLA+R ACK)
    if ((TWSR0 & TWI_NO_INFO) != TWI_MT_SLA_ACK && (TWSR0 & TWI_NO_INFO) != TWI_MT_DATA_ACK && (TWSR0 & 0xF8) != TWI_MR_SLA_ACK) {
        return TWI_ERR_DATA_NACK; // No ACK (NACK)
    }
    return TWI_SUCCESS;
}

twi_error_t TWI_write_addr(uint8_t address, uint8_t rw_bit) {
    // Shift address left by 1 and set R/W bit to 0 (write)
    uint8_t twi_addr = (address << 1) + rw_bit; 
    TWDR0 = twi_addr; // Load address into TWI Data Register
    TWCR0 = (1 << TWINT) | (1 << TWEN); // Start transmission
    while (!(TWCR0 & (1 << TWINT)));
    if ((rw_bit == TWI_WRITE) && ((TWSR0 & TWI_NO_INFO) != TWI_MT_SLA_ACK)) {
        return TWI_ERR_SLA_W_NACK;
    }
    if  ((rw_bit == TWI_READ) && ((TWSR0 & TWI_NO_INFO) != TWI_MR_SLA_ACK)) {
        return TWI_ERR_SLA_R_NACK;
    }
    return TWI_SUCCESS;
}

uint8_t twi_read_byte(uint8_t send_ack) {
    if (send_ack == 1) {
        TWCR0 = (1 << TWINT) | (1 << TWEN) | (1 << TWEA);
    } else {
        TWCR0 = (1 << TWINT) | (1 << TWEN);
    }
    while (!(TWCR0 & (1 << TWINT))); // Wait for TWI interrupt flag
    return TWDR0;
}

twi_error_t initialize_mcp23008_inputs(uint8_t device_id) {
    twi_error_t status;
    // Set all 8 pins to inputs by writing 0xFF to the IODIR register
    status = mcp_write_register(device_id, MCP23008_IODIR_REG, 0xFF);
    if (status != TWI_SUCCESS) return status;
    _delay_ms(100);
    //Enable internal pull-up resistors for all input pins (0xFF)
    status = mcp_write_register(device_id, MCP23008_GPPU_REG, 0xFF);
    if (status != TWI_SUCCESS) return status;
    _delay_ms(100);
    // Set active low
    status = mcp_write_register(device_id, MCP23008_IPOL_REG, 0xFF);
    if (status != TWI_SUCCESS) return status;
    _delay_ms(100);
    
    return TWI_SUCCESS;
}

twi_error_t mcp_write_register(uint8_t addr_offset, uint8_t reg, uint8_t data) {
    uint8_t full_addr = MCP23008_BASE_ADDR + addr_offset;
    twi_error_t error;

    if ((error = TWI_start()) != TWI_SUCCESS) goto exit_err;
    if ((error = TWI_write_addr(full_addr, TWI_WRITE)) != TWI_SUCCESS) goto exit_err;
    if ((error = TWI_write(reg)) != TWI_SUCCESS) goto exit_err;
    if ((error = TWI_write(data)) != TWI_SUCCESS) goto exit_err;

    TWI_stop();
    _delay_ms(100);
    return TWI_SUCCESS;
    
    exit_err:
        TWI_stop();
        _delay_ms(100);
        //printf("write_register fail\n");
        return error;
}

uint8_t mcp_read_register(uint8_t addr_offset, uint8_t reg, twi_error_t* error_code) {
    uint8_t full_addr = MCP23008_BASE_ADDR + addr_offset;
    twi_error_t error;
    uint8_t data = 0;

    if ((error = TWI_start()) != TWI_SUCCESS) goto exit_err;
    if ((error = TWI_write_addr(full_addr, TWI_WRITE)) != TWI_SUCCESS) goto exit_err;
    if ((error = TWI_write(reg)) != TWI_SUCCESS) goto exit_err;
    
    // Repeated start condition for reading
    if ((error = TWI_start()) != TWI_SUCCESS) goto exit_err;
    if ((error = TWI_write_addr(full_addr, TWI_READ)) != TWI_SUCCESS) goto exit_err;

    // Read one byte, sending NACK as it's the only byte being read
    data = twi_read_byte(0); // false means send NACK after receiving

    TWI_stop();
    *error_code = TWI_SUCCESS;
    return data;
    
    exit_err:
        TWI_stop();
        //printf("read_register fail\n");
        *error_code = error;
        return 0;
}

