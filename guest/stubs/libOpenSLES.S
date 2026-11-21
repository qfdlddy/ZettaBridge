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

@ The rest of the interface ids Android's libOpenSLES.so exports. Guest libraries import them
@ as data symbols at load time (BIND_NOW), so a missing one stops the whole library: Punch
@ Quest's libgame.so needs SL_IID_EFFECTSEND before it will even map. None of these interfaces is
@ part of the playback path this bridge serves, so the host answers GetInterface with
@ SL_RESULT_FEATURE_UNSUPPORTED (see HostOpensl's iid space check) instead of lying. Values are
@ in the same reserved 0x7B0100xx space, one per interface, and kSlIidUnsupportedFirst/Last in
@ core/src/android/host_opensl.cpp must match them.
.macro ZB_SL_IID name, value
    .global \name
    .type \name, %object
    .size \name, 4
\name:
    .word \value
.endm

    ZB_SL_IID SL_IID_3DCOMMIT, 0x7B010009
    ZB_SL_IID SL_IID_3DDOPPLER, 0x7B01000A
    ZB_SL_IID SL_IID_3DGROUPING, 0x7B01000B
    ZB_SL_IID SL_IID_3DLOCATION, 0x7B01000C
    ZB_SL_IID SL_IID_3DMACROSCOPIC, 0x7B01000D
    ZB_SL_IID SL_IID_3DSOURCE, 0x7B01000E
    ZB_SL_IID SL_IID_ANDROIDACOUSTICECHOCANCELLATION, 0x7B01000F
    ZB_SL_IID SL_IID_ANDROIDAUTOMATICGAINCONTROL, 0x7B010010
    ZB_SL_IID SL_IID_ANDROIDBUFFERQUEUESOURCE, 0x7B010011
    ZB_SL_IID SL_IID_ANDROIDEFFECT, 0x7B010012
    ZB_SL_IID SL_IID_ANDROIDEFFECTCAPABILITIES, 0x7B010013
    ZB_SL_IID SL_IID_ANDROIDEFFECTSEND, 0x7B010014
    ZB_SL_IID SL_IID_ANDROIDNOISESUPPRESSION, 0x7B010015
    ZB_SL_IID SL_IID_AUDIODECODERCAPABILITIES, 0x7B010016
    ZB_SL_IID SL_IID_AUDIOENCODER, 0x7B010017
    ZB_SL_IID SL_IID_AUDIOENCODERCAPABILITIES, 0x7B010018
    ZB_SL_IID SL_IID_AUDIOIODEVICECAPABILITIES, 0x7B010019
    ZB_SL_IID SL_IID_BASSBOOST, 0x7B01001A
    ZB_SL_IID SL_IID_DEVICEVOLUME, 0x7B01001B
    ZB_SL_IID SL_IID_DYNAMICINTERFACEMANAGEMENT, 0x7B01001C
    ZB_SL_IID SL_IID_DYNAMICSOURCE, 0x7B01001D
    ZB_SL_IID SL_IID_EFFECTSEND, 0x7B01001E
    ZB_SL_IID SL_IID_ENGINECAPABILITIES, 0x7B01001F
    ZB_SL_IID SL_IID_ENVIRONMENTALREVERB, 0x7B010020
    ZB_SL_IID SL_IID_EQUALIZER, 0x7B010021
    ZB_SL_IID SL_IID_LED, 0x7B010022
    ZB_SL_IID SL_IID_METADATAEXTRACTION, 0x7B010023
    ZB_SL_IID SL_IID_METADATATRAVERSAL, 0x7B010024
    ZB_SL_IID SL_IID_MIDIMESSAGE, 0x7B010025
    ZB_SL_IID SL_IID_MIDIMUTESOLO, 0x7B010026
    ZB_SL_IID SL_IID_MIDITEMPO, 0x7B010027
    ZB_SL_IID SL_IID_MIDITIME, 0x7B010028
    ZB_SL_IID SL_IID_MUTESOLO, 0x7B010029
    ZB_SL_IID SL_IID_PITCH, 0x7B01002A
    ZB_SL_IID SL_IID_PLAYBACKRATE, 0x7B01002B
    ZB_SL_IID SL_IID_PREFETCHSTATUS, 0x7B01002C
    ZB_SL_IID SL_IID_PRESETREVERB, 0x7B01002D
    ZB_SL_IID SL_IID_RATEPITCH, 0x7B01002E
    ZB_SL_IID SL_IID_SEEK, 0x7B01002F
    ZB_SL_IID SL_IID_THREADSYNC, 0x7B010030
    ZB_SL_IID SL_IID_VIBRA, 0x7B010031
    ZB_SL_IID SL_IID_VIRTUALIZER, 0x7B010032
    ZB_SL_IID SL_IID_VISUALIZATION, 0x7B010033

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
