#ifndef _NVDLA_DRM_GEM_H
#define _NVDLA_DRM_GEM_H

static void nvdla_gem_free_object(struct drm_gem_object *dobj);

static struct sg_table *nvdla_drm_gem_prime_get_sg_table(struct drm_gem_object *dobj);

static int nvdla_drm_gem_prime_vmap(struct drm_gem_object *obj, struct dma_buf_map *map);

static void nvdla_drm_gem_prime_vunmap(struct drm_gem_object *obj, struct dma_buf_map *map);

#endif