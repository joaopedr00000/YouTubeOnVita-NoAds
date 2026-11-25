// c++ -std=c++11 -Wall -Wextra -pedantic tools/test_adblock_dns.cpp -o /tmp/ytv-adblock-test && /tmp/ytv-adblock-test
#include "../experiments/cobalt/vita/ytv_adblock_dns.h"
#include <cassert>
#include <initializer_list>
int main(){
  for(const char* h: {"doubleclick.net","GOOGLEADS.G.DOUBLECLICK.NET.","pagead2.googlesyndication.com","www.googleadservices.com","adservice.google.com.br"}) assert(ytv_known_ad_host(h));
  for(const char* h: {"youtube.com","www.youtube.com","r1.googlevideo.com","accounts.google.com","i.ytimg.com","notdoubleclick.net","doubleclick.net.example","foo.adservice.google.com",""}) assert(!ytv_known_ad_host(h));
  assert(!ytv_known_ad_host(nullptr));
  const char* off="lr=1\r\nadblock=0\r\nquality=480\r\n";
  assert(ytv_adblock_config_enabled(off,strlen(off))==0);
  const char* on="adblock=0\nadblock=1\n";
  assert(ytv_adblock_config_enabled(on,strlen(on))==1);
  const char* invalid="notadblock=0\nadblock=00\n";
  assert(ytv_adblock_config_enabled(invalid,strlen(invalid))==1);
  assert(ytv_adblock_config_enabled(nullptr,0)==1);
}
