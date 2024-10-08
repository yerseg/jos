/* Simple command-line kernel monitor useful for
 * controlling the kernel and exploring the system interactively. */

#include <inc/stdio.h>
#include <inc/string.h>
#include <inc/memlayout.h>
#include <inc/assert.h>
#include <inc/env.h>
#include <inc/x86.h>

#include <kern/console.h>
#include <kern/monitor.h>
#include <kern/kdebug.h>
#include <kern/tsc.h>
#include <kern/timer.h>
#include <kern/env.h>
#include <kern/pmap.h>
#include <kern/trap.h>
#include <kern/kclock.h>

#define WHITESPACE "\t\r\n "
#define MAXARGS    16

/* Functions implementing monitor commands */
int mon_help(int argc, char **argv, struct Trapframe *tf);
int mon_kerninfo(int argc, char **argv, struct Trapframe *tf);
int mon_backtrace(int argc, char **argv, struct Trapframe *tf);
int mon_dumpcmos(int argc, char **argv, struct Trapframe *tf);
int mon_start(int argc, char **argv, struct Trapframe *tf);
int mon_stop(int argc, char **argv, struct Trapframe *tf);
int mon_frequency(int argc, char **argv, struct Trapframe *tf);
int mon_memory(int argc, char **argv, struct Trapframe *tf);
int mon_test_memory_all(int argc, char **argv, struct Trapframe *tf);
int mon_test_memory_one(int argc, char **argv, struct Trapframe *tf);
int mon_test_memory_two(int argc, char **argv, struct Trapframe *tf);
int mon_pagetable(int argc, char **argv, struct Trapframe *tf);
int mon_virt(int argc, char **argv, struct Trapframe *tf);

struct Command {
    const char *name;
    const char *desc;
    /* return -1 to force monitor to exit */
    int (*func)(int argc, char **argv, struct Trapframe *tf);
};

static struct Command commands[] = {
        {"help", "Display this list of commands", mon_help},
        {"kerninfo", "Display information about the kernel", mon_kerninfo},
        {"backtrace", "Print stack backtrace", mon_backtrace},
        {"dumpcmos", "Print CMOS contents", mon_dumpcmos},
        {"timer_start", "Start timer", mon_start},
        {"timer_stop", "Stop timer", mon_stop},
        {"timer_freq", "Timer frequency", mon_frequency},
        {"memory", "Memory lists", mon_memory},
        {"test_memory_all", "", mon_test_memory_all},
        {"test_memory_one", "", mon_test_memory_one},
        {"test_memory_two", "", mon_test_memory_two},
};
#define NCOMMANDS (sizeof(commands) / sizeof(commands[0]))

/* Implementations of basic kernel monitor commands */

int
mon_help(int argc, char **argv, struct Trapframe *tf) {
    for (size_t i = 0; i < NCOMMANDS; i++)
        cprintf("%s - %s\n", commands[i].name, commands[i].desc);
    return 0;
}

int
mon_kerninfo(int argc, char **argv, struct Trapframe *tf) {
    extern char _head64[], entry[], etext[], edata[], end[];

    cprintf("Special kernel symbols:\n");
    cprintf("  _head64 %16lx (virt)  %16lx (phys)\n", (unsigned long)_head64, (unsigned long)_head64);
    cprintf("  entry   %16lx (virt)  %16lx (phys)\n", (unsigned long)entry, (unsigned long)entry - KERN_BASE_ADDR);
    cprintf("  etext   %16lx (virt)  %16lx (phys)\n", (unsigned long)etext, (unsigned long)etext - KERN_BASE_ADDR);
    cprintf("  edata   %16lx (virt)  %16lx (phys)\n", (unsigned long)edata, (unsigned long)edata - KERN_BASE_ADDR);
    cprintf("  end     %16lx (virt)  %16lx (phys)\n", (unsigned long)end, (unsigned long)end - KERN_BASE_ADDR);
    cprintf("Kernel executable memory footprint: %luKB\n", (unsigned long)ROUNDUP(end - entry, 1024) / 1024);
    return 0;
}

int
mon_backtrace(int argc, char **argv, struct Trapframe *tf) {
    struct StackFrame {
        struct StackFrame *rbp;
        uint64_t rip;
    };

    struct StackFrame *stack = (struct StackFrame *)read_rbp();
    struct Ripdebuginfo dbg_info;

    cprintf("Stack backtrace:\n");
    while (stack) {
        debuginfo_rip(stack->rip, &dbg_info);
        cprintf("  rbp %016lx  rip %016lx\n", (uint64_t)(uint64_t *)stack, stack->rip);
        cprintf(
                "    %s:%d: %.*s+%ld\n",
                dbg_info.rip_file,
                dbg_info.rip_line,
                dbg_info.rip_fn_namelen,
                dbg_info.rip_fn_name,
                stack->rip - dbg_info.rip_fn_addr);

        stack = stack->rbp;
    }

    return 0;
}

int
mon_dumpcmos(int argc, char **argv, struct Trapframe *tf) {
    // Dump CMOS memory in the following format:
    // 00: 00 11 22 33 44 55 66 77 88 99 AA BB CC DD EE FF
    // 10: 00 ..
    // Make sure you understand the values read.
    // Hint: Use cmos_read8()/cmos_write8() functions.
    
    for (uint8_t i = 0; i < CMOS_SIZE; ++i) {
        if (i % 16 == 0) {
            cprintf("\n%02X: ", i);
        }

        cprintf("%02X ", cmos_read8(i));
    }

    cprintf("\n");

    return 0;
}

/* Implement timer_start (mon_start), timer_stop (mon_stop), timer_freq (mon_frequency) commands. */

int
mon_start(int argc, char** argv, struct Trapframe* tf) {
    if (argc < 2) {
        cprintf("Not enough arguments!");
        return 1;
    }

    timer_start(argv[1]);
    return 0;
}

int
mon_stop(int argc, char** argv, struct Trapframe* tf) {
    timer_stop();
    return 0;
}

int
mon_frequency(int argc, char** argv, struct Trapframe* tf) {
    if (argc < 2) {
        cprintf("Not enough arguments!");
        return 1;
    }
    
    timer_cpu_frequency(argv[1]);
    return 0;
}

/* Implement memory (mon_memory) command.
 * This command should call dump_memory_lists()
 */
int 
mon_memory(int argc, char **argv, struct Trapframe *tf) {
    dump_memory_lists();
    return 0;
}

int mon_test_memory_all(int argc, char **argv, struct Trapframe *tf) {
    run_mem_test_alloc_all_pages();
    return 0;
}

int mon_test_memory_one(int argc, char **argv, struct Trapframe *tf) {
    run_mem_test_alloc_all_pages_but_one();
    return 0;
}

int mon_test_memory_two(int argc, char **argv, struct Trapframe *tf) {
    run_mem_test_alloc_all_pages_but_two();
    return 0;
}

/* Implement mon_pagetable() and mon_virt()
 * (using dump_virtual_tree(), dump_page_table())*/
int
mon_virt(int argc, char **argv, struct Trapframe *tf) {
    dump_virtual_tree(&root, 0);
    return 0;
}

int
mon_pagetable(int argc, char **argv, struct Trapframe *tf) {
    dump_page_table(kspace.pml4);
    return 0;
}

/* Kernel monitor command interpreter */

static int
runcmd(char *buf, struct Trapframe *tf) {
    int argc = 0;
    char *argv[MAXARGS];

    argv[0] = NULL;

    /* Parse the command buffer into whitespace-separated arguments */
    for (;;) {
        /* gobble whitespace */
        while (*buf && strchr(WHITESPACE, *buf)) *buf++ = 0;
        if (!*buf) break;

        /* save and scan past next arg */
        if (argc == MAXARGS - 1) {
            cprintf("Too many arguments (max %d)\n", MAXARGS);
            return 0;
        }
        argv[argc++] = buf;
        while (*buf && !strchr(WHITESPACE, *buf)) buf++;
    }
    argv[argc] = NULL;

    /* Lookup and invoke the command */
    if (!argc) return 0;
    for (size_t i = 0; i < NCOMMANDS; i++) {
        if (strcmp(argv[0], commands[i].name) == 0)
            return commands[i].func(argc, argv, tf);
    }

    cprintf("Unknown command '%s'\n", argv[0]);
    return 0;
}

void
monitor(struct Trapframe *tf) {

    cprintf("Welcome to the JOS kernel monitor!\n");
    cprintf("Type 'help' for a list of commands.\n");

    if (tf) print_trapframe(tf);

    char *buf;
    do buf = readline("K> ");
    while (!buf || runcmd(buf, tf) >= 0);
}
