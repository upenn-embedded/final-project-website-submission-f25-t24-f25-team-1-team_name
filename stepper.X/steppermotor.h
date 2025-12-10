#include <avr/io.h>
#include <stdint.h>

void motor_init(void);
void rotate1(float turns, uint8_t dir);
void rotate2(float turns, uint8_t dir);
void init_pos(void);
void wait_stop_1(void);
void wait_stop_2(void);
void y_axis(float squares,  uint8_t dir);
void x_axis(float squares,  uint8_t dir);
void test(void);
void move_motor(char* line);

