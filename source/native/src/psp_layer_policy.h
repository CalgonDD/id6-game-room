#ifndef PSP_PROBE_POLICY_H
#define PSP_PROBE_POLICY_H
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#define PROBE_SURFACE 1302u
#define PROBE_PORT 42894
#define PROBE_MAX_ORDER 64
/* Strictly bounded: never truncate a layer order then publish the partial list. */
static int probe_parse_order(const char *text,unsigned *ids,unsigned cap) {
 const char *p=strstr(text,"surface render order:"); unsigned n=0;
 if(!p) return -1;
 p+=strlen("surface render order:");
 while(*p && *p!='\n' && *p!='\r') {
  char *end; unsigned long v;
  if(!isdigit((unsigned char)*p)) {++p;continue;}
  errno=0;v=strtoul(p,&end,0);
  if(end==p || errno==ERANGE || v>0xfffffffful || n==cap) return -1;
  ids[n++]=(unsigned)v;p=end;
  if(*p=='(') {while(*p && *p!=')' && *p!='\n') ++p;if(*p==')')++p;}
 }
 return (int)n;
}
static int probe_order_contains(const unsigned *ids,int n,unsigned id) {
 int i;for(i=0;i<n;++i)if(ids[i]==id)return 1;return 0;
}
static int probe_without_ours(unsigned *ids,int n) {
 int i,m=0;for(i=0;i<n;++i)if(ids[i]!=PROBE_SURFACE)ids[m++]=ids[i];return m;
}
static int probe_format_order(const unsigned *ids,int n,char *text,size_t cap) {
 int i;size_t used=0;if(!cap)return 0;text[0]=0;
 for(i=0;i<n;++i) {int rc=snprintf(text+used,cap-used,"%s0x%x",i?",":"",ids[i]);if(rc<0 || (size_t)rc>=cap-used)return 0;used+=(size_t)rc;}return 1;
}
#endif
