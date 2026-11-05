# Same array, but read every 16th word over sixteen passes. Each read lands in a different
# 16-word block, so the read passes get no spatial benefit from larger blocks. With small
# blocks the stride also maps to only a few sets, which makes it a conflict-miss test.
ADDI r1, r0, 0
ADDI r2, r0, 2048
ADDI r3, r0, 0
init:
SW r1, 0(r3)
ADDI r1, r1, 1
ADDI r3, r3, 4
BNE r1, r2, init
ADDI r9, r0, 16
ADDI r10, r0, 0
pass:
ADDI r1, r0, 0
ADDI r3, r0, 0
sum:
LW r4, 0(r3)
ADD r10, r10, r4
ADDI r1, r1, 16
ADDI r3, r3, 64
BNE r1, r2, sum
ADDI r9, r9, -1
BNE r9, r0, pass
NOP
