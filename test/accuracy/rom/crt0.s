@ Minimal GBA startup for the poryaaaa accuracy test ROM.
@ The header is finalised by gbafix (logo, checksum). The parameter block
@ directly after the header (ROM offset 0xC0) is patched by the harness.

	.arm
	.align 2, 0

	.global Start
Start:
	b Init

RomHeaderNintendoLogo:
	.space 156
RomHeaderGameTitle:
	.ascii "ACCURACYTEST"
RomHeaderGameCode:
	.ascii "ACCE"
RomHeaderMakerCode:
	.ascii "01"
	.byte 0x96      @ fixed value
	.byte 0         @ main unit code
	.byte 0         @ device type
	.space 7
	.byte 0         @ software version
	.byte 0         @ complement check (gbafix)
	.space 2

@ ---- Parameter block (0x080000C0, 28 bytes: struct AccParams in main.c) ----
	.global gAccParams
	.align 2, 0
gAccParams:
	.ascii "PORYACC1"   @ magic
	.byte 0xFF          @ type: 0 song/SE (gSongTable), 1 cry (gCryTable), 2 micro, 0xFF none
	.byte 120           @ cry volume
	.2byte 0            @ id
	.2byte 15360        @ cry pitch
	.byte 0             @ cry pan
	.byte 1             @ start frame (VBlank count at which the test is started)
	.2byte 140          @ cry length (SetPokemonCryLength; PlayCryInternal's default)
	.byte 0             @ cry release (SetPokemonCryRelease; PlayCryInternal's default)
	.byte 0             @ cry chorus (SetPokemonCryChorus, s8; PlayCryInternal's default: one track)
	.2byte 0            @ second cry id (gCryTable index, CRY_MODE_NORMAL's defaults)
	.byte 0             @ second cry start frame (0 = no second cry)
	.space 5            @ reserved

	.align 2, 0
Init:
	mov r0, #0x12           @ IRQ mode
	msr cpsr_cf, r0
	ldr sp, sp_irq
	mov r0, #0x1f           @ System mode
	msr cpsr_cf, r0
	ldr sp, sp_sys
	ldr r1, =0x3007FFC      @ INTR_VECTOR
	adr r0, IntrMain
	str r0, [r1]
	ldr r1, =AgbMain + 1
	mov lr, pc
	bx r1
	b Init

	.align 2, 0
sp_sys: .word 0x3008000 - 0x1c0
sp_irq: .word 0x3008000 - 0x60

	.pool

@ Interrupt dispatcher. Mirrors the shape of pokeemerald's IntrMain for the
@ two interrupts this ROM uses: VCOUNT (line 150 -> m4aSoundVSync) and
@ VBLANK (-> m4aSoundMain), checked in that priority order.
	.arm
	.align 2, 0
IntrMain:
	mov r3, #0x4000000
	add r3, r3, #0x200          @ REG_IE
	ldr r2, [r3]                @ IE | (IF << 16)
	and r1, r2, r2, lsr #16     @ pending & enabled
	ldr r12, =VCountIntr
	ands r0, r1, #0x0004        @ INTR_FLAG_VCOUNT
	bne IntrMain_Found
	ldr r12, =VBlankIntr
	ands r0, r1, #0x0001        @ INTR_FLAG_VBLANK
	bne IntrMain_Found
	@ Unexpected interrupt: acknowledge everything pending and return.
	mov r0, r1
	strh r0, [r3, #2]
	bx lr
IntrMain_Found:
	strh r0, [r3, #2]       @ acknowledge in IF
	ldr r2, =0x3007FF8          @ BIOS INTR_CHECK
	ldrh r1, [r2]
	orr r1, r1, r0
	strh r1, [r2]
	mrs r0, spsr
	stmfd sp!, {r0, lr}
	mrs r3, cpsr
	bic r3, r3, #0x1f
	orr r3, r3, #0x9f           @ System mode, IRQs still masked
	msr cpsr_cf, r3
	stmfd sp!, {lr}
	mov lr, pc
	bx r12
	ldmfd sp!, {lr}
	mrs r3, cpsr
	bic r3, r3, #0x1f
	orr r3, r3, #0x92           @ IRQ mode, IRQs masked
	msr cpsr_cf, r3
	ldmfd sp!, {r0, lr}
	msr spsr_cf, r0
	bx lr

	.pool
