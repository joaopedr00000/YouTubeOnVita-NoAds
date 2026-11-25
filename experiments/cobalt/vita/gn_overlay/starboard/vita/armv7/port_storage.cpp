// Starboard 15 local-user BLOB storage on Vita ux0 (not Google credentials).
// Atomic-style temp + backup swap; always recover rather than delete old backup.
#include "starboard/storage.h"
#include "starboard/user.h"
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>

struct SbUserPrivate { uint32_t magic; };
struct SbStorageRecordPrivate {
  char path[256];
  char* blob;
  int64_t bytes;
};
namespace {
SbUserPrivate local_user={0x56544944u};
SbStorageRecord current=nullptr;
constexpr int64_t max_bytes=8*1024*1024;
constexpr char dir[]="ux0:data/youtubevita-noads/storage";
bool name_path(const char* input,char* out,size_t n){
  const char* name=input?input:"default";
  size_t len=strlen(name);
  if(len==0 || len>64 || strstr(name,".."))return false;
  for(size_t i=0;i<len;i++){
    char c=name[i];
    if(!((c>='a'&&c<='z')||(c>='A'&&c<='Z')||
         (c>='0'&&c<='9')||c=='_'||c=='-'))return false;
  }
  int chars=snprintf(out,n,"%s/%s.dat",dir,name);
  return chars>0&&(size_t)chars<n;
}
bool read_file(const char* path,char** data,int64_t* bytes){
  *data=nullptr;*bytes=0;
  SceIoStat stat={};
  if(sceIoGetstat(path,&stat)<0)return true;
  if(stat.st_size<0||stat.st_size>max_bytes)return false;
  SceUID f=sceIoOpen(path,SCE_O_RDONLY,0);
  if(f<0)return false;
  int64_t len=stat.st_size;
  char* buf=len?(char*)malloc((size_t)len):nullptr;
  if(len&&!buf){sceIoClose(f);return false;}
  int64_t offset=0;
  while(offset<len){
    int count=sceIoRead(f,buf+offset,(unsigned)(len-offset>32768?32768:len-offset));
    if(count<=0){free(buf);sceIoClose(f);return false;}
    offset+=count;
  }
  sceIoClose(f);
  *data=buf;*bytes=len;
  return true;
}
bool save_file(const char* path,const char* data,int64_t bytes){
  char tmp[300],backup[300];
  if(snprintf(tmp,sizeof(tmp),"%s.new",path)>=(int)sizeof(tmp) ||
     snprintf(backup,sizeof(backup),"%s.backup",path)>=(int)sizeof(backup))
    return false;
  SceIoStat stat={};
  if(sceIoGetstat(tmp,&stat)>=0 || sceIoGetstat(backup,&stat)>=0)
    return false;
  SceUID f=sceIoOpen(tmp,SCE_O_CREAT|SCE_O_EXCL|SCE_O_WRONLY,0666);
  if(f<0)return false;
  bool okay=true;
  int64_t done=0;
  while(done<bytes){
    unsigned chunk=(unsigned)(bytes-done>32768?32768:bytes-done);
    int n=sceIoWrite(f,data+done,chunk);
    if(n<=0){okay=false;break;}
    done+=n;
  }
  if(okay)okay=sceIoSyncByFd(f,0)>=0;
  if(sceIoClose(f)<0)okay=false;
  if(!okay){sceIoRemove(tmp);return false;}
  bool prior=sceIoGetstat(path,&stat)>=0;
  bool switched=false;
  if(!prior)switched=sceIoRename(tmp,path)>=0;
  else if(sceIoRename(path,backup)>=0){
    if(sceIoRename(tmp,path)>=0){
      switched=true;
      sceIoRemove(backup);
    }else{
      sceIoRename(backup,path);
    }
  }
  if(!switched)sceIoRemove(tmp);
  return switched;
}
}
extern "C" {
SbUser SbUserGetCurrent(){return &local_user;}
SbStorageRecord SbStorageOpenRecord(SbUser user,const char* name){
  if(user!=&local_user || current)return nullptr;
  char path[256]={};
  if(!name_path(name,path,sizeof(path)))return nullptr;
  sceIoMkdir("ux0:data/youtubevita-noads",0777);
  sceIoMkdir(dir,0777);
  SbStorageRecord item=(SbStorageRecord)calloc(1,sizeof(SbStorageRecordPrivate));
  if(!item)return nullptr;
  strncpy(item->path,path,sizeof(item->path)-1);
  if(!read_file(path,&item->blob,&item->bytes)){free(item);return nullptr;}
  current=item;
  return item;
}
bool SbStorageCloseRecord(SbStorageRecord item){
  if(!item || item!=current)return false;
  free(item->blob);
  free(item);
  current=nullptr;
  return true;
}
int64_t SbStorageGetRecordSize(SbStorageRecord item){
  return item&&item==current?item->bytes:-1;
}
int64_t SbStorageReadRecord(SbStorageRecord item,char* output,int64_t len){
  if(item!=current || !item || len<0 || (!output&&len>0))return -1;
  int64_t actual=item->bytes<len?item->bytes:len;
  if(actual)memcpy(output,item->blob,(size_t)actual);
  return actual;
}
bool SbStorageWriteRecord(SbStorageRecord item,const char* data,int64_t len){
  if(item!=current || !item || len<0 || len>max_bytes ||
     (!data&&len>0))return false;
  char* new_blob=len?(char*)malloc((size_t)len):nullptr;
  if(len&&!new_blob)return false;
  if(len)memcpy(new_blob,data,(size_t)len);
  if(!save_file(item->path,data,len)){free(new_blob);return false;}
  free(item->blob);
  item->blob=new_blob;
  item->bytes=len;
  return true;
}
bool SbStorageDeleteRecord(SbUser user,const char* name){
  if(user!=&local_user || current)return false;
  char path[256]={};
  if(!name_path(name,path,sizeof(path)))return false;
  SceIoStat stat={};
  if(sceIoGetstat(path,&stat)<0)return false;
  return sceIoRemove(path)>=0;
}
}  // extern C

