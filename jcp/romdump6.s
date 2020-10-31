;; ROM Dump
;; This code is really, really dumb, since it expects to run
;; from the skunkboard startup. It won't even init hardware
;; or set up the OPL, since that's all technically done.
;; All it's going to do is use the utilities to dump
;; memory from $802000. We just blindly dump 4MB or 6MB,
;; ignoring how much is really used. There's one bit of
;; cleverness here though: Dumping the last 256 bytes with
;; the Jag CD attached will end up reading data from butch
;; (The Jaguar CD controller) instead, since that's where
;; it's memory-mapped. To avoid that, read the first 4MB,
;; bank switch, and read the last 2MB. This also avoids the
;; need to fix up this ancient snapshot of skunk.s to
;; support 6MB mode.

	.include "jaguar.inc"

	.extern skunkCONSOLECLOSE
	.extern skunkFILEWRITE
	.extern skunkFILECLOSE	
	
col	.equ $8FC0

; Note: JCP directly hacks these first values to determine bank to dump
; 0 = bank 0, anything else = bank 1, and the dump size. The bank switch
; is meaningless on SB rev 1
	move.b	#$0,d0				; data is at offset $AB in the COF file
	move.l	#1030, romblocks	; full blocks: 1030 = 4MB, 1546 = 6MB
	move.l	#252, romrem		; remainder: 252 = 4MB, 2444 = 6MB
	tst.b	d0
	beq.s	start				; assume already set to bank 0

	; set bank 1 before the dump
	move.w  #$4BA1, $C00000		; Destined For Bank 1!

start:
; start here - assumes PC file is already open (JCP does this to put a univ. header on it)
	move.w	#col,BG
	move.l	#$01c0,	d6			; color index used during erase

	move.l	#$802000,a1

	; Write 1031 or 1547, 4060 byte blocks for 4MB or 6MB (minus size of header)
	move.l	romblocks,d1

.wrlp:
	; flash 
	move.w	d6, BG
	move.w	VC,d6
	asl.w	#7,d6
	ori.w	#$c0,d6
	
	; copy from ROM to RAM - can't properly access ROM (cart or bios) while HPI is in write mode
	move.l	#1014,d0		; 4060 bytes / 4 (dwords) - 1 (dbra)
	move.l	#rombuf,a0

	; clear back to normal
	move.w	#col,BG
.cplp:
	move.l	(a1)+,(a0)+
	cmpa.l	#$C00000,a1
	blo		.contlp
	; Time to move on to bank 1
	move.l	#$800000,a1
	move.w	#$4BA1,$C00000
.contlp:
	dbra	d0,.cplp
	
	; set again (skunkFILEWRITE sets it back)
	move.w	d6,BG
	
	; dump RAM buffer to PC
	move.l	#rombuf,a0
	move.l	#4060,d0
	bsr	skunkFILEWRITE
	
	; loop till done
	dbra d1,.wrlp
	
	; last block is not an even multiple, write last 252 bytes
	; copy from ROM to RAM - can't properly access ROM (cart or bios) while HPI is in write mode
	move.l	romrem,d0		; 252 or 2444 bytes
	lsr.l	#2,d0			; divide by 4 (dwords)
	sub.l	#1,d0			; subtract 1 (dbra)
	move.l	#rombuf,a0

.cplp2:
	move.l	(a1)+,(a0)+
	dbra	d0,.cplp2
	
	; dump RAM buffer to PC
	move.l	#rombuf,a0
	move.l	romrem,d0
	bsr	skunkFILEWRITE
	
	; copy complete, close the file 
	bsr skunkFILECLOSE
	
	; now close the console, which will exit JCP and make us reset
	bsr skunkCONSOLECLOSE
	
	; flag done onscreen
	move.w	#$0000,BG

	; check how to return (depends on BIOS rev)
	move.l	$800800, d0
	cmp.l	#$00010100,d0
	beq.s	needjmp
	cmp.l	#$00010000,d0
	beq.s	needjmp
	
	; should be safe to rts - quick and clean return to command mode
	rts

needjmp:
	; fake a reset by rebooting the cart. This is not much
	; slower (visibly), but it does cause a USB reset.
	move.l $800404,a0
	jmp (a0)
	
	.bss
	.long
romblocks:	.ds.l	1
romrem:		.ds.l	1
rombuf:		.ds.b	4096
	
	.end
	
	
