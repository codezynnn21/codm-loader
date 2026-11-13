// injector — arm64 ptrace injector. runs as root, dlopens a .so into the target.
// usage: injector <package> <absolute path to libcodmhook.so>
// no frida, no external tooling: attach, remote mmap, remote dlopen, detach.

#define _GNU_SOURCE
#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ptrace.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef NT_PRSTATUS
#define NT_PRSTATUS 1
#endif

#define SYS_MMAP_ARM64 222
#define ARM_NR_CACHEFLUSH 0x0f0002
#define RTLD_NOW_ 2

struct arm64_regs {
    uint64_t regs[31];
    uint64_t sp;
    uint64_t pc;
    uint64_t pstate;
};

static int get_regs(pid_t pid, struct arm64_regs* regs) {
    struct iovec io = {regs, sizeof(*regs)};
    return ptrace(PTRACE_GETREGSET, pid, (void*)NT_PRSTATUS, &io) == 0 ? 0 : -1;
}

static int set_regs(pid_t pid, const struct arm64_regs* regs) {
    struct iovec io = {(void*)regs, sizeof(*regs)};
    return ptrace(PTRACE_SETREGSET, pid, (void*)NT_PRSTATUS, &io) == 0 ? 0 : -1;
}

static int write_remote(pid_t pid, uint64_t addr, const void* buf, size_t len) {
    const uint8_t* src = (const uint8_t*)buf;
    size_t off = 0;
    while (off < len) {
        errno = 0;
        long cur = ptrace(PTRACE_PEEKDATA, pid, (void*)(uintptr_t)(addr + off), 0);
        if (cur == -1 && errno != 0) return -1;
        uint64_t word = (uint64_t)cur;
        size_t n = len - off;
        if (n > sizeof(word)) n = sizeof(word);
        memcpy(&word, src + off, n);
        if (ptrace(PTRACE_POKEDATA, pid, (void*)(uintptr_t)(addr + off), (void*)(uintptr_t)word) == -1) {
            return -1;
        }
        off += n;
    }
    return 0;
}

static pid_t find_pid(const char* package) {
    DIR* dir = opendir("/proc");
    if (!dir) return -1;
    pid_t found = -1;
    struct dirent* entry;
    while ((entry = readdir(dir)) != NULL) {
        char* endp = NULL;
        long pid = strtol(entry->d_name, &endp, 10);
        if (!endp || *endp != '\0' || pid <= 0) continue;
        char path[64];
        snprintf(path, sizeof(path), "/proc/%ld/cmdline", pid);
        FILE* f = fopen(path, "r");
        if (!f) continue;
        char cmd[256] = {0};
        size_t got = fread(cmd, 1, sizeof(cmd) - 1, f);
        fclose(f);
        if (got == 0) continue;
        if (strstr(cmd, package) != NULL) {
            found = (pid_t)pid;
            break;
        }
    }
    closedir(dir);
    return found;
}

// first mapping of a module (file offset 0) - gives its load base.
static int module_base(pid_t pid, const char* needle, uint64_t* base) {
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/maps", pid);
    FILE* f = fopen(path, "r");
    if (!f) return -1;
    char line[512];
    int rc = -1;
    while (fgets(line, sizeof(line), f)) {
        unsigned long long start = 0, end = 0, offset = 0;
        char perms[8] = {0}, dev[16] = {0}, name[256] = {0};
        int n = sscanf(line, "%llx-%llx %7s %llx %15s %*s %255[^\n]",
                       &start, &end, perms, &offset, dev, name);
        if (n < 5) continue;
        if (offset != 0) continue;
        if (strstr(name, needle) == NULL) continue;
        *base = start;
        rc = 0;
        break;
    }
    fclose(f);
    return rc;
}

// mapping that contains addr - returns the module path + its base.
static int addr_module(pid_t pid, uint64_t addr, char* path_out, size_t path_len, uint64_t* base) {
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/maps", pid);
    FILE* f = fopen(path, "r");
    if (!f) return -1;
    char line[512];
    char module[256] = {0};
    int found = 0;
    while (fgets(line, sizeof(line), f)) {
        unsigned long long start = 0, end = 0, offset = 0;
        char perms[8] = {0}, dev[16] = {0}, name[256] = {0};
        int n = sscanf(line, "%llx-%llx %7s %llx %15s %*s %255[^\n]",
                       &start, &end, perms, &offset, dev, name);
        if (n < 5) continue;
        if (addr >= start && addr < end) {
            snprintf(module, sizeof(module), "%s", name);
            found = 1;
            break;
        }
    }
    fclose(f);
    if (!found || module[0] != '/') return -1;
    if (module_base(pid, module, base) != 0) return -1;
    snprintf(path_out, path_len, "%s", module);
    return 0;
}

// first executable mapping of a module - used to locate a svc #0 gadget.
static int module_exec(pid_t pid, const char* needle, uint64_t* start, uint64_t* end) {
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/maps", pid);
    FILE* f = fopen(path, "r");
    if (!f) return -1;
    char line[512];
    int rc = -1;
    while (fgets(line, sizeof(line), f)) {
        unsigned long long lo = 0, hi = 0, offset = 0;
        char perms[8] = {0}, dev[16] = {0}, name[256] = {0};
        int n = sscanf(line, "%llx-%llx %7s %llx %15s %*s %255[^\n]",
                       &lo, &hi, perms, &offset, dev, name);
        if (n < 5) continue;
        if (strstr(name, needle) == NULL) continue;
        if (perms[2] != 'x') continue;
        *start = lo;
        *end = hi;
        rc = 0;
        break;
    }
    fclose(f);
    return rc;
}

static int find_svc_local(uint64_t* svc, uint64_t* libc_base) {
    pid_t self = getpid();
    if (module_base(self, "/libc.so", libc_base) != 0) return -1;
    uint64_t start = 0, end = 0;
    if (module_exec(self, "/libc.so", &start, &end) != 0) return -1;
    const uint8_t* p = (const uint8_t*)(uintptr_t)start;
    size_t len = (size_t)(end - start);
    for (size_t i = 0; i + 4 <= len; i++) {
        if (p[i] == 0x01 && p[i + 1] == 0x00 && p[i + 2] == 0x00 && p[i + 3] == 0xD4) {
            *svc = start + i;
            return 0;
        }
    }
    return -1;
}

static int wait_stop(pid_t pid, int* status) {
    if (waitpid(pid, status, 0) < 0) return -1;
    if (!WIFSTOPPED(*status)) return -1;
    return 0;
}

static int remote_syscall(pid_t pid, uint64_t svc_pc, long nr,
                          uint64_t a0, uint64_t a1, uint64_t a2,
                          uint64_t a3, uint64_t a4, uint64_t a5,
                          uint64_t* result) {
    struct arm64_regs regs;
    if (get_regs(pid, &regs) != 0) return -1;
    regs.regs[0] = a0;
    regs.regs[1] = a1;
    regs.regs[2] = a2;
    regs.regs[3] = a3;
    regs.regs[4] = a4;
    regs.regs[5] = a5;
    regs.regs[8] = (uint64_t)nr;
    regs.pc = svc_pc;
    if (set_regs(pid, &regs) != 0) return -1;

    int status = 0;
    if (ptrace(PTRACE_SYSCALL, pid, 0, 0) < 0) return -1;
    if (wait_stop(pid, &status) != 0) return -1;
    if (ptrace(PTRACE_SYSCALL, pid, 0, 0) < 0) return -1;
    if (wait_stop(pid, &status) != 0) return -1;

    if (get_regs(pid, &regs) != 0) return -1;
    *result = regs.regs[0];
    return 0;
}

int main(int argc, char** argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: %s <package> <absolute path to hook .so>\n", argv[0]);
        return 1;
    }
    const char* package = argv[1];
    const char* so_path = argv[2];

    pid_t pid = find_pid(package);
    if (pid <= 0) {
        fprintf(stderr, "injector: package not running: %s\n", package);
        return 2;
    }
    printf("injector: target %s pid %d\n", package, pid);

    uint64_t local_svc = 0, local_libc = 0;
    if (find_svc_local(&local_svc, &local_libc) != 0) {
        fprintf(stderr, "injector: svc gadget not found in local libc\n");
        return 3;
    }
    uint64_t remote_libc = 0;
    if (module_base(pid, "/libc.so", &remote_libc) != 0) {
        fprintf(stderr, "injector: libc not mapped in target\n");
        return 3;
    }
    uint64_t remote_svc = remote_libc + (local_svc - local_libc);

    void* local_dlopen = dlsym(RTLD_DEFAULT, "dlopen");
    if (!local_dlopen) {
        fprintf(stderr, "injector: dlopen unresolved\n");
        return 4;
    }
    char dlopen_module[256] = {0};
    uint64_t local_dlopen_base = 0, remote_dlopen_base = 0;
    if (addr_module(getpid(), (uint64_t)(uintptr_t)local_dlopen, dlopen_module,
                    sizeof(dlopen_module), &local_dlopen_base) != 0) {
        fprintf(stderr, "injector: dlopen module not found locally\n");
        return 4;
    }
    if (module_base(pid, dlopen_module, &remote_dlopen_base) != 0) {
        fprintf(stderr, "injector: dlopen module not found in target: %s\n", dlopen_module);
        return 4;
    }
    uint64_t remote_dlopen = remote_dlopen_base +
                             ((uint64_t)(uintptr_t)local_dlopen - local_dlopen_base);

    if (ptrace(PTRACE_ATTACH, pid, 0, 0) < 0) {
        fprintf(stderr, "injector: attach failed: %s (root?)\n", strerror(errno));
        return 5;
    }
    int status = 0;
    if (wait_stop(pid, &status) != 0) {
        fprintf(stderr, "injector: attach wait failed\n");
        ptrace(PTRACE_DETACH, pid, 0, 0);
        return 5;
    }

    struct arm64_regs saved;
    if (get_regs(pid, &saved) != 0) {
        fprintf(stderr, "injector: regs read failed\n");
        ptrace(PTRACE_DETACH, pid, 0, 0);
        return 5;
    }

    uint64_t remote_mem = 0;
    if (remote_syscall(pid, remote_svc, SYS_MMAP_ARM64, 0, 0x4000, 7, 0x22,
                       (uint64_t)-1, 0, &remote_mem) != 0) {
        fprintf(stderr, "injector: remote mmap failed\n");
        set_regs(pid, &saved);
        ptrace(PTRACE_DETACH, pid, 0, 0);
        return 6;
    }
    if ((int64_t)remote_mem < 0) {
        fprintf(stderr, "injector: remote mmap returned %lld\n", (long long)(int64_t)remote_mem);
        set_regs(pid, &saved);
        ptrace(PTRACE_DETACH, pid, 0, 0);
        return 6;
    }
    printf("injector: remote buffer at 0x%llx\n", (unsigned long long)remote_mem);

    uint64_t path_addr = remote_mem + 0x100;
    uint64_t brk_addr = remote_mem + 0x200;
    uint64_t brk_insn = 0xD4200000ull;

    if (write_remote(pid, path_addr, so_path, strlen(so_path) + 1) != 0 ||
        write_remote(pid, brk_addr, &brk_insn, sizeof(brk_insn)) != 0) {
        fprintf(stderr, "injector: remote write failed\n");
        set_regs(pid, &saved);
        ptrace(PTRACE_DETACH, pid, 0, 0);
        return 7;
    }

    uint64_t ignored = 0;
    remote_syscall(pid, remote_svc, ARM_NR_CACHEFLUSH, brk_addr, brk_addr + 4, 0, 0, 0, 0, &ignored);

    struct arm64_regs call;
    if (get_regs(pid, &call) != 0) {
        set_regs(pid, &saved);
        ptrace(PTRACE_DETACH, pid, 0, 0);
        return 8;
    }
    call.regs[0] = path_addr;
    call.regs[1] = RTLD_NOW_;
    call.regs[30] = brk_addr;
    call.pc = remote_dlopen;
    if (set_regs(pid, &call) != 0) {
        set_regs(pid, &saved);
        ptrace(PTRACE_DETACH, pid, 0, 0);
        return 8;
    }

    if (ptrace(PTRACE_CONT, pid, 0, 0) < 0) {
        fprintf(stderr, "injector: continue failed\n");
        set_regs(pid, &saved);
        ptrace(PTRACE_DETACH, pid, 0, 0);
        return 8;
    }
    if (wait_stop(pid, &status) != 0) {
        fprintf(stderr, "injector: target stopped unexpectedly\n");
        set_regs(pid, &saved);
        ptrace(PTRACE_DETACH, pid, 0, 0);
        return 9;
    }

    int sig = WSTOPSIG(status);
    struct arm64_regs after;
    get_regs(pid, &after);
    printf("injector: dlopen returned 0x%llx (stop sig %d)\n",
           (unsigned long long)after.regs[0], sig);

    set_regs(pid, &saved);
    ptrace(PTRACE_DETACH, pid, 0, 0);

    if (after.regs[0] == 0) {
        fprintf(stderr, "injector: dlopen failed in target\n");
        return 10;
    }
    printf("injector: ok\n");
    return 0;
}
