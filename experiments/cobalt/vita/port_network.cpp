/*
 * Starboard 15 sockets implemented with actual Vita SceNet.
 * IPv4 TCP/UDP, nonblocking sockets, DNS via Vita resolver.
 * No fake answers; failed or unsupported features return errors.
 */
#include "starboard/time.h"
#include "starboard/system.h"
#include <cstdio>
#include "starboard/log.h"
#include "starboard/socket.h"
#include "starboard/socket_waiter.h"
#include <psp2/net/net.h>
#include <psp2/net/netctl.h>
#include <psp2/sysmodule.h>
#include <pthread.h>
#include <malloc.h>
#include <cstdlib>
#include <cstring>
#include <strings.h>
#include <climits>

struct SbSocketPrivate {
  int descriptor;
  SbSocketError error;
  SbSocketProtocol protocol;
  bool connected;
  void* waiter;
};

namespace {
pthread_mutex_t net_init_mu=PTHREAD_MUTEX_INITIALIZER;
void* net_heap=nullptr;
bool net_ready=false;
uint64_t net_last_attempt_us=0;
bool ensure_net() {
  if(__atomic_load_n(&net_ready,__ATOMIC_ACQUIRE))return true;
  pthread_mutex_lock(&net_init_mu);
  if(net_ready){
    pthread_mutex_unlock(&net_init_mu);
    return true;
  }
  const uint64_t now=(uint64_t)SbTimeGetMonotonicNow();
  if(net_last_attempt_us && now>=net_last_attempt_us &&
     now-net_last_attempt_us<2000000ull){
    pthread_mutex_unlock(&net_init_mu);
    return false;
  }
  net_last_attempt_us=now?now:1;
  const int module=sceSysmoduleLoadModule(SCE_SYSMODULE_NET);
  (void)module; // The module can already be loaded, particularly on Vita3K.
  if(!net_heap)net_heap=memalign(4096,1024*1024);
  if(!net_heap){
    pthread_mutex_unlock(&net_init_mu);
    SbLogRaw("YTV_NETWORK_NET_INIT malloc_failed retryable=1\n");
    return false;
  }
  memset(net_heap,0,1024*1024);
  SceNetInitParam params={};
  params.memory=net_heap;
  params.size=1024*1024;
  params.flags=0;
  const int status=sceNetInit(&params);
  const bool available=status>=0;
  if(!available){
    free(net_heap);net_heap=nullptr;
  }
  __atomic_store_n(&net_ready,available,__ATOMIC_RELEASE);
  pthread_mutex_unlock(&net_init_mu);
  char line[100];
  snprintf(line,sizeof(line),"YTV_NETWORK_NET_INIT status=0x%x ready=%d retryable=%d\n",
           (unsigned)status,(int)available,available?0:1);
  SbLogRaw(line);
  return available;
}
int net_errno() {
  int *ptr=sceNetErrnoLoc();
  return ptr?*ptr:0;
}
SbSocketError decode_error(int ret) {
  if(ret>=0)return kSbSocketOk;
  int error=net_errno();
  if(error==SCE_NET_EWOULDBLOCK || error==SCE_NET_EINPROGRESS ||
     error==SCE_NET_EALREADY || error==SCE_NET_EAGAIN)
    return kSbSocketPending;
  if(error==SCE_NET_ECONNRESET || error==SCE_NET_EPIPE)
    return kSbSocketErrorConnectionReset;
  return kSbSocketErrorFailed;
}
SbSocketError update_error(SbSocket socket,int retval) {
  if(!socket)return kSbSocketErrorFailed;
  socket->error=decode_error(retval);
  if(socket->error==kSbSocketOk)socket->connected=true;
  return socket->error;
}
SbSocketAddressType get_family(SceNetSockaddrIn native) {
  (void)native; return kSbSocketAddressTypeIpv4;
}
bool as_sockaddr(const SbSocketAddress* address,SceNetSockaddrIn* out){
  if(!address || !out || address->type!=kSbSocketAddressTypeIpv4 ||
     address->port<0 || address->port>65535)return false;
  memset(out,0,sizeof(*out));
  out->sin_len=sizeof(*out);
  out->sin_family=SCE_NET_AF_INET;
  out->sin_port=sceNetHtons((unsigned short)address->port);
  memcpy(&out->sin_addr.s_addr,address->address,4);
  return true;
}
void from_sockaddr(const SceNetSockaddrIn* in,SbSocketAddress* out){
  memset(out,0,sizeof(*out));
  out->type=get_family(*in);
  out->port=sceNetNtohs(in->sin_port);
  memcpy(out->address,&in->sin_addr.s_addr,4);
}
SbSocket wrap_socket(int descriptor,SbSocketProtocol protocol) {
  SbSocket socket=(SbSocket)malloc(sizeof(SbSocketPrivate));
  if(!socket){sceNetSocketClose(descriptor);return kSbSocketInvalid;}
  socket->descriptor=descriptor;socket->protocol=protocol;
  socket->error=kSbSocketOk;socket->connected=false;socket->waiter=nullptr;
  return socket;
}
bool setsock(SbSocket socket,int level,int option,int value){
  return socket && sceNetSetsockopt(socket->descriptor,level,option,
                               &value,sizeof(value))>=0;
}
void* resolver_alloc(unsigned int size,int rid,const char* name,void* user){
  (void)rid;(void)name;(void)user;
  return malloc(size);
}
void resolver_free(void* ptr,int rid,const char* name,void* user){
  (void)rid;(void)name;(void)user;
  free(ptr);
}
} // namespace

extern "C" {
bool SbSocketIsIpv6Supported() { return false; }

SbSocket SbSocketCreate(SbSocketAddressType family,SbSocketProtocol protocol) {
  if(family!=kSbSocketAddressTypeIpv4 || !ensure_net())return kSbSocketInvalid;
  int type=protocol==kSbSocketProtocolTcp?SCE_NET_SOCK_STREAM:
           protocol==kSbSocketProtocolUdp?SCE_NET_SOCK_DGRAM:0;
  if(!type)return kSbSocketInvalid;
  int number=sceNetSocket("ytv-sb15",SCE_NET_AF_INET,type,0);
  if(number<0)return kSbSocketInvalid;
  int one=1;
  if(sceNetSetsockopt(number,SCE_NET_SOL_SOCKET,SCE_NET_SO_NBIO,
                      &one,sizeof(one))<0){
    sceNetSocketClose(number);
    return kSbSocketInvalid;
  }
  return wrap_socket(number,protocol);
}

bool SbSocketDestroy(SbSocket socket) {
  if(!socket)return false;
  if(socket->waiter){
    SbSocketWaiterRemove((SbSocketWaiter)socket->waiter,socket);
  }
  int result=sceNetSocketClose(socket->descriptor);
  {
    char line[100];
    snprintf(line,sizeof(line),"YTV_NET_CLOSE t=%lldms fd=%d result=0x%x\n",
             (long long)(SbTimeGetMonotonicNow()/1000),socket->descriptor,
             (unsigned)result);
    SbLogRaw(line);
  }
  free(socket);
  return result>=0;
}

SbSocketError SbSocketConnect(SbSocket socket,const SbSocketAddress* address) {
  if(!socket)return kSbSocketErrorFailed;
  SceNetSockaddrIn native={};
  if(!as_sockaddr(address,&native))return socket->error=kSbSocketErrorFailed;
  int status=sceNetConnect(socket->descriptor,(SceNetSockaddr*)&native,
                            sizeof(native));
  SbSocketError result=update_error(socket,status);
  if(result==kSbSocketPending)socket->connected=false;
  static volatile int n=0;
  if(__sync_add_and_fetch(&n,1)<=64){
    char line[160];
    const uint8_t* a=(const uint8_t*)&native.sin_addr;
    snprintf(line,sizeof(line),"YTV_NET_CONNECT t=%lldms %u.%u.%u.%u:%d status=0x%x result=%d\n",
             (long long)(SbTimeGetMonotonicNow()/1000),a[0],a[1],a[2],a[3],(int)address->port,(unsigned)status,(int)result);
    SbLogRaw(line);
  }
  return result;
}

SbSocketError SbSocketBind(SbSocket socket,const SbSocketAddress* address){
  if(!socket)return kSbSocketErrorFailed;
  SceNetSockaddrIn native={};
  if(!as_sockaddr(address,&native))return socket->error=kSbSocketErrorFailed;
  int status=sceNetBind(socket->descriptor,(SceNetSockaddr*)&native,sizeof(native));
  socket->error=decode_error(status);
  return socket->error;
}
SbSocketError SbSocketListen(SbSocket socket){
  if(!socket)return kSbSocketErrorFailed;
  socket->error=decode_error(sceNetListen(socket->descriptor,8));
  return socket->error;
}
SbSocket SbSocketAccept(SbSocket socket){
  if(!socket)return kSbSocketInvalid;
  SceNetSockaddrIn sockaddr={};
  unsigned length=sizeof(sockaddr);
  int descriptor=sceNetAccept(socket->descriptor,(SceNetSockaddr*)&sockaddr,
                               &length);
  if(descriptor<0) {
    socket->error=decode_error(descriptor);
    return kSbSocketInvalid;
  }
  int one=1;
  if(sceNetSetsockopt(descriptor,SCE_NET_SOL_SOCKET,SCE_NET_SO_NBIO,
                      &one,sizeof(one))<0){
    sceNetSocketClose(descriptor);return kSbSocketInvalid;
  }
  SbSocket result=wrap_socket(descriptor,socket->protocol);
  if(result)result->connected=true;
  return result;
}
bool SbSocketIsConnected(SbSocket socket){
  if(!socket)return false;
  if(socket->connected)return true;
  SceNetSockaddrIn peer={};
  unsigned length=sizeof(peer);
  if(sceNetGetpeername(socket->descriptor,(SceNetSockaddr*)&peer,&length)>=0) {
    socket->connected=true;
    return true;
  }
  return false;
}
bool SbSocketIsConnectedAndIdle(SbSocket socket) {
  if(!socket || socket->protocol!=kSbSocketProtocolTcp ||
     !SbSocketIsConnected(socket))return false;
  char next=0;
  int n=sceNetRecv(socket->descriptor,&next,1,
                   SCE_NET_MSG_PEEK|SCE_NET_MSG_DONTWAIT);
  if(n==0){socket->connected=false;return false;} // remote orderly shutdown
  if(n>0)return false; // unread data: socket is NOT idle
  int code=net_errno();
  return code==SCE_NET_EWOULDBLOCK || code==SCE_NET_EAGAIN;
}
SbSocketError SbSocketGetLastError(SbSocket socket) {
  return socket?socket->error:kSbSocketErrorFailed;
}
bool SbSocketClearLastError(SbSocket socket) {
  if(!socket)return false;
  socket->error=kSbSocketOk;
  return true;
}
bool SbSocketGetLocalAddress(SbSocket socket,SbSocketAddress* out) {
  if(!socket || !out)return false;
  SceNetSockaddrIn native={};
  unsigned size=sizeof(native);
  if(sceNetGetsockname(socket->descriptor,(SceNetSockaddr*)&native,&size)<0)
    return false;
  from_sockaddr(&native,out);
  return true;
}
int SbSocketReceiveFrom(SbSocket socket,char* out_data,int data_size,
                        SbSocketAddress* out_source) {
  if(!socket || !out_data || data_size<0)return -1;
  SceNetSockaddrIn native={};
  unsigned length=sizeof(native);
  int result=sceNetRecvfrom(socket->descriptor,out_data,data_size,
            SCE_NET_MSG_DONTWAIT,
            out_source?(SceNetSockaddr*)&native:nullptr,
            out_source?&length:nullptr);
  socket->error=decode_error(result);
  if(result>=0 && out_source)from_sockaddr(&native,out_source);
  static volatile int n=0;
  if(__sync_add_and_fetch(&n,1)<=80){
    char line[120];
    snprintf(line,sizeof(line),"YTV_NET_RECV t=%lldms fd=%d want=%d got=%d err=%d\n",
             (long long)(SbTimeGetMonotonicNow()/1000),socket->descriptor,data_size,result,(int)socket->error);
    SbLogRaw(line);
  }
  return result;
}
int SbSocketSendTo(SbSocket socket,const char* data,int size,
                   const SbSocketAddress* destination) {
  if(!socket || !data || size<0)return -1;
  SceNetSockaddrIn native={};
  if(destination && !as_sockaddr(destination,&native)) {
    socket->error=kSbSocketErrorFailed;return -1;
  }
  int result=destination?
    sceNetSendto(socket->descriptor,data,size,SCE_NET_MSG_DONTWAIT,
              (SceNetSockaddr*)&native,sizeof(native)):
    sceNetSend(socket->descriptor,data,size,SCE_NET_MSG_DONTWAIT);
  socket->error=decode_error(result);
  static volatile int n=0;
  if(__sync_add_and_fetch(&n,1)<=80){
    char line[120];
    snprintf(line,sizeof(line),"YTV_NET_SEND t=%lldms fd=%d size=%d sent=%d err=%d\n",
             (long long)(SbTimeGetMonotonicNow()/1000),socket->descriptor,size,result,(int)socket->error);
    SbLogRaw(line);
  }
  return result;
}
bool SbSocketSetBroadcast(SbSocket socket,bool value) {
  return setsock(socket,SCE_NET_SOL_SOCKET,SCE_NET_SO_BROADCAST,value?1:0);
}
bool SbSocketSetReuseAddress(SbSocket socket,bool value) {
  return setsock(socket,SCE_NET_SOL_SOCKET,SCE_NET_SO_REUSEADDR,value?1:0);
}
bool SbSocketSetReceiveBufferSize(SbSocket socket,int32_t size){
  return size>0 && setsock(socket,SCE_NET_SOL_SOCKET,SCE_NET_SO_RCVBUF,size);
}
bool SbSocketSetSendBufferSize(SbSocket socket,int32_t size){
  return size>0 && setsock(socket,SCE_NET_SOL_SOCKET,SCE_NET_SO_SNDBUF,size);
}
bool SbSocketSetTcpNoDelay(SbSocket socket,bool value) {
  return socket && socket->protocol==kSbSocketProtocolTcp &&
         setsock(socket,SCE_NET_IPPROTO_TCP,SCE_NET_TCP_NODELAY,value?1:0);
}
bool SbSocketSetTcpKeepAlive(SbSocket socket,bool value,int64_t period) {
  // SceNet does not expose TCP_KEEPIDLE / interval. Do not falsely claim
  // support if caller requires a specific keepalive cadence.
  if(value && period>0)return false;
  return socket && socket->protocol==kSbSocketProtocolTcp &&
         setsock(socket,SCE_NET_SOL_SOCKET,SCE_NET_SO_KEEPALIVE,value?1:0);
}
bool SbSocketSetTcpWindowScaling(SbSocket socket,bool value){
  (void)socket;(void)value;return false; // unsupported by Vita SceNet
}
bool SbSocketJoinMulticastGroup(SbSocket socket,const SbSocketAddress* address){
  if(!socket || socket->protocol!=kSbSocketProtocolUdp ||
     !address || address->type!=kSbSocketAddressTypeIpv4 ||
     address->address[0]<224 || address->address[0]>239)return false;
  SceNetIpMreq member={};
  memcpy(&member.imr_multiaddr.s_addr,address->address,4);
  member.imr_interface.s_addr=0; // INADDR_ANY: system chooses interface
  return sceNetSetsockopt(socket->descriptor,SCE_NET_IPPROTO_IP,
                         SCE_NET_IP_ADD_MEMBERSHIP,&member,sizeof(member))>=0;
}
bool SbSocketGetInterfaceAddress(const SbSocketAddress* destination,
   SbSocketAddress* source,SbSocketAddress* netmask){
  if(!destination || !source || destination->type!=kSbSocketAddressTypeIpv4)
    return false;
  SbSocketAddress src={};SbSocketAddress mask={};
  src.type=kSbSocketAddressTypeIpv4;
  mask.type=kSbSocketAddressTypeIpv4;
  if(destination->address[0]==127){
    src.address[0]=127;src.address[3]=1;
    mask.address[0]=255;
  }else{
    int state=SCE_NETCTL_STATE_DISCONNECTED;
    if(sceNetCtlInit()<0 ||
       sceNetCtlInetGetState(&state)<0 ||
       state!=SCE_NETCTL_STATE_CONNECTED)return false;
    SceNetCtlInfo info={};
    if(sceNetCtlInetGetInfo(SCE_NETCTL_INFO_GET_IP_ADDRESS,&info)<0 ||
       sceNetInetPton(SCE_NET_AF_INET,info.ip_address,src.address)!=1)
      return false;
    if(netmask){
      if(sceNetCtlInetGetInfo(SCE_NETCTL_INFO_GET_NETMASK,&info)<0 ||
         sceNetInetPton(SCE_NET_AF_INET,info.netmask,mask.address)!=1)
        return false;
    }
  }
  *source=src;
  if(netmask)*netmask=mask;
  return true;
}
// Conservative host-only filter. Does NOT block in-player ads sent from
// first-party youtube.com/googlevideo.com endpoints. Never block those hosts.
extern "C" int g_ytv_adblock_enabled;
static bool ytv_known_ad_host(const char* name) {
  if (!name) return false;
  // Label-boundary suffix match; never block shared media/API hosts.
  size_t len=strlen(name);
  if(len && name[len-1]=='.') --len;
  const char* hosts[] = {
      "doubleclick.net", "googlesyndication.com", "googleadservices.com",
      "adservice.google.com", "adservice.google.com.br",
  };
  for (const char* h : hosts) {
    const size_t n=strlen(h);
    if(len>=n && !strncasecmp(name+len-n,h,n) &&
       (len==n || name[len-n-1]=='.')) return true;
  }
  return false;
}

static SbSocketResolution* resolve_impl(const char* hostname,int filters) {
  if(!hostname || !*hostname || strlen(hostname)>253 ||
     ((filters&kSbSocketResolveFilterIpv6) &&
      !(filters&kSbSocketResolveFilterIpv4)))return nullptr;
  if(__atomic_load_n(&g_ytv_adblock_enabled, __ATOMIC_RELAXED) &&
     ytv_known_ad_host(hostname)) {
    static volatile int count = 0;
    if (__sync_fetch_and_add(&count, 1) < 32) {
      char line[200];
      snprintf(line, sizeof(line), "YTV_ADBLOCK blocked_host=%s\n", hostname);
      SbLogRaw(line);
    }
    return nullptr;
  }
#if defined(YTV_NATIVE_DOWNLOAD_TEST)
  if(SbSystemNetworkIsDisconnected()){
    static volatile unsigned count=0;
    if(__sync_fetch_and_add(&count,1)<12)
      SbLogRaw("YTV_NETWORK_DIAGNOSTIC_DNS_BLOCKED offline=1\n");
    return nullptr;
  }
#endif
  if(!ensure_net())return nullptr;
  SceNetInAddr address={};
  if(sceNetInetPton(SCE_NET_AF_INET,hostname,&address.s_addr)!=1) {
    SceNetResolverParam options={};
    options.allocate=resolver_alloc;
    options.free=resolver_free;
    int resolver=sceNetResolverCreate("ytv-dns",&options,0);
    if(resolver<0)return nullptr;
    int result=sceNetResolverStartNtoa(resolver,hostname,&address,2000000,1,0);
    sceNetResolverDestroy(resolver);
    if(result<0)return nullptr;
  }
  SbSocketResolution* response=
     (SbSocketResolution*)malloc(sizeof(SbSocketResolution));
  if(!response)return nullptr;
  response->addresses=(SbSocketAddress*)calloc(1,sizeof(SbSocketAddress));
  if(!response->addresses){free(response);return nullptr;}
  response->address_count=1;
  response->addresses[0].type=kSbSocketAddressTypeIpv4;
  response->addresses[0].port=0;
  memcpy(response->addresses[0].address,&address.s_addr,4);
  return response;
}
// Logged: which hosts Cobalt resolves and with what result (diagnostics).
SbSocketResolution* SbSocketResolve(const char* hostname,int filters) {
  SbSocketResolution* r=resolve_impl(hostname,filters);
  static volatile int n=0;
  if(__sync_add_and_fetch(&n,1)<=64){
    char line[200];
    if(r && r->address_count>0){
      const uint8_t* a=r->addresses[0].address;
      snprintf(line,sizeof(line),"YTV_NET_RESOLVE %s filters=%d -> %u.%u.%u.%u\n",
               hostname?hostname:"(null)",filters,a[0],a[1],a[2],a[3]);
    }else{
      snprintf(line,sizeof(line),"YTV_NET_RESOLVE %s filters=%d -> FAILED\n",
               hostname?hostname:"(null)",filters);
    }
    SbLogRaw(line);
  }
  return r;
}
void SbSocketFreeResolution(SbSocketResolution* result){
  if(!result)return;
  free(result->addresses);
  free(result);
}
} // extern C


// Private bridge methods; not exported as Starboard symbols.
extern "C" bool ytv_socket_net_ensure(void){return ensure_net();}
extern "C" int ytv_socket_native_fd(SbSocket socket){
  return socket?socket->descriptor:-1;
}
extern "C" bool ytv_socket_waiter_claim(SbSocket socket,void* waiter){
  if(!socket || !waiter || socket->waiter)return false;
  socket->waiter=waiter;return true;
}
extern "C" void ytv_socket_waiter_release(SbSocket socket,void* waiter){
  if(socket && socket->waiter==waiter)socket->waiter=nullptr;
}
