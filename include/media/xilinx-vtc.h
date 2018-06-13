/*
 * Xilinx Video Timing Controller
 *
 * Copyright (C) 2013-2015 Ideas on Board
 * Copyright (C) 2013-2015 Xilinx, Inc.
 *
 * Contacts: Hyun Kwon <hyun.kwon@xilinx.com>
 *           Laurent Pinchart <laurent.pinchart@ideasonboard.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <media/xilinx-vip.h>


#ifndef __XILINX_VTC_H__
#define __XILINX_VTC_H__

struct device_node;
struct xvtc_device;

#define XVTC_MAX_HSIZE			8191
#define XVTC_MAX_VSIZE			8191


struct xvtc_config {
	unsigned int hblank_start;
	unsigned int hsync_start;
	unsigned int hsync_end;
	unsigned int hsize;
	unsigned int vblank_start;
	unsigned int vsync_start;
	unsigned int vsync_end;
	unsigned int vsize;
};

/**
 * struct xvtc_device - Xilinx Video Timing Controller device structure
 * @xvip: Xilinx Video IP device
 * @list: entry in the global VTC list
 * @has_detector: the VTC has a timing detector
 * @has_generator: the VTC has a timing generator
 * @config: generator timings configuration
 */
struct xvtc_device {
	struct xvip_device xvip;
	struct list_head list;

	bool has_detector;
	bool has_generator;

	struct xvtc_config config;
};


struct xvtc_device *xvtc_of_get(struct device_node *np);
void xvtc_put(struct xvtc_device *xvtc);

int xvtc_generator_start(struct xvtc_device *xvtc,
			 const struct xvtc_config *config);
int xvtc_generator_stop(struct xvtc_device *xvtc);

#endif /* __XILINX_VTC_H__ */
