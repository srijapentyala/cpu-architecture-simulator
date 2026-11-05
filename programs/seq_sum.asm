# Fill a[i] = i, then sum the array over four passes. Sequential access, so block size helps.
ADDI r1, r0, 0
ADDI r2, r0, 2048
ADDI r3, r0, 0
init:
SW r1, 0(r3)
ADDI r1, r1, 1
ADDI r3, r3, 4
BNE r1, r2, init
ADDI r9, r0, 4
ADDI r10, r0, 0
pass:
ADDI r1, r0, 0
ADDI r3, r0, 0
sum:
LW r4, 0(r3)
ADD r10, r10, r4
ADDI r1, r1, 1
ADDI r3, r3, 4
BNE r1, r2, sum
ADDI r9, r9, -1
BNE r9, r0, pass
NOP
