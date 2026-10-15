#include <linux/media.h>
#include <stddef.h>
#include <stdio.h>

int main(void)
{
    printf("{\"device_info\":%lu,\"get_topology\":%lu,\"sizes\":[%zu,%zu,%zu,%zu,%zu,%zu],"
           "\"offsets\":[%zu,%zu,%zu,%zu,%zu,%zu,%zu,%zu,%zu,%zu,%zu,%zu]}\n",
           (unsigned long)MEDIA_IOC_DEVICE_INFO, (unsigned long)MEDIA_IOC_G_TOPOLOGY,
           sizeof(struct media_device_info), sizeof(struct media_v2_topology),
           sizeof(struct media_v2_entity), sizeof(struct media_v2_interface),
           sizeof(struct media_v2_pad), sizeof(struct media_v2_link),
           offsetof(struct media_device_info, media_version),
           offsetof(struct media_v2_topology, num_entities), offsetof(struct media_v2_topology, ptr_entities),
           offsetof(struct media_v2_topology, num_interfaces), offsetof(struct media_v2_topology, ptr_interfaces),
           offsetof(struct media_v2_topology, num_pads), offsetof(struct media_v2_topology, ptr_pads),
           offsetof(struct media_v2_topology, num_links), offsetof(struct media_v2_topology, ptr_links),
           offsetof(struct media_v2_interface, devnode), offsetof(struct media_v2_pad, index),
           offsetof(struct media_v2_link, flags));
    return 0;
}
