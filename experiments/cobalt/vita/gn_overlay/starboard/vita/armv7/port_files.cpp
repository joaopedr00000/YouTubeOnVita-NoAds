/*
 * Starboard 15 file system backed by actual PS Vita sceIo APIs.
 * Only app0: (read-only) and ux0:data/youtubevita-noads (read-write) are allowed.
 * No virtual mock files; all file operations hit the Vita filesystem.
 */
#include "starboard/file.h"
#include "starboard/directory.h"
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/io/dirent.h>
#include <psp2common/kernel/iofilemgr.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <climits>

struct SbFilePrivate { SceUID descriptor; bool can_write; bool can_read; char path[1024]; };
struct SbDirectoryPrivate { SceUID descriptor; };

namespace {
constexpr const char* kHome="ux0:data/youtubevita-noads";
constexpr int64_t kWindowsEpochUs=11644473600000000LL;

// Date/time is in UTC on the Vita filesystem. Convert the Gregorian date
// into Starboard's microseconds-since-1601 Windows epoch.
int64_t days_from_civil(int y,unsigned month,unsigned day) {
  y -= month<=2;
  const int era=(y>=0?y:y-399)/400;
  const unsigned yoe=static_cast<unsigned>(y-era*400);
  const unsigned doy=(153*(static_cast<int>(month)+(month>2?-3:9))+2)/5+day-1;
  const unsigned yod=yoe*365+yoe/4-yoe/100+doy;
  return static_cast<int64_t>(era)*146097+static_cast<int>(yod)-719468;
}
int64_t to_starboard_time(const SceDateTime& t){
  if(t.year<1601 || t.month<1 || t.month>12 || t.day<1 || t.day>31)
    return 0;
  const int64_t seconds=days_from_civil(t.year,t.month,t.day)*86400+
   t.hour*3600+t.minute*60+t.second;
  return seconds*1000000LL+t.microsecond+kWindowsEpochUs;
}
bool starts(const char* text,const char* prefix){
  return strncmp(text,prefix,strlen(prefix))==0;
}
bool valid_path(const char* path,bool writable){
  if(!path || !*path || strlen(path)>1020 || strstr(path,"..") ||
     strchr(path,'\\'))return false;
  // Disallow arbitrary Vita mounts. app0: must remain read-only.
  if(starts(path,"app0:"))return !writable;
  if(starts(path,kHome)){
    const char next=path[strlen(kHome)];
    return next=='/' || next=='\0';
  }
  return false;
}
bool file_info(const SceIoStat& st,SbFileInfo* result){
  if(!result)return false;
  SbFileInfo v={};
  v.size=st.st_size;
  v.is_directory=SCE_S_ISDIR(st.st_mode)!=0;
  v.is_symbolic_link=false;
  v.last_modified=to_starboard_time(st.st_mtime);
  v.last_accessed=to_starboard_time(st.st_atime);
  v.creation_time=to_starboard_time(st.st_ctime);
  *result=v;
  return true;
}
int creation_mode(int flags){
  constexpr int modes=kSbFileOpenOnly|kSbFileCreateOnly|kSbFileOpenAlways|
                      kSbFileCreateAlways|kSbFileOpenTruncated;
  int mode=flags&modes;
  if(!mode || (mode&(mode-1)))return -1;
  switch(mode){
    case kSbFileOpenOnly:return 0;
    case kSbFileCreateOnly:return SCE_O_CREAT|SCE_O_EXCL;
    case kSbFileOpenAlways:return SCE_O_CREAT;
    case kSbFileCreateAlways:return SCE_O_CREAT|SCE_O_TRUNC;
    case kSbFileOpenTruncated:return SCE_O_TRUNC;
    default:return -1;
  }
}
}
extern "C" {
SbFile SbFileOpen(const char* path,int flags,bool* out_created,
                  SbFileError* out_error) {
  if(out_created)*out_created=false;
  if(out_error)*out_error=kSbFileErrorFailed;
  int creation=creation_mode(flags);
  bool read=(flags&kSbFileRead)!=0,write=(flags&kSbFileWrite)!=0;
  if(creation<0 || (!read&&!write) || !valid_path(path,write || creation!=0)){
    if(out_error)*out_error=kSbFileErrorAccessDenied;
    return kSbFileInvalid;
  }
  int mode=(read&&write)?SCE_O_RDWR:read?SCE_O_RDONLY:SCE_O_WRONLY;
  SceIoStat st={};
  bool existed=sceIoGetstat(path,&st)>=0;
  SceUID uid=sceIoOpen(path,mode|creation,0666);
  if(uid<0){
    if(out_error)*out_error=(creation== (SCE_O_CREAT|SCE_O_EXCL) && existed)?
        kSbFileErrorExists:kSbFileErrorFailed;
    return kSbFileInvalid;
  }
  SbFile file=static_cast<SbFile>(malloc(sizeof(SbFilePrivate)));
  if(!file){
    sceIoClose(uid);
    if(out_error)*out_error=kSbFileErrorNoMemory;
    return kSbFileInvalid;
  }
  file->descriptor=uid;file->can_write=write;file->can_read=read;
  strncpy(file->path,path,sizeof(file->path)-1);
  file->path[sizeof(file->path)-1]=0;
  if(out_created)*out_created=(!existed) || (creation&SCE_O_TRUNC)!=0;
  if(out_error)*out_error=kSbFileOk;
  return file;
}
bool SbFileClose(SbFile file){
  if(!file)return false;
  int r=sceIoClose(file->descriptor);
  free(file);
  return r>=0;
}
int SbFileRead(SbFile file,char* data,int size){
  if(!file||size<0||(!data&&size>0))return -1;
  return sceIoRead(file->descriptor,data,size);
}
int SbFileWrite(SbFile file,const char* data,int size){
  if(!file||!file->can_write||size<0||(!data&&size>0))return -1;
  return sceIoWrite(file->descriptor,data,size);
}
int64_t SbFileSeek(SbFile file,SbFileWhence whence,int64_t offset){
  if(!file)return -1;
  int kind=whence==kSbFileFromBegin?SCE_SEEK_SET:
           whence==kSbFileFromCurrent?SCE_SEEK_CUR:
           whence==kSbFileFromEnd?SCE_SEEK_END:-1;
  if(kind<0)return -1;
  return sceIoLseek(file->descriptor,offset,kind);
}
bool SbFileFlush(SbFile file){
  if(!file)return false;
  return sceIoSyncByFd(file->descriptor,0)>=0;
}
bool SbFileGetInfo(SbFile file,SbFileInfo* out_info){
  if(!file||!out_info)return false;
  SceIoStat st={};
  return sceIoGetstatByFd(file->descriptor,&st)>=0&&file_info(st,out_info);
}
bool SbFileGetPathInfo(const char* path,SbFileInfo* out_info){
  if(!valid_path(path,false)||!out_info)return false;
  SceIoStat st={};
  return sceIoGetstat(path,&st)>=0&&file_info(st,out_info);
}
bool SbFileExists(const char* path){
  if(!valid_path(path,false))return false;
  SceIoStat st={};
  return sceIoGetstat(path,&st)>=0;
}
bool SbFileDelete(const char* path){
  if(!valid_path(path,true) || !strcmp(path,kHome))return false;
  SceIoStat st={};
  if(sceIoGetstat(path,&st)<0)return false;
  return (SCE_S_ISDIR(st.st_mode)?sceIoRmdir(path):sceIoRemove(path))>=0;
}
bool SbFileTruncate(SbFile file,int64_t length){
  if(!file||!file->can_write||length<0||length>128*1024*1024LL)return false;
  SceIoStat st={};
  if(sceIoGetstat(file->path,&st)<0)return false;
  if(st.st_size==length)return true;
  // Prefer the native PS Vita API. Vita3K currently returns success from
  // SCE_CST_SIZE without actually changing the file, so always verify.
  SceIoStat requested=st;
  requested.st_size=length;
  if(sceIoChstat(file->path,&requested,SCE_CST_SIZE)>=0){
    SceIoStat checkstat={};
    if(sceIoGetstat(file->path,&checkstat)>=0 && checkstat.st_size==length)
      return true;
  }

  // Vita3K compatibility path: construct a checked copy then atomically switch
  // names as far as the sceIoRename filesystem implementation supports it.
  // Keep a backup and roll it back if the second rename fails.
  char temp[1080]={},backup[1080]={};
  if(snprintf(temp,sizeof(temp),"%s.ytvtmp.%08x",file->path,
              (unsigned)file->descriptor)>=(int)sizeof(temp) ||
     snprintf(backup,sizeof(backup),"%s.ytvbak.%08x",file->path,
              (unsigned)file->descriptor)>=(int)sizeof(backup))return false;

  // Never delete an existing backup left by a previous interrupted operation.
  // It may be the only recoverable copy of the user's file.
  SceIoStat transient={};
  if(sceIoGetstat(temp,&transient)>=0 ||
     sceIoGetstat(backup,&transient)>=0)return false;
  SceUID original=sceIoOpen(file->path,SCE_O_RDONLY,0);
  if(original<0)return false;
  SceUID tmp=sceIoOpen(temp,SCE_O_CREAT|SCE_O_EXCL|SCE_O_WRONLY,0666);
  if(tmp<0){sceIoClose(original);return false;}
  uint8_t buffer[65536]={};
  int64_t retained=length<st.st_size?length:st.st_size;
  int64_t remaining=retained;
  bool copied=true;
  while(remaining>0){
    unsigned size=(unsigned)(remaining>(int64_t)sizeof(buffer)?
                              sizeof(buffer):remaining);
    int got=sceIoRead(original,buffer,size);
    if(got!=(int)size || sceIoWrite(tmp,buffer,size)!=(int)size){
      copied=false;break;
    }
    remaining-=size;
  }
  memset(buffer,0,sizeof(buffer));
  remaining=length-retained;
  while(copied && remaining>0){
    unsigned size=(unsigned)(remaining>(int64_t)sizeof(buffer)?
                              sizeof(buffer):remaining);
    if(sceIoWrite(tmp,buffer,size)!=(int)size){copied=false;break;}
    remaining-=size;
  }
  if(copied)copied=(sceIoSyncByFd(tmp,0)>=0);
  sceIoClose(original);
  sceIoClose(tmp);
  if(!copied){sceIoRemove(temp);return false;}

  // Capture and restore cursor as closely as possible after replacement.
  int64_t old_pos=sceIoLseek(file->descriptor,0,SCE_SEEK_CUR);
  if(sceIoClose(file->descriptor)<0){
    sceIoRemove(temp);return false;
  }
  file->descriptor=-1;
  bool switched=false;
  if(sceIoRename(file->path,backup)>=0){
    if(sceIoRename(temp,file->path)>=0){
      switched=true;
      sceIoRemove(backup);
    } else {
      sceIoRename(backup,file->path);
    }
  }
  if(!switched)sceIoRemove(temp);

  int mode=file->can_read?SCE_O_RDWR:SCE_O_WRONLY;
  file->descriptor=sceIoOpen(file->path,mode,0);
  if(file->descriptor<0)return false;
  if(old_pos>=0)sceIoLseek(file->descriptor,old_pos<length?old_pos:length,SCE_SEEK_SET);
  if(!switched)return false;
  SceIoStat result={};
  return sceIoGetstat(file->path,&result)>=0 && result.st_size==length;
}
// Atomic cache replacement: only publish fully written, synced temp files.
// Never fall back to truncate-in-place: that can corrupt cache on interruption.
// If the Vita filesystem cannot rename onto existing target, fail closed.
bool SbFileAtomicReplace(const char* path,const char* data,int64_t size){
  if(!valid_path(path,true) || !strcmp(path,kHome) ||
     size<0 || size>8*1024*1024LL || (size>0 && !data))return false;
  char temp[1080]={};
  int written=snprintf(temp,sizeof(temp),"%s.atomic-new",path);
  if(written<=0 || written>=(int)sizeof(temp))return false;
  SceIoStat stat={};
  if(sceIoGetstat(temp,&stat)>=0)return false; // crash recovery, do not erase
  SceUID f=sceIoOpen(temp,SCE_O_CREAT|SCE_O_EXCL|SCE_O_WRONLY,0666);
  if(f<0)return false;
  int64_t offset=0;
  bool success=true;
  while(offset<size){
    unsigned step=(unsigned)(size-offset>65536?65536:size-offset);
    int count=sceIoWrite(f,data+offset,step);
    if(count<=0){success=false;break;}
    offset+=count;
  }
  if(success)success=(sceIoSyncByFd(f,0)>=0);
  if(sceIoClose(f)<0)success=false;
  if(!success){sceIoRemove(temp);return false;}
  // sceIoRename is the only publication step; no remove of old target first.
  int renamed=sceIoRename(temp,path);
  if(renamed<0){
    sceIoRemove(temp);
    return false;
  }
  SceIoStat result={};
  return sceIoGetstat(path,&result)>=0 && result.st_size==size;
}
bool SbFileCanOpen(const char* path,int flags){
  const int creation=creation_mode(flags);
  if(creation<0)return false;
  bool write=(flags&kSbFileWrite)!=0;
  if(!valid_path(path,write||creation!=0))return false;
  SceIoStat st={};bool exists=sceIoGetstat(path,&st)>=0;
  if(creation==(SCE_O_CREAT|SCE_O_EXCL))return !exists;
  if(creation==0||creation==SCE_O_TRUNC)return exists;
  return true;
}
int SbFileModeStringToFlags(const char* mode){
  if(!mode||!*mode)return 0;
  bool plus=strchr(mode,'+')!=nullptr;
  int rw=plus?(kSbFileRead|kSbFileWrite):mode[0]=='r'?kSbFileRead:kSbFileWrite;
  switch(mode[0]){
    case 'r':return kSbFileOpenOnly|rw;
    case 'w':return kSbFileCreateAlways|rw;
    case 'a':return kSbFileOpenAlways|rw;
    default:return 0;
  }
}
SbDirectory SbDirectoryOpen(const char* path,SbFileError* out_error){
  if(out_error)*out_error=kSbFileErrorFailed;
  if(!valid_path(path,false))return kSbDirectoryInvalid;
  SceUID uid=sceIoDopen(path);
  if(uid<0)return kSbDirectoryInvalid;
  SbDirectory directory=static_cast<SbDirectory>(malloc(sizeof(SbDirectoryPrivate)));
  if(!directory){sceIoDclose(uid);if(out_error)*out_error=kSbFileErrorNoMemory;return nullptr;}
  directory->descriptor=uid;
  if(out_error)*out_error=kSbFileOk;
  return directory;
}
bool SbDirectoryClose(SbDirectory directory){
  if(!directory)return false;
  int result=sceIoDclose(directory->descriptor);
  free(directory);
  return result>=0;
}
bool SbDirectoryGetNext(SbDirectory directory,char* out_entry,size_t size){
  if(!directory||!out_entry||size<256)return false;
  SceIoDirent e={};
  int got=sceIoDread(directory->descriptor,&e);
  if(got<=0)return false;
  size_t n=strnlen(e.d_name,sizeof(e.d_name));
  if(n>=size)return false;
  memcpy(out_entry,e.d_name,n);
  out_entry[n]='\0';
  return true;
}
bool SbDirectoryCanOpen(const char* path){
  if(!valid_path(path,false))return false;
  SceIoStat st={};
  return sceIoGetstat(path,&st)>=0&&SCE_S_ISDIR(st.st_mode);
}
bool SbDirectoryCreate(const char* path){
  if(!valid_path(path,true))return false;
  SceIoStat st={};
  if(sceIoGetstat(path,&st)>=0)return SCE_S_ISDIR(st.st_mode);
  return sceIoMkdir(path,0777)>=0;
}
}  // extern C

