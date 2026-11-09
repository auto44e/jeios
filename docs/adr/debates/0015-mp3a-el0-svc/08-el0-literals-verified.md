# EL0 leaf descriptor literals — independently verified (lead)

My derivation formula reproduces all three FROZEN kernel literals exactly
(TEXT 0x..0783, RODATA 0x..0783, RW 0x..0703), so the formula is trusted. The EL0
literals below match what Claude proposed in the design review:

USER_CODE   = 0x0020000000000fc3   AP=0b11 (EL1+EL0 RO), UXN=0 (EL0 exec), PXN=1, nG=1, AF=1, SH=11, Attr0
USER_RODATA = 0x0060000000000fc3   AP=0b11 (EL1+EL0 RO), UXN=1, PXN=1, nG=1, AF=1, SH=11, Attr0
USER_DATA   = 0x0060000000000f43   AP=0b01 (EL1+EL0 RW), UXN=1, PXN=1, nG=1, AF=1, SH=11, Attr0

Bit layout: [1:0]=0b11 valid+page; [4:2]=AttrIdx(0); [7:6]=AP[2:1]; [9:8]=SH(0b11);
[10]=AF; [11]=nG; [53]=PXN; [54]=UXN. AP: 00=EL1 RW, 01=EL1+EL0 RW, 10=EL1 RO,
11=EL1+EL0 RO. EL0 access needs AP[1]=bit6 set.

W^X check: USER_CODE is EL0-RO (AP=11) + EL0-exec (UXN=0) -> readable+executable, NOT
writable. USER_DATA/RODATA are UXN=1 (EL0 non-exec). So no EL0 leaf is both writable
and executable. USER_CODE is the ONLY EL0-executable class. All three are PXN=1 (EL1
cannot execute user pages). Check the worker's literals against THESE exact values.
