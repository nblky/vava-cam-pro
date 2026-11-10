#include "storage_io.h"
#include "recorder.h"
#include "upload.h"
#include "nas.h"
#include <errno.h>
#include <inttypes.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
enum { JOBS=4,OUTPUT=16384 };
struct io_job {uint64_t id,generation;bool running,done,abandoned;unsigned slot;char serial[32],command[512],result[OUTPUT];};
struct storage_io {pthread_mutex_t lock;pthread_cond_t wake;pthread_t worker;bool stop;uint64_t sequence;struct uploader *upload;struct nas *nas;struct io_job jobs[JOBS];};
bool storage_io_command(const char *name,bool station){
    const char *global[]={"storage-cleanup","upload-rate","storage-policy","storage-policy-set","record-cameras","record-query-all"};
    const char *camera[]={"record-info","record-list","record-query","record-delete","record-export","record-preview","export-list","export-cancel","export-retry","export-forget","upload-list","nas-list","upload-task","nas-task"};
    const char **names=station?global:camera;size_t count=station?sizeof(global)/sizeof(*global):sizeof(camera)/sizeof(*camera);
    for(size_t i=0;i<count;++i)if(!strcmp(name,names[i]))return true;return false;
}
static bool number(const char *text,unsigned limit,unsigned *out){if(!text||!*text||*text<'0'||*text>'9')return false;char *end;errno=0;unsigned long n=strtoul(text,&end,10);if(errno||*end||n>limit)return false;*out=(unsigned)n;return true;}
static bool identifier(const char *text,uint64_t *id){uint8_t b[8];if(!myvava_unhex(text,b,8))return false;*id=0;for(unsigned i=0;i<8;++i)*id=*id*256+b[i];return true;}
static bool wide_number(const char *text,uint64_t *out){
    if(!text||!*text||*text<'0'||*text>'9')return false;char *end;errno=0;unsigned long long n=strtoull(text,&end,10);if(errno||*end)return false;*out=(uint64_t)n;return true;
}
static void execute(struct storage_io *io,struct io_job *j){
    char *words[16],*save=NULL;unsigned count=0;for(char *p=strtok_r(j->command," ",&save);p&&count<16;p=strtok_r(NULL," ",&save))words[count++]=p;
    const char *error="invalid_storage_arguments",*cmd=count?words[0]:"";bool ok=false;unsigned page=0;uint64_t id=0,alias=0;
    recorder_expect_generation(j->generation);
    if(strcmp(cmd,"storage-policy")&&strcmp(cmd,"storage-policy-set")&&(!j->generation||j->generation!=recorder_storage_generation())){error="storage_changed_or_unavailable";goto done;}
    if(!strcmp(cmd,"record-cameras")&&(count==1||count==2)&&(count==1||number(words[1],63,&page))){ok=recorder_cameras(page,j->result,OUTPUT);error="storage_catalog_unavailable";}
    else if(!strcmp(cmd,"storage-policy")&&count==1){ok=recorder_retention_json(j->result,OUTPUT);}
    else if(!strcmp(cmd,"storage-policy-set")&&count==5){unsigned revision,enabled,days,pending;
        if(number(words[1],UINT32_MAX,&revision)&&number(words[2],1,&enabled)&&number(words[3],3650,&days)&&number(words[4],720,&pending)){
            const char *e=recorder_retention_config(revision,enabled,days,pending);if(e)error=e;else ok=recorder_retention_json(j->result,OUTPUT);}
    }
    else if(!strcmp(cmd,"storage-cleanup")&&count==1){uint64_t removed=0;ok=recorder_cleanup(&removed);if(ok)snprintf(j->result,OUTPUT,"{\"ok\":true,\"removed_bytes\":%" PRIu64 "}\n",removed);error="cleanup_requires_idle_storage_or_io_failed";}
    else if(!strcmp(cmd,"upload-rate")&&(count==1||count==2)){unsigned rate=0;ok=io->upload&&(count==1||(number(words[1],4096,&rate)&&upload_rate(io->upload,rate)));if(ok)snprintf(j->result,OUTPUT,"{\"ok\":true,\"rate_kib\":%u,\"scope\":\"cloud_upload\"}\n",upload_get_rate(io->upload));error="upload_rate_unavailable_or_invalid";}
    else if(!strcmp(cmd,"record-query-all")&&count==5){uint64_t before,generation,from,to;
        if(identifier(words[1],&before)&&wide_number(words[2],&generation)&&wide_number(words[3],&from)&&wide_number(words[4],&to)){
            const char *e=recorder_query_all(before,generation,from,to,j->result,OUTPUT);ok=e==NULL;if(e)error=e;}
    }
    else if(!strcmp(cmd,"record-query")&&count==6){uint64_t before,generation,from,to;
        if(identifier(words[2],&before)&&wide_number(words[3],&generation)&&wide_number(words[4],&from)&&wide_number(words[5],&to)){
            const char *e=recorder_query(j->slot,j->serial,before,generation,from,to,j->result,OUTPUT);ok=e==NULL;if(e)error=e;}
    }
    else if((!strcmp(cmd,"record-list")||!strcmp(cmd,"upload-list")||!strcmp(cmd,"nas-list"))&&(count==2||count==3)&&(count==2||number(words[2],4095,&page))){
        ok=!strcmp(cmd,"record-list")?recorder_list(j->slot,j->serial,page,j->result,OUTPUT):!strcmp(cmd,"upload-list")?upload_json(io->upload,j->slot,j->serial,page,j->result,OUTPUT):nas_json(io->nas,j->slot,j->serial,page,j->result,OUTPUT);error="storage_catalog_unavailable";
    }else if(!strcmp(cmd,"export-list")&&count==2){ok=upload_exports_json(io->upload,j->slot,j->serial,j->result,OUTPUT);error="export_status_unavailable";}
    else if(count>=3&&identifier(words[2],&id)){
        if(!strcmp(cmd,"record-info")&&count==3){ok=recorder_info_json(j->slot,j->serial,id,j->result,OUTPUT);error="storage_catalog_unavailable";}
        else if(!strcmp(cmd,"record-delete")&&count==3){const char *e=recorder_delete_result(j->slot,j->serial,id);ok=e==NULL;if(e)error=e;}
        else if((!strcmp(cmd,"record-export")||!strcmp(cmd,"record-preview"))&&count==4&&identifier(words[3],&alias)){ok=!strcmp(cmd,"record-preview")?upload_preview(io->upload,j->slot,j->serial,id,alias):upload_export(io->upload,j->slot,j->serial,id,alias);error="export_unavailable_or_conflict";if(ok)snprintf(j->result,OUTPUT,"{\"ok\":true,\"id\":\"%016" PRIx64 "\"}\n",alias);}
        else if((!strcmp(cmd,"export-cancel")||!strcmp(cmd,"export-retry"))&&count==3){ok=upload_export_action(io->upload,j->slot,j->serial,id,!strcmp(cmd,"export-cancel"));error="export_unavailable_or_conflict";}
        else if(!strcmp(cmd,"export-forget")&&count==3){ok=upload_export_forget(io->upload,j->slot,j->serial,id);error="export_unavailable_or_conflict";}
        else if((!strcmp(cmd,"upload-task")||!strcmp(cmd,"nas-task"))&&count==4){ok=!strcmp(cmd,"upload-task")?upload_action(io->upload,j->slot,j->serial,id,words[3]):nas_action(io->nas,j->slot,j->serial,id,words[3]);error="task_unavailable_or_action_refused";}
    }
done:recorder_expect_generation(0);if(!ok)snprintf(j->result,OUTPUT,"{\"ok\":false,\"error\":\"%s\"}\n",error);else if(!j->result[0])strcpy(j->result,"{\"ok\":true}\n");
}
static void *worker(void *arg){struct storage_io *io=arg;pthread_mutex_lock(&io->lock);
    while(!io->stop){struct io_job *job=NULL;for(unsigned i=0;i<JOBS;++i)if(io->jobs[i].id&&!io->jobs[i].running&&!io->jobs[i].done&&(!job||io->jobs[i].id<job->id))job=&io->jobs[i];
        if(!job){pthread_cond_wait(&io->wake,&io->lock);continue;}job->running=true;pthread_mutex_unlock(&io->lock);execute(io,job);pthread_mutex_lock(&io->lock);
        job->running=false;job->done=true;if(job->abandoned)memset(job,0,sizeof(*job));
    }pthread_mutex_unlock(&io->lock);return NULL;
}
struct storage_io *storage_io_start(struct uploader *u,struct nas *n){struct storage_io *io=calloc(1,sizeof(*io));if(!io)return NULL;io->upload=u;io->nas=n;
    if(pthread_mutex_init(&io->lock,NULL)){free(io);return NULL;}if(pthread_cond_init(&io->wake,NULL)){pthread_mutex_destroy(&io->lock);free(io);return NULL;}
    if(pthread_create(&io->worker,NULL,worker,io)){pthread_cond_destroy(&io->wake);pthread_mutex_destroy(&io->lock);free(io);return NULL;}return io;
}
void storage_io_stop(struct storage_io *io){if(!io)return;pthread_mutex_lock(&io->lock);io->stop=true;pthread_cond_signal(&io->wake);pthread_mutex_unlock(&io->lock);pthread_join(io->worker,NULL);pthread_cond_destroy(&io->wake);pthread_mutex_destroy(&io->lock);free(io);}
uint64_t storage_io_submit(struct storage_io *io,unsigned slot,const char *serial,unsigned count,char **words){
    if(!io||!count||count>16||strlen(serial)>=32)return 0;uint64_t id=0;pthread_mutex_lock(&io->lock);unsigned i=0;while(i<JOBS&&io->jobs[i].id)++i;
    if(i<JOBS&&!io->stop&&io->sequence<UINT64_MAX){struct io_job *j=&io->jobs[i];size_t used=0;bool ok=true;
        for(unsigned k=0;k<count;++k){int n=snprintf(j->command+used,sizeof(j->command)-used,"%s%s",k?" ":"",words[k]);if(n<0||(size_t)n>=sizeof(j->command)-used){ok=false;break;}used+=(size_t)n;}
        if(ok){j->slot=slot;strcpy(j->serial,serial);j->generation=recorder_storage_generation();j->id=id=++io->sequence;pthread_cond_signal(&io->wake);}else memset(j,0,sizeof(*j));
    }pthread_mutex_unlock(&io->lock);return id;
}
int storage_io_poll(struct storage_io *io,uint64_t id,char *out,size_t capacity,size_t *length){if(!io||!id)return -1;pthread_mutex_lock(&io->lock);int result=-1;
    for(unsigned i=0;i<JOBS;++i){struct io_job *j=&io->jobs[i];if(j->id!=id)continue;result=0;if(j->done){size_t size=strlen(j->result);result=size<capacity?1:-1;if(result==1){memcpy(out,j->result,size+1);*length=size;}memset(j,0,sizeof(*j));}break;}
    pthread_mutex_unlock(&io->lock);return result;
}
void storage_io_abandon(struct storage_io *io,uint64_t id){if(!io||!id)return;pthread_mutex_lock(&io->lock);for(unsigned i=0;i<JOBS;++i)if(io->jobs[i].id==id){if(io->jobs[i].running)io->jobs[i].abandoned=true;else memset(&io->jobs[i],0,sizeof(io->jobs[i]));break;}pthread_mutex_unlock(&io->lock);}
