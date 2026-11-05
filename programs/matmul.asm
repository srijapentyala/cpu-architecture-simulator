# C = A * B for 24x24 matrices, ijk order, with A[i][j] = i+j+1 and B[i][j] = i*j.
# B is walked column-wise, which is the classic poor-locality loop. r30 = sum of C.
ADDI r20, r0, 24
MUL r21, r20, r20
SLLI r22, r21, 2
ADD r23, r0, r22        # B base
ADD r24, r22, r22       # C base
ADDI r1, r0, 0
ADDI r5, r0, 0
init_i:
ADDI r2, r0, 0
init_j:
ADD r6, r1, r2
ADDI r6, r6, 1
SW r6, 0(r5)
MUL r7, r1, r2
ADD r8, r5, r23
SW r7, 0(r8)
ADDI r5, r5, 4
ADDI r2, r2, 1
BNE r2, r20, init_j
ADDI r1, r1, 1
BNE r1, r20, init_i
SLLI r13, r20, 2        # bytes per row
ADDI r1, r0, 0
mul_i:
ADDI r2, r0, 0
mul_j:
ADDI r3, r0, 0
ADDI r10, r0, 0
MUL r11, r1, r20
SLLI r11, r11, 2        # &A[i][0]
SLLI r12, r2, 2
ADD r12, r12, r23       # &B[0][j]
mul_k:
LW r14, 0(r11)
LW r15, 0(r12)
MUL r16, r14, r15
ADD r10, r10, r16
ADDI r11, r11, 4
ADD r12, r12, r13
ADDI r3, r3, 1
BNE r3, r20, mul_k
MUL r17, r1, r20
ADD r17, r17, r2
SLLI r17, r17, 2
ADD r17, r17, r24
SW r10, 0(r17)
ADDI r2, r2, 1
BNE r2, r20, mul_j
ADDI r1, r1, 1
BNE r1, r20, mul_i
ADDI r30, r0, 0
ADDI r1, r0, 0
ADD r5, r0, r24
check:
LW r6, 0(r5)
ADD r30, r30, r6
ADDI r5, r5, 4
ADDI r1, r1, 1
BNE r1, r21, check
NOP
