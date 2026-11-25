/*
 * Evergreen 5.50.2 ARM32 relocation proof on Vita/ Vita3K.
 * Official Google libcobalt.so is read as INPUT DATA ONLY.
 * We load PT_LOAD segments and apply exclusively resolvable R_ARM_REL relocations
 * to a temporary, non-executable USER_RW block.
 * Absolutely NO module init / function calls / code execution.
 */
#include <elf.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <kubridge.h>
#include <psp2/power.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <malloc.h>

// Memory budget. Evergreen's libcobalt carries its own musl malloc that
// grows through SbMemoryMap, so the browser heap is NOT the newlib heap: the
// newlib heap only serves the platform layer (and SbMemoryAllocate users).
// 112 MiB newlib + 34 MiB Cobalt image + 48 MiB VitaGL pool; everything
// else stays free for SbMemoryMap (Cobalt heap, V8) and thread stacks.
// (64 MiB was too small: media buffers come from SbMemoryAllocate, newlib use
// reached 60.6 MiB during 60 fps playback on hardware and an allocation
// failure ended in memcpy(NULL) inside libcobalt, while ~143 MiB of user
// memory was still free.)
unsigned int sceUserMainThreadStackSize = 4 * 1024 * 1024;
#if defined(YTV_LOWMEM)
// Legacy low-memory experimental profile; the v0.58 unified YouTube
// intentionally no longer defines YTV_LOWMEM and gets 112 MiB newlib.
 // Other port_* routines use ytv_lowmem_build only for legacy installs.
int ytv_lowmem_build = 1;
unsigned int _newlib_heap_size_user = 64 * 1024 * 1024;
#else
unsigned int _newlib_heap_size_user = 112 * 1024 * 1024;
#endif

#define IMAGE_CAPACITY (34u*1024u*1024u)
#define MAX_HEADERS 32u
#if defined(YTV_CUSTOM_COBALT)
#include "cobalt_candidate_profile.h"
#include <sodium.h>
#else
#define EXPECT_BYTES 33491716LL
#define EXPECT_SYMCOUNT 2434u
#define EXPECT_LOADS 4u
#define EXPECT_RELATIVE 61413u
#define EXPECT_ABS32 2884u
#define EXPECT_GLOB 157u
#define EXPECT_JUMP 664u
#define EXPECT_MISSING 211u
#define EXPECT_EXT_GLOB 14u
#define EXPECT_EXT_JUMP 197u
#define EXPECT_RESOLVED 64907u
#define EXPECT_XSTART 0x818000u
#define EXPECT_XLEN (0x1fa0000u-0x818000u)
#define EXPECT_CTORS 74u
#define EXPECT_EVENT 0x00818f7cu
#define EXPECT_FLUSH 0xd35198u
#endif
#define RELOC_R_ARM_RELATIVE 23u
#define RELOC_R_ARM_ABS32 2u
#define RELOC_R_ARM_GLOB_DAT 21u
#define RELOC_R_ARM_JUMP_SLOT 22u
extern uintptr_t ytv_find_real_starboard_export(const char *name);
extern unsigned ytv_real_starboard_export_count(void);


static int passes=0, failures=0;
static uint8_t *image = NULL;
static Elf32_Phdr headers[MAX_HEADERS];
static unsigned nheaders = 0;
static uint32_t base32 = 0;
static int fd = -1;

struct RelocStats {
  unsigned relative, abs32, glob, jump, missing, resolved, bad;
  unsigned external_glob, external_jump;
  uint32_t normalized_hash;
};

static SceUInt64 g_boot_t0=0;
static void log_line(const char *message) {
  if(!g_boot_t0)g_boot_t0=sceKernelGetProcessTimeWide();
  sceIoMkdir("ux0:data/youtubevita-noads",0777);
#if defined(YTV_COBALT_BOOT_STAGE)
  int f=sceIoOpen("ux0:data/youtubevita-noads/cobalt_boot.log",
#else
  int f=sceIoOpen("ux0:data/youtubevita-noads/relocations.log",
#endif
                 SCE_O_WRONLY | SCE_O_CREAT | SCE_O_APPEND,0666);
  if(f>=0) {
    char stamp[24];
    int sl=snprintf(stamp,sizeof(stamp),"[%7llums] ",
                    (unsigned long long)((sceKernelGetProcessTimeWide()-g_boot_t0)/1000));
    if(message[0]!='\n')sceIoWrite(f,stamp,(unsigned)sl);
    sceIoWrite(f,message,(unsigned)strlen(message));
    sceIoWrite(f,"\n",1);
    sceIoClose(f);
  }
}
static void check(int ok,const char *label) {
  char out[192];
  snprintf(out,sizeof(out),"%s %s",ok?"PASS":"FAIL",label);
  log_line(out);
  if(ok)passes++; else failures++;
}
static void note_u32(const char *key,unsigned a,unsigned b) {
  char out[192];
  snprintf(out,sizeof(out),"%s %u %u",key,a,b);
  log_line(out);
}
static int read_full(int64_t off,void *target,size_t n) {
  if(fd<0 || sceIoLseek(fd,off,SCE_SEEK_SET)<0)return 0;
  uint8_t *p=(uint8_t*)target;
  while(n) {
    unsigned chunk=(n>65536)?65536:(unsigned)n;
    int count=sceIoRead(fd,p,chunk);
    if(count<=0 || (unsigned)count>chunk)return 0;
    n-=count;p+=count;
  }
  return 1;
}
#if defined(YTV_CUSTOM_COBALT)
static int verify_candidate_sha(int64_t len) {
  crypto_hash_sha256_state state;
  unsigned char digest[crypto_hash_sha256_BYTES], buf[4096];
  char hex[crypto_hash_sha256_BYTES*2+1];
  if(crypto_hash_sha256_init(&state)!=0)return 0;
  for(int64_t off=0;off<len;){
    size_t n=(len-off>sizeof(buf))?sizeof(buf):(size_t)(len-off);
    if(!read_full(off,buf,n) || crypto_hash_sha256_update(&state,buf,n)!=0)return 0;
    off+=n;
  }
  if(crypto_hash_sha256_final(&state,digest)!=0)return 0;
  sodium_bin2hex(hex,sizeof(hex),digest,sizeof(digest));
  return strcmp(hex,CANDIDATE_SHA256)==0;
}
#endif
static int in_load(uint32_t addr,uint32_t length) {
  if((uint64_t)addr+length>IMAGE_CAPACITY)return 0;
  for(unsigned i=0;i<nheaders;i++) {
    const Elf32_Phdr *p=&headers[i];
    if(p->p_type==PT_LOAD &&
       addr>=p->p_vaddr &&
       (uint64_t)addr+length <= (uint64_t)p->p_vaddr + p->p_memsz)
      return 1;
  }
  return 0;
}
static int load_headers(const Elf32_Ehdr *eh,int64_t filelen) {
  if(eh->e_phnum==0 || eh->e_phnum>MAX_HEADERS ||
     eh->e_phentsize!=sizeof(Elf32_Phdr) ||
     (uint64_t)eh->e_phoff +
     (uint64_t)eh->e_phnum*sizeof(Elf32_Phdr)>(uint64_t)filelen)return 0;
  nheaders=eh->e_phnum;
  return read_full(eh->e_phoff,headers,nheaders*sizeof(Elf32_Phdr));
}
static uint32_t read_symbol_count(const Elf32_Ehdr *eh,int64_t filelen) {
  if(eh->e_shentsize!=sizeof(Elf32_Shdr) || eh->e_shnum==0 ||
     (uint64_t)eh->e_shoff +
     (uint64_t)eh->e_shnum*sizeof(Elf32_Shdr)>(uint64_t)filelen)return 0;
  for(unsigned i=0;i<eh->e_shnum;i++) {
    Elf32_Shdr sh={0};
    if(!read_full(eh->e_shoff+(int64_t)i*sizeof(sh),&sh,sizeof(sh)))return 0;
    if(sh.sh_type==SHT_DYNSYM && sh.sh_entsize==sizeof(Elf32_Sym) &&
       sh.sh_size%sizeof(Elf32_Sym)==0)
      return sh.sh_size/sizeof(Elf32_Sym);
  }
  return 0;
}
static int load_segments(int64_t filelen,unsigned *loads) {
  *loads=0;
  // Validate the entire layout before writing even one segment.
  for(unsigned i=0;i<nheaders;i++) {
    Elf32_Phdr *p=&headers[i];
    if(p->p_type!=PT_LOAD)continue;
    if(p->p_filesz>p->p_memsz ||
       (uint64_t)p->p_offset+p->p_filesz>(uint64_t)filelen ||
       (uint64_t)p->p_vaddr+p->p_memsz>IMAGE_CAPACITY)return 0;
  }
  for(unsigned i=0;i<nheaders;i++) {
    Elf32_Phdr *p=&headers[i];
    if(p->p_type!=PT_LOAD)continue;
    if(!read_full(p->p_offset,image+p->p_vaddr,p->p_filesz))return 0;
    if(p->p_memsz>p->p_filesz)
      memset(image+p->p_vaddr+p->p_filesz,0,p->p_memsz-p->p_filesz);
    (*loads)++;
  }
  return 1;
}
static int find_dynamic(uint32_t *rel_addr,uint32_t *rel_len,
                        uint32_t *plt_addr,uint32_t *plt_len,
                        uint32_t *sym_addr,uint32_t *sym_stride) {
  int found=0;
  for(unsigned i=0;i<nheaders;i++) {
    Elf32_Phdr *p=&headers[i];
    if(p->p_type!=PT_DYNAMIC)continue;
    if(!in_load(p->p_vaddr,p->p_memsz) ||
       p->p_memsz%sizeof(Elf32_Dyn))return 0;
    found++;
    const Elf32_Dyn *d=(const Elf32_Dyn*)(image+p->p_vaddr);
    for(unsigned j=0;j<p->p_memsz/sizeof(Elf32_Dyn);j++){
      const uint32_t v=d[j].d_un.d_val;
      switch(d[j].d_tag) {
        case DT_REL: *rel_addr=v; break;
        case DT_RELSZ: *rel_len=v; break;
        case DT_JMPREL: *plt_addr=v; break;
        case DT_PLTRELSZ: *plt_len=v; break;
        case DT_SYMTAB: *sym_addr=v; break;
        case DT_SYMENT: *sym_stride=v; break;
        default:break;
      }
      if(d[j].d_tag==DT_NULL)break;
    }
  }
  return found==1 &&
    *rel_len && *plt_len &&
    *rel_len%sizeof(Elf32_Rel)==0 &&
    *plt_len%sizeof(Elf32_Rel)==0 &&
    *sym_stride==sizeof(Elf32_Sym) &&
    in_load(*rel_addr,*rel_len) &&
    in_load(*plt_addr,*plt_len);
}
static int process_table(uint32_t start,uint32_t length,uint32_t symaddr,
                         uint32_t symcount,int apply,struct RelocStats *out) {
  const Elf32_Rel *table=(const Elf32_Rel*)(image+start);
  unsigned count=length/sizeof(Elf32_Rel);
  for(unsigned i=0;i<count;i++) {
    uint32_t r_offset=table[i].r_offset, info=table[i].r_info;
    unsigned ty=ELF32_R_TYPE(info), si=ELF32_R_SYM(info);
    if(!in_load(r_offset,sizeof(uint32_t)) || (r_offset&3)) {
      out->bad++; return 0;
    }
    uint32_t *target=(uint32_t*)(image+r_offset);
    uint32_t addend=*target, value=0;
    Elf32_Sym sym={0};
    if(ty!=RELOC_R_ARM_RELATIVE) {
      if(si>=symcount ||
         !in_load(symaddr+si*sizeof(Elf32_Sym),sizeof(Elf32_Sym))) {
        out->bad++;return 0;
      }
      sym=*((const Elf32_Sym*)(image+symaddr+si*sizeof(Elf32_Sym)));
    } else if(si!=0) {out->bad++;return 0;}
    if(ty==RELOC_R_ARM_RELATIVE){out->relative++;value=base32+addend;}
    else if(ty==RELOC_R_ARM_ABS32) {
      out->abs32++;
      if(sym.st_shndx==SHN_UNDEF){out->missing++;continue;}
      value=(sym.st_shndx==SHN_ABS?0:base32)+sym.st_value+addend;
    } else if(ty==RELOC_R_ARM_GLOB_DAT || ty==RELOC_R_ARM_JUMP_SLOT) {
      if(ty==RELOC_R_ARM_GLOB_DAT)out->glob++; else out->jump++;
      if(sym.st_shndx==SHN_UNDEF) {
        out->missing++;
        if(ty==RELOC_R_ARM_GLOB_DAT)out->external_glob++;
        else out->external_jump++;
        continue;
      }
      value=(sym.st_shndx==SHN_ABS?0:base32)+sym.st_value;
    } else {out->bad++;return 0;}
    // Never dereference the relocated code pointer. Compare only the data.
    if(apply) {
      *target=value;
      if(*target!=value) {out->bad++;return 0;}
    }
    out->resolved++;
    out->normalized_hash = (out->normalized_hash*33u) ^
      (value-base32) ^ (r_offset*17u);
  }
  return 1;
}
static int apply_real_imports(uint32_t symtab,uint32_t symcount,
                              uint32_t rela,uint32_t relsz,
                              uint32_t jmpa,uint32_t jmpsz,
                              unsigned *linked,unsigned *unresolved) {
  uint32_t strtab=0,strsz=0;
  for(unsigned i=0;i<nheaders;i++) {
    const Elf32_Phdr *p=&headers[i];
    if(p->p_type!=PT_DYNAMIC)continue;
    const Elf32_Dyn *d=(const Elf32_Dyn*)(image+p->p_vaddr);
    for(unsigned j=0;j<p->p_memsz/sizeof(Elf32_Dyn);j++) {
      if(d[j].d_tag==DT_STRTAB)strtab=d[j].d_un.d_ptr;
      if(d[j].d_tag==DT_STRSZ)strsz=d[j].d_un.d_val;
      if(d[j].d_tag==DT_NULL)break;
    }
  }
  if(strtab==0 || strsz==0 || !in_load(strtab,strsz) ||
     !in_load(symtab,symcount*sizeof(Elf32_Sym)))return 0;
  const uint32_t start[2]={rela,jmpa};
  const uint32_t size[2]={relsz,jmpsz};
  *linked=0;*unresolved=0;
  for(unsigned t=0;t<2;t++) {
    const Elf32_Rel *rels=(const Elf32_Rel*)(image+start[t]);
    for(unsigned i=0;i<size[t]/sizeof(Elf32_Rel);i++) {
      uint32_t kind=ELF32_R_TYPE(rels[i].r_info);
      if(kind!=RELOC_R_ARM_GLOB_DAT && kind!=RELOC_R_ARM_JUMP_SLOT)continue;
      unsigned idx=ELF32_R_SYM(rels[i].r_info);
      if(idx>=symcount || !in_load(rels[i].r_offset,4))return 0;
      const Elf32_Sym *sym=(const Elf32_Sym*)(image+symtab+idx*sizeof(Elf32_Sym));
      if(sym->st_shndx!=SHN_UNDEF)continue;
      if(sym->st_name>=strsz)return 0;
      const char *name=(const char*)(image+strtab+sym->st_name);
      unsigned available=strsz-sym->st_name;
      if(!memchr(name,0,available))return 0;
      uintptr_t address=ytv_find_real_starboard_export(name);
      if(address!=0) {
        *(uint32_t*)(image+rels[i].r_offset)=(uint32_t)address;
        if(*(uint32_t*)(image+rels[i].r_offset)!=(uint32_t)address)return 0;
        (*linked)++;
      } else {
        // Leave missing imports untouched; do NOT replace them with a fake stub.
        (*unresolved)++;
      }
    }
  }
  return 1;
}


#if defined(YTV_COBALT_BOOT_STAGE) && YTV_COBALT_BOOT_STAGE>=2
// Independent heartbeat: distinguishes a real deadlock (counters frozen)
// from slow progress. Reads only counters, never touches Cobalt state.
extern int ytv_runtime_once_count(void);
extern int ytv_memory_map_count(void);
extern unsigned ytv_memory_map_bytes(void);
extern void ytv_thread_cpu_report(char* out,int n,const char* main_name);
extern void ytv_gl_stats(char* out,int n);
static int heartbeat_thread(SceSize args,void* argp){
  (void)args;(void)argp;
  // Runs for the whole session (2 s steps for a minute, then 5 s) so a
  // later freeze still shows which threads use the CPU.
  for(unsigned t=2;;t+=(t<60?2:5)){
    sceKernelDelayThread(t<60?2000000:5000000);
    SceKernelFreeMemorySizeInfo mem={0};
    mem.size=sizeof(mem);
    sceKernelGetFreeMemorySize(&mem);
    char line[160];
    snprintf(line,sizeof(line),
             "COBALT_HEARTBEAT t=%us once=%d user_free=%d cdram_free=%d "
             "newlib_used=%d maps=%d map_bytes=%u",
             t,ytv_runtime_once_count(),mem.size_user,mem.size_cdram,
             (int)mallinfo().uordblks,ytv_memory_map_count(),
             ytv_memory_map_bytes());
    log_line(line);
    char cpu[300];
    ytv_thread_cpu_report(cpu,sizeof(cpu),NULL);
    log_line(cpu);
    ytv_gl_stats(cpu,sizeof(cpu));
    log_line(cpu);
  }
  return 0;
}
#endif


// V8's ARM FlushICache is the only Linux syscall in libcobalt.so
// (svc 0 with r7=0xF0002, cacheflush) at ELF 0xD35198. Vita has no such
// syscall; with the JIT enabled V8 calls it after emitting code. The loader
// redirects that function (in the RAM copy, before it becomes executable) to
// this shim, which performs the real D/I-cache maintenance via kubridge.
static void ytv_flush_icache(void* start, unsigned size) {
  if (start && size) kuKernelFlushCaches(start, size);
}
#define V8_FLUSH_ICACHE_OFFSET EXPECT_FLUSH
// "beq" after "ldr r1,[r6,#20]; cmp r1,#0" in CspDelegateSecure::CanLoad.
#define COBALT_CSP_REQUIRED_BRANCH 0x18239bcu
// "beq ok" after "ldr r0,[r5,#12]; cmp r0,#1" in HTMLDecoder (CSP headers).
#define COBALT_HTML_DECODER_CSP_BRANCH 0x1b9cc00u
// Embedded user_agent_style_sheet.css registration: size, literal, add-pc.
#define COBALT_UA_SHEET_MOVW 0x19954f0u
#define COBALT_UA_SHEET_LITERAL 0x19957ecu
#define COBALT_UA_SHEET_ADD_PC 0x19954fcu

static int get_init_array(uint32_t *addr,uint32_t *length){
  if(!addr||!length)return 0;
  *addr=0;*length=0;
  for(unsigned i=0;i<nheaders;i++){
    const Elf32_Phdr* p=&headers[i];
    if(p->p_type!=PT_DYNAMIC)continue;
    if(!in_load(p->p_vaddr,p->p_memsz))return 0;
    const Elf32_Dyn* d=(const Elf32_Dyn*)(image+p->p_vaddr);
    for(unsigned j=0;j<p->p_memsz/sizeof(*d);j++){
      if(d[j].d_tag==DT_INIT_ARRAY)*addr=d[j].d_un.d_ptr;
      else if(d[j].d_tag==DT_INIT_ARRAYSZ)*length=d[j].d_un.d_val;
      else if(d[j].d_tag==DT_NULL)break;
    }
  }
  return *addr && *length && (*length%4)==0 &&
         in_load(*addr,*length);
}

int main(void) {
#if defined(YTV_COBALT_BOOT_STAGE)
  // v0.64: every session starts with fresh logs. The previous session is kept
  // once as *_prev.log so a crash log survives the relaunch that follows it.
  {
    static const char* const kLogs[][2]={
      {"ux0:data/youtubevita-noads/cobalt_boot.log","ux0:data/youtubevita-noads/cobalt_boot_prev.log"},
      {"ux0:data/youtubevita-noads/starboard15.log","ux0:data/youtubevita-noads/starboard15_prev.log"},
    };
    for(unsigned i=0;i<sizeof(kLogs)/sizeof(kLogs[0]);++i){
      sceIoRemove(kLogs[i][1]);
      if(sceIoRename(kLogs[i][0],kLogs[i][1])<0)sceIoRemove(kLogs[i][0]);
    }
  }
  {
    // RAM granted to this process by its param.sfo memory mode: what is free
    // now plus what is already committed (newlib heap, eboot image, stack).
    extern long long ytv_measured_total_memory;
    SceKernelFreeMemorySizeInfo mem={0};
    mem.size=sizeof(mem);
    if(sceKernelGetFreeMemorySize(&mem)>=0)
      ytv_measured_total_memory=(long long)mem.size_user+
          (long long)_newlib_heap_size_user+sceUserMainThreadStackSize+
          4LL*1024*1024 /* eboot code+data */;
  }
  // Full application clocks (defaults are 333 MHz CPU / 111 MHz GPU).
  scePowerSetArmClockFrequency(444);
  scePowerSetBusClockFrequency(222);
  scePowerSetGpuClockFrequency(222);
  scePowerSetGpuXbarClockFrequency(166);
  // Main thread affinity: 3 user cores, or 4 with CapUnlocker.
  { extern int ytv_vita_cpu_mask(void); ytv_vita_cpu_mask(); }  // probes CapUnlocker 4th core
#endif
#if defined(YTV_COBALT_BOOT_STAGE)
  // The custom splash video + its own PCM jingle play exactly once here.
  // VitaGL's stock splash is disabled at link time; do not mute our audio.
  { extern int ytv_native_startup_splash(void);
    int intro=ytv_native_startup_splash();
    char intro_msg[200];
    extern int ytv_native_startup_splash_audio_status(void);
    extern int ytv_native_startup_splash_audio_started(void);
    extern int ytv_native_startup_splash_frames_presented(void);
    extern int ytv_native_startup_splash_frames_dropped(void);
    extern int ytv_native_startup_splash_vblank_fallbacks(void);
    snprintf(intro_msg,sizeof(intro_msg),
       "YTV_CUSTOM_SPLASH result=%d audio_status=%d audio_started=%d "
       "presented=%d skipped=%d vblank_fallbacks=%d",
       intro,ytv_native_startup_splash_audio_status(),
       ytv_native_startup_splash_audio_started(),
       ytv_native_startup_splash_frames_presented(),
       ytv_native_startup_splash_frames_dropped(),
       ytv_native_startup_splash_vblank_fallbacks());
    log_line(intro_msg);
  }
#endif
  log_line("\nSTART Evergreen armv7 RAM-only relocation validation");
#if defined(YTV_LOWMEM)
  log_line("YTV_MEMORY_PROFILE mode=normal newlib_mb=64\n");
#else
  log_line("YTV_MEMORY_PROFILE mode=extended newlib_mb=112 attr2=12\n");
#endif
  fd=sceIoOpen("app0:libcobalt.so",SCE_O_RDONLY,0);
  check(fd>=0,"official Cobalt ELF data accessible");
  if(fd<0)goto done;
  {
    int64_t len=sceIoLseek(fd,0,SCE_SEEK_END);
    check(len==EXPECT_BYTES,"ELF byte length matches selected profile");
    if(len!=EXPECT_BYTES)goto done;
#if defined(YTV_CUSTOM_COBALT)
    check(verify_candidate_sha(len),"candidate ELF matches generated SHA-256");
    if(failures)goto done;
    log_line("YTV_EXPERIMENTAL_COBALT_PROFILE " CANDIDATE_SHA256);
#endif
    Elf32_Ehdr eh={0};
    check(read_full(0,&eh,sizeof(eh)) &&
          memcmp(eh.e_ident,ELFMAG,SELFMAG)==0 &&
          eh.e_ident[EI_CLASS]==ELFCLASS32 &&
          eh.e_ident[EI_DATA]==ELFDATA2LSB &&
          eh.e_machine==EM_ARM && eh.e_type==ET_DYN,
          "ELF32 little endian ARM shared object");
    if(failures)goto done;
    check(load_headers(&eh,len),"validated ELF program headers");
    if(failures)goto done;
    uint32_t symcount=read_symbol_count(&eh,len);
    note_u32("DYNSYM_COUNT",symcount,0);
    check(symcount==EXPECT_SYMCOUNT,"symbol bounds from official SHT_DYNSYM");
    if(failures)goto done;
    SceUID uid=sceKernelAllocMemBlock("CobaltRelocRW",
                                      SCE_KERNEL_MEMBLOCK_TYPE_USER_RW,
                                      IMAGE_CAPACITY,NULL);
    check(uid>=0,"contiguous 34 MiB NON-EXECUTABLE user memory");
    if(uid<0)goto done;
    {
      void *base=NULL;
      int mapped=sceKernelGetMemBlockBase(uid,&base);
      check(mapped==0 && base!=NULL,"Vita user RW block mapping");
      if(mapped!=0 || !base)goto release;
      image=(uint8_t*)base;
      base32=(uint32_t)(uintptr_t)base;
      note_u32("COBALT_IMAGE_BASE",base32,IMAGE_CAPACITY);
      { extern uintptr_t ytv_cobalt_image_base; ytv_cobalt_image_base=base32; }
      unsigned count=0;
      check(load_segments(len,&count) && count==EXPECT_LOADS,
            "loaded all profiled PT_LOAD segments into virtual layout");
      if(failures)goto release;
      uint32_t ra=0,rl=0,ja=0,jl=0,sa=0,ss=0;
      check(find_dynamic(&ra,&rl,&ja,&jl,&sa,&ss) &&
            in_load(sa,symcount*ss),
            "read true DT_REL, DT_JMPREL and DT_SYMTAB from RAM");
      if(failures)goto release;
      note_u32("RELOCATION_TABLE_SIZE",rl/8,jl/8);
      struct RelocStats before={0};
      // First phase: bounds-check all relocation targets and symbol indices;
      // NO WRITES to relocation targets.
      int verified=process_table(ra,rl,sa,symcount,0,&before) &&
                   process_table(ja,jl,sa,symcount,0,&before);
      check(verified &&
            before.relative==EXPECT_RELATIVE &&
            before.abs32==EXPECT_ABS32 &&
            before.glob==EXPECT_GLOB &&
            before.jump==EXPECT_JUMP &&
            before.missing==EXPECT_MISSING &&
            before.bad==0 &&
            before.resolved==EXPECT_RESOLVED,
            "all profiled ARM REL entries validated without writing");
      if(failures)goto release;
      struct RelocStats after={0};
      // This phase only patches ordinary, non-executable Vita data RAM.
      int relocated=process_table(ra,rl,sa,symcount,1,&after) &&
                    process_table(ja,jl,sa,symcount,1,&after);
      note_u32("RELOCATED_AND_DEFERRED",after.resolved,after.missing);
      note_u32("UNRESOLVED_GLOB_JUMP",after.external_glob,after.external_jump);
      check(relocated && after.bad==0 && after.resolved==EXPECT_RESOLVED &&
            after.missing==EXPECT_MISSING,
            "profiled internally resolved relocations applied in RW RAM");
      check(after.normalized_hash==before.normalized_hash,
            "relocation values match pre-write computed hash");
      check(after.external_glob==EXPECT_EXT_GLOB && after.external_jump==EXPECT_EXT_JUMP,
            "profiled external relocation slots remain explicitly unresolved");
      check(image[0]==0x7f && image[1]=='E' && image[2]=='L' && image[3]=='F',
            "in-memory ELF header intact after relocations");
      unsigned imported=0,still_missing=0;
      int imports_valid=apply_real_imports(sa,symcount,ra,rl,ja,jl,
                                           &imported,&still_missing);
      note_u32("REAL_STARBOARD_EXPORTS_LINKED",imported,still_missing);
      check(imports_valid && imported==EXPECT_MISSING && still_missing==0 &&
            ytv_real_starboard_export_count()==212 &&
            imported+still_missing==EXPECT_MISSING,
            "all implemented Vita Starboard exports linked; remaining imports fail closed");
      if(failures)goto release;
      // Read-only-executable protection gate: we never invoke the Cobalt code.
      // Dynamic relocations in this official image all target RW data segments,
      // so protecting its RX segment does not interfere with unresolved imports.
      uint32_t xstart=0,xlen=0;unsigned execs=0;
      for(unsigned i=0;i<nheaders;i++){
        const Elf32_Phdr *p=&headers[i];
        if(p->p_type==PT_LOAD && (p->p_flags & PF_X)){
          execs++;
          uint32_t lo=p->p_vaddr & ~4095u;
          uint32_t hi=(uint32_t)(((uint64_t)p->p_vaddr+p->p_memsz+4095u)&~4095ull);
          xstart=lo;xlen=hi-lo;
        }
      }
      check(execs==1 && xstart==EXPECT_XSTART &&
            xlen==EXPECT_XLEN &&
            (uint64_t)xstart+xlen<=IMAGE_CAPACITY,
            "precise alignment and size of official Cobalt ARM executable segment");
      if(failures)goto release;

#if defined(YTV_COBALT_DIAG_CTOR_CUTOFF)
      // Debug VPK modifies only its private RAM copy of one constructor,
      // before mapping RX. Epilogue matches push-r4-lr/sub-sp-128 prologue.
      {
        unsigned target=0;
        int diag_fd=sceIoOpen("ux0:data/youtubevita-noads/ctor_cutoff.txt",SCE_O_RDONLY,0);
        if(diag_fd>=0){
          char value[24]={0};
          int n=sceIoRead(diag_fd,value,sizeof(value)-1);
          sceIoClose(diag_fd);
          if(n>0){
            unsigned scan=0;
            if(sscanf(value,"%x",&scan)==1)target=scan;
          }
        }
        if(target!=0){
          int allowed=target==0x819c74u || target==0x819c8cu ||
              target==0x819c98u || target==0x819cb0u || target==0x819cc8u;
          check(allowed && in_load(target,8),
               "constructor debug cutoff selected only from verified epilogues");
          if(failures)goto release;
          uint32_t* instr=(uint32_t*)(image+target);
          instr[0]=0xe28dd080u; // add sp,sp,#128
          instr[1]=0xe8bd8010u; // pop {r4,pc}
          note_u32("COBALT_CTOR_DEBUG_CUTOFF",target,1);
        }
      }
#endif

#if defined(YTV_COBALT_DIAG_EVENT_CUTOFF)
      {
        unsigned target=0;
        int f=sceIoOpen("ux0:data/youtubevita-noads/event_cutoff.txt",
                       SCE_O_RDONLY,0);
        if(f>=0){
          char value[24]={0};
          int n=sceIoRead(f,value,sizeof(value)-1);
          sceIoClose(f);
          if(n>0 && sscanf(value,"%x",&target)!=1)target=0;
        }
        if(target){
          // Valid instruction boundaries inside official Start event handler.
          const unsigned allowed[]={
            0x819160u,0x81916cu,0x81917cu,0x819180u,
            0x819188u,0x81918cu,0x819190u,0x819194u,0x81919cu,0x8191a8u,
            0x8191b4u,0x8191c4u,0x8191c8u,0x8191d0u,
            0x8191e0u,0x819200u,0x819204u,0x819210u,
            0x8193f8u,0x8193fcu,0x819400u,0x819440u,
            0x819444u,0x819454u,0x819458u,0x819464u,
            0x819468u,0x819474u
          };
          int okay=0;
          for(unsigned i=0;i<sizeof(allowed)/sizeof(allowed[0]);i++)
            if(target==allowed[i])okay=1;
          check(okay && in_load(target,8),"validated isolated Start event cutoff");
          if(failures)goto release;
          uint32_t* instr=(uint32_t*)(image+target);
          instr[0]=0xe28dd0b0u;  // add sp,sp,#176
          instr[1]=0xe8bd88f0u;  // pop {r4,r5,r6,r7,fp,pc}
          note_u32("COBALT_EVENT_CUTOFF",target,1);
        }
      }
#endif

#if defined(YTV_COBALT_DIAG_EVENT_CUTOFF)
      // Instrument only the private RW image before mapping RX: test Cobalt
      // start's nested configuration function in isolation.
      {
        unsigned target=0;
        int f=sceIoOpen("ux0:data/youtubevita-noads/inner_cutoff.txt",
                        SCE_O_RDONLY,0);
        if(f>=0){
          char value[24]={0};
          int n=sceIoRead(f,value,sizeof(value)-1);
          sceIoClose(f);
          if(n>0 && sscanf(value,"%x",&target)!=1)target=0;
        }
        if(target){
          const unsigned allowed[]={
            0x13210a8u,0x13210acu,0x13210c0u,0x13210ccu,
            0x13210d0u,0x13210e0u,0x13210e4u,0x1321160u,
            0x1321164u,0x1321170u,0x13211b8u,0x13211ccu,
            0x13211dcu,0x13211ecu,0x13211f4u,0x132124cu
          };
          int okay=0;
          for(unsigned i=0;i<sizeof(allowed)/sizeof(allowed[0]);i++)
            if(target==allowed[i])okay=1;
          check(okay && in_load(target,8),"verified nested Cobalt Start cutoff");
          if(failures)goto release;
          uint32_t* instr=(uint32_t*)(image+target);
          instr[0]=0xe28dd0c0u; // add sp,sp,#192
          instr[1]=0xe8bd8070u; // pop {r4,r5,r6,pc}
          note_u32("COBALT_INNER_CUTOFF",target,1);
        }
      }
#endif

#if defined(YTV_COBALT_DIAG_EVENT_CUTOFF)
      // Only the isolated diagnostic title can bisect the real SbOnce init
      // callback. Does not touch the official ELF file or normal relocation.
      {
        unsigned target=0;
        int f=sceIoOpen("ux0:data/youtubevita-noads/init_cutoff.txt",SCE_O_RDONLY,0);
        if(f>=0){
          char value[24]={0};
          int n=sceIoRead(f,value,sizeof(value)-1);
          sceIoClose(f);
          if(n>0 && sscanf(value,"%x",&target)!=1)target=0;
        }
        if(target){
          const unsigned allowed[]={
            0x15dc4dcu,0x15dc4f0u,0x15dc524u,0x15dc534u,
            0x15dc538u,0x15dc540u,0x15dc544u,0x15dc54cu,
            0x15dc59cu,0x15dc5b8u,0x15dc5c8u,0x15dc5dcu,
            0x15dc5ecu,0x15dc604u,0x15dc614u
          };
          int okay=0;
          for(unsigned i=0;i<sizeof(allowed)/sizeof(allowed[0]);i++)
            if(allowed[i]==target)okay=1;
          check(okay && in_load(target,8),"validated isolated Cobalt Once cutoff");
          if(failures)goto release;
          uint32_t* ins=(uint32_t*)(image+target);
          ins[0]=0xe28dd020u; // add sp,sp,#32
          ins[1]=0xe8bd81f0u; // pop {r4,r5,r6,r7,r8,pc}
          note_u32("COBALT_ONCE_INIT_CUTOFF",target,1);
        }
      }
#endif
      {
        check(in_load(V8_FLUSH_ICACHE_OFFSET,20) && V8_FLUSH_ICACHE_OFFSET>=xstart && V8_FLUSH_ICACHE_OFFSET+20<=xstart+xlen,"cacheflush patch lies in executable segment");
        if(failures)goto release;
        uint32_t* f=(uint32_t*)(image+V8_FLUSH_ICACHE_OFFSET);
        int matches=f[0]==0xe0801001u && f[1]==0xe3a02000u &&
                    f[2]==0xe52d7004u && f[4]==0xef000000u;
        check(matches,"V8 cacheflush syscall found at expected ELF offset");
        if(failures)goto release;
        f[0]=0xe51ff004u;                       // ldr pc, [pc, #-4]
        f[1]=(uint32_t)(uintptr_t)&ytv_flush_icache;
        note_u32("V8_FLUSH_ICACHE_SHIM",V8_FLUSH_ICACHE_OFFSET,f[1]);
      }
#if !defined(YTV_CUSTOM_COBALT)
      {
        // Browser builds: production Cobalt forces csp::kCSPRequired, so on a
        // page served without a Content-Security-Policy header
        // CspDelegateSecure::CanLoad refuses every subresource (CSS, scripts,
        // images) and the page stays black. Removing the "policy == required"
        // branch gives the debug-build behaviour (kCSPOptional). Applied only
        // when the original instructions match (Cobalt 25.lts.50.1035044).
        int want_relax=0;
#if defined(YTV_BROWSER_BUILD)
        want_relax=1;
#else
        { SceIoStat st; want_relax=sceIoGetstat("ux0:data/youtubevita-noads/csp_optional.txt",&st)>=0; }
#endif
        if(want_relax){
          uint32_t* c=(uint32_t*)(image+COBALT_CSP_REQUIRED_BRANCH);
          int matches=c[-2]==0xe5961014u && c[-1]==0xe3510000u && c[0]==0x0a000012u;
          note_u32("COBALT_CSP_OPTIONAL",COBALT_CSP_REQUIRED_BRANCH,(unsigned)matches);
          if(matches)c[0]=0xe320f000u;  // nop
          // dom_parser/html_decoder.cc: "Failure receiving Content Security
          // Policy headers" unless policy == kCSPOptional -> always accept.
          uint32_t* h=(uint32_t*)(image+COBALT_HTML_DECODER_CSP_BRANCH);
          int h_ok=h[-2]==0xe595000cu && h[-1]==0xe3500001u && h[0]==0x0a000030u;
          note_u32("COBALT_HTML_CSP_OPTIONAL",COBALT_HTML_DECODER_CSP_BRANCH,(unsigned)h_ok);
          if(h_ok)h[0]=0xea000030u;  // beq -> b
        }
        // Browser builds: replace Cobalt's TV user-agent style sheet (an
        // embedded resource: PC-relative pointer + "movw r3,#773" size) with
        // ours, so pages get a white background, margins, headings, links.
        const char* ua_path=NULL;
#if defined(YTV_BROWSER_BUILD)
        ua_path="app0:browser/ua.css";
#else
        ua_path="ux0:data/youtubevita-noads/ua.css";  // diagnostic
#endif
        SceUID ua_fd=sceIoOpen(ua_path,SCE_O_RDONLY,0);
        if(ua_fd>=0){
          static char* ua_css=NULL;
          SceOff ua_len=sceIoLseek(ua_fd,0,SCE_SEEK_END);
          sceIoLseek(ua_fd,0,SCE_SEEK_SET);
          uint32_t* mv=(uint32_t*)(image+COBALT_UA_SHEET_MOVW);
          uint32_t* lit=(uint32_t*)(image+COBALT_UA_SHEET_LITERAL);
          uint32_t add_pc=(uint32_t)(uintptr_t)(image+COBALT_UA_SHEET_ADD_PC);
          int ok=mv[0]==0xe3003305u && lit[0]==0xfee564b1u && ua_len>0 && ua_len<0xffff;
          if(ok) ua_css=(char*)malloc((size_t)ua_len+1);
          if(ok && ua_css && sceIoRead(ua_fd,ua_css,(SceSize)ua_len)==(int)ua_len){
            ua_css[ua_len]=0;
            uint32_t n=(uint32_t)ua_len;
            mv[0]=0xe3003000u|((n>>12)<<16)|(n&0xfffu);  // movw r3,#n
            lit[0]=(uint32_t)(uintptr_t)ua_css-(add_pc+8u);
            note_u32("COBALT_UA_SHEET",n,1);
          }else{
            note_u32("COBALT_UA_SHEET",(unsigned)ua_len,0);
          }
          sceIoClose(ua_fd);
        }
      }
#endif
      uint8_t first_text_bytes[16];
      memcpy(first_text_bytes,image+xstart,16);
      // v0.61 diagnostic: the issue #2 logs consistently terminate before
      // the old monolithic (~24 MiB) kuKernelMemProtect call. Use smaller
      // page-aligned 1 MiB chunks and durable pre-call checkpoints to
      // distinguish an unsupported page-protection operation from the V8
      // initialization problems observed after the call.
      int protection=0;
#if defined(YTV_V061_RX_CHUNKS)
      const uint32_t rx_chunk=1024u*1024u;
      uint32_t rx_done=0;
      note_u32("V061_RX_PROTECT_BEGIN",xstart,xlen);
      while(rx_done<xlen){
        uint32_t n=xlen-rx_done;
        if(n>rx_chunk)n=rx_chunk;
        note_u32("V061_RX_CHUNK_ENTER",xstart+rx_done,n);
        protection=kuKernelMemProtect(image+xstart+rx_done,n,
                         KU_KERNEL_PROT_READ|KU_KERNEL_PROT_EXEC);
        if(protection<0){
          note_u32("V061_RX_CHUNK_FAILED",(unsigned)protection,xstart+rx_done);
          break;
        }
        rx_done+=n;
      }
      note_u32("V061_RX_PROTECT_PROGRESS",rx_done,xlen);
      if(rx_done!=xlen && protection>=0)protection=-1;
#else
      protection=kuKernelMemProtect(image+xstart,xlen,
                           KU_KERNEL_PROT_READ|KU_KERNEL_PROT_EXEC);
#endif
      note_u32("COBALT_TEXT_RX_PROTECTION", (unsigned)protection, xlen);
      check(protection==0,"kubridge maps official Cobalt code segment R-X");
      if(failures)goto release;
#if defined(YTV_V061_RX_CHUNKS)
      log_line("V061_RX_FLUSH_BEGIN");
      for(uint32_t off=0;off<xlen;off+=rx_chunk){
        uint32_t n=xlen-off;
        if(n>rx_chunk)n=rx_chunk;
        kuKernelFlushCaches(image+xstart+off,n);
      }
      log_line("V061_RX_FLUSH_COMPLETE");
#else
      kuKernelFlushCaches(image+xstart,xlen);
#endif
      check(memcmp(first_text_bytes,image+xstart,16)==0,
            "Cobalt original text remains intact after RX permissions");

#if !defined(YTV_CUSTOM_COBALT)
      // A trusted, 2-instruction test function goes in the unused gap before
      // the official text PT_LOAD. It is not part of any Cobalt segment.
      const uint32_t stub_offset=xstart-0x1000u;
      check(stub_offset>=0x808e10u && !in_load(stub_offset,8),
            "trusted executable test page is outside every Cobalt PT_LOAD segment");
      if(failures)goto release;
      const uint32_t demo[2]={0xe3a0002au,0xe12fff1eu}; // mov r0,#42; bx lr
      memcpy(image+stub_offset,demo,sizeof(demo));
      protection=kuKernelMemProtect(image+stub_offset,0x1000,
                                     KU_KERNEL_PROT_READ|KU_KERNEL_PROT_EXEC);
      check(protection==0,"trusted test page separately mapped R-X inside 34MB image");
      if(failures)goto release;
      kuKernelFlushCaches(image+stub_offset,sizeof(demo));
      int (*known_function)(void)=(int (*)(void))(image+stub_offset);
      int trusted_return=known_function();
      note_u32("TRUSTED_ARM_EXEC_RETURN", (unsigned)trusted_return,stub_offset);
      check(trusted_return==42,"trusted ARM code executes inside full Cobalt allocation");

      // PT_LOAD #4 has a zero-initialized BSS tail. Data must stay writable.
      uint32_t bss_offset=0x203d9f0u;
      check(in_load(bss_offset,4),"BSS address belongs to writable data PT_LOAD");
      if(failures)goto release;
      uint32_t *bss=(uint32_t*)(image+bss_offset);
      uint32_t saved=*bss;
      *bss=0x53425654u;
      check(*bss==0x53425654u,"writable Cobalt BSS survives separate RX mapping");
      *bss=saved;
#endif
      uint32_t ctor_addr=0,ctor_bytes=0;
      int ctor_valid=get_init_array(&ctor_addr,&ctor_bytes) &&
                     ctor_bytes==EXPECT_CTORS*sizeof(uint32_t);
      if(ctor_valid){
        uint32_t* ctors=(uint32_t*)(image+ctor_addr);
        for(unsigned j=0;j<ctor_bytes/4;j++){
          uint32_t ptr=ctors[j] & ~1u;
          if(ptr<base32+xstart || ptr>=base32+xstart+xlen){
            ctor_valid=0;break;
          }
        }
      }
      note_u32("ELF_INIT_ARRAY",ctor_bytes/4,ctor_valid?1u:0u);
      check(ctor_valid,"all profiled Cobalt C++ initializers belong to RX text");
      if(failures)goto release;
#if defined(YTV_COBALT_BOOT_STAGE) && (YTV_COBALT_BOOT_STAGE>=1)
      if(still_missing!=0)goto release;
      typedef void (*Ctor)(void);
      uint32_t *initializer=(uint32_t*)(image+ctor_addr);
      log_line("COBALT_CONSTRUCTOR_PROBE_BEGIN");
#if !defined(YTV_CUSTOM_COBALT)
      // Probe an official Cobalt internal ARM memcpy function which ctor #0
      // calls through PLT. This checks actual cross-page execution and return.
      {
        typedef void (*ArmCopy)(void*,const void*,unsigned);
        ArmCopy fn=(ArmCopy)(uintptr_t)(base32+0x016b4da0u);
        unsigned char source[128]={},dest[128]={};
        for(unsigned j=0;j<sizeof(source);j++)source[j]=(unsigned char)(j^0x7bu);
        log_line("COBALT_MEMCPY_ENTER");
        fn(dest,source,sizeof(source));
        log_line("COBALT_MEMCPY_RETURN");
        check(memcmp(dest,source,sizeof(source))==0,
              "official Cobalt internal __aeabi_memcpy8 copies 128 bytes");
        if(failures)goto release;
      }

#endif
      for(unsigned j=0;j<ctor_bytes/4;j++){
        // One log line per constructor cost ~150 memory-card writes at
        // boot; only the summary below is logged.
        Ctor call=(Ctor)(uintptr_t)initializer[j];
        call();
#if defined(YTV_COBALT_DIAG_CTOR_CUTOFF)
        log_line("COBALT_DEBUG_ONE_CTOR_FINISHED");
        break; // diagnostic only
#endif
      }
      log_line("COBALT_CONSTRUCTORS_COMPLETE");
#if defined(YTV_COBALT_BOOT_STAGE) && YTV_COBALT_BOOT_STAGE>=2
      // The official Cobalt ELF exports SbEventHandle at fixed offset
      // 0x00818f7c (confirmed in readelf). A standalone test wires it into
      // our real Vita lifecycle dispatcher, with no mock/simulated UI.
      extern _Bool ytv_vita_lifecycle_begin(void(*)(const void*));
      extern int ytv_starboard_pump_scheduled(int);
      typedef void (*CobaltEventHandler)(const void*);
      const uint32_t cobalt_event_offset=EXPECT_EVENT;
      check((cobalt_event_offset&~1u)>=xstart &&
            (cobalt_event_offset&~1u)<xstart+xlen,
            "verified official Cobalt SbEventHandle inside executable segment");
      if(failures)goto release;
      CobaltEventHandler cobalt_event=(CobaltEventHandler)(
                               uintptr_t)(base32+cobalt_event_offset);
      extern void ytv_runtime_once_trace_enable(void);
      ytv_runtime_once_trace_enable();
      {
        char reg[300];
        ytv_thread_cpu_report(reg,sizeof(reg),"main");  /* registers this thread */
        SceUID hb=sceKernelCreateThread("ytv_heartbeat",heartbeat_thread,
                                        0x10000100,0x4000,0,0,NULL);
        if(hb>=0)sceKernelStartThread(hb,0,NULL);
      }
      log_line("COBALT_EVENT_START_ENTER");
      int started=ytv_vita_lifecycle_begin(cobalt_event);
      log_line("COBALT_EVENT_START_RETURN");
      check(started,"official Cobalt SbEventHandle received native Starboard Start");
      if(failures)goto release;
      // Cobalt's main-thread message pump posts its work through
      // SbEventSchedule, so the scheduled-event queue must keep running.
      // 60 s of pumping (~3600 iterations) for the boot probe.
      unsigned dispatched=0;
      // Runs until the app is closed unless pump_seconds.txt limits it
      // (used by the automated Vita3K tests).
      unsigned limit_seconds=0;
      {
        int f=sceIoOpen("ux0:data/youtubevita-noads/pump_seconds.txt",SCE_O_RDONLY,0);
        if(f>=0){
          char v[16]={0};
          int n=sceIoRead(f,v,sizeof(v)-1);
          sceIoClose(f);
          unsigned secs=0;
          if(n>0 && sscanf(v,"%u",&secs)==1 && secs>0 && secs<=86400)
            limit_seconds=secs;
        }
      }
      extern void ytv_starboard_wait_scheduled(long long);
      extern int ytv_vita_input_pump(void);
      extern int ytv_vita_lifecycle_pump(int);
      extern _Bool ytv_vita_lifecycle_should_exit(int*);
      const SceUInt64 loop_start=sceKernelGetSystemTimeWide();
      SceUInt64 next_note=loop_start+30000000ull;
      for(;;){
        // Every due Cobalt task runs now (no per-frame cap: Cobalt's main
        // thread work, input handling and timers all arrive this way).
        int n=ytv_starboard_pump_scheduled(0);
        if(n>0)dispatched+=(unsigned)n;
        ytv_vita_input_pump();
        {
          extern int ytv_vita_network_poll(void);
          static SceUInt64 next_net=0;
          SceUInt64 tnow=sceKernelGetSystemTimeWide();
          if(tnow>=next_net){next_net=tnow+1000000ull;ytv_vita_network_poll();}
          extern void ytv_addons_tick(void);
          ytv_addons_tick();
          extern void ytv_osk_tick(void);
          ytv_osk_tick();
        }
        int code=0;
        if(ytv_vita_lifecycle_should_exit(&code)){
          note_u32("COBALT_LIFECYCLE_EXIT",(unsigned)code,dispatched);
          break;
        }
        ytv_vita_lifecycle_pump(1);
        SceUInt64 now=sceKernelGetSystemTimeWide();
        if(now>=next_note){
          note_u32("COBALT_EVENT_PUMP",(unsigned)((now-loop_start)/1000000ull),dispatched);
          next_note+=30000000ull;
        }
        if(limit_seconds && now-loop_start>=(SceUInt64)limit_seconds*1000000ull)break;
        // Sleep until the next task is due or a new one is posted; wake at
        // least every 8 ms to sample the controller and touch panel.
        ytv_starboard_wait_scheduled(8000);
      }
      log_line("COBALT_EVENT_PUMP_COMPLETE");
#endif

#else
      log_line("COBALT_CODE_EXECUTION_NOT_ATTEMPTED");
#endif

    }
  release:
    check(sceKernelFreeMemBlock(uid)==0,"temporary relocation memory released");
    image=NULL;
  }
done:
  if(fd>=0)sceIoClose(fd);
  {
    char msg[160];
    snprintf(msg,sizeof(msg),"RESULT %s (%d passed, %d failed)",
             failures?"FAIL":"PASS",passes,failures);
    log_line(msg);
  }
  sceKernelExitProcess(failures?1:0);
  return failures?1:0;
}

