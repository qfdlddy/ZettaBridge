@ Hand-written companion of guest/stubs/gen/libOpenSLES.S. tools/build_guest.sh links
@ both files into one libOpenSLES.so.
@
@ OpenSL ES exports data, not just functions: the SL_IID_* interface ids and the
@ interface vtables. The generated file holds the function thunks only.
@
@ One vtable serves every interface. Slot N is "method N of whatever this object
@ is"; the host dispatched that call with a handle it created itself, so it knows
@ the object's interface kind and resolves the pair (kind, slot) on its side. Slot
@ 14 is the last one any interface uses (SLEngineItf_ has fifteen methods).
@
@ slCreateEngine hands the host the address of this table in r12 (see
@ GUEST_DATA_ARGUMENTS in tools/gen_stubs.py), because the host has to fill the
@ vtable field of every { vtable, handle } pair it returns.

    .syntax unified
    .arm

@ An SLInterfaceID is a pointer to a struct SLInterfaceID_ . The guest never
@ dereferences it: it dlopens the name, reads the pointer and passes it back to
@ GetInterface. Real ids are addresses inside the system library, so they cannot
@ be reproduced here; these are literal constants and the host compares them
@ numerically. 0x7B0100xx cannot be mistaken for a guest address in a real map.

    .data
    .p2align 2

    .global SL_IID_NULL
    .type SL_IID_NULL, %object
    .size SL_IID_NULL, 4
SL_IID_NULL:
    .word 0x00000000

    .global SL_IID_OBJECT
    .type SL_IID_OBJECT, %object
    .size SL_IID_OBJECT, 4
SL_IID_OBJECT:
    .word 0x7B010000

    .global SL_IID_ENGINE
    .type SL_IID_ENGINE, %object
    .size SL_IID_ENGINE, 4
SL_IID_ENGINE:
    .word 0x7B010001

    .global SL_IID_PLAY
    .type SL_IID_PLAY, %object
    .size SL_IID_PLAY, 4
SL_IID_PLAY:
    .word 0x7B010002

    .global SL_IID_RECORD
    .type SL_IID_RECORD, %object
    .size SL_IID_RECORD, 4
SL_IID_RECORD:
    .word 0x7B010003

    .global SL_IID_OUTPUTMIX
    .type SL_IID_OUTPUTMIX, %object
    .size SL_IID_OUTPUTMIX, 4
SL_IID_OUTPUTMIX:
    .word 0x7B010004

    .global SL_IID_VOLUME
    .type SL_IID_VOLUME, %object
    .size SL_IID_VOLUME, 4
SL_IID_VOLUME:
    .word 0x7B010005

    .global SL_IID_BUFFERQUEUE
    .type SL_IID_BUFFERQUEUE, %object
    .size SL_IID_BUFFERQUEUE, 4
SL_IID_BUFFERQUEUE:
    .word 0x7B010006

    .global SL_IID_ANDROIDSIMPLEBUFFERQUEUE
    .type SL_IID_ANDROIDSIMPLEBUFFERQUEUE, %object
    .size SL_IID_ANDROIDSIMPLEBUFFERQUEUE, 4
SL_IID_ANDROIDSIMPLEBUFFERQUEUE:
    .word 0x7B010007

    .global SL_IID_ANDROIDCONFIGURATION
    .type SL_IID_ANDROIDCONFIGURATION, %object
    .size SL_IID_ANDROIDCONFIGURATION, 4
SL_IID_ANDROIDCONFIGURATION:
    .word 0x7B010008

@ The one interface vtable. The entries are the generated slot thunks, so the
@ linker turns them into relative relocations against this library's own text.
    .p2align 4
    .global zb_sl_vtables
    .hidden zb_sl_vtables
    .type zb_sl_vtables, %object
    .size zb_sl_vtables, 15 * 4
zb_sl_vtables:
    .word zb_sl_slot_0
    .word zb_sl_slot_1
    .word zb_sl_slot_2
    .word zb_sl_slot_3
    .word zb_sl_slot_4
    .word zb_sl_slot_5
    .word zb_sl_slot_6
    .word zb_sl_slot_7
    .word zb_sl_slot_8
    .word zb_sl_slot_9
    .word zb_sl_slot_10
    .word zb_sl_slot_11
    .word zb_sl_slot_12
    .word zb_sl_slot_13
    .word zb_sl_slot_14
