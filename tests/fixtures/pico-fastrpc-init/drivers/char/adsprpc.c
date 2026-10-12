// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (c) 2012-2021, The Linux Foundation. All rights reserved. */
static int fastrpc_init_process(struct fastrpc_file *fl,
				struct fastrpc_ioctl_init_attrs *uproc)
{
	int err = 0, rh_hyp_done = 0;
	struct fastrpc_apps *me = &gfa;
	struct fastrpc_ioctl_invoke_crc ioctl;
	struct fastrpc_ioctl_init *init = &uproc->init;
	struct smq_phy_page pages[1];
	struct fastrpc_mmap *file = NULL, *mem = NULL;
	struct fastrpc_buf *imem = NULL;
	unsigned long imem_dma_attr = 0;
	char *proc_name = NULL;

	int unsigned_request = (uproc->attrs & FASTRPC_MODE_UNSIGNED_MODULE);
	int cid = fl->cid;
	struct fastrpc_channel_ctx *chan = &me->channel[cid];

	if (chan->unsigned_support &&
		fl->dev_minor == MINOR_NUM_DEV) {
		/* Make sure third party applications */
		/* can spawn only unsigned PD when */
		/* channel configured as secure. */
		if (chan->secure && !unsigned_request) {
			err = -ECONNREFUSED;
			goto bail;
		}
	}

	return fastrpc_channel_open(fl);
bail:
	return err;
}
