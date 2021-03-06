#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <linux/kvm.h>

struct vm_state {
    int kvm;
    int vm;
    int cpu;
    void *mem;
    size_t mem_size;
    struct kvm_run *run;
    size_t run_size;
    void *page_table;
    size_t page_table_size;
};

enum vm_mode {
    VM_MODE_REAL,
    VM_MODE_PROTECTED,
    VM_MODE_LONG
};

struct vm_options {
    enum vm_mode mode;
    size_t mem_size;
    size_t entry_point;
    int page_table_is_set;
    size_t page_table;
};

static void vm_free(struct vm_state *vm) {
    if (!vm)
        return;

    if (vm->run != MAP_FAILED)
        munmap(vm->run, vm->run_size);
    if (vm->cpu >= 0)
        close(vm->cpu);
    if (vm->vm >= 0)
        close(vm->vm);
    if (vm->kvm >= 0)
        close(vm->kvm);
    if (vm->mem != MAP_FAILED)
        munmap(vm->mem, vm->mem_size);
    if (vm->page_table != MAP_FAILED)
        munmap(vm->page_table, vm->page_table_size);

    free(vm);
}

static struct vm_state *vm_create(size_t mem_size) {
    struct vm_state *vm = malloc(sizeof(struct vm_state));
    if (!vm) {
        perror("malloc");
        goto fail;
    }

    vm->kvm = -1;
    vm->vm = -1;
    vm->cpu = -1;
    vm->mem = MAP_FAILED;
    vm->mem_size = 0;
    vm->run = MAP_FAILED;
    vm->run_size = 0;
    vm->page_table = MAP_FAILED;
    vm->page_table_size = 0;

    vm->kvm = open("/dev/kvm", O_RDWR);
    if (vm->kvm < 0) {
        perror("open /dev/kvm");
        goto fail;
    }

    vm->vm = ioctl(vm->kvm, KVM_CREATE_VM, 0);
    if (vm->vm < 0) {
        perror("KVM_CREATE_VM");
        goto fail;
    }

    vm->mem_size = mem_size;
    vm->mem = mmap(NULL, mem_size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (vm->mem == MAP_FAILED) {
        perror("mmap mem");
        goto fail;
    }

    struct kvm_userspace_memory_region region = {
        .slot = 0,
        .guest_phys_addr = 0,
        .memory_size = mem_size,
        .userspace_addr = (uintptr_t)vm->mem
    };

    if (ioctl(vm->vm, KVM_SET_USER_MEMORY_REGION, &region) < 0) {
        perror("KVM_SET_USER_MEMORY_REGION");
        goto fail;
    }

    vm->cpu = ioctl(vm->vm, KVM_CREATE_VCPU, 0);
    if (vm->cpu < 0) {
        perror("KVM_CREATE_VCPU");
        goto fail;
    }

    int run_size = ioctl(vm->kvm, KVM_GET_VCPU_MMAP_SIZE, 0);
    if (run_size < 0) {
        perror("KVM_GET_VCPU_MMAP_SIZE");
        goto fail;
    }

    vm->run_size = run_size;
    vm->run = mmap(NULL, run_size, PROT_READ | PROT_WRITE, MAP_SHARED, vm->cpu, 0);
    if (vm->run == MAP_FAILED) {
        perror("mmap run");
        goto fail;
    }

    return vm;

fail:
    vm_free(vm);
    return NULL;
}

static int vm_load_image(struct vm_state *vm, const char *path) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        perror("open image");
        goto fail;
    }

    ssize_t r = read(fd, vm->mem, vm->mem_size);
    if (r < 0) {
        perror("read image");
        goto fail;
    }

    close(fd);
    return 0;

fail:
    if (fd >= 0)
        close(fd);

    return -1;
}

const size_t PAGE_SIZE = 4096;

static size_t bytes_to_pages(size_t bytes) {
    return (bytes + PAGE_SIZE - 1) / PAGE_SIZE;
}

static int vm_fill_page_table(struct vm_state *vm, uint64_t *cr3) {
    const size_t pt_levels = 4;
    size_t pt_pages = 0;
    for (size_t level = 0, pages = bytes_to_pages(vm->mem_size); level < pt_levels; ++level) {
        pages = bytes_to_pages(pages * sizeof(uint64_t));
        pt_pages += pages;
    }

    size_t guest_pt_base = vm->mem_size;
    vm->page_table_size = pt_pages * PAGE_SIZE;
    vm->page_table = mmap(NULL, vm->page_table_size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (vm->page_table == MAP_FAILED) {
        perror("mmap page table");
        goto fail;
    }

    memset(vm->page_table, 0, vm->page_table_size);

    void *pt_base = vm->page_table;
    for (size_t level = 0, base = 0, pages = bytes_to_pages(vm->mem_size); level < pt_levels; ++level) {
        uint64_t *pt = (uint64_t*)pt_base;
        for (size_t i = 0; i < pages; ++i) {
            pt[i] = base + i * PAGE_SIZE + 0x03; // 3 = present + writable
        }

        base = (uint64_t)(pt_base - vm->page_table + guest_pt_base);
        pages = bytes_to_pages(pages * sizeof(uint64_t));
        pt_base += pages * PAGE_SIZE;
    }

    struct kvm_userspace_memory_region region = {
        .slot = 1,
        .guest_phys_addr = guest_pt_base,
        .memory_size = vm->page_table_size,
        .userspace_addr = (uintptr_t)vm->page_table,
    };

    if (ioctl(vm->vm, KVM_SET_USER_MEMORY_REGION, &region) < 0) {
        perror("KVM_SET_USER_MEMORY_REGION page table");
        goto fail;
    }

    *cr3 = guest_pt_base + (pt_pages - 1) * PAGE_SIZE;
    return 0;

fail:
    return -1;
}

static void vm_setup_segment(struct kvm_segment *seg, enum vm_mode mode, int is_code) {
    seg->base = 0;
    seg->selector = mode == VM_MODE_REAL ? 0 : (is_code ? 8 : 16);
    seg->limit = mode == VM_MODE_REAL ? 0xFFFF : 0xFFFFFFFF;
    seg->type = is_code ? 0x0B : 0x03;
    seg->db = mode == VM_MODE_PROTECTED ? 1 : 0;
    seg->l = mode == VM_MODE_LONG ? 1 : 0;
    seg->g = mode == VM_MODE_REAL ? 0 : 1;
}

static int vm_prepare_to_boot(struct vm_state *vm, const struct vm_options *options) {
    struct kvm_regs regs = {};
    struct kvm_sregs sregs = {};

    if (ioctl(vm->cpu, KVM_GET_REGS, &regs) < 0) {
        perror("KVM_GET_REGS");
        goto fail;
    }

    if (ioctl(vm->cpu, KVM_GET_SREGS, &sregs) < 0) {
        perror("KVM_GET_SREGS");
        goto fail;
    }

    switch (options->mode) {
    case VM_MODE_REAL:
        if (options->entry_point >= 0x10000) {
            fprintf(stderr, "Entry point too far for real mode\n");
            goto fail;
        }
        break;
    case VM_MODE_PROTECTED:
        if (options->entry_point >= 0x100000000ull) {
            fprintf(stderr, "Entry point too far for protected mode\n");
            goto fail;
        }
        sregs.cr0 |= 0x00000001; // PE
        break;
    case VM_MODE_LONG:
        if (options->page_table_is_set) {
            sregs.cr3 = options->page_table;
        } else {
            uint64_t cr3 = 0;
            if (vm_fill_page_table(vm, &cr3) < 0)
                goto fail;
            sregs.cr3 = cr3;
        }
        sregs.cr0 |= 0x80000001; // PG, PE
        sregs.cr4 |= 0x00000020; // PAE
        sregs.efer |= 0x00000500; // LMA, LME
        break;
    }

    regs.rip = options->entry_point;

    vm_setup_segment(&sregs.cs, options->mode, 1);
    vm_setup_segment(&sregs.ds, options->mode, 0);
    vm_setup_segment(&sregs.es, options->mode, 0);
    vm_setup_segment(&sregs.fs, options->mode, 0);
    vm_setup_segment(&sregs.gs, options->mode, 0);
    vm_setup_segment(&sregs.ss, options->mode, 0);

    if (ioctl(vm->cpu, KVM_SET_REGS, &regs) < 0) {
        perror("KVM_SET_REGS");
        goto fail;
    }

    if (ioctl(vm->cpu, KVM_SET_SREGS, &sregs) < 0) {
        perror("KVM_SET_SREGS");
        goto fail;
    }

    return 0;

fail:
    return -1;
}

static void vm_dump_segment(const char* name, struct kvm_segment *seg) {
    fprintf(stderr, "%s BASE=%016llx LIM=%08x SEL=%04x ", name, seg->base, seg->limit, seg->selector);
    fprintf(stderr, "TP=%x P=%x DPL=%x DB=%x S=%x L=%x G=%x A=%x\n",
        seg->type, seg->present, seg->dpl, seg->db, seg->s, seg->l, seg->g, seg->avl);
}

static void vm_dump(const struct vm_state *vm) {
    fprintf(stderr, "===== BEGIN VM STATE =====\n");

    const char *exit_reasons[] = {
        "KVM_EXIT_UNKNOWN", "KVM_EXIT_EXCEPTION", "KVM_EXIT_IO", "KVM_EXIT_HYPERCALL",
        "KVM_EXIT_DEBUG", "KVM_EXIT_HLT", "KVM_EXIT_MMIO", "KVM_EXIT_IRQ_WINDOW_OPEN",
        "KVM_EXIT_SHUTDOWN", "KVM_EXIT_FAIL_ENTRY", "KVM_EXIT_INTR", "KVM_EXIT_SET_TPR",
        "KVM_EXIT_TPR_ACCESS", "KVM_EXIT_S390_SIEIC", "KVM_EXIT_S390_RESET", "KVM_EXIT_DCR",
        "KVM_EXIT_NMI", "KVM_EXIT_INTERNAL_ERROR", "KVM_EXIT_OSI", "KVM_EXIT_PAPR_HCALL",
        "KVM_EXIT_S390_UCONTROL", "KVM_EXIT_WATCHDOG", "KVM_EXIT_S390_TSCH", "KVM_EXIT_EPR",
        "KVM_EXIT_SYSTEM_EVENT", "KVM_EXIT_S390_STSI", "KVM_EXIT_IOAPIC_EOI", "KVM_EXIT_HYPERV"
    };

    const uint32_t exit_reason = vm->run->exit_reason;
    fprintf(stderr, "Exit reason: %u (%s)\n\n", exit_reason,
        vm->run->exit_reason < sizeof(exit_reasons)/sizeof(*exit_reasons) ? exit_reasons[exit_reason] : "UNKNOWN");

    if (exit_reason == KVM_EXIT_IO) {
        if (vm->run->io.direction == KVM_EXIT_IO_OUT) {
            fprintf(stderr, "Write %ux%u bytes at port %04x: ",
                vm->run->io.count, vm->run->io.size, vm->run->io.port);
            for (size_t i = 0; i < vm->run->io.count * vm->run->io.size; ++i)
                fprintf(stderr, "%02x ", ((const uint8_t*)vm->run)[vm->run->io.data_offset + i]);
            fprintf(stderr, "\n\n");
        } else {
            fprintf(stderr, "Read %ux%u bytes at port %04x\n\n",
                vm->run->io.count, vm->run->io.size, vm->run->io.port);
        }
    } else if (exit_reason == KVM_EXIT_MMIO) {
        if (vm->run->mmio.is_write) {
            fprintf(stderr, "Write %u bytes at %016llx: ",
                vm->run->mmio.len, vm->run->mmio.phys_addr);
            for (size_t i = 0; i < vm->run->mmio.len; ++i)
                fprintf(stderr, "%02x ", vm->run->mmio.data[i]);
            fprintf(stderr, "\n\n");
        } else {
            fprintf(stderr, "Read %u bytes at %016llx\n\n",
                vm->run->mmio.len, vm->run->mmio.phys_addr);
        }
    }

    struct kvm_regs regs = {};
    if (ioctl(vm->cpu, KVM_GET_REGS, &regs) < 0) {
        perror("KVM_GET_REGS");
    } else {
        fprintf(stderr, "RAX=%016llx RBX=%016llx RCX=%016llx RDX=%016llx\n", regs.rax, regs.rbx, regs.rcx, regs.rdx);
        fprintf(stderr, "RSI=%016llx RDI=%016llx RSP=%016llx RBP=%016llx\n", regs.rsi, regs.rdi, regs.rsp, regs.rbp);
        fprintf(stderr, "R8 =%016llx R9 =%016llx R10=%016llx R11=%016llx\n", regs.r8,  regs.r9,  regs.r10, regs.r11);
        fprintf(stderr, "R12=%016llx R13=%016llx R14=%016llx R15=%016llx\n", regs.r12, regs.r13, regs.r14, regs.r15);
        fprintf(stderr, "RIP=%016llx RFL=%016llx\n\n", regs.rip, regs.rflags);
    }

    struct kvm_sregs sregs = {};
    if (ioctl(vm->cpu, KVM_GET_SREGS, &sregs) < 0) {
        perror("KVM_GET_SREGS");
    } else {

        vm_dump_segment("CS ", &sregs.cs);
        vm_dump_segment("DS ", &sregs.ds);
        vm_dump_segment("ES ", &sregs.es);
        vm_dump_segment("FS ", &sregs.fs);
        vm_dump_segment("GS ", &sregs.gs);
        vm_dump_segment("SS ", &sregs.ss);
        vm_dump_segment("TR ", &sregs.tr);
        vm_dump_segment("LDT", &sregs.ldt);
        fprintf(stderr, "GDT BASE=%016llx LIM=%04x        ", sregs.gdt.base, sregs.gdt.limit);
        fprintf(stderr, "IDT BASE=%016llx LIM=%04x\n\n", sregs.idt.base, sregs.idt.limit);

        fprintf(stderr, "CR0=%016llx CR2=%016llx CR3=%016llx CR4=%016llx\n", sregs.cr0, sregs.cr2, sregs.cr3, sregs.cr4);
        fprintf(stderr, "CR8=%016llx EFER=%016llx APIC=%016llx\n", sregs.cr8, sregs.efer, sregs.apic_base);
        fprintf(stderr, "INT BITMAP %016llx %016llx %016llx %016llx\n",
            sregs.interrupt_bitmap[0], sregs.interrupt_bitmap[1], sregs.interrupt_bitmap[2], sregs.interrupt_bitmap[3]);
    }

    fprintf(stderr, "===== END VM STATE =====\n\n");
}

static int vm_run(struct vm_state *vm) {
    while (1) {
        if (ioctl(vm->cpu, KVM_RUN, 0) < 0) {
            perror("KVM_RUN");
            goto fail;
        }

        if (vm->run->exit_reason == KVM_EXIT_IO && vm->run->io.port == 0x3F8 &&
                vm->run->io.size == 1 && vm->run->io.count == 1) {
            uint8_t* data = ((uint8_t*)vm->run) + vm->run->io.data_offset;
            if (vm->run->io.direction == KVM_EXIT_IO_OUT) {
                putchar(data[0]);
            } else {
                int c = getchar();
                if (c == EOF)
                    break;
                data[0] = c;
            }
            continue;
        }

        vm_dump(vm);
        goto fail;
    }

    return 0;

fail:
    return -1;
}


static int execute_image(const char *path, const struct vm_options *options) {
    struct vm_state *vm = vm_create(options->mem_size);
    if (!vm)
        goto fail;

    if (vm_load_image(vm, path) < 0)
        goto fail;

    if (vm_prepare_to_boot(vm, options) < 0)
        goto fail;

    if (vm_run(vm) < 0)
        goto fail;

    vm_free(vm);
    return 0;

fail:
    vm_free(vm);
    return -1;
}

static int parse_num(const char *s, size_t *out) {
    if (s == NULL || *s == '\0')
        return -1;

    char *end = 0;
    errno = 0;
    uint64_t num = strtoull(s, &end, 0);
    if (errno != 0)
        return -1;

    if (*end != '\0')
        return -1;

    if (num > SIZE_MAX)
        return -1;

    *out = (size_t)num;
    return 0;
}

int main(int argc, char **argv) {
    int opt = 0;
    struct vm_options options = {
        .mode = VM_MODE_REAL,
        .mem_size = 1024 * 1024,
    };

    while ((opt = getopt(argc, argv, "RPLe:p:m:")) != -1) {
        switch (opt) {
        case 'R':
            options.mode = VM_MODE_REAL;
            break;
        case 'P':
            options.mode = VM_MODE_PROTECTED;
            break;
        case 'L':
            options.mode = VM_MODE_LONG;
            break;
        case 'm':
            if (parse_num(optarg, &options.mem_size) < 0)
                goto bad_args;
            break;
        case 'e':
            if (parse_num(optarg, &options.entry_point) < 0)
                goto bad_args;
            break;
        case 'p':
            if (parse_num(optarg, &options.page_table) < 0)
                goto bad_args;
            options.page_table_is_set = 1;
            break;
        default:
            goto bad_args;
        }
    }

    if (optind >= argc)
        goto bad_args;

    if (execute_image(argv[optind], &options))
        return EXIT_FAILURE;

    return EXIT_SUCCESS;

bad_args:
    fprintf(stderr, "Usage: blankvm [-RPL] [-m mem_size] [-e entry] [-p page_table] image\n\n");
    fprintf(stderr, "  -R    real mode (16-bit)\n");
    fprintf(stderr, "  -P    protected mode (32-bit)\n");
    fprintf(stderr, "  -L    long mode (64-bit)\n");
    fprintf(stderr, "  -m    memory size\n");
    fprintf(stderr, "  -e    entry point address\n");
    fprintf(stderr, "  -p    page table address (only for long mode)\n\n");
    return EXIT_FAILURE;
}
