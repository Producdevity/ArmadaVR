#include "quest-camera-packet.h"
#include "quest-camera-blob.h"
#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#ifdef QUEST_CAMERA_VENDOR_TEST
#include <media/cam_defs.h>
#include <media/cam_isp.h>
#include <media/cam_isp_ife.h>
#include <media/cam_sensor.h>
#endif

static unsigned cases;
static uint32_t word(const void *p) { uint32_t v; memcpy(&v,p,4); return v; }
#ifdef QUEST_CAMERA_KERNEL_TEST
static uint32_t storage[8][2048];
static size_t allocation[8];
static int memory_error;
static unsigned gets, releases;
static int cam_mem_get_cpu_buf(int handle, uintptr_t *address, size_t *length)
{
    gets++;
    if (memory_error) return memory_error;
    assert(handle > 0 && handle <= 8);
    *address=(uintptr_t)storage[handle-1];*length=allocation[handle-1];return 0;
}
static void cam_mem_put_cpu_buf(int handle) { assert(handle > 0 && handle <= 8);releases++; }
#define CAM_ERR(...) ((void)0)
#define CAM_DBG(...) ((void)0)
#include "quest-camera-kernel.inc"
#endif


#ifdef QUEST_CAMERA_KERNEL_TEST
static const unsigned blob_sizes[4]={56,8,32,64},blob_types[4]={1,4,0,9};
static unsigned blob_calls;
static int blob_error;
static uint8_t expected_blobs[176];
static int verify_blob(void *user_data, uint32_t type, uint32_t size, uint8_t *data)
{
    assert(user_data==expected_blobs && blob_calls<4);
    unsigned offset=0;for(unsigned i=0;i<blob_calls;i++)offset+=4+blob_sizes[i];
    assert(type==blob_types[blob_calls] && size==blob_sizes[blob_calls]);
    assert(!memcmp(data,expected_blobs+offset+4,size));blob_calls++;return blob_error;
}
#endif
static void blob_check(void)
{
    const uint64_t rates[]={1,400000000,UINT64_C(4294967296),UINT64_MAX};
    for(unsigned rdi=0;rdi<4;rdi++)for(unsigned rate=0;rate<4;rate++) {
        uint8_t bytes[178];memset(bytes,0xa5,sizeof(bytes));uint8_t *data=bytes+1;
        struct quest_camera_clock_votes clk={.rdi=rdi,.pixel_hz=rates[rate],.rdi_hz=rates[3-rate]};
        struct quest_camera_bandwidth_votes bw={.rdi=rdi,.camnoc_bps=rates[rate],.mnoc_average_bps=1,
            .mnoc_peak_bps=rates[rate],.ddr_average_bps=rate?1:0,.ddr_peak_bps=rate?rates[rate]:0};
        assert(quest_camera_clock_blob(&clk,data,60));assert(quest_camera_csid_clock_blob(rates[rate],data+60,12));
        assert(quest_camera_hfr_blob(rdi,data+72,36));assert(quest_camera_bandwidth_blob(&bw,data+108,68));
        assert(bytes[0]==0xa5 && bytes[177]==0xa5);
#ifdef QUEST_CAMERA_VENDOR_TEST
        struct cam_isp_clock_config clock={.num_rdi=4,.left_pix_hz=clk.pixel_hz};
        uint8_t expected_clock[56]={0};memcpy(expected_clock,&clock,offsetof(struct cam_isp_clock_config,rdi_hz));
        memcpy(expected_clock+offsetof(struct cam_isp_clock_config,rdi_hz)+8*rdi,&clk.rdi_hz,8);
        assert(word(data)==(56u<<8|CAM_ISP_GENERIC_BLOB_TYPE_CLOCK_CONFIG) && !memcmp(data+4,expected_clock,56));
        struct cam_isp_csid_clock_config csid={.csid_clock=rates[rate]};
        assert(sizeof(csid)==8 && word(data+60)==(8u<<8|CAM_ISP_GENERIC_BLOB_TYPE_CSID_CLOCK_CONFIG));
        assert(!memcmp(data+64,&csid,sizeof(csid)));
        struct cam_isp_resource_hfr_config hfr={.num_ports=1,.port_hfr_config={{.resource_type=CAM_ISP_IFE_OUT_RES_RDI_0+rdi,
            .subsample_pattern=1,.framedrop_pattern=1}}};
        assert(sizeof(hfr)==32 && word(data+72)==(32u<<8|CAM_ISP_GENERIC_BLOB_TYPE_HFR_CONFIG));
        assert(!memcmp(data+76,&hfr,sizeof(hfr)));
        struct cam_isp_bw_config_v2 bandwidth={.num_paths=1,.axi_path={{.usage_data=CAM_ISP_USAGE_RDI,
            .transac_type=CAM_AXI_TRANSACTION_WRITE,.path_data_type=CAM_AXI_PATH_DATA_IFE_RDI0+rdi,
            .camnoc_bw=bw.camnoc_bps,.mnoc_ab_bw=bw.mnoc_average_bps,.mnoc_ib_bw=bw.mnoc_peak_bps,
            .ddr_ab_bw=bw.ddr_average_bps,.ddr_ib_bw=bw.ddr_peak_bps}}};
        assert(sizeof(bandwidth)==64 && word(data+108)==(64u<<8|CAM_ISP_GENERIC_BLOB_TYPE_BW_CONFIG_V2));
        assert(!memcmp(data+112,&bandwidth,sizeof(bandwidth)));
#endif
#ifdef QUEST_CAMERA_KERNEL_TEST
        memcpy(expected_blobs,data,176);memcpy((uint8_t *)storage[0]+16,data,176);allocation[0]=192;
        struct cam_cmd_buf_desc command={.mem_handle=1,.offset=16,.size=176,.length=176,
            .type=CAM_CMD_BUF_GENERIC,.meta_data=CAM_ISP_PACKET_META_GENERIC_BLOB_COMMON};
        blob_calls=0;unsigned before=releases;
        assert(!cam_packet_util_process_generic_cmd_buffer(&command,verify_blob,expected_blobs));
        assert(blob_calls==4 && releases==before+1);
        for(unsigned length=1;length<176;length++) {
            if(length==60 || length==72 || length==108)continue;
            command.length=length;blob_calls=0;before=releases;
            assert(cam_packet_util_process_generic_cmd_buffer(&command,verify_blob,expected_blobs)==-EINVAL);
            assert(releases==before+1);cases++;
        }
        command.length=176;blob_error=-ERANGE;blob_calls=0;before=releases;
        assert(cam_packet_util_process_generic_cmd_buffer(&command,verify_blob,expected_blobs)==-ERANGE);
        assert(blob_calls==1 && releases==before+1);blob_error=0;
        allocation[0]=191;blob_calls=0;
        assert(cam_packet_util_process_generic_cmd_buffer(&command,verify_blob,expected_blobs)==-EINVAL && !blob_calls);
#endif
        for(unsigned size=0;size<68;size++) {
            uint8_t bad[80];memset(bad,0xa5,sizeof(bad));
            if(size<60)assert(!quest_camera_clock_blob(&clk,bad,size));
            if(size<12)assert(!quest_camera_csid_clock_blob(1,bad,size));
            if(size<36)assert(!quest_camera_hfr_blob(rdi,bad,size));
            assert(!quest_camera_bandwidth_blob(&bw,bad,size));
            for(unsigned i=0;i<sizeof(bad);i++)assert(bad[i]==0xa5);
            cases++;
        }
        memset(bytes,0xa5,sizeof(bytes));clk.rdi=4;assert(!quest_camera_clock_blob(&clk,bytes,sizeof(bytes)));
        clk.rdi=rdi;clk.pixel_hz=0;assert(!quest_camera_clock_blob(&clk,bytes,sizeof(bytes)));
        clk.pixel_hz=1;clk.rdi_hz=0;assert(!quest_camera_clock_blob(&clk,bytes,sizeof(bytes)));
        bw.rdi=4;assert(!quest_camera_bandwidth_blob(&bw,bytes,sizeof(bytes)));bw.rdi=rdi;
        bw.camnoc_bps=0;assert(!quest_camera_bandwidth_blob(&bw,bytes,sizeof(bytes)));bw.camnoc_bps=1;
        bw.mnoc_average_bps=0;assert(!quest_camera_bandwidth_blob(&bw,bytes,sizeof(bytes)));bw.mnoc_average_bps=1;
        bw.mnoc_peak_bps=0;assert(!quest_camera_bandwidth_blob(&bw,bytes,sizeof(bytes)));bw.mnoc_peak_bps=1;
        bw.ddr_average_bps=2;bw.ddr_peak_bps=1;assert(!quest_camera_bandwidth_blob(&bw,bytes,sizeof(bytes)));
        bw.ddr_average_bps=0;assert(!quest_camera_bandwidth_blob(&bw,bytes,sizeof(bytes)));
        assert(!quest_camera_hfr_blob(4,bytes,sizeof(bytes)) && !quest_camera_csid_clock_blob(0,bytes,sizeof(bytes)));
        assert(!quest_camera_clock_blob(NULL,bytes,sizeof(bytes)) && !quest_camera_bandwidth_blob(NULL,bytes,sizeof(bytes)));
        for(unsigned i=0;i<sizeof(bytes);i++)assert(bytes[i]==0xa5);
        cases++;
    }
}

static void image_check(const struct quest_camera_rdi_buffer *b)
{
    uint8_t bytes[QUEST_CAMERA_RDI_IO_SIZE+2];memset(bytes,0xa5,sizeof(bytes));
    assert(quest_camera_rdi_buffer_configure(b,bytes+1,sizeof(bytes)-2));
    assert(bytes[0]==0xa5 && bytes[sizeof(bytes)-1]==0xa5);
#ifdef QUEST_CAMERA_VENDOR_TEST
    struct cam_buf_io_cfg expected={.mem_handle={b->memory_handle},
        .planes={{.width=b->width,.height=b->height,.plane_stride=b->stride,.slice_height=b->slice_height}},
        .format=b->format==QUEST_CAMERA_RAW_PLAIN16?CAM_FORMAT_PLAIN16_10:
            b->bits==8?CAM_FORMAT_MIPI_RAW_8:CAM_FORMAT_MIPI_RAW_10,
        .color_pattern=5,.bpp=b->bits,.resource_type=CAM_ISP_IFE_OUT_RES_RDI_0+b->rdi,
        .fence=b->completion_fence,.direction=2,.subsample_pattern=1,.framedrop_pattern=1};
    assert(sizeof(expected)==QUEST_CAMERA_RDI_IO_SIZE && !memcmp(bytes+1,&expected,sizeof(expected)));
#endif
    cases++;
}
static void image_bad(struct quest_camera_rdi_buffer b)
{
    uint8_t bytes[QUEST_CAMERA_RDI_IO_SIZE];memset(bytes,0xa5,sizeof(bytes));
    assert(!quest_camera_rdi_buffer_configure(&b,bytes,sizeof(bytes)));
    for(unsigned i=0;i<sizeof(bytes);i++)assert(bytes[i]==0xa5);
    cases++;
}
static void packet_bad(struct quest_camera_packet_config c)
{
    uint8_t bytes[520];memset(bytes,0xa5,sizeof(bytes));
    assert(!quest_camera_packet_size(&c));
    assert(!quest_camera_packet_encode(&c,bytes,sizeof(bytes)));
    for(unsigned i=0;i<sizeof(bytes);i++)assert(bytes[i]==0xa5);
    cases++;
}
static void packet_check(const struct quest_camera_packet_config *c)
{
    uint8_t bytes[520];memset(bytes,0xa5,sizeof(bytes));
    size_t n=quest_camera_packet_size(c);assert(n>=72 && n<=504);
    assert(quest_camera_packet_encode(c,bytes+1,n)==n);
    assert(bytes[0]==0xa5 && bytes[n+1]==0xa5);
    assert(word(bytes+1)==(uint32_t)c->opcode && word(bytes+5)==n);
#ifdef QUEST_CAMERA_VENDOR_TEST
    union { uint64_t align; uint8_t data[512]; } aligned={0};
    memcpy(aligned.data,bytes+1,n);struct cam_packet *p=(void *)aligned.data;
    assert(offsetof(struct cam_packet,payload)==56 && sizeof(struct cam_cmd_buf_desc)==24);
    assert(p->header.request_id==c->request_id && !p->header.flags && !p->header.padding);
    assert(p->num_cmd_buf==c->command_count && !p->cmd_buf_offset);
    assert(p->num_io_configs==(c->image?1u:0u) && !p->num_patches);
    assert(p->kmd_cmd_buf_index==(uint32_t)c->kernel_command_index && p->kmd_cmd_buf_offset==c->kernel_command_offset);
    const struct cam_cmd_buf_desc *d=(const void *)p->payload;
    for(size_t i=0;i<c->command_count;i++) {
        const struct quest_camera_command_buffer *b=c->commands+i;
        struct cam_cmd_buf_desc expected={.mem_handle=b->memory_handle,.offset=b->offset,
            .size=b->capacity,.length=b->length,.type=b->type,.meta_data=b->metadata};
        assert(!memcmp(d+i,&expected,sizeof(expected)));
    }
    if(c->image) {
        uint8_t expected[QUEST_CAMERA_RDI_IO_SIZE];
        assert(quest_camera_rdi_buffer_configure(c->image,expected,sizeof(expected)));
        assert(!memcmp((uint8_t *)p->payload+p->io_configs_offset,expected,sizeof(expected)));
    }
#ifdef QUEST_CAMERA_KERNEL_TEST
    assert(!cam_packet_util_validate_packet(p,n));
    assert(cam_packet_util_validate_packet(p,n-1)==-EINVAL);
    for(size_t i=0;i<c->command_count;i++) {
        const struct quest_camera_command_buffer *b=c->commands+i;
        allocation[b->memory_handle-1]=b->allocation_size;
        assert(!cam_packet_util_validate_cmd_desc((struct cam_cmd_buf_desc *)d+i));
    }
    if(c->kernel_command_index>=0) {
        const struct quest_camera_command_buffer *b=c->commands+c->kernel_command_index;
        struct cam_kmd_buf_info info;memset(&info,0,sizeof(info));
        unsigned prior=gets;
        assert(!cam_packet_util_get_kmd_buffer(p,&info) && gets==prior+1);
        assert(info.cpu_addr==storage[b->memory_handle-1]+(b->offset+b->length)/4);
        assert(info.handle==b->memory_handle && info.offset==b->offset+b->length);
        assert(info.size==b->capacity-b->length && !info.used_bytes);
        for(unsigned i=0;i<info.size/4;i++)info.cpu_addr[i]=0x12345678;
        cam_packet_util_put_cmd_mem_addr(info.handle);
        memory_error=-EIO;assert(cam_packet_util_get_kmd_buffer(p,&info)==-EIO);memory_error=0;
        allocation[b->memory_handle-1]=b->offset+b->capacity-1;
        prior=releases;assert(cam_packet_util_get_kmd_buffer(p,&info)==-EINVAL && releases==prior+1);
    }
#endif
#endif
    memset(bytes,0xa5,sizeof(bytes));
    assert(!quest_camera_packet_encode(c,bytes,n-1));
    for(unsigned i=0;i<sizeof(bytes);i++)assert(bytes[i]==0xa5);
    cases++;
}
int main(void)
{
    blob_check();
    struct quest_camera_rdi_buffer b={.memory_handle=20,.completion_fence=21,.width=640,
        .height=513,.stride=800,.slice_height=513,.bits=10,.format=QUEST_CAMERA_RAW_MIPI,.allocation_size=800*513};
    for(b.rdi=0;b.rdi<4;b.rdi++)for(unsigned format=0;format<3;format++) {
        b.bits=format==0?8:10;b.format=format==2?QUEST_CAMERA_RAW_PLAIN16:QUEST_CAMERA_RAW_MIPI;
        for(unsigned width=4;width<=4096;width+=4) {
            b.width=width;b.stride=((width*(format==2?16:b.bits)/8+15)/16)*16;
            b.allocation_size=b.stride*b.slice_height;image_check(&b);
        }
    }
    b=(struct quest_camera_rdi_buffer){.memory_handle=20,.completion_fence=21,.width=640,
        .height=513,.stride=800,.slice_height=513,.bits=10,.format=QUEST_CAMERA_RAW_MIPI,.allocation_size=800*513};
#define BAD_IMAGE(field,value) do {struct quest_camera_rdi_buffer bad=b;bad.field=value;image_bad(bad);} while(0)
    BAD_IMAGE(memory_handle,0);BAD_IMAGE(completion_fence,-1);BAD_IMAGE(width,0);BAD_IMAGE(width,639);
    BAD_IMAGE(width,4097);BAD_IMAGE(height,0);BAD_IMAGE(height,4097);BAD_IMAGE(stride,799);
    BAD_IMAGE(stride,65552);BAD_IMAGE(slice_height,512);BAD_IMAGE(slice_height,65537);
    BAD_IMAGE(bits,12);BAD_IMAGE(format,QUEST_CAMERA_RAW_PLAIN8);BAD_IMAGE(format,99);
    BAD_IMAGE(allocation_size,800*513-1);BAD_IMAGE(allocation_size,UINT64_MAX);BAD_IMAGE(rdi,4);
    struct quest_camera_rdi_buffer bad=b;bad.bits=8;bad.format=QUEST_CAMERA_RAW_PLAIN16;image_bad(bad);
    struct quest_camera_command_buffer cmds[8];
    struct quest_camera_packet_config c={.opcode=QUEST_CAMERA_PACKET_IFE_UPDATE,.request_id=INT64_MAX,
        .commands=cmds,.command_count=8,.kernel_command_index=0,.kernel_command_offset=64,.image=&b};
    for(size_t count=1;count<=8;count++)for(size_t kernel=0;kernel<count;kernel++)for(unsigned offset=0;offset<=64;offset+=4) {
        for(size_t i=0;i<count;i++)cmds[i]=(struct quest_camera_command_buffer){.memory_handle=(int32_t)i+1,
            .offset=offset,.capacity=512,.length=64,.type=i==kernel?5:9,.metadata=i==kernel?3:12,.allocation_size=8192};
        c.command_count=count;c.kernel_command_index=(int32_t)kernel;packet_check(&c);
        c.opcode=QUEST_CAMERA_PACKET_IFE_INITIALIZE;c.request_id=0;c.image=NULL;packet_check(&c);
        c.opcode=QUEST_CAMERA_PACKET_IFE_UPDATE;c.request_id=INT64_MAX;c.image=&b;
    }
    c.command_count=2;c.kernel_command_index=0;c.kernel_command_offset=64;
    cmds[0]=(struct quest_camera_command_buffer){.memory_handle=1,.capacity=512,.length=64,.type=5,.metadata=3,.allocation_size=8192};
    cmds[1]=(struct quest_camera_command_buffer){.memory_handle=1,.offset=512,.capacity=512,.length=64,.type=9,.metadata=12,.allocation_size=8192};
    packet_check(&c);cmds[1].offset=508;packet_bad(c);cmds[1].offset=512;
    cmds[1].allocation_size=4096;packet_bad(c);cmds[1].allocation_size=8192;
    cmds[1].memory_handle=b.memory_handle;packet_bad(c);cmds[1].memory_handle=1;
#define BAD_PACKET(field,value) do {struct quest_camera_packet_config bad=c;bad.field=value;packet_bad(bad);} while(0)
    BAD_PACKET(kernel_command_offset,60);BAD_PACKET(kernel_command_offset,68);BAD_PACKET(kernel_command_index,-1);
    BAD_PACKET(kernel_command_index,2);BAD_PACKET(command_count,9);BAD_PACKET(command_count,0);
    BAD_PACKET(commands,NULL);BAD_PACKET(request_id,0);BAD_PACKET(request_id,UINT64_MAX);BAD_PACKET(image,NULL);
    BAD_PACKET(opcode,0);BAD_PACKET(opcode,QUEST_CAMERA_PACKET_IFE_INITIALIZE);
    for(unsigned field=0;field<10;field++) {
        struct quest_camera_command_buffer before=cmds[0];
        switch(field) {
        case 0:cmds[0].memory_handle=0;break;case 1:cmds[0].offset=1;break;case 2:cmds[0].capacity=511;break;
        case 3:cmds[0].length=63;break;case 4:cmds[0].length=512;break;case 5:cmds[0].type=7;break;
        case 6:cmds[0].metadata=18;break;case 7:cmds[0].allocation_size=511;break;
        case 8:cmds[0].allocation_size=UINT64_MAX;break;case 9:cmds[0].capacity=0;break;
        }
        packet_bad(c);cmds[0]=before;
    }
    c=(struct quest_camera_packet_config){.opcode=QUEST_CAMERA_PACKET_SENSOR_NOP,.request_id=1,.kernel_command_index=-1};
    assert(quest_camera_packet_size(&c)==72);packet_check(&c);BAD_PACKET(request_id,0);BAD_PACKET(image,&b);
    for(unsigned op=0;op<=5;op++) {
        c=(struct quest_camera_packet_config){.opcode=QUEST_CAMERA_PACKET_SENSOR_STREAM_ON+op,
            .request_id=op==1?1:0,.commands=cmds,.command_count=1,.kernel_command_index=-1};
        cmds[0]=(struct quest_camera_command_buffer){.memory_handle=1,.capacity=512,.length=64,.type=7,.allocation_size=8192};
        packet_check(&c);cmds[0].type=10;
        if(op==3)packet_check(&c);else packet_bad(c);
    }
    c.opcode=QUEST_CAMERA_PACKET_PHY_CONFIGURE;cmds[0].type=5;packet_check(&c);
    cmds[0].metadata=3;packet_bad(c);cmds[0].metadata=0;
    BAD_PACKET(command_count,2);BAD_PACKET(request_id,1);BAD_PACKET(kernel_command_index,0);
    assert(!quest_camera_packet_size(NULL));assert(!quest_camera_packet_encode(&c,NULL,512));
    assert(!quest_camera_rdi_buffer_configure(NULL,&b,sizeof(b)));
    printf("QUEST_CAMERA_PACKET_PASS cases=%u hardware=0\n",cases);
}
