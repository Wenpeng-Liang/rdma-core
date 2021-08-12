/*
 * Copyright (c) 2016-2017 Hisilicon Limited.
 *
 * This software is available to you under a choice of one of two
 * licenses.  You may choose to be licensed under the terms of the GNU
 * General Public License (GPL) Version 2, available from the file
 * COPYING in the main directory of this source tree, or the
 * OpenIB.org BSD license below:
 *
 *     Redistribution and use in source and binary forms, with or
 *     without modification, are permitted provided that the following
 *     conditions are met:
 *
 *      - Redistributions of source code must retain the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer.
 *
 *      - Redistributions in binary form must reproduce the above
 *        copyright notice, this list of conditions and the following
 *        disclaimer in the documentation and/or other materials
 *        provided with the distribution.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <sys/mman.h>
#include <unistd.h>

#include "hns_roce_u.h"

static void hns_roce_free_context(struct ibv_context *ibctx);

#define HID_LEN			15
#define DEV_MATCH_LEN		128

#ifndef PCI_VENDOR_ID_HUAWEI
#define PCI_VENDOR_ID_HUAWEI			0x19E5
#endif

static const struct verbs_match_ent hca_table[] = {
	VERBS_MODALIAS_MATCH("acpi*:HISI00D1:*", &hns_roce_u_hw_v1),
	VERBS_MODALIAS_MATCH("of:N*T*Chisilicon,hns-roce-v1C*", &hns_roce_u_hw_v1),
	VERBS_MODALIAS_MATCH("of:N*T*Chisilicon,hns-roce-v1", &hns_roce_u_hw_v1),
	VERBS_PCI_MATCH(PCI_VENDOR_ID_HUAWEI, 0xA222, &hns_roce_u_hw_v2),
	VERBS_PCI_MATCH(PCI_VENDOR_ID_HUAWEI, 0xA223, &hns_roce_u_hw_v2),
	VERBS_PCI_MATCH(PCI_VENDOR_ID_HUAWEI, 0xA224, &hns_roce_u_hw_v2),
	VERBS_PCI_MATCH(PCI_VENDOR_ID_HUAWEI, 0xA225, &hns_roce_u_hw_v2),
	VERBS_PCI_MATCH(PCI_VENDOR_ID_HUAWEI, 0xA226, &hns_roce_u_hw_v2),
	VERBS_PCI_MATCH(PCI_VENDOR_ID_HUAWEI, 0xA227, &hns_roce_u_hw_v2),
	VERBS_PCI_MATCH(PCI_VENDOR_ID_HUAWEI, 0xA228, &hns_roce_u_hw_v2),
	VERBS_PCI_MATCH(PCI_VENDOR_ID_HUAWEI, 0xA22F, &hns_roce_u_hw_v2),
	{}
};

static const struct verbs_context_ops hns_common_ops = {
	.alloc_mw = hns_roce_u_alloc_mw,
	.alloc_pd = hns_roce_u_alloc_pd,
	.bind_mw = hns_roce_u_bind_mw,
	.cq_event = hns_roce_u_cq_event,
	.create_cq = hns_roce_u_create_cq,
	.create_qp = hns_roce_u_create_qp,
	.create_qp_ex = hns_roce_u_create_qp_ex,
	.dealloc_mw = hns_roce_u_dealloc_mw,
	.dealloc_pd = hns_roce_u_free_pd,
	.dereg_mr = hns_roce_u_dereg_mr,
	.destroy_cq = hns_roce_u_destroy_cq,
	.modify_cq = hns_roce_u_modify_cq,
	.query_device_ex = hns_roce_u_query_device,
	.query_port = hns_roce_u_query_port,
	.query_qp = hns_roce_u_query_qp,
	.reg_mr = hns_roce_u_reg_mr,
	.rereg_mr = hns_roce_u_rereg_mr,
	.create_srq = hns_roce_u_create_srq,
	.create_srq_ex = hns_roce_u_create_srq_ex,
	.modify_srq = hns_roce_u_modify_srq,
	.query_srq = hns_roce_u_query_srq,
	.destroy_srq = hns_roce_u_destroy_srq,
	.free_context = hns_roce_free_context,
	.create_ah = hns_roce_u_create_ah,
	.destroy_ah = hns_roce_u_destroy_ah,
	.open_xrcd = hns_roce_u_open_xrcd,
	.close_xrcd = hns_roce_u_close_xrcd,
	.open_qp = hns_roce_u_open_qp,
	.get_srq_num = hns_roce_u_get_srq_num,
};

/* command value is offset[15:8] */
static void hns_roce_mmap_set_command(int command, off_t *offset)
{
	*offset |= (command & 0xff) << 8;
}

/* index value is offset[63:16] | offset[7:0] */
static void hns_roce_mmap_set_index(unsigned long index, off_t *offset)
{
	*offset |= (index & 0xff) | ((index >> 8) << 16);
}

static off_t get_uar_mmap_offset(unsigned long idx, int page_size, int cmd)
{
	off_t offset = 0;

	hns_roce_mmap_set_command(cmd, &offset);
	hns_roce_mmap_set_index(idx, &offset);

	return offset * page_size;
}

static int mmap_dca(struct hns_roce_dca_ctx *dca_ctx, int cmd_fd, int page_size,
		    size_t size)
{
	void *addr;

	addr = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, cmd_fd,
		    get_uar_mmap_offset(0, page_size, HNS_ROCE_MMAP_DCA_PAGE));
	if (addr == MAP_FAILED)
		return -EINVAL;

	dca_ctx->buf_status = addr;
	dca_ctx->sync_status = addr + size / 2;

	return 0;
}

bool hnsdv_is_supported(struct ibv_device *device)
{
	return is_hns_dev(device);
}

struct ibv_context *hnsdv_open_device(struct ibv_device *device,
				      struct hnsdv_context_attr *attr)
{
	if (!is_hns_dev(device)) {
		errno = EOPNOTSUPP;
		return NULL;
	}

	return verbs_open_device(device, attr);
}

static void set_dca_pool_param(struct hnsdv_context_attr *attr, int page_size,
			       struct hns_roce_dca_ctx *ctx)
{
	if (attr->comp_mask & HNSDV_CONTEXT_MASK_DCA_UNIT_SIZE)
		ctx->unit_size = align(attr->dca_unit_size, page_size);
	else
		ctx->unit_size = page_size * HNS_DCA_DEFAULT_UNIT_PAGES;

	/* The memory pool cannot be expanded, only init the DCA context. */
	if (ctx->unit_size == 0)
		return;

	/* If not set, the memory pool can be expanded unlimitedly. */
	if (attr->comp_mask & HNSDV_CONTEXT_MASK_DCA_MAX_SIZE)
		ctx->max_size = DIV_ROUND_UP(attr->dca_max_size,
					ctx->unit_size) * ctx->unit_size;
	else
		ctx->max_size = HNS_DCA_MAX_MEM_SIZE;

	/* If not set, the memory pool cannot be shrunk. */
	if (attr->comp_mask & HNSDV_CONTEXT_MASK_DCA_MIN_SIZE)
		ctx->min_size = DIV_ROUND_UP(attr->dca_min_size,
					ctx->unit_size) * ctx->unit_size;
	else
		ctx->min_size = HNS_DCA_MAX_MEM_SIZE;
}

static int init_dca_context(struct hns_roce_context *ctx, int cmd_fd,
			    int page_size, struct hnsdv_context_attr *attr,
			    int max_qps, int mmap_size)
{
	struct hns_roce_dca_ctx *dca_ctx = &ctx->dca_ctx;
	int ret;

	dca_ctx->unit_size = 0;
	dca_ctx->mem_cnt = 0;
	list_head_init(&dca_ctx->mem_list);
	ret = pthread_spin_init(&dca_ctx->lock, PTHREAD_PROCESS_PRIVATE);
	if (ret)
		return ret;

	if (!attr || !(attr->flags & HNSDV_CONTEXT_FLAGS_DCA))
		return 0;

	set_dca_pool_param(attr, page_size, dca_ctx);

	if (mmap_size > 0) {
		const unsigned int bits_per_qp = 2 * HNS_DCA_BITS_PER_STATUS;

		if (!mmap_dca(dca_ctx, cmd_fd, page_size, mmap_size)) {
			dca_ctx->status_size = mmap_size;
			dca_ctx->max_qps = min_t(int, max_qps,
						 mmap_size *  8 / bits_per_qp);
		}
	}

	return 0;
}

static void uninit_dca_context(struct hns_roce_context *ctx)
{
	struct hns_roce_dca_ctx *dca_ctx = &ctx->dca_ctx;

	if (!(ctx->cap_flags & HNS_ROCE_CAP_FLAG_DCA_MODE))
		return;

	pthread_spin_lock(&dca_ctx->lock);
	hns_roce_cleanup_dca_mem(ctx);
	pthread_spin_unlock(&dca_ctx->lock);

	if (dca_ctx->buf_status)
		munmap(dca_ctx->buf_status, dca_ctx->status_size);

	pthread_spin_destroy(&dca_ctx->lock);
}

static int hns_roce_mmap(struct hns_roce_device *hr_dev,
			 struct hns_roce_context *context, int cmd_fd)
{
	int page_size = hr_dev->page_size;
	off_t offset;

	offset = get_uar_mmap_offset(0, page_size, HNS_ROCE_MMAP_REGULAR_PAGE);
	context->uar = mmap(NULL, page_size, PROT_READ | PROT_WRITE,
			    MAP_SHARED, cmd_fd, offset);
	if (context->uar == MAP_FAILED)
		return -EINVAL;

	offset = get_uar_mmap_offset(1, page_size, HNS_ROCE_MMAP_REGULAR_PAGE);
	if (hr_dev->hw_version == HNS_ROCE_HW_VER1) {
		/*
		 * when vma->vm_pgoff is 1, the cq_tptr_base includes 64K CQ,
		 * a pointer of CQ need 2B size
		 */
		context->cq_tptr_base = mmap(NULL, HNS_ROCE_CQ_DB_BUF_SIZE,
					     PROT_READ | PROT_WRITE, MAP_SHARED,
					     cmd_fd, offset);
		if (context->cq_tptr_base == MAP_FAILED)
			goto db_free;
	}

	return 0;

db_free:
	munmap(context->uar, hr_dev->page_size);

	return -EINVAL;
}

static void ucontext_set_cmd(struct hns_roce_alloc_ucontext *cmd,
			     struct hnsdv_context_attr *attr)
{
	if (!attr || !(attr->flags & HNSDV_CONTEXT_FLAGS_DCA))
		return;

	if (attr->comp_mask & HNSDV_CONTEXT_MASK_DCA_PRIME_QPS) {
		cmd->comp = HNS_ROCE_ALLOC_UCTX_COMP_DCA_MAX_QPS;
		cmd->dca_max_qps = attr->dca_prime_qps;
	}
}

static struct verbs_context *
hns_roce_alloc_context(struct ibv_device *ibdev, int cmd_fd, void *private_data)
{
	struct hnsdv_context_attr *ctx_attr = private_data;
	struct hns_roce_device *hr_dev = to_hr_dev(ibdev);
	struct hns_roce_alloc_ucontext_resp resp = {};
	struct hns_roce_alloc_ucontext cmd = {};
	struct ibv_device_attr dev_attrs;
	struct hns_roce_context *context;
	int i;

	context = verbs_init_and_alloc_context(ibdev, cmd_fd, context, ibv_ctx,
					       RDMA_DRIVER_HNS);
	if (!context)
		return NULL;

	ucontext_set_cmd(&cmd, ctx_attr);
	if (ibv_cmd_get_context(&context->ibv_ctx, &cmd.ibv_cmd, sizeof(cmd),
				&resp.ibv_resp, sizeof(resp)))
		goto err_free;

	if (!resp.cqe_size)
		context->cqe_size = HNS_ROCE_CQE_SIZE;
	else if (resp.cqe_size <= HNS_ROCE_V3_CQE_SIZE)
		context->cqe_size = resp.cqe_size;
	else
		context->cqe_size = HNS_ROCE_V3_CQE_SIZE;

	context->cap_flags = resp.cap_flags;

	context->num_qps = resp.qp_tab_size;
	context->num_srqs = resp.srq_tab_size;

	context->qp_table_shift = ffs(context->num_qps) - 1 -
				  HNS_ROCE_QP_TABLE_BITS;
	context->qp_table_mask = (1 << context->qp_table_shift) - 1;
	pthread_mutex_init(&context->qp_table_mutex, NULL);
	for (i = 0; i < HNS_ROCE_QP_TABLE_SIZE; ++i)
		context->qp_table[i].refcnt = 0;

	context->srq_table_shift = ffs(context->num_srqs) - 1 -
				       HNS_ROCE_SRQ_TABLE_BITS;
	context->srq_table_mask = (1 << context->srq_table_shift) - 1;
	pthread_mutex_init(&context->srq_table_mutex, NULL);
	for (i = 0; i < HNS_ROCE_SRQ_TABLE_SIZE; ++i)
		context->srq_table[i].refcnt = 0;

	if (hns_roce_u_query_device(&context->ibv_ctx.context, NULL,
				    container_of(&dev_attrs,
						 struct ibv_device_attr_ex,
						 orig_attr),
				    sizeof(dev_attrs)))
		goto err_free;

	hr_dev->hw_version = dev_attrs.hw_ver;
	context->max_qp_wr = dev_attrs.max_qp_wr;
	context->max_sge = dev_attrs.max_sge;
	context->max_cqe = dev_attrs.max_cqe;
	context->max_srq_wr = dev_attrs.max_srq_wr;
	context->max_srq_sge = dev_attrs.max_srq_sge;

	pthread_spin_init(&context->uar_lock, PTHREAD_PROCESS_PRIVATE);

	verbs_set_ops(&context->ibv_ctx, &hns_common_ops);
	verbs_set_ops(&context->ibv_ctx, &hr_dev->u_hw->hw_ops);

	if (init_dca_context(context, cmd_fd, hr_dev->page_size, ctx_attr,
			     resp.dca_qps, resp.dca_mmap_size))
		goto err_free;

	if (hns_roce_mmap(hr_dev, context, cmd_fd))
		goto dca_free;

	return &context->ibv_ctx;

dca_free:
	uninit_dca_context(context);

err_free:
	verbs_uninit_context(&context->ibv_ctx);
	free(context);
	return NULL;
}

static void hns_roce_free_context(struct ibv_context *ibctx)
{
	struct hns_roce_device *hr_dev = to_hr_dev(ibctx->device);
	struct hns_roce_context *context = to_hr_ctx(ibctx);

	munmap(context->uar, hr_dev->page_size);
	if (hr_dev->hw_version == HNS_ROCE_HW_VER1)
		munmap(context->cq_tptr_base, HNS_ROCE_CQ_DB_BUF_SIZE);

	uninit_dca_context(context);

	verbs_uninit_context(&context->ibv_ctx);
	free(context);
}

static void hns_uninit_device(struct verbs_device *verbs_device)
{
	struct hns_roce_device *dev = to_hr_dev(&verbs_device->device);

	free(dev);
}

static struct verbs_device *hns_device_alloc(struct verbs_sysfs_dev *sysfs_dev)
{
	struct hns_roce_device *dev;

	dev = calloc(1, sizeof(*dev));
	if (!dev)
		return NULL;

	dev->u_hw = sysfs_dev->match->driver_data;
	dev->hw_version = dev->u_hw->hw_version;
	dev->page_size = sysconf(_SC_PAGESIZE);
	return &dev->ibv_dev;
}

static const struct verbs_device_ops hns_roce_dev_ops = {
	.name = "hns",
	.match_min_abi_version = 0,
	.match_max_abi_version = INT_MAX,
	.match_table = hca_table,
	.alloc_device = hns_device_alloc,
	.uninit_device = hns_uninit_device,
	.alloc_context = hns_roce_alloc_context,
};

bool is_hns_dev(struct ibv_device *device)
{
	struct verbs_device *verbs_device = verbs_get_device(device);

	return verbs_device->ops == &hns_roce_dev_ops;
}
PROVIDER_DRIVER(hns, hns_roce_dev_ops);
