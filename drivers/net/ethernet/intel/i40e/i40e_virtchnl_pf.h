/*******************************************************************************
 *
 * Intel Ethernet Controller XL710 Family Linux Driver
 * Copyright(c) 2013 - 2015 Intel Corporation.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * The full GNU General Public License is included in this distribution in
 * the file called "COPYING".
 *
 * Contact Information:
 * e1000-devel Mailing List <e1000-devel@lists.sourceforge.net>
 * Intel Corporation, 5200 N.E. Elam Young Parkway, Hillsboro, OR 97124-6497
 *
 ******************************************************************************/

#ifndef _I40E_VIRTCHNL_PF_H_
#define _I40E_VIRTCHNL_PF_H_

#include "i40e.h"

#define I40E_MAX_VLANID 4095

#define I40E_VIRTCHNL_SUPPORTED_QTYPES 2

#define I40E_DEFAULT_NUM_MDD_EVENTS_ALLOWED	3
#define I40E_DEFAULT_NUM_INVALID_MSGS_ALLOWED	10

#define I40E_VLAN_PRIORITY_SHIFT	12
#define I40E_VLAN_MASK			0xFFF
#define I40E_PRIORITY_MASK		0x7000

/* Various queue ctrls */
enum i40e_queue_ctrl {
	I40E_QUEUE_CTRL_UNKNOWN = 0,
	I40E_QUEUE_CTRL_ENABLE,
	I40E_QUEUE_CTRL_ENABLECHECK,
	I40E_QUEUE_CTRL_DISABLE,
	I40E_QUEUE_CTRL_DISABLECHECK,
	I40E_QUEUE_CTRL_FASTDISABLE,
	I40E_QUEUE_CTRL_FASTDISABLECHECK,
};

/* VF states */
enum i40e_vf_states {
	I40E_VF_STATE_INIT = 0,
	I40E_VF_STATE_ACTIVE,
	I40E_VF_STATE_IWARPENA,
	I40E_VF_STATE_DISABLED,
	I40E_VF_STATE_MC_PROMISC,
	I40E_VF_STATE_UC_PROMISC,
	I40E_VF_STATE_PRE_ENABLE,
	I40E_VF_STATE_RESETTING
};

/* VF capabilities */
enum i40e_vf_capabilities {
	I40E_VIRTCHNL_VF_CAP_PRIVILEGE = 0,
	I40E_VIRTCHNL_VF_CAP_L2,
	I40E_VIRTCHNL_VF_CAP_IWARP,
};

/* VF information structure */
struct i40e_vf {
	struct i40e_pf *pf;

	/* VF id in the PF space */
	s16 vf_id;
	/* all VF vsis connect to the same parent */
	enum i40e_switch_element_types parent_type;
	struct virtchnl_version_info vf_ver;
	u32 driver_caps; /* reported by VF driver */

	/* VF Port Extender (PE) stag if used */
	u16 stag;

	struct virtchnl_ether_addr default_lan_addr;
	u16 port_vlan_id;
	bool pf_set_mac;	/* The VMM admin set the VF MAC address */
	bool trusted;

	/* VSI indices - actual VSI pointers are maintained in the PF structure
	 * When assigned, these will be non-zero, because VSI 0 is always
	 * the main LAN VSI for the PF.
	 */
	u16 lan_vsi_idx;	/* index into PF struct */
	u16 lan_vsi_id;		/* ID as used by firmware */

	u8 num_queue_pairs;	/* num of qps assigned to VF vsis */
	u8 num_req_queues;	/* num of requested qps */
	u64 num_mdd_events;	/* num of mdd events detected */
	/* num of continuous malformed or invalid msgs detected */
	u64 num_invalid_msgs;
	u64 num_valid_msgs;	/* num of valid msgs detected */

	unsigned long vf_caps;	/* vf's adv. capabilities */
	unsigned long vf_states;	/* vf's runtime states */
	unsigned int tx_rate;	/* Tx bandwidth limit in Mbps */
	bool link_forced;
	bool link_up;		/* only valid if VF link is forced */
	bool spoofchk;
	u16 num_mac;
	u16 num_vlan;

	/* RDMA Client */
	struct virtchnl_iwarp_qvlist_info *qvlist_info;
};

void i40e_free_vfs(struct i40e_pf *pf);
int i40e_pci_sriov_configure(struct pci_dev *dev, int num_vfs);
int i40e_alloc_vfs(struct i40e_pf *pf, u16 num_alloc_vfs);
int i40e_vc_process_vf_msg(struct i40e_pf *pf, s16 vf_id, u32 v_opcode,
			   u32 v_retval, u8 *msg, u16 msglen);
int i40e_vc_process_vflr_event(struct i40e_pf *pf);
bool i40e_reset_vf(struct i40e_vf *vf, bool flr);
bool i40e_reset_all_vfs(struct i40e_pf *pf, bool flr);
void i40e_vc_notify_vf_reset(struct i40e_vf *vf);

/* VF configuration related iplink handlers */
int i40e_ndo_set_vf_mac(struct net_device *netdev, int vf_id, u8 *mac);
int i40e_ndo_set_vf_port_vlan(struct net_device *netdev, int vf_id,
			      u16 vlan_id, u8 qos, __be16 vlan_proto);
int i40e_ndo_set_vf_bw(struct net_device *netdev, int vf_id, int min_tx_rate,
		       int max_tx_rate);
int i40e_ndo_set_vf_trust(struct net_device *netdev, int vf_id, bool setting);
int i40e_ndo_get_vf_config(struct net_device *netdev,
			   int vf_id, struct ifla_vf_info *ivi);
int i40e_ndo_set_vf_link_state(struct net_device *netdev, int vf_id, int link);
int i40e_ndo_set_vf_spoofchk(struct net_device *netdev, int vf_id, bool enable);

void i40e_vc_notify_link_state(struct i40e_pf *pf);
void i40e_vc_notify_reset(struct i40e_pf *pf);

#endif /* _I40E_VIRTCHNL_PF_H_ */
