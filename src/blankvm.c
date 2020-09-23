#include <fcntl.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
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

static int vm_prepare_to_boot(struct vm_state *vm) {
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

    regs.rip = 0;
    sregs.cs.base = 0;
    sregs.cs.selector = 0;

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

static int execute_image(const char *path, size_t mem_size) {
    struct vm_state *vm = vm_create(mem_size);
    if (!vm)
        goto fail;

    if (vm_load_image(vm, path) < 0)
        goto fail;

    if (vm_prepare_to_boot(vm) < 0)
        goto fail;

    if (vm_run(vm) < 0)
        goto fail;

    vm_free(vm);
    return 0;

fail:
    vm_free(vm);
    return -1;
}

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "blankvm <image>\n");
        return 1;
    }

    if (execute_image(argv[1], 1024*1024))
        return 1;

    return 0;
}
