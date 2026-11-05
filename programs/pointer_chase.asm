# Follow a linked list through 4096 words, 1597 words per hop: almost no spatial locality,
# and every hop is a load-use hazard. r5 = final pointer, r9 = running sum of pointers.
ADDI r20, r0, 4096
ADDI r21, r0, 1597
ADDI r1, r0, 0
build:
ADD r2, r1, r21
ANDI r2, r2, 4095
SLLI r3, r1, 2
SLLI r4, r2, 2
SW r4, 0(r3)
ADDI r1, r1, 1
BNE r1, r20, build
ADDI r5, r0, 0
ADDI r6, r0, 0
ADDI r8, r0, 50000
ADDI r9, r0, 0
chase:
LW r5, 0(r5)
ADD r9, r9, r5
ADDI r6, r6, 1
BNE r6, r8, chase
NOP
