blankvm
=======

Very simple KVM based hypervisor. Runs your code and nothing more.

System requirements:

- x86_64 CPU with virtualization support
- GNU/Linux with recent enough kernel
- gcc, nasm and cmake for building

```
Usage: blankvm [-RPL] [-m mem_size] [-e entry] [-p page_table] image

  -R    real mode (16-bit)
  -P    protected mode (32-bit)
  -L    long mode (64-bit)
  -m    memory size
  -e    entry point address
  -p    page table address (only for long mode)

Examples:

  blankvm -R test16.bin
  blankvm -P test32.bin
  blankvm -L test64.bin
```

Image is always loaded at physical address 0.

For real mode segment registers are set to 0. For protected mode segments are tuned to base=0, limit=0xFFFFFFFF. Values of other registers are unspecified.

Long mode requires a page table. Page table with 1:1 mapping is generated automatically.
If you need different mapping, you can add table to the image and specify its address with `-p` option.

Virtual serial port is mapped to `0x3F8` and linked to the stdin and stdout.
