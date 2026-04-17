/*
 * virtio-vsock PCI bindings (pure in-process, no vhost)
 *
 * Copyright 2025 Ramon Nogueira
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"

#include "hw/virtio/virtio-pci.h"
#include "hw/core/qdev-properties.h"
#include "hw/virtio/virtio-vsock.h"
#include "qemu/module.h"
#include "qom/object.h"

typedef struct VirtIOVSockPCI VirtIOVSockPCI;

#define TYPE_VIRTIO_VSOCK_PCI "virtio-vsock-pci-base"
DECLARE_INSTANCE_CHECKER(VirtIOVSockPCI, VIRTIO_VSOCK_PCI,
                         TYPE_VIRTIO_VSOCK_PCI)

struct VirtIOVSockPCI {
    VirtIOPCIProxy parent_obj;
    VirtIOVSock vdev;
};

static const Property virtio_vsock_pci_properties[] = {
    DEFINE_PROP_UINT32("vectors", VirtIOPCIProxy, nvectors, 3),
};

static void virtio_vsock_pci_realize(VirtIOPCIProxy *vpci_dev, Error **errp)
{
    VirtIOVSockPCI *dev = VIRTIO_VSOCK_PCI(vpci_dev);
    DeviceState *vdev = DEVICE(&dev->vdev);
    VirtIODevice *virtio_dev = VIRTIO_DEVICE(vdev);

    if (!virtio_legacy_check_disabled(virtio_dev)) {
        virtio_pci_force_virtio_1(vpci_dev);
    }

    qdev_realize(vdev, BUS(&vpci_dev->bus), errp);
}

static void virtio_vsock_pci_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    VirtioPCIClass *k = VIRTIO_PCI_CLASS(klass);
    PCIDeviceClass *pcidev_k = PCI_DEVICE_CLASS(klass);

    k->realize = virtio_vsock_pci_realize;
    set_bit(DEVICE_CATEGORY_MISC, dc->categories);
    device_class_set_props(dc, virtio_vsock_pci_properties);
    pcidev_k->vendor_id = PCI_VENDOR_ID_REDHAT_QUMRANET;
    pcidev_k->device_id = PCI_DEVICE_ID_VIRTIO_VSOCK;
    pcidev_k->revision = 0x00;
    pcidev_k->class_id = PCI_CLASS_COMMUNICATION_OTHER;
}

static void virtio_vsock_pci_instance_init(Object *obj)
{
    VirtIOVSockPCI *dev = VIRTIO_VSOCK_PCI(obj);

    virtio_instance_init_common(obj, &dev->vdev, sizeof(dev->vdev),
                                TYPE_VIRTIO_VSOCK);
}

static const VirtioPCIDeviceTypeInfo virtio_vsock_pci_info = {
    .base_name             = TYPE_VIRTIO_VSOCK_PCI,
    .generic_name          = "virtio-vsock-pci",
    .non_transitional_name = "virtio-vsock-pci-non-transitional",
    .instance_size = sizeof(VirtIOVSockPCI),
    .instance_init = virtio_vsock_pci_instance_init,
    .class_init    = virtio_vsock_pci_class_init,
};

static void virtio_vsock_pci_register(void)
{
    virtio_pci_types_register(&virtio_vsock_pci_info);
}

type_init(virtio_vsock_pci_register)
