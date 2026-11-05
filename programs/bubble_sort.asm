# Bubble-sort 256 pseudo-random words (LCG). The compare branch depends on the data, so it
# is hard to predict. r10 = 1 if the result is sorted, r11 = sum of the elements.
ADDI r20, r0, 256
ADDI r1, r0, 0
ADDI r2, r0, 1
ADDI r3, r0, 0
ADDI r7, r0, 75
seed:
MUL r2, r2, r7
ADDI r2, r2, 74
ANDI r2, r2, 65535
SW r2, 0(r3)
ADDI r3, r3, 4
ADDI r1, r1, 1
BNE r1, r20, seed
ADDI r1, r20, -1
outer:
BEQ r1, r0, verify
ADDI r2, r0, 0
ADDI r3, r0, 0
inner:
LW r4, 0(r3)
LW r5, 4(r3)
BGE r5, r4, noswap
SW r5, 0(r3)
SW r4, 4(r3)
noswap:
ADDI r3, r3, 4
ADDI r2, r2, 1
BNE r2, r1, inner
ADDI r1, r1, -1
J outer
verify:
ADDI r10, r0, 1
ADDI r11, r0, 0
ADDI r3, r0, 0
ADDI r2, r0, 0
ADDI r6, r20, -1
chk:
LW r4, 0(r3)
LW r5, 4(r3)
ADD r11, r11, r4
BGE r5, r4, ok
ADDI r10, r0, 0
ok:
ADDI r3, r3, 4
ADDI r2, r2, 1
BNE r2, r6, chk
LW r4, 0(r3)
ADD r11, r11, r4
NOP
