/*******************************************************************************
 *
 * Intel Ethernet Controller XL710 Family Linux Virtual Function Driver
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

#include <linux/prefetch.h>
#include <net/busy_poll.h>

#include "i40evf.h"
#include "i40e_trace.h"
#include "i40e_prototype.h"

static inline __le64 build_ctob(u32 td_cmd, u32 td_offset, unsigned int size,
				u32 td_tag)
{
	return cpu_to_le64(I40E_TX_DESC_DTYPE_DATA |
			   ((u64)td_cmd  << I40E_TXD_QW1_CMD_SHIFT) |
			   ((u64)td_offset << I40E_TXD_QW1_OFFSET_SHIFT) |
			   ((u64)size  << I40E_TXD_QW1_TX_BUF_SZ_SHIFT) |
			   ((u64)td_tag  << I40E_TXD_QW1_L2TAG1_SHIFT));
}

#define I40E_TXD_CMD (I40E_TX_DESC_CMD_EOP | I40E_TX_DESC_CMD_RS)

/**
 * i40e_unmap_and_free_tx_resource - Release a Tx buffer
 * @ring:      the ring that owns the buffer
 * @tx_buffer: the buffer to free
 **/
static void i40e_unmap_and_free_tx_resource(struct i40e_ring *ring,
					    struct i40e_tx_buffer *tx_buffer)
{
	if (tx_buffer->skb) {
		if (tx_buffer->tx_flags & I40E_TX_FLAGS_FD_SB)
			kfree(tx_buffer->raw_buf);
		else
			dev_kfree_skb_any(tx_buffer->skb);
		if (dma_unmap_len(tx_buffer, len))
			dma_unmap_single(ring->dev,
					 dma_unmap_addr(tx_buffer, dma),
					 dma_unmap_len(tx_buffer, len),
					 DMA_TO_DEVICE);
	} else if (dma_unmap_len(tx_buffer, len)) {
		dma_unmap_page(ring->dev,
			       dma_unmap_addr(tx_buffer, dma),
			       dma_unmap_len(tx_buffer, len),
			       DMA_TO_DEVICE);
	}

	tx_buffer->next_to_watch = NULL;
	tx_buffer->skb = NULL;
	dma_unmap_len_set(tx_buffer, len, 0);
	/* tx_buffer must be completely set up in the transmit path */
}

/**
 * i40evf_clean_tx_ring - Free any empty Tx buffers
 * @tx_ring: ring to be cleaned
 **/
void i40evf_clean_tx_ring(struct i40e_ring *tx_ring)
{
	unsigned long bi_size;
	u16 i;

	/* ring already cleared, nothing to do */
	if (!tx_ring->tx_bi)
		return;

	/* Free all the Tx ring sk_buffs */
	for (i = 0; i < tx_ring->count; i++)
		i40e_unmap_and_free_tx_resource(tx_ring, &tx_ring->tx_bi[i]);

	bi_size = sizeof(struct i40e_tx_buffer) * tx_ring->count;
	memset(tx_ring->tx_bi, 0, bi_size);

	/* Zero out the descriptor ring */
	memset(tx_ring->desc, 0, tx_ring->size);

	tx_ring->next_to_use = 0;
	tx_ring->next_to_clean = 0;

	if (!tx_ring->netdev)
		return;

	/* cleanup Tx queue statistics */
	netdev_tx_reset_queue(txring_txq(tx_ring));
}

/**
 * i40evf_free_tx_resources - Free Tx resources per queue
 * @tx_ring: Tx descriptor ring for a specific queue
 *
 * Free all transmit software resources
 **/
void i40evf_free_tx_resources(struct i40e_ring *tx_ring)
{
	i40evf_clean_tx_ring(tx_ring);
	kfree(tx_ring->tx_bi);
	tx_ring->tx_bi = NULL;

	if (tx_ring->desc) {
		dma_free_coherent(tx_ring->dev, tx_ring->size,
				  tx_ring->desc, tx_ring->dma);
		tx_ring->desc = NULL;
	}
}

/**
 * i40evf_get_tx_pending - how many Tx descriptors not processed
 * @tx_ring: the ring of descriptors
 * @in_sw: is tx_pending being checked in SW or HW
 *
 * Since there is no access to the ring head register
 * in XL710, we need to use our local copies
 **/
u32 i40evf_get_tx_pending(struct i40e_ring *ring, bool in_sw)
{
	u32 head, tail;

	/* underlying hardware might not allow access and/or always return
	 * 0 for the head/tail registers so just use the cached values
	 */
	head = ring->next_to_clean;
	tail = ring->next_to_use;

	if (head != tail)
		return (head < tail) ?
			tail - head : (tail + ring->count - head);

	return 0;
}

#define WB_STRIDE 4

/**
 * i40e_clean_tx_irq - Reclaim resources after transmit completes
 * @vsi: the VSI we care about
 * @tx_ring: Tx ring to clean
 * @napi_budget: Used to determine if we are in netpoll
 *
 * Returns true if there's any budget left (e.g. the clean is finished)
 **/
static bool i40e_clean_tx_irq(struct i40e_vsi *vsi,
			      struct i40e_ring *tx_ring, int napi_budget)
{
	u16 i = tx_ring->next_to_clean;
	struct i40e_tx_buffer *tx_buf;
	struct i40e_tx_desc *tx_desc;
	unsigned int total_bytes = 0, total_packets = 0;
	unsigned int budget = vsi->work_limit;

	tx_buf = &tx_ring->tx_bi[i];
	tx_desc = I40E_TX_DESC(tx_ring, i);
	i -= tx_ring->count;

	do {
		struct i40e_tx_desc *eop_desc = tx_buf->next_to_watch;

		/* if next_to_watch is not set then there is no work pending */
		if (!eop_desc)
			break;

		/* prevent any other reads prior to eop_desc */
		smp_rmb();

		i40e_trace(clean_tx_irq, tx_ring, tx_desc, tx_buf);
		/* if the descriptor isn't done, no work yet to do */
		if (!(eop_desc->cmd_type_offset_bsz &
		      cpu_to_le64(I40E_TX_DESC_DTYPE_DESC_DONE)))
			break;

		/* clear next_to_watch to prevent false hangs */
		tx_buf->next_to_watch = NULL;

		/* update the statistics for this packet */
		total_bytes += tx_buf->bytecount;
		total_packets += tx_buf->gso_segs;

		/* free the skb */
		napi_consume_skb(tx_buf->skb, napi_budget);

		/* unmap skb header data */
		dma_unmap_single(tx_ring->dev,
				 dma_unmap_addr(tx_buf, dma),
				 dma_unmap_len(tx_buf, len),
				 DMA_TO_DEVICE);

		/* clear tx_buffer data */
		tx_buf->skb = NULL;
		dma_unmap_len_set(tx_buf, len, 0);

		/* unmap remaining buffers */
		while (tx_desc != eop_desc) {
			i40e_trace(clean_tx_irq_unmap,
				   tx_ring, tx_desc, tx_buf);

			tx_buf++;
			tx_desc++;
			i++;
			if (unlikely(!i)) {
				i -= tx_ring->count;
				tx_buf = tx_ring->tx_bi;
				tx_desc = I40E_TX_DESC(tx_ring, 0);
			}

			/* unmap any remaining paged data */
			if (dma_unmap_len(tx_buf, len)) {
				dma_unmap_page(tx_ring->dev,
					       dma_unmap_addr(tx_buf, dma),
					       dma_unmap_len(tx_buf, len),
					       DMA_TO_DEVICE);
				dma_unmap_len_set(tx_buf, len, 0);
			}
		}

		/* move us one more past the eop_desc for start of next pkt */
		tx_buf++;
		tx_desc++;
		i++;
		if (unlikely(!i)) {
			i -= tx_ring->count;
			tx_buf = tx_ring->tx_bi;
			tx_desc = I40E_TX_DESC(tx_ring, 0);
		}

		prefetch(tx_desc);

		/* update budget accounting */
		budget--;
	} while (likely(budget));

	i += tx_ring->count;
	tx_ring->next_to_clean = i;
	u64_stats_update_begin(&tx_ring->syncp);
	tx_ring->stats.bytes += total_bytes;
	tx_ring->stats.packets += total_packets;
	u64_stats_update_end(&tx_ring->syncp);
	tx_ring->q_vector->tx.total_bytes += total_bytes;
	tx_ring->q_vector->tx.total_packets += total_packets;

	if (tx_ring->flags & I40E_TXR_FLAGS_WB_ON_ITR) {
		/* check to see if there are < 4 descriptors
		 * waiting to be written back, then kick the hardware to force
		 * them to be written back in case we stay in NAPI.
		 * In this mode on X722 we do not enable Interrupt.
		 */
		unsigned int j = i40evf_get_tx_pending(tx_ring, false);

		if (budget &&
		    ((j / WB_STRIDE) == 0) && (j > 0) &&
		    !test_bit(__I40E_VSI_DOWN, vsi->state) &&
		    (I40E_DESC_UNUSED(tx_ring) != tx_ring->count))
			tx_ring->arm_wb = true;
	}

	/* notify netdev of completed buffers */
	netdev_tx_completed_queue(txring_txq(tx_ring),
				  total_packets, total_bytes);

#define TX_WAKE_THRESHOLD ((s16)(DESC_NEEDED * 2))
	if (unlikely(total_packets && netif_carrier_ok(tx_ring->netdev) &&
		     (I40E_DESC_UNUSED(tx_ring) >= TX_WAKE_THRESHOLD))) {
		/* Make sure that anybody stopping the queue after this
		 * sees the new next_to_clean.
		 */
		smp_mb();
		if (__netif_subqueue_stopped(tx_ring->netdev,
					     tx_ring->queue_index) &&
		   !test_bit(__I40E_VSI_DOWN, vsi->state)) {
			netif_wake_subqueue(tx_ring->netdev,
					    tx_ring->queue_index);
			++tx_ring->tx_stats.restart_queue;
		}
	}

	return !!budget;
}

/**
 * i40evf_enable_wb_on_itr - Arm hardware to do a wb, interrupts are not enabled
 * @vsi: the VSI we care about
 * @q_vector: the vector on which to enable writeback
 *
 **/
static void i40e_enable_wb_on_itr(struct i40e_vsi *vsi,
				  struct i40e_q_vector *q_vector)
{
	u16 flags = q_vector->tx.ring[0].flags;
	u32 val;

	if (!(flags & I40E_TXR_FLAGS_WB_ON_ITR))
		return;

	if (q_vector->arm_wb_state)
		return;

	val = I40E_VFINT_DYN_CTLN1_WB_ON_ITR_MASK |
	      I40E_VFINT_DYN_CTLN1_ITR_INDX_MASK; /* set noitr */

	wr32(&vsi->back->hw,
	     I40E_VFINT_DYN_CTLN1(q_vector->v_idx +
				  vsi->base_vector - 1), val);
	q_vector->arm_wb_state = true;
}

/**
 * i40evf_force_wb - Issue SW Interrupt so HW does a wb
 * @vsi: the VSI we care about
 * @q_vector: the vector  on which to force writeback
 *
 **/
void i40evf_force_wb(struct i40e_vsi *vsi, struct i40e_q_vector *q_vector)
{
	u32 val = I40E_VFINT_DYN_CTLN1_INTENA_MASK |
		  I40E_VFINT_DYN_CTLN1_ITR_INDX_MASK | /* set noitr */
		  I40E_VFINT_DYN_CTLN1_SWINT_TRIG_MASK |
		  I40E_VFINT_DYN_CTLN1_SW_ITR_INDX_ENA_MASK
		  /* allow 00 to be written to the index */;

	wr32(&vsi->back->hw,
	     I40E_VFINT_DYN_CTLN1(q_vector->v_idx + vsi->base_vector - 1),
	     val);
}

/**
 * i40e_set_new_dynamic_itr - Find new ITR level
 * @rc: structure containing ring performance data
 *
 * Returns true if ITR changed, false if not
 *
 * Stores a new ITR value based on packets and byte counts during
 * the last interrupt.  The advantage of per interrupt computation
 * is faster updates and more accurate ITR for the current traffic
 * pattern.  Constants in this function were computed based on
 * theoretical maximum wire speed and thresholds were set based on
 * testing data as well as attempting to minimize response time
 * while increasing bulk throughput.
 **/
static bool i40e_set_new_dynamic_itr(struct i40e_ring_container *rc)
{
	enum i40e_latency_range new_latency_range = rc->latency_range;
	u32 new_itr = rc->itr;
	int bytes_per_usec;
	unsigned int usecs, estimated_usecs;

	if (rc->total_packets == 0 || !rc->itr)
		return false;

	usecs = (rc->itr << 1) * ITR_COUNTDOWN_START;
	bytes_per_usec = rc->total_bytes / usecs;

	/* The calculations in this algorithm depend on interrupts actually
	 * firing at the ITR rate. This may not happen if the packet rate is
	 * really low, or if we've been napi polling. Check to make sure
	 * that's not the case before we continue.
	 */
	estimated_usecs = jiffies_to_usecs(jiffies - rc->last_itr_update);
	if (estimated_usecs > usecs) {
		new_latency_range = I40E_LOW_LATENCY;
		goto reset_latency;
	}

	/* simple throttlerate management
	 *   0-10MB/s   lowest (50000 ints/s)
	 *  10-20MB/s   low    (20000 ints/s)
	 *  20-1249MB/s bulk   (18000 ints/s)
	 *
	 * The math works out because the divisor is in 10^(-6) which
	 * turns the bytes/us input value into MB/s values, but
	 * make sure to use usecs, as the register values written
	 * are in 2 usec increments in the ITR registers, and make sure
	 * to use the smoothed values that the countdown timer gives us.
	 */
	switch (new_latency_range) {
	case I40E_LOWEST_LATENCY:
		if (bytes_per_usec > 10)
			new_latency_range = I40E_LOW_LATENCY;
		break;
	case I40E_LOW_LATENCY:
		if (bytes_per_usec > 20)
			new_latency_range = I40E_BULK_LATENCY;
		else if (bytes_per_usec <= 10)
			new_latency_range = I40E_LOWEST_LATENCY;
		break;
	case I40E_BULK_LATENCY:
	default:
		if (bytes_per_usec <= 20)
			new_latency_range = I40E_LOW_LATENCY;
		break;
	}

reset_latency:
	rc->latency_range = new_latency_range;

	switch (new_latency_range) {
	case I40E_LOWEST_LATENCY:
		new_itr = I40E_ITR_50K;
		break;
	case I40E_LOW_LATENCY:
		new_itr = I40E_ITR_20K;
		break;
	case I40E_BULK_LATENCY:
		new_itr = I40E_ITR_18K;
		break;
	default:
		break;
	}

	rc->total_bytes = 0;
	rc->total_packets = 0;
	rc->last_itr_update = jiffies;

	if (new_itr != rc->itr) {
		rc->itr = new_itr;
		return true;
	}
	return false;
}

/**
 * i40evf_setup_tx_descriptors - Allocate the Tx descriptors
 * @tx_ring: the tx ring to set up
 *
 * Return 0 on success, negative on error
 **/
int i40evf_setup_tx_descriptors(struct i40e_ring *tx_ring)
{
	struct device *dev = tx_ring->dev;
	int bi_size;

	if (!dev)
		return -ENOMEM;

	/* warn if we are about to overwrite the pointer */
	WARN_ON(tx_ring->tx_bi);
	bi_size = sizeof(struct i40e_tx_buffer) * tx_ring->count;
	tx_ring->tx_bi = kzalloc(bi_size, GFP_KERNEL);
	if (!tx_ring->tx_bi)
		goto err;

	/* round up to nearest 4K */
	tx_ring->size = tx_ring->count * sizeof(struct i40e_tx_desc);
	tx_ring->size = ALIGN(tx_ring->size, 4096);
	tx_ring->desc = dma_alloc_coherent(dev, tx_ring->size,
					   &tx_ring->dma, GFP_KERNEL);
	if (!tx_ring->desc) {
		dev_info(dev, "Unable to allocate memory for the Tx descriptor ring, size=%d\n",
			 tx_ring->size);
		goto err;
	}

	tx_ring->next_to_use = 0;
	tx_ring->next_to_clean = 0;
	return 0;

err:
	kfree(tx_ring->tx_bi);
	tx_ring->tx_bi = NULL;
	return -ENOMEM;
}

/**
 * i40evf_clean_rx_ring - Free Rx buffers
 * @rx_ring: ring to be cleaned
 **/
void i40evf_clean_rx_ring(struct i40e_ring *rx_ring)
{
	unsigned long bi_size;
	u16 i;

	/* ring already cleared, nothing to do */
	if (!rx_ring->rx_bi)
		return;

	if (rx_ring->skb) {
		dev_kfree_skb(rx_ring->skb);
		rx_ring->skb = NULL;
	}

	/* Free all the Rx ring sk_buffs */
	for (i = 0; i < rx_ring->count; i++) {
		struct i40e_rx_buffer *rx_bi = &rx_ring->rx_bi[i];

		if (!rx_bi->page)
			continue;

		/* Invalidate cache lines that may have been written to by
		 * device so that we avoid corrupting memory.
		 */
		dma_sync_single_range_for_cpu(rx_ring->dev,
					      rx_bi->dma,
					      rx_bi->page_offset,
					      rx_ring->rx_buf_len,
					      DMA_FROM_DEVICE);

		/* free resources associated with mapping */
		dma_unmap_page_attrs(rx_ring->dev, rx_bi->dma,
				     i40e_rx_pg_size(rx_ring),
				     DMA_FROM_DEVICE,
				     I40E_RX_DMA_ATTR);

		__page_frag_cache_drain(rx_bi->page, rx_bi->pagecnt_bias);

		rx_bi->page = NULL;
		rx_bi->page_offset = 0;
	}

	bi_size = sizeof(struct i40e_rx_buffer) * rx_ring->count;
	memset(rx_ring->rx_bi, 0, bi_size);

	/* Zero out the descriptor ring */
	memset(rx_ring->desc, 0, rx_ring->size);

	rx_ring->next_to_alloc = 0;
	rx_ring->next_to_clean = 0;
	rx_ring->next_to_use = 0;
}

/**
 * i40evf_free_rx_resources - Free Rx resources
 * @rx_ring: ring to clean the resources from
 *
 * Free all receive software resources
 **/
void i40evf_free_rx_resources(struct i40e_ring *rx_ring)
{
	i40evf_clean_rx_ring(rx_ring);
	kfree(rx_ring->rx_bi);
	rx_ring->rx_bi = NULL;

	if (rx_ring->desc) {
		dma_free_coherent(rx_ring->dev, rx_ring->size,
				  rx_ring->desc, rx_ring->dma);
		rx_ring->desc = NULL;
	}
}

/**
 * i40evf_setup_rx_descriptors - Allocate Rx descriptors
 * @rx_ring: Rx descriptor ring (for a specific queue) to setup
 *
 * Returns 0 on success, negative on failure
 **/
int i40evf_setup_rx_descriptors(struct i40e_ring *rx_ring)
{
	struct device *dev = rx_ring->dev;
	int bi_size;

	/* warn if we are about to overwrite the pointer */
	WARN_ON(rx_ring->rx_bi);
	bi_size = sizeof(struct i40e_rx_buffer) * rx_ring->count;
	rx_ring->rx_bi = kzalloc(bi_size, GFP_KERNEL);
	if (!rx_ring->rx_bi)
		goto err;

	u64_stats_init(&rx_ring->syncp);

	/* Round up to nearest 4K */
	rx_ring->size = rx_ring->count * sizeof(union i40e_32byte_rx_desc);
	rx_ring->size = ALIGN(rx_ring->size, 4096);
	rx_ring->desc = dma_alloc_coherent(dev, rx_ring->size,
					   &rx_ring->dma, GFP_KERNEL);

	if (!rx_ring->desc) {
		dev_info(dev, "Unable to allocate memory for the Rx descriptor ring, size=%d\n",
			 rx_ring->size);
		goto err;
	}

	rx_ring->next_to_alloc = 0;
	rx_ring->next_to_clean = 0;
	rx_ring->next_to_use = 0;

	return 0;
err:
	kfree(rx_ring->rx_bi);
	rx_ring->rx_bi = NULL;
	return -ENOMEM;
}

/**
 * i40e_release_rx_desc - Store the new tail and head values
 * @rx_ring: ring to bump
 * @val: new head index
 **/
static inline void i40e_release_rx_desc(struct i40e_ring *rx_ring, u32 val)
{
	rx_ring->next_to_use = val;

	/* update next to alloc since we have filled the ring */
	rx_ring->next_to_alloc = val;

	/* Force memory writes to complete before letting h/w
	 * know there are new descriptors to fetch.  (Only
	 * applicable for weak-ordered memory model archs,
	 * such as IA-64).
	 */
	wmb();
	writel(val, rx_ring->tail);
}

/**
 * i40e_rx_offset - Return expected offset into page to access data
 * @rx_ring: Ring we are requesting offset of
 *
 * Returns the offset value for ring into the data buffer.
 */
static inline unsigned int i40e_rx_offset(struct i40e_ring *rx_ring)
{
	return ring_uses_build_skb(rx_ring) ? I40E_SKB_PAD : 0;
}

/**
 * i40e_alloc_mapped_page - recycle or make a new page
 * @rx_ring: ring to use
 * @bi: rx_buffer struct to modify
 *
 * Returns true if the page was successfully allocated or
 * reused.
 **/
static bool i40e_alloc_mapped_page(struct i40e_ring *rx_ring,
				   struct i40e_rx_buffer *bi)
{
	struct page *page = bi->page;
	dma_addr_t dma;

	/* since we are recycling buffers we should seldom need to alloc */
	if (likely(page)) {
		rx_ring->rx_stats.page_reuse_count++;
		return true;
	}

	/* alloc new page for storage */
	page = dev_alloc_pages(i40e_rx_pg_order(rx_ring));
	if (unlikely(!page)) {
		rx_ring->rx_stats.alloc_page_failed++;
		return false;
	}

	/* map page for use */
	dma = dma_map_page_attrs(rx_ring->dev, page, 0,
				 i40e_rx_pg_size(rx_ring),
				 DMA_FROM_DEVICE,
				 I40E_RX_DMA_ATTR);

	/* if mapping failed free memory back to system since
	 * there isn't much point in holding memory we can't use
	 */
	if (dma_mapping_error(rx_ring->dev, dma)) {
		__free_pages(page, i40e_rx_pg_order(rx_ring));
		rx_ring->rx_stats.alloc_page_failed++;
		return false;
	}

	bi->dma = dma;
	bi->page = page;
	bi->page_offset = i40e_rx_offset(rx_ring);

	/* initialize pagecnt_bias to 1 representing we fully own page */
	bi->pagecnt_bias = 1;

	return true;
}

/**
 * i40e_receive_skb - Send a completed packet up the stack
 * @rx_ring:  rx ring in play
 * @skb: packet to send up
 * @vlan_tag: vlan tag for packet
 **/
static void i40e_receive_skb(struct i40e_ring *rx_ring,
			     struct sk_buff *skb, u16 vlan_tag)
{
	struct i40e_q_vector *q_vector = rx_ring->q_vector;

	if ((rx_ring->netdev->features & NETIF_F_HW_VLAN_CTAG_RX) &&
	    (vlan_tag & VLAN_VID_MASK))
		__vlan_hwaccel_put_tag(skb, htons(ETH_P_8021Q), vlan_tag);

	napi_gro_receive(&q_vector->napi, skb);
}

/**
 * i40evf_alloc_rx_buffers - Replace used receive buffers
 * @rx_ring: ring to place buffers on
 * @cleaned_count: number of buffers to replace
 *
 * Returns false if all allocations were successful, true if any fail
 **/
bool i40evf_alloc_rx_buffers(struct i40e_ring *rx_ring, u16 cleaned_count)
{
	u16 ntu = rx_ring->next_to_use;
	union i40e_rx_desc *rx_desc;
	struct i40e_rx_buffer *bi;

	/* do nothing if no valid netdev defined */
	if (!rx_ring->netdev || !cleaned_count)
		return false;

	rx_desc = I40E_RX_DESC(rx_ring, ntu);
	bi = &rx_ring->rx_bi[ntu];

	do {
		if (!i40e_alloc_mapped_page(rx_ring, bi))
			goto no_buffers;

		/* sync the buffer for use by the device */
		dma_sync_single_range_for_device(rx_ring->dev, bi->dma,
						 bi->page_offset,
						 rx_ring->rx_buf_len,
						 DMA_FROM_DEVICE);

		/* Refresh the desc even if buffer_addrs didn't change
		 * because each write-back erases this info.
		 */
		rx_desc->read.pkt_addr = cpu_to_le64(bi->dma + bi->page_offset);

		rx_desc++;
		bi++;
		ntu++;
		if (unlikely(ntu == rx_ring->count)) {
			rx_desc = I40E_RX_DESC(rx_ring, 0);
			bi = rx_ring->rx_bi;
			ntu = 0;
		}

		/* clear the status bits for the next_to_use descriptor */
		rx_desc->wb.qword1.status_error_len = 0;

		cleaned_count--;
	} while (cleaned_count);

	if (rx_ring->next_to_use != ntu)
		i40e_release_rx_desc(rx_ring, ntu);

	return false;

no_buffers:
	if (rx_ring->next_to_use != ntu)
		i40e_release_rx_desc(rx_ring, ntu);

	/* make sure to come back via polling to try again after
	 * allocation failure
	 */
	return true;
}

/**
 * i40e_rx_checksum - Indicate in skb if hw indicated a good cksum
 * @vsi: the VSI we care about
 * @skb: skb currently being received and modified
 * @rx_desc: the receive descriptor
 **/
static inline void i40e_rx_checksum(struct i40e_vsi *vsi,
				    struct sk_buff *skb,
				    union i40e_rx_desc *rx_desc)
{
	struct i40e_rx_ptype_decoded decoded;
	u32 rx_error, rx_status;
	bool ipv4, ipv6;
	u8 ptype;
	u64 qword;

	qword = le64_to_cpu(rx_desc->wb.qword1.status_error_len);
	ptype = (qword & I40E_RXD_QW1_PTYPE_MASK) >> I40E_RXD_QW1_PTYPE_SHIFT;
	rx_error = (qword & I40E_RXD_QW1_ERROR_MASK) >>
		   I40E_RXD_QW1_ERROR_SHIFT;
	rx_status = (qword & I40E_RXD_QW1_STATUS_MASK) >>
		    I40E_RXD_QW1_STATUS_SHIFT;
	decoded = decode_rx_desc_ptype(ptype);

	skb->ip_summed = CHECKSUM_NONE;

	skb_checksum_none_assert(skb);

	/* Rx csum enabled and ip headers found? */
	if (!(vsi->netdev->features & NETIF_F_RXCSUM))
		return;

	/* did the hardware decode the packet and checksum? */
	if (!(rx_status & BIT(I40E_RX_DESC_STATUS_L3L4P_SHIFT)))
		return;

	/* both known and outer_ip must be set for the below code to work */
	if (!(decoded.known && decoded.outer_ip))
		return;

	ipv4 = (decoded.outer_ip == I40E_RX_PTYPE_OUTER_IP) &&
	       (decoded.outer_ip_ver == I40E_RX_PTYPE_OUTER_IPV4);
	ipv6 = (decoded.outer_ip == I40E_RX_PTYPE_OUTER_IP) &&
	       (decoded.outer_ip_ver == I40E_RX_PTYPE_OUTER_IPV6);

	if (ipv4 &&
	    (rx_error & (BIT(I40E_RX_DESC_ERROR_IPE_SHIFT) |
			 BIT(I40E_RX_DESC_ERROR_EIPE_SHIFT))))
		goto checksum_fail;

	/* likely incorrect csum if alternate IP extension headers found */
	if (ipv6 &&
	    rx_status & BIT(I40E_RX_DESC_STATUS_IPV6EXADD_SHIFT))
		/* don't increment checksum err here, non-fatal err */
		return;

	/* there was some L4 error, count error and punt packet to the stack */
	if (rx_error & BIT(I40E_RX_DESC_ERROR_L4E_SHIFT))
		goto checksum_fail;

	/* handle packets that were not able to be checksummed due
	 * to arrival speed, in this case the stack can compute
	 * the csum.
	 */
	if (rx_error & BIT(I40E_RX_DESC_ERROR_PPRS_SHIFT))
		return;

	/* Only report checksum unnecessary for TCP, UDP, or SCTP */
	switch (decoded.inner_prot) {
	case I40E_RX_PTYPE_INNER_PROT_TCP:
	case I40E_RX_PTYPE_INNER_PROT_UDP:
	case I40E_RX_PTYPE_INNER_PROT_SCTP:
		skb->ip_summed = CHECKSUM_UNNECESSARY;
		/* fall though */
	default:
		break;
	}

	return;

checksum_fail:
	vsi->back->hw_csum_rx_error++;
}

/**
 * i40e_ptype_to_htype - get a hash type
 * @ptype: the ptype value from the descriptor
 *
 * Returns a hash type to be used by skb_set_hash
 **/
static inline int i40e_ptype_to_htype(u8 ptype)
{
	struct i40e_rx_ptype_decoded decoded = decode_rx_desc_ptype(ptype);

	if (!decoded.known)
		return PKT_HASH_TYPE_NONE;

	if (decoded.outer_ip == I40E_RX_PTYPE_OUTER_IP &&
	    decoded.payload_layer == I40E_RX_PTYPE_PAYLOAD_LAYER_PAY4)
		return PKT_HASH_TYPE_L4;
	else if (decoded.outer_ip == I40E_RX_PTYPE_OUTER_IP &&
		 decoded.payload_layer == I40E_RX_PTYPE_PAYLOAD_LAYER_PAY3)
		return PKT_HASH_TYPE_L3;
	else
		return PKT_HASH_TYPE_L2;
}

/**
 * i40e_rx_hash - set the hash value in the skb
 * @ring: descriptor ring
 * @rx_desc: specific descriptor
 **/
static inline void i40e_rx_hash(struct i40e_ring *ring,
				union i40e_rx_desc *rx_desc,
				struct sk_buff *skb,
				u8 rx_ptype)
{
	u32 hash;
	const __le64 rss_mask =
		cpu_to_le64((u64)I40E_RX_DESC_FLTSTAT_RSS_HASH <<
			    I40E_RX_DESC_STATUS_FLTSTAT_SHIFT);

	if (ring->netdev->features & NETIF_F_RXHASH)
		return;

	if ((rx_desc->wb.qword1.status_error_len & rss_mask) == rss_mask) {
		hash = le32_to_cpu(rx_desc->wb.qword0.hi_dword.rss);
		skb_set_hash(skb, hash, i40e_ptype_to_htype(rx_ptype));
	}
}

/**
 * i40evf_process_skb_fields - Populate skb header fields from Rx descriptor
 * @rx_ring: rx descriptor ring packet is being transacted on
 * @rx_desc: pointer to the EOP Rx descriptor
 * @skb: pointer to current skb being populated
 * @rx_ptype: the packet type decoded by hardware
 *
 * This function checks the ring, descriptor, and packet information in
 * order to populate the hash, checksum, VLAN, protocol, and
 * other fields within the skb.
 **/
static inline
void i40evf_process_skb_fields(struct i40e_ring *rx_ring,
			       union i40e_rx_desc *rx_desc, struct sk_buff *skb,
			       u8 rx_ptype)
{
	i40e_rx_hash(rx_ring, rx_desc, skb, rx_ptype);

	i40e_rx_checksum(rx_ring->vsi, skb, rx_desc);

	skb_record_rx_queue(skb, rx_ring->queue_index);

	/* modifies the skb - consumes the enet header */
	skb->protocol = eth_type_trans(skb, rx_ring->netdev);
}

/**
 * i40e_cleanup_headers - Correct empty headers
 * @rx_ring: rx descriptor ring packet is being transacted on
 * @skb: pointer to current skb being fixed
 *
 * Also address the case where we are pulling data in on pages only
 * and as such no data is present in the skb header.
 *
 * In addition if skb is not at least 60 bytes we need to pad it so that
 * it is large enough to qualify as a valid Ethernet frame.
 *
 * Returns true if an error was encountered and skb was freed.
 **/
static bool i40e_cleanup_headers(struct i40e_ring *rx_ring, struct sk_buff *skb)
{
	/* if eth_skb_pad returns an error the skb was freed */
	if (eth_skb_pad(skb))
		return true;

	return false;
}

/**
 * i40e_reuse_rx_page - page flip buffer and store it back on the ring
 * @rx_ring: rx descriptor ring to store buffers on
 * @old_buff: donor buffer to have page reused
 *
 * Synchronizes page for reuse by the adapter
 **/
static void i40e_reuse_rx_page(struct i40e_ring *rx_ring,
			       struct i40e_rx_buffer *old_buff)
{
	struct i40e_rx_buffer *new_buff;
	u16 nta = rx_ring->next_to_alloc;

	new_buff = &rx_ring->rx_bi[nta];

	/* update, and store next to alloc */
	nta++;
	rx_ring->next_to_alloc = (nta < rx_ring->count) ? nta : 0;

	/* transfer page from old buffer to new buffer */
	new_buff->dma		= old_buff->dma;
	new_buff->page		= old_buff->page;
	new_buff->page_offset	= old_buff->page_offset;
	new_buff->pagecnt_bias	= old_buff->pagecnt_bias;
}

/**
 * i40e_page_is_reusable - check if any reuse is possible
 * @page: page struct to check
 *
 * A page is not reusable if it was allocated under low memory
 * conditions, or it's not in the same NUMA node as this CPU.
 */
static inline bool i40e_page_is_reusable(struct page *page)
{
	return (page_to_nid(page) == numa_mem_id()) &&
		!page_is_pfmemalloc(page);
}

/**
 * i40e_can_reuse_rx_page - Determine if this page can be reused by
 * the adapter for another receive
 *
 * @rx_buffer: buffer containing the page
 *
 * If page is reusable, rx_buffer->page_offset is adjusted to point to
 * an unused region in the page.
 *
 * For small pages, @truesize will be a constant value, half the size
 * of the memory at page.  We'll attempt to alternate between high and
 * low halves of the page, with one half ready for use by the hardware
 * and the other half being consumed by the stack.  We use the page
 * ref count to determine whether the stack has finished consuming the
 * portion of this page that was passed up with a previous packet.  If
 * the page ref count is >1, we'll assume the "other" half page is
 * still busy, and this page cannot be reused.
 *
 * For larger pages, @truesize will be the actual space used by the
 * received packet (adjusted upward to an even multiple of the cache
 * line size).  This will advance through the page by the amount
 * actually consumed by the received packets while there is still
 * space for a buffer.  Each region of larger pages will be used at
 * most once, after which the page will not be reused.
 *
 * In either case, if the page is reusable its refcount is increased.
 **/
static bool i40e_can_reuse_rx_page(struct i40e_rx_buffer *rx_buffer)
{
	unsigned int pagecnt_bias = rx_buffer->pagecnt_bias;
	struct page *page = rx_buffer->page;

	/* Is any reuse possible? */
	if (unlikely(!i40e_page_is_reusable(page)))
		return false;

#if (PAGE_SIZE < 8192)
	/* if we are only owner of page we can reuse it */
	if (unlikely((page_count(page) - pagecnt_bias) > 1))
		return false;
#else
#define I40E_LAST_OFFSET \
	(SKB_WITH_OVERHEAD(PAGE_SIZE) - I40E_RXBUFFER_2048)
	if (rx_buffer->page_offset > I40E_LAST_OFFSET)
		return false;
#endif

	/* If we have drained the page fragment pool we need to update
	 * the pagecnt_bias and page count so that we fully restock the
	 * number of references the driver holds.
	 */
	if (unlikely(!pagecnt_bias)) {
		page_ref_add(page, USHRT_MAX);
		rx_buffer->pagecnt_bias = USHRT_MAX;
	}

	return true;
}

/**
 * i40e_add_rx_frag - Add contents of Rx buffer to sk_buff
 * @rx_ring: rx descriptor ring to transact packets on
 * @rx_buffer: buffer containing page to add
 * @skb: sk_buff to place the data into
 * @size: packet length from rx_desc
 *
 * This function will add the data contained in rx_buffer->page to the skb.
 * It will just attach the page as a frag to the skb.
 *
 * The function will then update the page offset.
 **/
static void i40e_add_rx_frag(struct i40e_ring *rx_ring,
			     struct i40e_rx_buffer *rx_buffer,
			     struct sk_buff *skb,
			     unsigned int size)
{
#if (PAGE_SIZE < 8192)
	unsigned int truesize = i40e_rx_pg_size(rx_ring) / 2;
#else
	unsigned int truesize = SKB_DATA_ALIGN(size + i40e_rx_offset(rx_ring));
#endif

	skb_add_rx_frag(skb, skb_shinfo(skb)->nr_frags, rx_buffer->page,
			rx_buffer->page_offset, size, truesize);

	/* page is being used so we must update the page offset */
#if (PAGE_SIZE < 8192)
	rx_buffer->page_offset ^= truesize;
#else
	rx_buffer->page_offset += truesize;
#endif
}

/**
 * i40e_get_rx_buffer - Fetch Rx buffer and synchronize data for use
 * @rx_ring: rx descriptor ring to transact packets on
 * @size: size of buffer to add to skb
 *
 * This function will pull an Rx buffer from the ring and synchronize it
 * for use by the CPU.
 */
static struct i40e_rx_buffer *i40e_get_rx_buffer(struct i40e_ring *rx_ring,
						 const unsigned int size)
{
	struct i40e_rx_buffer *rx_buffer;

	rx_buffer = &rx_ring->rx_bi[rx_ring->next_to_clean];
	prefetchw(rx_buffer->page);

	/* we are reusing so sync this buffer for CPU use */
	dma_sync_single_range_for_cpu(rx_ring->dev,
				      rx_buffer->dma,
				      rx_buffer->page_offset,
				      size,
				      DMA_FROM_DEVICE);

	/* We have pulled a buffer for use, so decrement pagecnt_bias */
	rx_buffer->pagecnt_bias--;

	return rx_buffer;
}

/**
 * i40e_construct_skb - Allocate skb and populate it
 * @rx_ring: rx descriptor ring to transact packets on
 * @rx_buffer: rx buffer to pull data from
 * @size: size of buffer to add to skb
 *
 * This function allocates an skb.  It then populates it with the page
 * data from the current receive descriptor, taking care to set up the
 * skb correctly.
 */
static struct sk_buff *i40e_construct_skb(struct i40e_ring *rx_ring,
					  struct i40e_rx_buffer *rx_buffer,
					  unsigned int size)
{
	void *va;
#if (PAGE_SIZE < 8192)
	unsigned int truesize = i40e_rx_pg_size(rx_ring) / 2;
#else
	unsigned int truesize = SKB_DATA_ALIGN(size);
#endif
	unsigned int headlen;
	struct sk_buff *skb;

	/* prefetch first cache line of first page */
	va = page_address(rx_buffer->page) + rx_buffer->page_offset;
	prefetch(va);
#if L1_CACHE_BYTES < 128
	prefetch(va + L1_CACHE_BYTES);
#endif

	/* allocate a skb to store the frags */
	skb = __napi_alloc_skb(&rx_ring->q_vector->napi,
			       I40E_RX_HDR_SIZE,
			       GFP_ATOMIC | __GFP_NOWARN);
	if (unlikely(!skb))
		return NULL;

	/* Determine available headroom for copy */
	headlen = size;
	if (headlen > I40E_RX_HDR_SIZE)
		headlen = eth_get_headlen(va, I40E_RX_HDR_SIZE);

	/* align pull length to size of long to optimize memcpy performance */
	memcpy(__skb_put(skb, headlen), va, ALIGN(headlen, sizeof(long)));

	/* update all of the pointers */
	size -= headlen;
	if (size) {
		skb_add_rx_frag(skb, 0, rx_buffer->page,
				rx_buffer->page_offset + headlen,
				size, truesize);

		/* buffer is used by skb, update page_offset */
#if (PAGE_SIZE < 8192)
		rx_buffer->page_offset ^= truesize;
#else
		rx_buffer->page_offset += truesize;
#endif
	} else {
		/* buffer is unused, reset bias back to rx_buffer */
		rx_buffer->pagecnt_bias++;
	}

	return skb;
}

/**
 * i40e_build_skb - Build skb around an existing buffer
 * @rx_ring: Rx descriptor ring to transact packets on
 * @rx_buffer: Rx buffer to pull data from
 * @size: size of buffer to add to skb
 *
 * This function builds an skb around an existing Rx buffer, taking care
 * to set up the skb correctly and avoid any memcpy overhead.
 */
static struct sk_buff *i40e_build_skb(struct i40e_ring *rx_ring,
				      struct i40e_rx_buffer *rx_buffer,
				      unsigned int size)
{
	void *va;
#if (PAGE_SIZE < 8192)
	unsigned int truesize = i40e_rx_pg_size(rx_ring) / 2;
#else
	unsigned int truesize = SKB_DATA_ALIGN(sizeof(struct skb_shared_info)) +
				SKB_DATA_ALIGN(I40E_SKB_PAD + size);
#endif
	struct sk_buff *skb;

	/* prefetch first cache line of first page */
	va = page_address(rx_buffer->page) + rx_buffer->page_offset;
	prefetch(va);
#if L1_CACHE_BYTES < 128
	prefetch(va + L1_CACHE_BYTES);
#endif
	/* build an skb around the page buffer */
	skb = build_skb(va - I40E_SKB_PAD, truesize);
	if (unlikely(!skb))
		return NULL;

	/* update pointers within the skb to store the data */
	skb_reserve(skb, I40E_SKB_PAD);
	__skb_put(skb, size);

	/* buffer is used by skb, update page_offset */
#if (PAGE_SIZE < 8192)
	rx_buffer->page_offset ^= truesize;
#else
	rx_buffer->page_offset += truesize;
#endif

	return skb;
}

/**
 * i40e_put_rx_buffer - Clean up used buffer and either recycle or free
 * @rx_ring: rx descriptor ring to transact packets on
 * @rx_buffer: rx buffer to pull data from
 *
 * This function will clean up the contents of the rx_buffer.  It will
 * either recycle the bufer or unmap it and free the associated resources.
 */
static void i40e_put_rx_buffer(struct i40e_ring *rx_ring,
			       struct i40e_rx_buffer *rx_buffer)
{
	if (i40e_can_reuse_rx_page(rx_buffer)) {
		/* hand second half of page back to the ring */
		i40e_reuse_rx_page(rx_ring, rx_buffer);
		rx_ring->rx_stats.page_reuse_count++;
	} else {
		/* we are not reusing the buffer so unmap it */
		dma_unmap_page_attrs(rx_ring->dev, rx_buffer->dma,
				     i40e_rx_pg_size(rx_ring),
				     DMA_FROM_DEVICE, I40E_RX_DMA_ATTR);
		__page_frag_cache_drain(rx_buffer->page,
					rx_buffer->pagecnt_bias);
	}

	/* clear contents of buffer_info */
	rx_buffer->page = NULL;
}

/**
 * i40e_is_non_eop - process handling of non-EOP buffers
 * @rx_ring: Rx ring being processed
 * @rx_desc: Rx descriptor for current buffer
 * @skb: Current socket buffer containing buffer in progress
 *
 * This function updates next to clean.  If the buffer is an EOP buffer
 * this function exits returning false, otherwise it will place the
 * sk_buff in the next buffer to be chained and return true indicating
 * that this is in fact a non-EOP buffer.
 **/
static bool i40e_is_non_eop(struct i40e_ring *rx_ring,
			    union i40e_rx_desc *rx_desc,
			    struct sk_buff *skb)
{
	u32 ntc = rx_ring->next_to_clean + 1;

	/* fetch, update, and store next to clean */
	ntc = (ntc < rx_ring->count) ? ntc : 0;
	rx_ring->next_to_clean = ntc;

	prefetch(I40E_RX_DESC(rx_ring, ntc));

	/* if we are the last buffer then there is nothing else to do */
#define I40E_RXD_EOF BIT(I40E_RX_DESC_STATUS_EOF_SHIFT)
	if (likely(i40e_test_staterr(rx_desc, I40E_RXD_EOF)))
		return false;

	rx_ring->rx_stats.non_eop_descs++;

	return true;
}

/**
 * i40e_clean_rx_irq - Clean completed descriptors from Rx ring - bounce buf
 * @rx_ring: rx descriptor ring to transact packets on
 * @budget: Total limit on number of packets to process
 *
 * This function provides a "bounce buffer" approach to Rx interrupt
 * processing.  The advantage to this is that on systems that have
 * expensive overhead for IOMMU access this provides a means of avoiding
 * it by maintaining the mapping of the page to the system.
 *
 * Returns amount of work completed
 **/
static int i40e_clean_rx_irq(struct i40e_ring *rx_ring, int budget)
{
	unsigned int total_rx_bytes = 0, total_rx_packets = 0;
	struct sk_buff *skb = rx_ring->skb;
	u16 cleaned_count = I40E_DESC_UNUSED(rx_ring);
	bool failure = false;

	while (likely(total_rx_packets < (unsigned int)budget)) {
		struct i40e_rx_buffer *rx_buffer;
		union i40e_rx_desc *rx_desc;
		unsigned int size;
		u16 vlan_tag;
		u8 rx_ptype;
		u64 qword;

		/* return some buffers to hardware, one at a time is too slow */
		if (cleaned_count >= I40E_RX_BUFFER_WRITE) {
			failure = failure ||
				  i40evf_alloc_rx_buffers(rx_ring, cleaned_count);
			cleaned_count = 0;
		}

		rx_desc = I40E_RX_DESC(rx_ring, rx_ring->next_to_clean);

		/* status_error_len will always be zero for unused descriptors
		 * because it's cleared in cleanup, and overlaps with hdr_addr
		 * which is always zero because packet split isn't used, if the
		 * hardware wrote DD then the length will be non-zero
		 */
		qword = le64_to_cpu(rx_desc->wb.qword1.status_error_len);

		/* This memory barrier is needed to keep us from reading
		 * any other fields out of the rx_desc until we have
		 * verified the descriptor has been written back.
		 */
		dma_rmb();

		size = (qword & I40E_RXD_QW1_LENGTH_PBUF_MASK) >>
		       I40E_RXD_QW1_LENGTH_PBUF_SHIFT;
		if (!size)
			break;

		i40e_trace(clean_rx_irq, rx_ring, rx_desc, skb);
		rx_buffer = i40e_get_rx_buffer(rx_ring, size);

		/* retrieve a buffer from the ring */
		if (skb)
			i40e_add_rx_frag(rx_ring, rx_buffer, skb, size);
		else if (ring_uses_build_skb(rx_ring))
			skb = i40e_build_skb(rx_ring, rx_buffer, size);
		else
			skb = i40e_construct_skb(rx_ring, rx_buffer, size);

		/* exit if we failed to retrieve a buffer */
		if (!skb) {
			rx_ring->rx_stats.alloc_buff_failed++;
			rx_buffer->pagecnt_bias++;
			break;
		}

		i40e_put_rx_buffer(rx_ring, rx_buffer);
		cleaned_count++;

		if (i40e_is_non_eop(rx_ring, rx_desc, skb))
			continue;

		/* ERR_MASK will only have valid bits if EOP set, and
		 * what we are doing here is actually checking
		 * I40E_RX_DESC_ERROR_RXE_SHIFT, since it is the zeroth bit in
		 * the error field
		 */
		if (unlikely(i40e_test_staterr(rx_desc, BIT(I40E_RXD_QW1_ERROR_SHIFT)))) {
			dev_kfree_skb_any(skb);
			skb = NULL;
			continue;
		}

		if (i40e_cleanup_headers(rx_ring, skb)) {
			skb = NULL;
			continue;
		}

		/* probably a little skewed due to removing CRC */
		total_rx_bytes += skb->len;

		qword = le64_to_cpu(rx_desc->wb.qword1.status_error_len);
		rx_ptype = (qword & I40E_RXD_QW1_PTYPE_MASK) >>
			   I40E_RXD_QW1_PTYPE_SHIFT;

		/* populate checksum, VLAN, and protocol */
		i40evf_process_skb_fields(rx_ring, rx_desc, skb, rx_ptype);


		vlan_tag = (qword & BIT(I40E_RX_DESC_STATUS_L2TAG1P_SHIFT)) ?
			   le16_to_cpu(rx_desc->wb.qword0.lo_dword.l2tag1) : 0;

		i40e_trace(clean_rx_irq_rx, rx_ring, rx_desc, skb);
		i40e_receive_skb(rx_ring, skb, vlan_tag);
		skb = NULL;

		/* update budget accounting */
		total_rx_packets++;
	}

	rx_ring->skb = skb;

	u64_stats_update_begin(&rx_ring->syncp);
	rx_ring->stats.packets += total_rx_packets;
	rx_ring->stats.bytes += total_rx_bytes;
	u64_stats_update_end(&rx_ring->syncp);
	rx_ring->q_vector->rx.total_packets += total_rx_packets;
	rx_ring->q_vector->rx.total_bytes += total_rx_bytes;

	/* guarantee a trip back through this routine if there was a failure */
	return failure ? budget : (int)total_rx_packets;
}

static u32 i40e_buildreg_itr(const int type, const u16 itr)
{
	u32 val;

	val = I40E_VFINT_DYN_CTLN1_INTENA_MASK |
	      I40E_VFINT_DYN_CTLN1_CLEARPBA_MASK |
	      (type << I40E_VFINT_DYN_CTLN1_ITR_INDX_SHIFT) |
	      (itr << I40E_VFINT_DYN_CTLN1_INTERVAL_SHIFT);

	return val;
}

/* a small macro to shorten up some long lines */
#define INTREG I40E_VFINT_DYN_CTLN1
static inline int get_rx_itr(struct i40e_vsi *vsi, int idx)
{
	struct i40evf_adapter *adapter = vsi->back;

	return adapter->rx_rings[idx].rx_itr_setting;
}

static inline int get_tx_itr(struct i40e_vsi *vsi, int idx)
{
	struct i40evf_adapter *adapter = vsi->back;

	return adapter->tx_rings[idx].tx_itr_setting;
}

/**
 * i40e_update_enable_itr - Update itr and re-enable MSIX interrupt
 * @vsi: the VSI we care about
 * @q_vector: q_vector for which itr is being updated and interrupt enabled
 *
 **/
static inline void i40e_update_enable_itr(struct i40e_vsi *vsi,
					  struct i40e_q_vector *q_vector)
{
	struct i40e_hw *hw = &vsi->back->hw;
	bool rx = false, tx = false;
	u32 rxval, txval;
	int vector;
	int idx = q_vector->v_idx;
	int rx_itr_setting, tx_itr_setting;

	vector = (q_vector->v_idx + vsi->base_vector);

	/* avoid dynamic calculation if in countdown mode OR if
	 * all dynamic is disabled
	 */
	rxval = txval = i40e_buildreg_itr(I40E_ITR_NONE, 0);

	rx_itr_setting = get_rx_itr(vsi, idx);
	tx_itr_setting = get_tx_itr(vsi, idx);

	if (q_vector->itr_countdown > 0 ||
	    (!ITR_IS_DYNAMIC(rx_itr_setting) &&
	     !ITR_IS_DYNAMIC(tx_itr_setting))) {
		goto enable_int;
	}

	if (ITR_IS_DYNAMIC(rx_itr_setting)) {
		rx = i40e_set_new_dynamic_itr(&q_vector->rx);
		rxval = i40e_buildreg_itr(I40E_RX_ITR, q_vector->rx.itr);
	}

	if (ITR_IS_DYNAMIC(tx_itr_setting)) {
		tx = i40e_set_new_dynamic_itr(&q_vector->tx);
		txval = i40e_buildreg_itr(I40E_TX_ITR, q_vector->tx.itr);
	}

	if (rx || tx) {
		/* get the higher of the two ITR adjustments and
		 * use the same value for both ITR registers
		 * when in adaptive mode (Rx and/or Tx)
		 */
		u16 itr = max(q_vector->tx.itr, q_vector->rx.itr);

		q_vector->tx.itr = q_vector->rx.itr = itr;
		txval = i40e_buildreg_itr(I40E_TX_ITR, itr);
		tx = true;
		rxval = i40e_buildreg_itr(I40E_RX_ITR, itr);
		rx = true;
	}

	/* only need to enable the interrupt once, but need
	 * to possibly update both ITR values
	 */
	if (rx) {
		/* set the INTENA_MSK_MASK so that this first write
		 * won't actually enable the interrupt, instead just
		 * updating the ITR (it's bit 31 PF and VF)
		 */
		rxval |= BIT(31);
		/* don't check _DOWN because interrupt isn't being enabled */
		wr32(hw, INTREG(vector - 1), rxval);
	}

enable_int:
	if (!test_bit(__I40E_VSI_DOWN, vsi->state))
		wr32(hw, INTREG(vector - 1), txval);

	if (q_vector->itr_countdown)
		q_vector->itr_countdown--;
	else
		q_vector->itr_countdown = ITR_COUNTDOWN_START;
}

/**
 * i40evf_napi_poll - NAPI polling Rx/Tx cleanup routine
 * @napi: napi struct with our devices info in it
 * @budget: amount of work driver is allowed to do this pass, in packets
 *
 * This function will clean all queues associated with a q_vector.
 *
 * Returns the amount of work done
 **/
int i40evf_napi_poll(struct napi_struct *napi, int budget)
{
	struct i40e_q_vector *q_vector =
			       container_of(napi, struct i40e_q_vector, napi);
	struct i40e_vsi *vsi = q_vector->vsi;
	struct i40e_ring *ring;
	bool clean_complete = true;
	bool arm_wb = false;
	int budget_per_ring;
	int work_done = 0;

	if (test_bit(__I40E_VSI_DOWN, vsi->state)) {
		napi_complete(napi);
		return 0;
	}

	/* Since the actual Tx work is minimal, we can give the Tx a larger
	 * budget and be more aggressive about cleaning up the Tx descriptors.
	 */
	i40e_for_each_ring(ring, q_vector->tx) {
		if (!i40e_clean_tx_irq(vsi, ring, budget)) {
			clean_complete = false;
			continue;
		}
		arm_wb |= ring->arm_wb;
		ring->arm_wb = false;
	}

	/* Handle case where we are called by netpoll with a budget of 0 */
	if (budget <= 0)
		goto tx_only;

	/* We attempt to distribute budget to each Rx queue fairly, but don't
	 * allow the budget to go below 1 because that would exit polling early.
	 */
	budget_per_ring = max(budget/q_vector->num_ringpairs, 1);

	i40e_for_each_ring(ring, q_vector->rx) {
		int cleaned = i40e_clean_rx_irq(ring, budget_per_ring);

		work_done += cleaned;
		/* if we clean as many as budgeted, we must not be done */
		if (cleaned >= budget_per_ring)
			clean_complete = false;
	}

	/* If work not completed, return budget and polling will return */
	if (!clean_complete) {
		int cpu_id = smp_processor_id();

		/* It is possible that the interrupt affinity has changed but,
		 * if the cpu is pegged at 100%, polling will never exit while
		 * traffic continues and the interrupt will be stuck on this
		 * cpu.  We check to make sure affinity is correct before we
		 * continue to poll, otherwise we must stop polling so the
		 * interrupt can move to the correct cpu.
		 */
		if (!cpumask_test_cpu(cpu_id, &q_vector->affinity_mask)) {
			/* Tell napi that we are done polling */
			napi_complete_done(napi, work_done);

			/* Force an interrupt */
			i40evf_force_wb(vsi, q_vector);

			/* Return budget-1 so that polling stops */
			return budget - 1;
		}
tx_only:
		if (arm_wb) {
			q_vector->tx.ring[0].tx_stats.tx_force_wb++;
			i40e_enable_wb_on_itr(vsi, q_vector);
		}
		return budget;
	}

	if (vsi->back->flags & I40E_TXR_FLAGS_WB_ON_ITR)
		q_vector->arm_wb_state = false;

	/* Work is done so exit the polling mode and re-enable the interrupt */
	napi_complete_done(napi, work_done);

	i40e_update_enable_itr(vsi, q_vector);

	return min(work_done, budget - 1);
}

/**
 * i40evf_tx_prepare_vlan_flags - prepare generic TX VLAN tagging flags for HW
 * @skb:     send buffer
 * @tx_ring: ring to send buffer on
 * @flags:   the tx flags to be set
 *
 * Checks the skb and set up correspondingly several generic transmit flags
 * related to VLAN tagging for the HW, such as VLAN, DCB, etc.
 *
 * Returns error code indicate the frame should be dropped upon error and the
 * otherwise  returns 0 to indicate the flags has been set properly.
 **/
static inline int i40evf_tx_prepare_vlan_flags(struct sk_buff *skb,
					       struct i40e_ring *tx_ring,
					       u32 *flags)
{
	__be16 protocol = skb->protocol;
	u32  tx_flags = 0;

	if (protocol == htons(ETH_P_8021Q) &&
	    !(tx_ring->netdev->features & NETIF_F_HW_VLAN_CTAG_TX)) {
		/* When HW VLAN acceleration is turned off by the user the
		 * stack sets the protocol to 8021q so that the driver
		 * can take any steps required to support the SW only
		 * VLAN handling.  In our case the driver doesn't need
		 * to take any further steps so just set the protocol
		 * to the encapsulated ethertype.
		 */
		skb->protocol = vlan_get_protocol(skb);
		goto out;
	}

	/* if we have a HW VLAN tag being added, default to the HW one */
	if (skb_vlan_tag_present(skb)) {
		tx_flags |= skb_vlan_tag_get(skb) << I40E_TX_FLAGS_VLAN_SHIFT;
		tx_flags |= I40E_TX_FLAGS_HW_VLAN;
	/* else if it is a SW VLAN, check the next protocol and store the tag */
	} else if (protocol == htons(ETH_P_8021Q)) {
		struct vlan_hdr *vhdr, _vhdr;

		vhdr = skb_header_pointer(skb, ETH_HLEN, sizeof(_vhdr), &_vhdr);
		if (!vhdr)
			return -EINVAL;

		protocol = vhdr->h_vlan_encapsulated_proto;
		tx_flags |= ntohs(vhdr->h_vlan_TCI) << I40E_TX_FLAGS_VLAN_SHIFT;
		tx_flags |= I40E_TX_FLAGS_SW_VLAN;
	}

out:
	*flags = tx_flags;
	return 0;
}

/**
 * i40e_tso - set up the tso context descriptor
 * @first:    pointer to first Tx buffer for xmit
 * @hdr_len:  ptr to the size of the packet header
 * @cd_type_cmd_tso_mss: Quad Word 1
 *
 * Returns 0 if no TSO can happen, 1 if tso is going, or error
 **/
static int i40e_tso(struct i40e_tx_buffer *first, u8 *hdr_len,
		    u64 *cd_type_cmd_tso_mss)
{
	struct sk_buff *skb = first->skb;
	u64 cd_cmd, cd_tso_len, cd_mss;
	union {
		struct iphdr *v4;
		struct ipv6hdr *v6;
		unsigned char *hdr;
	} ip;
	union {
		struct tcphdr *tcp;
		struct udphdr *udp;
		unsigned char *hdr;
	} l4;
	u32 paylen, l4_offset;
	u16 gso_segs, gso_size;
	int err;

	if (skb->ip_summed != CHECKSUM_PARTIAL)
		return 0;

	if (!skb_is_gso(skb))
		return 0;

	err = skb_cow_head(skb, 0);
	if (err < 0)
		return err;

	ip.hdr = skb_network_header(skb);
	l4.hdr = skb_transport_header(skb);

	/* initialize outer IP header fields */
	if (ip.v4->version == 4) {
		ip.v4->tot_len = 0;
		ip.v4->check = 0;
	} else {
		ip.v6->payload_len = 0;
	}

	if (skb_shinfo(skb)->gso_type & (SKB_GSO_GRE |
					 SKB_GSO_GRE_CSUM |
					 SKB_GSO_IPXIP4 |
					 SKB_GSO_IPXIP6 |
					 SKB_GSO_UDP_TUNNEL |
					 SKB_GSO_UDP_TUNNEL_CSUM)) {
		if (!(skb_shinfo(skb)->gso_type & SKB_GSO_PARTIAL) &&
		    (skb_shinfo(skb)->gso_type & SKB_GSO_UDP_TUNNEL_CSUM)) {
			l4.udp->len = 0;

			/* determine offset of outer transport header */
			l4_offset = l4.hdr - skb->data;

			/* remove payload length from outer checksum */
			paylen = skb->len - l4_offset;
			csum_replace_by_diff(&l4.udp->check,
					     (__force __wsum)htonl(paylen));
		}

		/* reset pointers to inner headers */
		ip.hdr = skb_inner_network_header(skb);
		l4.hdr = skb_inner_transport_header(skb);

		/* initialize inner IP header fields */
		if (ip.v4->version == 4) {
			ip.v4->tot_len = 0;
			ip.v4->check = 0;
		} else {
			ip.v6->payload_len = 0;
		}
	}

	/* determine offset of inner transport header */
	l4_offset = l4.hdr - skb->data;

	/* remove payload length from inner checksum */
	paylen = skb->len - l4_offset;
	csum_replace_by_diff(&l4.tcp->check, (__force __wsum)htonl(paylen));

	/* compute length of segmentation header */
	*hdr_len = (l4.tcp->doff * 4) + l4_offset;

	/* pull values out of skb_shinfo */
	gso_size = skb_shinfo(skb)->gso_size;
	gso_segs = skb_shinfo(skb)->gso_segs;

	/* update GSO size and bytecount with header size */
	first->gso_segs = gso_segs;
	first->bytecount += (first->gso_segs - 1) * *hdr_len;

	/* find the field values */
	cd_cmd = I40E_TX_CTX_DESC_TSO;
	cd_tso_len = skb->len - *hdr_len;
	cd_mss = gso_size;
	*cd_type_cmd_tso_mss |= (cd_cmd << I40E_TXD_CTX_QW1_CMD_SHIFT) |
				(cd_tso_len << I40E_TXD_CTX_QW1_TSO_LEN_SHIFT) |
				(cd_mss << I40E_TXD_CTX_QW1_MSS_SHIFT);
	return 1;
}

/**
 * i40e_tx_enable_csum - Enable Tx checksum offloads
 * @skb: send buffer
 * @tx_flags: pointer to Tx flags currently set
 * @td_cmd: Tx descriptor command bits to set
 * @td_offset: Tx descriptor header offsets to set
 * @tx_ring: Tx descriptor ring
 * @cd_tunneling: ptr to context desc bits
 **/
static int i40e_tx_enable_csum(struct sk_buff *skb, u32 *tx_flags,
			       u32 *td_cmd, u32 *td_offset,
			       struct i40e_ring *tx_ring,
			       u32 *cd_tunneling)
{
	union {
		struct iphdr *v4;
		struct ipv6hdr *v6;
		unsigned char *hdr;
	} ip;
	union {
		struct tcphdr *tcp;
		struct udphdr *udp;
		unsigned char *hdr;
	} l4;
	unsigned char *exthdr;
	u32 offset, cmd = 0;
	__be16 frag_off;
	u8 l4_proto = 0;

	if (skb->ip_summed != CHECKSUM_PARTIAL)
		return 0;

	ip.hdr = skb_network_header(skb);
	l4.hdr = skb_transport_header(skb);

	/* compute outer L2 header size */
	offset = ((ip.hdr - skb->data) / 2) << I40E_TX_DESC_LENGTH_MACLEN_SHIFT;

	if (skb->encapsulation) {
		u32 tunnel = 0;
		/* define outer network header type */
		if (*tx_flags & I40E_TX_FLAGS_IPV4) {
			tunnel |= (*tx_flags & I40E_TX_FLAGS_TSO) ?
				  I40E_TX_CTX_EXT_IP_IPV4 :
				  I40E_TX_CTX_EXT_IP_IPV4_NO_CSUM;

			l4_proto = ip.v4->protocol;
		} else if (*tx_flags & I40E_TX_FLAGS_IPV6) {
			tunnel |= I40E_TX_CTX_EXT_IP_IPV6;

			exthdr = ip.hdr + sizeof(*ip.v6);
			l4_proto = ip.v6->nexthdr;
			if (l4.hdr != exthdr)
				ipv6_skip_exthdr(skb, exthdr - skb->data,
						 &l4_proto, &frag_off);
		}

		/* define outer transport */
		switch (l4_proto) {
		case IPPROTO_UDP:
			tunnel |= I40E_TXD_CTX_UDP_TUNNELING;
			*tx_flags |= I40E_TX_FLAGS_VXLAN_TUNNEL;
			break;
		case IPPROTO_GRE:
			tunnel |= I40E_TXD_CTX_GRE_TUNNELING;
			*tx_flags |= I40E_TX_FLAGS_VXLAN_TUNNEL;
			break;
		case IPPROTO_IPIP:
		case IPPROTO_IPV6:
			*tx_flags |= I40E_TX_FLAGS_VXLAN_TUNNEL;
			l4.hdr = skb_inner_network_header(skb);
			break;
		default:
			if (*tx_flags & I40E_TX_FLAGS_TSO)
				return -1;

			skb_checksum_help(skb);
			return 0;
		}

		/* compute outer L3 header size */
		tunnel |= ((l4.hdr - ip.hdr) / 4) <<
			  I40E_TXD_CTX_QW0_EXT_IPLEN_SHIFT;

		/* switch IP header pointer from outer to inner header */
		ip.hdr = skb_inner_network_header(skb);

		/* compute tunnel header size */
		tunnel |= ((ip.hdr - l4.hdr) / 2) <<
			  I40E_TXD_CTX_QW0_NATLEN_SHIFT;

		/* indicate if we need to offload outer UDP header */
		if ((*tx_flags & I40E_TX_FLAGS_TSO) &&
		    !(skb_shinfo(skb)->gso_type & SKB_GSO_PARTIAL) &&
		    (skb_shinfo(skb)->gso_type & SKB_GSO_UDP_TUNNEL_CSUM))
			tunnel |= I40E_TXD_CTX_QW0_L4T_CS_MASK;

		/* record tunnel offload values */
		*cd_tunneling |= tunnel;

		/* switch L4 header pointer from outer to inner */
		l4.hdr = skb_inner_transport_header(skb);
		l4_proto = 0;

		/* reset type as we transition from outer to inner headers */
		*tx_flags &= ~(I40E_TX_FLAGS_IPV4 | I40E_TX_FLAGS_IPV6);
		if (ip.v4->version == 4)
			*tx_flags |= I40E_TX_FLAGS_IPV4;
		if (ip.v6->version == 6)
			*tx_flags |= I40E_TX_FLAGS_IPV6;
	}

	/* Enable IP checksum offloads */
	if (*tx_flags & I40E_TX_FLAGS_IPV4) {
		l4_proto = ip.v4->protocol;
		/* the stack computes the IP header already, the only time we
		 * need the hardware to recompute it is in the case of TSO.
		 */
		cmd |= (*tx_flags & I40E_TX_FLAGS_TSO) ?
		       I40E_TX_DESC_CMD_IIPT_IPV4_CSUM :
		       I40E_TX_DESC_CMD_IIPT_IPV4;
	} else if (*tx_flags & I40E_TX_FLAGS_IPV6) {
		cmd |= I40E_TX_DESC_CMD_IIPT_IPV6;

		exthdr = ip.hdr + sizeof(*ip.v6);
		l4_proto = ip.v6->nexthdr;
		if (l4.hdr != exthdr)
			ipv6_skip_exthdr(skb, exthdr - skb->data,
					 &l4_proto, &frag_off);
	}

	/* compute inner L3 header size */
	offset |= ((l4.hdr - ip.hdr) / 4) << I40E_TX_DESC_LENGTH_IPLEN_SHIFT;

	/* Enable L4 checksum offloads */
	switch (l4_proto) {
	case IPPROTO_TCP:
		/* enable checksum offloads */
		cmd |= I40E_TX_DESC_CMD_L4T_EOFT_TCP;
		offset |= l4.tcp->doff << I40E_TX_DESC_LENGTH_L4_FC_LEN_SHIFT;
		break;
	case IPPROTO_SCTP:
		/* enable SCTP checksum offload */
		cmd |= I40E_TX_DESC_CMD_L4T_EOFT_SCTP;
		offset |= (sizeof(struct sctphdr) >> 2) <<
			  I40E_TX_DESC_LENGTH_L4_FC_LEN_SHIFT;
		break;
	case IPPROTO_UDP:
		/* enable UDP checksum offload */
		cmd |= I40E_TX_DESC_CMD_L4T_EOFT_UDP;
		offset |= (sizeof(struct udphdr) >> 2) <<
			  I40E_TX_DESC_LENGTH_L4_FC_LEN_SHIFT;
		break;
	default:
		if (*tx_flags & I40E_TX_FLAGS_TSO)
			return -1;
		skb_checksum_help(skb);
		return 0;
	}

	*td_cmd |= cmd;
	*td_offset |= offset;

	return 1;
}

/**
 * i40e_create_tx_ctx Build the Tx context descriptor
 * @tx_ring:  ring to create the descriptor on
 * @cd_type_cmd_tso_mss: Quad Word 1
 * @cd_tunneling: Quad Word 0 - bits 0-31
 * @cd_l2tag2: Quad Word 0 - bits 32-63
 **/
static void i40e_create_tx_ctx(struct i40e_ring *tx_ring,
			       const u64 cd_type_cmd_tso_mss,
			       const u32 cd_tunneling, const u32 cd_l2tag2)
{
	struct i40e_tx_context_desc *context_desc;
	int i = tx_ring->next_to_use;

	if ((cd_type_cmd_tso_mss == I40E_TX_DESC_DTYPE_CONTEXT) &&
	    !cd_tunneling && !cd_l2tag2)
		return;

	/* grab the next descriptor */
	context_desc = I40E_TX_CTXTDESC(tx_ring, i);

	i++;
	tx_ring->next_to_use = (i < tx_ring->count) ? i : 0;

	/* cpu_to_le32 and assign to struct fields */
	context_desc->tunneling_params = cpu_to_le32(cd_tunneling);
	context_desc->l2tag2 = cpu_to_le16(cd_l2tag2);
	context_desc->rsvd = cpu_to_le16(0);
	context_desc->type_cmd_tso_mss = cpu_to_le64(cd_type_cmd_tso_mss);
}

/**
 * __i40evf_chk_linearize - Check if there are more than 8 buffers per packet
 * @skb:      send buffer
 *
 * Note: Our HW can't DMA more than 8 buffers to build a packet on the wire
 * and so we need to figure out the cases where we need to linearize the skb.
 *
 * For TSO we need to count the TSO header and segment payload separately.
 * As such we need to check cases where we have 7 fragments or more as we
 * can potentially require 9 DMA transactions, 1 for the TSO header, 1 for
 * the segment payload in the first descriptor, and another 7 for the
 * fragments.
 **/
bool __i40evf_chk_linearize(struct sk_buff *skb)
{
	const struct skb_frag_struct *frag, *stale;
	int nr_frags, sum;

	/* no need to check if number of frags is less than 7 */
	nr_frags = skb_shinfo(skb)->nr_frags;
	if (nr_frags < (I40E_MAX_BUFFER_TXD - 1))
		return false;

	/* We need to walk through the list and validate that each group
	 * of 6 fragments totals at least gso_size.
	 */
	nr_frags -= I40E_MAX_BUFFER_TXD - 2;
	frag = &skb_shinfo(skb)->frags[0];

	/* Initialize size to the negative value of gso_size minus 1.  We
	 * use this as the worst case scenerio in which the frag ahead
	 * of us only provides one byte which is why we are limited to 6
	 * descriptors for a single transmit as the header and previous
	 * fragment are already consuming 2 descriptors.
	 */
	sum = 1 - skb_shinfo(skb)->gso_size;

	/* Add size of frags 0 through 4 to create our initial sum */
	sum += skb_frag_size(frag++);
	sum += skb_frag_size(frag++);
	sum += skb_frag_size(frag++);
	sum += skb_frag_size(frag++);
	sum += skb_frag_size(frag++);

	/* Walk through fragments adding latest fragment, testing it, and
	 * then removing stale fragments from the sum.
	 */
	for (stale = &skb_shinfo(skb)->frags[0];; stale++) {
		int stale_size = skb_frag_size(stale);

		sum += skb_frag_size(frag++);

		/* The stale fragment may present us with a smaller
		 * descriptor than the actual fragment size. To account
		 * for that we need to remove all the data on the front and
		 * figure out what the remainder would be in the last
		 * descriptor associated with the fragment.
		 */
		if (stale_size > I40E_MAX_DATA_PER_TXD) {
			int align_pad = -(stale->page_offset) &
					(I40E_MAX_READ_REQ_SIZE - 1);

			sum -= align_pad;
			stale_size -= align_pad;

			do {
				sum -= I40E_MAX_DATA_PER_TXD_ALIGNED;
				stale_size -= I40E_MAX_DATA_PER_TXD_ALIGNED;
			} while (stale_size > I40E_MAX_DATA_PER_TXD);
		}

		/* if sum is negative we failed to make sufficient progress */
		if (sum < 0)
			return true;

		if (!nr_frags--)
			break;

		sum -= stale_size;
	}

	return false;
}

/**
 * __i40evf_maybe_stop_tx - 2nd level check for tx stop conditions
 * @tx_ring: the ring to be checked
 * @size:    the size buffer we want to assure is available
 *
 * Returns -EBUSY if a stop is needed, else 0
 **/
int __i40evf_maybe_stop_tx(struct i40e_ring *tx_ring, int size)
{
	netif_stop_subqueue(tx_ring->netdev, tx_ring->queue_index);
	/* Memory barrier before checking head and tail */
	smp_mb();

	/* Check again in a case another CPU has just made room available. */
	if (likely(I40E_DESC_UNUSED(tx_ring) < size))
		return -EBUSY;

	/* A reprieve! - use start_queue because it doesn't call schedule */
	netif_start_subqueue(tx_ring->netdev, tx_ring->queue_index);
	++tx_ring->tx_stats.restart_queue;
	return 0;
}

/**
 * i40evf_tx_map - Build the Tx descriptor
 * @tx_ring:  ring to send buffer on
 * @skb:      send buffer
 * @first:    first buffer info buffer to use
 * @tx_flags: collected send information
 * @hdr_len:  size of the packet header
 * @td_cmd:   the command field in the descriptor
 * @td_offset: offset for checksum or crc
 **/
static inline void i40evf_tx_map(struct i40e_ring *tx_ring, struct sk_buff *skb,
				 struct i40e_tx_buffer *first, u32 tx_flags,
				 const u8 hdr_len, u32 td_cmd, u32 td_offset)
{
	unsigned int data_len = skb->data_len;
	unsigned int size = skb_headlen(skb);
	struct skb_frag_struct *frag;
	struct i40e_tx_buffer *tx_bi;
	struct i40e_tx_desc *tx_desc;
	u16 i = tx_ring->next_to_use;
	u32 td_tag = 0;
	dma_addr_t dma;

	if (tx_flags & I40E_TX_FLAGS_HW_VLAN) {
		td_cmd |= I40E_TX_DESC_CMD_IL2TAG1;
		td_tag = (tx_flags & I40E_TX_FLAGS_VLAN_MASK) >>
			 I40E_TX_FLAGS_VLAN_SHIFT;
	}

	first->tx_flags = tx_flags;

	dma = dma_map_single(tx_ring->dev, skb->data, size, DMA_TO_DEVICE);

	tx_desc = I40E_TX_DESC(tx_ring, i);
	tx_bi = first;

	for (frag = &skb_shinfo(skb)->frags[0];; frag++) {
		unsigned int max_data = I40E_MAX_DATA_PER_TXD_ALIGNED;

		if (dma_mapping_error(tx_ring->dev, dma))
			goto dma_error;

		/* record length, and DMA address */
		dma_unmap_len_set(tx_bi, len, size);
		dma_unmap_addr_set(tx_bi, dma, dma);

		/* align size to end of page */
		max_data += -dma & (I40E_MAX_READ_REQ_SIZE - 1);
		tx_desc->buffer_addr = cpu_to_le64(dma);

		while (unlikely(size > I40E_MAX_DATA_PER_TXD)) {
			tx_desc->cmd_type_offset_bsz =
				build_ctob(td_cmd, td_offset,
					   max_data, td_tag);

			tx_desc++;
			i++;

			if (i == tx_ring->count) {
				tx_desc = I40E_TX_DESC(tx_ring, 0);
				i = 0;
			}

			dma += max_data;
			size -= max_data;

			max_data = I40E_MAX_DATA_PER_TXD_ALIGNED;
			tx_desc->buffer_addr = cpu_to_le64(dma);
		}

		if (likely(!data_len))
			break;

		tx_desc->cmd_type_offset_bsz = build_ctob(td_cmd, td_offset,
							  size, td_tag);

		tx_desc++;
		i++;

		if (i == tx_ring->count) {
			tx_desc = I40E_TX_DESC(tx_ring, 0);
			i = 0;
		}

		size = skb_frag_size(frag);
		data_len -= size;

		dma = skb_frag_dma_map(tx_ring->dev, frag, 0, size,
				       DMA_TO_DEVICE);

		tx_bi = &tx_ring->tx_bi[i];
	}

	netdev_tx_sent_queue(txring_txq(tx_ring), first->bytecount);

	i++;
	if (i == tx_ring->count)
		i = 0;

	tx_ring->next_to_use = i;

	i40e_maybe_stop_tx(tx_ring, DESC_NEEDED);

	/* write last descriptor with RS and EOP bits */
	td_cmd |= I40E_TXD_CMD;
	tx_desc->cmd_type_offset_bsz =
			build_ctob(td_cmd, td_offset, size, td_tag);

	/* Force memory writes to complete before letting h/w know there
	 * are new descriptors to fetch.
	 *
	 * We also use this memory barrier to make certain all of the
	 * status bits have been updated before next_to_watch is written.
	 */
	wmb();

	/* set next_to_watch value indicating a packet is present */
	first->next_to_watch = tx_desc;

	/* notify HW of packet */
	if (netif_xmit_stopped(txring_txq(tx_ring)) || !skb->xmit_more) {
		writel(i, tx_ring->tail);

		/* we need this if more than one processor can write to our tail
		 * at a time, it synchronizes IO on IA64/Altix systems
		 */
		mmiowb();
	}

	return;

dma_error:
	dev_info(tx_ring->dev, "TX DMA map failed\n");

	/* clear dma mappings for failed tx_bi map */
	for (;;) {
		tx_bi = &tx_ring->tx_bi[i];
		i40e_unmap_and_free_tx_resource(tx_ring, tx_bi);
		if (tx_bi == first)
			break;
		if (i == 0)
			i = tx_ring->count;
		i--;
	}

	tx_ring->next_to_use = i;
}

/**
 * i40e_xmit_frame_ring - Sends buffer on Tx ring
 * @skb:     send buffer
 * @tx_ring: ring to send buffer on
 *
 * Returns NETDEV_TX_OK if sent, else an error code
 **/
static netdev_tx_t i40e_xmit_frame_ring(struct sk_buff *skb,
					struct i40e_ring *tx_ring)
{
	u64 cd_type_cmd_tso_mss = I40E_TX_DESC_DTYPE_CONTEXT;
	u32 cd_tunneling = 0, cd_l2tag2 = 0;
	struct i40e_tx_buffer *first;
	u32 td_offset = 0;
	u32 tx_flags = 0;
	__be16 protocol;
	u32 td_cmd = 0;
	u8 hdr_len = 0;
	int tso, count;

	/* prefetch the data, we'll need it later */
	prefetch(skb->data);

	i40e_trace(xmit_frame_ring, skb, tx_ring);

	count = i40e_xmit_descriptor_count(skb);
	if (i40e_chk_linearize(skb, count)) {
		if (__skb_linearize(skb)) {
			dev_kfree_skb_any(skb);
			return NETDEV_TX_OK;
		}
		count = i40e_txd_use_count(skb->len);
		tx_ring->tx_stats.tx_linearize++;
	}

	/* need: 1 descriptor per page * PAGE_SIZE/I40E_MAX_DATA_PER_TXD,
	 *       + 1 desc for skb_head_len/I40E_MAX_DATA_PER_TXD,
	 *       + 4 desc gap to avoid the cache line where head is,
	 *       + 1 desc for context descriptor,
	 * otherwise try next time
	 */
	if (i40e_maybe_stop_tx(tx_ring, count + 4 + 1)) {
		tx_ring->tx_stats.tx_busy++;
		return NETDEV_TX_BUSY;
	}

	/* record the location of the first descriptor for this packet */
	first = &tx_ring->tx_bi[tx_ring->next_to_use];
	first->skb = skb;
	first->bytecount = skb->len;
	first->gso_segs = 1;

	/* prepare the xmit flags */
	if (i40evf_tx_prepare_vlan_flags(skb, tx_ring, &tx_flags))
		goto out_drop;

	/* obtain protocol of skb */
	protocol = vlan_get_protocol(skb);

	/* setup IPv4/IPv6 offloads */
	if (protocol == htons(ETH_P_IP))
		tx_flags |= I40E_TX_FLAGS_IPV4;
	else if (protocol == htons(ETH_P_IPV6))
		tx_flags |= I40E_TX_FLAGS_IPV6;

	tso = i40e_tso(first, &hdr_len, &cd_type_cmd_tso_mss);

	if (tso < 0)
		goto out_drop;
	else if (tso)
		tx_flags |= I40E_TX_FLAGS_TSO;

	/* Always offload the checksum, since it's in the data descriptor */
	tso = i40e_tx_enable_csum(skb, &tx_flags, &td_cmd, &td_offset,
				  tx_ring, &cd_tunneling);
	if (tso < 0)
		goto out_drop;

	skb_tx_timestamp(skb);

	/* always enable CRC insertion offload */
	td_cmd |= I40E_TX_DESC_CMD_ICRC;

	i40e_create_tx_ctx(tx_ring, cd_type_cmd_tso_mss,
			   cd_tunneling, cd_l2tag2);

	i40evf_tx_map(tx_ring, skb, first, tx_flags, hdr_len,
		      td_cmd, td_offset);

	return NETDEV_TX_OK;

out_drop:
	i40e_trace(xmit_frame_ring_drop, first->skb, tx_ring);
	dev_kfree_skb_any(first->skb);
	first->skb = NULL;
	return NETDEV_TX_OK;
}

/**
 * i40evf_xmit_frame - Selects the correct VSI and Tx queue to send buffer
 * @skb:    send buffer
 * @netdev: network interface device structure
 *
 * Returns NETDEV_TX_OK if sent, else an error code
 **/
netdev_tx_t i40evf_xmit_frame(struct sk_buff *skb, struct net_device *netdev)
{
	struct i40evf_adapter *adapter = netdev_priv(netdev);
	struct i40e_ring *tx_ring = &adapter->tx_rings[skb->queue_mapping];

	/* hardware can't handle really short frames, hardware padding works
	 * beyond this point
	 */
	if (unlikely(skb->len < I40E_MIN_TX_LEN)) {
		if (skb_pad(skb, I40E_MIN_TX_LEN - skb->len))
			return NETDEV_TX_OK;
		skb->len = I40E_MIN_TX_LEN;
		skb_set_tail_pointer(skb, I40E_MIN_TX_LEN);
	}

	return i40e_xmit_frame_ring(skb, tx_ring);
}
