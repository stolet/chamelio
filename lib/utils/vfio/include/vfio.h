#ifndef GUEST_VFIO_H_
#define GUEST_VFIO_H_

#define VFIO_GROUP "/dev/vfio/noiommu-0"
#define VFIO_PCI_DEV "0000:00:03.0"

#define VFIO_API_VERSION 0

struct vfio {
    /* VFIO device fd */
    int dev;
    /* VFIO group */
    int group;
    /* VFIO container */
    int cont;

    /* Base of shared memory region for protocol */
    void *shm_base;
    /* Shared memory region size */
    size_t shm_size;
    /* Shared memory region offset */
    size_t shm_off;
};

int vfio_init(struct vfio *vfio);

#endif /* ndef GUEST_VFIO_H_ */
