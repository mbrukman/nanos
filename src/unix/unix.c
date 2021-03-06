#include <unix_internal.h>
#include <ftrace.h>
#include <gdb.h>
#include <page.h>

#define PF_DEBUG
#ifdef PF_DEBUG
#define pf_debug(x, ...) thread_log(current, x, ##__VA_ARGS__);
#else
#define pf_debug(x, ...)
#endif

u64 allocate_fd(process p, void *f)
{
    u64 fd = allocate_u64((heap)p->fdallocator, 1);
    if (fd == INVALID_PHYSICAL) {
	msg_err("fail; maxed out\n");
	return fd;
    }
    vector_set(p->files, fd, f);
    return fd;
}

u64 allocate_fd_gte(process p, u64 min, void *f)
{
    u64 fd = id_heap_alloc_gte(p->fdallocator, 1, min);
    if (fd == INVALID_PHYSICAL) {
        msg_err("failed\n");
    }
    else {
        vector_set(p->files, fd, f);
    }
    return fd;
}

void deallocate_fd(process p, int fd)
{
    vector_set(p->files, fd, 0);
    deallocate_u64((heap)p->fdallocator, fd, 1);
}

static void
deliver_segv(u64 vaddr, s32 si_code)
{
    struct siginfo s = {
        .si_signo = SIGSEGV,
         /* man sigaction: "si_errno is generally unused on Linux" */
        .si_errno = 0,
        .si_code = si_code,
        .sifields.sigfault = {
            .addr = vaddr,
        }
    };

    pf_debug("delivering SIGSEGV; vaddr 0x%lx si_code %s",
        vaddr, (si_code == SEGV_MAPERR) ? "SEGV_MAPPER" : "SEGV_ACCERR"
    );

    deliver_signal_to_thread(current, &s);
}

static boolean handle_protection_fault(context frame, u64 vaddr, vmap vm)
{
    /* vmap found, with protection violation set --> send prot violation */
    if (is_protection_fault(frame)) {
        pf_debug("page protection violation\naddr 0x%lx, rip 0x%lx, "
                 "error %s%s%s vm->flags (%s%s%s%s)",
                 vaddr, frame_return_address(frame),
                 is_write_fault(frame) ? "W" : "R",
                 is_usermode_fault(frame) ? "U" : "S",
                 is_instruction_fault(frame) ? "I" : "D",
                 (vm->flags & VMAP_FLAG_MMAP) ? "mmap " : "",
                 (vm->flags & VMAP_FLAG_ANONYMOUS) ? "anonymous " : "",
                 (vm->flags & VMAP_FLAG_WRITABLE) ? "writable " : "",
                 (vm->flags & VMAP_FLAG_EXEC) ? "executable " : "");

        deliver_segv(vaddr, SEGV_ACCERR);
        return true;
    }
    return false;
}

// it so happens that f and frame should be the same number?
define_closure_function(1, 1, void, default_fault_handler,
                        thread, t,
                        context, frame)
{
    boolean user = is_usermode_fault(frame);

    /* Really this should be the enclosed thread, but that won't fly
       for kernel page faults on user pages. If we were ever to
       support multiple processes, we may need to install current when
       resuming deferred processing. */
    process p = current->p;

    if (frame[FRAME_VECTOR] == 14) {
        u64 vaddr = fault_address(frame);
        vmap vm = vmap_from_vaddr(p, vaddr);
        if (vm == INVALID_ADDRESS) {
            if (user) {
                pf_debug("no vmap found for addr 0x%lx, rip 0x%lx", vaddr, frame[FRAME_RIP]);
                deliver_segv(vaddr, SEGV_MAPERR);

                /* schedule this thread to either run signal handler or terminate */
                schedule_frame(frame);
                runloop();
            } else {
                rprintf("\nUnhandled page fault in kernel mode: ");
                goto bug;
            }
        }

        if (is_pte_error(frame)) {
            /* no SEGV on reserved PTEs */
            msg_err("bug: pte entries reserved or corrupt\n");
#ifndef BOOT
            dump_ptes(pointer_from_u64(vaddr));
#endif
            goto bug;
        }

        if (handle_protection_fault(frame, vaddr, vm))
            return;

        if (do_demand_page(fault_address(frame), vm)) {
            /* Dirty hack until we get page faults out of the kernel:
               If we're in the kernel context, return to the frame directly. */
            if (frame == current_cpu()->kernel_frame) {
                current_cpu()->state = cpu_kernel;
                frame_return(frame);
            }
            schedule_frame(frame);
            return;
        }
    }

  bug:
    // panic handling in a more central location?
    apic_ipi(TARGET_EXCLUSIVE_BROADCAST, 0, shutdown_vector);
    rprintf("cpu: %d\n", current_cpu()->id);
    print_frame(frame);
    print_stack(frame);

    if (table_find(p->process_root, sym(fault))) {
        console("starting gdb\n");
        init_tcp_gdb(heap_general(get_kernel_heaps()), p, 9090);
        thread_sleep_uninterruptible();
    } else {
        halt("halt\n");
    }
}

void init_thread_fault_handler(thread t)
{
    init_closure(&t->fault_handler, default_fault_handler, t);
}

closure_function(0, 6, sysreturn, dummy_read,
                 void *, dest, u64, length, u64, offset_arg, thread, t, boolean, bh, io_completion, completion)
{
    thread_log(t, "%s: dest %p, length %ld, offset_arg %ld",
	       __func__, dest, length, offset_arg);
    if (completion)
        apply(completion, t, 0);
    return 0;
}

closure_function(1, 0, sysreturn, std_close,
                 file, f)
{
    unix_cache_free(get_unix_heaps(), file, bound(f));
    return 0;
}

closure_function(0, 6, sysreturn, stdout,
                 void*, d, u64, length, u64, offset, thread, t, boolean, bh, io_completion, completion)
{
    console_write(d, length);
    if (completion)
        apply(completion, t, length);
    return length;
}

closure_function(0, 1, u32, std_output_events,
                 thread, t /* ignore */)
{
    return EPOLLOUT;
}

closure_function(0, 1, u32, std_input_events,
                 thread, t /* ignore */)
{
    return 0;
}

extern struct syscall *linux_syscalls;

static boolean create_stdfiles(unix_heaps uh, process p)
{
    heap h = heap_general((kernel_heaps)uh);
    file in = unix_cache_alloc(uh, file);
    file out = unix_cache_alloc(uh, file);
    file err = unix_cache_alloc(uh, file);
    if (!in || !out || !err) {
        msg_err("failed to allocate files\n");
        return false;
    }
    assert(allocate_fd(p, in) == 0);
    assert(allocate_fd(p, out) == 1);
    assert(allocate_fd(p, err) == 2);

    /* Writes to in, reads from out and err act as if handled by the
       out and in files respectively. */
    init_fdesc(h, &in->f, FDESC_TYPE_STDIO);
    in->f.close = closure(h, std_close, in);
    init_fdesc(h, &out->f, FDESC_TYPE_STDIO);
    out->f.close = closure(h, std_close, out);
    init_fdesc(h, &err->f, FDESC_TYPE_STDIO);
    err->f.close = closure(h, std_close, err);
    in->f.write = out->f.write = err->f.write = closure(h, stdout);
    in->f.read = out->f.read = err->f.read = closure(h, dummy_read);
    in->f.flags = out->f.flags = err->f.flags = O_RDWR;
    out->f.events = err->f.events = closure(h, std_output_events);
    in->f.events = closure(h, std_input_events);
    return true;
}

process create_process(unix_heaps uh, tuple root, filesystem fs)
{
    kernel_heaps kh = (kernel_heaps)uh;
    heap h = heap_general(kh);
    process p = allocate(h, sizeof(struct process));
    boolean aslr = table_find(root, sym(noaslr)) == 0;

    p->uh = uh;
    p->brk = 0;
    p->pid = allocate_u64((heap)uh->processes, 1);

    /* don't need these for kernel process */
    if (p->pid > 1) {
        p->virtual = create_id_heap(h, h, PROCESS_VIRTUAL_HEAP_START,
                                    PROCESS_VIRTUAL_HEAP_LENGTH, HUGE_PAGESIZE);
        assert(p->virtual != INVALID_ADDRESS);
        assert(id_heap_set_area(heap_virtual_huge((kernel_heaps)uh),
                                PROCESS_VIRTUAL_HEAP_START, PROCESS_VIRTUAL_HEAP_LENGTH,
                                true, true));
        p->virtual_page = create_id_heap_backed(h, heap_backed(kh), (heap)p->virtual, PAGESIZE);
        assert(p->virtual_page != INVALID_ADDRESS);
        if (aslr)
            id_heap_set_randomize(p->virtual_page, true);

        /* This heap is used to track the lowest 32 bits of process
           address space. Allocations are presently only made from the
           top half for MAP_32BIT mappings. */
        p->virtual32 = create_id_heap(h, h, 0, 0x100000000, PAGESIZE);
        assert(p->virtual32 != INVALID_ADDRESS);
        if (aslr)
            id_heap_set_randomize(p->virtual32, true);
        mmap_process_init(p);
        init_vdso(p);
    } else {
        p->virtual = p->virtual_page = p->virtual32 = 0;
        p->vareas = p->vmaps = INVALID_ADDRESS;
    }
    p->fs = fs;
    p->cwd = root;
    p->process_root = root;
    p->fdallocator = create_id_heap(h, h, 0, infinity, 1);
    p->files = allocate_vector(h, 64);
    zero(p->files, sizeof(p->files));
    create_stdfiles(uh, p);
    init_threads(p);
    p->syscalls = linux_syscalls;
    p->sysctx = false;
    p->utime = p->stime = 0;
    p->start_time = now(CLOCK_ID_MONOTONIC);
    init_sigstate(&p->signals);
    zero(p->sigactions, sizeof(p->sigactions));
    p->posix_timer_ids = create_id_heap(h, h, 0, U32_MAX, 1);
    p->posix_timers = allocate_vector(h, 8);
    p->itimers = allocate_vector(h, 3);
    return p;
}

void proc_enter_user(process p)
{
    if (p->sysctx) {
        timestamp here = now(CLOCK_ID_MONOTONIC);
        p->stime += here - p->start_time;
        p->sysctx = false;
        p->start_time = here;
    }
}

void proc_enter_system(process p)
{
    if (!p->sysctx) {
        timestamp here = now(CLOCK_ID_MONOTONIC);
        p->utime += here - p->start_time;
        p->sysctx = true;
        p->start_time = here;
    }
}

void proc_pause(process p)
{
    timestamp here = now(CLOCK_ID_MONOTONIC);
    if (p->sysctx) {
        p->stime += here - p->start_time;
    }
    else {
        p->utime += here - p->start_time;
    }
}

void proc_resume(process p)
{
    p->start_time = now(CLOCK_ID_MONOTONIC);
}

timestamp proc_utime(process p)
{
    timestamp utime = p->utime;
    if (!p->sysctx) {
        utime += now(CLOCK_ID_MONOTONIC) - p->start_time;
    }
    return utime;
}

timestamp proc_stime(process p)
{
    timestamp stime = p->stime;
    if (p->sysctx) {
        stime += now(CLOCK_ID_MONOTONIC) - p->start_time;
    }
    return stime;
}

process init_unix(kernel_heaps kh, tuple root, filesystem fs)
{
    heap h = heap_general(kh);
    unix_heaps uh = allocate(h, sizeof(struct unix_heaps));

    /* a failure here means termination; just leak */
    if (uh == INVALID_ADDRESS)
	return INVALID_ADDRESS;

    uh->kh = *kh;
    uh->processes = create_id_heap(h, h, 1, 65535, 1);
    uh->file_cache = allocate_objcache(h, heap_backed(kh), sizeof(struct file), PAGESIZE);
    if (uh->file_cache == INVALID_ADDRESS)
	goto alloc_fail;
    if (!poll_init(uh))
	goto alloc_fail;
    if (!pipe_init(uh))
	goto alloc_fail;
    if (!unix_timers_init(uh))
        goto alloc_fail;
    if (ftrace_init(uh, fs))
	goto alloc_fail;

    set_syscall_handler(syscall_enter);
    process kernel_process = create_process(uh, root, fs);
    dummy_thread = create_thread(kernel_process);
    runtime_memcpy(dummy_thread->name, "dummy_thread",
        sizeof(dummy_thread->name));

    for (int i = 0; i < MAX_CPUS; i++)
        cpuinfo_from_id(i)->current_thread = dummy_thread;

    /* XXX remove once we have http PUT support */
    ftrace_enable();

    /* Install a fault handler for use when anonymous pages are
       faulted in within the interrupt handler (e.g. syscall bottom
       halves, I/O directly to user buffers). This is permissible now
       because we only support one process address space. Should this
       ever change, this will need to be reworked; either we make
       faults from the interrupt handler illegal or store a reference
       to the relevant thread frame upon entering the bottom half
       routine.
    */
    install_fallback_fault_handler((fault_handler)&dummy_thread->fault_handler);

    register_special_files(kernel_process);
    init_syscalls();
    register_file_syscalls(linux_syscalls);
#ifdef NET
    if (!netsyscall_init(uh))
	goto alloc_fail;
    register_net_syscalls(linux_syscalls);
#endif

    register_signal_syscalls(linux_syscalls);
    register_mmap_syscalls(linux_syscalls);
    register_thread_syscalls(linux_syscalls);
    register_poll_syscalls(linux_syscalls);
    register_clock_syscalls(linux_syscalls);
    register_timer_syscalls(linux_syscalls);
    register_other_syscalls(linux_syscalls);
    configure_syscalls(kernel_process);
    return kernel_process;
  alloc_fail:
    msg_err("failed to allocate kernel objects\n");
    return INVALID_ADDRESS;
}
