#ifndef UART_ESP_H_
#define UART_ESP_H_

#include <xc.h>
#include <stdint.h>

void uart1_init(void);
void uart1_send_byte(uint8_t d);
void uart1_send_string(const char *s);
uint8_t uart1_readline(char *out, uint8_t maxlen);

#endif /* UART_ESP_H_ */
