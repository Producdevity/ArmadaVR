#include "pico-hmd-config.h"
#include "pico-hmd-prober.h"
#include <xrt/xrt_prober.h>
#include <cjson/cJSON.h>
#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static unsigned creates, calibrations;
static bool succeed = true;
bool ssc_clock_calibrate_monotonic(struct ssc_clock *clock, unsigned attempts, int64_t limit)
{
    (void)clock;assert(attempts==16 && limit==1000000);calibrations++;return true;
}
bool pico_monado_hmd_create(const struct pico_hmd_config *c,struct xrt_device **d)
{
    assert(!*d && !strcmp(c->serial,"offline-test") && c->screen_width==1920 && c->screen_height==3664);
    assert(c->views[1].display.viewport.y_pixels==1832 && c->views[0].distortion[3].b.y==1);
    assert(c->sensors.suid_high[0]==UINT64_MAX && c->sensors.suid_low[0]==1);
    assert(c->sensors.bias.suid_high[1]==3 && c->sensors.bias.suid_low[1]==4);
    assert(c->sensors.sample_rate[0]==1000 && c->sensors.bias.rigid_body[0]==0);
    assert(c->tracking.minimum_accuracy==2 && c->tracking.acceleration_scale==1);
    struct ssc_clock clock; assert(c->sensors.calibrate(c->sensors.data,&clock));
    creates++;if(!succeed)return false;
    *d=calloc(1,sizeof(**d));assert(*d);return true;
}
static cJSON *fixture(void)
{
    return cJSON_Parse("{\"schema_version\":1,\"device\":\"PICOA7H10\",\"serial\":\"offline-test\","
      "\"screen\":{\"width\":1920,\"height\":3664,\"frame_interval_ns\":11111111},"
      "\"views\":["
      "{\"viewport\":[0,0,1920,1832],\"display\":[1832,1920],\"rotation\":[0,1,-1,0],"
      "\"fov\":[-0.8,0.8,0.8,-0.8],\"eye_pose\":[0,0,0,1,-0.032,0,0],"
      "\"grid_width\":2,\"grid_height\":2,\"distortion\":[[0,0,0,0,0,0],[1,0,1,0,1,0],[0,1,0,1,0,1],[1,1,1,1,1,1]]},"
      "{\"viewport\":[0,1832,1920,1832],\"display\":[1832,1920],\"rotation\":[0,1,-1,0],"
      "\"fov\":[-0.8,0.8,0.8,-0.8],\"eye_pose\":[0,0,0,1,0.032,0,0],"
      "\"grid_width\":2,\"grid_height\":2,\"distortion\":[[0,0,0,0,0,0],[1,0,1,0,1,0],[0,1,0,1,0,1],[1,1,1,1,1,1]]}],"
      "\"imu\":{\"suids\":[[\"ffffffffffffffff\",\"0000000000000001\"],[\"0000000000000001\",\"0000000000000002\"],"
      "[\"0000000000000002\",\"0000000000000003\"],[\"0000000000000003\",\"0000000000000004\"]],"
      "\"sample_rates\":[1000,1000],\"sensor_to_head\":[1,0,0,0,1,0,0,0,1],\"acceleration_scale\":1,\"angular_velocity_scale\":1,"
      "\"max_age_ns\":100000000,\"max_gap_ns\":10000000,\"minimum_accuracy\":2,\"startup_timeout_ns\":1000000000,"
      "\"report_timeout_ns\":500000000,\"bias_max_age_ns\":1000000000}}");
}
static void save(const char *path,cJSON *json)
{
    char *data=cJSON_PrintUnformatted(json);assert(data);
    FILE *f=fopen(path,"wb");assert(f);assert(fwrite(data,1,strlen(data),f)==strlen(data));assert(!fclose(f));free(data);
}
int main(void)
{
    char path[]="/tmp/reframed-hmd-config-XXXXXX";int fd=mkstemp(path);assert(fd>=0);close(fd);
    cJSON *json=fixture();assert(json);save(path,json);
    struct pico_hmd_config *config=NULL;
    assert(pico_hmd_config_load(path,&config));
    struct xrt_device *d=NULL;assert(pico_monado_hmd_create(config,&d));free(d);
    pico_hmd_config_destroy(&config);assert(!config);pico_hmd_config_destroy(&config);
    const char *top[]={"schema_version","device","serial","screen","views","imu"};
    unsigned refusals=0;
    for(unsigned i=0;i<6;i++) {
        cJSON *bad=cJSON_Duplicate(json,true);cJSON_DeleteItemFromObjectCaseSensitive(bad,top[i]);save(path,bad);
        assert(!pico_hmd_config_load(path,&config) && !config);cJSON_Delete(bad);refusals++;
        bad=cJSON_Duplicate(json,true);cJSON_AddItemToObject(bad,top[i],cJSON_Duplicate(cJSON_GetObjectItemCaseSensitive(json,top[i]),true));
        save(path,bad);assert(!pico_hmd_config_load(path,&config));cJSON_Delete(bad);refusals++;
    }
    const char *fields[]={"suids","sample_rates","sensor_to_head","acceleration_scale","angular_velocity_scale",
      "max_age_ns","max_gap_ns","minimum_accuracy","startup_timeout_ns","report_timeout_ns","bias_max_age_ns"};
    for(unsigned i=0;i<11;i++) {
        cJSON *bad=cJSON_Duplicate(json,true);cJSON *imu=cJSON_GetObjectItemCaseSensitive(bad,"imu");
        cJSON_ReplaceItemInObjectCaseSensitive(imu,fields[i],cJSON_CreateNull());save(path,bad);
        assert(!pico_hmd_config_load(path,&config));cJSON_Delete(bad);refusals++;
    }
    const char *eyes[]={"viewport","display","rotation","fov","eye_pose","grid_width","grid_height","distortion"};
    for(unsigned eye=0;eye<2;eye++)for(unsigned i=0;i<8;i++) {
        cJSON *bad=cJSON_Duplicate(json,true);cJSON *v=cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(bad,"views"),(int)eye);
        cJSON_DeleteItemFromObjectCaseSensitive(v,eyes[i]);save(path,bad);
        assert(!pico_hmd_config_load(path,&config));cJSON_Delete(bad);refusals++;
    }
    FILE *f=fopen(path,"wb");assert(f);assert(fwrite("{}\0trailing",1,11,f)==11);fclose(f);
    assert(!pico_hmd_config_load(path,&config));
    fd=open(path,O_WRONLY|O_TRUNC);assert(fd>=0);assert(!ftruncate(fd,2*1024*1024+1));close(fd);
    assert(!pico_hmd_config_load(path,&config));
    save(path,json);cJSON_Delete(json);
    char link[128];snprintf(link,sizeof(link),"%s-link",path);assert(!symlink(path,link));
    assert(!pico_hmd_config_load(link,&config));unlink(link);
    unsetenv("PICO_HMD_CONFIG");unsetenv("PICO_HMD_COUNTER");
    struct xrt_auto_prober *p=pico_hmd_create_auto_prober();struct xrt_device *devices[2]={0};
    assert(!p->lelo_dallas_autoprobe(p,NULL,false,NULL,devices));
    setenv("PICO_HMD_CONFIG",path,1);assert(!p->lelo_dallas_autoprobe(p,NULL,false,NULL,devices));
    setenv("PICO_HMD_COUNTER","1",1);assert(!p->lelo_dallas_autoprobe(p,NULL,true,NULL,devices));
    assert(p->lelo_dallas_autoprobe(p,NULL,false,NULL,devices)==1 && devices[0]);
    assert(!p->lelo_dallas_autoprobe(p,NULL,false,NULL,devices));p->destroy(p);free(devices[0]);devices[0]=NULL;
    succeed=false;p=pico_hmd_create_auto_prober();assert(!p->lelo_dallas_autoprobe(p,NULL,false,NULL,devices));
    assert(!p->lelo_dallas_autoprobe(p,NULL,false,NULL,devices));p->destroy(p);
    assert(creates==3 && calibrations==3);unlink(path);unsetenv("PICO_HMD_CONFIG");unsetenv("PICO_HMD_COUNTER");
    printf("PICO_HMD_CONFIG_PASS refusals=%u creates=%u\n",refusals,creates);return 0;
}
