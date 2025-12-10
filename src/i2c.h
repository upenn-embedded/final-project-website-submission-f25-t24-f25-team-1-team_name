#ifndef I2C_H
#define I2C_H

#include <stdint.h>
// TWI Status Codes (relevant ones)
#define TWI_WRITE           0x00
#define TWI_READ            0x01
#define TWI_START_SENT      0x08
#define TWI_REP_START_SENT  0x10
#define TWI_MT_SLA_ACK      0x18
#define TWI_MT_DATA_ACK     0x28
#define TWI_MR_SLA_ACK      0x40 // Master Receive (Slave Address + Read bit) ACK received
#define TWI_MR_DATA_ACK     0x50 // Master Receive (Data) NACK received
#define TW_MR_DATA_NACK     0x58 // Master Receive (Data) NACK received
#define TWI_NO_INFO         0xF8

typedef enum {
    TWI_SUCCESS = 0,
    TWI_ERR_START,
    TWI_ERR_REPEAT_START,
    TWI_ERR_SLA_W_NACK,
    TWI_ERR_SLA_R_NACK,
    TWI_ERR_DATA_NACK,
    TWI_ERR_STATUS,
    TWI_ERR_TIMEOUT
} twi_error_t;

#define MCP23008_BASE_ADDR 0x20 // Base I2C address

#define MCP23008_IODIR_REG 0x00 // IODIR register address for setting direct
#define MCP23008_IPOL_REG  0x01
#define MCP23008_IOCON_REG 0x05
#define MCP23008_GPPU_REG  0x06 // GPPU register address

#define MCP23008_GPIO_REG  0x09  // GPIO register address for reading data

void TWI_init(void);
twi_error_t TWI_start(void);
void TWI_stop(void);
twi_error_t TWI_write(uint8_t data);
twi_error_t TWI_write_addr(uint8_t address, uint8_t rw_bit);
uint8_t twi_read_byte(uint8_t send_ack);
twi_error_t mcp_write_register(uint8_t addr_offset, uint8_t reg, uint8_t data);
uint8_t mcp_read_register(uint8_t addr_offset, uint8_t reg, twi_error_t* error_code);
twi_error_t initialize_mcp23008_inputs(uint8_t device_id);

#endif
