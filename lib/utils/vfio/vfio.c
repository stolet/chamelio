#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <linux/vfio.h>
#include <linux/pci.h>

#include "log.h"
#include "vfio.h"

int vfio_map_region(int dev, int idx, void **addr, size_t *len, size_t *off);
static int vfio_get_region_info(int dev, int i, struct vfio_region_info *reg);

int vfio_init(struct vfio *vfio)
{
  struct vfio_group_status g_status = { .argsz = sizeof(g_status) };
  struct vfio_device_info device_info = { .argsz = sizeof(device_info) };

  /* Create vfio container */
  if ((vfio->cont = open("/dev/vfio/vfio", O_RDWR)) < 0)
  {
    LOG_ERROR("failed to open vfio container");
    return -1;
  }

  /* Check API version of container */
  if (ioctl(vfio->cont, VFIO_GET_API_VERSION) != VFIO_API_VERSION)
  {
    LOG_ERROR("api version doesn't match");
    goto error_cont;
  }

  if (!ioctl(vfio->cont, VFIO_CHECK_EXTENSION, VFIO_NOIOMMU_IOMMU))
  {
    LOG_ERROR("noiommu driver not supported");
    goto error_cont;
  }

  /* Open the vfio group */
  if((vfio->group = open(VFIO_GROUP, O_RDWR)) < 0)
  {
    LOG_ERROR("failed to open vfio group");
    goto error_cont;
  }

  /* Test if group is viable and available */
  ioctl(vfio->group, VFIO_GROUP_GET_STATUS, &g_status);
  if (!(g_status.flags & VFIO_GROUP_FLAGS_VIABLE))
  {
    LOG_ERROR("group is not viable or avail");
    goto error_group;
  }

  /* Add group to container */
  if (ioctl(vfio->group, VFIO_GROUP_SET_CONTAINER, &vfio->cont) < 0)
  {
    LOG_ERROR("failed to add group to container");
    goto error_group;
  }

  /* Enable desired IOMMU model */
  if (ioctl(vfio->cont, VFIO_SET_IOMMU, VFIO_NOIOMMU_IOMMU) < 0)
  {
    LOG_ERROR("failed to set IOMMU model");
    goto error_group;
  }

  /* Get file descriptor for device */
  if ((vfio->dev = ioctl(vfio->group, 
      VFIO_GROUP_GET_DEVICE_FD, VFIO_PCI_DEV)) < 0)
  {
    LOG_ERROR("failed to get fd for VFIO device");
    goto error_group;
  }

  /* Get device info */
  if (ioctl(vfio->dev, VFIO_DEVICE_GET_INFO, &device_info) < 0)
  {
    LOG_ERROR("failed to get device info");
    goto error_dev;
  }

  /* Map BAR 2 for shm between Chamelio and guest */
  if (vfio_map_region(vfio->dev, 2, &vfio->shm_base, 
      &vfio->shm_size, &vfio->shm_off) != 0)
  {
    LOG_ERROR("failed to map shm region");    
    goto error_dev;
  }

  return 0;

error_dev:
  close(vfio->dev);
error_group:
  close(vfio->group);
error_cont:
  close(vfio->cont);

  return -1;
}

int vfio_map_region(int dev, int idx, void **addr, size_t *len, size_t *off)
{
  void *ret;
  int prot, flags;
  struct vfio_region_info reg = { .argsz = sizeof(reg) };

  if (vfio_get_region_info(dev, idx, &reg) != 0) 
  {
    LOG_ERROR("failed to get region info");  
    return -1;
  }

  prot = PROT_READ | PROT_WRITE;
  flags = MAP_SHARED | MAP_POPULATE;
  ret = mmap(NULL, reg.size, prot, flags, dev, reg.offset);
  if (ret == MAP_FAILED)
  {
    LOG_ERROR("mmap failed");
    return -1;
  }

  *addr = ret;
  *len = reg.size;
  *off = reg.offset;

  return 0;
}

static int vfio_get_region_info(int dev, int i, struct vfio_region_info *reg)
{
  reg->index = i;

  if (ioctl(dev, VFIO_DEVICE_GET_REGION_INFO, reg))
  {
    LOG_ERROR("failed to get info");
    return -1;
  }

  return 0;
}
