/*
 * drivers/video/tegra/host/bus_client.h
 *
 * Tegra Graphics Host Cpu Register Access
 *
 * Copyright (c) 2010-2012, NVIDIA Corporation.
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
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef __NVHOST_BUS_CLIENT_H
#define __NVHOST_BUS_CLIENT_H

#include <linux/types.h>
struct nvhost_device;

void nvhost_read_module_regs(struct nvhost_device *ndev,
			u32 offset, int count, u32 *values);

void nvhost_write_module_regs(struct nvhost_device *ndev,
			u32 offset, int count, const u32 *values);

#endif
