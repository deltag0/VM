#include <iostream>
#include <string>
#include <fstream>
#include <sstream>
#include <bitset>
#include <signal.h>
#include <cstdint>
#include <stdexcept>
#include <Windows.h>
#include <conio.h>

using std::string;

enum
{
    R_R0 = 0,
    R_R1,
    R_R2,
    R_R3,
    R_R4,
    R_R5,
    R_R6,
    R_R7,
    R_PC, /* program counter */
    R_COND,
    R_COUNT
};
enum
{
    FL_POS = 1 << 0, /* P */
    FL_ZRO = 1 << 1, /* Z */
    FL_NEG = 1 << 2, /* N */
};
enum
{
    OP_BR = 0, /* branch */
    OP_ADD,    /* add  */
    OP_LD,     /* load */
    OP_ST,     /* store */
    OP_JSR,    /* jump register */
    OP_AND,    /* bitwise and */
    OP_LDR,    /* load register */
    OP_STR,    /* store register */
    OP_RTI,    /* unused */
    OP_NOT,    /* bitwise not */
    OP_LDI,    /* load indirect */
    OP_STI,    /* store indirect */
    OP_JMP,    /* jump */
    OP_RES,    /* reserved (unused) */
    OP_LEA,    /* load effective address */
    OP_TRAP    /* execute trap */
};

enum
{
    MR_KBSR = 0xFE00, /* keyboard status */
    MR_KBDR = 0xFE02  /* keyboard data */
};
enum
{
    TRAP_GETC = 0x20,  /* get character from keyboard, not echoed onto the terminal */
    TRAP_OUT = 0x21,   /* output a character */
    TRAP_PUTS = 0x22,  /* output a word string */
    TRAP_IN = 0x23,    /* get character from keyboard, echoed onto the terminal */
    TRAP_PUTSP = 0x24, /* output a byte string */
    TRAP_HALT = 0x25   /* halt the program */
};

#define MEMORY_MAX (1 << 16)
uint16_t memory[MEMORY_MAX];  /* 65536 locations */
uint16_t reg[R_COUNT];

HANDLE hStdin = INVALID_HANDLE_VALUE;
DWORD fdwMode, fdwOldMode;

void disable_input_buffering()
{
    hStdin = GetStdHandle(STD_INPUT_HANDLE);
    GetConsoleMode(hStdin, &fdwOldMode); /* save old mode */
    fdwMode = fdwOldMode
            ^ ENABLE_ECHO_INPUT  /* no input echo */
            ^ ENABLE_LINE_INPUT; /* return when one or
                                    more characters are available */
    SetConsoleMode(hStdin, fdwMode); /* set new mode */
    FlushConsoleInputBuffer(hStdin); /* clear buffer */
}

void restore_input_buffering()
{
    SetConsoleMode(hStdin, fdwOldMode);
}

uint16_t check_key()
{
    return WaitForSingleObject(hStdin, 1000) == WAIT_OBJECT_0 && _kbhit();
}
void handle_interrupt(int signal)
{
    restore_input_buffering();
    printf("\n");
    exit(-2);
}
uint16_t sign_extend(uint16_t x, int bit_count)
{
    if ((x >> (bit_count - 1)) & 1) {
        x |= (0xFFFF << bit_count);
    }
    return x;
}
void swap16(uint16_t &x)
{
    x = (x << 8) | (x >> 8);
}
void update_flags(uint16_t r)
{
    if (reg[r] == 0)
    {
        reg[R_COND] = FL_ZRO;
    }
    else if (reg[r] >> 15) /* a 1 in the left-most bit indicates negative */
    {
        reg[R_COND] = FL_NEG;
    }
    else
    {
        reg[R_COND] = FL_POS;
    }
}

void read_image_file(std::ifstream &ifs) {
    uint16_t origin;

    // big endian to little endian
    ifs.read(reinterpret_cast<char *>(&origin), sizeof(origin));

    swap16(origin); // Swap is needed if the input is in big endian, then we want to convert it into little endian

    uint16_t *max_read = memory + MEMORY_MAX - 1;
    uint16_t *p = memory + origin;

    while (p != max_read && ifs) {
        ifs.read(reinterpret_cast<char *>(p), sizeof(origin));
        swap16(*p); // same as above
        p++;
    }
}

int read_image(std::ifstream &ifs) {
    if (!ifs) {
        throw std::runtime_error("Invalid File provided");
    }

    read_image_file(ifs);
    return 1;
}

void mem_write(uint16_t address, uint16_t val)
{
    memory[address] = val;
}

uint16_t mem_read(uint16_t address)
{
    if (address == MR_KBSR)
    {
        if (check_key())
        {
            memory[MR_KBSR] = (1 << 15);
            memory[MR_KBDR] = getchar();
        }
        else
        {
            memory[MR_KBSR] = 0;
        }
    }
    return memory[address];
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        throw std::runtime_error("No image provided");
    }
    signal(SIGINT, handle_interrupt);
    disable_input_buffering();


    string file_name = argv[1];

    std::ifstream ifs{file_name, std::ios::binary};

    read_image(ifs);

    // since exactly one condition flag should be set at any given time, set the Z flag
    reg[R_COND] = FL_ZRO;

    // set the PC to starting position 
    // 0x3000 is the default -> 0011 0000 0000 0000
    enum { PC_START = 0x3000 };
    reg[R_PC] = PC_START;

    int running = 1;
    while (running) {
        /* FETCH */
        uint16_t instr = mem_read(reg[R_PC]++); // this is fine because we're R_PC doesn't mean anything. The actual R_PC address is what's in the registry
        uint16_t op = instr >> 12;

        switch (op) {
            case OP_ADD: {
                // our instructions:
                // first 4 bits -> tell us we're adding
                // next 3 bits -> tell us we're location of where we're storing the sum
                // next 3 bits -> register with first number
                // next bit -> mode
                // FOR REGISTER -> NEXT 2 BITS IS FILLER AND LAST 3 ARE REGISTER OF SECOND NUMBER
                // FOR IMMEDIATE -> VALUE EMBEDDED IN LOCATION OF ADDRESS
                // 0x7 -> 0111 (i.e useful for finding registry address)

                // storing regiter
                uint16_t r0 = (instr >> 9) & 0x7;

                // register address of second number to add
                uint16_t r1 = (instr >> 6) & 0x7;

                // check mode. 1 -> immeediate mode, 0 -> register mode
                uint16_t imm_flag = (instr >> 5) & 0x1;

                if (imm_flag) {
                    uint16_t imm5 = sign_extend(instr & 0x1F, 5); // second value (max of 2^5)
                    reg[r0] = reg[r1] + imm5;
                }
                else {
                    uint16_t r2 = instr & 0x7; // returns register address of second number to add

                    reg[r0] = reg[r1] + reg[r2];
                }

                update_flags(r0);
                break;
            }

            case OP_AND: {

                uint16_t r0 = (instr >> 9) & 0x7;

                uint16_t r1 = (instr >> 6) & 0x7;

                uint16_t mode = (instr >> 5) & 0x1;

                if (mode) {
                    reg[r0] = reg[r1] & sign_extend(instr & 0x1F, 5);
                }
                else {
                    uint16_t r2 = instr & 0x7;

                    reg[r0] = reg[r1] & reg[r2];
                }

                update_flags(r0);
                break;
            }

            case OP_NOT: {
                uint16_t r0 = (instr >> 9) & 0x7;

                uint16_t r1 = (instr >> 6) & 0x7;

                reg[r0] = ~reg[r1];
                update_flags(r0);
                break;
            }

            case OP_BR: {
                uint16_t offset = sign_extend(instr & 0x1FF, 9);
                uint16_t p = (instr >> 9) & 0x7;
                if (p & reg[R_COND]) {
                    reg[R_PC] += offset;
                }

                break;
            }

            case OP_JMP: {
                uint16_t baseR = (instr >> 6) & 0x7;
                reg[R_PC] = reg[baseR];
                break;
            }

            case OP_JSR: {
                uint16_t mode = (instr >> 11) & 0x1;
                reg[R_R7] = reg[R_PC];

                if (mode) {
                    reg[R_PC] += sign_extend(instr & 0x7FF, 11);
                }
                else {
                    uint16_t baseR = (instr >> 6) & 0x7;
                    reg[R_PC] = reg[baseR];
                }
                break;
            }

            case OP_LD: {
                uint16_t r0 = (instr >> 9) & 0x7;

                reg[r0] = mem_read(reg[R_PC] + (sign_extend(instr & 0x1FF, 9)));
                update_flags(r0);
                break;
            }

            case OP_LDI: {
                // storing registry
                uint16_t r0 = (instr >> 9) & 0x7;

                // offset address from current PC
                uint16_t pc9 = sign_extend(instr & 0x1FF, 9);

                // as you can see, the offset is only 9 bits, so the data from the address the offset is pointing to CANNOT be too far
                reg[r0] = mem_read(mem_read(reg[R_PC] + pc9));
                update_flags(r0);
                break;
            }

            case OP_LDR: {
                uint16_t r0 = (instr >> 9) & 0x7;
                uint16_t baseR = (instr >> 6) & 0x7;
                uint16_t offset = sign_extend(instr & 0x3F, 6);

                reg[r0] = mem_read(reg[baseR] + offset);
                update_flags(r0);
                break;
            }

            case OP_LEA: {
                uint16_t r0 = (instr >> 9) & 0x7;

                reg[r0] = reg[R_PC] + sign_extend(instr & 0x1FF, 9);
                update_flags(r0);
                break;
            }

            case OP_ST: {
                uint16_t r0 = (instr >> 9) & 0x7;

                mem_write(reg[R_PC] + sign_extend(instr & 0x1FF, 9), reg[r0]);
                break;
            }

            case OP_STI: {
                uint16_t r0 = (instr >> 9) & 0x7;

                mem_write(mem_read(reg[R_PC] + sign_extend(instr & 0x1FF, 9)), reg[r0]);
                break;
            }
                
            case OP_STR: {
                uint16_t r0 = (instr >> 9) & 0x7;
                uint16_t baseR = (instr >> 6) & 0x7;

                mem_write(reg[baseR] + sign_extend(instr & 0x3F, 6), reg[r0]);
                break;
            }

            case OP_TRAP: {
                reg[R_R7] = reg[R_PC];
                switch (instr & 0xFF) {
                    case TRAP_GETC: {
                        char c;

                        std::cin >> c;
                        // while (!(std::cin >> c)) {
                        //     std::cerr << "Invalid input\n";
                        //     std::cin.clear();
                        //     std::cin.ignore();
                        // }

                        reg[R_R0] = (uint16_t)c;
                        update_flags(R_R0);

                        break;
                    }

                    case TRAP_OUT: {
                        std::cout << (char)reg[R_R0];
                        break;
                    }

                    case TRAP_PUTS: {
                        uint16_t *c = memory + reg[R_R0];

                        while (*c != 0x0000) {
                            char curr = *c;

                            std::cout << curr;
                            c++;
                        }

                        break;
                    }

                    case TRAP_IN: {
                        std::cout << "Enter a character" << std::endl;
                        char c;

                        std::cin >> c;
                        // while (!(std::cin >> c)) {
                        //     std::cerr << "Invalid input" << std::endl;
                        //     std::cin.clear();
                        //     std::cin.ignore();
                        // }
                        std::cout << c << '\n';
                        reg[R_R0] = (uint16_t)c;
                        update_flags(R_R0);

                        break;
                    }

                    case TRAP_PUTSP: {
                        uint16_t *c = memory + reg[R_R0];
                        // assume each memory address stores 2 characters. 1 character in 1 byte, like in modern systems

                        while (*c != 0x0000) {
                            char char1 = (*c) & 0xFF;
                            char char2 = (*c) >> 8;
                            std::cout << char1;
                            if (char2) std::cout << char2;
                            c++;
                        }

                        break;
                    }

                    case TRAP_HALT:
                        std::cout << "HALT" << std::endl;
                        running = 0;
                        break;
                }


                break;
            }

            case OP_RES:
            case OP_RTI:
            default:
                throw std::runtime_error("Bad Instruction");
                break;
        }
    }

    restore_input_buffering();
}



/*
The LC-3 supports five addressing modes: immediate (or literal), register,
and three memory addressing modes: PC-relative, indirect, and Base+offset.

 Each time a GPR is written by an operate
or a load instruction, the N, Z, and P one-bit registers are individually set to 0 or 1

only modifying GPR

PC is incremented by 1 every time we execute something


ADD AND NOT, LEA, LD, SD, LDI, SDI, LDR

All L's and S's require 
offsets (They are load / store instructions)

Load -> load something in registers
Store -> store something in memory

LEA (requires registry): adds current value at PC with the sign extended value in its instruction into a register it specified


LD (requires registyr): Gets address PC is pointing to and increments it with 8bit value specified in instruction, and loads the value at that address in the registry specified
SD -> Opposite of LD (stores value of register in memory location specified)


LDI (requires registry): same thing as LD, but instead of loading the value from the offset, the value at the address is another address. It gets the value from that address. (should be a meaningful address)
STI -> Get address1 by adding offset to PC. Then treat the value of address1 as address2. Store content of SR (source registry) in address2

LDR (requires 2 registris R1, R2, offset): adds addresses of offset and R2, and stores the value of the added address into R1
STR -> Same thing as LDR but stores the value in SR (R1) into the second registry + offset

Control functions:

BR: 
Have 3 bits for (N, P, Z) somewhere in the middle of the instruction. 

If our N (negative), P (positive) and Z (zero) registries match at least one of the 3 bits we branch to the PC + offset

JMP:

Only has a registry address, and changes JMP directly to the value in the specified registry (should be an address)


TRAP:
request that the operating system performs a task for us (service call)




*/