#include "quest-camera-isp.h"
#include <string.h>

static void put32(uint8_t *p, uint32_t value)
{ for(unsigned i=0;i<4;i++)p[i]=(uint8_t)(value>>(8*i)); }
static void put64(uint8_t *p, uint64_t value)
{ for(unsigned i=0;i<8;i++)p[i]=(uint8_t)(value>>(8*i)); }
static bool lanes_valid(uint32_t cphy, uint32_t combo, uint32_t lanes, uint32_t map)
{
    if(cphy>1 || combo>1 || !lanes || lanes>(combo?(cphy?1u:2u):(cphy?3u:4u)) ||
       map>UINT16_MAX || (map>>(4*lanes)))return false;
    uint32_t used=0;
    for(unsigned i=0;i<lanes;i++) {
        unsigned lane=(map>>(4*i))&15;
        if(lane>=(cphy || combo?3u:4u) || (used&(1u<<lane)))return false;
        used|=1u<<lane;
    }
    return true;
}
bool quest_camera_isp_encode(const struct quest_camera_isp_config *c, void *output, size_t size)
{
    if(!c || !output || size<QUEST_CAMERA_ISP_ACQUIRE_SIZE || c->phy>6 || c->rdi>3 ||
       !lanes_valid(c->cphy,c->combo,c->lanes,c->lane_map) || c->virtual_channel>3 ||
       (c->bits!=8 && c->bits!=10) || !c->width || c->width>4096 || !c->height || c->height>4096)return false;
    uint32_t input=c->bits==8?2:3,format;
    switch(c->output) {
    case QUEST_CAMERA_RAW_MIPI:format=input;break;
    case QUEST_CAMERA_RAW_PLAIN8:if(c->bits!=8)return false;format=12;break;
    case QUEST_CAMERA_RAW_PLAIN16:format=c->bits==8?13:14;break;
    default:return false;
    }
    uint8_t bytes[QUEST_CAMERA_ISP_ACQUIRE_SIZE]={0};
    put32(bytes,0x2000);put32(bytes+8,1);put32(bytes+12,0x200);put32(bytes+16,192);
    uint8_t *in=bytes+24;
    put32(in,c->phy==6?0x400b:0x4001+c->phy);put32(in+4,c->cphy);
    put32(in+8,c->lanes);put32(in+12,c->lane_map);
    put32(in+16,c->virtual_channel);put32(in+32,c->bits==8?0x2a:0x2b);put32(in+48,1);put32(in+52,input);
    put32(in+68,c->width-1);put32(in+72,c->width);put32(in+92,c->height-1);put32(in+96,c->height);
    put32(in+120,1);
    uint8_t *out=in+152;
    put32(out,0x3006+c->rdi);put32(out+4,format);put32(out+8,c->width);put32(out+12,c->height);
    memcpy(output,bytes,sizeof(bytes));return true;
}
bool quest_camera_phy_encode(const struct quest_camera_phy_config *c, void *output, size_t size)
{
    if(!c || !output || size<QUEST_CAMERA_PHY_CONFIG_SIZE ||
       !lanes_valid(c->cphy,c->combo,c->lanes,c->lane_map) || c->settle_count>UINT16_MAX ||
       !c->data_rate || (c->mipi_flags&~6u))return false;
    uint8_t bytes[QUEST_CAMERA_PHY_CONFIG_SIZE]={0};
    bytes[2]=(uint8_t)c->lane_map;bytes[3]=(uint8_t)(c->lane_map>>8);
    bytes[4]=(uint8_t)c->mipi_flags;bytes[6]=(uint8_t)c->lanes;
    put64(bytes+8,(uint64_t)c->settle_count*200000000);put64(bytes+16,c->data_rate);
    memcpy(output,bytes,sizeof(bytes));return true;
}
