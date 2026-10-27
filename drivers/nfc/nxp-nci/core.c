// SPDX-License-Identifier: GPL-2.0-only
/*
 * Generic driver for NXP NCI NFC chips
 *
 * Copyright (C) 2014  NXP Semiconductors  All rights reserved.
 *
 * Authors: Clément Perrochaud <clement.perrochaud@nxp.com>
 *
 * Derived from PN544 device driver:
 * Copyright (C) 2012  Intel Corporation. All rights reserved.
 */

#include <linux/delay.h>
#include <linux/module.h>
#include <linux/nfc.h>

#include <net/nfc/nci_core.h>

#include "nxp-nci.h"

#define NXP_NCI_HDR_LEN	4

#define NXP_NCI_NFC_PROTOCOLS (NFC_PROTO_JEWEL_MASK | \
			       NFC_PROTO_MIFARE_MASK | \
			       NFC_PROTO_FELICA_MASK | \
			       NFC_PROTO_ISO14443_MASK | \
			       NFC_PROTO_ISO14443_B_MASK | \
			       NFC_PROTO_ISO15693_MASK | \
			       NFC_PROTO_NFC_DEP_MASK)

/*
 * Some NCI controllers (the ST54L on the Pixel 9a) poll a few times after
 * RF_DISCOVER and then wait in a low-power card detection mode that never
 * notices a card. Restarting discovery every rediscover_ms keeps them
 * polling. 0 (the default) leaves discovery alone.
 */
static unsigned int rediscover_ms;
module_param(rediscover_ms, uint, 0644);
MODULE_PARM_DESC(rediscover_ms, "Restart an idle discovery this often (ms), 0 never");

#define NXP_NCI_RF_PLL_UNLOCKED_NTF nci_opcode_pack(NCI_GID_RF_MGMT, 0x21)
#define NXP_NCI_RF_TXLDO_ERROR_NTF nci_opcode_pack(NCI_GID_RF_MGMT, 0x23)

static void nxp_nci_rediscover_work(struct work_struct *work)
{
	struct nxp_nci_info *info = container_of(to_delayed_work(work),
						 struct nxp_nci_info,
						 rediscover_work);
	struct nci_dev *ndev = info->ndev;
	struct nfc_dev *nfc = ndev->nfc_dev;
	unsigned int ms = READ_ONCE(rediscover_ms);
	__u32 prots;

	if (!ms || READ_ONCE(info->mode) != NXP_NCI_MODE_NCI)
		return;

	/* the NFC core's order: the device lock, then the NCI request lock */
	device_lock(&nfc->dev);
	if (nfc->polling && !ndev->target_active_prot &&
	    atomic_read(&ndev->state) == NCI_DISCOVERY && ndev->poll_prots) {
		prots = ndev->poll_prots;
		nfc->ops->stop_poll(nfc);
		if (nfc->ops->start_poll(nfc, prots, 0))
			dev_dbg(info->pdev, "rediscover: restart failed\n");
	}
	device_unlock(&nfc->dev);

	if (READ_ONCE(info->mode) == NXP_NCI_MODE_NCI)
		schedule_delayed_work(&info->rediscover_work, msecs_to_jiffies(ms));
}

static int nxp_nci_open(struct nci_dev *ndev)
{
	struct nxp_nci_info *info = nci_get_drvdata(ndev);
	int r = 0;

	mutex_lock(&info->info_lock);

	if (info->mode != NXP_NCI_MODE_COLD) {
		r = -EBUSY;
		goto open_exit;
	}

	if (info->phy_ops->set_mode)
		r = info->phy_ops->set_mode(info->phy_id, NXP_NCI_MODE_NCI);

	info->mode = NXP_NCI_MODE_NCI;
	if (rediscover_ms)
		schedule_delayed_work(&info->rediscover_work,
				      msecs_to_jiffies(rediscover_ms));

open_exit:
	mutex_unlock(&info->info_lock);
	return r;
}

static int nxp_nci_close(struct nci_dev *ndev)
{
	struct nxp_nci_info *info = nci_get_drvdata(ndev);
	int r = 0;

	mutex_lock(&info->info_lock);

	if (info->phy_ops->set_mode)
		r = info->phy_ops->set_mode(info->phy_id, NXP_NCI_MODE_COLD);

	info->mode = NXP_NCI_MODE_COLD;
	/*
	 * Not the _sync variant: the NCI core holds its request lock here, and
	 * the work takes it. A late run finds the device down and stops.
	 */
	cancel_delayed_work(&info->rediscover_work);

	mutex_unlock(&info->info_lock);
	return r;
}

static int nxp_nci_send(struct nci_dev *ndev, struct sk_buff *skb)
{
	struct nxp_nci_info *info = nci_get_drvdata(ndev);
	int r;

	if (!info->phy_ops->write) {
		kfree_skb(skb);
		return -EOPNOTSUPP;
	}

	if (info->mode != NXP_NCI_MODE_NCI) {
		kfree_skb(skb);
		return -EINVAL;
	}

	r = info->phy_ops->write(info->phy_id, skb);
	if (r < 0) {
		kfree_skb(skb);
		return r;
	}

	consume_skb(skb);
	return 0;
}

static int nxp_nci_rf_pll_unlocked_ntf(struct nci_dev *ndev,
				       struct sk_buff *skb)
{
	nfc_err(&ndev->nfc_dev->dev,
		"PLL didn't lock. Missing or unstable clock?\n");

	return 0;
}

static int nxp_nci_rf_txldo_error_ntf(struct nci_dev *ndev,
				      struct sk_buff *skb)
{
	nfc_err(&ndev->nfc_dev->dev,
		"RF transmitter couldn't start. Bad power and/or configuration?\n");

	return 0;
}

static const struct nci_driver_ops nxp_nci_core_ops[] = {
	{
		.opcode = NXP_NCI_RF_PLL_UNLOCKED_NTF,
		.ntf = nxp_nci_rf_pll_unlocked_ntf,
	},
	{
		.opcode = NXP_NCI_RF_TXLDO_ERROR_NTF,
		.ntf = nxp_nci_rf_txldo_error_ntf,
	},
};

static const struct nci_ops nxp_nci_ops = {
	.open = nxp_nci_open,
	.close = nxp_nci_close,
	.send = nxp_nci_send,
	.fw_download = nxp_nci_fw_download,
	.core_ops = nxp_nci_core_ops,
	.n_core_ops = ARRAY_SIZE(nxp_nci_core_ops),
};

int nxp_nci_probe(void *phy_id, struct device *pdev,
		  const struct nxp_nci_phy_ops *phy_ops,
		  unsigned int max_payload,
		  struct nci_dev **ndev)
{
	struct nxp_nci_info *info;
	int r;

	info = devm_kzalloc(pdev, sizeof(struct nxp_nci_info), GFP_KERNEL);
	if (!info)
		return -ENOMEM;

	info->phy_id = phy_id;
	info->pdev = pdev;
	info->phy_ops = phy_ops;
	info->max_payload = max_payload;
	INIT_WORK(&info->fw_info.work, nxp_nci_fw_work);
	INIT_DELAYED_WORK(&info->rediscover_work, nxp_nci_rediscover_work);
	init_completion(&info->fw_info.cmd_completion);
	mutex_init(&info->info_lock);

	if (info->phy_ops->set_mode) {
		r = info->phy_ops->set_mode(info->phy_id, NXP_NCI_MODE_COLD);
		if (r < 0)
			return r;
	}

	info->mode = NXP_NCI_MODE_COLD;

	info->ndev = nci_allocate_device(&nxp_nci_ops, NXP_NCI_NFC_PROTOCOLS,
					 NXP_NCI_HDR_LEN, 0);
	if (!info->ndev)
		return -ENOMEM;

	nci_set_parent_dev(info->ndev, pdev);
	nci_set_drvdata(info->ndev, info);
	r = nci_register_device(info->ndev);
	if (r < 0) {
		nci_free_device(info->ndev);
		return r;
	}

	*ndev = info->ndev;
	return r;
}
EXPORT_SYMBOL(nxp_nci_probe);

void nxp_nci_remove(struct nci_dev *ndev)
{
	struct nxp_nci_info *info = nci_get_drvdata(ndev);

	if (info->mode == NXP_NCI_MODE_FW)
		nxp_nci_fw_work_complete(info, -ESHUTDOWN);
	cancel_work_sync(&info->fw_info.work);

	/*
	 * nci_unregister_device() closes an open device through
	 * nxp_nci_close(), which takes info_lock itself: unregister first.
	 */
	nci_unregister_device(ndev);

	mutex_lock(&info->info_lock);
	if (info->phy_ops->set_mode)
		info->phy_ops->set_mode(info->phy_id, NXP_NCI_MODE_COLD);
	mutex_unlock(&info->info_lock);

	cancel_delayed_work_sync(&info->rediscover_work);
	nci_free_device(ndev);
}
EXPORT_SYMBOL(nxp_nci_remove);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("NXP NCI NFC driver");
MODULE_AUTHOR("Clément Perrochaud <clement.perrochaud@nxp.com>");
