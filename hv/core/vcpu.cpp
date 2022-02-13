#include "vcpu.h"
#include "gdt.h"
#include "idt.h"
#include "exit-handlers.h"

#include "../util/mm.h"
#include "../util/vmx.h"
#include "../util/arch.h"
#include "../util/segment.h"
#include "../util/trap-frame.h"
#include "../util/guest-context.h"

namespace hv {

// defined in vm-launch.asm
extern bool __vm_launch();

// defined in vm-exit.asm
extern void __vm_exit();

// virtualize the current cpu
// note, this assumes that execution is already restricted to the desired cpu
bool vcpu::virtualize() {
  if (!enable_vmx_operation())
    return false;

  DbgPrint("[hv] enabled vmx operation.\n");

  if (!enter_vmx_operation())
    return false;

  DbgPrint("[hv] entered vmx operation.\n");

  if (!set_vmcs_pointer()) {
    // TODO: cleanup
    vmx_vmxoff();
    return false;
  }

  DbgPrint("[hv] set vmcs pointer.\n");

  prepare_external_structures();

  DbgPrint("[hv] initialized external host structures.\n");

  write_vmcs_ctrl_fields();
  write_vmcs_host_fields();
  write_vmcs_guest_fields();

  DbgPrint("[hv] initialized the vmcs.\n");

  // launch the virtual machine
  if (!__vm_launch()) {
    DbgPrint("[hv] vmlaunch failed, error = %lli.\n", vmx_vmread(VMCS_VM_INSTRUCTION_ERROR));

    // TODO: cleanup
    vmx_vmxoff();
    return false;
  }

  DbgPrint("[hv] virtualized cpu #%i\n", KeGetCurrentProcessorIndex());

  return true;
}

// perform certain actions that are required before entering vmx operation
bool vcpu::enable_vmx_operation() {
  cpuid_eax_01 cpuid_1;
  __cpuid(reinterpret_cast<int*>(&cpuid_1), 1);

  // 3.23.6
  if (!cpuid_1.cpuid_feature_information_ecx.virtual_machine_extensions)
    return false;

  ia32_feature_control_register feature_control;
  feature_control.flags = __readmsr(IA32_FEATURE_CONTROL);

  // 3.23.7
  if (!feature_control.lock_bit || !feature_control.enable_vmx_outside_smx)
    return false;

  _disable();

  auto cr0 = __readcr0();
  auto cr4 = __readcr4();

  // 3.23.7
  cr4 |= CR4_VMX_ENABLE_FLAG;

  // 3.23.8
  cr0 |= __readmsr(IA32_VMX_CR0_FIXED0);
  cr0 &= __readmsr(IA32_VMX_CR0_FIXED1);
  cr4 |= __readmsr(IA32_VMX_CR4_FIXED0);
  cr4 &= __readmsr(IA32_VMX_CR4_FIXED1);

  __writecr0(cr0);
  __writecr4(cr4);

  _enable();

  return true;
}

// enter vmx operation by executing VMXON
bool vcpu::enter_vmx_operation() {
  ia32_vmx_basic_register vmx_basic;
  vmx_basic.flags = __readmsr(IA32_VMX_BASIC);

  // 3.24.11.5
  vmxon_.revision_id = vmx_basic.vmcs_revision_id;
  vmxon_.must_be_zero = 0;

  auto vmxon_phys = get_physical(&vmxon_);
  NT_ASSERT(vmxon_phys % 0x1000 == 0);

  // enter vmx operation
  if (!vmx_vmxon(vmxon_phys)) {
    // TODO: cleanup
    return false;
  }

  // 3.28.3.3.4
  vmx_invept(invept_all_context, {});

  return true;
}

// set the working-vmcs pointer to point to our vmcs structure
bool vcpu::set_vmcs_pointer() {
  ia32_vmx_basic_register vmx_basic;
  vmx_basic.flags = __readmsr(IA32_VMX_BASIC);

  // 3.24.2
  vmcs_.revision_id = vmx_basic.vmcs_revision_id;
  vmcs_.shadow_vmcs_indicator = 0;

  auto vmcs_phys = get_physical(&vmcs_);
  NT_ASSERT(vmcs_phys % 0x1000 == 0);

  if (!vmx_vmclear(vmcs_phys))
    return false;

  if (!vmx_vmptrld(vmcs_phys))
    return false;

  return true;
}

// initialize external structures
void vcpu::prepare_external_structures() {
  // setup the msr bitmap so that we don't vm-exit on any msr access
  memset(&msr_bitmap_, 0, sizeof(msr_bitmap_));

  // we don't care about anything that is in the TSS
  memset(&host_tss_, 0, sizeof(host_tss_));

  prepare_host_idt(host_idt_);

  prepare_host_gdt(host_gdt_, reinterpret_cast<uint64_t>(&host_tss_));
}

// write VMCS control fields
void vcpu::write_vmcs_ctrl_fields() {
  // 3.26.2

  // 3.24.6.1
  ia32_vmx_pinbased_ctls_register pin_based_ctrl;
  pin_based_ctrl.flags       = 0;
  pin_based_ctrl.virtual_nmi = 1;
  pin_based_ctrl.nmi_exiting = 1;
  write_ctrl_pin_based_safe(pin_based_ctrl);

  // 3.24.6.2
  ia32_vmx_procbased_ctls_register proc_based_ctrl;
  proc_based_ctrl.flags                       = 0;
#ifndef NDEBUG
  proc_based_ctrl.cr3_load_exiting            = 1;
  proc_based_ctrl.cr3_store_exiting           = 1;
#endif
  proc_based_ctrl.use_msr_bitmaps             = 1;
  proc_based_ctrl.activate_secondary_controls = 1;
  write_ctrl_proc_based_safe(proc_based_ctrl);

  // 3.24.6.2
  ia32_vmx_procbased_ctls2_register proc_based_ctrl2;
  proc_based_ctrl2.flags                  = 0;
  proc_based_ctrl2.enable_rdtscp          = 1;
  proc_based_ctrl2.enable_invpcid         = 1;
  proc_based_ctrl2.enable_xsaves          = 1;
  proc_based_ctrl2.enable_user_wait_pause = 1;
  proc_based_ctrl2.conceal_vmx_from_pt    = 1;
  write_ctrl_proc_based2_safe(proc_based_ctrl2);

  // 3.24.7
  ia32_vmx_exit_ctls_register exit_ctrl;
  exit_ctrl.flags                   = 0;
  exit_ctrl.save_debug_controls     = 1;
  exit_ctrl.host_address_space_size = 1;
  exit_ctrl.conceal_vmx_from_pt     = 1;
  write_ctrl_exit_safe(exit_ctrl);

  // 3.24.8
  ia32_vmx_entry_ctls_register entry_ctrl;
  entry_ctrl.flags               = 0;
  entry_ctrl.load_debug_controls = 1;
  entry_ctrl.ia32e_mode_guest    = 1;
  entry_ctrl.conceal_vmx_from_pt = 1;
  write_ctrl_entry_safe(entry_ctrl);

  // 3.24.6.3
  vmx_vmwrite(VMCS_CTRL_EXCEPTION_BITMAP, 0);

  // set up the pagefault mask and match in such a way so
  // that a vm-exit is never triggered for a pagefault
  vmx_vmwrite(VMCS_CTRL_PAGEFAULT_ERROR_CODE_MASK,  0);
  vmx_vmwrite(VMCS_CTRL_PAGEFAULT_ERROR_CODE_MATCH, 0);

  // 3.24.6.6
  vmx_vmwrite(VMCS_CTRL_CR4_GUEST_HOST_MASK, 0);
  vmx_vmwrite(VMCS_CTRL_CR4_READ_SHADOW,     0);
  vmx_vmwrite(VMCS_CTRL_CR0_GUEST_HOST_MASK, 0);
  vmx_vmwrite(VMCS_CTRL_CR0_READ_SHADOW,     0);

  // 3.24.6.7
  vmx_vmwrite(VMCS_CTRL_CR3_TARGET_COUNT,   0);
  vmx_vmwrite(VMCS_CTRL_CR3_TARGET_VALUE_0, 0);
  vmx_vmwrite(VMCS_CTRL_CR3_TARGET_VALUE_1, 0);
  vmx_vmwrite(VMCS_CTRL_CR3_TARGET_VALUE_2, 0);
  vmx_vmwrite(VMCS_CTRL_CR3_TARGET_VALUE_3, 0);

  // 3.24.6.9
  vmx_vmwrite(VMCS_CTRL_MSR_BITMAP_ADDRESS, get_physical(&msr_bitmap_));

  // 3.24.7.2
  vmx_vmwrite(VMCS_CTRL_VMEXIT_MSR_STORE_COUNT,   0);
  vmx_vmwrite(VMCS_CTRL_VMEXIT_MSR_STORE_ADDRESS, 0);
  vmx_vmwrite(VMCS_CTRL_VMEXIT_MSR_LOAD_COUNT,    0);
  vmx_vmwrite(VMCS_CTRL_VMEXIT_MSR_LOAD_ADDRESS,  0);

  // 3.24.8.2
  vmx_vmwrite(VMCS_CTRL_VMENTRY_MSR_LOAD_COUNT,   0);
  vmx_vmwrite(VMCS_CTRL_VMENTRY_MSR_LOAD_ADDRESS, 0);

  // 3.24.8.3
  vmx_vmwrite(VMCS_CTRL_VMENTRY_INTERRUPTION_INFORMATION_FIELD, 0);
  vmx_vmwrite(VMCS_CTRL_VMENTRY_EXCEPTION_ERROR_CODE,           0);
  vmx_vmwrite(VMCS_CTRL_VMENTRY_INSTRUCTION_LENGTH,             0);
}

// write VMCS host fields
void vcpu::write_vmcs_host_fields() {
  // 3.24.5
  // 3.26.2

  // TODO: we should be using our own control registers (even for cr0/cr4)
  vmx_vmwrite(VMCS_HOST_CR0, __readcr0());
  vmx_vmwrite(VMCS_HOST_CR3, __readcr3());
  vmx_vmwrite(VMCS_HOST_CR4, __readcr4());

  // ensure that rsp is NOT aligned to 16 bytes when execution starts
  auto const rsp = ((reinterpret_cast<size_t>(
    host_stack_) + host_stack_size) & ~0b1111ull) - 8;

  vmx_vmwrite(VMCS_HOST_RSP, rsp);
  vmx_vmwrite(VMCS_HOST_RIP, reinterpret_cast<size_t>(__vm_exit));

  vmx_vmwrite(VMCS_HOST_CS_SELECTOR, host_cs_selector.flags);
  vmx_vmwrite(VMCS_HOST_SS_SELECTOR, 0x00);
  vmx_vmwrite(VMCS_HOST_DS_SELECTOR, 0x00);
  vmx_vmwrite(VMCS_HOST_ES_SELECTOR, 0x00);
  vmx_vmwrite(VMCS_HOST_FS_SELECTOR, 0x00);
  vmx_vmwrite(VMCS_HOST_GS_SELECTOR, 0x00);
  vmx_vmwrite(VMCS_HOST_TR_SELECTOR, host_tr_selector.flags);

  vmx_vmwrite(VMCS_HOST_FS_BASE,   0);
  vmx_vmwrite(VMCS_HOST_GS_BASE,   0);
  vmx_vmwrite(VMCS_HOST_TR_BASE,   reinterpret_cast<size_t>(&host_tss_));
  vmx_vmwrite(VMCS_HOST_GDTR_BASE, reinterpret_cast<size_t>(&host_gdt_));
  vmx_vmwrite(VMCS_HOST_IDTR_BASE, reinterpret_cast<size_t>(&host_idt_));

  vmx_vmwrite(VMCS_HOST_SYSENTER_CS,  0);
  vmx_vmwrite(VMCS_HOST_SYSENTER_ESP, 0);
  vmx_vmwrite(VMCS_HOST_SYSENTER_EIP, 0);
}

// write VMCS guest fields
void vcpu::write_vmcs_guest_fields() {
  // 3.24.4
  // 3.26.3

  vmx_vmwrite(VMCS_GUEST_CR0, __readcr0());
  vmx_vmwrite(VMCS_GUEST_CR3, __readcr3());
  vmx_vmwrite(VMCS_GUEST_CR4, __readcr4());

  vmx_vmwrite(VMCS_GUEST_DR7, __readdr(7));

  // RIP and RSP are set in vm-launch.asm
  vmx_vmwrite(VMCS_GUEST_RSP, 0);
  vmx_vmwrite(VMCS_GUEST_RIP, 0);
  vmx_vmwrite(VMCS_GUEST_RFLAGS, __readeflags());

  // TODO: don't hardcode the segment selectors idiot...

  vmx_vmwrite(VMCS_GUEST_CS_SELECTOR,   0x10);
  vmx_vmwrite(VMCS_GUEST_SS_SELECTOR,   0x18);
  vmx_vmwrite(VMCS_GUEST_DS_SELECTOR,   0x2B);
  vmx_vmwrite(VMCS_GUEST_ES_SELECTOR,   0x2B);
  vmx_vmwrite(VMCS_GUEST_FS_SELECTOR,   0x53);
  vmx_vmwrite(VMCS_GUEST_GS_SELECTOR,   0x2B);
  vmx_vmwrite(VMCS_GUEST_TR_SELECTOR,   0x40);
  vmx_vmwrite(VMCS_GUEST_LDTR_SELECTOR, 0x00);

  segment_descriptor_register_64 gdtr, idtr;
  _sgdt(&gdtr);
  __sidt(&idtr);

  vmx_vmwrite(VMCS_GUEST_CS_BASE,   segment_base(gdtr, 0x10));
  vmx_vmwrite(VMCS_GUEST_SS_BASE,   segment_base(gdtr, 0x18));
  vmx_vmwrite(VMCS_GUEST_DS_BASE,   segment_base(gdtr, 0x2B));
  vmx_vmwrite(VMCS_GUEST_ES_BASE,   segment_base(gdtr, 0x2B));
  vmx_vmwrite(VMCS_GUEST_FS_BASE,   __readmsr(IA32_FS_BASE));
  vmx_vmwrite(VMCS_GUEST_GS_BASE,   __readmsr(IA32_GS_BASE));
  vmx_vmwrite(VMCS_GUEST_TR_BASE,   segment_base(gdtr, 0x40));
  vmx_vmwrite(VMCS_GUEST_LDTR_BASE, segment_base(gdtr, 0x00));

  vmx_vmwrite(VMCS_GUEST_CS_LIMIT,   __segmentlimit(0x10));
  vmx_vmwrite(VMCS_GUEST_SS_LIMIT,   __segmentlimit(0x18));
  vmx_vmwrite(VMCS_GUEST_DS_LIMIT,   __segmentlimit(0x2B));
  vmx_vmwrite(VMCS_GUEST_ES_LIMIT,   __segmentlimit(0x2B));
  vmx_vmwrite(VMCS_GUEST_FS_LIMIT,   __segmentlimit(0x53));
  vmx_vmwrite(VMCS_GUEST_GS_LIMIT,   __segmentlimit(0x2B));
  vmx_vmwrite(VMCS_GUEST_TR_LIMIT,   __segmentlimit(0x40));
  vmx_vmwrite(VMCS_GUEST_LDTR_LIMIT, __segmentlimit(0x00));

  vmx_vmwrite(VMCS_GUEST_CS_ACCESS_RIGHTS,   segment_access(gdtr, 0x10).flags);
  vmx_vmwrite(VMCS_GUEST_SS_ACCESS_RIGHTS,   segment_access(gdtr, 0x18).flags);
  vmx_vmwrite(VMCS_GUEST_DS_ACCESS_RIGHTS,   segment_access(gdtr, 0x2B).flags);
  vmx_vmwrite(VMCS_GUEST_ES_ACCESS_RIGHTS,   segment_access(gdtr, 0x2B).flags);
  vmx_vmwrite(VMCS_GUEST_FS_ACCESS_RIGHTS,   segment_access(gdtr, 0x53).flags);
  vmx_vmwrite(VMCS_GUEST_GS_ACCESS_RIGHTS,   segment_access(gdtr, 0x2B).flags);
  vmx_vmwrite(VMCS_GUEST_TR_ACCESS_RIGHTS,   segment_access(gdtr, 0x40).flags);
  vmx_vmwrite(VMCS_GUEST_LDTR_ACCESS_RIGHTS, segment_access(gdtr, 0x00).flags);

  vmx_vmwrite(VMCS_GUEST_GDTR_BASE, gdtr.base_address);
  vmx_vmwrite(VMCS_GUEST_IDTR_BASE, idtr.base_address);

  vmx_vmwrite(VMCS_GUEST_GDTR_LIMIT, gdtr.limit);
  vmx_vmwrite(VMCS_GUEST_IDTR_LIMIT, idtr.limit);

  vmx_vmwrite(VMCS_GUEST_DEBUGCTL,     __readmsr(IA32_DEBUGCTL));
  vmx_vmwrite(VMCS_GUEST_SYSENTER_CS,  __readmsr(IA32_SYSENTER_CS));
  vmx_vmwrite(VMCS_GUEST_SYSENTER_ESP, __readmsr(IA32_SYSENTER_ESP));
  vmx_vmwrite(VMCS_GUEST_SYSENTER_EIP, __readmsr(IA32_SYSENTER_EIP));

  vmx_vmwrite(VMCS_GUEST_ACTIVITY_STATE, vmx_active);

  vmx_vmwrite(VMCS_GUEST_INTERRUPTIBILITY_STATE, 0);

  vmx_vmwrite(VMCS_GUEST_PENDING_DEBUG_EXCEPTIONS, 0);

  vmx_vmwrite(VMCS_GUEST_VMCS_LINK_POINTER, 0xFFFFFFFF'FFFFFFFFull);
}

// called for every vm-exit
void vcpu::handle_vm_exit(guest_context* const ctx) {
  vmx_vmexit_reason exit_reason;
  exit_reason.flags = static_cast<uint32_t>(vmx_vmread(VMCS_EXIT_REASON));

  switch (exit_reason.basic_exit_reason) {
  case VMX_EXIT_REASON_MOV_CR:
    handle_mov_cr(ctx);
    break;
  case VMX_EXIT_REASON_EXECUTE_CPUID:
    emulate_cpuid(ctx);
    break;
  case VMX_EXIT_REASON_EXECUTE_RDMSR:
    emulate_rdmsr(ctx);
    break;
  case VMX_EXIT_REASON_EXECUTE_WRMSR:
    emulate_wrmsr(ctx);
    break;
  case VMX_EXIT_REASON_EXCEPTION_OR_NMI:
    handle_exception_or_nmi(ctx);
    break;
  case VMX_EXIT_REASON_NMI_WINDOW:
    handle_nmi_window(ctx);
    break;
  default:
    __debugbreak();
    DbgPrint("[hv] vm-exit occurred. RIP=0x%zX.\n", vmx_vmread(VMCS_GUEST_RIP));
    break;
  }
}

// called for every host interrupt
void vcpu::handle_host_interrupt(trap_frame* const frame) {
  switch (frame->vector) {
  // host NMIs
  case nmi:
    auto ctrl = read_ctrl_proc_based();
    ctrl.nmi_window_exiting = 1;
    write_ctrl_proc_based(ctrl);
    break;
  }
}

} // namespace hv
