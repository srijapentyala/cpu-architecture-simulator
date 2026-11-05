# Eight-iteration loop used to compare branch predictors.
ADDI r1, r0, 0
ADDI r2, r0, 8
ADDI r3, r0, 0
loop:
LW r4, 0(r3)
ADD r5, r4, r1
ADDI r1, r1, 1
ADDI r3, r3, 4
BNE r1, r2, loop
