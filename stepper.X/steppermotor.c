#include "steppermotor.h"
#include <avr/interrupt.h>
#include <util/delay.h>
#include <string.h>
#include <stdlib.h>
//#include "uart.h"
/*
PD2: Switch for Motor 1
PD3: Switch for Motor 2
PB1: Motor 1 DIR
PD5: Motor 1 STEP
PD6: Motor 2 DIR
PD0: Motor 2 STEP
*/
#define F_CPU 16000000UL

volatile uint32_t counter1 = 0;
volatile uint32_t counter2 = 0;

//stepper motor position tracking variables
uint32_t global_step_pos_x = 0;
uint32_t global_step_pos_y = 0;

const uint32_t step_per_square = 1600; //200 steps per rev, 8 microsteps, 1 rev per 4mm, 20mm per square

int square_to_steps(int squares){
    return squares * step_per_square;
}

int squares_to_counts(int squares){
    return squares * 200 * 2;
}

void motor_init(void) {
    DDRD |= (1 << PD1);
    DDRD |= (1 << PD4) | (1 << PD6);    
    DDRD &= ~((1 << PD2) | (1 << PD3));
    PORTD |= (1 << PD2) | (1 << PD3);
    DDRB |= (1 << PB1);
    DDRD |= (1 << PD0);
    EICRA |= (1 << ISC11) | (1 << ISC01);
    EICRA &= ~((1 << ISC10) | (1 << ISC00));
    TCCR1A = 0; 
    TCCR1B = (1 << WGM12) | (1 << CS11) | (1 << CS10); 
    OCR1A = (16000000UL / (2 * 64 * 200)) - 1; 
    TIMSK1 |= (1 << OCIE1A);
    PRR1 &= ~(1 << PRTIM3);
    TCCR3A = 0;
    TCNT3 = 0;
    TCCR3B = (1 << WGM32) | (1 << CS31) | (1 << CS30);
    OCR3A = (16000000UL / (2 * 64 * 200)) - 1;
    TIMSK3 |= (1 << OCIE3A);     
    sei();
}

void wait_stop_1(void){
    while (counter1 > 0);
}
void wait_stop_2(void){
    while (counter2 > 0);
}

void rotate1(float turns, uint8_t dir){
    wait_stop_1();
    if (dir){
        PORTD |= (1 << PD4);
    }
    else{
        PORTD &= ~(1 << PD4);    
    }
    
    counter1 = turns * 200 * 2;
    if (counter1 > 0) {
        TCCR1A |= (1 << COM1A0);
    }
}


void rotate2(float turns, uint8_t dir){
    wait_stop_2();
    if (dir){
        PORTD |= (1 << PD6);
    }
    else{
        PORTD &= ~(1 << PD6);
    }
    counter2 = turns * 200 * 2;
    if (counter2 > 0) {
        TCCR3A |= (1 << COM3A0);
    }
}

ISR(TIMER1_COMPA_vect){
    if (counter1 > 0){
        counter1--;
    } 
    else {
        TCCR1A &= ~(1 << COM1A0);
    }
}

ISR(TIMER3_COMPA_vect){
    if (counter2 > 0){
        counter2--;
    }
    else {
        TCCR3A &= ~(1 << COM3A0);
    }
}

ISR(INT0_vect){
    counter1 = 0;
    counter2 = 0;
    TCCR1A &= ~(1 << COM1A0);
    TCCR3A &= ~(1 << COM3A0);
}

ISR(INT1_vect) {
    counter2 = 0; 
    counter1 = 0;
    TCCR1A &= ~(1 << COM1A0);
    TCCR3A &= ~(1 << COM3A0);
}

void x_axis(float squares,  uint8_t dir){
    //1 turn = 4.2cm 
    //1 square = 3.7cm
    float turns = 0.9 * squares;
    rotate1(turns, dir);
    rotate2(turns, dir);
//    if (dir) global_step_pos_x += square_to_steps(squares);
//    else     global_step_pos_x -= square_to_steps(squares);
}

void y_axis(float squares,  uint8_t dir){
    //1 turn = 4cm
    //1 square = 3.7cm
    float turns = 0.925 * squares;
    uint8_t dir1 = 1;
    uint8_t dir2 = 0;
    if (dir){
        dir1 = 1;
        dir2 = 0;
    }
    else{
        dir1 = 0;
        dir2 = 1;
    }
    rotate1(turns, dir1);
    rotate2(turns, dir2);
//    if (dir) global_step_pos_x += square_to_steps(squares);
//    else     global_step_pos_x -= square_to_steps(squares);    
}

void test(void){
    char test_line[] = "g7e5";
    move_motor(test_line);
    _delay_ms(5000);    
//    x_axis(1.0, 0);
}

void init_pos(void){
    if(PIND & (1 << PD2)){
        EIFR |= (1 << INTF0);
        EIMSK |= (1 << INT0);    
        x_axis(100, 1);
        wait_stop_1(); 
        wait_stop_2();

        EIMSK &= ~(1 << INT0);

        global_step_pos_x = 0;
        _delay_ms(1000);
    }
    
    if(PIND & (1 << PD3)){
        EIFR |= (1 << INTF1);
        EIMSK |= (1 << INT1);
        y_axis(100, 1);
        wait_stop_1();
        wait_stop_2();

        EIMSK &= ~(1 << INT1);

        global_step_pos_y = 0;

        _delay_ms(500);
    }
    
    EIFR |= (1 << INTF1);
    EIMSK |= (1 << INT1);
    y_axis(0.811, 0);
    wait_stop_1();
    wait_stop_2();
    
    EIMSK &= ~(1 << INT1);
    
    global_step_pos_y = 0;
    
    _delay_ms(500);
}

void move_motor(char* line) {
    uint8_t start_x = line[0] - 'a'; 
    uint8_t start_y = line[1] - '1'; 
    uint8_t end_x   = line[2] - 'a';
    uint8_t end_y   = line[3] - '1';
    
    float dist_x = 7.0 - (float)start_x;
    if (dist_x > 0) {
        x_axis(dist_x, 0);
        wait_stop_1(); 
        wait_stop_2(); 
        _delay_ms(10000);
    }

    float dist_y = 7.0 - (float)start_y;
    if (dist_y > 0) {
        y_axis(dist_y, 0);
        wait_stop_1(); 
        wait_stop_2(); 
        _delay_ms(10000);
    }

    PORTD |= (1 << PD1); 
    _delay_ms(100000); 
    
    uint8_t enable_x_offset = 1;
    uint8_t enable_y_offset = 1;
    if (start_y == end_y){
        enable_x_offset = 0;
    }    
    if (start_x == end_x){
        enable_y_offset = 0;
    }
    

    float current_off_x = 0.0;
    float current_off_y = 0.0;

    if (enable_x_offset) {
        if (start_x == 0) {
            x_axis(0.5, 1); 
            current_off_x = 0.5;
        } else {
            x_axis(0.5, 0); 
            current_off_x = -0.5;
        }
        wait_stop_1(); wait_stop_2();
        _delay_ms(10000);
    }

    if (enable_y_offset) {
        y_axis(0.5, 0); 
        current_off_y = -0.5;
        wait_stop_1(); wait_stop_2();
        _delay_ms(10000);
    }

    float target_off_x = 0.0;
    float target_off_y = 0.0;

    if (enable_x_offset) {
        if (end_x == 0) target_off_x = 0.5;   
        else target_off_x = -0.5;             
    }

    if (enable_y_offset) {
        target_off_y = -0.5; 
    }

    float move_x = ((float)end_x + target_off_x) - ((float)start_x + current_off_x);
    float move_y = (float)end_y - (float)start_y ;
    
    uint8_t dir = 0;
    if (move_x != 0) {
        if (move_x > 0){
            dir = 1;
        }
        x_axis(fabs(move_x), dir);
        wait_stop_1(); wait_stop_2();
        _delay_ms(10000);
    }
    dir = 0;
    if (move_y != 0) {
        if (move_y > 0){
            dir = 1;
        }
        y_axis(fabs(move_y), dir);
        wait_stop_1(); wait_stop_2();
        _delay_ms(10000);
    }

    if (target_off_y != 0) {
        y_axis(0.5, 1);
        wait_stop_1(); wait_stop_2();
        _delay_ms(10000);
    }

    if (target_off_x != 0) {
        x_axis(0.5, (target_off_x > 0) ? 0 : 1);
        wait_stop_1(); wait_stop_2();
        _delay_ms(10000);
    }

    PORTD &= ~(1 << PD1); 
    _delay_ms(10000);   
    init_pos();
}