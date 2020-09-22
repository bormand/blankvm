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

        fprintf(stderr, "Exit reason: %u\n", vm->run->exit_reason);
        break;
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
