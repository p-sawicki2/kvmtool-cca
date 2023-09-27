#include "kvm/kvm-cpu.h"
#include "kvm/kvm.h"
#include "kvm/virtio.h"

#include "measurement/rim-measure.h"

#include <asm/ptrace.h>

#define COMPAT_PSR_F_BIT	0x00000040
#define COMPAT_PSR_I_BIT	0x00000080
#define COMPAT_PSR_E_BIT	0x00000200
#define COMPAT_PSR_MODE_SVC	0x00000013

#define SCTLR_EL1_E0E_MASK	(1 << 24)
#define SCTLR_EL1_EE_MASK	(1 << 25)

#ifdef RIM_MEASURE
 /*
  * PSR bits
  */
 #define PSR_MODE_EL0t   0x00000000
 #define PSR_MODE_EL1t   0x00000004
 #define PSR_MODE_EL1h   0x00000005
 #define PSR_MODE_EL2t   0x00000008
 #define PSR_MODE_EL2h   0x00000009
 #define PSR_MODE_EL3t   0x0000000c
 #define PSR_MODE_EL3h   0x0000000d
 #define PSR_MODE_MASK   0x0000000f

 /* AArch32 CPSR bits */
 #define PSR_MODE32_BIT          0x00000010

 /* AArch64 SPSR bits */
 #define PSR_F_BIT       0x00000040
 #define PSR_I_BIT       0x00000080
 #define PSR_A_BIT       0x00000100
 #define PSR_D_BIT       0x00000200
 #define PSR_SSBS_BIT    0x00001000
 #define PSR_PAN_BIT     0x00400000
 #define PSR_UAO_BIT     0x00800000
 #define PSR_DIT_BIT     0x01000000
 #define PSR_V_BIT       0x10000000
 #define PSR_C_BIT       0x20000000
 #define PSR_Z_BIT       0x40000000
 #define PSR_N_BIT       0x80000000
#endif

static __u64 __core_reg_id(__u64 offset)
{
	__u64 id = KVM_REG_ARM64 | KVM_REG_ARM_CORE | offset;

	if (offset < KVM_REG_ARM_CORE_REG(fp_regs))
		id |= KVM_REG_SIZE_U64;
	else if (offset < KVM_REG_ARM_CORE_REG(fp_regs.fpsr))
		id |= KVM_REG_SIZE_U128;
	else
		id |= KVM_REG_SIZE_U32;

	return id;
}

#define ARM64_CORE_REG(x) __core_reg_id(KVM_REG_ARM_CORE_REG(x))

unsigned long kvm_cpu__get_vcpu_mpidr(struct kvm_cpu *vcpu)
{
#ifndef RIM_MEASURE
	struct kvm_one_reg reg;
	u64 mpidr;

	reg.id = ARM64_SYS_REG(ARM_CPU_ID, ARM_CPU_ID_MPIDR);
	reg.addr = (u64)&mpidr;
	if (ioctl(vcpu->vcpu_fd, KVM_GET_ONE_REG, &reg) < 0)
		die("KVM_GET_ONE_REG failed (get_mpidr vcpu%ld", vcpu->cpu_id);

	return mpidr;
#else
	return vcpu->cpu_id;
#endif
}

static void reset_vcpu_aarch32(struct kvm_cpu *vcpu)
{
	struct kvm *kvm = vcpu->kvm;
	struct kvm_one_reg reg;
	u64 data;

	reg.addr = (u64)&data;

	/* pstate = all interrupts masked */
	data	= COMPAT_PSR_I_BIT | COMPAT_PSR_F_BIT | COMPAT_PSR_MODE_SVC;
	reg.id	= ARM64_CORE_REG(regs.pstate);
	if (ioctl(vcpu->vcpu_fd, KVM_SET_ONE_REG, &reg) < 0)
		die_perror("KVM_SET_ONE_REG failed (spsr[EL1])");

	/* Secondary cores are stopped awaiting PSCI wakeup */
	if (vcpu->cpu_id != 0)
		return;

	/* r0 = 0 */
	data	= 0;
	reg.id	= ARM64_CORE_REG(regs.regs[0]);
	if (ioctl(vcpu->vcpu_fd, KVM_SET_ONE_REG, &reg) < 0)
		die_perror("KVM_SET_ONE_REG failed (r0)");

	/* r1 = machine type (-1) */
	data	= -1;
	reg.id	= ARM64_CORE_REG(regs.regs[1]);
	if (ioctl(vcpu->vcpu_fd, KVM_SET_ONE_REG, &reg) < 0)
		die_perror("KVM_SET_ONE_REG failed (r1)");

	/* r2 = physical address of the device tree blob */
	data	= kvm->arch.dtb_guest_start;
	reg.id	= ARM64_CORE_REG(regs.regs[2]);
	if (ioctl(vcpu->vcpu_fd, KVM_SET_ONE_REG, &reg) < 0)
		die_perror("KVM_SET_ONE_REG failed (r2)");

	/* pc = start of kernel image */
	data	= kvm->arch.kern_guest_start;
	reg.id	= ARM64_CORE_REG(regs.pc);
	if (ioctl(vcpu->vcpu_fd, KVM_SET_ONE_REG, &reg) < 0)
		die_perror("KVM_SET_ONE_REG failed (pc)");
}

static void reset_vcpu_aarch64(struct kvm_cpu *vcpu)
{
	struct kvm *kvm = vcpu->kvm;
	struct kvm_one_reg reg;
	u64 data;

#ifndef RIM_MEASURE
	reg.addr = (u64)&data;

	if (!kvm->cfg.arch.is_realm) {
		/* pstate = all interrupts masked */
		data	= PSR_D_BIT | PSR_A_BIT | PSR_I_BIT | PSR_F_BIT | PSR_MODE_EL1h;
		reg.id	= ARM64_CORE_REG(regs.pstate);
		if (ioctl(vcpu->vcpu_fd, KVM_SET_ONE_REG, &reg) < 0)
			die_perror("KVM_SET_ONE_REG failed (PSTATE)");
	}

	/* x1...x3 = 0 */
	data	= 0;
	reg.id	= ARM64_CORE_REG(regs.regs[1]);
	if (ioctl(vcpu->vcpu_fd, KVM_SET_ONE_REG, &reg) < 0)
		die_perror("KVM_SET_ONE_REG failed (x1)");

	reg.id	= ARM64_CORE_REG(regs.regs[2]);
	if (ioctl(vcpu->vcpu_fd, KVM_SET_ONE_REG, &reg) < 0)
		die_perror("KVM_SET_ONE_REG failed (x2)");

	reg.id	= ARM64_CORE_REG(regs.regs[3]);
	if (ioctl(vcpu->vcpu_fd, KVM_SET_ONE_REG, &reg) < 0)
		die_perror("KVM_SET_ONE_REG failed (x3)");
#endif
	/* Secondary cores are stopped awaiting PSCI wakeup */
	if (vcpu->cpu_id == 0) {
#ifndef RIM_MEASURE
		/* x0 = physical address of the device tree blob */
		data	= kvm->arch.dtb_guest_start;
		reg.id	= ARM64_CORE_REG(regs.regs[0]);
		if (ioctl(vcpu->vcpu_fd, KVM_SET_ONE_REG, &reg) < 0)
			die_perror("KVM_SET_ONE_REG failed (x0)");

		/* pc = start of kernel image */
		data	= kvm->arch.kern_guest_start;
		reg.id	= ARM64_CORE_REG(regs.pc);
		if (ioctl(vcpu->vcpu_fd, KVM_SET_ONE_REG, &reg) < 0)
			die_perror("KVM_SET_ONE_REG failed (pc)");
#else
		measurer_reset_vcpu_aarch64(kvm->arch.kern_guest_start, 0x1, kvm->arch.dtb_guest_start);
#endif
	}
	else {
		measurer_reset_vcpu_aarch64(0, 0, 0);
	}

#ifndef RIM_MEASURE
	if (kvm->cfg.arch.is_realm) {
		int feature = KVM_ARM_VCPU_REC;

		if (ioctl(vcpu->vcpu_fd, KVM_ARM_VCPU_FINALIZE, &feature) < 0)
			die_perror("KVM_ARM_VCPU_FINALIZE(KVM_ARM_VCPU_REC)");
	}
#endif
}

void kvm_cpu__select_features(struct kvm *kvm, struct kvm_vcpu_init *init)
{
	if (kvm->cfg.arch.aarch32_guest) {
		if (!kvm__supports_extension(kvm, KVM_CAP_ARM_EL1_32BIT))
			die("32bit guests are not supported\n");
		init->features[0] |= 1UL << KVM_ARM_VCPU_EL1_32BIT;
	}

	if (kvm->cfg.arch.has_pmuv3) {
#ifndef RIM_MEASURE
		if (!kvm__supports_extension(kvm, KVM_CAP_ARM_PMU_V3))
			die("PMUv3 is not supported");
#endif
		init->features[0] |= 1UL << KVM_ARM_VCPU_PMU_V3;
	}

	/* Enable pointer authentication if available */
	if (kvm__supports_extension(kvm, KVM_CAP_ARM_PTRAUTH_ADDRESS) &&
	    kvm__supports_extension(kvm, KVM_CAP_ARM_PTRAUTH_GENERIC)) {
		init->features[0] |= 1UL << KVM_ARM_VCPU_PTRAUTH_ADDRESS;
		init->features[0] |= 1UL << KVM_ARM_VCPU_PTRAUTH_GENERIC;
	}

	/* If SVE is not disabled explicitly, enable if available */
	if (!kvm->cfg.arch.disable_sve &&
	    kvm__supports_vm_extension(kvm, KVM_CAP_ARM_SVE))
		init->features[0] |= 1UL << KVM_ARM_VCPU_SVE;
}

int kvm_cpu__configure_features(struct kvm_cpu *vcpu)
{
#ifndef RIM_MEASURE
	struct kvm *kvm = vcpu->kvm;

	if (!kvm->cfg.arch.disable_sve &&
	    kvm__supports_vm_extension(kvm, KVM_CAP_ARM_SVE)) {
		int feature = KVM_ARM_VCPU_SVE;

		if (ioctl(vcpu->vcpu_fd, KVM_ARM_VCPU_FINALIZE, &feature)) {
			pr_err("KVM_ARM_VCPU_FINALIZE: %s", strerror(errno));
			return -1;
		}
	}
#endif
	return 0;
}

void kvm_cpu__reset_vcpu(struct kvm_cpu *vcpu)
{
	struct kvm *kvm = vcpu->kvm;
	cpu_set_t *affinity;
	int ret;

	/* VCPU reset is done before activating the realm. */
	if (kvm->arch.realm_is_active)
		return;

	affinity = kvm->arch.vcpu_affinity_cpuset;
	if (affinity) {
		ret = sched_setaffinity(0, sizeof(cpu_set_t), affinity);
		if (ret == -1)
			die_perror("sched_setaffinity");
	}

	if (kvm->cfg.arch.aarch32_guest)
		return reset_vcpu_aarch32(vcpu);
	else
		return reset_vcpu_aarch64(vcpu);
}

int kvm_cpu__get_endianness(struct kvm_cpu *vcpu)
{
	struct kvm_one_reg reg;
	u64 psr;
	u64 sctlr;

	/*
	 * Quoting the definition given by Peter Maydell:
	 *
	 * "Endianness of the CPU which does the virtio reset at the
	 * point when it does that reset"
	 *
	 * We first check for an AArch32 guest: its endianness can
	 * change when using SETEND, which affects the CPSR.E bit.
	 *
	 * If we're AArch64, use SCTLR_EL1.E0E if access comes from
	 * EL0, and SCTLR_EL1.EE if access comes from EL1.
	 */
	reg.id = ARM64_CORE_REG(regs.pstate);
	reg.addr = (u64)&psr;
	if (ioctl(vcpu->vcpu_fd, KVM_GET_ONE_REG, &reg) < 0)
		die("KVM_GET_ONE_REG failed (spsr[EL1])");

	if (psr & PSR_MODE32_BIT)
		return (psr & COMPAT_PSR_E_BIT) ? VIRTIO_ENDIAN_BE : VIRTIO_ENDIAN_LE;

	reg.id = ARM64_SYS_REG(ARM_CPU_CTRL, ARM_CPU_CTRL_SCTLR_EL1);
	reg.addr = (u64)&sctlr;
	if (ioctl(vcpu->vcpu_fd, KVM_GET_ONE_REG, &reg) < 0)
		die("KVM_GET_ONE_REG failed (SCTLR_EL1)");

	if ((psr & PSR_MODE_MASK) == PSR_MODE_EL0t)
		sctlr &= SCTLR_EL1_E0E_MASK;
	else
		sctlr &= SCTLR_EL1_EE_MASK;
	return sctlr ? VIRTIO_ENDIAN_BE : VIRTIO_ENDIAN_LE;
}

void kvm_cpu__show_code(struct kvm_cpu *vcpu)
{
	struct kvm_one_reg reg;
	unsigned long data;
	int debug_fd = kvm_cpu__get_debug_fd();

	reg.addr = (u64)&data;

	if (vcpu->kvm->cfg.arch.is_realm)
		return;

	dprintf(debug_fd, "\n*pc:\n");
	reg.id = ARM64_CORE_REG(regs.pc);
	if (ioctl(vcpu->vcpu_fd, KVM_GET_ONE_REG, &reg) < 0)
		die("KVM_GET_ONE_REG failed (show_code @ PC)");

	kvm__dump_mem(vcpu->kvm, data, 32, debug_fd);

	dprintf(debug_fd, "\n*lr:\n");
	reg.id = ARM64_CORE_REG(regs.regs[30]);
	if (ioctl(vcpu->vcpu_fd, KVM_GET_ONE_REG, &reg) < 0)
		die("KVM_GET_ONE_REG failed (show_code @ LR)");

	kvm__dump_mem(vcpu->kvm, data, 32, debug_fd);
}

void kvm_cpu__show_registers(struct kvm_cpu *vcpu)
{
	struct kvm_one_reg reg;
	unsigned long data;
	int debug_fd = kvm_cpu__get_debug_fd();

	reg.addr = (u64)&data;
	dprintf(debug_fd, "\n Registers:\n");

	if (vcpu->kvm->cfg.arch.is_realm) {
		dprintf(debug_fd, " UNACCESSIBLE\n");
		return;
	}

	reg.id		= ARM64_CORE_REG(regs.pc);
	if (ioctl(vcpu->vcpu_fd, KVM_GET_ONE_REG, &reg) < 0)
		die("KVM_GET_ONE_REG failed (pc)");
	dprintf(debug_fd, " PC:    0x%lx\n", data);

	reg.id		= ARM64_CORE_REG(regs.pstate);
	if (ioctl(vcpu->vcpu_fd, KVM_GET_ONE_REG, &reg) < 0)
		die("KVM_GET_ONE_REG failed (pstate)");
	dprintf(debug_fd, " PSTATE:    0x%lx\n", data);

	reg.id		= ARM64_CORE_REG(sp_el1);
	if (ioctl(vcpu->vcpu_fd, KVM_GET_ONE_REG, &reg) < 0)
		die("KVM_GET_ONE_REG failed (sp_el1)");
	dprintf(debug_fd, " SP_EL1:    0x%lx\n", data);

	reg.id		= ARM64_CORE_REG(regs.regs[30]);
	if (ioctl(vcpu->vcpu_fd, KVM_GET_ONE_REG, &reg) < 0)
		die("KVM_GET_ONE_REG failed (lr)");
	dprintf(debug_fd, " LR:    0x%lx\n", data);
}
