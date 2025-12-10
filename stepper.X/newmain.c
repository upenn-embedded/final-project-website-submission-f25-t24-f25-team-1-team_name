#define F_CPU 16000000UL
#include <avr/io.h>
#include <util/delay.h>
#include "steppermotor.h"

int main(void){
    motor_init();

    while (1){
        init_pos();
        init_pos();
        init_pos();
        init_pos();
//        test();
//        _delay_ms(5000);
        while(1){}
    }
    return 0;
}