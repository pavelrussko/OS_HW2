/*******************************************************************************
 *
 * Intel Ethernet Controller XL710 Family Linux Driver
 * Copyright(c) 2013 - 2016 Intel Corporation.
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

#include "i40e.h"

/*********************notification routines***********************/

/**
 * i40e_vc_vf_broadcast
 * @pf: pointer to the PF structure
 * @opcode: operation code
 * @retval: return value
 * @msg: pointer to the msg buffer
 * @msglen: msg length
 *
 * send a message to all VFs on a given PF
 **/
static void i40e_vc_vf_broadcast(struct i40e_pf *pf,
				 enum virtchnl_ops v_opcode,
				 i40e_status v_retval, u8 *msg,
				 u16 msglen)
{
	struct i40e_hw *hw = &pf->hw;
	struct i40e_vf *vf = pf->vf;
	int i;

	for (i = 0; i < pf->num_alloc_vfs; i++, vf++) {
		int abs_vf_id = vf->vf_id + (int)hw->func_caps.vf_base_id;
		/* Not all vfs are enabled so skip the ones that are not */
		if (!test_bit(I40E_VF_STATE_INIT, &vf->vf_states) &&
		    !test_bit(I40E_VF_STATE_ACTIVE, &vf->vf_states))
			continue;

		/* Ignore return value on purpose - a given VF may fail, but
		 * we need to keep going and send to all of them
		 */
		i40e_aq_send_msg_to_vf(hw, abs_vf_id, v_opcode, v_retval,
				       msg, msglen, NULL);
	}
}

/**
 * i40e_vc_notify_vf_link_state
 * @vf: pointer to the VF structure
 *
 * send a link status message to a single VF
 **/
static void i40e_vc_notify_vf_link_state(struct i40e_vf *vf)
{
	struct virtchnl_pf_event pfe;
	struct i40e_pf *pf = vf->pf;
	struct i40e_hw *hw = &pf->hw;
	struct i40e_link_status *ls = &pf->hw.phy.link_info;
	int abs_vf_id = vf->vf_id + (int)hw->func_caps.vf_base_id;

	pfe.event = VIRTCHNL_EVENT_LINK_CHANGE;
	pfe.severity = PF_EVENT_SEVERITY_INFO;
	if (vf->link_forced) {
		pfe.event_data.link_event.link_status = vf->link_up;
		pfe.event_data.link_event.link_speed =
			(vf->link_up ? I40E_LINK_SPEED_40GB : 0);
	} else {
		pfe.event_data.link_event.link_status =
			ls->link_info & I40E_AQ_LINK_UP;
		pfe.event_data.link_event.link_speed =
			(enum virtchnl_link_speed)ls->link_speed;
	}
	i40e_aq_send_msg_to_vf(hw, abs_vf_id, VIRTCHNL_OP_EVENT,
			       0, (u8 *)&pfe, sizeof(pfe), NULL);
}

/**
 * i40e_vc_notify_link_state
 * @pf: pointer to the PF structure
 *
 * send a link status message to all VFs on a given PF
 **/
void i40e_vc_notify_link_state(struct i40e_pf *pf)
{
	int i;

	for (i = 0; i < pf->num_alloc_vfs; i++)
		i40e_vc_notify_vf_link_state(&pf->vf[i]);
}

/**
 * i40e_vc_notify_reset
 * @pf: pointer to the PF structure
 *
 * indicate a pending reset to all VFs on a given PF
 **/
void i40e_vc_notify_reset(struct i40e_pf *pf)
{
	struct virtchnl_pf_event pfe;

	pfe.event = VIRTCHNL_EVENT_RESET_IMPENDING;
	pfe.severity = PF_EVENT_SEVERITY_CERTAIN_DOOM;
	i40e_vc_vf_broadcast(pf, VIRTCHNL_OP_EVENT, 0,
			     (u8 *)&pfe, sizeof(struct virtchnl_pf_event));
}

/**
 * i40e_vc_notify_vf_reset
 * @vf: pointer to the VF structure
 *
 * indicate a pending reset to the given VF
 **/
void i40e_vc_notify_vf_reset(struct i40e_vf *vf)
{
	struct virtchnl_pf_event pfe;
	int abs_vf_id;

	/* validate the request */
	if (!vf || vf->vf_id >= vf->pf->num_alloc_vfs)
		return;

	/* verify if the VF is in either init or active before proceeding */
	if (!test_bit(I40E_VF_STATE_INIT, &vf->vf_states) &&
	    !test_bit(I40E_VF_STATE_ACTIVE, &vf->vf_states))
		return;

	abs_vf_id = vf->vf_id + (int)vf->pf->hw.func_caps.vf_base_id;

	pfe.event = VIRTCHNL_EVENT_RESET_IMPENDING;
	pfe.severity = PF_EVENT_SEVERITY_CERTAIN_DOOM;
	i40e_aq_send_msg_to_vf(&vf->pf->hw, abs_vf_id, VIRTCHNL_OP_EVENT,
			       0, (u8 *)&pfe,
			       sizeof(struct virtchnl_pf_event), NULL);
}
/***********************misc routines*****************************/

/**
 * i40e_vc_disable_vf
 * @vf: pointer to the VF info
 *
 * Disable the VF through a SW reset.
 **/
static inline void i40e_vc_disable_vf(struct i40e_vf *vf)
{
	struct i40e_pf *pf = vf->pf;
	int i;

	i40e_vc_notify_vf_reset(vf);

	/* We want to ensure that an actual reset occurs initiated after this
	 * function was called. However, we do not want to wait forever, so
	 * we'll give a reasonable time and print a message if we failed to
	 * ensure a reset.
	 */
	for (i = 0; i < 20; i++) {
		/* If PF is in VFs releasing state reset VF is impossible,
		 * so leave it.
		 */
		if (test_bit(__I40E_VFS_RELEASING, pf->state))
			return;
		if (i40e_reset_vf(vf, false))
			return;
		usleep_range(10000, 20000);
	}

	dev_warn(&vf->pf->pdev->dev,
		 "Failed to initiate reset for VF %d after 200 milliseconds\n",
		 vf->vf_id);
}

/**
 * i40e_vc_isvalid_vsi_id
 * @vf: pointer to the VF info
 * @vsi_id: VF relative VSI id
 *
 * check for the valid VSI id
 **/
static inline bool i40e_vc_isvalid_vsi_id(struct i40e_vf *vf, u16 vsi_id)
{
	struct i40e_pf *pf = vf->pf;
	struct i40e_vsi *vsi = i40e_find_vsi_from_id(pf, vsi_id);

	return (vsi && (vsi->vf_id == vf->vf_id));
}

/**
 * i40e_vc_isvalid_queue_id
 * @vf: pointer to the VF info
 * @vsi_id: vsi id
 * @qid: vsi relative queue id
 *
 * check for the valid queue id
 **/
static inline bool i40e_vc_isvalid_queue_id(struct i40e_vf *vf, u16 vsi_id,
					    u16 qid)
{
	struct i40e_pf *pf = vf->pf;
	struct i40e_vsi *vsi = i40e_find_vsi_from_id(pf, vsi_id);

	return (vsi && (qid < vsi->alloc_queue_pairs));
}

/**
 * i40e_vc_isvalid_vector_id
 * @vf: pointer to the VF info
 * @vector_id: VF relative vector id
 *
 * check for the valid vector id
 **/
static inline bool i40e_vc_isvalid_vector_id(struct i40e_vf *vf, u32 vector_id)
{
	struct i40e_pf *pf = vf->pf;

	return vector_id < pf->hw.func_caps.num_msix_vectors_vf;
}

/***********************vf resource mgmt routines*****************/

/**
 * i40e_vc_get_pf_queue_id
 * @vf: pointer to the VF info
 * @vsi_id: id of VSI as provided by the FW
 * @vsi_queue_id: vsi relative queue id
 *
 * return PF relative queue id
 **/
static u16 i40e_vc_get_pf_queue_id(struct i40e_vf *vf, u16 vsi_id,
				   u8 vsi_queue_id)
{
	struct i40e_pf *pf = vf->pf;
	struct i40e_vsi *vsi = i40e_find_vsi_from_id(pf, vsi_id);
	u16 pf_queue_id = I40E_QUEUE_END_OF_LIST;

	if (!vsi)
		return pf_queue_id;

	if (le16_to_cpu(vsi->info.mapping_flags) &
	    I40E_AQ_VSI_QUE_MAP_NONCONTIG)
		pf_queue_id =
			le16_to_cpu(vsi->info.queue_mapping[vsi_queue_id]);
	else
		pf_queue_id = le16_to_cpu(vsi->info.queue_mapping[0]) +
			      vsi_queue_id;

	return pf_queue_id;
}

/**
 * i40e_config_irq_link_list
 * @vf: pointer to the VF info
 * @vsi_id: id of VSI as given by the FW
 * @vecmap: irq map info
 *
 * configure irq link list from the map
 **/
static void i40e_config_irq_link_list(struct i40e_vf *vf, u16 vsi_id,
				      struct virtchnl_vector_map *vecmap)
{
	unsigned long linklistmap = 0, tempmap;
	struct i40e_pf *pf = vf->pf;
	struct i40e_hw *hw = &pf->hw;
	u16 vsi_queue_id, pf_queue_id;
	enum i40e_queue_type qtype;
	u16 next_q, vector_id, size;
	u32 reg, reg_idx;
	u16 itr_idx = 0;

	vector_id = vecmap->vector_id;
	/* setup the head */
	if (0 == vector_id)
		reg_idx = I40E_VPINT_LNKLST0(vf->vf_id);
	else
		reg_idx = I40E_VPINT_LNKLSTN(
		     ((pf->hw.func_caps.num_msix_vectors_vf - 1) * vf->vf_id) +
		     (vector_id - 1));

	if (vecmap->rxq_map == 0 && vecmap->txq_map == 0) {
		/* Special case - No queues mapped on this vector */
		wr32(hw, reg_idx, I40E_VPINT_LNKLST0_FIRSTQ_INDX_MASK);
		goto irq_list_done;
	}
	tempmap = vecmap->rxq_map;
	for_each_set_bit(vsi_queue_id, &tempmap, I40E_MAX_VSI_QP) {
		linklistmap |= (BIT(I40E_VIRTCHNL_SUPPORTED_QTYPES *
				    vsi_queue_id));
	}

	tempmap = vecmap->txq_map;
	for_each_set_bit(vsi_queue_id, &tempmap, I40E_MAX_VSI_QP) {
		linklistmap |= (BIT(I40E_VIRTCHNL_SUPPORTED_QTYPES *
				     vsi_queue_id + 1));
	}

	size = I40E_MAX_VSI_QP * I40E_VIRTCHNL_SUPPORTED_QTYPES;
	next_q = find_first_bit(&linklistmap, size);
	if (unlikely(next_q == size))
		goto irq_list_done;

	vsi_queue_id = next_q / I40E_VIRTCHNL_SUPPORTED_QTYPES;
	qtype = next_q % I40E_VIRTCHNL_SUPPORTED_QTYPES;
	pf_queue_id = i40e_vc_get_pf_queue_id(vf, vsi_id, vsi_queue_id);
	reg = ((qtype << I40E_VPINT_LNKLSTN_FIRSTQ_TYPE_SHIFT) | pf_queue_id);

	wr32(hw, reg_idx, reg);

	while (next_q < size) {
		switch (qtype) {
		case I40E_QUEUE_TYPE_RX:
			reg_idx = I40E_QINT_RQCTL(pf_queue_id);
			itr_idx = vecmap->rxitr_idx;
			break;
		case I40E_QUEUE_TYPE_TX:
			reg_idx = I40E_QINT_TQCTL(pf_queue_id);
			itr_idx = vecmap->txitr_idx;
			break;
		default:
			break;
		}

		next_q = find_next_bit(&linklistmap, size, next_q + 1);
		if (next_q < size) {
			vsi_queue_id = next_q / I40E_VIRTCHNL_SUPPORTED_QTYPES;
			qtype = next_q % I40E_VIRTCHNL_SUPPORTED_QTYPES;
			pf_queue_id = i40e_vc_get_pf_queue_id(vf, vsi_id,
							      vsi_queue_id);
		} else {
			pf_queue_id = I40E_QUEUE_END_OF_LIST;
			qtype = 0;
		}

		/* format for the RQCTL & TQCTL regs is same */
		reg = (vector_id) |
		    (qtype << I40E_QINT_RQCTL_NEXTQ_TYPE_SHIFT) |
		    (pf_queue_id << I40E_QINT_RQCTL_NEXTQ_INDX_SHIFT) |
		    BIT(I40E_QINT_RQCTL_CAUSE_ENA_SHIFT) |
		    (itr_idx << I40E_QINT_RQCTL_ITR_INDX_SHIFT);
		wr32(hw, reg_idx, reg);
	}

	/* if the vf is running in polling mode and using interrupt zero,
	 * need to disable auto-mask on enabling zero interrupt for VFs.
	 */
	if ((vf->driver_caps & VIRTCHNL_VF_OFFLOAD_RX_POLLING) &&
	    (vector_id == 0)) {
		reg = rd32(hw, I40E_GLINT_CTL);
		if (!(reg & I40E_GLINT_CTL_DIS_AUTOMASK_VF0_MASK)) {
			reg |= I40E_GLINT_CTL_DIS_AUTOMASK_VF0_MASK;
			wr32(hw, I40E_GLINT_CTL, reg);
		}
	}

irq_list_done:
	i40e_flush(hw);
}

/**
 * i40e_release_iwarp_qvlist
 * @vf: pointer to the VF.
 *
 **/
static void i40e_release_iwarp_qvlist(struct i40e_vf *vf)
{
	struct i40e_pf *pf = vf->pf;
	struct virtchnl_iwarp_qvlist_info *qvlist_info = vf->qvlist_info;
	u32 msix_vf;
	u32 i;

	if (!vf->qvlist_info)
		return;

	msix_vf = pf->hw.func_caps.num_msix_vectors_vf;
	for (i = 0; i < qvlist_info->num_vectors; i++) {
		struct virtchnl_iwarp_qv_info *qv_info;
		u32 next_q_index, next_q_type;
		struct i40e_hw *hw = &pf->hw;
		u32 v_idx, reg_idx, reg;

		qv_info = &qvlist_info->qv_info[i];
		if (!qv_info)
			continue;
		v_idx = qv_info->v_idx;
		if (qv_info->ceq_idx != I40E_QUEUE_INVALID_IDX) {
			/* Figure out the queue after CEQ and make that the
			 * first queue.
			 */
			reg_idx = (msix_vf - 1) * vf->vf_id + qv_info->ceq_idx;
			reg = rd32(hw, I40E_VPINT_CEQCTL(reg_idx));
			next_q_index = (reg & I40E_VPINT_CEQCTL_NEXTQ_INDX_MASK)
					>> I40E_VPINT_CEQCTL_NEXTQ_INDX_SHIFT;
			next_q_type = (reg & I40E_VPINT_CEQCTL_NEXTQ_TYPE_MASK)
					>> I40E_VPINT_CEQCTL_NEXTQ_TYPE_SHIFT;

			reg_idx = ((msix_vf - 1) * vf->vf_id) + (v_idx - 1);
			reg = (next_q_index &
			       I40E_VPINT_LNKLSTN_FIRSTQ_INDX_MASK) |
			       (next_q_type <<
			       I40E_VPINT_LNKLSTN_FIRSTQ_TYPE_SHIFT);

			wr32(hw, I40E_VPINT_LNKLSTN(reg_idx), reg);
		}
	}
	kfree(vf->qvlist_info);
	vf->qvlist_info = NULL;
}

/**
 * i40e_config_iwarp_qvlist
 * @vf: pointer to the VF info
 * @qvlist_info: queue and vector list
 *
 * Return 0 on success or < 0 on error
 **/
static int i40e_config_iwarp_qvlist(struct i40e_vf *vf,
				    struct virtchnl_iwarp_qvlist_info *qvlist_info)
{
	struct i40e_pf *pf = vf->pf;
	struct i40e_hw *hw = &pf->hw;
	struct virtchnl_iwarp_qv_info *qv_info;
	u32 v_idx, i, reg_idx, reg;
	u32 next_q_idx, next_q_type;
	u32 msix_vf, size;
	int ret = 0;

	msix_vf = pf->hw.func_caps.num_msix_vectors_vf;

	if (qvlist_info->num_vectors > msix_vf) {
		dev_warn(&pf->pdev->dev,
			 "Incorrect number of iwarp vectors %u. Maximum %u allowed.\n",
			 qvlist_info->num_vectors,
			 msix_vf);
		ret = -EINVAL;
		goto err_out;
	}

	size = sizeof(struct virtchnl_iwarp_qvlist_info) +
	       (sizeof(struct virtchnl_iwarp_qv_info) *
						(qvlist_info->num_vectors - 1));
	kfree(vf->qvlist_info);
	vf->qvlist_info = kzalloc(size, GFP_KERNEL);
	if (!vf->qvlist_info) {
		ret = -ENOMEM;
		goto err_out;
	}
	vf->qvlist_info->num_vectors = qvlist_info->num_vectors;

	msix_vf = pf->hw.func_caps.num_msix_vectors_vf;
	for (i = 0; i < qvlist_info->num_vectors; i++) {
		qv_info = &qvlist_info->qv_info[i];
		if (!qv_info)
			continue;
		v_idx = qv_info->v_idx;

		/* Validate vector id belongs to this vf */
		if (!i40e_vc_isvalid_vector_id(vf, v_idx)) {
			ret = -EINVAL;
			goto err_free;
		}

		vf->qvlist_info->qv_info[i] = *qv_info;

		reg_idx = ((msix_vf - 1) * vf->vf_id) + (v_idx - 1);
		/* We might be sharing the interrupt, so get the first queue
		 * index and type, push it down the list by adding the new
		 * queue on top. Also link it with the new queue in CEQCTL.
		 */
		reg = rd32(hw, I40E_VPINT_LNKLSTN(reg_idx));
		next_q_idx = ((reg & I40E_VPINT_LNKLSTN_FIRSTQ_INDX_MASK) >>
				I40E_VPINT_LNKLSTN_FIRSTQ_INDX_SHIFT);
		next_q_type = ((reg & I40E_VPINT_LNKLSTN_FIRSTQ_TYPE_MASK) >>
				I40E_VPINT_LNKLSTN_FIRSTQ_TYPE_SHIFT);

		if (qv_info->ceq_idx != I40E_QUEUE_INVALID_IDX) {
			reg_idx = (msix_vf - 1) * vf->vf_id + qv_info->ceq_idx;
			reg = (I40E_VPINT_CEQCTL_CAUSE_ENA_MASK |
			(v_idx << I40E_VPINT_CEQCTL_MSIX_INDX_SHIFT) |
			(qv_info->itr_idx << I40E_VPINT_CEQCTL_ITR_INDX_SHIFT) |
			(next_q_type << I40E_VPINT_CEQCTL_NEXTQ_TYPE_SHIFT) |
			(next_q_idx << I40E_VPINT_CEQCTL_NEXTQ_INDX_SHIFT));
			wr32(hw, I40E_VPINT_CEQCTL(reg_idx), reg);

			reg_idx = ((msix_vf - 1) * vf->vf_id) + (v_idx - 1);
			reg = (qv_info->ceq_idx &
			       I40E_VPINT_LNKLSTN_FIRSTQ_INDX_MASK) |
			       (I40E_QUEUE_TYPE_PE_CEQ <<
			       I40E_VPINT_LNKLSTN_FIRSTQ_TYPE_SHIFT);
			wr32(hw, I40E_VPINT_LNKLSTN(reg_idx), reg);
		}

		if (qv_info->aeq_idx != I40E_QUEUE_INVALID_IDX) {
			reg = (I40E_VPINT_AEQCTL_CAUSE_ENA_MASK |
			(v_idx << I40E_VPINT_AEQCTL_MSIX_INDX_SHIFT) |
			(qv_info->itr_idx << I40E_VPINT_AEQCTL_ITR_INDX_SHIFT));

			wr32(hw, I40E_VPINT_AEQCTL(vf->vf_id), reg);
		}
	}

	return 0;
err_free:
	kfree(vf->qvlist_info);
	vf->qvlist_info = NULL;
err_out:
	return ret;
}

/**
 * i40e_config_vsi_tx_queue
 * @vf: pointer to the VF info
 * @vsi_id: id of VSI as provided by the FW
 * @vsi_queue_id: vsi relative queue index
 * @info: config. info
 *
 * configure tx queue
 **/
static int i40e_config_vsi_tx_queue(struct i40e_vf *vf, u16 vsi_id,
				    u16 vsi_queue_id,
				    struct virtchnl_txq_info *info)
{
	struct i40e_pf *pf = vf->pf;
	struct i40e_hw *hw = &pf->hw;
	struct i40e_hmc_obj_txq tx_ctx;
	struct i40e_vsi *vsi;
	u16 pf_queue_id;
	u32 qtx_ctl;
	int ret = 0;

	if (!i40e_vc_isvalid_vsi_id(vf, info->vsi_id)) {
		ret = -ENOENT;
		goto error_context;
	}
	pf_queue_id = i40e_vc_get_pf_queue_id(vf, vsi_id, vsi_queue_id);
	vsi = i40e_find_vsi_from_id(pf, vsi_id);
	if (!vsi) {
		ret = -ENOENT;
		goto error_context;
	}

	/* clear the context structure first */
	memset(&tx_ctx, 0, sizeof(struct i40e_hmc_obj_txq));

	/* only set the required fields */
	tx_ctx.base = info->dma_ring_addr / 128;
	tx_ctx.qlen = info->ring_len;
	tx_ctx.rdylist = le16_to_cpu(vsi->info.qs_handle[0]);
	tx_ctx.rdylist_act = 0;
	tx_ctx.head_wb_ena = info->headwb_enabled;
	tx_ctx.head_wb_addr = info->dma_headwb_addr;

	/* clear the context in the HMC */
	ret = i40e_clear_lan_tx_queue_context(hw, pf_queue_id);
	if (ret) {
		dev_err(&pf->pdev->dev,
			"Failed to clear VF LAN Tx queue context %d, error: %d\n",
			pf_queue_id, ret);
		ret = -ENOENT;
		goto error_context;
	}

	/* set the context in the HMC */
	ret = i40e_set_lan_tx_queue_context(hw, pf_queue_id, &tx_ctx);
	if (ret) {
		dev_err(&pf->pdev->dev,
			"Failed to set VF LAN Tx queue context %d error: %d\n",
			pf_queue_id, ret);
		ret = -ENOENT;
		goto error_context;
	}

	/* associate this queue with the PCI VF function */
	qtx_ctl = I40E_QTX_CTL_VF_QUEUE;
	qtx_ctl |= ((hw->pf_id << I40E_QTX_CTL_PF_INDX_SHIFT)
		    & I40E_QTX_CTL_PF_INDX_MASK);
	qtx_ctl |= (((vf->vf_id + hw->func_caps.vf_base_id)
		     << I40E_QTX_CTL_VFVM_INDX_SHIFT)
		    & I40E_QTX_CTL_VFVM_INDX_MASK);
	wr32(hw, I40E_QTX_CTL(pf_queue_id), qtx_ctl);
	i40e_flush(hw);

error_context:
	return ret;
}

/**
 * i40e_config_vsi_rx_queue
 * @vf: pointer to the VF info
 * @vsi_id: id of VSI  as provided by the FW
 * @vsi_queue_id: vsi relative queue index
 * @info: config. info
 *
 * configure rx queue
 **/
static int i40e_config_vsi_rx_queue(struct i40e_vf *vf, u16 vsi_id,
				    u16 vsi_queue_id,
				    struct virtchnl_rxq_info *info)
{
	u16 pf_queue_id = i40e_vc_get_pf_queue_id(vf, vsi_id, vsi_queue_id);
	struct i40e_pf *pf = vf->pf;
	struct i40e_vsi *vsi = pf->vsi[vf->lan_vsi_idx];
	struct i40e_hw *hw = &pf->hw;
	struct i40e_hmc_obj_rxq rx_ctx;
	int ret = 0;

	/* clear the context structure first */
	memset(&rx_ctx, 0, sizeof(struct i40e_hmc_obj_rxq));

	/* only set the required fields */
	rx_ctx.base = info->dma_ring_addr / 128;
	rx_ctx.qlen = info->ring_len;

	if (info->splithdr_enabled) {
		rx_ctx.hsplit_0 = I40E_RX_SPLIT_L2      |
				  I40E_RX_SPLIT_IP      |
				  I40E_RX_SPLIT_TCP_UDP |
				  I40E_RX_SPLIT_SCTP;
		/* header length validation */
		if (info->hdr_size > ((2 * 1024) - 64)) {
			ret = -EINVAL;
			goto error_param;
		}
		rx_ctx.hbuff = info->hdr_size >> I40E_RXQ_CTX_HBUFF_SHIFT;

		/* set split mode 10b */
		rx_ctx.dtype = I40E_RX_DTYPE_HEADER_SPLIT;
	}

	/* databuffer length validation */
	if (info->databuffer_size > ((16 * 1024) - 128)) {
		ret = -EINVAL;
		goto error_param;
	}
	rx_ctx.dbuff = info->databuffer_size >> I40E_RXQ_CTX_DBUFF_SHIFT;

	/* max pkt. length validation */
	if (info->max_pkt_size >= (16 * 1024) || info->max_pkt_size < 64) {
		ret = -EINVAL;
		goto error_param;
	}
	rx_ctx.rxmax = info->max_pkt_size;

	/* if port VLAN is configured increase the max packet size */
	if (vsi->info.pvid)
		rx_ctx.rxmax += VLAN_HLEN;

	/* enable 32bytes desc always */
	rx_ctx.dsize = 1;

	/* default values */
	rx_ctx.lrxqthresh = 1;
	rx_ctx.crcstrip = 1;
	rx_ctx.prefena = 1;
	rx_ctx.l2tsel = 1;

	/* clear the context in the HMC */
	ret = i40e_clear_lan_rx_queue_context(hw, pf_queue_id);
	if (ret) {
		dev_err(&pf->pdev->dev,
			"Failed to clear VF LAN Rx queue context %d, error: %d\n",
			pf_queue_id, ret);
		ret = -ENOENT;
		goto error_param;
	}

	/* set the context in the HMC */
	ret = i40e_set_lan_rx_queue_context(hw, pf_queue_id, &rx_ctx);
	if (ret) {
		dev_err(&pf->pdev->dev,
			"Failed to set VF LAN Rx queue context %d error: %d\n",
			pf_queue_id, ret);
		ret = -ENOENT;
		goto error_param;
	}

error_param:
	return ret;
}

/**
 * i40e_alloc_vsi_res
 * @vf: pointer to the VF info
 * @type: type of VSI to allocate
 *
 * alloc VF vsi context & resources
 **/
static int i40e_alloc_vsi_res(struct i40e_vf *vf, enum i40e_vsi_type type)
{
	struct i40e_mac_filter *f = NULL;
	struct i40e_pf *pf = vf->pf;
	struct i40e_vsi *vsi;
	int ret = 0;

	vsi = i40e_vsi_setup(pf, type, pf->vsi[pf->lan_vsi]->seid, vf->vf_id);

	if (!vsi) {
		dev_err(&pf->pdev->dev,
			"add vsi failed for VF %d, aq_err %d\n",
			vf->vf_id, pf->hw.aq.asq_last_status);
		ret = -ENOENT;
		goto error_alloc_vsi_res;
	}
	if (type == I40E_VSI_SRIOV) {
		u64 hena = i40e_pf_get_default_rss_hena(pf);
		u8 broadcast[ETH_ALEN];

		vf->lan_vsi_idx = vsi->idx;
		vf->lan_vsi_id = vsi->id;
		/* If the port VLAN has been configured and then the
		 * VF driver was removed then the VSI port VLAN
		 * configuration was destroyed.  Check if there is
		 * a port VLAN and restore the VSI configuration if
		 * needed.
		 */
		if (vf->port_vlan_id)
			i40e_vsi_add_pvid(vsi, vf->port_vlan_id);

		spin_lock_bh(&vsi->mac_filter_hash_lock);
		if (is_valid_ether_addr(vf->default_lan_addr.addr)) {
			f = i40e_add_mac_filter(vsi,
						vf->default_lan_addr.addr);
			if (!f)
				dev_info(&pf->pdev->dev,
					 "Could not add MAC filter %pM for VF %d\n",
					vf->default_lan_addr.addr, vf->vf_id);
		}
		eth_broadcast_addr(broadcast);
		f = i40e_add_mac_filter(vsi, broadcast);
		if (!f)
			dev_info(&pf->pdev->dev,
				 "Could not allocate VF broadcast filter\n");
		spin_unlock_bh(&vsi->mac_filter_hash_lock);
		wr32(&pf->hw, I40E_VFQF_HENA1(0, vf->vf_id), (u32)hena);
		wr32(&pf->hw, I40E_VFQF_HENA1(1, vf->vf_id), (u32)(hena >> 32));
	}

	/* program mac filter */
	ret = i40e_sync_vsi_filters(vsi);
	if (ret)
		dev_err(&pf->pdev->dev, "Unable to program ucast filters\n");

	/* Set VF bandwidth if specified */
	if (vf->tx_rate) {
		ret = i40e_aq_config_vsi_bw_limit(&pf->hw, vsi->seid,
						  vf->tx_rate / 50, 0, NULL);
		if (ret)
			dev_err(&pf->pdev->dev, "Unable to set tx rate, VF %d, error code %d.\n",
				vf->vf_id, ret);
	}

error_alloc_vsi_res:
	return ret;
}

/**
 * i40e_enable_vf_mappings
 * @vf: pointer to the VF info
 *
 * enable VF mappings
 **/
static void i40e_enable_vf_mappings(struct i40e_vf *vf)
{
	struct i40e_pf *pf = vf->pf;
	struct i40e_hw *hw = &pf->hw;
	u32 reg, total_queue_pairs = 0;
	int j;

	/* Tell the hardware we're using noncontiguous mapping. HW requires
	 * that VF queues be mapped using this method, even when they are
	 * contiguous in real life
	 */
	i40e_write_rx_ctl(hw, I40E_VSILAN_QBASE(vf->lan_vsi_id),
			  I40E_VSILAN_QBASE_VSIQTABLE_ENA_MASK);

	/* enable VF vplan_qtable mappings */
	reg = I40E_VPLAN_MAPENA_TXRX_ENA_MASK;
	wr32(hw, I40E_VPLAN_MAPENA(vf->vf_id), reg);

	/* map PF queues to VF queues */
	for (j = 0; j < pf->vsi[vf->lan_vsi_idx]->alloc_queue_pairs; j++) {
		u16 qid = i40e_vc_get_pf_queue_id(vf, vf->lan_vsi_id, j);

		reg = (qid & I40E_VPLAN_QTABLE_QINDEX_MASK);
		wr32(hw, I40E_VPLAN_QTABLE(total_queue_pairs, vf->vf_id), reg);
		total_queue_pairs++;
	}

	/* map PF queues to VSI */
	for (j = 0; j < 7; j++) {
		if (j * 2 >= pf->vsi[vf->lan_vsi_idx]->alloc_queue_pairs) {
			reg = 0x07FF07FF;	/* unused */
		} else {
			u16 qid = i40e_vc_get_pf_queue_id(vf, vf->lan_vsi_id,
							  j * 2);
			reg = qid;
			qid = i40e_vc_get_pf_queue_id(vf, vf->lan_vsi_id,
						      (j * 2) + 1);
			reg |= qid << 16;
		}
		i40e_write_rx_ctl(hw, I40E_VSILAN_QTABLE(j, vf->lan_vsi_id),
				  reg);
	}

	i40e_flush(hw);
}

/**
 * i40e_disable_vf_mappings
 * @vf: pointer to the VF info
 *
 * disable VF mappings
 **/
static void i40e_disable_vf_mappings(struct i40e_vf *vf)
{
	struct i40e_pf *pf = vf->pf;
	struct i40e_hw *hw = &pf->hw;
	int i;

	/* disable qp mappings */
	wr32(hw, I40E_VPLAN_MAPENA(vf->vf_id), 0);
	for (i = 0; i < I40E_MAX_VSI_QP; i++)
		wr32(hw, I40E_VPLAN_QTABLE(i, vf->vf_id),
		     I40E_QUEUE_END_OF_LIST);
	i40e_flush(hw);
}

/**
 * i40e_free_vf_res
 * @vf: pointer to the VF info
 *
 * free VF resources
 **/
static void i40e_free_vf_res(struct i40e_vf *vf)
{
	struct i40e_pf *pf = vf->pf;
	struct i40e_hw *hw = &pf->hw;
	u32 reg_idx, reg;
	int i, msix_vf;

	/* Start by disabling VF's configuration API to prevent the OS from
	 * accessing the VF's VSI after it's freed / invalidated.
	 */
	clear_bit(I40E_VF_STATE_INIT, &vf->vf_states);

	/* It's possible the VF had requeuested more queues than the default so
	 * do the accounting here when we're about to free them.
	 */
	if (vf->num_queue_pairs > I40E_DEFAULT_QUEUES_PER_VF) {
		pf->queues_left += vf->num_queue_pairs -
				   I40E_DEFAULT_QUEUES_PER_VF;
	}

	/* free vsi & disconnect it from the parent uplink */
	if (vf->lan_vsi_idx) {
		i40e_vsi_release(pf->vsi[vf->lan_vsi_idx]);
		vf->lan_vsi_idx = 0;
		vf->lan_vsi_id = 0;
		vf->num_mac = 0;
	}
	msix_vf = pf->hw.func_caps.num_msix_vectors_vf;

	/* disable interrupts so the VF starts in a known state */
	for (i = 0; i < msix_vf; i++) {
		/* format is same for both registers */
		if (0 == i)
			reg_idx = I40E_VFINT_DYN_CTL0(vf->vf_id);
		else
			reg_idx = I40E_VFINT_DYN_CTLN(((msix_vf - 1) *
						      (vf->vf_id))
						     + (i - 1));
		wr32(hw, reg_idx, I40E_VFINT_DYN_CTLN_CLEARPBA_MASK);
		i40e_flush(hw);
	}

	/* clear the irq settings */
	for (i = 0; i < msix_vf; i++) {
		/* format is same for both registers */
		if (0 == i)
			reg_idx = I40E_VPINT_LNKLST0(vf->vf_id);
		else
			reg_idx = I40E_VPINT_LNKLSTN(((msix_vf - 1) *
						      (vf->vf_id))
						     + (i - 1));
		reg = (I40E_VPINT_LNKLSTN_FIRSTQ_TYPE_MASK |
		       I40E_VPINT_LNKLSTN_FIRSTQ_INDX_MASK);
		wr32(hw, reg_idx, reg);
		i40e_flush(hw);
	}
	/* reset some of the state variables keeping track of the resources */
	vf->num_queue_pairs = 0;
	clear_bit(I40E_VF_STATE_MC_PROMISC, &vf->vf_states);
	clear_bit(I40E_VF_STATE_UC_PROMISC, &vf->vf_states);
}

/**
 * i40e_alloc_vf_res
 * @vf: pointer to the VF info
 *
 * allocate VF resources
 **/
static int i40e_alloc_vf_res(struct i40e_vf *vf)
{
	struct i40e_pf *pf = vf->pf;
	int total_queue_pairs = 0;
	int ret;

	if (vf->num_req_queues &&
	    vf->num_req_queues <= pf->queues_left + I40E_DEFAULT_QUEUES_PER_VF)
		pf->num_vf_qps = vf->num_req_queues;
	else
		pf->num_vf_qps = I40E_DEFAULT_QUEUES_PER_VF;

	/* allocate hw vsi context & associated resources */
	ret = i40e_alloc_vsi_res(vf, I40E_VSI_SRIOV);
	if (ret)
		goto error_alloc;
	total_queue_pairs += pf->vsi[vf->lan_vsi_idx]->alloc_queue_pairs;

	/* We account for each VF to get a default number of queue pairs.  If
	 * the VF has now requested more, we need to account for that to make
	 * certain we never request more queues than we actually have left in
	 * HW.
	 */
	if (total_queue_pairs > I40E_DEFAULT_QUEUES_PER_VF)
		pf->queues_left -=
			total_queue_pairs - I40E_DEFAULT_QUEUES_PER_VF;

	if (vf->trusted)
		set_bit(I40E_VIRTCHNL_VF_CAP_PRIVILEGE, &vf->vf_caps);
	else
		clear_bit(I40E_VIRTCHNL_VF_CAP_PRIVILEGE, &vf->vf_caps);

	/* store the total qps number for the runtime
	 * VF req validation
	 */
	vf->num_queue_pairs = total_queue_pairs;

	/* VF is now completely initialized */
	set_bit(I40E_VF_STATE_INIT, &vf->vf_states);

error_alloc:
	if (ret)
		i40e_free_vf_res(vf);

	return ret;
}

#define VF_DEVICE_STATUS 0xAA
#define VF_TRANS_PENDING_MASK 0x20
/**
 * i40e_quiesce_vf_pci
 * @vf: pointer to the VF structure
 *
 * Wait for VF PCI transactions to be cleared after reset. Returns -EIO
 * if the transactions never clear.
 **/
static int i40e_quiesce_vf_pci(struct i40e_vf *vf)
{
	struct i40e_pf *pf = vf->pf;
	struct i40e_hw *hw = &pf->hw;
	int vf_abs_id, i;
	u32 reg;

	vf_abs_id = vf->vf_id + hw->func_caps.vf_base_id;

	wr32(hw, I40E_PF_PCI_CIAA,
	     VF_DEVICE_STATUS | (vf_abs_id << I40E_PF_PCI_CIAA_VF_NUM_SHIFT));
	for (i = 0; i < 100; i++) {
		reg = rd32(hw, I40E_PF_PCI_CIAD);
		if ((reg & VF_TRANS_PENDING_MASK) == 0)
			return 0;
		udelay(1);
	}
	return -EIO;
}

/**
 * i40e_trigger_vf_reset
 * @vf: pointer to the VF structure
 * @flr: VFLR was issued or not
 *
 * Trigger hardware to start a reset for a particular VF. Expects the caller
 * to wait the proper amount of time to allow hardware to reset the VF before
 * it cleans up and restores VF functionality.
 **/
static void i40e_trigger_vf_reset(struct i40e_vf *vf, bool flr)
{
	struct i40e_pf *pf = vf->pf;
	struct i40e_hw *hw = &pf->hw;
	u32 reg, reg_idx, bit_idx;

	/* warn the VF */
	clear_bit(I40E_VF_STATE_ACTIVE, &vf->vf_states);

	/* Disable VF's configuration API during reset. The flag is re-enabled
	 * in i40e_alloc_vf_res(), when it's safe again to access VF's VSI.
	 * It's normally disabled in i40e_free_vf_res(), but it's safer
	 * to do it earlier to give some time to finish to any VF config
	 * functions that may still be running at this point.
	 */
	clear_bit(I40E_VF_STATE_INIT, &vf->vf_states);

	/* In the case of a VFLR, the HW has already reset the VF and we
	 * just need to clean up, so don't hit the VFRTRIG register.
	 */
	if (!flr) {
		/* reset VF using VPGEN_VFRTRIG reg */
		reg = rd32(hw, I40E_VPGEN_VFRTRIG(vf->vf_id));
		reg |= I40E_VPGEN_VFRTRIG_VFSWR_MASK;
		wr32(hw, I40E_VPGEN_VFRTRIG(vf->vf_id), reg);
		i40e_flush(hw);
	}
	/* clear the VFLR bit in GLGEN_VFLRSTAT */
	reg_idx = (hw->func_caps.vf_base_id + vf->vf_id) / 32;
	bit_idx = (hw->func_caps.vf_base_id + vf->vf_id) % 32;
	wr32(hw, I40E_GLGEN_VFLRSTAT(reg_idx), BIT(bit_idx));
	i40e_flush(hw);

	if (i40e_quiesce_vf_pci(vf))
		dev_err(&pf->pdev->dev, "VF %d PCI transactions stuck\n",
			vf->vf_id);
}

/**
 * i40e_cleanup_reset_vf
 * @vf: pointer to the VF structure
 *
 * Cleanup a VF after the hardware reset is finished. Expects the caller to
 * have verified whether the reset is finished properly, and ensure the
 * minimum amount of wait time has passed.
 **/
static void i40e_cleanup_reset_vf(struct i40e_vf *vf)
{
	struct i40e_pf *pf = vf->pf;
	struct i40e_hw *hw = &pf->hw;
	u32 reg;

	/* free VF resources to begin resetting the VSI state */
	i40e_free_vf_res(vf);

	/* Enable hardware by clearing the reset bit in the VPGEN_VFRTRIG reg.
	 * By doing this we allow HW to access VF memory at any point. If we
	 * did it any sooner, HW could access memory while it was being freed
	 * in i40e_free_vf_res(), causing an IOMMU fault.
	 *
	 * On the other hand, this needs to be done ASAP, because the VF driver
	 * is waiting for this to happen and may report a timeout. It's
	 * harmless, but it gets logged into Guest OS kernel log, so best avoid
	 * it.
	 */
	reg = rd32(hw, I40E_VPGEN_VFRTRIG(vf->vf_id));
	reg &= ~I40E_VPGEN_VFRTRIG_VFSWR_MASK;
	wr32(hw, I40E_VPGEN_VFRTRIG(vf->vf_id), reg);

	/* reallocate VF resources to finish resetting the VSI state */
	if (!i40e_alloc_vf_res(vf)) {
		int abs_vf_id = vf->vf_id + hw->func_caps.vf_base_id;
		i40e_enable_vf_mappings(vf);
		set_bit(I40E_VF_STATE_ACTIVE, &vf->vf_states);
		clear_bit(I40E_VF_STATE_DISABLED, &vf->vf_states);
		/* Do not notify the client during VF init */
		if (!test_and_clear_bit(I40E_VF_STATE_PRE_ENABLE,
					&vf->vf_states))
			i40e_notify_client_of_vf_reset(pf, abs_vf_id);
		vf->num_vlan = 0;
	}

	/* Tell the VF driver the reset is done. This needs to be done only
	 * after VF has been fully initialized, because the VF driver may
	 * request resources immediately after setting this flag.
	 */
	wr32(hw, I40E_VFGEN_RSTAT1(vf->vf_id), VIRTCHNL_VFR_VFACTIVE);
}

/**
 * i40e_reset_vf
 * @vf: pointer to the VF structure
 * @flr: VFLR was issued or not
 *
 * Returns true if the VF is in reset, resets successfully, or resets
 * are disabled and false otherwise.
 **/
bool i40e_reset_vf(struct i40e_vf *vf, bool flr)
{
	struct i40e_pf *pf = vf->pf;
	struct i40e_hw *hw = &pf->hw;
	bool rsd = false;
	u32 reg;
	int i;

	if (test_bit(__I40E_VF_RESETS_DISABLED, pf->state))
		return true;

	/* Bail out if VFs are disabled. */
	if (test_bit(__I40E_VF_DISABLE, pf->state))
		return true;

	/* If VF is being reset already we don't need to continue. */
	if (test_and_set_bit(I40E_VF_STATE_RESETTING, &vf->vf_states))
		return true;

	i40e_trigger_vf_reset(vf, flr);

	/* poll VPGEN_VFRSTAT reg to make sure
	 * that reset is complete
	 */
	for (i = 0; i < 10; i++) {
		/* VF reset requires driver to first reset the VF and then
		 * poll the status register to make sure that the reset
		 * completed successfully. Due to internal HW FIFO flushes,
		 * we must wait 10ms before the register will be valid.
		 */
		usleep_range(10000, 20000);
		reg = rd32(hw, I40E_VPGEN_VFRSTAT(vf->vf_id));
		if (reg & I40E_VPGEN_VFRSTAT_VFRD_MASK) {
			rsd = true;
			break;
		}
	}

	if (flr)
		usleep_range(10000, 20000);

	if (!rsd)
		dev_err(&pf->pdev->dev, "VF reset check timeout on VF %d\n",
			vf->vf_id);
	usleep_range(10000, 20000);

	/* On initial reset, we don't have any queues to disable */
	if (vf->lan_vsi_idx != 0)
		i40e_vsi_stop_rings(pf->vsi[vf->lan_vsi_idx]);

	i40e_cleanup_reset_vf(vf);

	i40e_flush(hw);
	usleep_range(20000, 40000);
	clear_bit(I40E_VF_STATE_RESETTING, &vf->vf_states);

	return true;
}

/**
 * i40e_reset_all_vfs
 * @pf: pointer to the PF structure
 * @flr: VFLR was issued or not
 *
 * Reset all allocated VFs in one go. First, tell the hardware to reset each
 * VF, then do all the waiting in one chunk, and finally finish restoring each
 * VF after the wait. This is useful during PF routines which need to reset
 * all VFs, as otherwise it must perform these resets in a serialized fashion.
 *
 * Returns true if any VFs were reset, and false otherwise.
 **/
bool i40e_reset_all_vfs(struct i40e_pf *pf, bool flr)
{
	struct i40e_hw *hw = &pf->hw;
	struct i40e_vf *vf;
	int i, v;
	u32 reg;

	/* If we don't have any VFs, then there is nothing to reset */
	if (!pf->num_alloc_vfs)
		return false;

	/* If VFs have been disabled, there is no need to reset */
	if (test_and_set_bit(__I40E_VF_DISABLE, pf->state))
		return false;

	/* Begin reset on all VFs at once */
	for (v = 0; v < pf->num_alloc_vfs; v++) {
		vf = &pf->vf[v];
		/* If VF is being reset no need to trigger reset again */
		if (!test_bit(I40E_VF_STATE_RESETTING, &vf->vf_states))
			i40e_trigger_vf_reset(&pf->vf[v], flr);
	}

	/* HW requires some time to make sure it can flush the FIFO for a VF
	 * when it resets it. Poll the VPGEN_VFRSTAT register for each VF in
	 * sequence to make sure that it has completed. We'll keep track of
	 * the VFs using a simple iterator that increments once that VF has
	 * finished resetting.
	 */
	for (i = 0, v = 0; i < 10 && v < pf->num_alloc_vfs; i++) {
		usleep_range(10000, 20000);

		/* Check each VF in sequence, beginning with the VF to fail
		 * the previous check.
		 */
		while (v < pf->num_alloc_vfs) {
			vf = &pf->vf[v];
			if (!test_bit(I40E_VF_STATE_RESETTING, &vf->vf_states)) {
				reg = rd32(hw, I40E_VPGEN_VFRSTAT(vf->vf_id));
				if (!(reg & I40E_VPGEN_VFRSTAT_VFRD_MASK))
					break;
			}

			/* If the current VF has finished resetting, move on
			 * to the next VF in sequence.
			 */
			v++;
		}
	}

	if (flr)
		usleep_range(10000, 20000);

	/* Display a warning if at least one VF didn't manage to reset in
	 * time, but continue on with the operation.
	 */
	if (v < pf->num_alloc_vfs)
		dev_err(&pf->pdev->dev, "VF reset check timeout on VF %d\n",
			pf->vf[v].vf_id);
	usleep_range(10000, 20000);

	/* Begin disabling all the rings associated with VFs, but do not wait
	 * between each VF.
	 */
	for (v = 0; v < pf->num_alloc_vfs; v++) {
		/* On initial reset, we don't have any queues to disable */
		if (pf->vf[v].lan_vsi_idx == 0)
			continue;

		/* If VF is reset in another thread just continue */
		if (test_bit(I40E_VF_STATE_RESETTING, &vf->vf_states))
			continue;

		i40e_vsi_stop_rings_no_wait(pf->vsi[pf->vf[v].lan_vsi_idx]);
	}

	/* Now that we've notified HW to disable all of the VF rings, wait
	 * until they finish.
	 */
	for (v = 0; v < pf->num_alloc_vfs; v++) {
		/* On initial reset, we don't have any queues to disable */
		if (pf->vf[v].lan_vsi_idx == 0)
			continue;

		/* If VF is reset in another thread just continue */
		if (test_bit(I40E_VF_STATE_RESETTING, &vf->vf_states))
			continue;

		i40e_vsi_wait_queues_disabled(pf->vsi[pf->vf[v].lan_vsi_idx]);
	}

	/* Hw may need up to 50ms to finish disabling the RX queues. We
	 * minimize the wait by delaying only once for all VFs.
	 */
	mdelay(50);

	/* Finish the reset on each VF */
	for (v = 0; v < pf->num_alloc_vfs; v++) {
		/* If VF is reset in another thread just continue */
		if (test_bit(I40E_VF_STATE_RESETTING, &vf->vf_states))
			continue;

		i40e_cleanup_reset_vf(&pf->vf[v]);
	}

	i40e_flush(hw);
	usleep_range(20000, 40000);
	clear_bit(__I40E_VF_DISABLE, pf->state);

	return true;
}

/**
 * i40e_free_vfs
 * @pf: pointer to the PF structure
 *
 * free VF resources
 **/
void i40e_free_vfs(struct i40e_pf *pf)
{
	struct i40e_hw *hw = &pf->hw;
	u32 reg_idx, bit_idx;
	int i, tmp, vf_id;

	if (!pf->vf)
		return;

	set_bit(__I40E_VFS_RELEASING, pf->state);
	while (test_and_set_bit(__I40E_VF_DISABLE, pf->state))
		usleep_range(1000, 2000);

	i40e_notify_client_of_vf_enable(pf, 0);

	/* Disable IOV before freeing resources. This lets any VF drivers
	 * running in the host get themselves cleaned up before we yank
	 * the carpet out from underneath their feet.
	 */
	if (!pci_vfs_assigned(pf->pdev))
		pci_disable_sriov(pf->pdev);
	else
		dev_warn(&pf->pdev->dev, "VFs are assigned - not disabling SR-IOV\n");

	/* Amortize wait time by stopping all VFs at the same time */
	for (i = 0; i < pf->num_alloc_vfs; i++) {
		if (test_bit(I40E_VF_STATE_INIT, &pf->vf[i].vf_states))
			continue;

		i40e_vsi_stop_rings_no_wait(pf->vsi[pf->vf[i].lan_vsi_idx]);
	}

	for (i = 0; i < pf->num_alloc_vfs; i++) {
		if (test_bit(I40E_VF_STATE_INIT, &pf->vf[i].vf_states))
			continue;

		i40e_vsi_wait_queues_disabled(pf->vsi[pf->vf[i].lan_vsi_idx]);
	}

	/* free up VF resources */
	tmp = pf->num_alloc_vfs;
	pf->num_alloc_vfs = 0;
	for (i = 0; i < tmp; i++) {
		if (test_bit(I40E_VF_STATE_INIT, &pf->vf[i].vf_states))
			i40e_free_vf_res(&pf->vf[i]);
		/* disable qp mappings */
		i40e_disable_vf_mappings(&pf->vf[i]);
	}

	kfree(pf->vf);
	pf->vf = NULL;

	/* This check is for when the driver is unloaded while VFs are
	 * assigned. Setting the number of VFs to 0 through sysfs is caught
	 * before this function ever gets called.
	 */
	if (!pci_vfs_assigned(pf->pdev)) {
		/* Acknowledge VFLR for all VFS. Without this, VFs will fail to
		 * work correctly when SR-IOV gets re-enabled.
		 */
		for (vf_id = 0; vf_id < tmp; vf_id++) {
			reg_idx = (hw->func_caps.vf_base_id + vf_id) / 32;
			bit_idx = (hw->func_caps.vf_base_id + vf_id) % 32;
			wr32(hw, I40E_GLGEN_VFLRSTAT(reg_idx), BIT(bit_idx));
		}
	}
	clear_bit(__I40E_VF_DISABLE, pf->state);
	clear_bit(__I40E_VFS_RELEASING, pf->state);
}

#ifdef CONFIG_PCI_IOV
/**
 * i40e_alloc_vfs
 * @pf: pointer to the PF structure
 * @num_alloc_vfs: number of VFs to allocate
 *
 * allocate VF resources
 **/
int i40e_alloc_vfs(struct i40e_pf *pf, u16 num_alloc_vfs)
{
	struct i40e_vf *vfs;
	int i, ret = 0;

	/* Disable interrupt 0 so we don't try to handle the VFLR. */
	i40e_irq_dynamic_disable_icr0(pf);

	/* Check to see if we're just allocating resources for extant VFs */
	if (pci_num_vf(pf->pdev) != num_alloc_vfs) {
		ret = pci_enable_sriov(pf->pdev, num_alloc_vfs);
		if (ret) {
			pf->flags &= ~I40E_FLAG_VEB_MODE_ENABLED;
			pf->num_alloc_vfs = 0;
			goto err_iov;
		}
	}
	/* allocate memory */
	vfs = kcalloc(num_alloc_vfs, sizeof(struct i40e_vf), GFP_KERNEL);
	if (!vfs) {
		ret = -ENOMEM;
		goto err_alloc;
	}
	pf->vf = vfs;

	/* apply default profile */
	for (i = 0; i < num_alloc_vfs; i++) {
		vfs[i].pf = pf;
		vfs[i].parent_type = I40E_SWITCH_ELEMENT_TYPE_VEB;
		vfs[i].vf_id = i;

		/* assign default capabilities */
		set_bit(I40E_VIRTCHNL_VF_CAP_L2, &vfs[i].vf_caps);
		vfs[i].spoofchk = true;

		set_bit(I40E_VF_STATE_PRE_ENABLE, &vfs[i].vf_states);

	}
	pf->num_alloc_vfs = num_alloc_vfs;

	/* VF resources get allocated during reset */
	i40e_reset_all_vfs(pf, false);

	i40e_notify_client_of_vf_enable(pf, num_alloc_vfs);

err_alloc:
	if (ret)
		i40e_free_vfs(pf);
err_iov:
	/* Re-enable interrupt 0. */
	i40e_irq_dynamic_enable_icr0(pf);
	return ret;
}

#endif
/**
 * i40e_pci_sriov_enable
 * @pdev: pointer to a pci_dev structure
 * @num_vfs: number of VFs to allocate
 *
 * Enable or change the number of VFs
 **/
static int i40e_pci_sriov_enable(struct pci_dev *pdev, int num_vfs)
{
#ifdef CONFIG_PCI_IOV
	struct i40e_pf *pf = pci_get_drvdata(pdev);
	int pre_existing_vfs = pci_num_vf(pdev);
	int err = 0;

	if (test_bit(__I40E_TESTING, pf->state)) {
		dev_warn(&pdev->dev,
			 "Cannot enable SR-IOV virtual functions while the device is undergoing diagnostic testing\n");
		err = -EPERM;
		goto err_out;
	}

	if (pre_existing_vfs && pre_existing_vfs != num_vfs)
		i40e_free_vfs(pf);
	else if (pre_existing_vfs && pre_existing_vfs == num_vfs)
		goto out;

	if (num_vfs > pf->num_req_vfs) {
		dev_warn(&pdev->dev, "Unable to enable %d VFs. Limited to %d VFs due to device resource constraints.\n",
			 num_vfs, pf->num_req_vfs);
		err = -EPERM;
		goto err_out;
	}

	dev_info(&pdev->dev, "Allocating %d VFs.\n", num_vfs);
	err = i40e_alloc_vfs(pf, num_vfs);
	if (err) {
		dev_warn(&pdev->dev, "Failed to enable SR-IOV: %d\n", err);
		goto err_out;
	}

out:
	return num_vfs;

err_out:
	return err;
#endif
	return 0;
}

/**
 * i40e_pci_sriov_configure
 * @pdev: pointer to a pci_dev structure
 * @num_vfs: number of VFs to allocate
 *
 * Enable or change the number of VFs. Called when the user updates the number
 * of VFs in sysfs.
 **/
int i40e_pci_sriov_configure(struct pci_dev *pdev, int num_vfs)
{
	struct i40e_pf *pf = pci_get_drvdata(pdev);

	if (num_vfs) {
		if (!(pf->flags & I40E_FLAG_VEB_MODE_ENABLED)) {
			pf->flags |= I40E_FLAG_VEB_MODE_ENABLED;
			i40e_do_reset_safe(pf, I40E_PF_RESET_AND_REBUILD_FLAG);
		}
		return i40e_pci_sriov_enable(pdev, num_vfs);
	}

	if (!pci_vfs_assigned(pf->pdev)) {
		i40e_free_vfs(pf);
		pf->flags &= ~I40E_FLAG_VEB_MODE_ENABLED;
		i40e_do_reset_safe(pf, I40E_PF_RESET_AND_REBUILD_FLAG);
	} else {
		dev_warn(&pdev->dev, "Unable to free VFs because some are assigned to VMs.\n");
		return -EINVAL;
	}
	return 0;
}

/***********************virtual channel routines******************/

/**
 * i40e_vc_send_msg_to_vf
 * @vf: pointer to the VF info
 * @v_opcode: virtual channel opcode
 * @v_retval: virtual channel return value
 * @msg: pointer to the msg buffer
 * @msglen: msg length
 *
 * send msg to VF
 **/
static int i40e_vc_send_msg_to_vf(struct i40e_vf *vf, u32 v_opcode,
				  u32 v_retval, u8 *msg, u16 msglen)
{
	struct i40e_pf *pf;
	struct i40e_hw *hw;
	int abs_vf_id;
	i40e_status aq_ret;

	/* validate the request */
	if (!vf || vf->vf_id >= vf->pf->num_alloc_vfs)
		return -EINVAL;

	pf = vf->pf;
	hw = &pf->hw;
	abs_vf_id = vf->vf_id + hw->func_caps.vf_base_id;

	/* single place to detect unsuccessful return values */
	if (v_retval) {
		vf->num_invalid_msgs++;
		dev_info(&pf->pdev->dev, "VF %d failed opcode %d, retval: %d\n",
			 vf->vf_id, v_opcode, v_retval);
		if (vf->num_invalid_msgs >
		    I40E_DEFAULT_NUM_INVALID_MSGS_ALLOWED) {
			dev_err(&pf->pdev->dev,
				"Number of invalid messages exceeded for VF %d\n",
				vf->vf_id);
			dev_err(&pf->pdev->dev, "Use PF Control I/F to enable the VF\n");
			set_bit(I40E_VF_STATE_DISABLED, &vf->vf_states);
		}
	} else {
		vf->num_valid_msgs++;
		/* reset the invalid counter, if a valid message is received. */
		vf->num_invalid_msgs = 0;
	}

	aq_ret = i40e_aq_send_msg_to_vf(hw, abs_vf_id,	v_opcode, v_retval,
					msg, msglen, NULL);
	if (aq_ret) {
		dev_info(&pf->pdev->dev,
			 "Unable to send the message to VF %d aq_err %d\n",
			 vf->vf_id, pf->hw.aq.asq_last_status);
		return -EIO;
	}

	return 0;
}

/**
 * i40e_vc_send_resp_to_vf
 * @vf: pointer to the VF info
 * @opcode: operation code
 * @retval: return value
 *
 * send resp msg to VF
 **/
static int i40e_vc_send_resp_to_vf(struct i40e_vf *vf,
				   enum virtchnl_ops opcode,
				   i40e_status retval)
{
	return i40e_vc_send_msg_to_vf(vf, opcode, retval, NULL, 0);
}

/**
 * i40e_vc_get_version_msg
 * @vf: pointer to the VF info
 *
 * called from the VF to request the API version used by the PF
 **/
static int i40e_vc_get_version_msg(struct i40e_vf *vf, u8 *msg)
{
	struct virtchnl_version_info info = {
		VIRTCHNL_VERSION_MAJOR, VIRTCHNL_VERSION_MINOR
	};

	vf->vf_ver = *(struct virtchnl_version_info *)msg;
	/* VFs running the 1.0 API expect to get 1.0 back or they will cry. */
	if (VF_IS_V10(&vf->vf_ver))
		info.minor = VIRTCHNL_VERSION_MINOR_NO_VF_CAPS;
	return i40e_vc_send_msg_to_vf(vf, VIRTCHNL_OP_VERSION,
				      I40E_SUCCESS, (u8 *)&info,
				      sizeof(struct virtchnl_version_info));
}

/**
 * i40e_vc_get_max_frame_size
 * @vf: pointer to the VF
 *
 * Max frame size is determined based on the current port's max frame size and
 * whether a port VLAN is configured on this VF. The VF is not aware whether
 * it's in a port VLAN so the PF needs to account for this in max frame size
 * checks and sending the max frame size to the VF.
 **/
static u16 i40e_vc_get_max_frame_size(struct i40e_vf *vf)
{
	u16 max_frame_size = vf->pf->hw.phy.link_info.max_frame_size;

	if (vf->port_vlan_id)
		max_frame_size -= VLAN_HLEN;

	return max_frame_size;
}

/**
 * i40e_vc_get_vf_resources_msg
 * @vf: pointer to the VF info
 * @msg: pointer to the msg buffer
 * @msglen: msg length
 *
 * called from the VF to request its resources
 **/
static int i40e_vc_get_vf_resources_msg(struct i40e_vf *vf, u8 *msg)
{
	struct virtchnl_vf_resource *vfres = NULL;
	struct i40e_pf *pf = vf->pf;
	i40e_status aq_ret = 0;
	struct i40e_vsi *vsi;
	int num_vsis = 1;
	int len = 0;
	int ret;

	if (!test_bit(I40E_VF_STATE_INIT, &vf->vf_states)) {
		aq_ret = I40E_ERR_PARAM;
		goto err;
	}

	len = (sizeof(struct virtchnl_vf_resource) +
	       sizeof(struct virtchnl_vsi_resource) * num_vsis);

	vfres = kzalloc(len, GFP_KERNEL);
	if (!vfres) {
		aq_ret = I40E_ERR_NO_MEMORY;
		len = 0;
		goto err;
	}
	if (VF_IS_V11(&vf->vf_ver))
		vf->driver_caps = *(u32 *)msg;
	else
		vf->driver_caps = VIRTCHNL_VF_OFFLOAD_L2 |
				  VIRTCHNL_VF_OFFLOAD_RSS_REG |
				  VIRTCHNL_VF_OFFLOAD_VLAN;

	vfres->vf_cap_flags = VIRTCHNL_VF_OFFLOAD_L2;
	vsi = pf->vsi[vf->lan_vsi_idx];
	if (!vsi->info.pvid)
		vfres->vf_cap_flags |= VIRTCHNL_VF_OFFLOAD_VLAN;

	if (i40e_vf_client_capable(pf, vf->vf_id) &&
	    (vf->driver_caps & VIRTCHNL_VF_OFFLOAD_IWARP)) {
		vfres->vf_cap_flags |= VIRTCHNL_VF_OFFLOAD_IWARP;
		set_bit(I40E_VF_STATE_IWARPENA, &vf->vf_states);
	} else {
		clear_bit(I40E_VF_STATE_IWARPENA, &vf->vf_states);
	}

	if (vf->driver_caps & VIRTCHNL_VF_OFFLOAD_RSS_PF) {
		vfres->vf_cap_flags |= VIRTCHNL_VF_OFFLOAD_RSS_PF;
	} else {
		if ((pf->hw_features & I40E_HW_RSS_AQ_CAPABLE) &&
		    (vf->driver_caps & VIRTCHNL_VF_OFFLOAD_RSS_AQ))
			vfres->vf_cap_flags |= VIRTCHNL_VF_OFFLOAD_RSS_AQ;
		else
			vfres->vf_cap_flags |= VIRTCHNL_VF_OFFLOAD_RSS_REG;
	}

	if (pf->hw_features & I40E_HW_MULTIPLE_TCP_UDP_RSS_PCTYPE) {
		if (vf->driver_caps & VIRTCHNL_VF_OFFLOAD_RSS_PCTYPE_V2)
			vfres->vf_cap_flags |=
				VIRTCHNL_VF_OFFLOAD_RSS_PCTYPE_V2;
	}

	if (vf->driver_caps & VIRTCHNL_VF_OFFLOAD_ENCAP)
		vfres->vf_cap_flags |= VIRTCHNL_VF_OFFLOAD_ENCAP;

	if ((pf->hw_features & I40E_HW_OUTER_UDP_CSUM_CAPABLE) &&
	    (vf->driver_caps & VIRTCHNL_VF_OFFLOAD_ENCAP_CSUM))
		vfres->vf_cap_flags |= VIRTCHNL_VF_OFFLOAD_ENCAP_CSUM;

	if (vf->driver_caps & VIRTCHNL_VF_OFFLOAD_RX_POLLING) {
		if (pf->flags & I40E_FLAG_MFP_ENABLED) {
			dev_err(&pf->pdev->dev,
				"VF %d requested polling mode: this feature is supported only when the device is running in single function per port (SFP) mode\n",
				 vf->vf_id);
			aq_ret = I40E_ERR_PARAM;
			goto err;
		}
		vfres->vf_cap_flags |= VIRTCHNL_VF_OFFLOAD_RX_POLLING;
	}

	if (pf->hw_features & I40E_HW_WB_ON_ITR_CAPABLE) {
		if (vf->driver_caps & VIRTCHNL_VF_OFFLOAD_WB_ON_ITR)
			vfres->vf_cap_flags |=
					VIRTCHNL_VF_OFFLOAD_WB_ON_ITR;
	}

	if (vf->driver_caps & VIRTCHNL_VF_OFFLOAD_REQ_QUEUES)
		vfres->vf_cap_flags |= VIRTCHNL_VF_OFFLOAD_REQ_QUEUES;

	vfres->num_vsis = num_vsis;
	vfres->num_queue_pairs = vf->num_queue_pairs;
	vfres->max_vectors = pf->hw.func_caps.num_msix_vectors_vf;
	vfres->rss_key_size = I40E_HKEY_ARRAY_SIZE;
	vfres->rss_lut_size = I40E_VF_HLUT_ARRAY_SIZE;
	vfres->max_mtu = i40e_vc_get_max_frame_size(vf);

	if (vf->lan_vsi_idx) {
		vfres->vsi_res[0].vsi_id = vf->lan_vsi_id;
		vfres->vsi_res[0].vsi_type = VIRTCHNL_VSI_SRIOV;
		vfres->vsi_res[0].num_queue_pairs = vsi->alloc_queue_pairs;
		/* VFs only use TC 0 */
		vfres->vsi_res[0].qset_handle
					  = le16_to_cpu(vsi->info.qs_handle[0]);
		ether_addr_copy(vfres->vsi_res[0].default_mac_addr,
				vf->default_lan_addr.addr);
	}
	set_bit(I40E_VF_STATE_ACTIVE, &vf->vf_states);

err:
	/* send the response back to the VF */
	ret = i40e_vc_send_msg_to_vf(vf, VIRTCHNL_OP_GET_VF_RESOURCES,
				     aq_ret, (u8 *)vfres, len);

	kfree(vfres);
	return ret;
}

/**
 * i40e_vc_reset_vf_msg
 * @vf: pointer to the VF info
 * @msg: pointer to the msg buffer
 * @msglen: msg length
 *
 * called from the VF to reset itself,
 * unlike other virtchnl messages, PF driver
 * doesn't send the response back to the VF
 **/
static void i40e_vc_reset_vf_msg(struct i40e_vf *vf)
{
	if (test_bit(I40E_VF_STATE_ACTIVE, &vf->vf_states))
		i40e_reset_vf(vf, false);
}

/**
 * i40e_getnum_vf_vsi_vlan_filters
 * @vsi: pointer to the vsi
 *
 * called to get the number of VLANs offloaded on this VF
 **/
static inline int i40e_getnum_vf_vsi_vlan_filters(struct i40e_vsi *vsi)
{
	struct i40e_mac_filter *f;
	int num_vlans = 0, bkt;

	hash_for_each(vsi->mac_filter_hash, bkt, f, hlist) {
		if (f->vlan >= 0 && f->vlan <= I40E_MAX_VLANID)
			num_vlans++;
	}

	return num_vlans;
}

/**
 * i40e_vc_config_promiscuous_mode_msg
 * @vf: pointer to the VF info
 * @msg: pointer to the msg buffer
 * @msglen: msg length
 *
 * called from the VF to configure the promiscuous mode of
 * VF vsis
 **/
static int i40e_vc_config_promiscuous_mode_msg(struct i40e_vf *vf,
					       u8 *msg, u16 msglen)
{
	struct virtchnl_promisc_info *info =
	    (struct virtchnl_promisc_info *)msg;
	struct i40e_pf *pf = vf->pf;
	struct i40e_hw *hw = &pf->hw;
	struct i40e_mac_filter *f;
	i40e_status aq_ret = 0;
	bool allmulti = false;
	struct i40e_vsi *vsi;
	bool alluni = false;
	int aq_err = 0;
	int bkt;

	vsi = i40e_find_vsi_from_id(pf, info->vsi_id);
	if (!test_bit(I40E_VF_STATE_ACTIVE, &vf->vf_states) ||
	    !i40e_vc_isvalid_vsi_id(vf, info->vsi_id) ||
	    !vsi) {
		aq_ret = I40E_ERR_PARAM;
		goto error_param;
	}
	if (!test_bit(I40E_VIRTCHNL_VF_CAP_PRIVILEGE, &vf->vf_caps)) {
		dev_err(&pf->pdev->dev,
			"Unprivileged VF %d is attempting to configure promiscuous mode\n",
			vf->vf_id);
		/* Lie to the VF on purpose. */
		aq_ret = 0;
		goto error_param;
	}
	/* Multicast promiscuous handling*/
	if (info->flags & FLAG_VF_MULTICAST_PROMISC)
		allmulti = true;

	if (vf->port_vlan_id) {
		aq_ret = i40e_aq_set_vsi_mc_promisc_on_vlan(hw, vsi->seid,
							    allmulti,
							    vf->port_vlan_id,
							    NULL);
	} else if (i40e_getnum_vf_vsi_vlan_filters(vsi)) {
		hash_for_each(vsi->mac_filter_hash, bkt, f, hlist) {
			if (f->vlan < 0 || f->vlan > I40E_MAX_VLANID)
				continue;
			aq_ret = i40e_aq_set_vsi_mc_promisc_on_vlan(hw,
								    vsi->seid,
								    allmulti,
								    f->vlan,
								    NULL);
			aq_err = pf->hw.aq.asq_last_status;
			if (aq_ret) {
				dev_err(&pf->pdev->dev,
					"Could not add VLAN %d to multicast promiscuous domain err %s aq_err %s\n",
					f->vlan,
					i40e_stat_str(&pf->hw, aq_ret),
					i40e_aq_str(&pf->hw, aq_err));
				break;
			}
		}
	} else {
		aq_ret = i40e_aq_set_vsi_multicast_promiscuous(hw, vsi->seid,
							       allmulti, NULL);
		aq_err = pf->hw.aq.asq_last_status;
		if (aq_ret) {
			dev_err(&pf->pdev->dev,
				"VF %d failed to set multicast promiscuous mode err %s aq_err %s\n",
				vf->vf_id,
				i40e_stat_str(&pf->hw, aq_ret),
				i40e_aq_str(&pf->hw, aq_err));
			goto error_param;
		}
	}

	if (!aq_ret) {
		dev_info(&pf->pdev->dev,
			 "VF %d successfully set multicast promiscuous mode\n",
			 vf->vf_id);
		if (allmulti)
			set_bit(I40E_VF_STATE_MC_PROMISC, &vf->vf_states);
		else
			clear_bit(I40E_VF_STATE_MC_PROMISC, &vf->vf_states);
	}

	if (info->flags & FLAG_VF_UNICAST_PROMISC)
		alluni = true;
	if (vf->port_vlan_id) {
		aq_ret = i40e_aq_set_vsi_uc_promisc_on_vlan(hw, vsi->seid,
							    alluni,
							    vf->port_vlan_id,
							    NULL);
	} else if (i40e_getnum_vf_vsi_vlan_filters(vsi)) {
		hash_for_each(vsi->mac_filter_hash, bkt, f, hlist) {
			if (f->vlan < 0 || f->vlan > I40E_MAX_VLANID)
				continue;
			aq_ret = i40e_aq_set_vsi_uc_promisc_on_vlan(hw,
								    vsi->seid,
								    alluni,
								    f->vlan,
								    NULL);
			aq_err = pf->hw.aq.asq_last_status;
			if (aq_ret)
				dev_err(&pf->pdev->dev,
					"Could not add VLAN %d to Unicast promiscuous domain err %s aq_err %s\n",
					f->vlan,
					i40e_stat_str(&pf->hw, aq_ret),
					i40e_aq_str(&pf->hw, aq_err));
		}
	} else {
		aq_ret = i40e_aq_set_vsi_unicast_promiscuous(hw, vsi->seid,
							     alluni, NULL,
							     true);
		aq_err = pf->hw.aq.asq_last_status;
		if (aq_ret) {
			dev_err(&pf->pdev->dev,
				"VF %d failed to set unicast promiscuous mode %8.8x err %s aq_err %s\n",
				vf->vf_id, info->flags,
				i40e_stat_str(&pf->hw, aq_ret),
				i40e_aq_str(&pf->hw, aq_err));
			goto error_param;
		}
	}

	if (!aq_ret) {
		dev_info(&pf->pdev->dev,
			 "VF %d successfully set unicast promiscuous mode\n",
			 vf->vf_id);
		if (alluni)
			set_bit(I40E_VF_STATE_UC_PROMISC, &vf->vf_states);
		else
			clear_bit(I40E_VF_STATE_UC_PROMISC, &vf->vf_states);
	}

error_param:
	/* send the response to the VF */
	return i40e_vc_send_resp_to_vf(vf,
				       VIRTCHNL_OP_CONFIG_PROMISCUOUS_MODE,
				       aq_ret);
}

/**
 * i40e_vc_config_queues_msg
 * @vf: pointer to the VF info
 * @msg: pointer to the msg buffer
 * @msglen: msg length
 *
 * called from the VF to configure the rx/tx
 * queues
 **/
static int i40e_vc_config_queues_msg(struct i40e_vf *vf, u8 *msg, u16 msglen)
{
	struct virtchnl_vsi_queue_config_info *qci =
	    (struct virtchnl_vsi_queue_config_info *)msg;
	struct virtchnl_queue_pair_info *qpi;
	struct i40e_pf *pf = vf->pf;
	u16 vsi_id, vsi_queue_id;
	i40e_status aq_ret = 0;
	int i;

	if (!test_bit(I40E_VF_STATE_ACTIVE, &vf->vf_states)) {
		aq_ret = I40E_ERR_PARAM;
		goto error_param;
	}

	vsi_id = qci->vsi_id;
	if (!i40e_vc_isvalid_vsi_id(vf, vsi_id)) {
		aq_ret = I40E_ERR_PARAM;
		goto error_param;
	}
	for (i = 0; i < qci->num_queue_pairs; i++) {
		qpi = &qci->qpair[i];
		vsi_queue_id = qpi->txq.queue_id;
		if ((qpi->txq.vsi_id != vsi_id) ||
		    (qpi->rxq.vsi_id != vsi_id) ||
		    (qpi->rxq.queue_id != vsi_queue_id) ||
		    !i40e_vc_isvalid_queue_id(vf, vsi_id, vsi_queue_id)) {
			aq_ret = I40E_ERR_PARAM;
			goto error_param;
		}

		if (i40e_config_vsi_rx_queue(vf, vsi_id, vsi_queue_id,
					     &qpi->rxq) ||
		    i40e_config_vsi_tx_queue(vf, vsi_id, vsi_queue_id,
					     &qpi->txq)) {
			aq_ret = I40E_ERR_PARAM;
			goto error_param;
		}
	}
	/* set vsi num_queue_pairs in use to num configured by VF */
	pf->vsi[vf->lan_vsi_idx]->num_queue_pairs = qci->num_queue_pairs;

error_param:
	/* send the response to the VF */
	return i40e_vc_send_resp_to_vf(vf, VIRTCHNL_OP_CONFIG_VSI_QUEUES,
				       aq_ret);
}

/**
 * i40e_vc_config_irq_map_msg
 * @vf: pointer to the VF info
 * @msg: pointer to the msg buffer
 * @msglen: msg length
 *
 * called from the VF to configure the irq to
 * queue map
 **/
static int i40e_vc_config_irq_map_msg(struct i40e_vf *vf, u8 *msg, u16 msglen)
{
	struct virtchnl_irq_map_info *irqmap_info =
	    (struct virtchnl_irq_map_info *)msg;
	struct virtchnl_vector_map *map;
	u16 vsi_id, vsi_queue_id, vector_id;
	i40e_status aq_ret = 0;
	unsigned long tempmap;
	int i;

	if (!test_bit(I40E_VF_STATE_ACTIVE, &vf->vf_states)) {
		aq_ret = I40E_ERR_PARAM;
		goto error_param;
	}

	for (i = 0; i < irqmap_info->num_vectors; i++) {
		map = &irqmap_info->vecmap[i];

		vector_id = map->vector_id;
		vsi_id = map->vsi_id;
		/* validate msg params */
		if (!i40e_vc_isvalid_vector_id(vf, vector_id) ||
		    !i40e_vc_isvalid_vsi_id(vf, vsi_id)) {
			aq_ret = I40E_ERR_PARAM;
			goto error_param;
		}

		/* lookout for the invalid queue index */
		tempmap = map->rxq_map;
		for_each_set_bit(vsi_queue_id, &tempmap, I40E_MAX_VSI_QP) {
			if (!i40e_vc_isvalid_queue_id(vf, vsi_id,
						      vsi_queue_id)) {
				aq_ret = I40E_ERR_PARAM;
				goto error_param;
			}
		}

		tempmap = map->txq_map;
		for_each_set_bit(vsi_queue_id, &tempmap, I40E_MAX_VSI_QP) {
			if (!i40e_vc_isvalid_queue_id(vf, vsi_id,
						      vsi_queue_id)) {
				aq_ret = I40E_ERR_PARAM;
				goto error_param;
			}
		}

		i40e_config_irq_link_list(vf, vsi_id, map);
	}
error_param:
	/* send the response to the VF */
	return i40e_vc_send_resp_to_vf(vf, VIRTCHNL_OP_CONFIG_IRQ_MAP,
				       aq_ret);
}

/**
 * i40e_vc_enable_queues_msg
 * @vf: pointer to the VF info
 * @msg: pointer to the msg buffer
 * @msglen: msg length
 *
 * called from the VF to enable all or specific queue(s)
 **/
static int i40e_vc_enable_queues_msg(struct i40e_vf *vf, u8 *msg, u16 msglen)
{
	struct virtchnl_queue_select *vqs =
	    (struct virtchnl_queue_select *)msg;
	struct i40e_pf *pf = vf->pf;
	u16 vsi_id = vqs->vsi_id;
	i40e_status aq_ret = 0;

	if (!test_bit(I40E_VF_STATE_ACTIVE, &vf->vf_states)) {
		aq_ret = I40E_ERR_PARAM;
		goto error_param;
	}

	if (!i40e_vc_isvalid_vsi_id(vf, vsi_id)) {
		aq_ret = I40E_ERR_PARAM;
		goto error_param;
	}

	if ((0 == vqs->rx_queues) && (0 == vqs->tx_queues)) {
		aq_ret = I40E_ERR_PARAM;
		goto error_param;
	}

	if (i40e_vsi_start_rings(pf->vsi[vf->lan_vsi_idx]))
		aq_ret = I40E_ERR_TIMEOUT;
error_param:
	/* send the response to the VF */
	return i40e_vc_send_resp_to_vf(vf, VIRTCHNL_OP_ENABLE_QUEUES,
				       aq_ret);
}

/**
 * i40e_vc_disable_queues_msg
 * @vf: pointer to the VF info
 * @msg: pointer to the msg buffer
 * @msglen: msg length
 *
 * called from the VF to disable all or specific
 * queue(s)
 **/
static int i40e_vc_disable_queues_msg(struct i40e_vf *vf, u8 *msg, u16 msglen)
{
	struct virtchnl_queue_select *vqs =
	    (struct virtchnl_queue_select *)msg;
	struct i40e_pf *pf = vf->pf;
	i40e_status aq_ret = 0;

	if (!test_bit(I40E_VF_STATE_ACTIVE, &vf->vf_states)) {
		aq_ret = I40E_ERR_PARAM;
		goto error_param;
	}

	if (!i40e_vc_isvalid_vsi_id(vf, vqs->vsi_id)) {
		aq_ret = I40E_ERR_PARAM;
		goto error_param;
	}

	if ((0 == vqs->rx_queues) && (0 == vqs->tx_queues)) {
		aq_ret = I40E_ERR_PARAM;
		goto error_param;
	}

	i40e_vsi_stop_rings(pf->vsi[vf->lan_vsi_idx]);

error_param:
	/* send the response to the VF */
	return i40e_vc_send_resp_to_vf(vf, VIRTCHNL_OP_DISABLE_QUEUES,
				       aq_ret);
}

/**
 * i40e_check_enough_queue - find big enough queue number
 * @vf: pointer to the VF info
 * @needed: the number of items needed
 *
 * Returns the base item index of the queue, or negative for error
 **/
static int i40e_check_enough_queue(struct i40e_vf *vf, u16 needed)
{
	unsigned int  i, cur_queues, more, pool_size;
	struct i40e_lump_tracking *pile;
	struct i40e_pf *pf = vf->pf;
	struct i40e_vsi *vsi;

	vsi = pf->vsi[vf->lan_vsi_idx];
	cur_queues = vsi->alloc_queue_pairs;

	/* if current allocated queues are enough for need */
	if (cur_queues >= needed)
		return vsi->base_queue;

	pile = pf->qp_pile;
	if (cur_queues > 0) {
		/* if the allocated queues are not zero
		 * just check if there are enough queues for more
		 * behind the allocated queues.
		 */
		more = needed - cur_queues;
		for (i = vsi->base_queue + cur_queues;
			i < pile->num_entries; i++) {
			if (pile->list[i] & I40E_PILE_VALID_BIT)
				break;

			if (more-- == 1)
				/* there is enough */
				return vsi->base_queue;
		}
	}

	pool_size = 0;
	for (i = 0; i < pile->num_entries; i++) {
		if (pile->list[i] & I40E_PILE_VALID_BIT) {
			pool_size = 0;
			continue;
		}
		if (needed <= ++pool_size)
			/* there is enough */
			return i;
	}

	return -ENOMEM;
}

/**
 * i40e_vc_request_queues_msg
 * @vf: pointer to the VF info
 * @msg: pointer to the msg buffer
 * @msglen: msg length
 *
 * VFs get a default number of queues but can use this message to request a
 * different number.  If the request is successful, PF will reset the VF and
 * return 0.  If unsuccessful, PF will send message informing VF of number of
 * available queues and return result of sending VF a message.
 **/
static int i40e_vc_request_queues_msg(struct i40e_vf *vf, u8 *msg, int msglen)
{
	struct virtchnl_vf_res_request *vfres =
		(struct virtchnl_vf_res_request *)msg;
	int req_pairs = vfres->num_queue_pairs;
	int cur_pairs = vf->num_queue_pairs;
	struct i40e_pf *pf = vf->pf;

	if (!test_bit(I40E_VF_STATE_ACTIVE, &vf->vf_states))
		return -EINVAL;

	if (req_pairs <= 0) {
		dev_err(&pf->pdev->dev,
			"VF %d tried to request %d queues.  Ignoring.\n",
			vf->vf_id, req_pairs);
	} else if (req_pairs > I40E_MAX_VF_QUEUES) {
		dev_err(&pf->pdev->dev,
			"VF %d tried to request more than %d queues.\n",
			vf->vf_id,
			I40E_MAX_VF_QUEUES);
		vfres->num_queue_pairs = I40E_MAX_VF_QUEUES;
	} else if (req_pairs - cur_pairs > pf->queues_left) {
		dev_warn(&pf->pdev->dev,
			 "VF %d requested %d more queues, but only %d left.\n",
			 vf->vf_id,
			 req_pairs - cur_pairs,
			 pf->queues_left);
		vfres->num_queue_pairs = pf->queues_left + cur_pairs;
	} else if (i40e_check_enough_queue(vf, req_pairs) < 0) {
		dev_warn(&pf->pdev->dev,
			 "VF %d requested %d more queues, but there is not enough for it.\n",
			 vf->vf_id,
			 req_pairs - cur_pairs);
		vfres->num_queue_pairs = cur_pairs;
	} else {
		/* successful request */
		vf->num_req_queues = req_pairs;
		i40e_vc_notify_vf_reset(vf);
		i40e_reset_vf(vf, false);
		return 0;
	}

	return i40e_vc_send_msg_to_vf(vf, VIRTCHNL_OP_REQUEST_QUEUES, 0,
				      (u8 *)vfres, sizeof(*vfres));
}

/**
 * i40e_vc_get_stats_msg
 * @vf: pointer to the VF info
 * @msg: pointer to the msg buffer
 * @msglen: msg length
 *
 * called from the VF to get vsi stats
 **/
static int i40e_vc_get_stats_msg(struct i40e_vf *vf, u8 *msg, u16 msglen)
{
	struct virtchnl_queue_select *vqs =
	    (struct virtchnl_queue_select *)msg;
	struct i40e_pf *pf = vf->pf;
	struct i40e_eth_stats stats;
	i40e_status aq_ret = 0;
	struct i40e_vsi *vsi;

	memset(&stats, 0, sizeof(struct i40e_eth_stats));

	if (!test_bit(I40E_VF_STATE_ACTIVE, &vf->vf_states)) {
		aq_ret = I40E_ERR_PARAM;
		goto error_param;
	}

	if (!i40e_vc_isvalid_vsi_id(vf, vqs->vsi_id)) {
		aq_ret = I40E_ERR_PARAM;
		goto error_param;
	}

	vsi = pf->vsi[vf->lan_vsi_idx];
	if (!vsi) {
		aq_ret = I40E_ERR_PARAM;
		goto error_param;
	}
	i40e_update_eth_stats(vsi);
	stats = vsi->eth_stats;

error_param:
	/* send the response back to the VF */
	return i40e_vc_send_msg_to_vf(vf, VIRTCHNL_OP_GET_STATS, aq_ret,
				      (u8 *)&stats, sizeof(stats));
}

/* If the VF is not trusted restrict the number of MAC/VLAN it can program
 * MAC filters: 16 for multicast, 1 for MAC, 1 for broadcast
 */
#define I40E_VC_MAX_MAC_ADDR_PER_VF (16 + 1 + 1)
#define I40E_VC_MAX_VLAN_PER_VF 8

/**
 * i40e_check_vf_permission
 * @vf: pointer to the VF info
 * @macaddr: pointer to the MAC Address being checked
 *
 * Check if the VF has permission to add or delete unicast MAC address
 * filters and return error code -EPERM if not.  Then check if the
 * address filter requested is broadcast or zero and if so return
 * an invalid MAC address error code.
 **/
static inline int i40e_check_vf_permission(struct i40e_vf *vf, u8 *macaddr)
{
	struct i40e_pf *pf = vf->pf;
	int ret = 0;

	if (is_broadcast_ether_addr(macaddr) ||
		   is_zero_ether_addr(macaddr)) {
		dev_err(&pf->pdev->dev, "invalid VF MAC addr %pM\n", macaddr);
		ret = I40E_ERR_INVALID_MAC_ADDR;
	} else if (vf->pf_set_mac && !is_multicast_ether_addr(macaddr) &&
		   !test_bit(I40E_VIRTCHNL_VF_CAP_PRIVILEGE, &vf->vf_caps) &&
		   !ether_addr_equal(macaddr, vf->default_lan_addr.addr)) {
		/* If the host VMM administrator has set the VF MAC address
		 * administratively via the ndo_set_vf_mac command then deny
		 * permission to the VF to add or delete unicast MAC addresses.
		 * Unless the VF is privileged and then it can do whatever.
		 * The VF may request to set the MAC address filter already
		 * assigned to it so do not return an error in that case.
		 */
		dev_err(&pf->pdev->dev,
			"VF attempting to override administratively set MAC address, reload the VF driver to resume normal operation\n");
		ret = -EPERM;
	} else if ((vf->num_mac >= I40E_VC_MAX_MAC_ADDR_PER_VF) &&
		   !test_bit(I40E_VIRTCHNL_VF_CAP_PRIVILEGE, &vf->vf_caps)) {
		dev_err(&pf->pdev->dev,
			"VF is not trusted, switch the VF to trusted to add more functionality\n");
		ret = -EPERM;
	}
	return ret;
}

/**
 * i40e_vc_add_mac_addr_msg
 * @vf: pointer to the VF info
 * @msg: pointer to the msg buffer
 * @msglen: msg length
 *
 * add guest mac address filter
 **/
static int i40e_vc_add_mac_addr_msg(struct i40e_vf *vf, u8 *msg, u16 msglen)
{
	struct virtchnl_ether_addr_list *al =
	    (struct virtchnl_ether_addr_list *)msg;
	struct i40e_pf *pf = vf->pf;
	struct i40e_vsi *vsi = NULL;
	u16 vsi_id = al->vsi_id;
	i40e_status ret = 0;
	int i;

	if (!test_bit(I40E_VF_STATE_ACTIVE, &vf->vf_states) ||
	    !i40e_vc_isvalid_vsi_id(vf, vsi_id)) {
		ret = I40E_ERR_PARAM;
		goto error_param;
	}

	for (i = 0; i < al->num_elements; i++) {
		ret = i40e_check_vf_permission(vf, al->list[i].addr);
		if (ret)
			goto error_param;
	}
	vsi = pf->vsi[vf->lan_vsi_idx];

	/* Lock once, because all function inside for loop accesses VSI's
	 * MAC filter list which needs to be protected using same lock.
	 */
	spin_lock_bh(&vsi->mac_filter_hash_lock);

	/* add new addresses to the list */
	for (i = 0; i < al->num_elements; i++) {
		struct i40e_mac_filter *f;

		f = i40e_find_mac(vsi, al->list[i].addr);
		if (!f) {
			f = i40e_add_mac_filter(vsi, al->list[i].addr);

			if (!f) {
				dev_err(&pf->pdev->dev,
					"Unable to add MAC filter %pM for VF %d\n",
					al->list[i].addr, vf->vf_id);
				ret = I40E_ERR_PARAM;
				spin_unlock_bh(&vsi->mac_filter_hash_lock);
				goto error_param;
			} else {
				vf->num_mac++;
			}
		}
	}
	spin_unlock_bh(&vsi->mac_filter_hash_lock);

	/* program the updated filter list */
	ret = i40e_sync_vsi_filters(vsi);
	if (ret)
		dev_err(&pf->pdev->dev, "Unable to program VF %d MAC filters, error %d\n",
			vf->vf_id, ret);

error_param:
	/* send the response to the VF */
	return i40e_vc_send_resp_to_vf(vf, VIRTCHNL_OP_ADD_ETH_ADDR,
				       ret);
}

/**
 * i40e_vc_del_mac_addr_msg
 * @vf: pointer to the VF info
 * @msg: pointer to the msg buffer
 * @msglen: msg length
 *
 * remove guest mac address filter
 **/
static int i40e_vc_del_mac_addr_msg(struct i40e_vf *vf, u8 *msg, u16 msglen)
{
	struct virtchnl_ether_addr_list *al =
	    (struct virtchnl_ether_addr_list *)msg;
	struct i40e_pf *pf = vf->pf;
	struct i40e_vsi *vsi = NULL;
	u16 vsi_id = al->vsi_id;
	i40e_status ret = 0;
	int i;

	if (!test_bit(I40E_VF_STATE_ACTIVE, &vf->vf_states) ||
	    !i40e_vc_isvalid_vsi_id(vf, vsi_id)) {
		ret = I40E_ERR_PARAM;
		goto error_param;
	}

	for (i = 0; i < al->num_elements; i++) {
		if (is_broadcast_ether_addr(al->list[i].addr) ||
		    is_zero_ether_addr(al->list[i].addr)) {
			dev_err(&pf->pdev->dev, "Invalid MAC addr %pM for VF %d\n",
				al->list[i].addr, vf->vf_id);
			ret = I40E_ERR_INVALID_MAC_ADDR;
			goto error_param;
		}

		if (vf->pf_set_mac &&
		    ether_addr_equal(al->list[i].addr,
				     vf->default_lan_addr.addr)) {
			dev_err(&pf->pdev->dev,
				"MAC addr %pM has been set by PF, cannot delete it for VF %d, reset VF to change MAC addr\n",
				vf->default_lan_addr.addr, vf->vf_id);
			ret = I40E_ERR_PARAM;
			goto error_param;
		}
	}
	vsi = pf->vsi[vf->lan_vsi_idx];

	spin_lock_bh(&vsi->mac_filter_hash_lock);
	/* delete addresses from the list */
	for (i = 0; i < al->num_elements; i++)
		if (i40e_del_mac_filter(vsi, al->list[i].addr)) {
			ret = I40E_ERR_INVALID_MAC_ADDR;
			spin_unlock_bh(&vsi->mac_filter_hash_lock);
			goto error_param;
		} else {
			vf->num_mac--;
		}

	spin_unlock_bh(&vsi->mac_filter_hash_lock);

	/* program the updated filter list */
	ret = i40e_sync_vsi_filters(vsi);
	if (ret)
		dev_err(&pf->pdev->dev, "Unable to program VF %d MAC filters, error %d\n",
			vf->vf_id, ret);

error_param:
	/* send the response to the VF */
	return i40e_vc_send_resp_to_vf(vf, VIRTCHNL_OP_DEL_ETH_ADDR,
				       ret);
}

/**
 * i40e_vc_add_vlan_msg
 * @vf: pointer to the VF info
 * @msg: pointer to the msg buffer
 * @msglen: msg length
 *
 * program guest vlan id
 **/
static int i40e_vc_add_vlan_msg(struct i40e_vf *vf, u8 *msg, u16 msglen)
{
	struct virtchnl_vlan_filter_list *vfl =
	    (struct virtchnl_vlan_filter_list *)msg;
	struct i40e_pf *pf = vf->pf;
	struct i40e_vsi *vsi = NULL;
	u16 vsi_id = vfl->vsi_id;
	i40e_status aq_ret = 0;
	int i;

	if ((vf->num_vlan >= I40E_VC_MAX_VLAN_PER_VF) &&
	    !test_bit(I40E_VIRTCHNL_VF_CAP_PRIVILEGE, &vf->vf_caps)) {
		dev_err(&pf->pdev->dev,
			"VF is not trusted, switch the VF to trusted to add more VLAN addresses\n");
		goto error_param;
	}
	if (!test_bit(I40E_VF_STATE_ACTIVE, &vf->vf_states) ||
	    !i40e_vc_isvalid_vsi_id(vf, vsi_id)) {
		aq_ret = I40E_ERR_PARAM;
		goto error_param;
	}

	for (i = 0; i < vfl->num_elements; i++) {
		if (vfl->vlan_id[i] > I40E_MAX_VLANID) {
			aq_ret = I40E_ERR_PARAM;
			dev_err(&pf->pdev->dev,
				"invalid VF VLAN id %d\n", vfl->vlan_id[i]);
			goto error_param;
		}
	}
	vsi = pf->vsi[vf->lan_vsi_idx];
	if (vsi->info.pvid) {
		aq_ret = I40E_ERR_PARAM;
		goto error_param;
	}

	i40e_vlan_stripping_enable(vsi);
	for (i = 0; i < vfl->num_elements; i++) {
		/* add new VLAN filter */
		int ret = i40e_vsi_add_vlan(vsi, vfl->vlan_id[i]);
		if (!ret)
			vf->num_vlan++;

		if (test_bit(I40E_VF_STATE_UC_PROMISC, &vf->vf_states))
			i40e_aq_set_vsi_uc_promisc_on_vlan(&pf->hw, vsi->seid,
							   true,
							   vfl->vlan_id[i],
							   NULL);
		if (test_bit(I40E_VF_STATE_MC_PROMISC, &vf->vf_states))
			i40e_aq_set_vsi_mc_promisc_on_vlan(&pf->hw, vsi->seid,
							   true,
							   vfl->vlan_id[i],
							   NULL);

		if (ret)
			dev_err(&pf->pdev->dev,
				"Unable to add VLAN filter %d for VF %d, error %d\n",
				vfl->vlan_id[i], vf->vf_id, ret);
	}

error_param:
	/* send the response to the VF */
	return i40e_vc_send_resp_to_vf(vf, VIRTCHNL_OP_ADD_VLAN, aq_ret);
}

/**
 * i40e_vc_remove_vlan_msg
 * @vf: pointer to the VF info
 * @msg: pointer to the msg buffer
 * @msglen: msg length
 *
 * remove programmed guest vlan id
 **/
static int i40e_vc_remove_vlan_msg(struct i40e_vf *vf, u8 *msg, u16 msglen)
{
	struct virtchnl_vlan_filter_list *vfl =
	    (struct virtchnl_vlan_filter_list *)msg;
	struct i40e_pf *pf = vf->pf;
	struct i40e_vsi *vsi = NULL;
	u16 vsi_id = vfl->vsi_id;
	i40e_status aq_ret = 0;
	int i;

	if (!test_bit(I40E_VF_STATE_ACTIVE, &vf->vf_states) ||
	    !i40e_vc_isvalid_vsi_id(vf, vsi_id)) {
		aq_ret = I40E_ERR_PARAM;
		goto error_param;
	}

	for (i = 0; i < vfl->num_elements; i++) {
		if (vfl->vlan_id[i] > I40E_MAX_VLANID) {
			aq_ret = I40E_ERR_PARAM;
			goto error_param;
		}
	}

	vsi = pf->vsi[vf->lan_vsi_idx];
	if (vsi->info.pvid) {
		aq_ret = I40E_ERR_PARAM;
		goto error_param;
	}

	for (i = 0; i < vfl->num_elements; i++) {
		i40e_vsi_kill_vlan(vsi, vfl->vlan_id[i]);
		vf->num_vlan--;

		if (test_bit(I40E_VF_STATE_UC_PROMISC, &vf->vf_states))
			i40e_aq_set_vsi_uc_promisc_on_vlan(&pf->hw, vsi->seid,
							   false,
							   vfl->vlan_id[i],
							   NULL);
		if (test_bit(I40E_VF_STATE_MC_PROMISC, &vf->vf_states))
			i40e_aq_set_vsi_mc_promisc_on_vlan(&pf->hw, vsi->seid,
							   false,
							   vfl->vlan_id[i],
							   NULL);
	}

error_param:
	/* send the response to the VF */
	return i40e_vc_send_resp_to_vf(vf, VIRTCHNL_OP_DEL_VLAN, aq_ret);
}

/**
 * i40e_vc_iwarp_msg
 * @vf: pointer to the VF info
 * @msg: pointer to the msg buffer
 * @msglen: msg length
 *
 * called from the VF for the iwarp msgs
 **/
static int i40e_vc_iwarp_msg(struct i40e_vf *vf, u8 *msg, u16 msglen)
{
	struct i40e_pf *pf = vf->pf;
	int abs_vf_id = vf->vf_id + pf->hw.func_caps.vf_base_id;
	i40e_status aq_ret = 0;

	if (!test_bit(I40E_VF_STATE_ACTIVE, &vf->vf_states) ||
	    !test_bit(I40E_VF_STATE_IWARPENA, &vf->vf_states)) {
		aq_ret = I40E_ERR_PARAM;
		goto error_param;
	}

	i40e_notify_client_of_vf_msg(pf->vsi[pf->lan_vsi], abs_vf_id,
				     msg, msglen);

error_param:
	/* send the response to the VF */
	return i40e_vc_send_resp_to_vf(vf, VIRTCHNL_OP_IWARP,
				       aq_ret);
}

/**
 * i40e_vc_iwarp_qvmap_msg
 * @vf: pointer to the VF info
 * @msg: pointer to the msg buffer
 * @msglen: msg length
 * @config: config qvmap or release it
 *
 * called from the VF for the iwarp msgs
 **/
static int i40e_vc_iwarp_qvmap_msg(struct i40e_vf *vf, u8 *msg, u16 msglen,
				   bool config)
{
	struct virtchnl_iwarp_qvlist_info *qvlist_info =
				(struct virtchnl_iwarp_qvlist_info *)msg;
	i40e_status aq_ret = 0;

	if (!test_bit(I40E_VF_STATE_ACTIVE, &vf->vf_states) ||
	    !test_bit(I40E_VF_STATE_IWARPENA, &vf->vf_states)) {
		aq_ret = I40E_ERR_PARAM;
		goto error_param;
	}

	if (config) {
		if (i40e_config_iwarp_qvlist(vf, qvlist_info))
			aq_ret = I40E_ERR_PARAM;
	} else {
		i40e_release_iwarp_qvlist(vf);
	}

error_param:
	/* send the response to the VF */
	return i40e_vc_send_resp_to_vf(vf,
			       config ? VIRTCHNL_OP_CONFIG_IWARP_IRQ_MAP :
			       VIRTCHNL_OP_RELEASE_IWARP_IRQ_MAP,
			       aq_ret);
}

/**
 * i40e_vc_config_rss_key
 * @vf: pointer to the VF info
 * @msg: pointer to the msg buffer
 * @msglen: msg length
 *
 * Configure the VF's RSS key
 **/
static int i40e_vc_config_rss_key(struct i40e_vf *vf, u8 *msg, u16 msglen)
{
	struct virtchnl_rss_key *vrk =
		(struct virtchnl_rss_key *)msg;
	struct i40e_pf *pf = vf->pf;
	struct i40e_vsi *vsi = NULL;
	u16 vsi_id = vrk->vsi_id;
	i40e_status aq_ret = 0;

	if (!test_bit(I40E_VF_STATE_ACTIVE, &vf->vf_states) ||
	    !i40e_vc_isvalid_vsi_id(vf, vsi_id) ||
	    (vrk->key_len != I40E_HKEY_ARRAY_SIZE)) {
		aq_ret = I40E_ERR_PARAM;
		goto err;
	}

	vsi = pf->vsi[vf->lan_vsi_idx];
	aq_ret = i40e_config_rss(vsi, vrk->key, NULL, 0);
err:
	/* send the response to the VF */
	return i40e_vc_send_resp_to_vf(vf, VIRTCHNL_OP_CONFIG_RSS_KEY,
				       aq_ret);
}

/**
 * i40e_vc_config_rss_lut
 * @vf: pointer to the VF info
 * @msg: pointer to the msg buffer
 * @msglen: msg length
 *
 * Configure the VF's RSS LUT
 **/
static int i40e_vc_config_rss_lut(struct i40e_vf *vf, u8 *msg, u16 msglen)
{
	struct virtchnl_rss_lut *vrl =
		(struct virtchnl_rss_lut *)msg;
	struct i40e_pf *pf = vf->pf;
	struct i40e_vsi *vsi = NULL;
	u16 vsi_id = vrl->vsi_id;
	i40e_status aq_ret = 0;

	if (!test_bit(I40E_VF_STATE_ACTIVE, &vf->vf_states) ||
	    !i40e_vc_isvalid_vsi_id(vf, vsi_id) ||
	    (vrl->lut_entries != I40E_VF_HLUT_ARRAY_SIZE)) {
		aq_ret = I40E_ERR_PARAM;
		goto err;
	}

	vsi = pf->vsi[vf->lan_vsi_idx];
	aq_ret = i40e_config_rss(vsi, NULL, vrl->lut, I40E_VF_HLUT_ARRAY_SIZE);
	/* send the response to the VF */
err:
	return i40e_vc_send_resp_to_vf(vf, VIRTCHNL_OP_CONFIG_RSS_LUT,
				       aq_ret);
}

/**
 * i40e_vc_get_rss_hena
 * @vf: pointer to the VF info
 * @msg: pointer to the msg buffer
 * @msglen: msg length
 *
 * Return the RSS HENA bits allowed by the hardware
 **/
static int i40e_vc_get_rss_hena(struct i40e_vf *vf, u8 *msg, u16 msglen)
{
	struct virtchnl_rss_hena *vrh = NULL;
	struct i40e_pf *pf = vf->pf;
	i40e_status aq_ret = 0;
	int len = 0;

	if (!test_bit(I40E_VF_STATE_ACTIVE, &vf->vf_states)) {
		aq_ret = I40E_ERR_PARAM;
		goto err;
	}
	len = sizeof(struct virtchnl_rss_hena);

	vrh = kzalloc(len, GFP_KERNEL);
	if (!vrh) {
		aq_ret = I40E_ERR_NO_MEMORY;
		len = 0;
		goto err;
	}
	vrh->hena = i40e_pf_get_default_rss_hena(pf);
err:
	/* send the response back to the VF */
	aq_ret = i40e_vc_send_msg_to_vf(vf, VIRTCHNL_OP_GET_RSS_HENA_CAPS,
					aq_ret, (u8 *)vrh, len);
	kfree(vrh);
	return aq_ret;
}

/**
 * i40e_vc_set_rss_hena
 * @vf: pointer to the VF info
 * @msg: pointer to the msg buffer
 * @msglen: msg length
 *
 * Set the RSS HENA bits for the VF
 **/
static int i40e_vc_set_rss_hena(struct i40e_vf *vf, u8 *msg, u16 msglen)
{
	struct virtchnl_rss_hena *vrh =
		(struct virtchnl_rss_hena *)msg;
	struct i40e_pf *pf = vf->pf;
	struct i40e_hw *hw = &pf->hw;
	i40e_status aq_ret = 0;

	if (!test_bit(I40E_VF_STATE_ACTIVE, &vf->vf_states)) {
		aq_ret = I40E_ERR_PARAM;
		goto err;
	}
	i40e_write_rx_ctl(hw, I40E_VFQF_HENA1(0, vf->vf_id), (u32)vrh->hena);
	i40e_write_rx_ctl(hw, I40E_VFQF_HENA1(1, vf->vf_id),
			  (u32)(vrh->hena >> 32));

	/* send the response to the VF */
err:
	return i40e_vc_send_resp_to_vf(vf, VIRTCHNL_OP_SET_RSS_HENA, aq_ret);
}

/**
 * i40e_vc_enable_vlan_stripping
 * @vf: pointer to the VF info
 * @msg: pointer to the msg buffer
 * @msglen: msg length
 *
 * Enable vlan header stripping for the VF
 **/
static int i40e_vc_enable_vlan_stripping(struct i40e_vf *vf, u8 *msg,
					 u16 msglen)
{
	struct i40e_vsi *vsi = vf->pf->vsi[vf->lan_vsi_idx];
	i40e_status aq_ret = 0;

	if (!test_bit(I40E_VF_STATE_ACTIVE, &vf->vf_states)) {
		aq_ret = I40E_ERR_PARAM;
		goto err;
	}

	i40e_vlan_stripping_enable(vsi);

	/* send the response to the VF */
err:
	return i40e_vc_send_resp_to_vf(vf, VIRTCHNL_OP_ENABLE_VLAN_STRIPPING,
				       aq_ret);
}

/**
 * i40e_vc_disable_vlan_stripping
 * @vf: pointer to the VF info
 * @msg: pointer to the msg buffer
 * @msglen: msg length
 *
 * Disable vlan header stripping for the VF
 **/
static int i40e_vc_disable_vlan_stripping(struct i40e_vf *vf, u8 *msg,
					  u16 msglen)
{
	struct i40e_vsi *vsi = vf->pf->vsi[vf->lan_vsi_idx];
	i40e_status aq_ret = 0;

	if (!test_bit(I40E_VF_STATE_ACTIVE, &vf->vf_states)) {
		aq_ret = I40E_ERR_PARAM;
		goto err;
	}

	i40e_vlan_stripping_disable(vsi);

	/* send the response to the VF */
err:
	return i40e_vc_send_resp_to_vf(vf, VIRTCHNL_OP_DISABLE_VLAN_STRIPPING,
				       aq_ret);
}

/**
 * i40e_vc_process_vf_msg
 * @pf: pointer to the PF structure
 * @vf_id: source VF id
 * @msg: pointer to the msg buffer
 * @msglen: msg length
 * @msghndl: msg handle
 *
 * called from the common aeq/arq handler to
 * process request from VF
 **/
int i40e_vc_process_vf_msg(struct i40e_pf *pf, s16 vf_id, u32 v_opcode,
			   u32 v_retval, u8 *msg, u16 msglen)
{
	struct i40e_hw *hw = &pf->hw;
	int local_vf_id = vf_id - (s16)hw->func_caps.vf_base_id;
	struct i40e_vf *vf;
	int ret;

	pf->vf_aq_requests++;
	if (local_vf_id >= pf->num_alloc_vfs)
		return -EINVAL;
	vf = &(pf->vf[local_vf_id]);

	/* Check if VF is disabled. */
	if (test_bit(I40E_VF_STATE_DISABLED, &vf->vf_states))
		return I40E_ERR_PARAM;

	/* perform basic checks on the msg */
	ret = virtchnl_vc_validate_vf_msg(&vf->vf_ver, v_opcode, msg, msglen);

	/* perform additional checks specific to this driver */
	if (v_opcode == VIRTCHNL_OP_CONFIG_RSS_KEY) {
		struct virtchnl_rss_key *vrk = (struct virtchnl_rss_key *)msg;

		if (vrk->key_len != I40E_HKEY_ARRAY_SIZE)
			ret = -EINVAL;
	} else if (v_opcode == VIRTCHNL_OP_CONFIG_RSS_LUT) {
		struct virtchnl_rss_lut *vrl = (struct virtchnl_rss_lut *)msg;

		if (vrl->lut_entries != I40E_VF_HLUT_ARRAY_SIZE)
			ret = -EINVAL;
	}

	if (ret) {
		i40e_vc_send_resp_to_vf(vf, v_opcode, I40E_ERR_PARAM);
		dev_err(&pf->pdev->dev, "Invalid message from VF %d, opcode %d, len %d\n",
			local_vf_id, v_opcode, msglen);
		switch (ret) {
		case VIRTCHNL_ERR_PARAM:
			return -EPERM;
		default:
			return -EINVAL;
		}
	}

	switch (v_opcode) {
	case VIRTCHNL_OP_VERSION:
		ret = i40e_vc_get_version_msg(vf, msg);
		break;
	case VIRTCHNL_OP_GET_VF_RESOURCES:
		ret = i40e_vc_get_vf_resources_msg(vf, msg);
		break;
	case VIRTCHNL_OP_RESET_VF:
		i40e_vc_reset_vf_msg(vf);
		ret = 0;
		break;
	case VIRTCHNL_OP_CONFIG_PROMISCUOUS_MODE:
		ret = i40e_vc_config_promiscuous_mode_msg(vf, msg, msglen);
		break;
	case VIRTCHNL_OP_CONFIG_VSI_QUEUES:
		ret = i40e_vc_config_queues_msg(vf, msg, msglen);
		break;
	case VIRTCHNL_OP_CONFIG_IRQ_MAP:
		ret = i40e_vc_config_irq_map_msg(vf, msg, msglen);
		break;
	case VIRTCHNL_OP_ENABLE_QUEUES:
		ret = i40e_vc_enable_queues_msg(vf, msg, msglen);
		i40e_vc_notify_vf_link_state(vf);
		break;
	case VIRTCHNL_OP_DISABLE_QUEUES:
		ret = i40e_vc_disable_queues_msg(vf, msg, msglen);
		break;
	case VIRTCHNL_OP_ADD_ETH_ADDR:
		ret = i40e_vc_add_mac_addr_msg(vf, msg, msglen);
		break;
	case VIRTCHNL_OP_DEL_ETH_ADDR:
		ret = i40e_vc_del_mac_addr_msg(vf, msg, msglen);
		break;
	case VIRTCHNL_OP_ADD_VLAN:
		ret = i40e_vc_add_vlan_msg(vf, msg, msglen);
		break;
	case VIRTCHNL_OP_DEL_VLAN:
		ret = i40e_vc_remove_vlan_msg(vf, msg, msglen);
		break;
	case VIRTCHNL_OP_GET_STATS:
		ret = i40e_vc_get_stats_msg(vf, msg, msglen);
		break;
	case VIRTCHNL_OP_IWARP:
		ret = i40e_vc_iwarp_msg(vf, msg, msglen);
		break;
	case VIRTCHNL_OP_CONFIG_IWARP_IRQ_MAP:
		ret = i40e_vc_iwarp_qvmap_msg(vf, msg, msglen, true);
		break;
	case VIRTCHNL_OP_RELEASE_IWARP_IRQ_MAP:
		ret = i40e_vc_iwarp_qvmap_msg(vf, msg, msglen, false);
		break;
	case VIRTCHNL_OP_CONFIG_RSS_KEY:
		ret = i40e_vc_config_rss_key(vf, msg, msglen);
		break;
	case VIRTCHNL_OP_CONFIG_RSS_LUT:
		ret = i40e_vc_config_rss_lut(vf, msg, msglen);
		break;
	case VIRTCHNL_OP_GET_RSS_HENA_CAPS:
		ret = i40e_vc_get_rss_hena(vf, msg, msglen);
		break;
	case VIRTCHNL_OP_SET_RSS_HENA:
		ret = i40e_vc_set_rss_hena(vf, msg, msglen);
		break;
	case VIRTCHNL_OP_ENABLE_VLAN_STRIPPING:
		ret = i40e_vc_enable_vlan_stripping(vf, msg, msglen);
		break;
	case VIRTCHNL_OP_DISABLE_VLAN_STRIPPING:
		ret = i40e_vc_disable_vlan_stripping(vf, msg, msglen);
		break;
	case VIRTCHNL_OP_REQUEST_QUEUES:
		ret = i40e_vc_request_queues_msg(vf, msg, msglen);
		break;

	case VIRTCHNL_OP_UNKNOWN:
	default:
		dev_err(&pf->pdev->dev, "Unsupported opcode %d from VF %d\n",
			v_opcode, local_vf_id);
		ret = i40e_vc_send_resp_to_vf(vf, v_opcode,
					      I40E_ERR_NOT_IMPLEMENTED);
		break;
	}

	return ret;
}

/**
 * i40e_vc_process_vflr_event
 * @pf: pointer to the PF structure
 *
 * called from the vlfr irq handler to
 * free up VF resources and state variables
 **/
int i40e_vc_process_vflr_event(struct i40e_pf *pf)
{
	struct i40e_hw *hw = &pf->hw;
	u32 reg, reg_idx, bit_idx;
	struct i40e_vf *vf;
	int vf_id;

	if (!test_bit(__I40E_VFLR_EVENT_PENDING, pf->state))
		return 0;

	/* Re-enable the VFLR interrupt cause here, before looking for which
	 * VF got reset. Otherwise, if another VF gets a reset while the
	 * first one is being processed, that interrupt will be lost, and
	 * that VF will be stuck in reset forever.
	 */
	reg = rd32(hw, I40E_PFINT_ICR0_ENA);
	reg |= I40E_PFINT_ICR0_ENA_VFLR_MASK;
	wr32(hw, I40E_PFINT_ICR0_ENA, reg);
	i40e_flush(hw);

	clear_bit(__I40E_VFLR_EVENT_PENDING, pf->state);
	for (vf_id = 0; vf_id < pf->num_alloc_vfs; vf_id++) {
		reg_idx = (hw->func_caps.vf_base_id + vf_id) / 32;
		bit_idx = (hw->func_caps.vf_base_id + vf_id) % 32;
		/* read GLGEN_VFLRSTAT register to find out the flr VFs */
		vf = &pf->vf[vf_id];
		reg = rd32(hw, I40E_GLGEN_VFLRSTAT(reg_idx));
		if (reg & BIT(bit_idx))
			/* i40e_reset_vf will clear the bit in GLGEN_VFLRSTAT */
			i40e_reset_vf(vf, true);
	}

	return 0;
}

/**
 * i40e_ndo_set_vf_mac
 * @netdev: network interface device structure
 * @vf_id: VF identifier
 * @mac: mac address
 *
 * program VF mac address
 **/
int i40e_ndo_set_vf_mac(struct net_device *netdev, int vf_id, u8 *mac)
{
	struct i40e_netdev_priv *np = netdev_priv(netdev);
	struct i40e_vsi *vsi = np->vsi;
	struct i40e_pf *pf = vsi->back;
	struct i40e_mac_filter *f;
	struct i40e_vf *vf;
	int ret = 0;
	struct hlist_node *h;
	int bkt;
	u8 i;

	/* validate the request */
	if (vf_id >= pf->num_alloc_vfs) {
		dev_err(&pf->pdev->dev,
			"Invalid VF Identifier %d\n", vf_id);
		ret = -EINVAL;
		goto error_param;
	}

	vf = &(pf->vf[vf_id]);
	vsi = pf->vsi[vf->lan_vsi_idx];

	/* When the VF is resetting wait until it is done.
	 * It can take up to 200 milliseconds,
	 * but wait for up to 300 milliseconds to be safe.
	 * If the VF is indeed in reset, the vsi pointer has
	 * to show on the newly loaded vsi under pf->vsi[id].
	 */
	for (i = 0; i < 15; i++) {
		if (test_bit(I40E_VF_STATE_INIT, &vf->vf_states)) {
			if (i > 0)
				vsi = pf->vsi[vf->lan_vsi_idx];
			break;
		}
		msleep(20);
	}
	if (!test_bit(I40E_VF_STATE_INIT, &vf->vf_states)) {
		dev_err(&pf->pdev->dev, "VF %d still in reset. Try again.\n",
			vf_id);
		ret = -EAGAIN;
		goto error_param;
	}

	if (is_multicast_ether_addr(mac)) {
		dev_err(&pf->pdev->dev,
			"Invalid Ethernet address %pM for VF %d\n", mac, vf_id);
		ret = -EINVAL;
		goto error_param;
	}

	/* Lock once because below invoked function add/del_filter requires
	 * mac_filter_hash_lock to be held
	 */
	spin_lock_bh(&vsi->mac_filter_hash_lock);

	/* delete the temporary mac address */
	if (!is_zero_ether_addr(vf->default_lan_addr.addr))
		i40e_del_mac_filter(vsi, vf->default_lan_addr.addr);

	/* Delete all the filters for this VSI - we're going to kill it
	 * anyway.
	 */
	hash_for_each_safe(vsi->mac_filter_hash, bkt, h, f, hlist)
		__i40e_del_filter(vsi, f);

	spin_unlock_bh(&vsi->mac_filter_hash_lock);

	/* program mac filter */
	if (i40e_sync_vsi_filters(vsi)) {
		dev_err(&pf->pdev->dev, "Unable to program ucast filters\n");
		ret = -EIO;
		goto error_param;
	}
	ether_addr_copy(vf->default_lan_addr.addr, mac);

	if (is_zero_ether_addr(mac)) {
		vf->pf_set_mac = false;
		dev_info(&pf->pdev->dev, "Removing MAC on VF %d\n", vf_id);
	} else {
		vf->pf_set_mac = true;
		dev_info(&pf->pdev->dev, "Setting MAC %pM on VF %d\n",
			 mac, vf_id);
	}

	/* Force the VF driver stop so it has to reload with new MAC address */
	i40e_vc_disable_vf(vf);
	dev_info(&pf->pdev->dev, "Reload the VF driver to make this change effective.\n");

error_param:
	return ret;
}

/**
 * i40e_ndo_set_vf_port_vlan
 * @netdev: network interface device structure
 * @vf_id: VF identifier
 * @vlan_id: mac address
 * @qos: priority setting
 * @vlan_proto: vlan protocol
 *
 * program VF vlan id and/or qos
 **/
int i40e_ndo_set_vf_port_vlan(struct net_device *netdev, int vf_id,
			      u16 vlan_id, u8 qos, __be16 vlan_proto)
{
	u16 vlanprio = vlan_id | (qos << I40E_VLAN_PRIORITY_SHIFT);
	struct i40e_netdev_priv *np = netdev_priv(netdev);
	struct i40e_pf *pf = np->vsi->back;
	struct i40e_vsi *vsi;
	struct i40e_vf *vf;
	int ret = 0;

	/* validate the request */
	if (vf_id >= pf->num_alloc_vfs) {
		dev_err(&pf->pdev->dev, "Invalid VF Identifier %d\n", vf_id);
		ret = -EINVAL;
		goto error_pvid;
	}

	if ((vlan_id > I40E_MAX_VLANID) || (qos > 7)) {
		dev_err(&pf->pdev->dev, "Invalid VF Parameters\n");
		ret = -EINVAL;
		goto error_pvid;
	}

	if (vlan_proto != htons(ETH_P_8021Q)) {
		dev_err(&pf->pdev->dev, "VF VLAN protocol is not supported\n");
		ret = -EPROTONOSUPPORT;
		goto error_pvid;
	}

	vf = &(pf->vf[vf_id]);
	vsi = pf->vsi[vf->lan_vsi_idx];
	if (!test_bit(I40E_VF_STATE_INIT, &vf->vf_states)) {
		dev_err(&pf->pdev->dev, "VF %d still in reset. Try again.\n",
			vf_id);
		ret = -EAGAIN;
		goto error_pvid;
	}

	if (le16_to_cpu(vsi->info.pvid) == vlanprio)
		/* duplicate request, so just return success */
		goto error_pvid;

	i40e_vc_disable_vf(vf);
	/* During reset the VF got a new VSI, so refresh a pointer. */
	vsi = pf->vsi[vf->lan_vsi_idx];
	/* Locked once because multiple functions below iterate list */
	spin_lock_bh(&vsi->mac_filter_hash_lock);

	/* Check for condition where there was already a port VLAN ID
	 * filter set and now it is being deleted by setting it to zero.
	 * Additionally check for the condition where there was a port
	 * VLAN but now there is a new and different port VLAN being set.
	 * Before deleting all the old VLAN filters we must add new ones
	 * with -1 (I40E_VLAN_ANY) or otherwise we're left with all our
	 * MAC addresses deleted.
	 */
	if ((!(vlan_id || qos) ||
	    vlanprio != le16_to_cpu(vsi->info.pvid)) &&
	    vsi->info.pvid) {
		ret = i40e_add_vlan_all_mac(vsi, I40E_VLAN_ANY);
		if (ret) {
			dev_info(&vsi->back->pdev->dev,
				 "add VF VLAN failed, ret=%d aq_err=%d\n", ret,
				 vsi->back->hw.aq.asq_last_status);
			spin_unlock_bh(&vsi->mac_filter_hash_lock);
			goto error_pvid;
		}
	}

	if (vsi->info.pvid) {
		/* remove all filters on the old VLAN */
		i40e_rm_vlan_all_mac(vsi, (le16_to_cpu(vsi->info.pvid) &
					   VLAN_VID_MASK));
	}

	spin_unlock_bh(&vsi->mac_filter_hash_lock);
	if (vlan_id || qos)
		ret = i40e_vsi_add_pvid(vsi, vlanprio);
	else
		i40e_vsi_remove_pvid(vsi);
	spin_lock_bh(&vsi->mac_filter_hash_lock);

	if (vlan_id) {
		dev_info(&pf->pdev->dev, "Setting VLAN %d, QOS 0x%x on VF %d\n",
			 vlan_id, qos, vf_id);

		/* add new VLAN filter for each MAC */
		ret = i40e_add_vlan_all_mac(vsi, vlan_id);
		if (ret) {
			dev_info(&vsi->back->pdev->dev,
				 "add VF VLAN failed, ret=%d aq_err=%d\n", ret,
				 vsi->back->hw.aq.asq_last_status);
			spin_unlock_bh(&vsi->mac_filter_hash_lock);
			goto error_pvid;
		}

		/* remove the previously added non-VLAN MAC filters */
		i40e_rm_vlan_all_mac(vsi, I40E_VLAN_ANY);
	}

	spin_unlock_bh(&vsi->mac_filter_hash_lock);

	/* Schedule the worker thread to take care of applying changes */
	i40e_service_event_schedule(vsi->back);

	if (ret) {
		dev_err(&pf->pdev->dev, "Unable to update VF vsi context\n");
		goto error_pvid;
	}

	/* The Port VLAN needs to be saved across resets the same as the
	 * default LAN MAC address.
	 */
	vf->port_vlan_id = le16_to_cpu(vsi->info.pvid);
	ret = 0;

error_pvid:
	return ret;
}

/**
 * i40e_ndo_set_vf_bw
 * @netdev: network interface device structure
 * @vf_id: VF identifier
 * @tx_rate: Tx rate
 *
 * configure VF Tx rate
 **/
int i40e_ndo_set_vf_bw(struct net_device *netdev, int vf_id, int min_tx_rate,
		       int max_tx_rate)
{
	struct i40e_netdev_priv *np = netdev_priv(netdev);
	struct i40e_pf *pf = np->vsi->back;
	struct i40e_vsi *vsi;
	struct i40e_vf *vf;
	int ret = 0;

	/* validate the request */
	if (vf_id >= pf->num_alloc_vfs) {
		dev_err(&pf->pdev->dev, "Invalid VF Identifier %d.\n", vf_id);
		ret = -EINVAL;
		goto error;
	}

	if (min_tx_rate) {
		dev_err(&pf->pdev->dev, "Invalid min tx rate (%d) (greater than 0) specified for VF %d.\n",
			min_tx_rate, vf_id);
		return -EINVAL;
	}

	vf = &(pf->vf[vf_id]);
	vsi = pf->vsi[vf->lan_vsi_idx];
	if (!test_bit(I40E_VF_STATE_INIT, &vf->vf_states)) {
		dev_err(&pf->pdev->dev, "VF %d still in reset. Try again.\n",
			vf_id);
		ret = -EAGAIN;
		goto error;
	}

	ret = i40e_set_bw_limit(vsi, vsi->seid, max_tx_rate);
	if (ret)
		goto error;

	vf->tx_rate = max_tx_rate;
error:
	return ret;
}

/**
 * i40e_ndo_get_vf_config
 * @netdev: network interface device structure
 * @vf_id: VF identifier
 * @ivi: VF configuration structure
 *
 * return VF configuration
 **/
int i40e_ndo_get_vf_config(struct net_device *netdev,
			   int vf_id, struct ifla_vf_info *ivi)
{
	struct i40e_netdev_priv *np = netdev_priv(netdev);
	struct i40e_vsi *vsi = np->vsi;
	struct i40e_pf *pf = vsi->back;
	struct i40e_vf *vf;
	int ret = 0;

	/* validate the request */
	if (vf_id >= pf->num_alloc_vfs) {
		dev_err(&pf->pdev->dev, "Invalid VF Identifier %d\n", vf_id);
		ret = -EINVAL;
		goto error_param;
	}

	vf = &(pf->vf[vf_id]);
	/* first vsi is always the LAN vsi */
	vsi = pf->vsi[vf->lan_vsi_idx];
	if (!test_bit(I40E_VF_STATE_INIT, &vf->vf_states)) {
		dev_err(&pf->pdev->dev, "VF %d still in reset. Try again.\n",
			vf_id);
		ret = -EAGAIN;
		goto error_param;
	}

	ivi->vf = vf_id;

	ether_addr_copy(ivi->mac, vf->default_lan_addr.addr);

	ivi->max_tx_rate = vf->tx_rate;
	ivi->min_tx_rate = 0;
	ivi->vlan = le16_to_cpu(vsi->info.pvid) & I40E_VLAN_MASK;
	ivi->qos = (le16_to_cpu(vsi->info.pvid) & I40E_PRIORITY_MASK) >>
		   I40E_VLAN_PRIORITY_SHIFT;
	if (vf->link_forced == false)
		ivi->linkstate = IFLA_VF_LINK_STATE_AUTO;
	else if (vf->link_up == true)
		ivi->linkstate = IFLA_VF_LINK_STATE_ENABLE;
	else
		ivi->linkstate = IFLA_VF_LINK_STATE_DISABLE;
	ivi->spoofchk = vf->spoofchk;
	ivi->trusted = vf->trusted;
	ret = 0;

error_param:
	return ret;
}

/**
 * i40e_ndo_set_vf_link_state
 * @netdev: network interface device structure
 * @vf_id: VF identifier
 * @link: required link state
 *
 * Set the link state of a specified VF, regardless of physical link state
 **/
int i40e_ndo_set_vf_link_state(struct net_device *netdev, int vf_id, int link)
{
	struct i40e_netdev_priv *np = netdev_priv(netdev);
	struct i40e_pf *pf = np->vsi->back;
	struct virtchnl_pf_event pfe;
	struct i40e_hw *hw = &pf->hw;
	struct i40e_vf *vf;
	int abs_vf_id;
	int ret = 0;

	/* validate the request */
	if (vf_id >= pf->num_alloc_vfs) {
		dev_err(&pf->pdev->dev, "Invalid VF Identifier %d\n", vf_id);
		ret = -EINVAL;
		goto error_out;
	}

	vf = &pf->vf[vf_id];
	abs_vf_id = vf->vf_id + hw->func_caps.vf_base_id;

	pfe.event = VIRTCHNL_EVENT_LINK_CHANGE;
	pfe.severity = PF_EVENT_SEVERITY_INFO;

	switch (link) {
	case IFLA_VF_LINK_STATE_AUTO:
		vf->link_forced = false;
		pfe.event_data.link_event.link_status =
			pf->hw.phy.link_info.link_info & I40E_AQ_LINK_UP;
		pfe.event_data.link_event.link_speed =
			(enum virtchnl_link_speed)
			pf->hw.phy.link_info.link_speed;
		break;
	case IFLA_VF_LINK_STATE_ENABLE:
		vf->link_forced = true;
		vf->link_up = true;
		pfe.event_data.link_event.link_status = true;
		pfe.event_data.link_event.link_speed = VIRTCHNL_LINK_SPEED_40GB;
		break;
	case IFLA_VF_LINK_STATE_DISABLE:
		vf->link_forced = true;
		vf->link_up = false;
		pfe.event_data.link_event.link_status = false;
		pfe.event_data.link_event.link_speed = 0;
		break;
	default:
		ret = -EINVAL;
		goto error_out;
	}
	/* Notify the VF of its new link state */
	i40e_aq_send_msg_to_vf(hw, abs_vf_id, VIRTCHNL_OP_EVENT,
			       0, (u8 *)&pfe, sizeof(pfe), NULL);

error_out:
	return ret;
}

/**
 * i40e_ndo_set_vf_spoofchk
 * @netdev: network interface device structure
 * @vf_id: VF identifier
 * @enable: flag to enable or disable feature
 *
 * Enable or disable VF spoof checking
 **/
int i40e_ndo_set_vf_spoofchk(struct net_device *netdev, int vf_id, bool enable)
{
	struct i40e_netdev_priv *np = netdev_priv(netdev);
	struct i40e_vsi *vsi = np->vsi;
	struct i40e_pf *pf = vsi->back;
	struct i40e_vsi_context ctxt;
	struct i40e_hw *hw = &pf->hw;
	struct i40e_vf *vf;
	int ret = 0;

	/* validate the request */
	if (vf_id >= pf->num_alloc_vfs) {
		dev_err(&pf->pdev->dev, "Invalid VF Identifier %d\n", vf_id);
		ret = -EINVAL;
		goto out;
	}

	vf = &(pf->vf[vf_id]);
	if (!test_bit(I40E_VF_STATE_INIT, &vf->vf_states)) {
		dev_err(&pf->pdev->dev, "VF %d still in reset. Try again.\n",
			vf_id);
		ret = -EAGAIN;
		goto out;
	}

	if (enable == vf->spoofchk)
		goto out;

	vf->spoofchk = enable;
	memset(&ctxt, 0, sizeof(ctxt));
	ctxt.seid = pf->vsi[vf->lan_vsi_idx]->seid;
	ctxt.pf_num = pf->hw.pf_id;
	ctxt.info.valid_sections = cpu_to_le16(I40E_AQ_VSI_PROP_SECURITY_VALID);
	if (enable)
		ctxt.info.sec_flags |= (I40E_AQ_VSI_SEC_FLAG_ENABLE_VLAN_CHK |
					I40E_AQ_VSI_SEC_FLAG_ENABLE_MAC_CHK);
	ret = i40e_aq_update_vsi_params(hw, &ctxt, NULL);
	if (ret) {
		dev_err(&pf->pdev->dev, "Error %d updating VSI parameters\n",
			ret);
		ret = -EIO;
	}
out:
	return ret;
}

/**
 * i40e_ndo_set_vf_trust
 * @netdev: network interface device structure of the pf
 * @vf_id: VF identifier
 * @setting: trust setting
 *
 * Enable or disable VF trust setting
 **/
int i40e_ndo_set_vf_trust(struct net_device *netdev, int vf_id, bool setting)
{
	struct i40e_netdev_priv *np = netdev_priv(netdev);
	struct i40e_pf *pf = np->vsi->back;
	struct i40e_vf *vf;
	int ret = 0;

	/* validate the request */
	if (vf_id >= pf->num_alloc_vfs) {
		dev_err(&pf->pdev->dev, "Invalid VF Identifier %d\n", vf_id);
		return -EINVAL;
	}

	if (pf->flags & I40E_FLAG_MFP_ENABLED) {
		dev_err(&pf->pdev->dev, "Trusted VF not supported in MFP mode.\n");
		return -EINVAL;
	}

	vf = &pf->vf[vf_id];

	if (setting == vf->trusted)
		goto out;

	vf->trusted = setting;
	i40e_vc_disable_vf(vf);
	dev_info(&pf->pdev->dev, "VF %u is now %strusted\n",
		 vf_id, setting ? "" : "un");
out:
	return ret;
}
