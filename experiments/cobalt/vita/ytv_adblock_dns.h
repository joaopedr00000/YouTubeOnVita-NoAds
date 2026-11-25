#pragma once
#include <stddef.h>
#include <string.h>

// ASCII DNS matching, no allocation or dependency on locale.
static inline char ytv_dns_lower(char c) {
  return c >= 'A' && c <= 'Z' ? (char)(c + ('a' - 'A')) : c;
}
static inline bool ytv_dns_match(const char* name, size_t n, const char* suffix, bool subdomains) {
  const size_t m = strlen(suffix);
  if (n < m || (n > m && (!subdomains || name[n-m-1] != '.'))) return false;
  for (size_t i = 0; i < m; ++i)
    if (ytv_dns_lower(name[n-m+i]) != suffix[i]) return false;
  return true;
}
static inline bool ytv_known_ad_host(const char* name) {
  if (!name || !*name) return false;
  size_t n = strlen(name);
  if (n && name[n-1] == '.') --n;
  return ytv_dns_match(name,n,"doubleclick.net",true) ||
         ytv_dns_match(name,n,"googlesyndication.com",true) ||
         ytv_dns_match(name,n,"googleadservices.com",true) ||
         ytv_dns_match(name,n,"adservice.google.com",false) ||
         ytv_dns_match(name,n,"adservice.google.com.br",false);
}

// Only an exact, complete config line may change the default.
static inline int ytv_adblock_config_enabled(const char* p, size_t n) {
  if (!p) return 1;
  size_t start=0;
  int result=1;
  while (start<n) {
    size_t end=start;
    while(end<n && p[end]!='\n') ++end;
    size_t len=end-start;
    if(len && p[start+len-1]=='\r') --len;
    if(len==9 && !memcmp(p+start,"adblock=",8) &&
       (p[start+8]=='0'||p[start+8]=='1')) result=p[start+8]-'0';
    start=end<n?end+1:n;
  }
  return result;
}
