// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright 2020 Google LLC
 *
 * This driver provides the ability to view and manage Type C ports through the
 * Chrome OS EC.
 */

#include <linux/acpi.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_data/cros_ec_commands.h>
#include <linux/platform_data/cros_usbpd_notify.h>
#include <linux/platform_device.h>
#include <linux/usb/pd_vdo.h>
#include <linux/usb/typec_dp.h>
#include <linux/usb/typec_tbt.h>

#include "cros_ec_typec.h"
#include "cros_typec_vdm.h"
#include "cros_typec_altmode.h"

#define DRV_NAME "cros-ec-typec"

#define DP_PORT_VDO	(DP_CAP_DFP_D | DP_CAP_RECEPTACLE | \
			 DP_CONF_SET_PIN_ASSIGN(BIT(DP_PIN_ASSIGN_C) | \
						BIT(DP_PIN_ASSIGN_D) | \
						BIT(DP_PIN_ASSIGN_E)))

static void cros_typec_role_switch_quirk(struct fwnode_handle *fwnode)
{
#ifdef CONFIG_ACPI
	struct fwnode_handle *switch_fwnode;

	/* Supply the USB role switch with the correct pld_crc if it's missing. */
	switch_fwnode = fwnode_find_reference(fwnode, "usb-role-switch", 0);
	if (!IS_ERR_OR_NULL(switch_fwnode)) {
		struct acpi_device *adev = to_acpi_device_node(switch_fwnode);

		if (adev && !adev->pld_crc)
			adev->pld_crc = to_acpi_device_node(fwnode)->pld_crc;
		fwnode_handle_put(switch_fwnode);
	}
#endif
}

static int cros_typec_enter_usb_mode(struct typec_port *tc_port, enum usb_mode mode)
{
	struct cros_typec_port *port = typec_get_drvdata(tc_port);
	struct ec_params_typec_control req = {
		.port = port->port_num,
		.command = (mode == USB_MODE_USB4) ?
			TYPEC_CONTROL_COMMAND_ENTER_MODE : TYPEC_CONTROL_COMMAND_EXIT_MODES,
		.mode_to_enter = CROS_EC_ALTMODE_USB4
	};

	return cros_ec_cmd(port->typec_data->ec, 0, EC_CMD_TYPEC_CONTROL,
			  &req, sizeof(req), NULL, 0);
}

static int cros_typec_perform_role_swap(struct typec_port *tc_port, int target_role, u8 swap_type)
{
	struct cros_typec_port *port = typec_get_drvdata(tc_port);
	struct cros_typec_data *data = port->typec_data;
	struct ec_response_usb_pd_control_v2 resp;
	struct ec_params_usb_pd_control req;
	int role, ret;

	/* Must be at least v1 to support role swap. */
	if (!data->pd_ctrl_ver)
		return -EOPNOTSUPP;

	/* First query the state */
	req.port = port->port_num;
	req.role = USB_PD_CTRL_ROLE_NO_CHANGE;
	req.mux = USB_PD_CTRL_MUX_NO_CHANGE;
	req.swap = USB_PD_CTRL_SWAP_NONE;

	ret = cros_ec_cmd(data->ec, data->pd_ctrl_ver, EC_CMD_USB_PD_CONTROL,
				&req, sizeof(req), &resp, sizeof(resp));
	if (ret < 0)
		return ret;

	switch (swap_type) {
	case USB_PD_CTRL_SWAP_DATA:
		role = (resp.role & PD_CTRL_RESP_ROLE_DATA) ? TYPEC_HOST :
						TYPEC_DEVICE;
		break;
	case USB_PD_CTRL_SWAP_POWER:
		role = (resp.role & PD_CTRL_RESP_ROLE_POWER) ? TYPEC_SOURCE :
						TYPEC_SINK;
		break;
	default:
		dev_warn(data->dev, "Unsupported role swap type %d\n", swap_type);
		return -EOPNOTSUPP;
	}

	if (role == target_role)
		return 0;

	req.swap = swap_type;
	ret = cros_ec_cmd(data->ec, data->pd_ctrl_ver, EC_CMD_USB_PD_CONTROL,
				&req, sizeof(req), &resp, sizeof(resp));
	if (ret < 0)
		return ret;

	switch (swap_type) {
	case USB_PD_CTRL_SWAP_DATA:
		role = resp.role & PD_CTRL_RESP_ROLE_DATA ? TYPEC_HOST : TYPEC_DEVICE;
		if (role != target_role) {
			dev_err(data->dev, "Data role swap failed despite EC returning success\n");
			return -EIO;
		}
		typec_set_data_role(tc_port, target_role);
		break;
	case USB_PD_CTRL_SWAP_POWER:
		role = resp.role & PD_CTRL_RESP_ROLE_POWER ? TYPEC_SOURCE : TYPEC_SINK;
		if (role != target_role) {
			dev_err(data->dev, "Power role swap failed despite EC returning success\n");
			return -EIO;
		}
		typec_set_pwr_role(tc_port, target_role);
		break;
	default:
		/* Should never execute */
		break;
	}

	return 0;
}

static int cros_typec_dr_swap(struct typec_port *port, enum typec_data_role role)
{
	return cros_typec_perform_role_swap(port, role, USB_PD_CTRL_SWAP_DATA);
}

static int cros_typec_pr_swap(struct typec_port *port, enum typec_role role)
{
	return cros_typec_perform_role_swap(port, role, USB_PD_CTRL_SWAP_POWER);
}

static const struct typec_operations cros_typec_usb_mode_ops = {
	.enter_usb_mode = cros_typec_enter_usb_mode,
	.dr_set = cros_typec_dr_swap,
	.pr_set = cros_typec_pr_swap,
};

static int cros_typec_parse_port_props(struct typec_capability *cap,
				       struct fwnode_handle *fwnode,
				       struct device *dev)
{
	const char *buf;
	int ret;

	memset(cap, 0, sizeof(*cap));
	ret = fwnode_property_read_string(fwnode, "power-role", &buf);
	if (ret) {
		dev_err(dev, "power-role not found: %d\n", ret);
		return ret;
	}

	ret = typec_find_port_power_role(buf);
	if (ret < 0)
		return ret;
	cap->type = ret;

	ret = fwnode_property_read_string(fwnode, "data-role", &buf);
	if (ret) {
		dev_err(dev, "data-role not found: %d\n", ret);
		return ret;
	}

	ret = typec_find_port_data_role(buf);
	if (ret < 0)
		return ret;
	cap->data = ret;

	/* Try-power-role is optional. */
	ret = fwnode_property_read_string(fwnode, "try-power-role", &buf);
	if (ret) {
		dev_warn(dev, "try-power-role not found: %d\n", ret);
		cap->prefer_role = TYPEC_NO_PREFERRED_ROLE;
	} else {
		ret = typec_find_power_role(buf);
		if (ret < 0)
			return ret;
		cap->prefer_role = ret;
	}

	if (fwnode_property_present(fwnode, "usb2-port"))
		cap->usb_capability |= USB_CAPABILITY_USB2;
	if (fwnode_property_present(fwnode, "usb3-port"))
		cap->usb_capability |= USB_CAPABILITY_USB3;
	if (fwnode_property_present(fwnode, "usb4-port"))
		cap->usb_capability |= USB_CAPABILITY_USB4;

	cros_typec_role_switch_quirk(fwnode);

	cap->fwnode = fwnode;

	return 0;
}

static int cros_typec_get_switch_handles(struct cros_typec_port *port,
					 struct fwnode_handle *fwnode,
					 struct device *dev)
{
	int ret = 0;

	port->mux = fwnode_typec_mux_get(fwnode);
	if (IS_ERR(port->mux)) {
		ret = PTR_ERR(port->mux);
		dev_err_probe(dev, ret, "Mux handle not found\n");
		goto mux_err;
	}

	port->retimer = fwnode_typec_retimer_get(fwnode);
	if (IS_ERR(port->retimer)) {
		ret = PTR_ERR(port->retimer);
		dev_err_probe(dev, ret, "Retimer handle not found\n");
		goto retimer_sw_err;
	}

	port->ori_sw = fwnode_typec_switch_get(fwnode);
	if (IS_ERR(port->ori_sw)) {
		ret = PTR_ERR(port->ori_sw);
		dev_err_probe(dev, ret, "Orientation switch handle not found\n");
		goto ori_sw_err;
	}

	port->role_sw = fwnode_usb_role_switch_get(fwnode);
	if (IS_ERR(port->role_sw)) {
		ret = PTR_ERR(port->role_sw);
		dev_err_probe(dev, ret, "USB role switch handle not found\n");
		goto role_sw_err;
	}

	return 0;

role_sw_err:
	typec_switch_put(port->ori_sw);
	port->ori_sw = NULL;
ori_sw_err:
	typec_retimer_put(port->retimer);
	port->retimer = NULL;
retimer_sw_err:
	typec_mux_put(port->mux);
	port->mux = NULL;
mux_err:
	return ret;
}

static int cros_typec_add_partner(struct cros_typec_data *typec, int port_num,
				  bool pd_en)
{
	struct cros_typec_port *port = typec->ports[port_num];
	struct typec_partner_desc p_desc = {
		.usb_pd = pd_en,
	};
	int ret = 0;

	/*
	 * Fill an initial PD identity, which will then be updated with info
	 * from the EC.
	 */
	p_desc.identity = &port->p_identity;

	port->partner = typec_register_partner(port->port, &p_desc);
	if (IS_ERR(port->partner)) {
		ret = PTR_ERR(port->partner);
		port->partner = NULL;
	}

	return ret;
}

static void cros_typec_unregister_altmodes(struct cros_typec_data *typec, int port_num,
					   bool is_partner)
{
	struct cros_typec_port *port = typec->ports[port_num];
	struct cros_typec_altmode_node *node, *tmp;
	struct list_head *head;

	head = is_partner ? &port->partner_mode_list : &port->plug_mode_list;
	list_for_each_entry_safe(node, tmp, head, list) {
		list_del(&node->list);
		typec_unregister_altmode(node->amode);
		devm_kfree(typec->dev, node);
	}
}

/*
 * Map the Type-C Mux state to retimer state and call the retimer set function. We need this
 * because we re-use the Type-C mux state for retimers.
 */
static int cros_typec_retimer_set(struct typec_retimer *retimer, struct typec_mux_state state)
{
	struct typec_retimer_state rstate = {
		.alt = state.alt,
		.mode = state.mode,
		.data = state.data,
	};

	return typec_retimer_set(retimer, &rstate);
}

static int cros_typec_usb_disconnect_state(struct cros_typec_port *port)
{
	port->state.alt = NULL;
	port->state.mode = TYPEC_STATE_USB;
	port->state.data = NULL;

	usb_role_switch_set_role(port->role_sw, USB_ROLE_NONE);
	typec_switch_set(port->ori_sw, TYPEC_ORIENTATION_NONE);
	cros_typec_retimer_set(port->retimer, port->state);

	return typec_mux_set(port->mux, &port->state);
}

static void cros_typec_remove_partner(struct cros_typec_data *typec,
				      int port_num)
{
	struct cros_typec_port *port = typec->ports[port_num];

	if (!port->partner)
		return;

	cros_typec_unregister_altmodes(typec, port_num, true);

	typec_partner_set_usb_power_delivery(port->partner, NULL);
	usb_power_delivery_unregister_capabilities(port->partner_sink_caps);
	port->partner_sink_caps = NULL;
	usb_power_delivery_unregister_capabilities(port->partner_src_caps);
	port->partner_src_caps = NULL;
	usb_power_delivery_unregister(port->partner_pd);
	port->partner_pd = NULL;

	cros_typec_usb_disconnect_state(port);
	port->mux_flags = USB_PD_MUX_NONE;

	typec_unregister_partner(port->partner);
	port->partner = NULL;
	memset(&port->p_identity, 0, sizeof(port->p_identity));
	port->sop_disc_done = false;
}

static void cros_typec_remove_cable(struct cros_typec_data *typec,
				    int port_num)
{
	struct cros_typec_port *port = typec->ports[port_num];

	if (!port->cable)
		return;

	cros_typec_unregister_altmodes(typec, port_num, false);

	typec_unregister_plug(port->plug);
	port->plug = NULL;
	typec_unregister_cable(port->cable);
	port->cable = NULL;
	memset(&port->c_identity, 0, sizeof(port->c_identity));
	port->sop_prime_disc_done = false;
}

static void cros_typec_unregister_port_altmodes(struct cros_typec_port *port)
{
	int i;

	for (i = 0; i < CROS_EC_ALTMODE_MAX; i++)
		typec_unregister_altmode(port->port_altmode[i]);
}

static void cros_unregister_ports(struct cros_typec_data *typec)
{
	int i;

	for (i = 0; i < typec->num_ports; i++) {
		if (!typec->ports[i])
			continue;

		cros_typec_remove_partner(typec, i);
		cros_typec_remove_cable(typec, i);

		usb_role_switch_put(typec->ports[i]->role_sw);
		typec_switch_put(typec->ports[i]->ori_sw);
		typec_mux_put(typec->ports[i]->mux);
		cros_typec_unregister_port_altmodes(typec->ports[i]);
		typec_unregister_port(typec->ports[i]->port);
	}
}

/*
 * Register port alt modes with known values till we start retrieving
 * port capabilities from the EC.
 */
static int cros_typec_register_port_altmodes(struct cros_typec_data *typec,
					      int port_num)
{
	struct cros_typec_port *port = typec->ports[port_num];
	struct typec_altmode_desc desc;
	struct typec_altmode *amode;

	/* All PD capable CrOS devices are assumed to support DP altmode. */
	memset(&desc, 0, sizeof(desc));
	desc.svid = USB_TYPEC_DP_SID;
	desc.mode = USB_TYPEC_DP_MODE;
	desc.vdo = DP_PORT_VDO;
	amode = cros_typec_register_displayport(port, &desc,
						typec->ap_driven_altmode);
	if (IS_ERR(amode))
		return PTR_ERR(amode);
	port->port_altmode[CROS_EC_ALTMODE_DP] = amode;

	/*
	 * Register TBT compatibility alt mode. The EC will not enter the mode
	 * if it doesn't support it and it will not enter automatically by
	 * design so we can use the |ap_driven_altmode| feature to check if we
	 * should register it.
	 */
	if (typec->ap_driven_altmode) {
		memset(&desc, 0, sizeof(desc));
		desc.svid = USB_TYPEC_TBT_SID;
		desc.mode = TBT_MODE;
		desc.inactive = true;
		amode = cros_typec_register_thunderbolt(port, &desc);
		if (IS_ERR(amode))
			return PTR_ERR(amode);
		port->port_altmode[CROS_EC_ALTMODE_TBT] = amode;
	}

	port->state.alt = NULL;
	port->state.mode = TYPEC_STATE_USB;
	port->state.data = NULL;

	return 0;
}

static int cros_typec_init_ports(struct cros_typec_data *typec)
{
	struct device *dev = typec->dev;
	struct typec_capability *cap;
	struct fwnode_handle *fwnode;
	struct cros_typec_port *cros_port;
	const char *port_prop;
	int ret;
	int nports;
	u32 port_num = 0;

	nports = device_get_child_node_count(dev);
	if (nports == 0) {
		dev_err(dev, "No port entries found.\n");
		return -ENODEV;
	}

	if (nports > typec->num_ports) {
		dev_err(dev, "More ports listed than can be supported.\n");
		return -EINVAL;
	}

	/* DT uses "reg" to specify port number. */
	port_prop = dev->of_node ? "reg" : "port-number";
	device_for_each_child_node(dev, fwnode) {
		if (fwnode_property_read_u32(fwnode, port_prop, &port_num)) {
			ret = -EINVAL;
			dev_err(dev, "No port-number for port, aborting.\n");
			goto unregister_ports;
		}

		if (port_num >= typec->num_ports) {
			dev_err(dev, "Invalid port number.\n");
			ret = -EINVAL;
			goto unregister_ports;
		}

		dev_dbg(dev, "Registering port %d\n", port_num);

		cros_port = devm_kzalloc(dev, sizeof(*cros_port), GFP_KERNEL);
		if (!cros_port) {
			ret = -ENOMEM;
			goto unregister_ports;
		}

		cros_port->port_num = port_num;
		cros_port->typec_data = typec;
		typec->ports[port_num] = cros_port;
		cap = &cros_port->caps;

		ret = cros_typec_parse_port_props(cap, fwnode, dev);
		if (ret < 0)
			goto unregister_ports;

		cap->driver_data = cros_port;
		cap->ops = &cros_typec_usb_mode_ops;

		cros_port->port = typec_register_port(dev, cap);
		if (IS_ERR(cros_port->port)) {
			ret = PTR_ERR(cros_port->port);
			dev_err_probe(dev, ret, "Failed to register port %d\n", port_num);
			goto unregister_ports;
		}

		ret = cros_typec_get_switch_handles(cros_port, fwnode, dev);
		if (ret) {
			dev_dbg(dev, "No switch control for port %d, err: %d\n", port_num, ret);
			if (ret == -EPROBE_DEFER)
				goto unregister_ports;
		}

		ret = cros_typec_register_port_altmodes(typec, port_num);
		if (ret) {
			dev_err(dev, "Failed to register port altmodes\n");
			goto unregister_ports;
		}

		cros_port->disc_data = devm_kzalloc(dev, EC_PROTO2_MAX_RESPONSE_SIZE, GFP_KERNEL);
		if (!cros_port->disc_data) {
			ret = -ENOMEM;
			goto unregister_ports;
		}

		INIT_LIST_HEAD(&cros_port->partner_mode_list);
		INIT_LIST_HEAD(&cros_port->plug_mode_list);
	}

	return 0;

unregister_ports:
	fwnode_handle_put(fwnode);
	cros_unregister_ports(typec);
	return ret;
}

static int cros_typec_usb_safe_state(struct cros_typec_port *port)
{
	int ret;
	port->state.mode = TYPEC_STATE_SAFE;

	ret = cros_typec_retimer_set(port->retimer, port->state);
	if (!ret)
		ret = typec_mux_set(port->mux, &port->state);

	return ret;
}

/**
 * cros_typec_get_cable_vdo() - Get Cable VDO of the connected cable
 * @port: Type-C port data
 * @svid: Standard or Vendor ID to match
 *
 * Returns the Cable VDO if match is found and returns 0 if match is not found.
 */
static int cros_typec_get_cable_vdo(struct cros_typec_port *port, u16 svid)
{
	struct list_head *head = &port->plug_mode_list;
	struct cros_typec_altmode_node *node;
	u32 ret = 0;

	list_for_each_entry(node, head, list) {
		if (node->amode->svid == svid)
			return node->amode->vdo;
	}

	return ret;
}

/*
 * Spoof the VDOs that were likely communicated by the partner for TBT alt
 * mode.
 */
static int cros_typec_enable_tbt(struct cros_typec_data *typec,
				 int port_num,
				 struct ec_response_usb_pd_control_v2 *pd_ctrl)
{
	struct cros_typec_port *port = typec->ports[port_num];
	struct typec_thunderbolt_data data;
	int ret;

	if (typec->pd_ctrl_ver < 2) {
		dev_err(typec->dev,
			"PD_CTRL version too old: %d\n", typec->pd_ctrl_ver);
		return -ENOTSUPP;
	}

	/* Device Discover Mode VDO */
	data.device_mode = TBT_MODE;

	if (pd_ctrl->control_flags & USB_PD_CTRL_TBT_LEGACY_ADAPTER)
		data.device_mode = TBT_SET_ADAPTER(TBT_ADAPTER_TBT3);

	/* Cable Discover Mode VDO */
	data.cable_mode = TBT_MODE;

	data.cable_mode |= cros_typec_get_cable_vdo(port, USB_TYPEC_TBT_SID);

	data.cable_mode |= TBT_SET_CABLE_SPEED(pd_ctrl->cable_speed);

	if (pd_ctrl->control_flags & USB_PD_CTRL_OPTICAL_CABLE)
		data.cable_mode |= TBT_CABLE_OPTICAL;

	if (pd_ctrl->control_flags & USB_PD_CTRL_ACTIVE_LINK_UNIDIR)
		data.cable_mode |= TBT_CABLE_LINK_TRAINING;

	data.cable_mode |= TBT_SET_CABLE_ROUNDED(pd_ctrl->cable_gen);

	/* Enter Mode VDO */
	data.enter_vdo = TBT_SET_CABLE_SPEED(pd_ctrl->cable_speed);

	if (pd_ctrl->control_flags & USB_PD_CTRL_ACTIVE_CABLE)
		data.enter_vdo |= TBT_ENTER_MODE_ACTIVE_CABLE;

	if (!port->state.alt) {
		port->state.alt = port->port_altmode[CROS_EC_ALTMODE_TBT];
		ret = cros_typec_usb_safe_state(port);
		if (ret)
			return ret;
	}

	port->state.data = &data;
	port->state.mode = TYPEC_TBT_MODE;

	return typec_mux_set(port->mux, &port->state);
}

/* Spoof the VDOs that were likely communicated by the partner. */
static int cros_typec_enable_dp(struct cros_typec_data *typec,
				int port_num,
				struct ec_response_usb_pd_control_v2 *pd_ctrl)
{
	struct cros_typec_port *port = typec->ports[port_num];
	struct typec_displayport_data dp_data;
	u32 cable_tbt_vdo;
	u32 cable_dp_vdo;
	int ret;

	if (typec->pd_ctrl_ver < 2) {
		dev_err(typec->dev,
			"PD_CTRL version too old: %d\n", typec->pd_ctrl_ver);
		return -ENOTSUPP;
	}

	if (!pd_ctrl->dp_mode) {
		dev_err(typec->dev, "No valid DP mode provided.\n");
		return -EINVAL;
	}

	/* Status VDO. */
	dp_data.status = DP_STATUS_ENABLED;
	if (port->mux_flags & USB_PD_MUX_HPD_IRQ)
		dp_data.status |= DP_STATUS_IRQ_HPD;
	if (port->mux_flags & USB_PD_MUX_HPD_LVL)
		dp_data.status |= DP_STATUS_HPD_STATE;

	/* Configuration VDO. */
	dp_data.conf = DP_CONF_SET_PIN_ASSIGN(pd_ctrl->dp_mode);
	if (!port->state.alt) {
		port->state.alt = port->port_altmode[CROS_EC_ALTMODE_DP];
		ret = cros_typec_usb_safe_state(port);
		if (ret)
			return ret;
	}

	port->state.data = &dp_data;
	port->state.mode = TYPEC_MODAL_STATE(ffs(pd_ctrl->dp_mode));

	/* Get cable VDO for cables with DPSID to check DPAM2.1 is supported */
	cable_dp_vdo = cros_typec_get_cable_vdo(port, USB_TYPEC_DP_SID);

	/**
	 * Get cable VDO for thunderbolt cables and cables with DPSID but does not
	 * support DPAM2.1.
	 */
	cable_tbt_vdo = cros_typec_get_cable_vdo(port, USB_TYPEC_TBT_SID);

	if (cable_dp_vdo & DP_CAP_DPAM_VERSION) {
		dp_data.conf |= cable_dp_vdo;
	} else if (cable_tbt_vdo) {
		dp_data.conf |=  TBT_CABLE_SPEED(cable_tbt_vdo) << DP_CONF_SIGNALLING_SHIFT;

		/* Cable Type */
		if (cable_tbt_vdo & TBT_CABLE_OPTICAL)
			dp_data.conf |= DP_CONF_CABLE_TYPE_OPTICAL << DP_CONF_CABLE_TYPE_SHIFT;
		else if (cable_tbt_vdo & TBT_CABLE_RETIMER)
			dp_data.conf |= DP_CONF_CABLE_TYPE_RE_TIMER << DP_CONF_CABLE_TYPE_SHIFT;
		else if (cable_tbt_vdo & TBT_CABLE_ACTIVE_PASSIVE)
			dp_data.conf |= DP_CONF_CABLE_TYPE_RE_DRIVER << DP_CONF_CABLE_TYPE_SHIFT;
	} else if (PD_IDH_PTYPE(port->c_identity.id_header) == IDH_PTYPE_PCABLE) {
		dp_data.conf |= VDO_TYPEC_CABLE_SPEED(port->c_identity.vdo[0]) <<
				DP_CONF_SIGNALLING_SHIFT;
	}

	ret = cros_typec_retimer_set(port->retimer, port->state);
	if (!ret)
		ret = typec_mux_set(port->mux, &port->state);

	if (!ret)
		ret = cros_typec_displayport_status_update(port->state.alt,
							   port->state.data);

	return ret;
}

static int cros_typec_enable_usb4(struct cros_typec_data *typec,
				  int port_num,
				  struct ec_response_usb_pd_control_v2 *pd_ctrl)
{
	struct cros_typec_port *port = typec->ports[port_num];
	struct enter_usb_data data;

	data.eudo = EUDO_USB_MODE_USB4 << EUDO_USB_MODE_SHIFT;

	/* Cable Speed */
	data.eudo |= pd_ctrl->cable_speed << EUDO_CABLE_SPEED_SHIFT;

	/* Cable Type */
	if (pd_ctrl->control_flags & USB_PD_CTRL_OPTICAL_CABLE)
		data.eudo |= EUDO_CABLE_TYPE_OPTICAL << EUDO_CABLE_TYPE_SHIFT;
	else if (cros_typec_get_cable_vdo(port, USB_TYPEC_TBT_SID) & TBT_CABLE_RETIMER)
		data.eudo |= EUDO_CABLE_TYPE_RE_TIMER << EUDO_CABLE_TYPE_SHIFT;
	else if (pd_ctrl->control_flags & USB_PD_CTRL_ACTIVE_CABLE)
		data.eudo |= EUDO_CABLE_TYPE_RE_DRIVER << EUDO_CABLE_TYPE_SHIFT;

	data.active_link_training = !!(pd_ctrl->control_flags &
				       USB_PD_CTRL_ACTIVE_LINK_UNIDIR);

	port->state.alt = NULL;
	port->state.data = &data;
	port->state.mode = TYPEC_MODE_USB4;

	return typec_mux_set(port->mux, &port->state);
}

static int cros_typec_configure_mux(struct cros_typec_data *typec, int port_num,
				struct ec_response_usb_pd_control_v2 *pd_ctrl)
{
	struct cros_typec_port *port = typec->ports[port_num];
	struct ec_response_usb_pd_mux_info resp;
	struct ec_params_usb_pd_mux_info req = {
		.port = port_num,
	};
	struct ec_params_usb_pd_mux_ack mux_ack;
	enum typec_orientation orientation;
	struct cros_typec_altmode_node *node;
	int ret;

	ret = cros_ec_cmd(typec->ec, 0, EC_CMD_USB_PD_MUX_INFO,
			  &req, sizeof(req), &resp, sizeof(resp));
	if (ret < 0) {
		dev_warn(typec->dev, "Failed to get mux info for port: %d, err = %d\n",
			 port_num, ret);
		return ret;
	}

	/* No change needs to be made, let's exit early. */
	if (port->mux_flags == resp.flags && port->role == pd_ctrl->role)
		return 0;

	port->mux_flags = resp.flags;
	port->role = pd_ctrl->role;

	if (port->mux_flags == USB_PD_MUX_NONE) {
		ret = cros_typec_usb_disconnect_state(port);
		goto mux_ack;
	}

	if (port->mux_flags & USB_PD_MUX_POLARITY_INVERTED)
		orientation = TYPEC_ORIENTATION_REVERSE;
	else
		orientation = TYPEC_ORIENTATION_NORMAL;

	ret = typec_switch_set(port->ori_sw, orientation);
	if (ret)
		return ret;

	ret = usb_role_switch_set_role(typec->ports[port_num]->role_sw,
					pd_ctrl->role & PD_CTRL_RESP_ROLE_DATA
					? USB_ROLE_HOST : USB_ROLE_DEVICE);
	if (ret)
		return ret;

	if (port->mux_flags & USB_PD_MUX_USB4_ENABLED) {
		ret = cros_typec_enable_usb4(typec, port_num, pd_ctrl);
	} else if (port->mux_flags & USB_PD_MUX_TBT_COMPAT_ENABLED) {
		ret = cros_typec_enable_tbt(typec, port_num, pd_ctrl);
	} else if (port->mux_flags & USB_PD_MUX_DP_ENABLED) {
		ret = cros_typec_enable_dp(typec, port_num, pd_ctrl);
	} else if (port->mux_flags & USB_PD_MUX_SAFE_MODE) {
		ret = cros_typec_usb_safe_state(port);
	} else if (port->mux_flags & USB_PD_MUX_USB_ENABLED) {
		port->state.alt = NULL;
		port->state.mode = TYPEC_STATE_USB;

		ret = cros_typec_retimer_set(port->retimer, port->state);
		if (!ret)
			ret = typec_mux_set(port->mux, &port->state);
	} else {
		dev_dbg(typec->dev,
			"Unrecognized mode requested, mux flags: %x\n",
			port->mux_flags);
	}

	/* Iterate all partner alt-modes and set the active alternate mode. */
	list_for_each_entry(node, &port->partner_mode_list, list) {
		typec_altmode_update_active(
			node->amode,
			port->state.alt &&
				node->amode->svid == port->state.alt->svid);
	}

mux_ack:
	if (!typec->needs_mux_ack)
		return ret;

	/* Sending Acknowledgment to EC */
	mux_ack.port = port_num;

	if (cros_ec_cmd(typec->ec, 0, EC_CMD_USB_PD_MUX_ACK, &mux_ack,
			sizeof(mux_ack), NULL, 0) < 0)
		dev_warn(typec->dev,
			 "Failed to send Mux ACK to EC for port: %d\n",
			 port_num);

	return ret;
}

static void cros_typec_set_port_params_v0(struct cros_typec_data *typec,
		int port_num, struct ec_response_usb_pd_control *resp)
{
	struct typec_port *port = typec->ports[port_num]->port;
	enum typec_orientation polarity;

	if (!resp->enabled)
		polarity = TYPEC_ORIENTATION_NONE;
	else if (!resp->polarity)
		polarity = TYPEC_ORIENTATION_NORMAL;
	else
		polarity = TYPEC_ORIENTATION_REVERSE;

	typec_set_pwr_role(port, resp->role ? TYPEC_SOURCE : TYPEC_SINK);
	typec_set_orientation(port, polarity);
}

static void cros_typec_set_port_params_v1(struct cros_typec_data *typec,
		int port_num, struct ec_response_usb_pd_control_v1 *resp)
{
	struct typec_port *port = typec->ports[port_num]->port;
	enum typec_orientation polarity;
	bool pd_en;
	int ret;

	if (!(resp->enabled & PD_CTRL_RESP_ENABLED_CONNECTED))
		polarity = TYPEC_ORIENTATION_NONE;
	else if (!resp->polarity)
		polarity = TYPEC_ORIENTATION_NORMAL;
	else
		polarity = TYPEC_ORIENTATION_REVERSE;
	typec_set_orientation(port, polarity);
	typec_set_data_role(port, resp->role & PD_CTRL_RESP_ROLE_DATA ?
			TYPEC_HOST : TYPEC_DEVICE);
	typec_set_pwr_role(port, resp->role & PD_CTRL_RESP_ROLE_POWER ?
			TYPEC_SOURCE : TYPEC_SINK);
	typec_set_vconn_role(port, resp->role & PD_CTRL_RESP_ROLE_VCONN ?
			TYPEC_SOURCE : TYPEC_SINK);

	/* Register/remove partners when a connect/disconnect occurs. */
	if (resp->enabled & PD_CTRL_RESP_ENABLED_CONNECTED) {
		if (typec->ports[port_num]->partner)
			return;

		pd_en = resp->enabled & PD_CTRL_RESP_ENABLED_PD_CAPABLE;
		ret = cros_typec_add_partner(typec, port_num, pd_en);
		if (ret)
			dev_warn(typec->dev,
				 "Failed to register partner on port: %d\n",
				 port_num);
	} else {
		cros_typec_remove_partner(typec, port_num);
		cros_typec_remove_cable(typec, port_num);
	}
}

/*
 * Helper function to register partner/plug altmodes.
 */
static int cros_typec_register_altmodes(struct cros_typec_data *typec, int port_num,
					bool is_partner)
{
	struct cros_typec_port *port = typec->ports[port_num];
	struct ec_response_typec_discovery *sop_disc = port->disc_data;
	struct cros_typec_altmode_node *node;
	struct typec_altmode_desc desc;
	struct typec_altmode *amode;
	int num_altmodes = 0;
	int ret = 0;
	int i, j;

	for (i = 0; i < sop_disc->svid_count; i++) {
		for (j = 0; j < sop_disc->svids[i].mode_count; j++) {
			memset(&desc, 0, sizeof(desc));
			desc.svid = sop_disc->svids[i].svid;
			desc.mode = j + 1;
			desc.vdo = sop_disc->svids[i].mode_vdo[j];

			if (is_partner)
				amode = typec_partner_register_altmode(port->partner, &desc);
			else
				amode = typec_plug_register_altmode(port->plug, &desc);

			if (IS_ERR(amode)) {
				ret = PTR_ERR(amode);
				goto err_cleanup;
			}

			/* If no memory is available we should unregister and exit. */
			node = devm_kzalloc(typec->dev, sizeof(*node), GFP_KERNEL);
			if (!node) {
				ret = -ENOMEM;
				typec_unregister_altmode(amode);
				goto err_cleanup;
			}

			node->amode = amode;

			if (is_partner)
				list_add_tail(&node->list, &port->partner_mode_list);
			else
				list_add_tail(&node->list, &port->plug_mode_list);
			num_altmodes++;
		}
	}

	if (is_partner)
		ret = typec_partner_set_num_altmodes(port->partner, num_altmodes);
	else
		ret = typec_plug_set_num_altmodes(port->plug, num_altmodes);

	if (ret < 0) {
		dev_err(typec->dev, "Unable to set %s num_altmodes for port: %d\n",
			is_partner ? "partner" : "plug", port_num);
		goto err_cleanup;
	}

	return 0;

err_cleanup:
	cros_typec_unregister_altmodes(typec, port_num, is_partner);
	return ret;
}

/*
 * Parse the PD identity data from the EC PD discovery responses and copy that to the supplied
 * PD identity struct.
 */
static void cros_typec_parse_pd_identity(struct usb_pd_identity *id,
					 struct ec_response_typec_discovery *disc)
{
	int i;

	/* First, update the PD identity VDOs for the partner. */
	if (disc->identity_count > 0)
		id->id_header = disc->discovery_vdo[0];
	if (disc->identity_count > 1)
		id->cert_stat = disc->discovery_vdo[1];
	if (disc->identity_count > 2)
		id->product = disc->discovery_vdo[2];

	/* Copy the remaining identity VDOs till a maximum of 6. */
	for (i = 3; i < disc->identity_count && i < VDO_MAX_OBJECTS; i++)
		id->vdo[i - 3] = disc->discovery_vdo[i];
}

static int cros_typec_handle_sop_prime_disc(struct cros_typec_data *typec, int port_num, u16 pd_revision)
{
	struct cros_typec_port *port = typec->ports[port_num];
	struct ec_response_typec_discovery *disc = port->disc_data;
	struct typec_cable_desc c_desc = {};
	struct typec_plug_desc p_desc;
	struct ec_params_typec_discovery req = {
		.port = port_num,
		.partner_type = TYPEC_PARTNER_SOP_PRIME,
	};
	u32 cable_plug_type;
	int ret = 0;

	memset(disc, 0, EC_PROTO2_MAX_RESPONSE_SIZE);
	ret = cros_ec_cmd(typec->ec, 0, EC_CMD_TYPEC_DISCOVERY, &req, sizeof(req),
			  disc, EC_PROTO2_MAX_RESPONSE_SIZE);
	if (ret < 0) {
		dev_err(typec->dev, "Failed to get SOP' discovery data for port: %d\n", port_num);
		goto sop_prime_disc_exit;
	}

	/* Parse the PD identity data, even if only 0s were returned. */
	cros_typec_parse_pd_identity(&port->c_identity, disc);

	if (disc->identity_count != 0) {
		cable_plug_type = VDO_TYPEC_CABLE_TYPE(port->c_identity.vdo[0]);
		switch (cable_plug_type) {
		case CABLE_ATYPE:
			c_desc.type = USB_PLUG_TYPE_A;
			break;
		case CABLE_BTYPE:
			c_desc.type = USB_PLUG_TYPE_B;
			break;
		case CABLE_CTYPE:
			c_desc.type = USB_PLUG_TYPE_C;
			break;
		case CABLE_CAPTIVE:
			c_desc.type = USB_PLUG_CAPTIVE;
			break;
		default:
			c_desc.type = USB_PLUG_NONE;
		}
		c_desc.active = PD_IDH_PTYPE(port->c_identity.id_header) == IDH_PTYPE_ACABLE;
	}

	c_desc.identity = &port->c_identity;
	c_desc.pd_revision = pd_revision;

	port->cable = typec_register_cable(port->port, &c_desc);
	if (IS_ERR(port->cable)) {
		ret = PTR_ERR(port->cable);
		port->cable = NULL;
		goto sop_prime_disc_exit;
	}

	p_desc.index = TYPEC_PLUG_SOP_P;
	port->plug = typec_register_plug(port->cable, &p_desc);
	if (IS_ERR(port->plug)) {
		ret = PTR_ERR(port->plug);
		port->plug = NULL;
		goto sop_prime_disc_exit;
	}

	ret = cros_typec_register_altmodes(typec, port_num, false);
	if (ret < 0) {
		dev_err(typec->dev, "Failed to register plug altmodes, port: %d\n", port_num);
		goto sop_prime_disc_exit;
	}

	return 0;

sop_prime_disc_exit:
	cros_typec_remove_cable(typec, port_num);
	return ret;
}

static int cros_typec_handle_sop_disc(struct cros_typec_data *typec, int port_num, u16 pd_revision)
{
	struct cros_typec_port *port = typec->ports[port_num];
	struct ec_response_typec_discovery *sop_disc = port->disc_data;
	struct ec_params_typec_discovery req = {
		.port = port_num,
		.partner_type = TYPEC_PARTNER_SOP,
	};
	int ret = 0;

	if (!port->partner) {
		dev_err(typec->dev,
			"SOP Discovery received without partner registered, port: %d\n",
			port_num);
		ret = -EINVAL;
		goto disc_exit;
	}

	typec_partner_set_pd_revision(port->partner, pd_revision);

	memset(sop_disc, 0, EC_PROTO2_MAX_RESPONSE_SIZE);
	ret = cros_ec_cmd(typec->ec, 0, EC_CMD_TYPEC_DISCOVERY, &req, sizeof(req),
			  sop_disc, EC_PROTO2_MAX_RESPONSE_SIZE);
	if (ret < 0) {
		dev_err(typec->dev, "Failed to get SOP discovery data for port: %d\n", port_num);
		goto disc_exit;
	}

	cros_typec_parse_pd_identity(&port->p_identity, sop_disc);

	ret = typec_partner_set_identity(port->partner);
	if (ret < 0) {
		dev_err(typec->dev, "Failed to update partner PD identity, port: %d\n", port_num);
		goto disc_exit;
	}

	ret = cros_typec_register_altmodes(typec, port_num, true);
	if (ret < 0) {
		dev_err(typec->dev, "Failed to register partner altmodes, port: %d\n", port_num);
		goto disc_exit;
	}

disc_exit:
	return ret;
}

static int cros_typec_send_clear_event(struct cros_typec_data *typec, int port_num, u32 events_mask)
{
	struct ec_params_typec_control req = {
		.port = port_num,
		.command = TYPEC_CONTROL_COMMAND_CLEAR_EVENTS,
		.clear_events_mask = events_mask,
	};

	return cros_ec_cmd(typec->ec, 0, EC_CMD_TYPEC_CONTROL, &req,
			   sizeof(req), NULL, 0);
}

static void cros_typec_register_partner_pdos(struct cros_typec_data *typec,
					     struct ec_response_typec_status *resp, int port_num)
{
	struct usb_power_delivery_capabilities_desc caps_desc = {};
	struct usb_power_delivery_desc desc = {
		.revision = (le16_to_cpu(resp->sop_revision) & 0xff00) >> 4,
	};
	struct cros_typec_port *port = typec->ports[port_num];

	if (!port->partner || port->partner_pd)
		return;

	/* If no caps are available, don't bother creating a device. */
	if (!resp->source_cap_count && !resp->sink_cap_count)
		return;

	port->partner_pd = typec_partner_usb_power_delivery_register(port->partner, &desc);
	if (IS_ERR(port->partner_pd)) {
		dev_warn(typec->dev, "Failed to register partner PD device, port: %d\n", port_num);
		return;
	}

	typec_partner_set_usb_power_delivery(port->partner, port->partner_pd);

	memcpy(caps_desc.pdo, resp->source_cap_pdos, sizeof(u32) * resp->source_cap_count);
	caps_desc.role = TYPEC_SOURCE;
	port->partner_src_caps = usb_power_delivery_register_capabilities(port->partner_pd,
									  &caps_desc);
	if (IS_ERR(port->partner_src_caps))
		dev_warn(typec->dev, "Failed to register source caps, port: %d\n", port_num);

	memset(&caps_desc, 0, sizeof(caps_desc));
	memcpy(caps_desc.pdo, resp->sink_cap_pdos, sizeof(u32) * resp->sink_cap_count);
	caps_desc.role = TYPEC_SINK;
	port->partner_sink_caps = usb_power_delivery_register_capabilities(port->partner_pd,
									   &caps_desc);
	if (IS_ERR(port->partner_sink_caps))
		dev_warn(typec->dev, "Failed to register sink caps, port: %d\n", port_num);
}

static void cros_typec_handle_status(struct cros_typec_data *typec, int port_num)
{
	struct ec_response_typec_status resp;
	struct ec_params_typec_status req = {
		.port = port_num,
	};
	int ret;

	ret = cros_ec_cmd(typec->ec, 0, EC_CMD_TYPEC_STATUS, &req, sizeof(req),
			  &resp, sizeof(resp));
	if (ret < 0) {
		dev_warn(typec->dev, "EC_CMD_TYPEC_STATUS failed for port: %d\n", port_num);
		return;
	}

	/* If we got a hard reset, unregister everything and return. */
	if (resp.events & PD_STATUS_EVENT_HARD_RESET) {
		cros_typec_remove_partner(typec, port_num);
		cros_typec_remove_cable(typec, port_num);

		ret = cros_typec_send_clear_event(typec, port_num,
						  PD_STATUS_EVENT_HARD_RESET);
		if (ret < 0)
			dev_warn(typec->dev,
				 "Failed hard reset event clear, port: %d\n", port_num);
		return;
	}

	/* Handle any events appropriately. */
	if (resp.events & PD_STATUS_EVENT_SOP_DISC_DONE && !typec->ports[port_num]->sop_disc_done) {
		u16 sop_revision;

		/* Convert BCD to the format preferred by the TypeC framework */
		sop_revision = (le16_to_cpu(resp.sop_revision) & 0xff00) >> 4;
		ret = cros_typec_handle_sop_disc(typec, port_num, sop_revision);
		if (ret < 0)
			dev_err(typec->dev, "Couldn't parse SOP Disc data, port: %d\n", port_num);
		else {
			typec->ports[port_num]->sop_disc_done = true;
			ret = cros_typec_send_clear_event(typec, port_num,
							  PD_STATUS_EVENT_SOP_DISC_DONE);
			if (ret < 0)
				dev_warn(typec->dev,
					 "Failed SOP Disc event clear, port: %d\n", port_num);
		}
		if (resp.sop_connected)
			typec_set_pwr_opmode(typec->ports[port_num]->port, TYPEC_PWR_MODE_PD);

		cros_typec_register_partner_pdos(typec, &resp, port_num);
	}

	if (resp.events & PD_STATUS_EVENT_SOP_PRIME_DISC_DONE &&
	    !typec->ports[port_num]->sop_prime_disc_done) {
		u16 sop_prime_revision;

		/* Convert BCD to the format preferred by the TypeC framework */
		sop_prime_revision = (le16_to_cpu(resp.sop_prime_revision) & 0xff00) >> 4;
		ret = cros_typec_handle_sop_prime_disc(typec, port_num, sop_prime_revision);
		if (ret < 0)
			dev_err(typec->dev, "Couldn't parse SOP' Disc data, port: %d\n", port_num);
		else {
			typec->ports[port_num]->sop_prime_disc_done = true;
			ret = cros_typec_send_clear_event(typec, port_num,
							  PD_STATUS_EVENT_SOP_PRIME_DISC_DONE);
			if (ret < 0)
				dev_warn(typec->dev,
					 "Failed SOP Disc event clear, port: %d\n", port_num);
		}
	}

	if (resp.events & PD_STATUS_EVENT_VDM_REQ_REPLY) {
		cros_typec_handle_vdm_response(typec, port_num);
		ret = cros_typec_send_clear_event(typec, port_num, PD_STATUS_EVENT_VDM_REQ_REPLY);
		if (ret < 0)
			dev_warn(typec->dev, "Failed VDM Reply event clear, port: %d\n", port_num);
	}

	if (resp.events & PD_STATUS_EVENT_VDM_ATTENTION) {
		cros_typec_handle_vdm_attention(typec, port_num);
		ret = cros_typec_send_clear_event(typec, port_num, PD_STATUS_EVENT_VDM_ATTENTION);
		if (ret < 0)
			dev_warn(typec->dev, "Failed VDM attention event clear, port: %d\n",
				 port_num);
	}
}

static int cros_typec_port_update(struct cros_typec_data *typec, int port_num)
{
	struct ec_params_usb_pd_control req;
	struct ec_response_usb_pd_control_v2 resp;
	int ret;

	if (port_num < 0 || port_num >= typec->num_ports) {
		dev_err(typec->dev, "cannot get status for invalid port %d\n",
			port_num);
		return -EINVAL;
	}

	req.port = port_num;
	req.role = USB_PD_CTRL_ROLE_NO_CHANGE;
	req.mux = USB_PD_CTRL_MUX_NO_CHANGE;
	req.swap = USB_PD_CTRL_SWAP_NONE;

	ret = cros_ec_cmd(typec->ec, typec->pd_ctrl_ver,
			  EC_CMD_USB_PD_CONTROL, &req, sizeof(req),
			  &resp, sizeof(resp));
	if (ret < 0)
		return ret;

	/* Update the switches if they exist, according to requested state */
	ret = cros_typec_configure_mux(typec, port_num, &resp);
	if (ret)
		dev_warn(typec->dev, "Configure muxes failed, err = %d\n", ret);

	dev_dbg(typec->dev, "Enabled %d: 0x%hhx\n", port_num, resp.enabled);
	dev_dbg(typec->dev, "Role %d: 0x%hhx\n", port_num, resp.role);
	dev_dbg(typec->dev, "Polarity %d: 0x%hhx\n", port_num, resp.polarity);
	dev_dbg(typec->dev, "State %d: %s\n", port_num, resp.state);

	if (typec->pd_ctrl_ver != 0)
		cros_typec_set_port_params_v1(typec, port_num,
			(struct ec_response_usb_pd_control_v1 *)&resp);
	else
		cros_typec_set_port_params_v0(typec, port_num,
			(struct ec_response_usb_pd_control *) &resp);

	if (typec->typec_cmd_supported)
		cros_typec_handle_status(typec, port_num);

	return 0;
}

static int cros_typec_get_cmd_version(struct cros_typec_data *typec)
{
	struct ec_params_get_cmd_versions_v1 req_v1;
	struct ec_response_get_cmd_versions resp;
	int ret;

	/* We're interested in the PD control command version. */
	req_v1.cmd = EC_CMD_USB_PD_CONTROL;
	ret = cros_ec_cmd(typec->ec, 1, EC_CMD_GET_CMD_VERSIONS,
			  &req_v1, sizeof(req_v1), &resp, sizeof(resp));
	if (ret < 0)
		return ret;

	if (resp.version_mask & EC_VER_MASK(2))
		typec->pd_ctrl_ver = 2;
	else if (resp.version_mask & EC_VER_MASK(1))
		typec->pd_ctrl_ver = 1;
	else
		typec->pd_ctrl_ver = 0;

	dev_dbg(typec->dev, "PD Control has version mask 0x%02x\n",
		typec->pd_ctrl_ver & 0xff);

	return 0;
}

static void cros_typec_port_work(struct work_struct *work)
{
	struct cros_typec_data *typec = container_of(work, struct cros_typec_data, port_work);
	int ret, i;

	for (i = 0; i < typec->num_ports; i++) {
		ret = cros_typec_port_update(typec, i);
		if (ret < 0)
			dev_warn(typec->dev, "Update failed for port: %d\n", i);
	}
}

static int cros_ec_typec_event(struct notifier_block *nb,
			       unsigned long host_event, void *_notify)
{
	struct cros_typec_data *typec = container_of(nb, struct cros_typec_data, nb);

	flush_work(&typec->port_work);
	schedule_work(&typec->port_work);

	return NOTIFY_OK;
}

#ifdef CONFIG_ACPI
static const struct acpi_device_id cros_typec_acpi_id[] = {
	{ "GOOG0014", 0 },
	{}
};
MODULE_DEVICE_TABLE(acpi, cros_typec_acpi_id);
#endif

#ifdef CONFIG_OF
static const struct of_device_id cros_typec_of_match[] = {
	{ .compatible = "google,cros-ec-typec", },
	{}
};
MODULE_DEVICE_TABLE(of, cros_typec_of_match);
#endif

static int cros_typec_probe(struct platform_device *pdev)
{
	struct cros_ec_dev *ec_dev = NULL;
	struct device *dev = &pdev->dev;
	struct cros_typec_data *typec;
	struct ec_response_usb_pd_ports resp;
	int ret, i;

	typec = devm_kzalloc(dev, sizeof(*typec), GFP_KERNEL);
	if (!typec)
		return -ENOMEM;

	typec->dev = dev;

	typec->ec = dev_get_drvdata(pdev->dev.parent);
	if (!typec->ec || !typec->ec->ec) {
		dev_warn(dev, "couldn't find parent EC device\n");
		return -EPROBE_DEFER;
	}

	platform_set_drvdata(pdev, typec);

	ret = cros_typec_get_cmd_version(typec);
	if (ret < 0) {
		dev_err(dev, "failed to get PD command version info\n");
		return ret;
	}

	ec_dev = dev_get_drvdata(&typec->ec->ec->dev);
	if (!ec_dev)
		return -EPROBE_DEFER;

	typec->typec_cmd_supported = cros_ec_check_features(ec_dev, EC_FEATURE_TYPEC_CMD);
	typec->needs_mux_ack = cros_ec_check_features(ec_dev, EC_FEATURE_TYPEC_MUX_REQUIRE_AP_ACK);
	typec->ap_driven_altmode = cros_ec_check_features(
		ec_dev, EC_FEATURE_TYPEC_REQUIRE_AP_MODE_ENTRY);

	ret = cros_ec_cmd(typec->ec, 0, EC_CMD_USB_PD_PORTS, NULL, 0,
			  &resp, sizeof(resp));
	if (ret < 0)
		return ret;

	typec->num_ports = resp.num_ports;
	if (typec->num_ports > EC_USB_PD_MAX_PORTS) {
		dev_warn(typec->dev,
			 "Too many ports reported: %d, limiting to max: %d\n",
			 typec->num_ports, EC_USB_PD_MAX_PORTS);
		typec->num_ports = EC_USB_PD_MAX_PORTS;
	}

	ret = cros_typec_init_ports(typec);
	if (ret < 0)
		return ret;

	INIT_WORK(&typec->port_work, cros_typec_port_work);

	/*
	 * Safe to call port update here, since we haven't registered the
	 * PD notifier yet.
	 */
	for (i = 0; i < typec->num_ports; i++) {
		ret = cros_typec_port_update(typec, i);
		if (ret < 0)
			goto unregister_ports;
	}

	typec->nb.notifier_call = cros_ec_typec_event;
	ret = cros_usbpd_register_notify(&typec->nb);
	if (ret < 0)
		goto unregister_ports;

	return 0;

unregister_ports:
	cros_unregister_ports(typec);
	return ret;
}

static void cros_typec_remove(struct platform_device *pdev)
{
	struct cros_typec_data *typec = platform_get_drvdata(pdev);

	cros_usbpd_unregister_notify(&typec->nb);
	cancel_work_sync(&typec->port_work);
	cros_unregister_ports(typec);
}

static int __maybe_unused cros_typec_suspend(struct device *dev)
{
	struct cros_typec_data *typec = dev_get_drvdata(dev);

	cancel_work_sync(&typec->port_work);

	return 0;
}

static int __maybe_unused cros_typec_resume(struct device *dev)
{
	struct cros_typec_data *typec = dev_get_drvdata(dev);

	/* Refresh port state. */
	schedule_work(&typec->port_work);

	return 0;
}

static const struct dev_pm_ops cros_typec_pm_ops = {
	SET_SYSTEM_SLEEP_PM_OPS(cros_typec_suspend, cros_typec_resume)
};

static struct platform_driver cros_typec_driver = {
	.driver	= {
		.name = DRV_NAME,
		.acpi_match_table = ACPI_PTR(cros_typec_acpi_id),
		.of_match_table = of_match_ptr(cros_typec_of_match),
		.pm = &cros_typec_pm_ops,
	},
	.probe = cros_typec_probe,
	.remove = cros_typec_remove,
};

module_platform_driver(cros_typec_driver);

MODULE_AUTHOR("Prashant Malani <pmalani@chromium.org>");
MODULE_DESCRIPTION("Chrome OS EC Type C control");
MODULE_LICENSE("GPL");
