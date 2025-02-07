/*
  NoirVisor - Hardware-Accelerated Hypervisor solution

  Copyright 2018-2023, Zero Tang. All rights reserved.

  This file is the Exit Handler of CVM in SVM-Core.

  This program is distributed in the hope that it will be useful, but 
  without any warranty (no matter implied warranty or merchantability
  or fitness for a particular purpose, etc.).

  File Location: /svm_core/svm_cvexit.c
*/

#include <nvdef.h>
#include <nvbdk.h>
#include <nvstatus.h>
#include <noirhvm.h>
#include <svm_intrin.h>
#include <nv_intrin.h>
#include <amd64.h>
#include "svm_vmcb.h"
#include "svm_npt.h"
#include "svm_exit.h"
#include "svm_def.h"

void noir_hvcode nvc_svm_inject_cvm_exception(noir_gpr_state_p gpr_state,noir_svm_vcpu_p vcpu,noir_svm_custom_vcpu_p cvcpu,u8 vector,bool ev,u32 error_code,u64 pf_addr,u8 fetch_length,u8p fetched_instruction)
{
	// FIXME: If guest enabled SVM and under nested virtualization, injection should consider L1 hypervisor's interceptions.
	if(noir_bt(&cvcpu->header.exception_bitmap,vector)==false)
	{
		noir_svm_inject_event(cvcpu->vmcb.virt,vector,amd64_fault_trap_exception,ev,true,error_code);
		if(vector==amd64_page_fault)
		{
			// For #PF exceptions, CR2 must be taken care of.
			noir_svm_vmwrite64(cvcpu->vmcb.virt,guest_cr2,pf_addr);
			noir_svm_vmcb_btr32(cvcpu->vmcb.virt,vmcb_clean_bits,noir_svm_clean_cr2);
		}
		// Profiler: Classify the interception as emulation.
		cvcpu->header.statistics_internal.selector=&cvcpu->header.statistics.interceptions.emulation;
	}
	else
	{
		// The host specified interception on this exception.
		nvc_svm_switch_to_host_vcpu(gpr_state,vcpu);
		cvcpu->header.exit_context.intercept_code=cv_exception;
		cvcpu->header.exit_context.exception.vector=vector;
		cvcpu->header.exit_context.exception.ev_valid=ev;
		cvcpu->header.exit_context.exception.reserved=0;
		cvcpu->header.exit_context.exception.error_code=error_code;
		if(vector==amd64_page_fault)
		{
			cvcpu->header.exit_context.exception.pf_addr=pf_addr;
			cvcpu->header.exit_context.exception.fetched_bytes=fetch_length<15?fetch_length:15;
			noir_movsb(cvcpu->header.exit_context.exception.instruction_bytes,fetched_instruction,cvcpu->header.exit_context.exception.fetched_bytes);
		}
		// Profiler: Classify the interception as exception interception.
		cvcpu->header.statistics_internal.selector=&cvcpu->header.statistics.interceptions.exception;
	}
}

// Unexpected VM-Exit occured!
void static noir_hvcode fastcall nvc_svm_default_cvexit_handler(noir_gpr_state_p gpr_state,noir_svm_vcpu_p vcpu,noir_svm_custom_vcpu_p cvcpu)
{
	nvc_svm_switch_to_host_vcpu(gpr_state,vcpu);
	cvcpu->header.exit_context.intercept_code=cv_scheduler_bug;
}

// Expected Intercept Code: -1
void static noir_hvcode fastcall nvc_svm_invalid_state_cvexit_handler(noir_gpr_state_p gpr_state,noir_svm_vcpu_p vcpu,noir_svm_custom_vcpu_p cvcpu)
{
	void* vmcb=cvcpu->vmcb.virt;
	u64 cr0,cr3,cr4,efer,dr6,dr7;
	svm_segment_access_rights cs_attrib;
	u64 csb,dsb,esb,ssb;
	amd64_event_injection evi;
	// Invalid State in VMCB is detected by processor.
	// Deliver to subverted host.
	nvc_svm_switch_to_host_vcpu(gpr_state,vcpu);
	cvcpu->header.exit_context.intercept_code=cv_invalid_state;
	// We may record additional information to indicate why the state is invalid
	// in order to help hypervisor developer check their vCPU state settings.
	cvcpu->header.exit_context.invalid_state.id=noir_cvm_invalid_state_amd_v;
	cvcpu->header.exit_context.invalid_state.reason=noir_svm_failure_unknown_failure;
	// Preparation: Read all registers subject to consistency checks.
	cr0=noir_svm_vmread64(vmcb,guest_cr0);
	cr3=noir_svm_vmread64(vmcb,guest_cr3);
	cr4=noir_svm_vmread64(vmcb,guest_cr4);
	efer=noir_svm_vmread64(vmcb,guest_efer);
	dr6=noir_svm_vmread64(vmcb,guest_dr6);
	dr7=noir_svm_vmread64(vmcb,guest_dr7);
	cs_attrib.value=noir_svm_vmread16(vmcb,guest_cs_attrib);
	csb=noir_svm_vmread64(vmcb,guest_cs_base);
	dsb=noir_svm_vmread64(vmcb,guest_ds_base);
	esb=noir_svm_vmread64(vmcb,guest_es_base);
	ssb=noir_svm_vmread64(vmcb,guest_ss_base);
	evi.value=noir_svm_vmread64(vmcb,event_injection);
	// Examination I: Is CR0.CD reset while CR0.NW is set?
	if(!noir_bt(&cr0,amd64_cr0_cd) && noir_bt(&cr0,amd64_cr0_nw))
		cvcpu->header.exit_context.invalid_state.reason=noir_svm_failure_cr0_cd0_nw1;
	// Examination II: Are upper 32 bits of CR0 zero?
	if(cr0>0xFFFFFFFF)
		cvcpu->header.exit_context.invalid_state.reason=noir_svm_failure_cr0_high;
	// Examination III: Are there MBZ bits set in CR3?
	if(cr3>>52)
		cvcpu->header.exit_context.invalid_state.reason=noir_svm_failure_cr3_mbz;
	// Examination IV: Are there MBZ bits set in CR4?
	if(cr4 & 0xFFFFFFFFFF08F000)
		cvcpu->header.exit_context.invalid_state.reason=noir_svm_failure_cr4_mbz;
	// Examination V: Are upper 32 bits of DR6 zero?
	if(dr6>0xFFFFFFFF)
		cvcpu->header.exit_context.invalid_state.reason=noir_svm_failure_dr6_high;
	// Examination VI: Are upper 32 bits of DR7 zero?
	if(dr7>0xFFFFFFFF)
		cvcpu->header.exit_context.invalid_state.reason=noir_svm_failure_dr7_high;
	// Examination VII: Are there MBZ bits set in EFER?
	if(efer & 0xFFFFFFFFFFF90200)
		cvcpu->header.exit_context.invalid_state.reason=noir_svm_failure_efer_mbz;
	// Examination VIII:  Are EFER.LME and CR0.PG both set while CR4.PAE is reset?
	if(noir_bt(&efer,amd64_efer_lme) && noir_bt(&cr0,amd64_cr0_pg) && !noir_bt(&cr4,amd64_cr4_pae))
		cvcpu->header.exit_context.invalid_state.reason=noir_svm_failure_efer_lme1_cr0_pg1_cr4_pae0;
	// Examination IX: Are EFER.LME and CR0.PG both set while CR0.PE is reset?
	if(noir_bt(&efer,amd64_efer_lme) && noir_bt(&cr0,amd64_cr0_pg) && !noir_bt(&cr0,amd64_cr0_pe))
		cvcpu->header.exit_context.invalid_state.reason=noir_svm_failure_efer_lme1_cr0_pg1_pe0;
	// Examination X: Are EFER.LME, CR0.PG, CR4.PAE, CS.L, and CS.D all set?
	if(noir_bt(&efer,amd64_efer_lme) && noir_bt(&cr0,amd64_cr0_pg) && noir_bt(&cr4,amd64_cr4_pae) && cs_attrib.long_mode && cs_attrib.default_size)
		cvcpu->header.exit_context.invalid_state.reason=noir_svm_failure_efer_lme1_cr0_pg1_cr4_pae1_cs_l1_d1;
	// Examination XI: Is the event being injected illegal?
	if(evi.valid)
	{
		// Is the vector of the exception is actually not an exception?
		if(evi.type==amd64_fault_trap_exception && (evi.type>=0x20 || evi.type==amd64_nmi_interrupt))
			cvcpu->header.exit_context.invalid_state.reason=noir_svm_failure_illegal_event_injection;
		// Is type of event reserved?
		if(evi.type==1 || evi.type>4)cvcpu->header.exit_context.invalid_state.reason=noir_svm_failure_illegal_event_injection;
	}
	// Examination XII: Is the segment base canonical?
	if(csb>0x7fffffffffff && csb<0xffff800000000000)cvcpu->header.exit_context.invalid_state.reason=noir_svm_failure_incanonical_segment_base;
	if(dsb>0x7fffffffffff && dsb<0xffff800000000000)cvcpu->header.exit_context.invalid_state.reason=noir_svm_failure_incanonical_segment_base;
	if(esb>0x7fffffffffff && esb<0xffff800000000000)cvcpu->header.exit_context.invalid_state.reason=noir_svm_failure_incanonical_segment_base;
	if(ssb>0x7fffffffffff && ssb<0xffff800000000000)cvcpu->header.exit_context.invalid_state.reason=noir_svm_failure_incanonical_segment_base;
}

// Expected Intercept Code: 0x0~0x1F
void static noir_hvcode fastcall nvc_svm_cr4_read_cvexit_handler(noir_gpr_state_p gpr_state,noir_svm_vcpu_p vcpu,noir_svm_custom_vcpu_p cvcpu)
{
	// Access to CR4 register is intercepted.
	nvc_svm_cr_access_exit_info info;
	info.value=noir_svm_vmread64(cvcpu->vmcb.virt,exit_info1);
	// Process the CR4.MCE shadowing.
	ulong_ptr *gpr_array=(ulong_ptr*)gpr_state;
	gpr_array[info.gpr]=noir_svm_vmread(cvcpu->vmcb.virt,guest_cr4);
	if(!cvcpu->shadowed_bits.mce)noir_btr((u32*)&gpr_array[info.gpr],amd64_cr4_mce);
	noir_svm_advance_rip(cvcpu->vmcb.virt);
	// Profiler: CR4 handler is Hypervisor's emulation.
	cvcpu->header.statistics_internal.selector=&cvcpu->header.statistics.interceptions.emulation;
}

void static noir_hvcode fastcall nvc_svm_cr4_write_cvexit_handler(noir_gpr_state_p gpr_state,noir_svm_vcpu_p vcpu,noir_svm_custom_vcpu_p cvcpu)
{
	// Access to CR4 register is intercepted.
	nvc_svm_cr_access_exit_info info;
	info.value=noir_svm_vmread64(cvcpu->vmcb.virt,exit_info1);
	// Process the CR4.MCE shadowing.
	ulong_ptr *gpr_array=(ulong_ptr*)gpr_state;
	cvcpu->shadowed_bits.mce=noir_bt((u32*)&gpr_array[info.gpr],amd64_cr4_mce);
	noir_svm_vmwrite64(cvcpu->vmcb.virt,guest_cr4,gpr_array[info.gpr]|amd64_cr4_mce_bit);
	noir_svm_advance_rip(cvcpu->vmcb.virt);
	// Profiler: CR4 handler is Hypervisor's emulation.
	cvcpu->header.statistics_internal.selector=&cvcpu->header.statistics.interceptions.emulation;
}

void static noir_hvcode fastcall nvc_svm_cr_access_cvexit_handler(noir_gpr_state_p gpr_state,noir_svm_vcpu_p vcpu,noir_svm_custom_vcpu_p cvcpu)
{
	// Access to Control Registers is intercepted.
	// We don't have to determine whether Interception is subject to be delivered to subverted host,
	// in that accesses to control registers are only intercepted if the subverted host specifies so.
	// Switch to subverted host in order to handle this instruction.
	nvc_svm_cr_access_exit_info info;
	u32 code=noir_svm_vmread32(cvcpu->vmcb.virt,exit_code);
	info.value=noir_svm_vmread64(cvcpu->vmcb.virt,exit_info1);
	nvc_svm_switch_to_host_vcpu(gpr_state,vcpu);
	// Save the Exit Context.
	cvcpu->header.exit_context.intercept_code=cv_cr_access;
	cvcpu->header.exit_context.cr_access.cr_number=code&0xf;
	cvcpu->header.exit_context.cr_access.gpr_number=(u32)info.gpr;
	cvcpu->header.exit_context.cr_access.mov_instruction=(u32)info.mov;
	cvcpu->header.exit_context.cr_access.write=noir_bt(&code,4);
	// Profiler: Classify the interception.
	cvcpu->header.statistics_internal.selector=&cvcpu->header.statistics.interceptions.cr;
}

// Expected Intercept Code: 0x20~0x3F
void static noir_hvcode fastcall nvc_svm_dr_access_cvexit_handler(noir_gpr_state_p gpr_state,noir_svm_vcpu_p vcpu,noir_svm_custom_vcpu_p cvcpu)
{
	// Access to Debug Registers is intercepted.
	// We don't have to determine whether Interception is subject to be delivered to subverted host,
	// in that accesses to debug registers are only intercepted if the subverted host specifies so.
	// Switch to subverted host in order to handle this instruction.
	nvc_svm_dr_access_exit_info info;
	u32 code=noir_svm_vmread32(cvcpu->vmcb.virt,exit_code);
	info.value=noir_svm_vmread64(cvcpu->vmcb.virt,exit_info1);
	nvc_svm_switch_to_host_vcpu(gpr_state,vcpu);
	// Save the Exit Context.
	cvcpu->header.exit_context.intercept_code=cv_dr_access;
	cvcpu->header.exit_context.dr_access.dr_number=code&0xf;
	cvcpu->header.exit_context.dr_access.gpr_number=(u32)info.gpr;
	cvcpu->header.exit_context.dr_access.write=noir_bt(&code,4);
	// Profiler: Classify the interception.
	cvcpu->header.statistics_internal.selector=&cvcpu->header.statistics.interceptions.dr;
}

// Expected Intercept Code: 0x40~0x5F, except 0x52 and 0x5E.
void static noir_hvcode fastcall nvc_svm_exception_cvexit_handler(noir_gpr_state_p gpr_state,noir_svm_vcpu_p vcpu,noir_svm_custom_vcpu_p cvcpu)
{
	// Exception is intercepted.
	// We don't have to determine whether Exception is subject to be delivered to subverted
	// host, in that Exceptions are intercepted only if the subverted host specifies so.
	// Switch to subverted host in order to handle this instruction.
	u64 code=noir_svm_vmread32(cvcpu->vmcb.virt,exit_code);
	u64 vector=code&0x1f;
	nvc_svm_switch_to_host_vcpu(gpr_state,vcpu);
	// Deliver Exception Context...
	cvcpu->header.exit_context.intercept_code=cv_exception;
	cvcpu->header.exit_context.exception.vector=(u32)vector;
	switch(vector)
	{
		case amd64_page_fault:
		{
			// Page-Fault has more info to save.
			cvcpu->header.exit_context.exception.pf_addr=noir_svm_vmread64(cvcpu->vmcb.virt,exit_info2);
			cvcpu->header.exit_context.exception.fetched_bytes=noir_svm_vmread8(cvcpu->vmcb.virt,number_of_bytes_fetched);
			noir_movsb(cvcpu->header.exit_context.exception.instruction_bytes,(u8*)((ulong_ptr)cvcpu->vmcb.virt+guest_instruction_bytes),15);
		}
		case amd64_segment_not_present:
		case amd64_stack_segment_fault:
		case amd64_general_protection:
		case amd64_alignment_check:
		case amd64_control_protection:
		{
			cvcpu->header.exit_context.exception.ev_valid=true;
			cvcpu->header.exit_context.exception.error_code=noir_svm_vmread32(cvcpu->vmcb.virt,exit_info1);
			break;
		}
		default:
		{
			cvcpu->header.exit_context.exception.ev_valid=false;
			cvcpu->header.exit_context.exception.error_code=0;
			break;
		}
	}
	// Profiler: Classify the interception.
	cvcpu->header.statistics_internal.selector=&cvcpu->header.statistics.interceptions.exception;
}

// Expected Intercept Code: 0x52
void static noir_hvcode fastcall nvc_svm_mc_cvexit_handler(noir_gpr_state_p gpr_state,noir_svm_vcpu_p vcpu,noir_svm_custom_vcpu_p cvcpu)
{
	// Machine-Check is intercepted.
	// CVM User is not supposed to intercept a physical #MC exception.
	// Transfer the #MC exception to the host.
	nvc_svm_switch_to_host_vcpu(gpr_state,vcpu);
	// Treat #MC as a scheduler's exit.
	cvcpu->header.exit_context.intercept_code=cv_scheduler_exit;
	// Control-flow could come back if this #MC is correctable.
}

// Expected Intercept Code: 0x5E
void static noir_hvcode fastcall nvc_svm_sx_cvexit_handler(noir_gpr_state_p gpr_state,noir_svm_vcpu_p vcpu,noir_svm_custom_vcpu_p cvcpu)
{
	// Security Exception is intercepted.
	// We have to check if #SX is induced by an arrived INIT signal.
	void* vmcb=cvcpu->vmcb.virt;
	u32 error_code=noir_svm_vmread32(vmcb,exit_info1);
	switch(error_code)
	{
		case amd64_sx_init_redirection:
		{
			// This is #SX is induced by an arrived INIT signal conversion.
			nvc_svm_switch_to_host_vcpu(gpr_state,vcpu);
			cvcpu->header.exit_context.intercept_code=cv_scheduler_exit;
			// Although unlikely to be happening, emulate the INIT Signal.
			nvc_svm_emulate_init_signal(gpr_state,vcpu->vmcb.virt,vcpu->cpuid_fms);
			break;
		}
		default:
		{
			// Unknown #SX reason. Deliver to subverted host.
			nvc_svm_switch_to_host_vcpu(gpr_state,vcpu);
			cvcpu->header.exit_context.intercept_code=cv_exception;
			cvcpu->header.exit_context.exception.vector=amd64_security_exception;
			cvcpu->header.exit_context.exception.ev_valid=1;
			cvcpu->header.exit_context.exception.error_code=error_code;
			break;
		}
	}
}

// Expected Intercept Code: 0x60
void static noir_hvcode fastcall nvc_svm_extint_cvexit_handler(noir_gpr_state_p gpr_state,noir_svm_vcpu_p vcpu,noir_svm_custom_vcpu_p cvcpu)
{
	// External Interrupt has arrived.
	// According to AMD-V, external interrupt is held pending until GIF is set.
	// Switching to subverted host should have the interrupt handed in under hypervision.
	nvc_svm_switch_to_host_vcpu(gpr_state,vcpu);
	cvcpu->header.exit_context.intercept_code=cv_scheduler_exit;
}

// Expected Intercept Code: 0x61
void static noir_hvcode fastcall nvc_svm_nmi_cvexit_handler(noir_gpr_state_p gpr_state,noir_svm_vcpu_p vcpu,noir_svm_custom_vcpu_p cvcpu)
{
	// Non-Maskable Interrupt has arrived. According to AMD-V, NMI is held pending until GIF is set.
	// Switching to the subverted host should have the interrupt handed in under hypervision.
	nvc_svm_switch_to_host_vcpu(gpr_state,vcpu);
	cvcpu->header.exit_context.intercept_code=cv_scheduler_exit;
}

// Expected Intercept Code: 0x62
void static noir_hvcode fastcall nvc_svm_smi_cvexit_handler(noir_gpr_state_p gpr_state,noir_svm_vcpu_p vcpu,noir_svm_custom_vcpu_p cvcpu)
{
	// System-Management Interrupt has arrived. According to AMD-V, SMI is held pending until GIF is set.
	// Switching to the subverted host should have the interrupt handed in under hypervision.
	// No Internal SMIs are intercepted because all I/O instructions are intercepted.
	// If the firmware locks the SMM, SMIs would not be intercepted. However, SMI handler could recognize
	// if it is running in guest context and should have adjusted its environment to fit accordingly.
	nvc_svm_switch_to_host_vcpu(gpr_state,vcpu);
	cvcpu->header.exit_context.intercept_code=cv_scheduler_exit;
}

// Expected Intercept Code: 0x63
void static noir_hvcode fastcall nvc_svm_virtint_cvexit_handler(noir_gpr_state_p gpr_state,noir_svm_vcpu_p vcpu,noir_svm_custom_vcpu_p cvcpu)
{
	// This interception either indicates an interrupt window or the interrupt should be injected.
	if(cvcpu->special_state.prev_virq)
	{
		const u8 virq_vector=noir_svm_vmread8(cvcpu->vmcb.virt,avic_virq_vector);
		// Use the conventional method to inject virtual interrupt.
		noir_svm_inject_event(cvcpu->vmcb.virt,virq_vector,amd64_external_virtual_interrupt,false,true,0);
		cvcpu->special_state.prev_virq=false;	// Indicate there is no pending vIRQ anymore.
		// When we intercept interupt-window, ignore the TPR.
		noir_svm_vmcb_bts32(cvcpu->vmcb.virt,avic_control,nvc_svm_avic_control_ignore_vtpr);
		noir_svm_vmcb_btr32(cvcpu->vmcb.virt,vmcb_clean_bits,noir_svm_clean_avic);
		// Also, since the VIRQ is activated, remove the validity.
		cvcpu->header.injected_event.attributes.valid=false;
	}
	else
	{
		// When there is no pending vIRQ anymore, such interception is indicating an interrupt window.
		// Cancel the VIRQ.
		noir_svm_vmcb_btr32(cvcpu->vmcb.virt,avic_control,nvc_svm_avic_control_ignore_vtpr);
		noir_svm_vmcb_btr32(cvcpu->vmcb.virt,avic_control,nvc_svm_avic_control_virq);
		noir_svm_vmcb_btr32(cvcpu->vmcb.virt,vmcb_clean_bits,noir_svm_clean_avic);
		if(cvcpu->header.vcpu_options.intercept_interrupt_window)
		{
			// If the user hypervisor specified to intercept an interrupt window,
			// transfer the context to the subverted host.
			nvc_svm_switch_to_host_vcpu(gpr_state,vcpu);
			cvcpu->header.exit_context.intercept_code=cv_interrupt_window;
		}
	}
}

// Expected Intercept Code: 0x72
void static noir_hvcode fastcall nvc_svm_cpuid_cvexit_handler(noir_gpr_state_p gpr_state,noir_svm_vcpu_p vcpu,noir_svm_custom_vcpu_p cvcpu)
{
	noir_nsv_virtual_cpu_p nsvcpu=(noir_nsv_virtual_cpu_p)cvcpu->header.vmsa.virt;
	// Determine whether CPUID-Interception is subject to be delivered to subverted host.
	if(cvcpu->header.vcpu_options.intercept_cpuid)
	{
		if(cvcpu->vm->header.properties.nsv_guest)
		{
			// For NSV-Guest, interception of CPUID instruction triggers a Non-Automatic Exit.
			nvc_svm_nsv_load_nae_synthetic_msr_state(cvcpu);
			// Load specific information.
			nsvcpu->nsvs.vc_error_code=cv_cpuid_instruction;
			nsvcpu->nsvs.vc_info1=nsvcpu->nsvs.vc_info2=0;
		}
		else
		{
			// Switch to subverted host in order to handle the cpuid instruction.
			nvc_svm_switch_to_host_vcpu(gpr_state,vcpu);
			cvcpu->header.exit_context.intercept_code=cv_cpuid_instruction;
			cvcpu->header.exit_context.cpuid.leaf.a=(u32)cvcpu->header.gpr.rax;
			cvcpu->header.exit_context.cpuid.leaf.c=(u32)cvcpu->header.gpr.rcx;
			// Profiler: Classify the interception.
			cvcpu->header.statistics_internal.selector=&cvcpu->header.statistics.interceptions.cpuid;
		}
	}
	else
	{
		// NoirVisor will be handling CVM's CPUID Interception.
		u32 leaf=(u32)gpr_state->rax,subleaf=(u32)gpr_state->rcx;
		noir_cpuid_general_info info={0};
		bool qp_hit=false;
		// Use QuickPath to handle CPUID interception.
		// Search per-vCPU QuickPath list.
		for(u32 i=0;i<noir_cvm_cpuid_quickpath_limit_per_vcpu;i++)
		{
			noir_cvm_cpuid_quickpath_info_p qp=&cvcpu->header.cpuid_quickpath[i];
			if(qp->leaf==leaf)
			{
				if(qp->options.has_subleaf==false || qp->subleaf==subleaf)
				{
					info.eax=qp->eax;
					info.ebx=qp->ebx;
					info.ecx=qp->ecx;
					info.edx=qp->edx;
					qp_hit=true;
					break;
				}
			}
		}
		if(!qp_hit)
		{
			// Search per-VM QuickPath list.
			for(u32 i=0;i<noir_cvm_cpuid_quickpath_limit_per_vm;i++)
			{
				noir_cvm_cpuid_quickpath_info_p qp=&cvcpu->vm->header.cpuid_quickpath[i];
				if(qp->leaf==leaf)
				{
					if(qp->options.has_subleaf==false || qp->subleaf==subleaf)
					{
						info.eax=qp->eax;
						info.ebx=qp->ebx;
						info.ecx=qp->ecx;
						info.edx=qp->edx;
						qp_hit=true;
						break;
					}
				}
			}
		}
		*(u32*)&gpr_state->rax=info.eax;
		*(u32*)&gpr_state->rbx=info.ebx;
		*(u32*)&gpr_state->rcx=info.ecx;
		*(u32*)&gpr_state->rdx=info.edx;
		noir_svm_advance_rip(cvcpu->vmcb.virt);
		// Profiler: Classify the interception.
		cvcpu->header.statistics_internal.selector=&cvcpu->header.statistics.interceptions.emulation;
	}
	// DO NOT advance the rip unless cpuid is handled by NoirVisor.
}

// Expected Intercept Code: 0x73
void static noir_hvcode fastcall nvc_svm_rsm_cvexit_handler(noir_gpr_state_p gpr_state,noir_svm_vcpu_p vcpu,noir_svm_custom_vcpu_p cvcpu)
{
	// FIXME: Implement SMM.
	// Current implementation will inject #UD.
	nvc_svm_inject_cvm_exception(gpr_state,vcpu,cvcpu,amd64_invalid_opcode,false,0,0,0,null);
}

// Expected Intercept Code: 0x74
void static noir_hvcode fastcall nvc_svm_iret_cvexit_handler(noir_gpr_state_p gpr_state,noir_svm_vcpu_p vcpu,noir_svm_custom_vcpu_p cvcpu)
{
	// The iret instruction indicates NMI-window.
	if(cvcpu->special_state.prev_nmi)
	{
		cvcpu->special_state.prev_nmi=false;
		// Inject the pending NMI and keep the NMI-window interception on.
		noir_svm_vmwrite32(cvcpu->vmcb.virt,event_injection,cvcpu->header.injected_event.attributes.value);
		noir_svm_vmwrite32(cvcpu->vmcb.virt,event_error_code,cvcpu->header.injected_event.error_code);
	}
	else if(cvcpu->header.vcpu_options.intercept_nmi_window)
	{
		nvc_svm_switch_to_host_vcpu(gpr_state,vcpu);
		cvcpu->header.exit_context.intercept_code=cv_interrupt_window;
		cvcpu->header.exit_context.interrupt_window.nmi=true;				// This is an NMI-window.
		cvcpu->header.exit_context.interrupt_window.iret_passed=false;		// The iret instruction is not yet executed.
		cvcpu->header.exit_context.interrupt_window.reserved=0;
		// Cancel the interceptions of iret instruction.
		noir_svm_vmcb_btr32(cvcpu->vmcb.virt,intercept_instruction1,nvc_svm_intercept_vector1_iret);
		noir_svm_vmcb_btr32(cvcpu->vmcb.virt,vmcb_clean_bits,noir_svm_clean_interception);
	}
	// Profiler: Classify the interception.
	cvcpu->header.statistics_internal.selector=&cvcpu->header.statistics.interceptions.emulation;
}

// Expected Intercept Code: 0x76
void static noir_hvcode fastcall nvc_svm_invd_cvexit_handler(noir_gpr_state_p gpr_state,noir_svm_vcpu_p vcpu,noir_svm_custom_vcpu_p cvcpu)
{
	// The invd instruction would corrupt cache globally and it must thereby be intercepted.
	// Execute wbinvd to protect global cache.
	noir_wbinvd();
	noir_svm_advance_rip(cvcpu->vmcb.virt);
	// Profiler: Classify the interception.
	cvcpu->header.statistics_internal.selector=&cvcpu->header.statistics.interceptions.emulation;
}

// Expected Intercept Code: 0x78
void static noir_hvcode fastcall nvc_svm_hlt_cvexit_handler(noir_gpr_state_p gpr_state,noir_svm_vcpu_p vcpu,noir_svm_custom_vcpu_p cvcpu)
{
	// The hlt instruction halts the execution of processor.
	// In this regard, schedule the host to the processor.
	nvc_svm_switch_to_host_vcpu(gpr_state,vcpu);
	cvcpu->header.exit_context.intercept_code=cv_hlt_instruction;
	// Profiler: Classify the interception.
	cvcpu->header.statistics_internal.selector=&cvcpu->header.statistics.interceptions.halt;
	// Halting is an Automatic Exit. Advance the rip by NoirVisor.
	if(cvcpu->vm->header.properties.nsv_guest)noir_svm_advance_rip(cvcpu->vmcb.virt);
}

// Expected Intercept Code: 0x7A
void static noir_hvcode fastcall nvc_svm_invlpga_cvexit_handler(noir_gpr_state_p gpr_state,noir_svm_vcpu_p vcpu,noir_svm_custom_vcpu_p cvcpu)
{
	// NoirVisor currently does not support Nested Virtualization. Inject a #UD.
	nvc_svm_inject_cvm_exception(gpr_state,vcpu,cvcpu,amd64_invalid_opcode,false,0,0,0,null);
	// Profiler: Classify the interception.
	cvcpu->header.statistics_internal.selector=&cvcpu->header.statistics.interceptions.emulation;
}

// Expected Intercept Code: 0x7B
void static noir_hvcode fastcall nvc_svm_io_cvexit_handler(noir_gpr_state_p gpr_state,noir_svm_vcpu_p vcpu,noir_svm_custom_vcpu_p cvcpu)
{
	noir_nsv_virtual_cpu_p nsvcpu=(noir_nsv_virtual_cpu_p)cvcpu->header.vmsa.virt;
	nvc_svm_io_exit_info info;
	info.value=noir_svm_vmread32(cvcpu->vmcb.virt,exit_info1);
	cvcpu->header.exit_context.io.access.io_type=(u16)info.type;
	cvcpu->header.exit_context.io.access.string=(u16)info.string;
	cvcpu->header.exit_context.io.access.repeat=(u16)info.repeat;
	cvcpu->header.exit_context.io.access.operand_size=(u16)info.op_size;
	cvcpu->header.exit_context.io.access.address_width=(u16)info.addr_size<<1;
	cvcpu->header.exit_context.io.port=(u16)info.port;
	if(cvcpu->vm->header.properties.nsv_guest)
	{
		// I/O instructions are Non-Automatic Exits to NSV-Guests.
		nvc_svm_nsv_load_nae_synthetic_msr_state(cvcpu);
		nsvcpu->nsvs.vc_error_code=cv_io_instruction;
		nsvcpu->nsvs.vc_info1=(u64)cvcpu->header.exit_context.io.access.value;
		nsvcpu->nsvs.vc_info2=(u64)cvcpu->header.exit_context.io.port;
	}
	else
	{
		// Deliver the I/O interception to subverted host.
		nvc_svm_switch_to_host_vcpu(gpr_state,vcpu);
		cvcpu->header.exit_context.intercept_code=cv_io_instruction;
		cvcpu->header.exit_context.io.rax=cvcpu->header.gpr.rax;
		cvcpu->header.exit_context.io.rcx=cvcpu->header.gpr.rcx;
		cvcpu->header.exit_context.io.rsi=cvcpu->header.gpr.rsi;
		cvcpu->header.exit_context.io.rdi=cvcpu->header.gpr.rdi;
		cvcpu->header.exit_context.io.segment.selector=noir_svm_vmread16(cvcpu->vmcb.virt,(info.segment<<4)+guest_es_selector);
		cvcpu->header.exit_context.io.segment.attrib=noir_svm_vmread16(cvcpu->vmcb.virt,(info.segment<<4)+guest_es_attrib);
		cvcpu->header.exit_context.io.segment.limit=noir_svm_vmread32(cvcpu->vmcb.virt,(info.segment<<4)+guest_es_limit);
		cvcpu->header.exit_context.io.segment.base=noir_svm_vmread64(cvcpu->vmcb.virt,(info.segment<<4)+guest_es_base);
		// Profiler: Classify the interception.
		cvcpu->header.statistics_internal.selector=&cvcpu->header.statistics.interceptions.io;
	}
}

// Expected Intercept Code: 0x7C
bool static fastcall nvc_svm_rdmsr_cvexit_handler(noir_gpr_state_p gpr_state,noir_svm_custom_vcpu_p cvcpu)
{
	u32 index=(u32)gpr_state->rcx;
	large_integer val;
	bool advance=true;
	switch(index)
	{
		case amd64_tsc:
		{
			val.value=noir_svm_vmread64(cvcpu->vmcb.virt,tsc_offset)+noir_rdtsc();
			break;
		}
		case amd64_apic_base:
		{
			val.value=cvcpu->header.msrs.apic.value;
			break;
		}
		case amd64_mtrr_cap:
		{
			// Support no MTRRs, but support WC type.
			val.value=0x400;
			break;
		}
		case amd64_mtrr_def_type:
		{
			val.value=cvcpu->header.msrs.mtrr.def_type;
			break;
		}
		case amd64_sysenter_cs:
		{
			val.value=noir_svm_vmread64(cvcpu->vmcb.virt,guest_sysenter_cs);
			break;
		}
		case amd64_sysenter_esp:
		{
			val.value=noir_svm_vmread64(cvcpu->vmcb.virt,guest_sysenter_esp);
			break;
		}
		case amd64_sysenter_eip:
		{
			val.value=noir_svm_vmread64(cvcpu->vmcb.virt,guest_sysenter_eip);
			break;
		}
		case amd64_pat:
		{
			val.value=noir_svm_vmread64(cvcpu->vmcb.virt,guest_pat);
			break;
		}
		case amd64_efer:
		{
			// Perform SVME shadowing...
			val.value=noir_svm_vmread64(cvcpu->vmcb.virt,guest_efer);
			if(!cvcpu->shadowed_bits.svme)noir_btr(&val.low,amd64_efer_svme);
			break;
		}
		case amd64_star:
		{
			val.value=noir_svm_vmread64(cvcpu->vmcb.virt,guest_star);
			break;
		}
		case amd64_lstar:
		{
			val.value=noir_svm_vmread64(cvcpu->vmcb.virt,guest_lstar);
			break;
		}
		case amd64_cstar:
		{
			val.value=noir_svm_vmread64(cvcpu->vmcb.virt,guest_cstar);
			break;
		}
		case amd64_sfmask:
		{
			val.value=noir_svm_vmread64(cvcpu->vmcb.virt,guest_sfmask);
			break;
		}
		case amd64_fs_base:
		{
			val.value=noir_svm_vmread64(cvcpu->vmcb.virt,guest_fs_base);
			break;
		}
		case amd64_gs_base:
		{
			val.value=noir_svm_vmread64(cvcpu->vmcb.virt,guest_gs_base);
			break;
		}
		case amd64_kernel_gs_base:
		{
			val.value=noir_svm_vmread64(cvcpu->vmcb.virt,guest_kernel_gs_base);
			break;
		}
		default:
		{
			advance=false;
			break;
		}
	}
	if(advance)
	{
		*(u32*)&gpr_state->rax=val.low;
		*(u32*)&gpr_state->rdx=val.high;
	}
	return advance;
}

bool static fastcall nvc_svm_wrmsr_cvexit_handler(noir_gpr_state_p gpr_state,noir_svm_custom_vcpu_p cvcpu)
{
	u32 index=(u32)gpr_state->rcx;
	large_integer val;
	bool advance=true;
	val.low=(u32)gpr_state->rax;
	val.high=(u32)gpr_state->rdx;
	switch(index)
	{
		case amd64_tsc:
		{
			noir_svm_vmwrite64(cvcpu->vmcb.virt,tsc_offset,val.value-noir_rdtsc());
			// TSC-Offsetting is an interception field in VMCB.
			noir_svm_vmcb_btr32(cvcpu->vmcb.virt,vmcb_clean_bits,noir_svm_clean_interception);
			break;
		}
		case amd64_apic_base:
		{
			if(val.value & amd64_apic_rsvd_mask)
				advance=false;
			else if(page_base(val.value)==page_base(cvcpu->header.msrs.apic.value) && noir_bt64(&val.value,amd64_apic_bsc)==noir_bt64(&cvcpu->header.msrs.apic.value,amd64_apic_bsc))
				cvcpu->header.msrs.apic.value=val.value;
			else
				advance=false;
			break;
		}
		case amd64_sysenter_cs:
		{
			noir_svm_vmwrite64(cvcpu->vmcb.virt,guest_sysenter_cs,val.value);
			break;
		}
		case amd64_sysenter_esp:
		{
			noir_svm_vmwrite64(cvcpu->vmcb.virt,guest_sysenter_esp,val.value);
			break;
		}
		case amd64_sysenter_eip:
		{
			noir_svm_vmwrite64(cvcpu->vmcb.virt,guest_sysenter_eip,val.value);
			break;
		}
		case amd64_pat:
		{
			noir_svm_vmwrite64(cvcpu->vmcb.virt,guest_pat,val.value);
			// Guest PAT is cached as NPT fields in VMCB.
			noir_svm_vmcb_btr32(cvcpu->vmcb.virt,vmcb_clean_bits,noir_svm_clean_npt);
			break;
		}
		case amd64_efer:
		{
			// Perform SVME shadowing...
			cvcpu->shadowed_bits.svme=noir_bt(&val.low,amd64_efer_svme);
			noir_svm_vmwrite64(cvcpu->vmcb.virt,guest_efer,val.value|amd64_efer_svme_bit);
			// Guest EFER is cached as Control Register in VMCB.
			noir_svm_vmcb_btr32(cvcpu->vmcb.virt,vmcb_clean_bits,noir_svm_clean_control_reg);
			break;
		}
		case amd64_star:
		{
			noir_svm_vmwrite64(cvcpu->vmcb.virt,guest_star,val.value);
			break;
		}
		case amd64_lstar:
		{
			noir_svm_vmwrite64(cvcpu->vmcb.virt,guest_lstar,val.value);
			break;
		}
		case amd64_cstar:
		{
			noir_svm_vmwrite64(cvcpu->vmcb.virt,guest_cstar,val.value);
			break;
		}
		case amd64_sfmask:
		{
			noir_svm_vmwrite64(cvcpu->vmcb.virt,guest_sfmask,val.value);
			break;
		}
		case amd64_fs_base:
		{
			noir_svm_vmwrite64(cvcpu->vmcb.virt,guest_fs_base,val.value);
			break;
		}
		case amd64_gs_base:
		{
			noir_svm_vmwrite64(cvcpu->vmcb.virt,guest_gs_base,val.value);
			break;
		}
		case amd64_kernel_gs_base:
		{
			noir_svm_vmwrite64(cvcpu->vmcb.virt,guest_kernel_gs_base,val.value);
			break;
		}
		default:
		{
			advance=false;
			break;
		}
	}
	return advance;
}

void static noir_hvcode fastcall nvc_svm_msr_cvexit_handler(noir_gpr_state_p gpr_state,noir_svm_vcpu_p vcpu,noir_svm_custom_vcpu_p cvcpu)
{
	// Determine whether MSR-Interception is subject to be delivered to subverted host.
	bool op_write=noir_svm_vmread8(cvcpu->vmcb.virt,exit_info1);
	const u32 index=(const u32)gpr_state->rcx;
	bool emulate=false;
	if(index>=0x40000000 && index<0x80000000)
	{
		// These are MSRs reserved by NoirVisor. User hypervisors cannot intercept them.
		bool no_exit=op_write?nvc_svm_wrmsr_nsvexit_handler(gpr_state,vcpu,cvcpu):nvc_svm_rdmsr_nsvexit_handler(gpr_state,vcpu,cvcpu);
		if(!no_exit)nvc_svm_switch_to_host_vcpu(gpr_state,vcpu);
	}
	else if(cvcpu->header.vcpu_options.intercept_msr)
	{
		bool intercept=true;
		if(cvcpu->header.msr_interceptions.valid)
		{
			// User Hypervisor wants to intercept only a portion of MSRs.
			switch(index)
			{
				case amd64_apic_base:
				{
					intercept=cvcpu->header.msr_interceptions.intercept_apic;
					break;
				}
				case amd64_mtrr_cap:
				case amd64_mtrr_phys_base0:
				case amd64_mtrr_phys_mask0:
				case amd64_mtrr_phys_base1:
				case amd64_mtrr_phys_mask1:
				case amd64_mtrr_phys_base2:
				case amd64_mtrr_phys_mask2:
				case amd64_mtrr_phys_base3:
				case amd64_mtrr_phys_mask3:
				case amd64_mtrr_phys_base4:
				case amd64_mtrr_phys_mask4:
				case amd64_mtrr_phys_base5:
				case amd64_mtrr_phys_mask5:
				case amd64_mtrr_phys_base6:
				case amd64_mtrr_phys_mask6:
				case amd64_mtrr_phys_base7:
				case amd64_mtrr_phys_mask7:
				case amd64_mtrr_fix64k_00000:
				case amd64_mtrr_fix16k_80000:
				case amd64_mtrr_fix16k_a0000:
				case amd64_mtrr_fix4k_c0000:
				case amd64_mtrr_fix4k_c8000:
				case amd64_mtrr_fix4k_d0000:
				case amd64_mtrr_fix4k_d8000:
				case amd64_mtrr_fix4k_e0000:
				case amd64_mtrr_fix4k_e8000:
				case amd64_mtrr_fix4k_f0000:
				case amd64_mtrr_fix4k_f8000:
				case amd64_pat:
				case amd64_mtrr_def_type:
				{
					intercept=cvcpu->header.msr_interceptions.intercept_mtrr;
					break;
				}
				case amd64_sysenter_cs:
				case amd64_sysenter_esp:
				case amd64_sysenter_eip:
				{
					intercept=cvcpu->header.msr_interceptions.intercept_sysenter;
					break;
				}
				case amd64_u_cet:
				case amd64_s_cet:
				case amd64_pl0_ssp:
				case amd64_pl1_ssp:
				case amd64_pl2_ssp:
				case amd64_pl3_ssp:
				case amd64_isst_addr:
				{
					intercept=cvcpu->header.msr_interceptions.intercept_cet;
					break;
				}
				case amd64_star:
				case amd64_lstar:
				case amd64_cstar:
				case amd64_sfmask:
				case amd64_fs_base:
				case amd64_gs_base:
				case amd64_kernel_gs_base:
				{
					intercept=cvcpu->header.msr_interceptions.intercept_syscall;
					break;
				}
				case amd64_smi_trig_io_cycle:
				case amd64_pstate_current_limit:
				case amd64_pstate_control:
				case amd64_pstate_status:
				case amd64_smbase:
				case amd64_smm_addr:
				case amd64_smm_mask:
				case amd64_local_smi_status:
				{
					intercept=cvcpu->header.msr_interceptions.intercept_smm;
					break;
				}
				default:
				{
					// There are too many MSRs for x2APIC. Use a range to determine them.
					if(index>=amd64_x2apic_msr_start && index<=amd64_x2apic_msr_end)
						intercept=cvcpu->header.msr_interceptions.intercept_apic;
					else
						intercept=false;
					break;
				}
			}
		}
		if(intercept)
		{
			// Switch to subverted host in order to handle the MSR instruction.
			nvc_svm_switch_to_host_vcpu(gpr_state,vcpu);
			cvcpu->header.exit_context.intercept_code=op_write?cv_wrmsr_instruction:cv_rdmsr_instruction;
			cvcpu->header.exit_context.msr.eax=(u32)cvcpu->header.gpr.rax;
			cvcpu->header.exit_context.msr.edx=(u32)cvcpu->header.gpr.rdx;
			cvcpu->header.exit_context.msr.ecx=(u32)cvcpu->header.gpr.rcx;
			// Profiler: Classify the interception.
			cvcpu->header.statistics_internal.selector=&cvcpu->header.statistics.interceptions.msr;
		}
		// If interception is not going to be passed to User Hypervisor, let NoirVisor emulate the MSR access.
		emulate=!intercept;
	}
	if(emulate)
	{
		// NoirVisor will be handling CVM's MSR Interception.
		bool advance=op_write?nvc_svm_wrmsr_cvexit_handler(gpr_state,cvcpu):nvc_svm_rdmsr_cvexit_handler(gpr_state,cvcpu);
		// Profiler: Classify the interception as hypervisor's emulation.
		cvcpu->header.statistics_internal.selector=&cvcpu->header.statistics.interceptions.emulation;
		if(advance)
			noir_svm_advance_rip(cvcpu);
		else
			nvc_svm_inject_cvm_exception(gpr_state,vcpu,cvcpu,amd64_invalid_opcode,false,0,0,0,null);
	}
}

// Expected Intercept Code: 0x7F
void static noir_hvcode fastcall nvc_svm_shutdown_cvexit_handler(noir_gpr_state_p gpr_state,noir_svm_vcpu_p vcpu,noir_svm_custom_vcpu_p cvcpu)
{
	// The shutdown condition occured. Deliver to the subverted host.
	nvc_svm_switch_to_host_vcpu(gpr_state,vcpu);
	cvcpu->header.exit_context.intercept_code=cv_shutdown_condition;
	// Profiler: Classify the interception.
	cvcpu->header.statistics_internal.selector=&cvcpu->header.statistics.interceptions.emulation;
}

// Expected Intercept Code: 0x80
void static noir_hvcode fastcall nvc_svm_vmrun_cvexit_handler(noir_gpr_state_p gpr_state,noir_svm_vcpu_p vcpu,noir_svm_custom_vcpu_p cvcpu)
{
	// NoirVisor currently does not support Nested Virtualization. Inject a #UD.
	nvc_svm_inject_cvm_exception(gpr_state,vcpu,cvcpu,amd64_invalid_opcode,false,0,0,0,null);
	// Profiler: Classify the interception.
	cvcpu->header.statistics_internal.selector=&cvcpu->header.statistics.interceptions.emulation;
}

// Expected Intercept Code: 0x81
void static noir_hvcode fastcall nvc_svm_vmmcall_cvexit_handler(noir_gpr_state_p gpr_state,noir_svm_vcpu_p vcpu,noir_svm_custom_vcpu_p cvcpu)
{
	// u64 inslen=noir_svm_vmread64(cvcpu->vmcb.virt,next_rip)-noir_svm_vmread64(cvcpu->vmcb.virt,guest_rip);
	// The Guest invoked a hypercall. Deliver to the subverted host.
	nvc_svm_switch_to_host_vcpu(gpr_state,vcpu);
	cvcpu->header.exit_context.intercept_code=cv_hypercall;
	// Profiler: Classify the interception.
	cvcpu->header.statistics_internal.selector=&cvcpu->header.statistics.interceptions.hypercall;
	// For NSV guests, this is an explicit hypercall to the user hypervisor.
	// NoirVisor will advance the rip register in lieu of the user hypervisor.
	if(cvcpu->vm->header.properties.nsv_guest)noir_svm_advance_rip(cvcpu->vmcb.virt);
}

// Expected Intercept Code: 0x82
void static noir_hvcode fastcall nvc_svm_vmload_cvexit_handler(noir_gpr_state_p gpr_state,noir_svm_vcpu_p vcpu,noir_svm_custom_vcpu_p cvcpu)
{
	// NoirVisor currently does not support Nested Virtualization. Inject a #UD.
	nvc_svm_inject_cvm_exception(gpr_state,vcpu,cvcpu,amd64_invalid_opcode,false,0,0,0,null);
	// Profiler: Classify the interception.
	cvcpu->header.statistics_internal.selector=&cvcpu->header.statistics.interceptions.emulation;
}

// Expected Intercept Code: 0x83
void static noir_hvcode fastcall nvc_svm_vmsave_cvexit_handler(noir_gpr_state_p gpr_state,noir_svm_vcpu_p vcpu,noir_svm_custom_vcpu_p cvcpu)
{
	// NoirVisor currently does not support Nested Virtualization. Inject a #UD.
	nvc_svm_inject_cvm_exception(gpr_state,vcpu,cvcpu,amd64_invalid_opcode,false,0,0,0,null);
	// Profiler: Classify the interception.
	cvcpu->header.statistics_internal.selector=&cvcpu->header.statistics.interceptions.emulation;
}

// Expected Intercept Code: 0x84
void static noir_hvcode fastcall nvc_svm_stgi_cvexit_handler(noir_gpr_state_p gpr_state,noir_svm_vcpu_p vcpu,noir_svm_custom_vcpu_p cvcpu)
{
	// NoirVisor currently does not support Nested Virtualization. Inject a #UD.
	nvc_svm_inject_cvm_exception(gpr_state,vcpu,cvcpu,amd64_invalid_opcode,false,0,0,0,null);
	// Profiler: Classify the interception.
	cvcpu->header.statistics_internal.selector=&cvcpu->header.statistics.interceptions.emulation;
}

// Expected Intercept Code: 0x85
void static noir_hvcode fastcall nvc_svm_clgi_cvexit_handler(noir_gpr_state_p gpr_state,noir_svm_vcpu_p vcpu,noir_svm_custom_vcpu_p cvcpu)
{
	// NoirVisor currently does not support Nested Virtualization. Inject a #UD.
	nvc_svm_inject_cvm_exception(gpr_state,vcpu,cvcpu,amd64_invalid_opcode,false,0,0,0,null);
	// Profiler: Classify the interception.
	cvcpu->header.statistics_internal.selector=&cvcpu->header.statistics.interceptions.emulation;
}

// Expected Intercept Code: 0x86
void static noir_hvcode fastcall nvc_svm_skinit_cvexit_handler(noir_gpr_state_p gpr_state,noir_svm_vcpu_p vcpu,noir_svm_custom_vcpu_p cvcpu)
{
	// NoirVisor currently does not support Nested Virtualization. Inject a #UD.
	nvc_svm_inject_cvm_exception(gpr_state,vcpu,cvcpu,amd64_invalid_opcode,false,0,0,0,null);
	// Profiler: Classify the interception.
	cvcpu->header.statistics_internal.selector=&cvcpu->header.statistics.interceptions.emulation;
}

// Expected Intercept Code: 0x400
void static noir_hvcode fastcall nvc_svm_nested_pf_cvexit_handler(noir_gpr_state_p gpr_state,noir_svm_vcpu_p vcpu,noir_svm_custom_vcpu_p cvcpu)
{
	amd64_npt_fault_code fault;
	u64 gpa=noir_svm_vmread64(cvcpu->vmcb.virt,exit_info2);
	// #NPF occured, tell the subverted host there is a memory access fault.
	nvc_svm_switch_to_host_vcpu(gpr_state,vcpu);
	fault.value=noir_svm_vmread64(cvcpu->vmcb.virt,exit_info1);
	cvcpu->header.exit_context.memory_access.gpa=gpa;
	cvcpu->header.exit_context.memory_access.access.present=(u8)fault.present;
	cvcpu->header.exit_context.memory_access.access.write=(u8)fault.write;
	cvcpu->header.exit_context.memory_access.access.execute=(u8)fault.execute;
	cvcpu->header.exit_context.memory_access.access.user=(u8)fault.user;
	cvcpu->header.exit_context.memory_access.flags.value=0;
	if(fault.npf_addr)
	{
		cvcpu->header.exit_context.intercept_code=cv_memory_access;
		cvcpu->header.exit_context.memory_access.access.fetched_bytes=noir_svm_vmread8(cvcpu->vmcb.virt,number_of_bytes_fetched);
		noir_movsb(cvcpu->header.exit_context.memory_access.instruction_bytes,(u8*)((ulong_ptr)cvcpu->vmcb.virt+guest_instruction_bytes),15);
		if(cvcpu->header.exit_context.intercept_code==cv_memory_access && cvcpu->header.vcpu_options.decode_memory_access_instruction)
		{
			// To decode instruction, vCPU state is required.
			nvc_svm_load_basic_exit_context(cvcpu);
			nvc_emu_decode_memory_access(&cvcpu->header);
		}
	}
	else if(fault.npf_table)
	{
		// For such #NPF, there are misconfigurations!
		cvcpu->header.exit_context.intercept_code=cv_scheduler_npt_misconfig;
	}
	// Profiler: Classify the interception.
	cvcpu->header.statistics_internal.selector=&cvcpu->header.statistics.interceptions.npf;
}

// Expected Intercept Code: 0x401
void static noir_hvcode fastcall nvc_svm_incomplete_ipi_cvexit_handler(noir_gpr_state_p gpr_state,noir_svm_vcpu_p vcpu,noir_svm_custom_vcpu_p cvcpu)
{
	;
}

// Expected Intercept Code: 0x402
void static noir_hvcode fastcall nvc_svm_unaccelerated_avic_cvexit_handler(noir_gpr_state_p gpr_state,noir_svm_vcpu_p vcpu,noir_svm_custom_vcpu_p cvcpu)
{
	;
}