// Starboard 15 local-user BLOB storage on Vita ux0. Cobalt keeps its cookie
// jar and localStorage here, i.e. the YouTube session tokens.
// Atomic-style temp + backup swap; always recover rather than delete old backup.
//
// At rest every record is encrypted and authenticated (XChaCha20-Poly1305,
// libsodium). The key is derived from this console's OpenPSID and an app
// secret, and is never written to storage: a copied .dat (backup, FTP, a
// shared log folder, a memory card moved to another Vita) cannot be read or
// replayed. Files are bound to their own name (associated data), so records
// cannot be swapped. Plaintext records from older versions are re-written
// encrypted the first time they are opened.
// Limit: code running on the same console with the same knowledge (or with
// kernel access, which can also read this process's memory) can still
// recover the key; at-rest encryption raises the bar, it is not a sandbox.
#include "starboard/log.h"
#include "starboard/storage.h"
#include "starboard/user.h"
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/io/dirent.h>
#include <psp2/kernel/openpsid.h>
#include <psp2/kernel/rng.h>
#include <sodium.h>
#include <pthread.h>
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
// Several records may be open at once (Cobalt keeps more than one store).
constexpr int kMaxOpen=8;
SbStorageRecord open_records[kMaxOpen]={};
bool is_open(SbStorageRecord r){
  if(!r)return false;
  for(auto o:open_records)if(o==r)return true;
  return false;
}
constexpr int64_t max_bytes=8*1024*1024;
constexpr char dir[]="ux0:data/youtubevita-noads/storage";
// Record name -> file. Cobalt's record names may contain dots or exceed
// 64 chars; those used to be rejected, which silently disabled persistence
// (cookies, localStorage, sign-in). Names are now sanitized ([A-Za-z0-9_-],
// other bytes -> '_', at most 48 chars) plus a hash of the full name, so
// distinct names never collide. Path traversal intent is still refused.
bool name_path(const char* input,char* out,size_t n){
  const char* name=input?input:"default";
  size_t len=strlen(name);
  if(len==0 || len>255 || strstr(name,"..") || strchr(name,'/') || strchr(name,'\\') ||
     strchr(name,':'))return false;
  char safe[49];
  size_t k=0;
  bool changed=len>48;
  for(size_t i=0;i<len && k<48;i++){
    char c=name[i];
    bool ok=(c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='_'||c=='-';
    if(!ok)changed=true;
    safe[k++]=ok?c:'_';
  }
  safe[k]=0;
  int chars;
  if(!changed){
    chars=snprintf(out,n,"%s/%s.dat",dir,safe);  // unchanged names keep their file
  }else{
    uint32_t h=2166136261u;  // FNV-1a of the full name
    for(size_t i=0;i<len;i++){h^=(uint8_t)name[i];h*=16777619u;}
    chars=snprintf(out,n,"%s/%s_%08x.dat",dir,safe,(unsigned)h);
  }
  return chars>0&&(size_t)chars<n;
}
// ---- At-rest encryption -----------------------------------------------------
constexpr char kMagic[8]={'Y','T','V','E','N','C','0','1'};
constexpr size_t kNonce=crypto_aead_xchacha20poly1305_ietf_NPUBBYTES;  // 24
constexpr size_t kTag=crypto_aead_xchacha20poly1305_ietf_ABYTES;       // 16
constexpr size_t kHeader=sizeof(kMagic)+kNonce;
// App secret mixed into the key (not a substitute for the device binding).
const unsigned char kPepper[32]={
  0x5b,0x1e,0xa7,0x93,0x0c,0xd4,0x61,0x2f,0xe8,0x47,0xb3,0x9a,0x16,0x7d,0xc5,0x08,
  0x3e,0xf1,0x84,0x2a,0x99,0x6c,0x0d,0xb7,0x52,0xe3,0x1f,0xa8,0x74,0xc9,0x30,0x6b};
unsigned char g_key[32];
bool g_key_ok=false;
pthread_once_t g_key_once=PTHREAD_ONCE_INIT;
void derive_key(){
  // No sodium_init(): it sets up libsodium's random generator, which has no
  // entropy source on the Vita (it broke the start-up in Vita3K). BLAKE2b and
  // XChaCha20-Poly1305 run on their portable implementations without it, and
  // nonces come from the system RNG (sceKernelGetRandomNumber).
  SceKernelOpenPsId id;
  memset(&id,0,sizeof(id));
  if(sceKernelGetOpenPsId(&id)<0)return;
  // Fork-specific domain: old session blobs are not portable between apps.
  static constexpr char kForkKeyDomain[] = "YouTubeVita-NoAds/storage/v1";
  unsigned char msg[sizeof(kForkKeyDomain) - 1 + 16];
  memcpy(msg,kForkKeyDomain,sizeof(kForkKeyDomain) - 1);
  memcpy(msg + sizeof(kForkKeyDomain) - 1,id.id,16);
  g_key_ok=crypto_generichash(g_key,sizeof(g_key),msg,sizeof(msg),kPepper,sizeof(kPepper))==0;
  sodium_memzero(&id,sizeof(id));
  sodium_memzero(msg,sizeof(msg));
}
bool key_ready(){pthread_once(&g_key_once,derive_key);return g_key_ok;}
const char* base_name(const char* path){const char* b=strrchr(path,'/');return b?b+1:path;}
// plaintext -> magic|nonce|ciphertext|tag (caller frees *out).
bool seal(const char* path,const char* data,int64_t bytes,char** out,int64_t* out_bytes){
  *out=nullptr;*out_bytes=0;
  if(!key_ready())return false;
  size_t total=kHeader+(size_t)bytes+kTag;
  unsigned char* buf=(unsigned char*)malloc(total);
  if(!buf)return false;
  memcpy(buf,kMagic,sizeof(kMagic));
  unsigned char* nonce=buf+sizeof(kMagic);
  if(sceKernelGetRandomNumber(nonce,kNonce)<0){free(buf);return false;}
  const char* ad=base_name(path);
  unsigned long long clen=0;
  if(crypto_aead_xchacha20poly1305_ietf_encrypt(buf+kHeader,&clen,(const unsigned char*)data,
       (unsigned long long)bytes,(const unsigned char*)ad,strlen(ad),nullptr,nonce,g_key)!=0){
    free(buf);return false;
  }
  *out=(char*)buf;*out_bytes=(int64_t)(kHeader+clen);
  return true;
}
// Returns 1 decrypted, 0 legacy plaintext (left as is), -1 rejected.
int open_sealed(const char* path,char** data,int64_t* bytes){
  if(*bytes<(int64_t)sizeof(kMagic) || memcmp(*data,kMagic,sizeof(kMagic))!=0)return 0;
  if(*bytes<(int64_t)(kHeader+kTag) || !key_ready())return -1;
  const unsigned char* in=(const unsigned char*)*data;
  size_t clen=(size_t)*bytes-kHeader;
  unsigned char* plain=(unsigned char*)malloc(clen-kTag+1);
  if(!plain)return -1;
  const char* ad=base_name(path);
  unsigned long long plen=0;
  if(crypto_aead_xchacha20poly1305_ietf_decrypt(plain,&plen,nullptr,in+kHeader,clen,
       (const unsigned char*)ad,strlen(ad),in+sizeof(kMagic),g_key)!=0){
    free(plain);return -1;  // other console, tampered or truncated
  }
  sodium_memzero(*data,(size_t)*bytes);
  free(*data);
  *data=(char*)plain;*bytes=(int64_t)plen;
  return 1;
}

bool read_file(const char* path,char** data,int64_t* bytes){
  *data=nullptr;*bytes=0;
  SceIoStat stat={};
  {
    // Interrupted swap: the previous record survives as .backup.
    char backup[300];
    if(snprintf(backup,sizeof(backup),"%s.backup",path)<(int)sizeof(backup) &&
       sceIoGetstat(path,&stat)<0 && sceIoGetstat(backup,&stat)>=0)
      sceIoRename(backup,path);
  }
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
bool save_file_raw(const char* path,const char* data,int64_t bytes);
// Encrypts, then writes atomically. Never falls back to plaintext.
bool save_file(const char* path,const char* data,int64_t bytes){
  char* sealed=nullptr;
  int64_t sealed_bytes=0;
  if(!seal(path,data,bytes,&sealed,&sealed_bytes)){
    SbLogRaw("YTV_LIFECYCLE_STORAGE_ENCRYPT_FAILED (record not written)\n");
    return false;
  }
  bool ok=save_file_raw(path,sealed,sealed_bytes);
  free(sealed);
  return ok;
}
bool save_file_raw(const char* path,const char* data,int64_t bytes){
  char tmp[300],backup[300];
  if(snprintf(tmp,sizeof(tmp),"%s.new",path)>=(int)sizeof(tmp) ||
     snprintf(backup,sizeof(backup),"%s.backup",path)>=(int)sizeof(backup))
    return false;
  SceIoStat stat={};
  // Leftovers from a write interrupted by closing the app (the Vita kills the
  // process without a Stop event). Previously these made every later save
  // fail, so the login/session never persisted again.
  if(sceIoGetstat(tmp,&stat)>=0)sceIoRemove(tmp);
  if(sceIoGetstat(backup,&stat)>=0){
    if(sceIoGetstat(path,&stat)<0)sceIoRename(backup,path);  // recover
    else sceIoRemove(backup);
  }
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
  if(user!=&local_user)return nullptr;
  char path[256]={};
  bool named=name_path(name,path,sizeof(path));
  {
    // LIFECYCLE prefix: kept in the log without verbose.txt (persistence
    // problems are otherwise invisible on hardware).
    char line[200];
    snprintf(line,sizeof(line),"YTV_LIFECYCLE_STORAGE_OPEN name=%.80s path=%s\n",
             name?name:"(null)",named?path:"(refused)");
    SbLogRaw(line);
  }
  if(!named)return nullptr;
  sceIoMkdir("ux0:data/youtubevita-noads",0777);
  sceIoMkdir(dir,0777);
  {
    // Once per run: encrypt every record still in plaintext (records Cobalt
    // does not open this run would otherwise stay readable) and drop
    // plaintext leftovers of interrupted writes.
    static bool swept=false;
    if(!swept && key_ready()){
      swept=true;
      SceUID d=sceIoDopen(dir);
      if(d>=0){
        SceIoDirent e;
        int migrated=0,removed=0;
        for(;;){
          memset(&e,0,sizeof(e));
          if(sceIoDread(d,&e)<=0)break;
          char full[320];
          if(snprintf(full,sizeof(full),"%s/%s",dir,e.d_name)>=(int)sizeof(full))continue;
          size_t n=strlen(e.d_name);
          bool leftover=(n>7&&!strcmp(e.d_name+n-7,".backup"))||(n>4&&!strcmp(e.d_name+n-4,".new"));
          if(leftover){
            char* blob=nullptr;int64_t bytes=0;
            SceUID f=sceIoOpen(full,SCE_O_RDONLY,0);
            char head[8]={0};
            if(f>=0){sceIoRead(f,head,8);sceIoClose(f);}
            // Remove only plaintext leftovers; an encrypted .backup may be
            // the only copy after an interrupted swap (read_file recovers it).
            if(memcmp(head,kMagic,8)!=0){sceIoRemove(full);removed++;}
            (void)blob;(void)bytes;
            continue;
          }
          if(n<4||strcmp(e.d_name+n-4,".dat"))continue;
          char* blob=nullptr;int64_t bytes=0;
          if(!read_file(full,&blob,&bytes)||!bytes){free(blob);continue;}
          if(memcmp(blob,kMagic,sizeof(kMagic))!=0 && save_file(full,blob,bytes))migrated++;
          sodium_memzero(blob,(size_t)bytes);
          free(blob);
        }
        sceIoDclose(d);
        char line[96];
        snprintf(line,sizeof(line),"YTV_LIFECYCLE_STORAGE_SWEEP migrated=%d leftovers_removed=%d\n",
                 migrated,removed);
        SbLogRaw(line);
      }
    }
  }
  SbStorageRecord item=(SbStorageRecord)calloc(1,sizeof(SbStorageRecordPrivate));
  if(!item)return nullptr;
  strncpy(item->path,path,sizeof(item->path)-1);
  if(!read_file(path,&item->blob,&item->bytes)){free(item);return nullptr;}
  {
    int r=item->bytes?open_sealed(path,&item->blob,&item->bytes):1;
    char line[200];
    if(r<0){
      // Not decryptable here: start with an empty record (signed out) rather
      // than handing Cobalt garbage. The file is replaced at the next write.
      free(item->blob);item->blob=nullptr;item->bytes=0;
      snprintf(line,sizeof(line),"YTV_LIFECYCLE_STORAGE_DECRYPT_FAILED path=%s\n",path);
      SbLogRaw(line);
    }else if(r==0){
      // Plaintext from an older version: encrypt it in place right away.
      bool ok=save_file(path,item->blob,item->bytes);
      snprintf(line,sizeof(line),"YTV_LIFECYCLE_STORAGE_MIGRATED path=%s bytes=%lld encrypted=%d\n",
               path,(long long)item->bytes,(int)ok);
      SbLogRaw(line);
    }
  }
  int slot=-1;
  for(int i=0;i<kMaxOpen;i++)if(!open_records[i]){slot=i;break;}
  if(slot<0){free(item->blob);free(item);return nullptr;}
  open_records[slot]=item;
  return item;
}
bool SbStorageCloseRecord(SbStorageRecord item){
  if(!is_open(item))return false;
  for(auto& o:open_records)if(o==item)o=nullptr;
  free(item->blob);
  free(item);
  return true;
}
int64_t SbStorageGetRecordSize(SbStorageRecord item){
  return is_open(item)?item->bytes:-1;
}
int64_t SbStorageReadRecord(SbStorageRecord item,char* output,int64_t len){
  if(!is_open(item) || len<0 || (!output&&len>0))return -1;
  int64_t actual=item->bytes<len?item->bytes:len;
  if(actual)memcpy(output,item->blob,(size_t)actual);
  return actual;
}
bool SbStorageWriteRecord(SbStorageRecord item,const char* data,int64_t len){
  if(!is_open(item) || len<0 || len>max_bytes ||
     (!data&&len>0))return false;
  char* new_blob=len?(char*)malloc((size_t)len):nullptr;
  if(len&&!new_blob)return false;
  if(len)memcpy(new_blob,data,(size_t)len);
  bool saved=save_file(item->path,data,len);
  {
    char line[200];
    snprintf(line,sizeof(line),"YTV_LIFECYCLE_STORAGE_WRITE path=%s bytes=%lld ok=%d\n",
             item->path,(long long)len,(int)saved);
    SbLogRaw(line);
  }
  if(!saved){free(new_blob);return false;}
  free(item->blob);
  item->blob=new_blob;
  item->bytes=len;
  return true;
}
bool SbStorageDeleteRecord(SbUser user,const char* name){
  if(user!=&local_user)return false;
  char path[256]={};
  if(!name_path(name,path,sizeof(path)))return false;
  SceIoStat stat={};
  if(sceIoGetstat(path,&stat)<0)return false;
  return sceIoRemove(path)>=0;
}
}  // extern C

