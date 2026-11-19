.section  .hdr, "a", @progbits
.set      nomips16
.set      nomicromips

/* read by lua/make.lua */
.local    hdr
.type     hdr, @object
hdr:
  .long   __image_offset
  .long   __image_size
  .long   __ldr_size
  .long   __ldr_rom_lui_ori
  /* packed payload (wii vc), all 0 if not used; see ldr.S */
  .long   __unpack_start      /* file offset of the unpacker */
  .long   __unpack_size
  .long   __ldr_size_lui_ori
  .long   __ldr_dest_lui_ori
  .long   __ldr_jump
  .long   __image_start       /* ram address of the image */
  .long   0
  .long   0
.size     hdr, . - hdr
