/*
 * This file is subject to the terms and conditions of the GNU General Public
 * License.  See the file "COPYING" in the main directory of this archive
 * for more details.
 *
 * Copyright (C) 2008 Ralf Baechle (ralf@linux-mips.org)
 * Copyright (C) 2012 MIPS Technologies, Inc.  All rights reserved.
 */

#define pr_fmt(fmt) "irq-mips-gic: " fmt

#include <linux/bitfield.h>
#include <linux/bitmap.h>
#include <linux/clocksource.h>
#include <linux/cpuhotplug.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/irqchip.h>
#include <linux/irqdomain.h>
#include <linux/of_address.h>
#include <linux/percpu.h>
#include <linux/sched.h>
#include <linux/smp.h>

#include <asm/mips-cps.h>
#include <asm/setup.h>
#include <asm/traps.h>

#include <dt-bindings/interrupt-controller/mips-gic.h>

#define GIC_MAX_INTRS		256
#define GIC_MAX_LONGS		BITS_TO_LONGS(GIC_MAX_INTRS)

/* Add 2 to convert GIC CPU pin to core interrupt */
#define GIC_CPU_PIN_OFFSET	2

/* Mapped interrupt to pin X, then GIC will generate the vector (X+1). */
#define GIC_PIN_TO_VEC_OFFSET	1

/* Convert between local/shared IRQ number and GIC HW IRQ number. */
#define GIC_LOCAL_HWIRQ_BASE	0
#define GIC_LOCAL_TO_HWIRQ(x)	(GIC_LOCAL_HWIRQ_BASE + (x))
#define GIC_HWIRQ_TO_LOCAL(x)	((x) - GIC_LOCAL_HWIRQ_BASE)
#define GIC_SHARED_HWIRQ_BASE	GIC_NUM_LOCAL_INTRS
#define GIC_SHARED_TO_HWIRQ(x)	(GIC_SHARED_HWIRQ_BASE + (x))
#define GIC_HWIRQ_TO_SHARED(x)	((x) - GIC_SHARED_HWIRQ_BASE)

void __iomem *mips_gic_base;

static DEFINE_PER_CPU_READ_MOSTLY(unsigned long[GIC_MAX_LONGS], pcpu_masks);

static DEFINE_RAW_SPINLOCK(gic_lock);
static struct irq_domain *gic_irq_domain;
static int gic_shared_intrs;
static unsigned int gic_cpu_pin;
static struct irq_chip gic_level_irq_controller, gic_edge_irq_controller;

#ifdef CONFIG_GENERIC_IRQ_IPI
static DECLARE_BITMAP(ipi_resrv, GIC_MAX_INTRS);
static DECLARE_BITMAP(ipi_available, GIC_MAX_INTRS);
#endif /* CONFIG_GENERIC_IRQ_IPI */

static struct gic_all_vpes_chip_data {
	u32	map;
	bool	mask;
} gic_all_vpes_chip_data[GIC_NUM_LOCAL_INTRS];

static int __gic_with_next_online_cpu(int prev)
{
	unsigned int cpu;

	/* Discover the next online CPU */
	cpu = cpumask_next(prev, cpu_online_mask);

	/* If there isn't one, we're done */
	if (cpu >= nr_cpu_ids)
		return cpu;

	/*
	 * Move the access lock to the next CPU's GIC local register block.
	 *
	 * Set GIC_VL_OTHER. Since the caller holds gic_lock nothing can
	 * clobber the written value.
	 */
	write_gic_vl_other(mips_cm_vp_id(cpu));

	return cpu;
}

static inline void gic_unlock_cluster(void)
{
	if (mips_cps_multicluster_cpus())
		mips_cm_unlock_other();
}

/**
 * for_each_online_cpu_gic() - Iterate over online CPUs, access local registers
 * @cpu: An integer variable to hold the current CPU number
 * @gic_lock: A pointer to raw spin lock used as a guard
 *
 * Iterate over online CPUs & configure the other/redirect register region to
 * access each CPUs GIC local register block, which can be accessed from the
 * loop body using read_gic_vo_*() or write_gic_vo_*() accessor functions or
 * their derivatives.
 */
#define for_each_online_cpu_gic(cpu, gic_lock)		\
	guard(raw_spinlock_irqsave)(gic_lock);		\
	for ((cpu) = __gic_with_next_online_cpu(-1);	\
	     (cpu) < nr_cpu_ids;			\
	     gic_unlock_cluster(),			\
	     (cpu) = __gic_with_next_online_cpu(cpu))

/**
 * gic_irq_lock_cluster() - Lock redirect block access to IRQ's cluster
 * @d: struct irq_data corresponding to the interrupt we're interested in
 *
 * Locks redirect register block access to the global register block of the GIC
 * within the remote cluster that the IRQ corresponding to @d is affine to,
 * returning true when this redirect block setup & locking has been performed.
 *
 * If @d is affine to the local cluster then no locking is performed and this
 * function will return false, indicating to the caller that it should access
 * the local clusters registers without the overhead of indirection through the
 * redirect block.
 *
 * In summary, if this function returns true then the caller should access GIC
 * registers using redirect register block accessors & then call
 * mips_cm_unlock_other() when done. If this function returns false then the
 * caller should trivially access GIC registers in the local cluster.
 *
 * Returns true if locking performed, else false.
 */
static bool gic_irq_lock_cluster(struct irq_data *d)
{
	unsigned int cpu, cl;

	cpu = cpumask_first(irq_data_get_effective_affinity_mask(d));
	BUG_ON(cpu >= NR_CPUS);

	cl = cpu_cluster(&cpu_data[cpu]);
	if (cl == cpu_cluster(&current_cpu_data))
		return false;
	if (mips_cps_numcores(cl) == 0)
		return false;
	mips_cm_lock_other(cl, 0, 0, CM_GCR_Cx_OTHER_BLOCK_GLOBAL);
	return true;
}

static void gic_clear_pcpu_masks(unsigned int intr)
{
	unsigned int i;

	/* Clear the interrupt's bit in all pcpu_masks */
	for_each_possible_cpu(i)
		clear_bit(intr, per_cpu_ptr(pcpu_masks, i));
}

static bool gic_local_irq_is_routable(int intr)
{
	u32 vpe_ctl;

	/* All local interrupts are routable in EIC mode. */
	if (cpu_has_veic)
		return true;

	vpe_ctl = read_gic_vl_ctl();
	switch (intr) {
	case GIC_LOCAL_INT_TIMER:
		return vpe_ctl & GIC_VX_CTL_TIMER_ROUTABLE;
	case GIC_LOCAL_INT_PERFCTR:
		return vpe_ctl & GIC_VX_CTL_PERFCNT_ROUTABLE;
	case GIC_LOCAL_INT_FDC:
		return vpe_ctl & GIC_VX_CTL_FDC_ROUTABLE;
	case GIC_LOCAL_INT_SWINT0:
	case GIC_LOCAL_INT_SWINT1:
		return vpe_ctl & GIC_VX_CTL_SWINT_ROUTABLE;
	default:
		return true;
	}
}

static void gic_bind_eic_interrupt(int irq, int set)
{
	/* Convert irq vector # to hw int # */
	irq -= GIC_PIN_TO_VEC_OFFSET;

	/* Set irq to use shadow set */
	write_gic_vl_eic_shadow_set(irq, set);
}

static void gic_send_ipi(struct irq_data *d, unsigned int cpu)
{
	irq_hw_number_t hwirq = GIC_HWIRQ_TO_SHARED(irqd_to_hwirq(d));

	if (gic_irq_lock_cluster(d)) {
		write_gic_redir_wedge(GIC_WEDGE_RW | hwirq);
		mips_cm_unlock_other();
	} else {
		write_gic_wedge(GIC_WEDGE_RW | hwirq);
	}
}

int gic_get_c0_compare_int(void)
{
	if (!gic_local_irq_is_routable(GIC_LOCAL_INT_TIMER))
		return MIPS_CPU_IRQ_BASE + cp0_compare_irq;
	return irq_create_mapping(gic_irq_domain,
				  GIC_LOCAL_TO_HWIRQ(GIC_LOCAL_INT_TIMER));
}

int gic_get_c0_perfcount_int(void)
{
	if (!gic_local_irq_is_routable(GIC_LOCAL_INT_PERFCTR)) {
		/* Is the performance counter shared with the timer? */
		if (cp0_perfcount_irq < 0)
			return -1;
		return MIPS_CPU_IRQ_BASE + cp0_perfcount_irq;
	}
	return irq_create_mapping(gic_irq_domain,
				  GIC_LOCAL_TO_HWIRQ(GIC_LOCAL_INT_PERFCTR));
}

int gic_get_c0_fdc_int(void)
{
	if (!gic_local_irq_is_routable(GIC_LOCAL_INT_FDC)) {
		/* Is the FDC IRQ even present? */
		if (cp0_fdc_irq < 0)
			return -1;
		return MIPS_CPU_IRQ_BASE + cp0_fdc_irq;
	}

	return irq_create_mapping(gic_irq_domain,
				  GIC_LOCAL_TO_HWIRQ(GIC_LOCAL_INT_FDC));
}

static void gic_handle_shared_int(bool chained)
{
	unsigned int intr;
	unsigned long *pcpu_mask;
	DECLARE_BITMAP(pending, GIC_MAX_INTRS);

	/* Get per-cpu bitmaps */
	pcpu_mask = this_cpu_ptr(pcpu_masks);

	if (mips_cm_is64)
		__ioread64_copy(pending, addr_gic_pend(),
				DIV_ROUND_UP(gic_shared_intrs, 64));
	else
		__ioread32_copy(pending, addr_gic_pend(),
				DIV_ROUND_UP(gic_shared_intrs, 32));

	bitmap_and(pending, pending, pcpu_mask, gic_shared_intrs);

	for_each_set_bit(intr, pending, gic_shared_intrs) {
		if (chained)
			generic_handle_domain_irq(gic_irq_domain,
						  GIC_SHARED_TO_HWIRQ(intr));
		else
			do_domain_IRQ(gic_irq_domain,
				      GIC_SHARED_TO_HWIRQ(intr));
	}
}

static void gic_mask_irq(struct irq_data *d)
{
	unsigned int intr = GIC_HWIRQ_TO_SHARED(d->hwirq);

	if (gic_irq_lock_cluster(d)) {
		write_gic_redir_rmask(intr);
		mips_cm_unlock_other();
	} else {
		write_gic_rmask(intr);
	}

	gic_clear_pcpu_masks(intr);
}

static void gic_unmask_irq(struct irq_data *d)
{
	unsigned int intr = GIC_HWIRQ_TO_SHARED(d->hwirq);
	unsigned int cpu;

	if (gic_irq_lock_cluster(d)) {
		write_gic_redir_smask(intr);
		mips_cm_unlock_other();
	} else {
		write_gic_smask(intr);
	}

	gic_clear_pcpu_masks(intr);
	cpu = cpumask_first(irq_data_get_effective_affinity_mask(d));
	set_bit(intr, per_cpu_ptr(pcpu_masks, cpu));
}

static void gic_ack_irq(struct irq_data *d)
{
	unsigned int irq = GIC_HWIRQ_TO_SHARED(d->hwirq);

	if (gic_irq_lock_cluster(d)) {
		write_gic_redir_wedge(irq);
		mips_cm_unlock_other();
	} else {
		write_gic_wedge(irq);
	}
}

static int gic_set_type(struct irq_data *d, unsigned int type)
{
	unsigned int irq, pol, trig, dual;
	unsigned long flags;

	irq = GIC_HWIRQ_TO_SHARED(d->hwirq);

	raw_spin_lock_irqsave(&gic_lock, flags);
	switch (type & IRQ_TYPE_SENSE_MASK) {
	case IRQ_TYPE_EDGE_FALLING:
		pol = GIC_POL_FALLING_EDGE;
		trig = GIC_TRIG_EDGE;
		dual = GIC_DUAL_SINGLE;
		break;
	case IRQ_TYPE_EDGE_RISING:
		pol = GIC_POL_RISING_EDGE;
		trig = GIC_TRIG_EDGE;
		dual = GIC_DUAL_SINGLE;
		break;
	case IRQ_TYPE_EDGE_BOTH:
		pol = 0; /* Doesn't matter */
		trig = GIC_TRIG_EDGE;
		dual = GIC_DUAL_DUAL;
		break;
	case IRQ_TYPE_LEVEL_LOW:
		pol = GIC_POL_ACTIVE_LOW;
		trig = GIC_TRIG_LEVEL;
		dual = GIC_DUAL_SINGLE;
		break;
	case IRQ_TYPE_LEVEL_HIGH:
	default:
		pol = GIC_POL_ACTIVE_HIGH;
		trig = GIC_TRIG_LEVEL;
		dual = GIC_DUAL_SINGLE;
		break;
	}

	if (gic_irq_lock_cluster(d)) {
		change_gic_redir_pol(irq, pol);
		change_gic_redir_trig(irq, trig);
		change_gic_redir_dual(irq, dual);
		mips_cm_unlock_other();
	} else {
		change_gic_pol(irq, pol);
		change_gic_trig(irq, trig);
		change_gic_dual(irq, dual);
	}

	if (trig == GIC_TRIG_EDGE)
		irq_set_chip_handler_name_locked(d, &gic_edge_irq_controller,
						 handle_edge_irq, NULL);
	else
		irq_set_chip_handler_name_locked(d, &gic_level_irq_controller,
						 handle_level_irq, NULL);
	raw_spin_unlock_irqrestore(&gic_lock, flags);

	return 0;
}

#ifdef CONFIG_SMP
static int gic_set_affinity(struct irq_data *d, const struct cpumask *cpumask,
			    bool force)
{
	unsigned int irq = GIC_HWIRQ_TO_SHARED(d->hwirq);
	unsigned int cpu, cl, old_cpu, old_cl;
	unsigned long flags;

	/*
	 * The GIC specifies that we can only route an interrupt to one VP(E),
	 * ie. CPU in Linux parlance, at a time. Therefore we always route to
	 * the first forced or online CPU in the mask.
	 */
	if (force)
		cpu = cpumask_first(cpumask);
	else
		cpu = cpumask_first_and(cpumask, cpu_online_mask);

	if (cpu >= NR_CPUS)
		return -EINVAL;

	old_cpu = cpumask_first(irq_data_get_effective_affinity_mask(d));
	old_cl = cpu_cluster(&cpu_data[old_cpu]);
	cl = cpu_cluster(&cpu_data[cpu]);

	raw_spin_lock_irqsave(&gic_lock, flags);

	/*
	 * If we're moving affinity between clusters, stop routing the
	 * interrupt to any VP(E) in the old cluster.
	 */
	if (cl != old_cl) {
		if (gic_irq_lock_cluster(d)) {
			write_gic_redir_map_vp(irq, 0);
			mips_cm_unlock_other();
		} else {
			write_gic_map_vp(irq, 0);
		}
	}

	/*
	 * Update effective affinity - after this gic_irq_lock_cluster() will
	 * begin operating on the new cluster.
	 */
	irq_data_update_effective_affinity(d, cpumask_of(cpu));

	/*
	 * If we're moving affinity between clusters, configure the interrupt
	 * trigger type in the new cluster.
	 */
	if (cl != old_cl)
		gic_set_type(d, irqd_get_trigger_type(d));

	/* Route the interrupt to its new VP(E) */
	if (gic_irq_lock_cluster(d)) {
		write_gic_redir_map_pin(irq,
					GIC_MAP_PIN_MAP_TO_PIN | gic_cpu_pin);
		write_gic_redir_map_vp(irq, BIT(mips_cm_vp_id(cpu)));

		/* Update the pcpu_masks */
		gic_clear_pcpu_masks(irq);
		if (read_gic_redir_mask(irq))
			set_bit(irq, per_cpu_ptr(pcpu_masks, cpu));

		mips_cm_unlock_other();
	} else {
		write_gic_map_pin(irq, GIC_MAP_PIN_MAP_TO_PIN | gic_cpu_pin);
		write_gic_map_vp(irq, BIT(mips_cm_vp_id(cpu)));

		/* Update the pcpu_masks */
		gic_clear_pcpu_masks(irq);
		if (read_gic_mask(irq))
			set_bit(irq, per_cpu_ptr(pcpu_masks, cpu));
	}

	raw_spin_unlock_irqrestore(&gic_lock, flags);

	return IRQ_SET_MASK_OK;
}
#endif

static struct irq_chip gic_level_irq_controller = {
	.name			=	"MIPS GIC",
	.irq_mask		=	gic_mask_irq,
	.irq_unmask		=	gic_unmask_irq,
	.irq_set_type		=	gic_set_type,
#ifdef CONFIG_SMP
	.irq_set_affinity	=	gic_set_affinity,
#endif
};

static struct irq_chip gic_edge_irq_controller = {
	.name			=	"MIPS GIC",
	.irq_ack		=	gic_ack_irq,
	.irq_mask		=	gic_mask_irq,
	.irq_unmask		=	gic_unmask_irq,
	.irq_set_type		=	gic_set_type,
#ifdef CONFIG_SMP
	.irq_set_affinity	=	gic_set_affinity,
#endif
	.ipi_send_single	=	gic_send_ipi,
};

static void gic_handle_local_int(bool chained)
{
	unsigned long pending, masked;
	unsigned int intr;

	pending = read_gic_vl_pend();
	masked = read_gic_vl_mask();

	bitmap_and(&pending, &pending, &masked, GIC_NUM_LOCAL_INTRS);

	for_each_set_bit(intr, &pending, GIC_NUM_LOCAL_INTRS) {
		if (chained)
			generic_handle_domain_irq(gic_irq_domain,
						  GIC_LOCAL_TO_HWIRQ(intr));
		else
			do_domain_IRQ(gic_irq_domain,
				      GIC_LOCAL_TO_HWIRQ(intr));
	}
}

static void gic_mask_local_irq(struct irq_data *d)
{
	int intr = GIC_HWIRQ_TO_LOCAL(d->hwirq);

	write_gic_vl_rmask(BIT(intr));
}

static void gic_unmask_local_irq(struct irq_data *d)
{
	int intr = GIC_HWIRQ_TO_LOCAL(d->hwirq);

	write_gic_vl_smask(BIT(intr));
}

static struct irq_chip gic_local_irq_controller = {
	.name			=	"MIPS GIC Local",
	.irq_mask		=	gic_mask_local_irq,
	.irq_unmask		=	gic_unmask_local_irq,
};

static void gic_mask_local_irq_all_vpes(struct irq_data *d)
{
	struct gic_all_vpes_chip_data *cd;
	int intr, cpu;

	if (!mips_cps_multicluster_cpus())
		return;

	intr = GIC_HWIRQ_TO_LOCAL(d->hwirq);
	cd = irq_data_get_irq_chip_data(d);
	cd->mask = false;

	for_each_online_cpu_gic(cpu, &gic_lock)
		write_gic_vo_rmask(BIT(intr));
}

static void gic_unmask_local_irq_all_vpes(struct irq_data *d)
{
	struct gic_all_vpes_chip_data *cd;
	int intr, cpu;

	if (!mips_cps_multicluster_cpus())
		return;

	intr = GIC_HWIRQ_TO_LOCAL(d->hwirq);
	cd = irq_data_get_irq_chip_data(d);
	cd->mask = true;

	for_each_online_cpu_gic(cpu, &gic_lock)
		write_gic_vo_smask(BIT(intr));
}

static void gic_all_vpes_irq_cpu_online(void)
{
	static const unsigned int local_intrs[] = {
		GIC_LOCAL_INT_TIMER,
		GIC_LOCAL_INT_PERFCTR,
		GIC_LOCAL_INT_FDC,
	};
	unsigned long flags;
	int i;

	raw_spin_lock_irqsave(&gic_lock, flags);

	for (i = 0; i < ARRAY_SIZE(local_intrs); i++) {
		unsigned int intr = local_intrs[i];
		struct gic_all_vpes_chip_data *cd;

		if (!gic_local_irq_is_routable(intr))
			continue;
		cd = &gic_all_vpes_chip_data[intr];
		write_gic_vl_map(mips_gic_vx_map_reg(intr), cd->map);
		if (cd->mask)
			write_gic_vl_smask(BIT(intr));
	}

	raw_spin_unlock_irqrestore(&gic_lock, flags);
}

static struct irq_chip gic_all_vpes_local_irq_controller = {
	.name			= "MIPS GIC Local",
	.irq_mask		= gic_mask_local_irq_all_vpes,
	.irq_unmask		= gic_unmask_local_irq_all_vpes,
};

static void __gic_irq_dispatch(void)
{
	gic_handle_local_int(false);
	gic_handle_shared_int(false);
}

static void gic_irq_dispatch(struct irq_desc *desc)
{
	gic_handle_local_int(true);
	gic_handle_shared_int(true);
}

static int gic_shared_irq_domain_map(struct irq_domain *d, unsigned int virq,
				     irq_hw_number_t hw, unsigned int cpu)
{
	int intr = GIC_HWIRQ_TO_SHARED(hw);
	struct irq_data *data;
	unsigned long flags;

	data = irq_get_irq_data(virq);
	irq_data_update_effective_affinity(data, cpumask_of(cpu));

	raw_spin_lock_irqsave(&gic_lock, flags);

	/* Route the interrupt to its VP(E) */
	if (gic_irq_lock_cluster(data)) {
		write_gic_redir_map_pin(intr,
					GIC_MAP_PIN_MAP_TO_PIN | gic_cpu_pin);
		write_gic_redir_map_vp(intr, BIT(mips_cm_vp_id(cpu)));
		mips_cm_unlock_other();
	} else {
		write_gic_map_pin(intr, GIC_MAP_PIN_MAP_TO_PIN | gic_cpu_pin);
		write_gic_map_vp(intr, BIT(mips_cm_vp_id(cpu)));
	}

	raw_spin_unlock_irqrestore(&gic_lock, flags);

	return 0;
}

static int gic_irq_domain_xlate(struct irq_domain *d, struct device_node *ctrlr,
				const u32 *intspec, unsigned int intsize,
				irq_hw_number_t *out_hwirq,
				unsigned int *out_type)
{
	if (intsize != 3)
		return -EINVAL;

	if (intspec[0] == GIC_SHARED)
		*out_hwirq = GIC_SHARED_TO_HWIRQ(intspec[1]);
	else if (intspec[0] == GIC_LOCAL)
		*out_hwirq = GIC_LOCAL_TO_HWIRQ(intspec[1]);
	else
		return -EINVAL;
	*out_type = intspec[2] & IRQ_TYPE_SENSE_MASK;

	return 0;
}

static int gic_irq_domain_map(struct irq_domain *d, unsigned int virq,
			      irq_hw_number_t hwirq)
{
	struct gic_all_vpes_chip_data *cd;
	unsigned int intr;
	int err, cpu;
	u32 map;

	if (hwirq >= GIC_SHARED_HWIRQ_BASE) {
#ifdef CONFIG_GENERIC_IRQ_IPI
		/* verify that shared irqs don't conflict with an IPI irq */
		if (test_bit(GIC_HWIRQ_TO_SHARED(hwirq), ipi_resrv))
			return -EBUSY;
#endif /* CONFIG_GENERIC_IRQ_IPI */

		err = irq_domain_set_hwirq_and_chip(d, virq, hwirq,
						    &gic_level_irq_controller,
						    NULL);
		if (err)
			return err;

		irqd_set_single_target(irq_desc_get_irq_data(irq_to_desc(virq)));
		return gic_shared_irq_domain_map(d, virq, hwirq, 0);
	}

	intr = GIC_HWIRQ_TO_LOCAL(hwirq);
	map = GIC_MAP_PIN_MAP_TO_PIN | gic_cpu_pin;

	/*
	 * If adding support for more per-cpu interrupts, keep the
	 * array in gic_all_vpes_irq_cpu_online() in sync.
	 */
	switch (intr) {
	case GIC_LOCAL_INT_TIMER:
	case GIC_LOCAL_INT_PERFCTR:
	case GIC_LOCAL_INT_FDC:
		/*
		 * HACK: These are all really percpu interrupts, but
		 * the rest of the MIPS kernel code does not use the
		 * percpu IRQ API for them.
		 */
		cd = &gic_all_vpes_chip_data[intr];
		cd->map = map;
		err = irq_domain_set_hwirq_and_chip(d, virq, hwirq,
						    &gic_all_vpes_local_irq_controller,
						    cd);
		if (err)
			return err;

		irq_set_handler(virq, handle_percpu_irq);
		break;

	default:
		err = irq_domain_set_hwirq_and_chip(d, virq, hwirq,
						    &gic_local_irq_controller,
						    NULL);
		if (err)
			return err;

		irq_set_handler(virq, handle_percpu_devid_irq);
		irq_set_percpu_devid(virq);
		break;
	}

	if (!gic_local_irq_is_routable(intr))
		return -EPERM;

	if (mips_cps_multicluster_cpus()) {
		for_each_online_cpu_gic(cpu, &gic_lock)
			write_gic_vo_map(mips_gic_vx_map_reg(intr), map);
	}

	return 0;
}

static int gic_irq_domain_alloc(struct irq_domain *d, unsigned int virq,
				unsigned int nr_irqs, void *arg)
{
	struct irq_fwspec *fwspec = arg;
	irq_hw_number_t hwirq;

	if (fwspec->param[0] == GIC_SHARED)
		hwirq = GIC_SHARED_TO_HWIRQ(fwspec->param[1]);
	else
		hwirq = GIC_LOCAL_TO_HWIRQ(fwspec->param[1]);

	return gic_irq_domain_map(d, virq, hwirq);
}

static void gic_irq_domain_free(struct irq_domain *d, unsigned int virq,
			 unsigned int nr_irqs)
{
}

static const struct irq_domain_ops gic_irq_domain_ops = {
	.xlate = gic_irq_domain_xlate,
	.alloc = gic_irq_domain_alloc,
	.free = gic_irq_domain_free,
	.map = gic_irq_domain_map,
};

#ifdef CONFIG_GENERIC_IRQ_IPI

static int gic_ipi_domain_xlate(struct irq_domain *d, struct device_node *ctrlr,
				const u32 *intspec, unsigned int intsize,
				irq_hw_number_t *out_hwirq,
				unsigned int *out_type)
{
	/*
	 * There's nothing to translate here. hwirq is dynamically allocated and
	 * the irq type is always edge triggered.
	 * */
	*out_hwirq = 0;
	*out_type = IRQ_TYPE_EDGE_RISING;

	return 0;
}

static int gic_ipi_domain_alloc(struct irq_domain *d, unsigned int virq,
				unsigned int nr_irqs, void *arg)
{
	struct cpumask *ipimask = arg;
	irq_hw_number_t hwirq, base_hwirq;
	int cpu, ret, i;

	base_hwirq = find_first_bit(ipi_available, gic_shared_intrs);
	if (base_hwirq == gic_shared_intrs)
		return -ENOMEM;

	/* check that we have enough space */
	for (i = base_hwirq; i < nr_irqs; i++) {
		if (!test_bit(i, ipi_available))
			return -EBUSY;
	}
	bitmap_clear(ipi_available, base_hwirq, nr_irqs);

	/* map the hwirq for each cpu consecutively */
	i = 0;
	for_each_cpu(cpu, ipimask) {
		hwirq = GIC_SHARED_TO_HWIRQ(base_hwirq + i);

		ret = irq_domain_set_hwirq_and_chip(d, virq + i, hwirq,
						    &gic_edge_irq_controller,
						    NULL);
		if (ret)
			goto error;

		ret = irq_domain_set_hwirq_and_chip(d->parent, virq + i, hwirq,
						    &gic_edge_irq_controller,
						    NULL);
		if (ret)
			goto error;

		/* Set affinity to cpu.  */
		irq_data_update_effective_affinity(irq_get_irq_data(virq + i),
						   cpumask_of(cpu));
		ret = irq_set_irq_type(virq + i, IRQ_TYPE_EDGE_RISING);
		if (ret)
			goto error;

		ret = gic_shared_irq_domain_map(d, virq + i, hwirq, cpu);
		if (ret)
			goto error;

		i++;
	}

	return 0;
error:
	bitmap_set(ipi_available, base_hwirq, nr_irqs);
	return ret;
}

static void gic_ipi_domain_free(struct irq_domain *d, unsigned int virq,
				unsigned int nr_irqs)
{
	irq_hw_number_t base_hwirq;
	struct irq_data *data;

	data = irq_get_irq_data(virq);
	if (!data)
		return;

	base_hwirq = GIC_HWIRQ_TO_SHARED(irqd_to_hwirq(data));
	bitmap_set(ipi_available, base_hwirq, nr_irqs);
}

static int gic_ipi_domain_match(struct irq_domain *d, struct device_node *node,
				enum irq_domain_bus_token bus_token)
{
	bool is_ipi;

	switch (bus_token) {
	case DOMAIN_BUS_IPI:
		is_ipi = d->bus_token == bus_token;
		return (!node || to_of_node(d->fwnode) == node) && is_ipi;
		break;
	default:
		return 0;
	}
}

static const struct irq_domain_ops gic_ipi_domain_ops = {
	.xlate = gic_ipi_domain_xlate,
	.alloc = gic_ipi_domain_alloc,
	.free = gic_ipi_domain_free,
	.match = gic_ipi_domain_match,
};

static int gic_register_ipi_domain(struct device_node *node)
{
	struct irq_domain *gic_ipi_domain;
	unsigned int v[2], num_ipis;

	gic_ipi_domain = irq_domain_create_hierarchy(gic_irq_domain, IRQ_DOMAIN_FLAG_IPI_PER_CPU,
						     GIC_NUM_LOCAL_INTRS + gic_shared_intrs,
						     of_fwnode_handle(node), &gic_ipi_domain_ops,
						     NULL);
	if (!gic_ipi_domain) {
		pr_err("Failed to add IPI domain");
		return -ENXIO;
	}

	irq_domain_update_bus_token(gic_ipi_domain, DOMAIN_BUS_IPI);

	if (node &&
	    !of_property_read_u32_array(node, "mti,reserved-ipi-vectors", v, 2)) {
		bitmap_set(ipi_resrv, v[0], v[1]);
	} else {
		/*
		 * Reserve 2 interrupts per possible CPU/VP for use as IPIs,
		 * meeting the requirements of arch/mips SMP.
		 */
		num_ipis = 2 * num_possible_cpus();
		bitmap_set(ipi_resrv, gic_shared_intrs - num_ipis, num_ipis);
	}

	bitmap_copy(ipi_available, ipi_resrv, GIC_MAX_INTRS);

	return 0;
}

#else /* !CONFIG_GENERIC_IRQ_IPI */

static inline int gic_register_ipi_domain(struct device_node *node)
{
	return 0;
}

#endif /* !CONFIG_GENERIC_IRQ_IPI */

static int gic_cpu_startup(unsigned int cpu)
{
	/* Enable or disable EIC */
	change_gic_vl_ctl(GIC_VX_CTL_EIC,
			  cpu_has_veic ? GIC_VX_CTL_EIC : 0);

	/* Clear all local IRQ masks (ie. disable all local interrupts) */
	write_gic_vl_rmask(~0);

	/* Enable desired interrupts */
	gic_all_vpes_irq_cpu_online();

	return 0;
}

static int __init gic_of_init(struct device_node *node,
			      struct device_node *parent)
{
	unsigned int cpu_vec, i, gicconfig, cl, nclusters;
	unsigned long reserved;
	phys_addr_t gic_base;
	struct resource res;
	size_t gic_len;
	int ret;

	/* Find the first available CPU vector. */
	i = 0;
	reserved = (C_SW0 | C_SW1) >> __ffs(C_SW0);
	while (!of_property_read_u32_index(node, "mti,reserved-cpu-vectors",
					   i++, &cpu_vec))
		reserved |= BIT(cpu_vec);

	cpu_vec = find_first_zero_bit(&reserved, hweight_long(ST0_IM));
	if (cpu_vec == hweight_long(ST0_IM)) {
		pr_err("No CPU vectors available\n");
		return -ENODEV;
	}

	if (of_address_to_resource(node, 0, &res)) {
		/*
		 * Probe the CM for the GIC base address if not specified
		 * in the device-tree.
		 */
		if (mips_cm_present()) {
			gic_base = read_gcr_gic_base() &
				~CM_GCR_GIC_BASE_GICEN;
			gic_len = 0x20000;
			pr_warn("Using inherited base address %pa\n",
				&gic_base);
		} else {
			pr_err("Failed to get memory range\n");
			return -ENODEV;
		}
	} else {
		gic_base = res.start;
		gic_len = resource_size(&res);
	}

	if (mips_cm_present()) {
		write_gcr_gic_base(gic_base | CM_GCR_GIC_BASE_GICEN);
		/* Ensure GIC region is enabled before trying to access it */
		__sync();
	}

	mips_gic_base = ioremap(gic_base, gic_len);
	if (!mips_gic_base) {
		pr_err("Failed to ioremap gic_base\n");
		return -ENOMEM;
	}

	gicconfig = read_gic_config();
	gic_shared_intrs = FIELD_GET(GIC_CONFIG_NUMINTERRUPTS, gicconfig);
	gic_shared_intrs = (gic_shared_intrs + 1) * 8;

	if (cpu_has_veic) {
		/* Always use vector 1 in EIC mode */
		gic_cpu_pin = 0;
		set_vi_handler(gic_cpu_pin + GIC_PIN_TO_VEC_OFFSET,
			       __gic_irq_dispatch);
	} else {
		gic_cpu_pin = cpu_vec - GIC_CPU_PIN_OFFSET;
		irq_set_chained_handler(MIPS_CPU_IRQ_BASE + cpu_vec,
					gic_irq_dispatch);
	}

	gic_irq_domain = irq_domain_create_simple(of_fwnode_handle(node),
						  GIC_NUM_LOCAL_INTRS +
						  gic_shared_intrs, 0,
						  &gic_irq_domain_ops, NULL);
	if (!gic_irq_domain) {
		pr_err("Failed to add IRQ domain");
		return -ENXIO;
	}

	ret = gic_register_ipi_domain(node);
	if (ret)
		return ret;

	board_bind_eic_interrupt = &gic_bind_eic_interrupt;

	/*
	 * Initialise each cluster's GIC shared registers to sane default
	 * values.
	 * Otherwise, the IPI set up will be erased if we move code
	 * to gic_cpu_startup for each cpu.
	 */
	nclusters = mips_cps_numclusters();
	for (cl = 0; cl < nclusters; cl++) {
		if (cl == cpu_cluster(&current_cpu_data)) {
			for (i = 0; i < gic_shared_intrs; i++) {
				change_gic_pol(i, GIC_POL_ACTIVE_HIGH);
				change_gic_trig(i, GIC_TRIG_LEVEL);
				write_gic_rmask(i);
			}
		} else if (mips_cps_numcores(cl) != 0) {
			mips_cm_lock_other(cl, 0, 0, CM_GCR_Cx_OTHER_BLOCK_GLOBAL);
			for (i = 0; i < gic_shared_intrs; i++) {
				change_gic_redir_pol(i, GIC_POL_ACTIVE_HIGH);
				change_gic_redir_trig(i, GIC_TRIG_LEVEL);
				write_gic_redir_rmask(i);
			}
			mips_cm_unlock_other();

		} else {
			pr_warn("No CPU cores on the cluster %d skip it\n", cl);
		}
	}

	return cpuhp_setup_state(CPUHP_AP_IRQ_MIPS_GIC_STARTING,
				 "irqchip/mips/gic:starting",
				 gic_cpu_startup, NULL);
}
IRQCHIP_DECLARE(mips_gic, "mti,gic", gic_of_init);
