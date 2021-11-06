/*
 * Copyright (c) 2017-2018, NVIDIA CORPORATION. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License version 2
 * as published by the Free Software Foundation; or, when distributed
 * separately from the Linux kernel or incorporated into other
 * software packages, subject to the following license:
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *  * Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 *  * Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *  * Neither the name of NVIDIA CORPORATION nor the names of its
 *    contributors may be used to endorse or promote products derived
 *    from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <linux/version.h>
#include <linux/dma-buf-map.h>
#include <linux/dma-mapping.h>
#include <linux/dma-map-ops.h>

#include <drm/drm.h>
#include <drm/drm_drv.h>
#include <drm/drm_gem_cma_helper.h>

#include <nvdla_linux.h>
#include <nvdla_ioctl.h>

#include "nvdla_gem.h"

#define to_nvdla_obj(x) container_of(x, struct nvdla_gem_object, object)

struct nvdla_gem_object {
	struct drm_gem_object object;

	void *kvaddr;
	dma_addr_t dma_addr;
	unsigned long dma_attrs;

	/* Used when IOMMU is enabled */
	unsigned long num_pages;
	struct page **pages;
};

static int32_t nvdla_fill_task_desc(struct nvdla_ioctl_submit_task *local_task,
				struct nvdla_task *task)
{
	struct nvdla_mem_handle *handles;

	/* update task desc fields */
	task->num_addresses = local_task->num_addresses;

	handles = kzalloc(local_task->num_addresses *
				sizeof(struct nvdla_mem_handle), GFP_KERNEL);
	if (handles == NULL)
		return -EFAULT;

	/* get user addresses list */
	if (copy_from_user(handles,
		(void __user *)local_task->address_list,
		(task->num_addresses *
			sizeof(struct nvdla_mem_handle)))) {
		pr_err("failed to copy address list from user ptr\n");
		kfree(handles);
		return -EFAULT;
	}

	task->address_list = handles;

	return 0;
}

static int32_t nvdla_submit(struct drm_device *drm, void *arg,
					struct drm_file *file)
{
	int32_t err = 0;
	struct nvdla_task *task;
	struct nvdla_ioctl_submit_task local_task;
	struct nvdla_ioctl_submit_task __user *user_task;
	struct nvdla_device *nvdla_dev = dev_get_drvdata(drm->dev);
	struct nvdla_submit_args *args =
			(struct nvdla_submit_args *)arg;

	user_task = (struct nvdla_ioctl_submit_task __user *)
			(uintptr_t)args->tasks;
	if (!user_task)
		return -EINVAL;

	/* IOCTL copy descriptors */
	if (copy_from_user(&local_task, (void __user *)user_task,
			(sizeof(*user_task))))
		return -EFAULT;

	task = kzalloc(sizeof(*task), GFP_KERNEL);
	if (task == NULL)
		return -EFAULT;

	nvdla_dev->task = task;
	kref_init(&task->ref);
	task->nvdla_dev = nvdla_dev;
	task->file = file;

	/* update task desc fields */
	err = nvdla_fill_task_desc(&local_task, task);
	if (err)
		goto free_task_desc;

	err = nvdla_task_submit(nvdla_dev, task);

	kfree(task->address_list);

free_task_desc:
	kfree(task);
	return err;
}

static int32_t nvdla_gem_alloc(struct nvdla_gem_object *nobj)
{
	struct drm_gem_object *dobj = &nobj->object;
	struct drm_device *drm = dobj->dev;

	nobj->dma_attrs = DMA_ATTR_WRITE_COMBINE;

	nobj->kvaddr = dma_alloc_attrs(drm->dev, dobj->size, &nobj->dma_addr,
						GFP_KERNEL, nobj->dma_attrs);

	if (!nobj->kvaddr)
		return -ENOMEM;

	return 0;
}

static void nvdla_gem_free(struct nvdla_gem_object *nobj)
{
	struct drm_gem_object *dobj = &nobj->object;
	struct drm_device *drm = dobj->dev;

	dma_free_attrs(drm->dev, dobj->size, nobj->kvaddr, nobj->dma_addr,
				nobj->dma_attrs);
}

static vm_fault_t nvdla_gem_fault(struct vm_fault *vmf)
{
        struct vm_area_struct *vma = vmf->vma;
        struct drm_gem_object *gem = vma->vm_private_data;
        struct nvdla_gem_object *nobj = to_nvdla_obj(gem);
        struct page *page;
        pgoff_t offset;

        if (!nobj->pages)
                return VM_FAULT_SIGBUS;

        offset = (vmf->address - vma->vm_start) >> PAGE_SHIFT;
        page = nobj->pages[offset];

        return vmf_insert_page(vma, vmf->address, page);
}

const struct vm_operations_struct nvdla_gem_vm_ops = { 
        .fault = nvdla_gem_fault,
        .open = drm_gem_vm_open,
        .close = drm_gem_vm_close,
};

// https://www.mail-archive.com/amd-gfx@lists.freedesktop.org/msg53124.html
// https://lists.freedesktop.org/archives/intel-gfx/2019-June/201694.html
// https://www.spinics.net/lists/linux-tegra/msg53819.html
// https://patchew.org/Xen/20200915145958.19993-1-tzimmermann@suse.de/20200915145958.19993-18-tzimmermann@suse.de/
static const struct drm_gem_object_funcs nvdla_gem_object_funcs = {
	.free = nvdla_gem_free_object,
	.get_sg_table = nvdla_drm_gem_prime_get_sg_table,
	.export = drm_gem_prime_export,
	.vmap = nvdla_drm_gem_prime_vmap,
	.vunmap = nvdla_drm_gem_prime_vunmap,
	.vm_ops = &nvdla_gem_vm_ops,
	// .vm_ops = &drm_gem_cma_vm_ops,
};

static struct nvdla_gem_object *
nvdla_gem_create_object(struct drm_device *drm, uint32_t size)
{
	int32_t ret;
	struct drm_gem_object *dobj;
	struct nvdla_gem_object *nobj;

	size = round_up(size, PAGE_SIZE);

	nobj = kzalloc(sizeof(*nobj), GFP_KERNEL);
	if (!nobj)
		return ERR_PTR(-ENOMEM);

	dobj = &nobj->object;

	dobj->funcs = &nvdla_gem_object_funcs;

	drm_gem_private_object_init(drm, dobj, size);

	ret = nvdla_gem_alloc(nobj);
	if (ret)
		goto free_nvdla_obj;

	return nobj;

free_nvdla_obj:
	kfree(nobj);
	return ERR_PTR(ret);
}

static void nvdla_gem_free_object(struct drm_gem_object *dobj)
{
	struct nvdla_gem_object *nobj;

	drm_gem_free_mmap_offset(dobj);

	nobj = to_nvdla_obj(dobj);

	nvdla_gem_free(nobj);

	kfree(nobj);
}

static struct nvdla_gem_object *
nvdla_gem_create_with_handle(struct drm_file *file_priv,
				struct drm_device *drm, uint32_t size,
				uint32_t *handle)
{
	int32_t ret;
	struct drm_gem_object *dobj;
	struct nvdla_gem_object *nobj;

	nobj = nvdla_gem_create_object(drm, size);
	if (IS_ERR(nobj))
		return ERR_CAST(nobj);

	dobj = &nobj->object;

	ret = drm_gem_handle_create(file_priv, dobj, handle);
	if (ret)
		goto free_drm_object;

	drm_gem_object_put(dobj);

	return nobj;

free_drm_object:
	nvdla_gem_free_object(dobj);

	return ERR_PTR(ret);
}

static int32_t nvdla_gem_create(struct drm_device *drm, void *data,
				struct drm_file *file)
{
	struct nvdla_gem_object *nobj;
	struct nvdla_gem_create_args *args = data;

	nobj = nvdla_gem_create_with_handle(file, drm, args->size,
					 &args->handle);
	if (IS_ERR(nobj))
		return PTR_ERR(nobj);

	return 0;
}

static int32_t nvdla_drm_gem_object_mmap(struct drm_gem_object *dobj,
					struct vm_area_struct *vma)
{
	int32_t ret;
	struct nvdla_gem_object *nobj = to_nvdla_obj(dobj);
	struct drm_device *drm = dobj->dev;

	vma->vm_flags &= ~VM_PFNMAP;
	vma->vm_pgoff = 0;

	ret = dma_mmap_attrs(drm->dev, vma, nobj->kvaddr, nobj->dma_addr,
			     dobj->size, nobj->dma_attrs);
	if (ret)
		drm_gem_vm_close(vma);

	return ret;
}

static int32_t nvdla_drm_gem_mmap_buf(struct drm_gem_object *obj,
				struct vm_area_struct *vma)
{
	int32_t ret;

	ret = drm_gem_mmap_obj(obj, obj->size, vma);
	if (ret)
		return ret;

	return nvdla_drm_gem_object_mmap(obj, vma);
}

static int32_t nvdla_drm_gem_mmap(struct file *filp, struct vm_area_struct *vma)
{
	int32_t ret;
	struct drm_gem_object *obj;

	ret = drm_gem_mmap(filp, vma);
	if (ret)
		return ret;

	obj = vma->vm_private_data;

	return nvdla_drm_gem_object_mmap(obj, vma);
}

static struct sg_table
*nvdla_drm_gem_prime_get_sg_table(struct drm_gem_object *dobj)
{
	int32_t ret;
	struct sg_table *sgt;
	struct drm_device *drm = dobj->dev;
	struct nvdla_gem_object *nobj = to_nvdla_obj(dobj);

	sgt = kzalloc(sizeof(*sgt), GFP_KERNEL);
	if (!sgt)
		return ERR_PTR(-ENOMEM);

	ret = dma_get_sgtable_attrs(drm->dev, sgt, nobj->kvaddr,
				    nobj->dma_addr, dobj->size,
				    nobj->dma_attrs);
	if (ret) {
		DRM_ERROR("failed to allocate sgt, %d\n", ret);
		kfree(sgt);
		return ERR_PTR(ret);
	}

	return sgt;
}

static int nvdla_drm_gem_prime_vmap(struct drm_gem_object *obj, struct dma_buf_map *map)
{
	struct nvdla_gem_object *nobj = to_nvdla_obj(obj);

	if (nobj->pages) {
		void *vaddr = vmap(nobj->pages, nobj->num_pages, VM_MAP,
						  pgprot_writecombine(PAGE_KERNEL));
		if (!vaddr)
			return -ENOMEM;
		dma_buf_map_set_vaddr(map, vaddr);
		return 0;
	}

	if (nobj->dma_attrs & DMA_ATTR_NO_KERNEL_MAPPING)
		return -ENOMEM;

	dma_buf_map_set_vaddr(map, nobj->kvaddr);

	return 0;
}

static void nvdla_drm_gem_prime_vunmap(struct drm_gem_object *obj, struct dma_buf_map *map)
{
	struct nvdla_gem_object *nobj = to_nvdla_obj(obj);

	if (nobj->pages) {
		vunmap(map->vaddr);
		return;
	}

	/* Nothing to do if allocated by DMA mapping API. */
}

int32_t nvdla_gem_dma_addr(struct drm_device *dev, struct drm_file *file,
			uint32_t fd, dma_addr_t *addr)
{
	int32_t ret;
	uint32_t handle;
	struct nvdla_gem_object *nobj;
	struct drm_gem_object *dobj;

	ret = drm_gem_prime_fd_to_handle(dev, file, fd, &handle);
	if (ret)
		return ret;

	dobj = drm_gem_object_lookup(file, handle);
	if (!dobj)
		return -EINVAL;

	nobj = to_nvdla_obj(dobj);

	*addr = nobj->dma_addr;

	drm_gem_object_put(dobj);

	return 0;
}

static int32_t nvdla_gem_map_offset(struct drm_device *drm, void *data,
				struct drm_file *file)
{
	int32_t ret;
	struct drm_gem_object *dobj;
	struct nvdla_gem_map_offset_args *args = data;

	dobj = drm_gem_object_lookup(file, args->handle);
	if (!dobj)
		return -EINVAL;

	ret = drm_gem_create_mmap_offset(dobj);
	if (ret)
		goto out;

	args->offset = drm_vma_node_offset_addr(&dobj->vma_node);

out:
	drm_gem_object_put(dobj);

	return 0;
}

// see: https://gitlab.manjaro.org/packages/extra/linux512-extramodules/nvidia-390xx/-/issues/1
#if (LINUX_VERSION_CODE >= KERNEL_VERSION(5, 12, 0))
static int32_t nvdla_gem_destroy(struct drm_device *drm, void *data,
				struct drm_file *file)
{
	struct nvdla_gem_destroy_args *args = data;

	return drm_gem_handle_delete(file, args->handle);
}
#else
static int32_t nvdla_gem_destroy(struct drm_device *drm, void *data,
				struct drm_file *file)
{
	struct nvdla_gem_destroy_args *args = data;

	return drm_gem_dumb_destroy(file, drm, args->handle);
}
#endif

static const struct file_operations nvdla_drm_fops = {
	.owner = THIS_MODULE,
	.open = drm_open,
	.release = drm_release,
	.unlocked_ioctl = drm_ioctl,
	.mmap = nvdla_drm_gem_mmap,
	.poll = drm_poll,
	.read = drm_read,
#ifdef CONFIG_COMPAT
	.compat_ioctl = drm_compat_ioctl,
#endif
	.llseek = noop_llseek,
};

static const struct drm_ioctl_desc nvdla_drm_ioctls[] = {
	DRM_IOCTL_DEF_DRV(NVDLA_SUBMIT, nvdla_submit, DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(NVDLA_GEM_CREATE, nvdla_gem_create, DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(NVDLA_GEM_MMAP, nvdla_gem_map_offset, DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(NVDLA_GEM_DESTROY, nvdla_gem_destroy, DRM_RENDER_ALLOW),
};

static struct drm_driver nvdla_drm_driver = {
	.driver_features = DRIVER_GEM  | DRIVER_RENDER,


	.prime_handle_to_fd = drm_gem_prime_handle_to_fd,
	.prime_fd_to_handle = drm_gem_prime_fd_to_handle,
	.gem_prime_mmap		= nvdla_drm_gem_mmap_buf,

	.ioctls = nvdla_drm_ioctls,
	.num_ioctls = ARRAY_SIZE(nvdla_drm_ioctls),
	.fops = &nvdla_drm_fops,

	.name = "nvdla",
	.desc = "NVDLA driver",
	.date = "20171017",
	.major = 0,
	.minor = 0,
	.patchlevel = 0,
};

// https://lkml.org/lkml/2019/9/2/810
int32_t nvdla_drm_probe(struct nvdla_device *nvdla_dev)
{
	int32_t err;
	struct drm_device *drm;
	struct drm_driver *driver = &nvdla_drm_driver;

	drm = drm_dev_alloc(driver, &nvdla_dev->pdev->dev);
	if (IS_ERR(drm))
		return PTR_ERR(drm);

	nvdla_dev->drm = drm;

	err = drm_dev_register(drm, 0);
	return err;
}

void nvdla_drm_remove(struct nvdla_device *nvdla_dev)
{
	drm_dev_unregister(nvdla_dev->drm);
	drm_mode_config_cleanup(nvdla_dev->drm);
	drm_dev_put(nvdla_dev->drm);
}
