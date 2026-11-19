#include "z64_version.h"

.section  .ldr, "ax", @progbits
.set      nomips16
.set      nomicromips

#if Z64_VERSION == Z64_OOT10 || \
    Z64_VERSION == Z64_OOT11 || \
    Z64_VERSION == Z64_OOT12 || \
    Z64_VERSION == Z64_OOTMQJ || \
    Z64_VERSION == Z64_OOTMQU || \
    Z64_VERSION == Z64_OOTGCJ || \
    Z64_VERSION == Z64_OOTGCU || \
    Z64_VERSION == Z64_OOTCEJ
.equ      entry_size, 0x60
#elif Z64_VERSION == Z64_OOTIQC
.equ      entry_size, 0x50
#endif

#ifndef GZ_PACKED

/*
 * The loader replaces the game's entry code. It copies the gz image from rom
 * to ram, then jumps to the game's entry code, which the patcher moved to the
 * start of the image.
 */

.local    ldr
.ent      ldr
.type     ldr, @function
ldr:
  lui     $v0, 0xA460
#if Z64_VERSION != Z64_OOTIQC
  /* check for homeboy reset */
  lui     $v1, 0xA805
  lw      $v1, 0x0020($v1)
  beq     $v1, 0x6864, no_dma
#endif
  /* begin dma transfer */
  lui     $v1, %hi(__image_ram)
  addiu   $v1, $v1, %lo(__image_ram)
  sw      $v1, 0x0000($v0)
  lui_ori:
  lui     $v1, 0x0000
  ori     $v1, $v1, 0x0000
  sw      $v1, 0x0004($v0)
  lui     $v1, %hi(__image_size - 1)
  addiu   $v1, $v1, %lo(__image_size - 1)
  sw      $v1, 0x000C($v0)
  /* wait for completion */
  dma_busy_loop:
    lw    $v1, 0x0010($v0)
    andi  $v1, $v1, 1
    bnez  $v1, dma_busy_loop
  li      $v1, 2
  sw      $v1, 0x0010($v0)
  no_dma:
  /* jump to preamble, moved here by patcher */
  j       ldr
  .fill   entry_size - (. - ldr)
.end      ldr
.size     ldr, . - ldr

.global   __ldr_rom_lui_ori
.set      __ldr_rom_lui_ori, lui_ori - ldr

/* no packed payload */
.global   __ldr_size_lui_ori
.set      __ldr_size_lui_ori, 0
.global   __ldr_dest_lui_ori
.set      __ldr_dest_lui_ori, 0
.global   __ldr_jump
.set      __ldr_jump, 0
.global   __unpack_start
.set      __unpack_start, 0
.global   __unpack_size
.set      __unpack_size, 0

#else /* GZ_PACKED */

/*
 * Packed payload, for Wii VC where the rom can't grow past 32 MiB.
 * The patcher stores [yaz0 compressed gz image][unpacker] in rom. The loader
 * copies that to the end of gz's own memory area and runs the unpacker, which
 * decompresses the image in place to its normal address (writing behind what
 * it still has to read; the patcher picks the address so that this is safe)
 * and jumps to the game's entry code at the start of the image.
 * Only memory that gz occupies anyway is used, and no code is run from the
 * part of it the image is decompressed into.
 * The patcher fills in the dma address, rom address, payload size and the
 * jump to the unpacker.
 */

.local    ldr
.ent      ldr
.type     ldr, @function
ldr:
.set      noreorder
.set      noat
  lui     $v0, 0xA460
  /* check for homeboy reset: gz is still in ram, skip loading */
  lui     $v1, 0xA805
  lw      $v1, 0x0020($v1)
  ori     $at, $zero, 0x6864
  beq     $v1, $at, no_dma
  /* begin dma transfer */
  dest_lui_ori:
  lui     $v1, 0x0000
  ori     $v1, $v1, 0x0000
  sw      $v1, 0x0000($v0)
  lui_ori:
  lui     $v1, 0x0000
  ori     $v1, $v1, 0x0000
  sw      $v1, 0x0004($v0)
  size_lui_ori:
  lui     $v1, 0x0000
  ori     $v1, $v1, 0x0000
  sw      $v1, 0x000C($v0)
  /* wait for completion */
  dma_busy_loop:
    lw    $v1, 0x0010($v0)
    andi  $v1, $v1, 1
    bnez  $v1, dma_busy_loop
    nop
  ori     $v1, $zero, 2
  sw      $v1, 0x0010($v0)
  /* run the unpacker */
  jump:
  j       ldr
  nop
  no_dma:
  /* jump to preamble, moved here by patcher */
  j       ldr
  nop
.set      at
.set      reorder
  .fill   entry_size - (. - ldr)
.end      ldr
.size     ldr, . - ldr

.global   __ldr_rom_lui_ori
.set      __ldr_rom_lui_ori, lui_ori - ldr
.global   __ldr_size_lui_ori
.set      __ldr_size_lui_ori, size_lui_ori - ldr
.global   __ldr_dest_lui_ori
.set      __ldr_dest_lui_ori, dest_lui_ori - ldr
.global   __ldr_jump
.set      __ldr_jump, jump - ldr

/*
 * Unpacker. Placed right after the yaz0 data. It is not part of the image:
 * the patcher takes it from the header area of gz.bin (__unpack_start,
 * __unpack_size) and fills in the address of the yaz0 data (first lui/ori).
 * Only plain instructions are used, and the output is written through kseg0
 * followed by the same cache maintenance libultra does (osWritebackDCacheAll,
 * osInvalICache), to stay on paths every game uses.
 */

.equ      UNPACK_SIZE, 0x100

.section  .hdr.unpack, "a", @progbits
.balign   16
.global   __unpack_start
.type     __unpack_start, @function
__unpack_start:
.set      noreorder
.set      noat
  /* a0 = yaz0 data (patched), t0 = decompressed size */
  lui     $a0, 0x0000
  ori     $a0, $a0, 0x0000
  lw      $t0, 0x0004($a0)
  addiu   $a0, $a0, 0x0010
  /* a1 = output, t0 = output end */
  lui     $a1, %hi(__image_start)
  addiu   $a1, $a1, %lo(__image_start)
  addu    $t0, $a1, $t0
group:
  beq     $a1, $t0, done
  nop
  lbu     $t1, 0x0000($a0)          /* code byte */
  addiu   $a0, $a0, 1
  ori     $t2, $zero, 8             /* chunks left in group */
chunk:
  beq     $a1, $t0, done
  andi    $t3, $t1, 0x80
  beqz    $t3, backref
  sll     $t1, $t1, 1
  /* literal byte */
  lbu     $t4, 0x0000($a0)
  addiu   $a0, $a0, 1
  sb      $t4, 0x0000($a1)
  b       next
  addiu   $a1, $a1, 1
backref:
  lbu     $t4, 0x0000($a0)
  lbu     $t5, 0x0001($a0)
  addiu   $a0, $a0, 2
  srl     $t6, $t4, 4               /* t6 = N */
  andi    $t4, $t4, 0x0F
  sll     $t4, $t4, 8
  or      $t4, $t4, $t5
  addiu   $t4, $t4, 1               /* t4 = distance */
  subu    $t5, $a1, $t4             /* t5 = copy source */
  bnez    $t6, copy
  addiu   $t6, $t6, 2               /* length = N + 2 */
  lbu     $t6, 0x0000($a0)          /* N = 0: length = next byte + 0x12 */
  addiu   $a0, $a0, 1
  addiu   $t6, $t6, 0x12
copy:
  lbu     $t7, 0x0000($t5)
  addiu   $t5, $t5, 1
  sb      $t7, 0x0000($a1)
  addiu   $t6, $t6, -1
  bnez    $t6, copy
  addiu   $a1, $a1, 1
next:
  addiu   $t2, $t2, -1
  bnez    $t2, chunk
  nop
  b       group
  nop
done:
  /* write back and invalidate the data cache (8 KiB, 16 byte lines) */
  lui     $t0, 0x8000
  ori     $t1, $t0, 0x2000
dcache:
  cache   0x01, 0x0000($t0)         /* index writeback invalidate (D) */
  addiu   $t0, $t0, 0x10
  bne     $t0, $t1, dcache
  nop
  /* invalidate the instruction cache (16 KiB, 32 byte lines) */
  lui     $t0, 0x8000
  ori     $t1, $t0, 0x4000
icache:
  cache   0x00, 0x0000($t0)         /* index invalidate (I) */
  addiu   $t0, $t0, 0x20
  bne     $t0, $t1, icache
  nop
  /* jump to preamble, moved to the start of the image by the patcher */
  lui     $t0, %hi(__image_start)
  addiu   $t0, $t0, %lo(__image_start)
  jr      $t0
  nop
.set      at
.set      reorder
  .fill   UNPACK_SIZE - (. - __unpack_start)
.size     __unpack_start, . - __unpack_start

.global   __unpack_size
.set      __unpack_size, UNPACK_SIZE

#endif /* GZ_PACKED */

.global   __ldr_size
.set      __ldr_size, entry_size
