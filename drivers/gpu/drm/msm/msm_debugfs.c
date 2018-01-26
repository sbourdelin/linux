/*
 * Copyright (C) 2013-2016 Red Hat
 * Author: Rob Clark <robdclark@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 as published by
 * the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifdef CONFIG_DEBUG_FS

#include <generated/utsrelease.h>
#include <linux/debugfs.h>
#include "msm_drv.h"
#include "msm_gpu.h"
#include "msm_kms.h"
#include "msm_debugfs.h"

static int msm_gpu_crash_show(struct seq_file *m, void *data)
{
	struct msm_gpu *gpu = m->private;
	struct msm_gpu_state *state;

	state = msm_gpu_crashstate_get(gpu);
	if (!state)
		return 0;

	seq_printf(m, "%s Crash Status:\n", gpu->name);
	seq_puts(m, "Kernel: " UTS_RELEASE "\n");
	seq_printf(m, "Time: %ld s %ld us\n",
		state->time.tv_sec, state->time.tv_usec);
	if (state->comm)
		seq_printf(m, "comm: %s\n", state->comm);
	if (state->cmd)
		seq_printf(m, "cmdline: %s\n", state->cmd);

	gpu->funcs->show(gpu, state, m);

	msm_gpu_crashstate_put(gpu);

	return 0;
}

static ssize_t msm_gpu_crash_write(struct file *file, const char __user *buf,
		size_t count, loff_t *pos)
{
	struct msm_gpu *gpu = ((struct seq_file *)file->private_data)->private;

	dev_err(gpu->dev->dev, "Releasing the GPU crash state\n");
	msm_gpu_crashstate_put(gpu);

	return count;
}

static int msm_gpu_crash_open(struct inode *inode, struct file *file)
{
	struct msm_drm_private *priv = inode->i_private;

	if (!priv->gpu)
		return -ENODEV;

	return single_open(file, msm_gpu_crash_show, priv->gpu);
}

static const struct file_operations msm_gpu_crash_fops = {
	.owner = THIS_MODULE,
	.open = msm_gpu_crash_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = single_release,
	.write = msm_gpu_crash_write,
};

static int msm_gpu_show(struct drm_device *dev, struct seq_file *m)
{
	struct msm_drm_private *priv = dev->dev_private;
	struct msm_gpu *gpu = priv->gpu;
	struct msm_gpu_state *state;

	if (!gpu)
		return 0;

	pm_runtime_get_sync(&gpu->pdev->dev);
	state = gpu->funcs->gpu_state_get(gpu);
	pm_runtime_put_sync(&gpu->pdev->dev);

	if (IS_ERR(state))
		return PTR_ERR(state);

	seq_printf(m, "%s Status:\n", gpu->name);
	gpu->funcs->show(gpu, state, m);

	gpu->funcs->gpu_state_put(state);

	return 0;
}

static int msm_gem_show(struct drm_device *dev, struct seq_file *m)
{
	struct msm_drm_private *priv = dev->dev_private;
	struct msm_gpu *gpu = priv->gpu;

	if (gpu) {
		seq_printf(m, "Active Objects (%s):\n", gpu->name);
		msm_gem_describe_objects(&gpu->active_list, m);
	}

	seq_printf(m, "Inactive Objects:\n");
	msm_gem_describe_objects(&priv->inactive_list, m);

	return 0;
}

static int msm_mm_show(struct drm_device *dev, struct seq_file *m)
{
	struct drm_printer p = drm_seq_file_printer(m);

	drm_mm_print(&dev->vma_offset_manager->vm_addr_space_mm, &p);

	return 0;
}

static int msm_fb_show(struct drm_device *dev, struct seq_file *m)
{
	struct msm_drm_private *priv = dev->dev_private;
	struct drm_framebuffer *fb, *fbdev_fb = NULL;

	if (priv->fbdev) {
		seq_printf(m, "fbcon ");
		fbdev_fb = priv->fbdev->fb;
		msm_framebuffer_describe(fbdev_fb, m);
	}

	mutex_lock(&dev->mode_config.fb_lock);
	list_for_each_entry(fb, &dev->mode_config.fb_list, head) {
		if (fb == fbdev_fb)
			continue;

		seq_printf(m, "user ");
		msm_framebuffer_describe(fb, m);
	}
	mutex_unlock(&dev->mode_config.fb_lock);

	return 0;
}

static int show_locked(struct seq_file *m, void *arg)
{
	struct drm_info_node *node = (struct drm_info_node *) m->private;
	struct drm_device *dev = node->minor->dev;
	int (*show)(struct drm_device *dev, struct seq_file *m) =
			node->info_ent->data;
	int ret;

	ret = mutex_lock_interruptible(&dev->struct_mutex);
	if (ret)
		return ret;

	ret = show(dev, m);

	mutex_unlock(&dev->struct_mutex);

	return ret;
}

static struct drm_info_list msm_debugfs_list[] = {
		{"gpu", show_locked, 0, msm_gpu_show},
		{"gem", show_locked, 0, msm_gem_show},
		{ "mm", show_locked, 0, msm_mm_show },
		{ "fb", show_locked, 0, msm_fb_show },
};

static int late_init_minor(struct drm_minor *minor)
{
	int ret;

	if (!minor)
		return 0;

	ret = msm_rd_debugfs_init(minor);
	if (ret) {
		dev_err(minor->dev->dev, "could not install rd debugfs\n");
		return ret;
	}

	ret = msm_perf_debugfs_init(minor);
	if (ret) {
		dev_err(minor->dev->dev, "could not install perf debugfs\n");
		return ret;
	}

	return 0;
}

int msm_debugfs_late_init(struct drm_device *dev)
{
	int ret;
	ret = late_init_minor(dev->primary);
	if (ret)
		return ret;
	ret = late_init_minor(dev->render);
	if (ret)
		return ret;
	ret = late_init_minor(dev->control);
	return ret;
}

int msm_debugfs_init(struct drm_minor *minor)
{
	struct drm_device *dev = minor->dev;
	struct msm_drm_private *priv = dev->dev_private;
	int ret;

	ret = drm_debugfs_create_files(msm_debugfs_list,
			ARRAY_SIZE(msm_debugfs_list),
			minor->debugfs_root, minor);

	if (ret) {
		dev_err(dev->dev, "could not install msm_debugfs_list\n");
		return ret;
	}

	debugfs_create_file("crash", 0644, minor->debugfs_root,
		priv, &msm_gpu_crash_fops);

	if (priv->kms->funcs->debugfs_init)
		ret = priv->kms->funcs->debugfs_init(priv->kms, minor);

	return ret;
}
#endif

