/* See COPYRIGHT for copyright information. */

#include <inc/x86.h>
#include <inc/mmu.h>
#include <inc/error.h>
#include <inc/string.h>
#include <inc/assert.h>
#include <inc/elf.h>

#include <kern/env.h>
#include <kern/pmap.h>
#include <kern/trap.h>
#include <kern/monitor.h>
#include <kern/sched.h>
#include <kern/kdebug.h>
#include <kern/macro.h>
#include <kern/pmap.h>
#include <kern/traceopt.h>

/* Currently active environment */
struct Env *curenv = NULL;

#ifdef CONFIG_KSPACE
/* All environments */
struct Env env_array[NENV];
struct Env *envs = env_array;
#else
/* All environments */
struct Env *envs = NULL;
#endif

/* Free environment list
 * (linked by Env->env_link) */
static struct Env *env_free_list;

/* NOTE: Should be at least LOGNENV */
#define ENVGENSHIFT 12

/* Converts an envid to an env pointer.
 * If checkperm is set, the specified environment must be either the
 * current environment or an immediate child of the current environment.
 *
 * RETURNS
 *     0 on success, -E_BAD_ENV on error.
 *   On success, sets *env_store to the environment.
 *   On error, sets *env_store to NULL. */
int
envid2env(envid_t envid, struct Env **env_store, bool need_check_perm) {
    struct Env *env;

    /* If envid is zero, return the current environment. */
    if (!envid) {
        *env_store = curenv;
        return 0;
    }

    /* Look up the Env structure via the index part of the envid,
     * then check the env_id field in that struct Env
     * to ensure that the envid is not stale
     * (i.e., does not refer to a _previous_ environment
     * that used the same slot in the envs[] array). */
    env = &envs[ENVX(envid)];
    if (env->env_status == ENV_FREE || env->env_id != envid) {
        *env_store = NULL;
        return -E_BAD_ENV;
    }

    /* Check that the calling environment has legitimate permission
     * to manipulate the specified environment.
     * If checkperm is set, the specified environment
     * must be either the current environment
     * or an immediate child of the current environment. */
    if (need_check_perm && env != curenv && env->env_parent_id != curenv->env_id) {
        *env_store = NULL;
        return -E_BAD_ENV;
    }

    *env_store = env;
    return 0;
}

/* Mark all environments in 'envs' as free, set their env_ids to 0,
 * and insert them into the env_free_list.
 * Make sure the environments are in the free list in the same order
 * they are in the envs array (i.e., so that the first call to
 * env_alloc() returns envs[0]).
 */
void
env_init(void) {
    /* kzalloc_region only works with current_space != NULL */

    /* Allocate envs array with kzalloc_region
     * (don't forget about rounding) */
	envs = (struct Env *)kzalloc_region(sizeof(*envs) * NENV);
    memset(envs, 0, sizeof(*envs) * NENV);

    /* Map envs to UENVS read-only,
     * but user-accessible (with PROT_USER_ set) */
	if (map_region(current_space, UENVS, &kspace, (uintptr_t)envs, UENVS_SIZE, PROT_R | PROT_USER_))
       panic("Cannot map physical region at %p of size %lld", (void *)envs, UENVS_SIZE);
    
    /* Set up envs array */
    memset(envs, 0, sizeof(envs) * sizeof(struct Env));

    for (size_t i = 0; i < NENV; i++) {
        envs[NENV - i - 1].env_link = env_free_list;
        env_free_list = &envs[NENV - i - 1];
    }
}

/* Allocates and initializes a new environment.
 * On success, the new environment is stored in *newenv_store.
 *
 * Returns
 *     0 on success, < 0 on failure.
 * Errors
 *    -E_NO_FREE_ENV if all NENVS environments are allocated
 *    -E_NO_MEM on memory exhaustion
 */
int
env_alloc(struct Env **newenv_store, envid_t parent_id, enum EnvType type) {

    struct Env *env;
    if (!(env = env_free_list))
        return -E_NO_FREE_ENV;

    /* Allocate and set up the page directory for this environment. */
    int res = init_address_space(&env->address_space);
    if (res < 0) return res;

    /* Generate an env_id for this environment */
    int32_t generation = (env->env_id + (1 << ENVGENSHIFT)) & ~(NENV - 1);
    /* Don't create a negative env_id */
    if (generation <= 0) generation = 1 << ENVGENSHIFT;
    env->env_id = generation | (env - envs);

    /* Set the basic status variables */
    env->env_parent_id = parent_id;
#ifdef CONFIG_KSPACE
    env->env_type = ENV_TYPE_KERNEL;
#else
    env->env_type = type;
#endif
    env->env_status = ENV_RUNNABLE;
    env->env_runs = 0;

    /* Clear out all the saved register state,
     * to prevent the register values
     * of a prior environment inhabiting this Env structure
     * from "leaking" into our new environment */
    memset(&env->env_tf, 0, sizeof(env->env_tf));

    /* Set up appropriate initial values for the segment registers.
     * GD_UD is the user data (KD - kernel data) segment selector in the GDT, and
     * GD_UT is the user text (KT - kernel text) segment selector (see inc/memlayout.h).
     * The low 2 bits of each segment register contains the
     * Requestor Privilege Level (RPL); 3 means user mode, 0 - kernel mode.  When
     * we switch privilege levels, the hardware does various
     * checks involving the RPL and the Descriptor Privilege Level
     * (DPL) stored in the descriptors themselves */

#ifdef CONFIG_KSPACE
    env->env_tf.tf_ds = GD_KD;
    env->env_tf.tf_es = GD_KD;
    env->env_tf.tf_ss = GD_KD;
    env->env_tf.tf_cs = GD_KT;

    // LAB 3: Your code here:
    static uintptr_t stack_top = 0x2000000;
    env->env_tf.tf_rsp = stack_top - (env - envs) * 2 * PAGE_SIZE;

#else
    env->env_tf.tf_ds = GD_UD | 3;
    env->env_tf.tf_es = GD_UD | 3;
    env->env_tf.tf_ss = GD_UD | 3;
    env->env_tf.tf_cs = GD_UT | 3;
    env->env_tf.tf_rsp = USER_STACK_TOP;
#endif

    /* For now init trapframe with IF set */
    env->env_tf.tf_rflags = FL_IF;

    /* Clear the page fault handler until user installs one. */
    env->env_pgfault_upcall = 0;

    /* Also clear the IPC receiving flag. */
    env->env_ipc_recving = 0;

    /* Commit the allocation */
    env_free_list = env->env_link;
    *newenv_store = env;

    if (trace_envs) cprintf("[%08x] new env %08x\n", curenv ? curenv->env_id : 0, env->env_id);
    return 0;
}

/* Pass the original ELF image to binary/size and bind all the symbols within
 * its loaded address space specified by image_start/image_end.
 * Make sure you understand why you need to check that each binding
 * must be performed within the image_start/image_end range.
 */
static int
bind_functions(struct Env *env, uint8_t *binary, size_t size, uintptr_t image_start, uintptr_t image_end) {
    /* NOTE: find_function from kdebug.c should be used */

    if (image_start == image_end)
        return 0;

    // uint8_t *const binary_end = binary + size;

    assert(image_start < image_end);

    struct Elf *const header = (struct Elf *)binary;
    struct Secthdr *const sh_begin = (struct Secthdr *)(binary + header->e_shoff);
    struct Secthdr *const sh_end = sh_begin + header->e_shnum;
    const char *shstr  = (char *)binary + (sh_begin + header->e_shstrndx)->sh_offset;

    // assert(sh_end <= binary_end);

    struct Elf64_Sym *sym_table_begin = NULL;
    struct Elf64_Sym *sym_table_end = NULL;
    const char *str_table = NULL;

    for (struct Secthdr *sh = sh_begin; sh < sh_end; ++sh) {
        if (sh->sh_type == ELF_SHT_STRTAB && 
            (const uint8_t *)sh - (const uint8_t *)sh_end != header->e_shstrndx && 
            !strcmp(".strtab", shstr + sh->sh_name)) {
            str_table = (const char *)(binary + sh->sh_offset);
        }

        if (sh->sh_type == ELF_SHT_SYMTAB) {
            sym_table_begin = (struct Elf64_Sym *)(binary + sh->sh_offset);
            sym_table_end = sym_table_begin + sh->sh_size / sizeof(struct Elf64_Sym);
        }

        if (sym_table_begin && sym_table_end && str_table) {
            break;
        }
    }

    assert(sym_table_begin && sym_table_end && str_table);
    // assert(sym_table_end <= binary_end);

    for (struct Elf64_Sym *sym = sym_table_begin; sym < sym_table_end; ++sym) {
        if (ELF64_ST_BIND(sym->st_info) == STB_GLOBAL &&
            ELF64_ST_TYPE(sym->st_info) == STT_OBJECT &&
            image_start <= sym->st_value && sym->st_value < image_end) {

            uintptr_t fn_ptr = 0;
            const char *fn_name = str_table + sym->st_name;
            if ((fn_ptr = find_function(fn_name))) {
                //TODO: possible not-aligned access, fix needed
                *((uintptr_t *)(sym->st_value)) = fn_ptr;
                cprintf("bind_functions: function resolved - %s\n", fn_name);
                continue;
            }

            cprintf("bind_functions: cannot resolve function - %s\n", fn_name);
        }
    }

    return 0;
}

/* Set up the initial program binary, stack, and processor flags
 * for a user process.
 * This function is ONLY called during kernel initialization,
 * before running the first environment.
 *
 * This function loads all loadable segments from the ELF binary image
 * into the environment's user memory, starting at the appropriate
 * virtual addresses indicated in the ELF program header.
 * At the same time it clears to zero any portions of these segments
 * that are marked in the program header as being mapped
 * but not actually present in the ELF file - i.e., the program's bss section.
 *
 * All this is very similar to what our boot loader does, except the boot
 * loader also needs to read the code from disk.  Take a look at
 * LoaderPkg/Loader/Bootloader.c to get ideas.
 *
 * Finally, this function maps one page for the program's initial stack.
 *
 * load_icode returns -E_INVALID_EXE if it encounters problems.
 *  - How might load_icode fail?  What might be wrong with the given input?
 *
 * Hints:
 *   Load each program segment into memory
 *   at the address specified in the ELF section header.
 *   You should only load segments with ph->p_type == ELF_PROG_LOAD.
 *   Each segment's address can be found in ph->p_va
 *   and its size in memory can be found in ph->p_memsz.
 *   The ph->p_filesz bytes from the ELF binary, starting at
 *   'binary + ph->p_offset', should be copied to address
 *   ph->p_va.  Any remaining memory bytes should be cleared to zero.
 *   (The ELF header should have ph->p_filesz <= ph->p_memsz.)
 *   Use functions from the previous labs to allocate and map pages.
 *
 *   All page protection bits should be user read/write for now.
 *   ELF segments are not necessarily page-aligned, but you can
 *   assume for this function that no two segments will touch
 *   the same page.
 *
 *   You may find a function like map_region useful.
 *
 *   Loading the segments is much simpler if you can move data
 *   directly into the virtual addresses stored in the ELF binary.
 *   So which page directory should be in force during
 *   this function?
 *
 *   You must also do something with the program's entry point,
 *   to make sure that the environment starts executing there.
 *   What?  (See env_run() and env_pop_tf() below.) */
static int
load_icode(struct Env *env, uint8_t *binary, size_t size) {
    uint8_t *const binary_end = binary + size;
    struct Elf *header = (struct Elf *)binary;

    assert(header->e_magic == ELF_MAGIC);

    switch_address_space(&env->address_space);

    struct Proghdr *const ph_begin = (struct Proghdr *)(binary + header->e_phoff);
    struct Proghdr *const ph_end = ph_begin + header->e_phnum;

    for (struct Proghdr *ph = ph_begin; ph < ph_end; ++ph) {
        if (ph->p_type == ELF_PROG_LOAD) {
            assert(ph->p_filesz <= ph->p_memsz);
            assert(binary + ph->p_offset <= binary_end);

            map_region(&env->address_space, ROUNDDOWN((uintptr_t)ph->p_va, PAGE_SIZE), NULL, 0, ROUNDUP((uintptr_t)ph->p_va + ph->p_memsz, PAGE_SIZE) - ROUNDDOWN((uintptr_t)ph->p_va, PAGE_SIZE), PROT_RWX | PROT_USER_ | ALLOC_ZERO); 
            memmove((void *)ph->p_va, binary + ph->p_offset, ph->p_filesz);
            memset(((void *)ph->p_va + ph->p_filesz), 0, ph->p_memsz - ph->p_filesz);

            cprintf("Mapped: 0x%lx\n", ph->p_va);
        }
    }

    map_region(&env->address_space, USER_STACK_TOP - USER_STACK_SIZE, NULL, 0, USER_STACK_SIZE, PROT_R | PROT_W | PROT_USER_ | ALLOC_ZERO);
    env->binary = binary;
    env->env_tf.tf_rip = header->e_entry;

    switch_address_space(&kspace);

    return 0;
}

/* Allocates a new env with env_alloc, loads the named elf
 * binary into it with load_icode, and sets its env_type.
 * This function is ONLY called during kernel initialization,
 * before running the first user-mode environment.
 * The new env's parent ID is set to 0.
 */
void
env_create(uint8_t *binary, size_t size, enum EnvType type) {   
    struct Env *env;
    int rc = env_alloc(&env, 0, type);
    if (rc < 0) {
        panic("env_alloc: error, rc = %d", rc);
    }
    
    env->binary = binary;
    env->env_type = type;
    assert(load_icode(env, binary, size) == 0);
}


/* Frees env and all memory it uses */
void
env_free(struct Env *env) {

    /* Note the environment's demise. */
    if (trace_envs) cprintf("[%08x] free env %08x\n", curenv ? curenv->env_id : 0, env->env_id);

#ifndef CONFIG_KSPACE
    /* If freeing the current environment, switch to kern_pgdir
     * before freeing the page directory, just in case the page
     * gets reused. */
    if (&env->address_space == current_space)
        switch_address_space(&kspace);

    static_assert(MAX_USER_ADDRESS % HUGE_PAGE_SIZE == 0, "Misaligned MAX_USER_ADDRESS");
    release_address_space(&env->address_space);
#endif

    /* Return the environment to the free list */
    env->env_status = ENV_FREE;
    env->env_link = env_free_list;
    env_free_list = env;
}

/* Frees environment env
 *
 * If env was the current one, then runs a new environment
 * (and does not return to the caller)
 */
void
env_destroy(struct Env *env) {
    /* If env is currently running on other CPUs, we change its state to
     * ENV_DYING. A zombie environment will be freed the next time
     * it traps to the kernel. */

    if (env->env_status == ENV_RUNNING && env != curenv) {
        env->env_status = ENV_DYING;
        return;
    }

    env_free(env);

    if (env == curenv) {
        sched_yield();
    }

    in_page_fault = 0;
}

#ifdef CONFIG_KSPACE
void
csys_exit(void) {
    if (!curenv) panic("curenv = NULL");
    env_destroy(curenv);
}

void
csys_yield(struct Trapframe *tf) {
    memcpy(&curenv->env_tf, tf, sizeof(struct Trapframe));
    sched_yield();
}
#endif

/* Restores the register values in the Trapframe with the 'ret' instruction.
 * This exits the kernel and starts executing some environment's code.
 *
 * This function does not return.
 */

_Noreturn void
env_pop_tf(struct Trapframe *tf) {
    asm volatile(
            "movq %0, %%rsp\n"
            "movq 0(%%rsp), %%r15\n"
            "movq 8(%%rsp), %%r14\n"
            "movq 16(%%rsp), %%r13\n"
            "movq 24(%%rsp), %%r12\n"
            "movq 32(%%rsp), %%r11\n"
            "movq 40(%%rsp), %%r10\n"
            "movq 48(%%rsp), %%r9\n"
            "movq 56(%%rsp), %%r8\n"
            "movq 64(%%rsp), %%rsi\n"
            "movq 72(%%rsp), %%rdi\n"
            "movq 80(%%rsp), %%rbp\n"
            "movq 88(%%rsp), %%rdx\n"
            "movq 96(%%rsp), %%rcx\n"
            "movq 104(%%rsp), %%rbx\n"
            "movq 112(%%rsp), %%rax\n"
            "movw 120(%%rsp), %%es\n"
            "movw 128(%%rsp), %%ds\n"
            "addq $152,%%rsp\n" /* skip tf_trapno and tf_errcode */
            "iretq" ::"g"(tf)
            : "memory");

    /* Mostly to placate the compiler */
    panic("Reached unrecheble\n");
}

/* Context switch from curenv to env.
 * This function does not return.
 *
 * Step 1: If this is a context switch (a new environment is running):
 *       1. Set the current environment (if any) back to
 *          ENV_RUNNABLE if it is ENV_RUNNING (think about
 *          what other states it can be in),
 *       2. Set 'curenv' to the new environment,
 *       3. Set its status to ENV_RUNNING,
 *       4. Update its 'env_runs' counter,
 *       5. Use switch_address_space() to switch to its address space.
 * Step 2: Use env_pop_tf() to restore the environment's
 *       registers and starting execution of process.

 * Hints:
 *    If this is the first call to env_run, curenv is NULL.
 *
 *    This function loads the new environment's state from
 *    env->env_tf.  Go back through the code you wrote above
 *    and make sure you have set the relevant parts of
 *    env->env_tf to sensible values.
 */
_Noreturn void
env_run(struct Env *env) {
    assert(env);

    if (trace_envs_more) {
        const char *state[] = {"FREE", "DYING", "RUNNABLE", "RUNNING", "NOT_RUNNABLE"};
        if (curenv) cprintf("[%08X] env stopped: %s\n", curenv->env_id, state[curenv->env_status]);
        cprintf("[%08X] env started: %s\n", env->env_id, state[env->env_status]);
    }

    if (curenv && curenv->env_status == ENV_RUNNING) {
        curenv->env_status = ENV_RUNNABLE;
    }

    curenv = env;
    curenv->env_status = ENV_RUNNING;
    ++curenv->env_runs;

    switch_address_space(&curenv->address_space);
    env_pop_tf(&env->env_tf);
    panic("env_run: env_pop_tf returned!");

    while (1) {}
}
