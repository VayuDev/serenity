/*
 * Copyright (c) 2022, Timon Kruiper <timonkruiper@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Format.h>
#include <AK/Vector.h>

#include <Kernel/Arch/Processor.h>
#include <Kernel/Arch/TrapFrame.h>
#include <Kernel/Arch/aarch64/ASM_wrapper.h>
#include <Kernel/Arch/aarch64/CPU.h>
#include <Kernel/InterruptDisabler.h>
#include <Kernel/Process.h>
#include <Kernel/Random.h>
#include <Kernel/Scheduler.h>
#include <Kernel/Thread.h>
#include <Kernel/Time/TimeManagement.h>

extern "C" uintptr_t vector_table_el1;

namespace Kernel {

extern "C" void thread_context_first_enter(void);
extern "C" void exit_kernel_thread(void);
extern "C" void context_first_init(Thread* from_thread, Thread* to_thread) __attribute__((used));
extern "C" void enter_thread_context(Thread* from_thread, Thread* to_thread) __attribute__((used));

Processor* g_current_processor;

void Processor::initialize(u32 cpu)
{
    VERIFY(g_current_processor == nullptr);

    auto current_exception_level = static_cast<u64>(Aarch64::Asm::get_current_exception_level());
    dbgln("CPU{} started in: EL{}", cpu, current_exception_level);

    dbgln("Drop CPU{} to EL1", cpu);
    drop_to_exception_level_1();

    // Load EL1 vector table
    Aarch64::Asm::el1_vector_table_install(&vector_table_el1);

    g_current_processor = this;
}

[[noreturn]] void Processor::halt()
{
    disable_interrupts();
    for (;;)
        asm volatile("wfi");
}

void Processor::flush_tlb_local(VirtualAddress, size_t)
{
    // FIXME: Figure out how to flush a single page
    asm volatile("dsb ishst");
    asm volatile("tlbi vmalle1is");
    asm volatile("dsb ish");
    asm volatile("isb");
}

void Processor::flush_tlb(Memory::PageDirectory const*, VirtualAddress vaddr, size_t page_count)
{
    flush_tlb_local(vaddr, page_count);
}

u32 Processor::clear_critical()
{
    InterruptDisabler disabler;
    auto prev_critical = in_critical();
    auto& proc = current();
    proc.m_in_critical = 0;
    if (proc.m_in_irq == 0)
        proc.check_invoke_scheduler();
    return prev_critical;
}

u32 Processor::smp_wake_n_idle_processors(u32 wake_count)
{
    (void)wake_count;
    // FIXME: Actually wake up other cores when SMP is supported for aarch64.
    return 0;
}

void Processor::initialize_context_switching(Thread& initial_thread)
{
    VERIFY(initial_thread.process().is_kernel_process());

    m_scheduler_initialized = true;

    // FIXME: Figure out if we need to call {pre_,post_,}init_finished once aarch64 supports SMP
    Processor::set_current_in_scheduler(true);

    auto& regs = initial_thread.regs();
    // clang-format off
    asm volatile(
        "mov sp, %[new_sp] \n"

        "sub sp, sp, 24 \n"
        "str %[from_to_thread], [sp, #0] \n"
        "str %[from_to_thread], [sp, #8] \n"
        "br %[new_ip] \n"
        :: [new_sp] "r" (regs.sp_el0),
        [new_ip] "r" (regs.elr_el1),
        [from_to_thread] "r" (&initial_thread)
    );
    // clang-format on

    VERIFY_NOT_REACHED();
}

void Processor::switch_context(Thread*& from_thread, Thread*& to_thread)
{
    VERIFY(!m_in_irq);
    VERIFY(m_in_critical == 1);

    dbgln_if(CONTEXT_SWITCH_DEBUG, "switch_context --> switching out of: {} {}", VirtualAddress(from_thread), *from_thread);

    // m_in_critical is restored in enter_thread_context
    from_thread->save_critical(m_in_critical);

    // clang-format off
    asm volatile(
        "sub sp, sp, #248 \n"
        "stp x0, x1,     [sp, #(0 * 0)] \n"
        "stp x2, x3,     [sp, #(2 * 8)] \n"
        "stp x4, x5,     [sp, #(4 * 8)] \n"
        "stp x6, x7,     [sp, #(6 * 8)] \n"
        "stp x8, x9,     [sp, #(8 * 8)] \n"
        "stp x10, x11,   [sp, #(10 * 8)] \n"
        "stp x12, x13,   [sp, #(12 * 8)] \n"
        "stp x14, x15,   [sp, #(14 * 8)] \n"
        "stp x16, x17,   [sp, #(16 * 8)] \n"
        "stp x18, x19,   [sp, #(18 * 8)] \n"
        "stp x20, x21,   [sp, #(20 * 8)] \n"
        "stp x22, x23,   [sp, #(22 * 8)] \n"
        "stp x24, x25,   [sp, #(24 * 8)] \n"
        "stp x26, x27,   [sp, #(26 * 8)] \n"
        "stp x28, x29,   [sp, #(28 * 8)] \n"
        "str x30,        [sp, #(30 * 8)] \n"
        "mov x0, sp \n"
        "str x0, %[from_sp] \n"
        "ldr x0, =1f \n"
        "str x0, %[from_ip] \n"

        "ldr x0, %[to_sp] \n"
        "mov sp, x0 \n"

        "sub sp, sp, 24 \n"
        "ldr x0, %[from_thread] \n"
        "ldr x1, %[to_thread] \n"
        "ldr x2, %[to_ip] \n"
        "str x0, [sp, #0] \n"
        "str x1, [sp, #8] \n"
        "str x2, [sp, #16] \n"

        "bl enter_thread_context \n"
        "ldr x0, [sp, #16]\n"
        "br x0 \n"

        "1: \n"
        "add sp, sp, 24 \n"

        "ldp x0, x1,     [sp, #(0 * 0)] \n"
        "ldp x2, x3,     [sp, #(2 * 8)] \n"
        "ldp x4, x5,     [sp, #(4 * 8)] \n"
        "ldp x6, x7,     [sp, #(6 * 8)] \n"
        "ldp x8, x9,     [sp, #(8 * 8)] \n"
        "ldp x10, x11,   [sp, #(10 * 8)] \n"
        "ldp x12, x13,   [sp, #(12 * 8)] \n"
        "ldp x14, x15,   [sp, #(14 * 8)] \n"
        "ldp x16, x17,   [sp, #(16 * 8)] \n"
        "ldp x18, x19,   [sp, #(18 * 8)] \n"
        "ldp x20, x21,   [sp, #(20 * 8)] \n"
        "ldp x22, x23,   [sp, #(22 * 8)] \n"
        "ldp x24, x25,   [sp, #(24 * 8)] \n"
        "ldp x26, x27,   [sp, #(26 * 8)] \n"
        "ldp x28, x29,   [sp, #(28 * 8)] \n"
        "ldr x30,        [sp, #(30 * 8)] \n"

        "sub sp, sp, 24 \n"
        "ldr x0, [sp, #0] \n"
        "ldr x1, [sp, #8] \n"
        "str x0, %[from_thread] \n"
        "str x1, %[to_thread] \n"

        "add sp, sp, #272 \n"
        :
        [from_ip] "=m"(from_thread->regs().elr_el1),
        [from_sp] "=m"(from_thread->regs().sp_el0),
        "=m"(from_thread),
        "=m"(to_thread)

        : [to_ip] "m"(to_thread->regs().elr_el1),
        [to_sp] "m"(to_thread->regs().sp_el0),
        [from_thread] "m"(from_thread),
        [to_thread] "m"(to_thread)
        : "memory", "x0", "x1", "x2");
    // clang-format on

    dbgln_if(CONTEXT_SWITCH_DEBUG, "switch_context <-- from {} {} to {} {}", VirtualAddress(from_thread), *from_thread, VirtualAddress(to_thread), *to_thread);
}

void Processor::assume_context(Thread& thread, FlatPtr flags)
{
    (void)thread;
    (void)flags;
    TODO_AARCH64();
}

FlatPtr Processor::init_context(Thread& thread, bool leave_crit)
{
    VERIFY(g_scheduler_lock.is_locked());
    if (leave_crit) {
        // Leave the critical section we set up in Process::exec,
        // but because we still have the scheduler lock we should end up with 1
        VERIFY(in_critical() == 2);
        m_in_critical = 1; // leave it without triggering anything or restoring flags
    }

    u64 kernel_stack_top = thread.kernel_stack_top();

    // Add a random offset between 0-256 (16-byte aligned)
    kernel_stack_top -= round_up_to_power_of_two(get_fast_random<u8>(), 16);

    u64 stack_top = kernel_stack_top;

    auto& thread_regs = thread.regs();

    // Push a RegisterState and TrapFrame onto the stack, which will be popped of the stack and restored into the
    // state of the processor by restore_previous_context.
    stack_top -= sizeof(RegisterState);
    RegisterState& eretframe = *reinterpret_cast<RegisterState*>(stack_top);
    memcpy(eretframe.x, thread_regs.x, sizeof(thread_regs.x));

    // x30 is the Link Register for the aarch64 ABI, so this will return to exit_kernel_thread when main thread function returns.
    eretframe.x[30] = FlatPtr(&exit_kernel_thread);
    eretframe.elr_el1 = thread_regs.elr_el1;
    eretframe.sp_el0 = kernel_stack_top;
    eretframe.tpidr_el0 = 0; // FIXME: Correctly initialize this when aarch64 has support for thread local storage.

    Aarch64::SPSR_EL1 saved_program_status_register_el1 = {};

    // Don't mask any interrupts, so all interrupts are enabled when transfering into the new context
    saved_program_status_register_el1.D = 0;
    saved_program_status_register_el1.A = 0;
    saved_program_status_register_el1.I = 0;
    saved_program_status_register_el1.F = 0;

    // Set exception origin mode to EL1t, so when the context is restored, we'll be executing in EL1 with SP_EL0
    // FIXME: This must be EL0t when aarch64 supports userspace applications.
    saved_program_status_register_el1.M = Aarch64::SPSR_EL1::Mode::EL1t;
    memcpy(&eretframe.spsr_el1, &saved_program_status_register_el1, sizeof(u64));

    // Push a TrapFrame onto the stack
    stack_top -= sizeof(TrapFrame);
    TrapFrame& trap = *reinterpret_cast<TrapFrame*>(stack_top);
    trap.regs = &eretframe;
    trap.next_trap = nullptr;

    if constexpr (CONTEXT_SWITCH_DEBUG) {
        dbgln("init_context {} ({}) set up to execute at ip={}, sp={}, stack_top={}",
            thread,
            VirtualAddress(&thread),
            VirtualAddress(thread_regs.elr_el1),
            VirtualAddress(thread_regs.sp_el0),
            VirtualAddress(stack_top));
    }

    // This make sure the thread first executes thread_context_first_enter, which will actually call restore_previous_context
    // which restores the context set up above.
    thread_regs.set_sp(stack_top);
    thread_regs.set_ip(FlatPtr(&thread_context_first_enter));

    return stack_top;
}

void Processor::enter_trap(TrapFrame& trap, bool raise_irq)
{
    VERIFY_INTERRUPTS_DISABLED();
    VERIFY(&Processor::current() == this);
    // FIXME: Figure out if we need prev_irq_level, see duplicated code in Kernel/Arch/x86/common/Processor.cpp
    if (raise_irq)
        m_in_irq++;
    auto* current_thread = Processor::current_thread();
    if (current_thread) {
        auto& current_trap = current_thread->current_trap();
        trap.next_trap = current_trap;
        current_trap = &trap;
        // FIXME: Determine PreviousMode from TrapFrame when userspace programs can run on aarch64
        auto new_previous_mode = Thread::PreviousMode::KernelMode;
        if (current_thread->set_previous_mode(new_previous_mode)) {
            current_thread->update_time_scheduled(TimeManagement::scheduler_current_time(), new_previous_mode == Thread::PreviousMode::KernelMode, false);
        }
    } else {
        trap.next_trap = nullptr;
    }
}

void Processor::exit_trap(TrapFrame& trap)
{
    VERIFY_INTERRUPTS_DISABLED();
    VERIFY(&Processor::current() == this);

    // Temporarily enter a critical section. This is to prevent critical
    // sections entered and left within e.g. smp_process_pending_messages
    // to trigger a context switch while we're executing this function
    // See the comment at the end of the function why we don't use
    // ScopedCritical here.
    m_in_critical = m_in_critical + 1;

    // FIXME: Figure out if we need prev_irq_level, see duplicated code in Kernel/Arch/x86/common/Processor.cpp
    m_in_irq = 0;

    auto* current_thread = Processor::current_thread();
    if (current_thread) {
        auto& current_trap = current_thread->current_trap();
        current_trap = trap.next_trap;
        Thread::PreviousMode new_previous_mode;
        if (current_trap) {
            VERIFY(current_trap->regs);
            // FIXME: Determine PreviousMode from TrapFrame when userspace programs can run on aarch64
            new_previous_mode = Thread::PreviousMode::KernelMode;
        } else {
            // If we don't have a higher level trap then we're back in user mode.
            // Which means that the previous mode prior to being back in user mode was kernel mode
            new_previous_mode = Thread::PreviousMode::KernelMode;
        }

        if (current_thread->set_previous_mode(new_previous_mode))
            current_thread->update_time_scheduled(TimeManagement::scheduler_current_time(), true, false);
    }

    VERIFY_INTERRUPTS_DISABLED();

    // Leave the critical section without actually enabling interrupts.
    // We don't want context switches to happen until we're explicitly
    // triggering a switch in check_invoke_scheduler.
    m_in_critical = m_in_critical - 1;
    if (!m_in_irq && !m_in_critical)
        check_invoke_scheduler();
}

ErrorOr<Vector<FlatPtr, 32>> Processor::capture_stack_trace(Thread& thread, size_t max_frames)
{
    (void)thread;
    (void)max_frames;
    TODO_AARCH64();
    return Vector<FlatPtr, 32> {};
}

void Processor::check_invoke_scheduler()
{
    VERIFY_INTERRUPTS_DISABLED();
    VERIFY(!m_in_irq);
    VERIFY(!m_in_critical);
    VERIFY(&Processor::current() == this);
    if (m_invoke_scheduler_async && m_scheduler_initialized) {
        m_invoke_scheduler_async = false;
        Scheduler::invoke_async();
    }
}

NAKED void thread_context_first_enter(void)
{
    asm(
        "ldr x0, [sp, #0] \n"
        "ldr x1, [sp, #8] \n"
        "add sp, sp, 24 \n"
        "bl context_first_init \n"
        "b restore_context_and_eret \n");
}

void exit_kernel_thread(void)
{
    Thread::current()->exit();
}

extern "C" void context_first_init([[maybe_unused]] Thread* from_thread, [[maybe_unused]] Thread* to_thread)
{
    VERIFY(!are_interrupts_enabled());

    dbgln_if(CONTEXT_SWITCH_DEBUG, "switch_context <-- from {} {} to {} {} (context_first_init)", VirtualAddress(from_thread), *from_thread, VirtualAddress(to_thread), *to_thread);

    VERIFY(to_thread == Thread::current());

    Scheduler::enter_current(*from_thread);

    auto in_critical = to_thread->saved_critical();
    VERIFY(in_critical > 0);
    Processor::restore_critical(in_critical);

    // Since we got here and don't have Scheduler::context_switch in the
    // call stack (because this is the first time we switched into this
    // context), we need to notify the scheduler so that it can release
    // the scheduler lock. We don't want to enable interrupts at this point
    // as we're still in the middle of a context switch. Doing so could
    // trigger a context switch within a context switch, leading to a crash.
    Scheduler::leave_on_first_switch(InterruptsState::Disabled);
}

extern "C" void enter_thread_context(Thread* from_thread, Thread* to_thread)
{
    VERIFY(from_thread == to_thread || from_thread->state() != Thread::State::Running);
    VERIFY(to_thread->state() == Thread::State::Running);

    Processor::set_current_thread(*to_thread);

    to_thread->set_cpu(Processor::current().id());

    auto in_critical = to_thread->saved_critical();
    VERIFY(in_critical > 0);
    Processor::restore_critical(in_critical);
}

}
