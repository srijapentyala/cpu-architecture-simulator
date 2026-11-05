# Two periodic branches: one taken every 4th iteration, one strictly alternating. A
# per-branch counter cannot learn the alternating one; global history can.
ADDI r1, r0, 0
ADDI r2, r0, 20000
ADDI r10, r0, 0
ADDI r11, r0, 0
loop:
ANDI r3, r1, 3
BEQ r3, r0, skip
ADDI r10, r10, 1
skip:
ANDI r4, r1, 1
BNE r4, r0, odd
ADDI r11, r11, 1
odd:
ADDI r1, r1, 1
BNE r1, r2, loop
NOP
