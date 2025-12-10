#include <avr/io.h>
#include <avr/interrupt.h>
#include <stdbool.h>
#ifndef F_CPU
#define F_CPU 16000000UL
#endif

#include <xc.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h> // For memcpy
#include <util/delay.h>
#include "uart.h"
#include "i2c.h"
#include "steppermotor.h"
#include "uart_esp.h"

#define NUM_MCP 8
#define GRAVEYARD_RANK '9'

typedef enum {
    STATE_IDLE,                 // Waiting for the first piece to be lifted
    STATE_PIECE_LIFTED,         // One piece has been lifted
    STATE_PIECE_CAPTURED,       // A second piece was lifted (capture scenario)
} GameState_t;

volatile bool move_ready_to_send = false;

int8_t g_start_row = -1, g_start_col = -1;
int8_t g_end_row = -1, g_end_col = -1;
int8_t g_captured_row = -1, g_captured_col = -1;
GameState_t g_current_state = STATE_IDLE;

// ISR variables
volatile bool perform_scan_flag = false;
volatile bool notmoving_flag = true;

void Timer4_Init(void) {
    //cli();
    TCCR4B |= (1 << WGM42); // CTC mode
    // Set Compare Match Register for 1 second (16,000,000 / 1024 prescaler)
    OCR4A = 7812; // Generates an interrupt every 15625 clock cycles
    // Enable Timer/Counter 1 Output Compare Match A Interrupt
    TIMSK4 |= (1 << OCIE4A);
    // Start timer with F_CPU/1024 prescaler
    TCCR4B |= (1 << CS42) | (1 << CS40);
    //sei();
}

ISR(TIMER4_COMPA_vect) {
    // This code runs every 1 seconds
    perform_scan_flag = true; 
    //printf("flag set %u \n", perform_scan_flag);
}

void disable_timer4_compa_interrupt(void) {
    // Clear the OCIE4A bit (Output Compare Interrupt Enable A for Timer 4)
    // using a bitwise AND operation with a bit mask.
    TIMSK4 &= ~(1 << OCIE4A); 
}

void enable_timer4_compa_interrupt(void) {
    // Optionally, you can re-enable it later using a bitwise OR operation.
    TIMSK4 |= (1 << OCIE4A);
}

void coords_to_chess_notation(int8_t row, int8_t col, char* buffer) {
    if (row >= 0 && row < 8 && col >= 0 && col < 8) {
        // Column (File) conversion: 'a' + 0 -> 'a', 'a' + 7 -> 'h'
        buffer[0] = 'a' + col; 
        // Row (Rank) conversion: '1' + 0 -> '1', '1' + 7 -> '8'
        buffer[1] = '1' + row; 
        buffer[2] = '\0'; // Null-terminate the string
    } else {
        // Handle invalid coordinates
        strncpy(buffer, "--", 3);
    }
}

void format_move_string(const char* start_pos_str, const char* end_pos_str, const char* capture_pos_str, char* output_buffer) {
    // Start with the move itself
    strcpy(output_buffer, start_pos_str);
    strcat(output_buffer, end_pos_str);

    // Append the capture coordinates if a capture occurred
    if (capture_pos_str != NULL && strlen(capture_pos_str) == 2) {
        strcat(output_buffer, capture_pos_str);
    }
}

void print_gpio_matrix(uint8_t *buffer) {
    printf("\n--Chess Board Status--\n");

    // Print board from rank 8 down to rank 1
    for (int8_t rank = 7; rank >= 0; rank--) {
        printf("%d| ", rank + 1);

        // Loop through files A -> H
        for (int8_t file = 0; file < 8; file++) {
            uint8_t column_byte = buffer[file];

            // Check the bit for this rank
            // bit = 1 ? piece present
            if ((column_byte >> rank) & 1) {
                printf("X ");
            } else {
                printf(". ");
            }
        }

        printf("|%d\n", rank + 1);
    }

    printf(" ------------------- \n");
    printf("   A B C D E F G H   \n");
}


void process_chess_command(char* input_line) {
    size_t len = strlen(input_line);
    char cmd_buffer[5]; // 4 chars + null terminator
    // CASE 1: Normal Move (e.g., "e2e4") - Length 4
    //init_pos();
    if (len == 4) {
        move_motor(input_line);
    }

    // CASE 2: Capture (e.g., "e5d6d5") - Length 6
    else if (len == 6) {    
        // Start Location: The captured square
        cmd_buffer[0] = input_line[4]; // Capture File (e.g., 'd')
        cmd_buffer[1] = input_line[5]; // Capture Rank (e.g., '5')
        
        // End Location: Graveyard
        // File: Same as start file (This satisfies "Nearest File")
        // Rank: Fixed at Rank 9
        cmd_buffer[2] = input_line[4]; 
        cmd_buffer[3] = GRAVEYARD_RANK; 
        
        cmd_buffer[4] = '\0'; 
        
        move_motor(cmd_buffer);
        // Copy the first 4 chars (e.g., "e5d6")
        strncpy(cmd_buffer, input_line, 4);
        cmd_buffer[4] = '\0';
        
        move_motor(cmd_buffer);
    }

    // CASE 3: Castle (e.g., "e1g1h1f1") - Length 8
    else if (len == 8) {
        // Move the King (First 4 chars)
        strncpy(cmd_buffer, input_line, 4);
        cmd_buffer[4] = '\0';
        move_motor(cmd_buffer);

        // Move the Rook (Last 4 chars)
        strncpy(cmd_buffer, input_line + 4, 4);
        cmd_buffer[4] = '\0';
        move_motor(cmd_buffer);
    }
}

int main(void) {
    //cli();
    uart_init();
    printf("serial print uart init\n");
    uart1_init();
    printf("serial coms to ESP started\n");
    Timer4_Init();
    printf("timer4 init\n");
    TWI_init();
    printf("TWI init\n");
    motor_init();
    //init_pos();
    printf("init\n");
    
    // sei();
    // init_pos();
    printf("motor init\n");
    uint8_t board_status_buffer[8] = {0xFF};       // Current state
    uint8_t base_board_state[8] = {0xFF};      // The state after the last *validated* move

    twi_error_t status;
    twi_error_t read_status;
    
    //int8_t captured_piece_row = -1, captured_piece_col = -1; // Track the captured piece location

    print_gpio_matrix(&board_status_buffer);
    // GPIO expander initialization
    for (uint8_t i = 0; i < NUM_MCP; i++) {
        status = initialize_mcp23008_inputs(i);
        if (status != TWI_SUCCESS) {
            // Handle initialization error (e.g., LED warning)
            printf("ERROR: on chip %u, status: %u\n", i, status);
            DDRB |= (1 << PB5); 
            PORTB |= (1 << PB5); 
        } else {
            printf("TWI on %u START SUCESS\n", i);
        }
    }
    
    // Perform an initial scan to populate the old buffer before the loop starts
    for (uint8_t j = 0; j < NUM_MCP; j++) {
        uint8_t data = (mcp_read_register(j, MCP23008_GPIO_REG, &status));
        if (status == TWI_SUCCESS) {
            base_board_state[j] = data;
        } else {
            base_board_state[j] = 0xAA; // Use an error placeholder pattern
        }
    }
    memcpy(&board_status_buffer, &base_board_state, sizeof(base_board_state));
    
    printf("Initial board state captured.\n");
    print_gpio_matrix(&board_status_buffer);
    
    char start_pos_str[3];
    char end_pos_str[3];
    char capture_pos_str[3];
    char move_string_buffer[8];
    char line[64];
    g_current_state = STATE_IDLE;
    sei();
    while (1) {
        
        if (uart1_readline(line, sizeof(line))) {
        //    disable_timer4_compa_interrupt;
            printf("Received from ESP32: %s\n", line);
        //    process_chess_command(line);
        //    enable_timer4_compa_interrupt;
        }
        
        if (perform_scan_flag && notmoving_flag) {
            // pause interrupts. printing can take time.
            perform_scan_flag = false;
            // Scan the hardware and populate board_status_buffer
            int8_t current_scan_removed_count = 0;
            int8_t current_scan_added_count = 0;
            int8_t observed_removed_row = -1, observed_removed_col = -1;
            int8_t observed_added_row = -1, observed_added_col = -1;
            uint8_t col = 0;
            uint8_t diff = 0x00;
            for (col = 0; col < NUM_MCP; col++) {
                uint8_t data = (mcp_read_register(col, MCP23008_GPIO_REG, &read_status));
                if (read_status == TWI_SUCCESS) {
                    diff = data ^ base_board_state[col] ;
                    if (diff > 0) {
                        for (uint8_t row = 0; row < 8; row++) {
                            if ((diff >> row) & 1) {
                                if ((base_board_state[col] >> row) & 1) {
                                    current_scan_removed_count++;
                                    observed_removed_row = row;
                                    observed_removed_col = col;
                                } else {
                                    current_scan_added_count++;
                                    observed_added_row = row;
                                    observed_added_col = col;
                                }
                            }
                        }
                    }
                    board_status_buffer[col] = data;
                }
            }
            // printf("%u\n",diff);
            //if (current_scan_removed_count != 0 || current_scan_added_count !=0) {
            //    printf("removed: %u, added %u \n",current_scan_removed_count,current_scan_added_count);
            //}
            //printf("removed: %u, added %u \n",current_scan_removed_count,current_scan_added_count);
            switch (g_current_state) {
                case STATE_IDLE:
                    // Look for exactly one piece being lifted from the base state
                    if (current_scan_removed_count == 1 && current_scan_added_count == 0) {
                        g_start_row = observed_removed_row;
                        g_start_col = observed_removed_col;
                        g_current_state = STATE_PIECE_LIFTED;
                        coords_to_chess_notation(g_start_row, g_start_col, start_pos_str);
                        
                        printf("STATE: Piece lifted at %s. Waiting for placement/capture.\n", start_pos_str);
                        print_gpio_matrix(&board_status_buffer);
                    } else if (current_scan_added_count > 0) {
                        print_gpio_matrix(&board_status_buffer);
                        memcpy(&base_board_state, &board_status_buffer, sizeof(board_status_buffer));
                    }
                    break;
                case STATE_PIECE_LIFTED:
                    if (current_scan_added_count == 1 && current_scan_removed_count == 1) {
                        // A single piece was placed back down (standard move completion)
                        g_end_row = observed_added_row;
                        g_end_col = observed_added_col;
                        if (g_start_row != g_end_row || g_start_col != g_end_col) {
                            coords_to_chess_notation(g_start_row, g_start_col, start_pos_str);
                            coords_to_chess_notation(g_end_row, g_end_col, end_pos_str);
                            
                            format_move_string(start_pos_str, end_pos_str, NULL, move_string_buffer);
                            printf("STATE: Standard Move Complete! Move: %s\n", move_string_buffer);
                            // TX move to ESP HERE
                            uart1_send_string(move_string_buffer);
                                    
                            g_current_state = STATE_IDLE;
                            print_gpio_matrix(&board_status_buffer);
                            memcpy(&base_board_state, &board_status_buffer, sizeof(board_status_buffer));
                        } else {
                            printf("INFO: Piece returned to original position. Back to IDLE.\n");
                            g_current_state = STATE_IDLE;
                        }
                        print_gpio_matrix(&board_status_buffer);
                        memcpy(&base_board_state, &board_status_buffer, sizeof(board_status_buffer));
                    } else if (current_scan_removed_count == 2 && current_scan_added_count == 0) {
                        // A second piece was lifted before the first was placed (a capture scenario start)
                        g_captured_row = observed_removed_row;
                        g_captured_col = observed_removed_col;
                        coords_to_chess_notation(g_captured_row, g_captured_col, capture_pos_str);
                        printf("STATE: Second piece lifted (Capture detected at %s).\n", capture_pos_str);
                        g_current_state = STATE_PIECE_CAPTURED;
                        print_gpio_matrix(&board_status_buffer);
                        memcpy(&base_board_state, &board_status_buffer, sizeof(board_status_buffer));
                        

                    } else if (current_scan_added_count > 1 || current_scan_removed_count > 2) {
                        // Multiple ambiguous changes, reset state machine for safety
                        printf("INFO: Ambiguous changes or noise while waiting for move completion. Resetting state.\n");
                        g_current_state = STATE_IDLE;
                        // Force a resync of the base state to the current physical state, abandoning partial moves.
                        print_gpio_matrix(&board_status_buffer);
                        memcpy(&base_board_state, &board_status_buffer, sizeof(board_status_buffer));
                    } else {
                        //printf("STATE: PIECE_LIFTED \n");
                    }
                    break;
                case STATE_PIECE_CAPTURED:
                    //printf("removed: %u, added %u \n",current_scan_removed_count,current_scan_added_count);
            
                    // We are waiting ONLY for a single piece to be placed back down
                    if (current_scan_added_count == 1 && current_scan_removed_count == 0) {
                        // The move is finished
                        g_end_row = observed_added_row;
                        g_end_col = observed_added_col;

                        coords_to_chess_notation(g_start_row, g_start_col, start_pos_str);
                        coords_to_chess_notation(g_end_row, g_end_col, end_pos_str);
                        coords_to_chess_notation(g_captured_row, g_captured_col, capture_pos_str);
                        
                        format_move_string(start_pos_str, end_pos_str, capture_pos_str, move_string_buffer);
                        printf("STATE: Capture Move Complete. Move: %s\n", move_string_buffer);
                        
                        // TX move to ESP HERE
                        uart1_send_string(move_string_buffer);
                        g_current_state = STATE_IDLE;
                        g_captured_row = -1; g_captured_col = -1;
                        g_start_row = -1; g_start_col = -1;
                        g_end_row = -1; g_end_col = -1;
                        
                        // Sync the base state to the new board layout
                        print_gpio_matrix(&board_status_buffer);
                        memcpy(&base_board_state, &board_status_buffer, sizeof(board_status_buffer));
                    } else {
                        //printf("STATE: PIECE_CAPTURED \n");
                    }
                    break;
                // case statement end
            }
            // print_gpio_matrix(&board_status_buffer);
        }
        
    }
    
    return 0;
}
