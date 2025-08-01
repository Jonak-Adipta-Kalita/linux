/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (c) 2018, Intel Corporation. */

#ifndef _ICE_COMMON_H_
#define _ICE_COMMON_H_

#include <linux/bitfield.h>

#include "ice.h"
#include "ice_type.h"
#include "ice_nvm.h"
#include "ice_flex_pipe.h"
#include "ice_parser.h"
#include <linux/avf/virtchnl.h>
#include "ice_switch.h"
#include "ice_fdir.h"

#define ICE_SQ_SEND_DELAY_TIME_MS	10
#define ICE_SQ_SEND_MAX_EXECUTE		3

#define FEC_REG_SHIFT 2
#define FEC_RECV_ID_SHIFT 4
#define FEC_CORR_LOW_REG_PORT0 (0x02 << FEC_REG_SHIFT)
#define FEC_CORR_HIGH_REG_PORT0 (0x03 << FEC_REG_SHIFT)
#define FEC_UNCORR_LOW_REG_PORT0 (0x04 << FEC_REG_SHIFT)
#define FEC_UNCORR_HIGH_REG_PORT0 (0x05 << FEC_REG_SHIFT)
#define FEC_CORR_LOW_REG_PORT1 (0x42 << FEC_REG_SHIFT)
#define FEC_CORR_HIGH_REG_PORT1 (0x43 << FEC_REG_SHIFT)
#define FEC_UNCORR_LOW_REG_PORT1 (0x44 << FEC_REG_SHIFT)
#define FEC_UNCORR_HIGH_REG_PORT1 (0x45 << FEC_REG_SHIFT)
#define FEC_CORR_LOW_REG_PORT2 (0x4A << FEC_REG_SHIFT)
#define FEC_CORR_HIGH_REG_PORT2 (0x4B << FEC_REG_SHIFT)
#define FEC_UNCORR_LOW_REG_PORT2 (0x4C << FEC_REG_SHIFT)
#define FEC_UNCORR_HIGH_REG_PORT2 (0x4D << FEC_REG_SHIFT)
#define FEC_CORR_LOW_REG_PORT3 (0x52 << FEC_REG_SHIFT)
#define FEC_CORR_HIGH_REG_PORT3 (0x53 << FEC_REG_SHIFT)
#define FEC_UNCORR_LOW_REG_PORT3 (0x54 << FEC_REG_SHIFT)
#define FEC_UNCORR_HIGH_REG_PORT3 (0x55 << FEC_REG_SHIFT)
#define FEC_RECEIVER_ID_PCS0 (0x33 << FEC_RECV_ID_SHIFT)
#define FEC_RECEIVER_ID_PCS1 (0x34 << FEC_RECV_ID_SHIFT)

#define ICE_CGU_R9			0x24
#define ICE_CGU_R9_TIME_REF_FREQ_SEL	GENMASK(2, 0)
#define ICE_CGU_R9_CLK_EREF0_EN		BIT(4)
#define ICE_CGU_R9_TIME_REF_EN		BIT(5)
#define ICE_CGU_R9_TIME_SYNC_EN		BIT(6)
#define ICE_CGU_R9_ONE_PPS_OUT_EN	BIT(7)
#define ICE_CGU_R9_ONE_PPS_OUT_AMP	GENMASK(19, 18)

#define ICE_CGU_R16			0x40
#define ICE_CGU_R16_TSPLL_CK_REFCLKFREQ	GENMASK(31, 24)

#define ICE_CGU_R19			0x4C
#define ICE_CGU_R19_TSPLL_FBDIV_INTGR_E82X	GENMASK(7, 0)
#define ICE_CGU_R19_TSPLL_FBDIV_INTGR_E825	GENMASK(9, 0)
#define ICE_CGU_R19_TSPLL_NDIVRATIO	GENMASK(19, 16)

#define ICE_CGU_R22			0x58
#define ICE_CGU_R22_TIME1588CLK_DIV	GENMASK(23, 20)
#define ICE_CGU_R22_TIME1588CLK_DIV2	BIT(30)

#define ICE_CGU_R23			0x5C
#define ICE_CGU_R24			0x60
#define ICE_CGU_R24_FBDIV_FRAC		GENMASK(21, 0)
#define ICE_CGU_R23_R24_TSPLL_ENABLE	BIT(24)
#define ICE_CGU_R23_R24_REF1588_CK_DIV	GENMASK(30, 27)
#define ICE_CGU_R23_R24_TIME_REF_SEL	BIT(31)

#define ICE_CGU_BW_TDC			0x31C
#define ICE_CGU_BW_TDC_PLLLOCK_SEL	GENMASK(30, 29)

#define ICE_CGU_RO_LOCK			0x3F0
#define ICE_CGU_RO_LOCK_TRUE_LOCK	BIT(12)
#define ICE_CGU_RO_LOCK_UNLOCK		BIT(13)

#define ICE_CGU_CNTR_BIST		0x344
#define ICE_CGU_CNTR_BIST_PLLLOCK_SEL_0	BIT(15)
#define ICE_CGU_CNTR_BIST_PLLLOCK_SEL_1	BIT(16)

#define ICE_CGU_RO_BWM_LF		0x370
#define ICE_CGU_RO_BWM_LF_TRUE_LOCK	BIT(12)

int ice_init_hw(struct ice_hw *hw);
void ice_deinit_hw(struct ice_hw *hw);
int ice_check_reset(struct ice_hw *hw);
int ice_reset(struct ice_hw *hw, enum ice_reset_req req);
int ice_create_all_ctrlq(struct ice_hw *hw);
int ice_init_all_ctrlq(struct ice_hw *hw);
void ice_shutdown_all_ctrlq(struct ice_hw *hw, bool unloading);
void ice_destroy_all_ctrlq(struct ice_hw *hw);
int
ice_clean_rq_elem(struct ice_hw *hw, struct ice_ctl_q_info *cq,
		  struct ice_rq_event_info *e, u16 *pending);
int
ice_get_link_status(struct ice_port_info *pi, bool *link_up);
int ice_update_link_info(struct ice_port_info *pi);
int
ice_acquire_res(struct ice_hw *hw, enum ice_aq_res_ids res,
		enum ice_aq_res_access_type access, u32 timeout);
void ice_release_res(struct ice_hw *hw, enum ice_aq_res_ids res);
int
ice_alloc_hw_res(struct ice_hw *hw, u16 type, u16 num, bool btm, u16 *res);
int
ice_free_hw_res(struct ice_hw *hw, u16 type, u16 num, u16 *res);
int ice_aq_alloc_free_res(struct ice_hw *hw,
			  struct ice_aqc_alloc_free_res_elem *buf, u16 buf_size,
			  enum ice_adminq_opc opc);
bool ice_is_sbq_supported(struct ice_hw *hw);
struct ice_ctl_q_info *ice_get_sbq(struct ice_hw *hw);
int
ice_sq_send_cmd(struct ice_hw *hw, struct ice_ctl_q_info *cq,
		struct libie_aq_desc *desc, void *buf, u16 buf_size,
		struct ice_sq_cd *cd);
void ice_clear_pxe_mode(struct ice_hw *hw);
int ice_get_caps(struct ice_hw *hw);

void ice_set_safe_mode_caps(struct ice_hw *hw);

int ice_write_rxq_ctx(struct ice_hw *hw, struct ice_rlan_ctx *rlan_ctx,
		      u32 rxq_index);
int ice_read_rxq_ctx(struct ice_hw *hw, struct ice_rlan_ctx *rlan_ctx,
		     u32 rxq_index);
int ice_read_txq_ctx(struct ice_hw *hw, struct ice_tlan_ctx *tlan_ctx,
		     u32 txq_index);
int ice_write_txq_ctx(struct ice_hw *hw, struct ice_tlan_ctx *tlan_ctx,
		      u32 txq_index);

int
ice_aq_get_rss_lut(struct ice_hw *hw, struct ice_aq_get_set_rss_lut_params *get_params);
int
ice_aq_set_rss_lut(struct ice_hw *hw, struct ice_aq_get_set_rss_lut_params *set_params);
int
ice_aq_get_rss_key(struct ice_hw *hw, u16 vsi_handle,
		   struct ice_aqc_get_set_rss_keys *keys);
int
ice_aq_set_rss_key(struct ice_hw *hw, u16 vsi_handle,
		   struct ice_aqc_get_set_rss_keys *keys);

bool ice_check_sq_alive(struct ice_hw *hw, struct ice_ctl_q_info *cq);
int ice_aq_q_shutdown(struct ice_hw *hw, bool unloading);
void ice_fill_dflt_direct_cmd_desc(struct libie_aq_desc *desc, u16 opcode);

void ice_pack_txq_ctx(const struct ice_tlan_ctx *ctx, ice_txq_ctx_buf_t *buf);

extern struct mutex ice_global_cfg_lock_sw;

int
ice_aq_send_cmd(struct ice_hw *hw, struct libie_aq_desc *desc,
		void *buf, u16 buf_size, struct ice_sq_cd *cd);
int ice_aq_get_fw_ver(struct ice_hw *hw, struct ice_sq_cd *cd);

int
ice_aq_send_driver_ver(struct ice_hw *hw, struct ice_driver_ver *dv,
		       struct ice_sq_cd *cd);
int
ice_aq_set_port_params(struct ice_port_info *pi, bool double_vlan,
		       struct ice_sq_cd *cd);
int
ice_aq_get_phy_caps(struct ice_port_info *pi, bool qual_mods, u8 report_mode,
		    struct ice_aqc_get_phy_caps_data *caps,
		    struct ice_sq_cd *cd);
bool ice_is_phy_rclk_in_netlist(struct ice_hw *hw);
bool ice_is_clock_mux_in_netlist(struct ice_hw *hw);
bool ice_is_cgu_in_netlist(struct ice_hw *hw);
bool ice_is_gps_in_netlist(struct ice_hw *hw);
int
ice_aq_get_netlist_node(struct ice_hw *hw, struct ice_aqc_get_link_topo *cmd,
			u8 *node_part_number, u16 *node_handle);
int
ice_aq_list_caps(struct ice_hw *hw, void *buf, u16 buf_size, u32 *cap_count,
		 enum ice_adminq_opc opc, struct ice_sq_cd *cd);
int
ice_discover_dev_caps(struct ice_hw *hw, struct ice_hw_dev_caps *dev_caps);
void
ice_update_phy_type(u64 *phy_type_low, u64 *phy_type_high,
		    u16 link_speeds_bitmap);
int
ice_aq_manage_mac_write(struct ice_hw *hw, const u8 *mac_addr, u8 flags,
			struct ice_sq_cd *cd);
bool ice_is_generic_mac(struct ice_hw *hw);
int ice_clear_pf_cfg(struct ice_hw *hw);
int
ice_aq_set_phy_cfg(struct ice_hw *hw, struct ice_port_info *pi,
		   struct ice_aqc_set_phy_cfg_data *cfg, struct ice_sq_cd *cd);
bool ice_fw_supports_link_override(struct ice_hw *hw);
int
ice_get_link_default_override(struct ice_link_default_override_tlv *ldo,
			      struct ice_port_info *pi);
bool ice_is_phy_caps_an_enabled(struct ice_aqc_get_phy_caps_data *caps);
bool ice_is_fw_health_report_supported(struct ice_hw *hw);
int ice_aq_set_health_status_cfg(struct ice_hw *hw, u8 event_source);
int ice_aq_get_phy_equalization(struct ice_hw *hw, u16 data_in, u16 op_code,
				u8 serdes_num, int *output);
int
ice_aq_get_fec_stats(struct ice_hw *hw, u16 pcs_quad, u16 pcs_port,
		     enum ice_fec_stats_types fec_type, u32 *output);

enum ice_fc_mode ice_caps_to_fc_mode(u8 caps);
enum ice_fec_mode ice_caps_to_fec_mode(u8 caps, u8 fec_options);
int
ice_set_fc(struct ice_port_info *pi, u8 *aq_failures,
	   bool ena_auto_link_update);
int
ice_cfg_phy_fc(struct ice_port_info *pi, struct ice_aqc_set_phy_cfg_data *cfg,
	       enum ice_fc_mode req_mode);
bool
ice_phy_caps_equals_cfg(struct ice_aqc_get_phy_caps_data *caps,
			struct ice_aqc_set_phy_cfg_data *cfg);
void
ice_copy_phy_caps_to_cfg(struct ice_port_info *pi,
			 struct ice_aqc_get_phy_caps_data *caps,
			 struct ice_aqc_set_phy_cfg_data *cfg);
int
ice_cfg_phy_fec(struct ice_port_info *pi, struct ice_aqc_set_phy_cfg_data *cfg,
		enum ice_fec_mode fec);
int
ice_aq_set_link_restart_an(struct ice_port_info *pi, bool ena_link,
			   struct ice_sq_cd *cd);
int
ice_aq_set_mac_cfg(struct ice_hw *hw, u16 max_frame_size, struct ice_sq_cd *cd);
int
ice_aq_get_link_info(struct ice_port_info *pi, bool ena_lse,
		     struct ice_link_status *link, struct ice_sq_cd *cd);
int
ice_aq_set_event_mask(struct ice_hw *hw, u8 port_num, u16 mask,
		      struct ice_sq_cd *cd);
int
ice_aq_set_mac_loopback(struct ice_hw *hw, bool ena_lpbk, struct ice_sq_cd *cd);

int
ice_aq_set_port_id_led(struct ice_port_info *pi, bool is_orig_mode,
		       struct ice_sq_cd *cd);
int
ice_aq_get_port_options(struct ice_hw *hw,
			struct ice_aqc_get_port_options_elem *options,
			u8 *option_count, u8 lport, bool lport_valid,
			u8 *active_option_idx, bool *active_option_valid,
			u8 *pending_option_idx, bool *pending_option_valid);
int
ice_aq_set_port_option(struct ice_hw *hw, u8 lport, u8 lport_valid,
		       u8 new_option);
int ice_get_phy_lane_number(struct ice_hw *hw);
int
ice_aq_sff_eeprom(struct ice_hw *hw, u16 lport, u8 bus_addr,
		  u16 mem_addr, u8 page, u8 set_page, u8 *data, u8 length,
		  bool write, struct ice_sq_cd *cd);
u32 ice_get_link_speed(u16 index);

int
ice_cfg_vsi_rdma(struct ice_port_info *pi, u16 vsi_handle, u16 tc_bitmap,
		 u16 *max_rdmaqs);
int
ice_ena_vsi_rdma_qset(struct ice_port_info *pi, u16 vsi_handle, u8 tc,
		      u16 *rdma_qset, u16 num_qsets, u32 *qset_teid);
int
ice_dis_vsi_rdma_qset(struct ice_port_info *pi, u16 count, u32 *qset_teid,
		      u16 *q_id);
int
ice_dis_vsi_txq(struct ice_port_info *pi, u16 vsi_handle, u8 tc, u8 num_queues,
		u16 *q_handle, u16 *q_ids, u32 *q_teids,
		enum ice_disq_rst_src rst_src, u16 vmvf_num,
		struct ice_sq_cd *cd);
int
ice_cfg_vsi_lan(struct ice_port_info *pi, u16 vsi_handle, u8 tc_bitmap,
		u16 *max_lanqs);
int
ice_ena_vsi_txq(struct ice_port_info *pi, u16 vsi_handle, u8 tc, u16 q_handle,
		u8 num_qgrps, struct ice_aqc_add_tx_qgrp *buf, u16 buf_size,
		struct ice_sq_cd *cd);
int
ice_aq_cfg_lan_txq(struct ice_hw *hw, struct ice_aqc_cfg_txqs_buf *buf,
		   u16 buf_size, u16 num_qs, u8 oldport, u8 newport,
		   struct ice_sq_cd *cd);
int ice_replay_vsi(struct ice_hw *hw, u16 vsi_handle);
void ice_replay_post(struct ice_hw *hw);
struct ice_q_ctx *
ice_get_lan_q_ctx(struct ice_hw *hw, u16 vsi_handle, u8 tc, u16 q_handle);
int ice_sbq_rw_reg(struct ice_hw *hw, struct ice_sbq_msg_input *in, u16 flag);
int ice_aq_get_cgu_input_pin_measure(struct ice_hw *hw, u8 dpll_idx,
				     struct ice_cgu_input_measure *meas,
				     u16 meas_num);
int
ice_aq_get_cgu_abilities(struct ice_hw *hw,
			 struct ice_aqc_get_cgu_abilities *abilities);
int
ice_aq_set_input_pin_cfg(struct ice_hw *hw, u8 input_idx, u8 flags1, u8 flags2,
			 u32 freq, s32 phase_delay);
int
ice_aq_get_input_pin_cfg(struct ice_hw *hw, u8 input_idx, u8 *status, u8 *type,
			 u8 *flags1, u8 *flags2, u32 *freq, s32 *phase_delay);
int
ice_aq_set_output_pin_cfg(struct ice_hw *hw, u8 output_idx, u8 flags,
			  u8 src_sel, u32 freq, s32 phase_delay);
int
ice_aq_get_output_pin_cfg(struct ice_hw *hw, u8 output_idx, u8 *flags,
			  u8 *src_sel, u32 *freq, u32 *src_freq);
int
ice_aq_get_cgu_dpll_status(struct ice_hw *hw, u8 dpll_num, u8 *ref_state,
			   u8 *dpll_state, u8 *config, s64 *phase_offset,
			   u8 *eec_mode);
int
ice_aq_set_cgu_dpll_config(struct ice_hw *hw, u8 dpll_num, u8 ref_state,
			   u8 config, u8 eec_mode);
int
ice_aq_set_cgu_ref_prio(struct ice_hw *hw, u8 dpll_num, u8 ref_idx,
			u8 ref_priority);
int
ice_aq_get_cgu_ref_prio(struct ice_hw *hw, u8 dpll_num, u8 ref_idx,
			u8 *ref_prio);
int
ice_aq_get_cgu_info(struct ice_hw *hw, u32 *cgu_id, u32 *cgu_cfg_ver,
		    u32 *cgu_fw_ver);

int
ice_aq_set_phy_rec_clk_out(struct ice_hw *hw, u8 phy_output, bool enable,
			   u32 *freq);
int
ice_aq_get_phy_rec_clk_out(struct ice_hw *hw, u8 *phy_output, u8 *port_num,
			   u8 *flags, u16 *node_handle);
int ice_aq_get_sensor_reading(struct ice_hw *hw,
			      struct ice_aqc_get_sensor_reading_resp *data);
void
ice_stat_update40(struct ice_hw *hw, u32 reg, bool prev_stat_loaded,
		  u64 *prev_stat, u64 *cur_stat);
void
ice_stat_update32(struct ice_hw *hw, u32 reg, bool prev_stat_loaded,
		  u64 *prev_stat, u64 *cur_stat);
int
ice_sched_query_elem(struct ice_hw *hw, u32 node_teid,
		     struct ice_aqc_txsched_elem_data *buf);
int
ice_aq_set_gpio(struct ice_hw *hw, u16 gpio_ctrl_handle, u8 pin_idx, bool value,
		struct ice_sq_cd *cd);
int
ice_aq_get_gpio(struct ice_hw *hw, u16 gpio_ctrl_handle, u8 pin_idx,
		bool *value, struct ice_sq_cd *cd);
bool ice_is_100m_speed_supported(struct ice_hw *hw);
u16 ice_get_link_speed_based_on_phy_type(u64 phy_type_low, u64 phy_type_high);
int
ice_aq_set_lldp_mib(struct ice_hw *hw, u8 mib_type, void *buf, u16 buf_size,
		    struct ice_sq_cd *cd);
bool ice_fw_supports_lldp_fltr_ctrl(struct ice_hw *hw);
int ice_lldp_fltr_add_remove(struct ice_hw *hw, struct ice_vsi *vsi, bool add);
int ice_lldp_execute_pending_mib(struct ice_hw *hw);
int
ice_aq_read_i2c(struct ice_hw *hw, struct ice_aqc_link_topo_addr topo_addr,
		u16 bus_addr, __le16 addr, u8 params, u8 *data,
		struct ice_sq_cd *cd);
int
ice_aq_write_i2c(struct ice_hw *hw, struct ice_aqc_link_topo_addr topo_addr,
		 u16 bus_addr, __le16 addr, u8 params, const u8 *data,
		 struct ice_sq_cd *cd);
int ice_get_pca9575_handle(struct ice_hw *hw, u16 *pca9575_handle);
int ice_read_pca9575_reg(struct ice_hw *hw, u8 offset, u8 *data);
bool ice_fw_supports_report_dflt_cfg(struct ice_hw *hw);
int ice_read_cgu_reg(struct ice_hw *hw, u32 addr, u32 *val);
int ice_write_cgu_reg(struct ice_hw *hw, u32 addr, u32 val);
#endif /* _ICE_COMMON_H_ */
