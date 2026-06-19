#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <ctype.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <sys/stat.h>
#include <dirent.h>
#include <time.h>

typedef enum { TN, TS, TB, TL, TM, TX } Tag;
typedef struct Value Value;
typedef struct { Value* items; long len, cap; int perm; } List;
typedef struct { char** keys; Value* vals; long len, cap; int perm; } Map;
struct Value { Tag t; double n; char* s; List* l; Map* m; };

/* ===== Ebb GC: track allocations during a request, free on ebb, promote escapes ===== */
static void** g_rec=0; static long g_reclen=0,g_reccap=0; static int g_in_req=0;
static void g_record(void* p){ if(g_reclen>=g_reccap){ g_reccap=g_reccap?g_reccap*2:2048; g_rec=realloc(g_rec,g_reccap*sizeof(void*)); } g_rec[g_reclen++]=p; }
static void* galloc(long n){ void* p=malloc(n); if(g_in_req) g_record(p); return p; }
static char* gstrdup(const char* s){ if(!s)s=""; long n=strlen(s)+1; char* r=galloc(n); memcpy(r,s,n); return r; }
static void* grealloc(void* old,long n){ void* p=realloc(old,n); if(p!=old){ for(long i=g_reclen-1;i>=0;i--) if(g_rec[i]==old){ g_rec[i]=p; break; } } return p; }
static void g_unrec(void* p){ for(long i=g_reclen-1;i>=0;i--) if(g_rec[i]==p){ g_rec[i]=g_rec[--g_reclen]; return; } }
static void ebb(void){ for(long i=0;i<g_reclen;i++) free(g_rec[i]); g_reclen=0; }
static void pin(Value v){ if(v.t==TS){ if(v.s) g_unrec(v.s); return; } if(v.t==TL){ if(v.l->perm) return; g_unrec(v.l); g_unrec(v.l->items); v.l->perm=1; for(long i=0;i<v.l->len;i++) pin(v.l->items[i]); return; } if(v.t==TM){ if(v.m->perm) return; g_unrec(v.m); g_unrec(v.m->keys); g_unrec(v.m->vals); v.m->perm=1; for(long i=0;i<v.m->len;i++){ g_unrec(v.m->keys[i]); pin(v.m->vals[i]); } return; } }

static int g_argc=0; static char** g_argv=0;

static Value NUM(double n){ Value v; v.t=TN; v.n=n; v.s=0; v.l=0; v.m=0; return v; }
static Value BOOLV(int b){ Value v=NUM(b?1:0); v.t=TB; return v; }
static Value NIL(void){ Value v=NUM(0); v.t=TX; return v; }
static Value STR(const char* s){ Value v; v.t=TS; v.s=gstrdup(s?s:""); v.n=0; v.l=0; v.m=0; return v; }
static char* numstr(double d){ char* b=galloc(40); if(d==(long)d) sprintf(b,"%ld",(long)d); else sprintf(b,"%g",d); return b; }
static char* tostr(Value v){
  if(v.t==TS) return v.s;
  if(v.t==TN) return numstr(v.n);
  if(v.t==TB) return v.n!=0?"yes":"no";
  if(v.t==TX) return "nothing";
  if(v.t==TL){ long cap=256,len=1; char* o=galloc(cap); o[0]='['; for(long i=0;i<v.l->len;i++){ char* p=tostr(v.l->items[i]); long lp=strlen(p); long need=len+lp+4; if(need>cap){ cap=need*2; o=grealloc(o,cap);} if(i){ o[len++]=','; o[len++]=' '; } memcpy(o+len,p,lp); len+=lp; } o[len++]=']'; o[len]=0; return o; }
  if(v.t==TM){ long cap=256,len=1; char* o=galloc(cap); o[0]='{'; for(long i=0;i<v.m->len;i++){ char* k=v.m->keys[i]; char* p=tostr(v.m->vals[i]); long lk=strlen(k),lp=strlen(p); long need=len+lk+lp+6; if(need>cap){ cap=need*2; o=grealloc(o,cap);} if(i){ o[len++]=','; o[len++]=' '; } memcpy(o+len,k,lk); len+=lk; o[len++]=':'; o[len++]=' '; memcpy(o+len,p,lp); len+=lp; } o[len++]='}'; o[len]=0; return o; }
  return "";
}
static int truthy(Value v){ if(v.t==TX) return 0; if(v.t==TB) return v.n!=0; return 1; }
static int veq(Value a, Value b){ if((a.t==TN||a.t==TB)&&(b.t==TN||b.t==TB)) return a.n==b.n; if(a.t!=b.t) return 0; if(a.t==TS) return strcmp(a.s,b.s)==0; if(a.t==TX) return 1; return 0; }
static Value ADD(Value a, Value b){ if(a.t==TN&&b.t==TN) return NUM(a.n+b.n); char* x=tostr(a); char* y=tostr(b); char* r=malloc(strlen(x)+strlen(y)+1); strcpy(r,x); strcat(r,y); Value v=STR(r); free(r); return v; }
static Value SUB(Value a,Value b){ return NUM(a.n-b.n); }
static Value MUL(Value a,Value b){ return NUM(a.n*b.n); }
static Value DIVV(Value a,Value b){ return NUM(a.n/b.n); }
static Value NEG(Value a){ return NUM(-a.n); }
static Value EQ(Value a,Value b){ return BOOLV(veq(a,b)); }
static Value NE(Value a,Value b){ return BOOLV(!veq(a,b)); }
static Value LT(Value a,Value b){ if(a.t==TS&&b.t==TS) return BOOLV(strcmp(a.s,b.s)<0); return BOOLV(a.n<b.n); }
static Value GT(Value a,Value b){ if(a.t==TS&&b.t==TS) return BOOLV(strcmp(a.s,b.s)>0); return BOOLV(a.n>b.n); }
static Value LE(Value a,Value b){ if(a.t==TS&&b.t==TS) return BOOLV(strcmp(a.s,b.s)<=0); return BOOLV(a.n<=b.n); }
static Value GE(Value a,Value b){ if(a.t==TS&&b.t==TS) return BOOLV(strcmp(a.s,b.s)>=0); return BOOLV(a.n>=b.n); }
static Value ANDV(Value a,Value b){ return BOOLV(truthy(a)&&truthy(b)); }
static Value ORV(Value a,Value b){ return BOOLV(truthy(a)||truthy(b)); }
static Value NOTV(Value a){ return BOOLV(!truthy(a)); }
static List* newlist(void){ List* l=galloc(sizeof(List)); l->len=0; l->cap=8; l->items=galloc(sizeof(Value)*8); l->perm=!g_in_req; return l; }
static Value LIST0(void){ Value v; v.t=TL; v.l=newlist(); v.s=0; v.m=0; v.n=0; return v; }
static void listpush(Value lv, Value x){ List* l=lv.l; if(l->perm) pin(x); if(l->len>=l->cap){ l->cap*=2; l->items=grealloc(l->items,sizeof(Value)*l->cap);} l->items[l->len++]=x; }
static Value MKLIST(int n, ...){ Value v=LIST0(); va_list ap; va_start(ap,n); for(int i=0;i<n;i++) listpush(v, va_arg(ap,Value)); va_end(ap); return v; }
static Map* newmap(void){ Map* m=galloc(sizeof(Map)); m->len=0; m->cap=8; m->keys=galloc(sizeof(char*)*8); m->vals=galloc(sizeof(Value)*8); m->perm=!g_in_req; return m; }
static Value MAP0(void){ Value v; v.t=TM; v.m=newmap(); v.s=0; v.l=0; v.n=0; return v; }
static void mapset(Value mv, Value k, Value val){ Map* m=mv.m; char* key=tostr(k); if(m->perm) pin(val); for(long i=0;i<m->len;i++) if(strcmp(m->keys[i],key)==0){ m->vals[i]=val; return; } if(m->len>=m->cap){ m->cap*=2; m->keys=grealloc(m->keys,sizeof(char*)*m->cap); m->vals=grealloc(m->vals,sizeof(Value)*m->cap);} m->keys[m->len]=(m->perm?strdup(key):gstrdup(key)); m->vals[m->len]=val; m->len++; }
static Value MKMAP(int n, ...){ Value v=MAP0(); va_list ap; va_start(ap,n); for(int i=0;i<n;i++){ Value k=va_arg(ap,Value); Value val=va_arg(ap,Value); mapset(v,k,val);} va_end(ap); return v; }
static Value INDEX(Value c, Value k){
  if(c.t==TL){ long i=(long)k.n; if(i<0)i+=c.l->len; if(i<0||i>=c.l->len) return NIL(); return c.l->items[i]; }
  if(c.t==TM){ char* key=tostr(k); for(long i=0;i<c.m->len;i++) if(strcmp(c.m->keys[i],key)==0) return c.m->vals[i]; return NIL(); }
  if(c.t==TS){ long L=strlen(c.s); long i=(long)k.n; if(i<0)i+=L; if(i<0||i>=L) return STR(""); char b[2]={c.s[i],0}; return STR(b); }
  return NIL();
}
static void SETAT(Value c, Value k, Value val){ if(c.t==TL){ long i=(long)k.n; if(i>=0&&i<c.l->len) c.l->items[i]=val; } else if(c.t==TM) mapset(c,k,val); }
static Value SLICE(Value c, Value a, Value b){ long lo=(long)a.n, hi=(long)b.n;
  if(c.t==TS){ long L=strlen(c.s); if(lo<0)lo+=L; if(hi<0)hi+=L; if(lo<0)lo=0; if(hi>L)hi=L; if(hi<lo)hi=lo; char* r=malloc(hi-lo+1); memcpy(r,c.s+lo,hi-lo); r[hi-lo]=0; Value v=STR(r); free(r); return v; }
  if(c.t==TL){ Value v=LIST0(); long L=c.l->len; if(lo<0)lo+=L; if(hi<0)hi+=L; if(lo<0)lo=0; if(hi>L)hi=L; for(long i=lo;i<hi;i++) listpush(v,c.l->items[i]); return v; }
  return NIL();
}
static Value LEN(Value v){ if(v.t==TS) return NUM(strlen(v.s)); if(v.t==TL) return NUM(v.l->len); if(v.t==TM) return NUM(v.m->len); return NUM(0); }
static Value INOP(Value a, Value b){ if(b.t==TS&&a.t==TS) return BOOLV(strstr(b.s,a.s)!=0); if(b.t==TL){ for(long i=0;i<b.l->len;i++) if(veq(a,b.l->items[i])) return BOOLV(1); return BOOLV(0);} if(b.t==TM){ char* key=tostr(a); for(long i=0;i<b.m->len;i++) if(strcmp(b.m->keys[i],key)==0) return BOOLV(1); return BOOLV(0);} return BOOLV(0); }
static void SAY(Value v){ printf("%s\n", tostr(v)); }
static Value B_text(Value a){ return STR(tostr(a)); }
static Value B_length(Value a){ return LEN(a); }
static Value B_keys(Value m){ Value v=LIST0(); if(m.t==TM) for(long i=0;i<m.m->len;i++) listpush(v,STR(m.m->keys[i])); return v; }
static Value B_values(Value m){ Value v=LIST0(); if(m.t==TM) for(long i=0;i<m.m->len;i++) listpush(v,m.m->vals[i]); return v; }
static Value B_range(Value a, Value b, int two){ Value v=LIST0(); long lo=two?(long)a.n:0, hi=two?(long)b.n:(long)a.n; for(long i=lo;i<hi;i++) listpush(v,NUM(i)); return v; }
static Value B_upper(Value a){ char* s=gstrdup(tostr(a)); for(char* p=s;*p;p++)*p=toupper((unsigned char)*p); return STR(s); }
static Value B_lower(Value a){ char* s=gstrdup(tostr(a)); for(char* p=s;*p;p++)*p=tolower((unsigned char)*p); return STR(s); }
static Value B_trim(Value a){ char* s=tostr(a); while(*s==' '||*s=='\t'||*s=='\n')s++; long e=strlen(s); while(e>0&&(s[e-1]==' '||s[e-1]=='\t'||s[e-1]=='\n'))e--; char* r=malloc(e+1); memcpy(r,s,e); r[e]=0; return STR(r); }
static Value B_number(Value a){ if(a.t==TN) return a; return NUM(atof(tostr(a))); }
static Value B_join(Value lst, Value sep){ if(lst.t!=TL) return STR(""); char* d=tostr(sep); long cap=8192; char* o=malloc(cap); o[0]=0; long ln=0; for(long i=0;i<lst.l->len;i++){ char* piece=tostr(lst.l->items[i]); long need=ln+strlen(piece)+strlen(d)+1; if(need>cap){ cap=need*2; o=realloc(o,cap);} if(i){ strcat(o,d);} strcat(o,piece); ln=strlen(o);} return STR(o); }
static Value B_split(Value a, Value sepv){ Value v=LIST0(); char* s=tostr(a); char* sep=tostr(sepv); long sl=strlen(sep); if(sl==0){ for(long i=0;s[i];i++){ char b[2]={s[i],0}; listpush(v,STR(b)); } return v; } char* p=s; char* q; while((q=strstr(p,sep))){ long n=q-p; char* r=malloc(n+1); memcpy(r,p,n); r[n]=0; listpush(v,STR(r)); free(r); p=q+sl; } listpush(v,STR(p)); return v; }
static Value B_sort(Value lst){ if(lst.t!=TL) return lst; Value v=LIST0(); for(long i=0;i<lst.l->len;i++) listpush(v,lst.l->items[i]); for(long i=1;i<v.l->len;i++){ Value key=v.l->items[i]; long j=i-1; while(j>=0 && truthy(GT(v.l->items[j],key))){ v.l->items[j+1]=v.l->items[j]; j--; } v.l->items[j+1]=key; } return v; }
static Value B_contains(Value a, Value b){ return INOP(b,a); }
static Value B_slice(Value c, Value a, Value b){ return SLICE(c,a,b); }
static Value B_replace(Value s, Value oldv, Value newv){ char* str=tostr(s); char* o=tostr(oldv); char* nw=tostr(newv); long ol=strlen(o); long nl=strlen(nw); if(ol==0) return STR(str); long cnt=0; { char* p=str; char* q; while((q=strstr(p,o))){ cnt++; p=q+ol; } } long outlen=strlen(str)+cnt*(nl-ol)+1; char* out=malloc(outlen>0?outlen:1); char* w=out; char* p=str; char* q; while((q=strstr(p,o))){ long pre=q-p; memcpy(w,p,pre); w+=pre; memcpy(w,nw,nl); w+=nl; p=q+ol; } strcpy(w,p); Value v=STR(out); free(out); return v; }
static Value B_read_file(Value pth){ FILE* f=fopen(tostr(pth),"rb"); if(!f) return STR(""); fseek(f,0,SEEK_END); long n=ftell(f); fseek(f,0,SEEK_SET); char* b=malloc(n+1); fread(b,1,n,f); b[n]=0; fclose(f); Value v=STR(b); free(b); return v; }
static Value B_write_file(Value pth, Value c){ FILE* f=fopen(tostr(pth),"wb"); if(f){ char* s=tostr(c); fwrite(s,1,strlen(s),f); fclose(f);} return NIL(); }
static Value B_append_file(Value pth, Value c){ FILE* f=fopen(tostr(pth),"ab"); if(f){ char* s=tostr(c); fwrite(s,1,strlen(s),f); fclose(f);} return NIL(); }
static Value B_arguments(void){ Value v=LIST0(); for(int i=1;i<g_argc;i++) listpush(v,STR(g_argv[i])); return v; }
static char* readpipe(FILE* p){ long cap=4096,len=0; char* b=malloc(cap); int ch; while((ch=fgetc(p))!=EOF){ if(len+1>=cap){cap*=2;b=realloc(b,cap);} b[len++]=ch; } b[len]=0; return b; }
static Value B_run(Value cmd){ char* full=malloc(strlen(tostr(cmd))+8); sprintf(full,"%s 2>&1",tostr(cmd)); FILE* p=popen(full,"r"); free(full); if(!p) return STR(""); char* b=readpipe(p); pclose(p); long len=strlen(b); while(len>0&&b[len-1]=='\n') b[--len]=0; Value v=STR(b); free(b); return v; }
static Value B_shell(Value cmd){ char* full=malloc(strlen(tostr(cmd))+8); sprintf(full,"%s 2>&1",tostr(cmd)); FILE* p=popen(full,"r"); free(full); if(!p) return MKMAP(2,STR("output"),STR(""),STR("code"),NUM(1)); char* b=readpipe(p); int st=pclose(p); int code=(st==-1)?1:(st>>8); long len=strlen(b); while(len>0&&b[len-1]=='\n') b[--len]=0; Value v=MKMAP(2,STR("output"),STR(b),STR("code"),NUM(code)); free(b); return v; }
static int b64v(char c){ if(c>='A'&&c<='Z')return c-'A'; if(c>='a'&&c<='z')return c-'a'+26; if(c>='0'&&c<='9')return c-'0'+52; if(c=='+')return 62; if(c=='/')return 63; return -1; }
static Value B_b64decode(Value sv){ char* in=tostr(sv); long n=strlen(in); char* out=malloc(n+1); long o=0; int buf=0,bits=0; for(long i=0;i<n;i++){ int v=b64v(in[i]); if(v<0) continue; buf=(buf<<6)|v; bits+=6; if(bits>=8){ bits-=8; out[o++]=(char)((buf>>bits)&0xFF); } } out[o]=0; Value r=STR(out); free(out); return r; }
static char* sdup(const char* s){ char* r=malloc(strlen(s)+1); strcpy(r,s); return r; }
static Value B_url_decode(Value v){ char* s=tostr(v); long n=strlen(s); char* o=malloc(n+1); long j=0; for(long i=0;i<n;i++){ if(s[i]=='%'&&i+2<n){ char h=s[i+1],l=s[i+2]; int hi=(h<='9')?h-'0':(tolower(h)-'a'+10); int lo=(l<='9')?l-'0':(tolower(l)-'a'+10); o[j++]=(char)(hi*16+lo); i+=2; } else if(s[i]=='+') o[j++]=' '; else o[j++]=s[i]; } o[j]=0; Value r=STR(o); free(o); return r; }
static Value B_url_encode(Value v){ char* s=tostr(v); long n=strlen(s); char* o=malloc(n*3+1); long j=0; for(long i=0;i<n;i++){ unsigned char c=s[i]; if((c>='A'&&c<='Z')||(c>='a'&&c<='z')||(c>='0'&&c<='9')||c=='-'||c=='_'||c=='.'||c=='~') o[j++]=c; else { sprintf(o+j,"%%%02X",c); j+=3; } } o[j]=0; Value r=STR(o); free(o); return r; }
static Value B_html_escape(Value v){ char* s=tostr(v); char* o=malloc(strlen(s)*6+1); char* w=o; for(char* p=s;*p;p++){ if(*p=='<'){strcpy(w,"&lt;");w+=4;} else if(*p=='>'){strcpy(w,"&gt;");w+=4;} else if(*p=='&'){strcpy(w,"&amp;");w+=5;} else if(*p=='"'){strcpy(w,"&quot;");w+=6;} else *w++=*p; } *w=0; Value r=STR(o); free(o); return r; }
static void json_str(char** out,long* cap,long* len,const char* s){ long need=*len+strlen(s)*6+4; if(need>*cap){*cap=need*2;*out=realloc(*out,*cap);} char* w=*out+*len; *w++='"'; for(const char* p=s;*p;p++){ unsigned char c=*p; if(c=='"'){*w++='\\';*w++='"';} else if(c=='\\'){*w++='\\';*w++='\\';} else if(c=='\n'){*w++='\\';*w++='n';} else if(c=='\t'){*w++='\\';*w++='t';} else if(c=='\r'){*w++='\\';*w++='r';} else if(c<0x20){sprintf(w,"\\u%04x",c);w+=6;} else *w++=c; } *w++='"'; *w=0; *len=w-*out; }
static void to_json_rec(Value v,char** out,long* cap,long* len){ if(*len+64>*cap){*cap=(*len+64)*2;*out=realloc(*out,*cap);} if(v.t==TS){json_str(out,cap,len,v.s);return;} if(v.t==TN){char* ns=numstr(v.n);strcpy(*out+*len,ns);*len+=strlen(ns);free(ns);return;} if(v.t==TB){const char* b=v.n!=0?"true":"false";strcpy(*out+*len,b);*len+=strlen(b);return;} if(v.t==TX){strcpy(*out+*len,"null");*len+=4;return;} if(v.t==TL){(*out)[(*len)++]='['; for(long i=0;i<v.l->len;i++){ if(i)(*out)[(*len)++]=','; to_json_rec(v.l->items[i],out,cap,len);} if(*len+2>*cap){*cap=*len+2;*out=realloc(*out,*cap);} (*out)[(*len)++]=']'; (*out)[*len]=0; return;} if(v.t==TM){(*out)[(*len)++]='{'; for(long i=0;i<v.m->len;i++){ if(i)(*out)[(*len)++]=','; json_str(out,cap,len,v.m->keys[i]); (*out)[(*len)++]=':'; to_json_rec(v.m->vals[i],out,cap,len);} if(*len+2>*cap){*cap=*len+2;*out=realloc(*out,*cap);} (*out)[(*len)++]='}'; (*out)[*len]=0; return;} }
static Value B_to_json(Value v){ long cap=256,len=0; char* o=malloc(cap); o[0]=0; to_json_rec(v,&o,&cap,&len); o[len]=0; Value r=STR(o); free(o); return r; }
static Value jparse(const char* s,long* i);
static void jws(const char* s,long* i){ while(s[*i]==' '||s[*i]=='\t'||s[*i]=='\n'||s[*i]=='\r')(*i)++; }
static Value jstring(const char* s,long* i){ (*i)++; char* b=malloc(strlen(s)+1); long j=0; while(s[*i]&&s[*i]!='"'){ if(s[*i]=='\\'){ (*i)++; char c=s[*i]; if(c=='n')b[j++]='\n'; else if(c=='t')b[j++]='\t'; else if(c=='r')b[j++]='\r'; else if(c=='u'){ int code=0; for(int k=0;k<4;k++){(*i)++; char h=s[*i]; code=code*16+((h<='9')?h-'0':(tolower(h)-'a'+10));} b[j++]=(char)code; } else b[j++]=c; (*i)++; } else b[j++]=s[(*i)++]; } if(s[*i]=='"')(*i)++; b[j]=0; Value v=STR(b); free(b); return v; }
static Value jparse(const char* s,long* i){ jws(s,i); char c=s[*i];
  if(c=='"')return jstring(s,i);
  if(c=='{'){ (*i)++; Value m=MAP0(); jws(s,i); if(s[*i]=='}'){(*i)++;return m;} for(;;){ jws(s,i); Value k=jstring(s,i); jws(s,i); if(s[*i]==':')(*i)++; Value v=jparse(s,i); mapset(m,k,v); jws(s,i); if(s[*i]==','){(*i)++;continue;} if(s[*i]=='}'){(*i)++;} break; } return m; }
  if(c=='['){ (*i)++; Value a=LIST0(); jws(s,i); if(s[*i]==']'){(*i)++;return a;} for(;;){ Value v=jparse(s,i); listpush(a,v); jws(s,i); if(s[*i]==','){(*i)++;continue;} if(s[*i]==']'){(*i)++;} break; } return a; }
  if(c=='t'){*i+=4;return BOOLV(1);} if(c=='f'){*i+=5;return BOOLV(0);} if(c=='n'){*i+=4;return NIL();}
  { char* end; double d=strtod(s+*i,&end); *i=end-s; return NUM(d); } }
static Value B_from_json(Value v){ long i=0; return jparse(tostr(v),&i); }
static Value B_make_dir(Value p){ char cmd[4096]; snprintf(cmd,sizeof cmd,"mkdir -p '%s'",tostr(p)); system(cmd); return NIL(); }
static Value B_path_exists(Value p){ struct stat st; return BOOLV(stat(tostr(p),&st)==0); }
static Value B_is_file(Value p){ struct stat st; return BOOLV(stat(tostr(p),&st)==0&&S_ISREG(st.st_mode)); }
static Value B_is_dir(Value p){ struct stat st; return BOOLV(stat(tostr(p),&st)==0&&S_ISDIR(st.st_mode)); }
static Value B_file_size(Value p){ struct stat st; if(stat(tostr(p),&st)==0) return NUM(st.st_size); return NUM(0); }
static Value B_list_dir(Value p){ Value v=LIST0(); DIR* d=opendir(tostr(p)); if(!d)return v; struct dirent* e; while((e=readdir(d))){ if(strcmp(e->d_name,".")&&strcmp(e->d_name,"..")) listpush(v,STR(e->d_name)); } closedir(d); return B_sort(v); }
static Value B_remove_path(Value p){ char cmd[4096]; snprintf(cmd,sizeof cmd,"rm -rf '%s'",tostr(p)); system(cmd); return NIL(); }
static Value B_move_path(Value a,Value b){ char cmd[8192]; snprintf(cmd,sizeof cmd,"mkdir -p \"$(dirname '%s')\"; mv '%s' '%s'",tostr(b),tostr(a),tostr(b)); system(cmd); return NIL(); }
static Value B_dirname(Value p){ char* s=sdup(tostr(p)); char* slash=strrchr(s,'/'); if(!slash){free(s);return STR("");} *slash=0; Value v=STR(s); free(s); return v; }
static Value B_basename(Value p){ char* s=tostr(p); char* slash=strrchr(s,'/'); return STR(slash?slash+1:s); }
static Value B_path_join(int n, ...){ char buf[8192]; buf[0]=0; va_list ap; va_start(ap,n); for(int i=0;i<n;i++){ Value a=va_arg(ap,Value); if(i&&buf[0]&&buf[strlen(buf)-1]!='/') strcat(buf,"/"); strcat(buf,tostr(a)); } va_end(ap); return STR(buf); }
static Value B_home_dir(void){ char* h=getenv("HOME"); return STR(h?h:"."); }
static Value B_env(Value k){ char* v=getenv(tostr(k)); return STR(v?v:""); }
static Value B_now(void){ return NUM((double)time(0)); }
static Value B_clock(void){ time_t t=time(0); struct tm* m=localtime(&t); char b[16]; sprintf(b,"%02d:%02d:%02d",m->tm_hour,m->tm_min,m->tm_sec); return STR(b); }
static Value B_today(void){ time_t t=time(0); struct tm* m=localtime(&t); char b[16]; sprintf(b,"%04d-%02d-%02d",m->tm_year+1900,m->tm_mon+1,m->tm_mday); return STR(b); }
static Value B_http_get(Value url, Value headers){ char cmd[16384]; int n=snprintf(cmd,sizeof cmd,"curl -sL"); if(headers.t==TM){ for(long i=0;i<headers.m->len;i++) n+=snprintf(cmd+n,sizeof cmd-n," -H '%s: %s'",headers.m->keys[i],tostr(headers.m->vals[i])); } snprintf(cmd+n,sizeof cmd-n," '%s'",tostr(url)); Value out=B_run(STR(cmd)); return MKMAP(2,STR("status"),NUM(200),STR("body"),out); }
static Value parse_query(const char* q){ Value m=MAP0(); if(!q||!*q)return m; char* s=sdup(q); char* p=s; while(p&&*p){ char* amp=strchr(p,'&'); if(amp)*amp=0; char* eq=strchr(p,'='); if(eq){*eq=0; Value k=B_url_decode(STR(p)); Value v=B_url_decode(STR(eq+1)); mapset(m,k,v);} p=amp?amp+1:0; } free(s); return m; }
static char* ci_strstr(const char* h, const char* n){ if(!*n) return (char*)h; for(; *h; h++){ const char* a=h; const char* b=n; while(*a && *b && tolower((unsigned char)*a)==tolower((unsigned char)*b)){ a++; b++; } if(!*b) return (char*)h; } return 0; }
static char* recv_request(int c,long* blen){ long cap=8192,len=0; char* buf=malloc(cap); for(;;){ if(len+4096>=cap){cap*=2;buf=realloc(buf,cap);} long r=recv(c,buf+len,4096,0); if(r<=0)break; len+=r; buf[len]=0; char* he=strstr(buf,"\r\n\r\n"); if(he){ long hlen=he-buf+4; char* cl=ci_strstr(buf,"content-length:"); long want=cl?atol(cl+15):0; while((long)(len-hlen)<want){ if(len+4096>=cap){cap*=2;buf=realloc(buf,cap);} long r2=recv(c,buf+len,4096,0); if(r2<=0)break; len+=r2; } buf[len]=0; break; } } *blen=len; return buf; }
static Value parse_request(char* raw){ Value req=MAP0(); char* nl=strstr(raw,"\r\n"); if(!nl)return req; *nl=0; char* method=raw; char* sp=strchr(raw,' '); if(!sp)return req; *sp=0; char* target=sp+1; char* sp2=strchr(target,' '); if(sp2)*sp2=0; char* q=strchr(target,'?'); char* query=""; if(q){*q=0;query=q+1;} mapset(req,STR("method"),STR(method)); mapset(req,STR("path"),B_url_decode(STR(target))); mapset(req,STR("query"),parse_query(query)); Value hdrs=MAP0(); char* he=strstr(nl+2,"\r\n\r\n"); char* line=nl+2; while(line&&he&&line<he){ char* eol=strstr(line,"\r\n"); if(!eol||eol>he)break; *eol=0; char* col=strchr(line,':'); if(col){*col=0; char* val=col+1; while(*val==' ')val++; mapset(hdrs,STR(line),STR(val));} line=eol+2; } mapset(req,STR("headers"),hdrs); mapset(req,STR("body"),STR(he?he+4:"")); return req; }
static Value B_typeof(Value v){ if(v.t==TS)return STR("string"); if(v.t==TN)return STR("number"); if(v.t==TB)return STR("bool"); if(v.t==TL)return STR("list"); if(v.t==TM)return STR("map"); return STR("nothing"); }
static Value B_tcp_listen(Value pv){ int srv=socket(AF_INET,SOCK_STREAM,0); int opt=1; setsockopt(srv,SOL_SOCKET,SO_REUSEADDR,&opt,sizeof opt); struct sockaddr_in a; memset(&a,0,sizeof a); a.sin_family=AF_INET; a.sin_addr.s_addr=INADDR_ANY; a.sin_port=htons((long)pv.n); if(bind(srv,(struct sockaddr*)&a,sizeof a)<0) return NUM(-1); listen(srv,64); return NUM(srv); }
static Value B_accept_req(Value sv){ int c=accept((int)sv.n,0,0); if(c<0) return NIL(); long bl; char* raw=recv_request(c,&bl); Value req=(raw&&bl>0)?parse_request(raw):NIL(); if(req.t==TM) mapset(req,STR("_conn"),NUM(c)); if(raw) free(raw); return req; }
static Value B_respond(Value req, Value resp){ Value cv=INDEX(req,STR("_conn")); int c=(cv.t==TN)?(int)cv.n:-1; if(c<0) return NIL(); long status=200; char* body=""; char* ctype="text/html; charset=utf-8"; Value xh=NIL(); if(resp.t==TS){ body=resp.s; } else if(resp.t==TM){ Value st=INDEX(resp,STR("status")); if(st.t==TN)status=(long)st.n; Value bd=INDEX(resp,STR("body")); if(bd.t==TM||bd.t==TL){ body=tostr(B_to_json(bd)); ctype="application/json"; } else if(bd.t!=TX) body=tostr(bd); Value ty=INDEX(resp,STR("type")); if(ty.t==TS)ctype=ty.s; xh=INDEX(resp,STR("headers")); } char head[4096]; long bl2=strlen(body); int hn=snprintf(head,sizeof head,"HTTP/1.1 %ld OK\r\nContent-Type: %s\r\nContent-Length: %ld\r\nConnection: close\r\n",status,ctype,bl2); if(xh.t==TM){ for(long i=0;i<xh.m->len;i++) hn+=snprintf(head+hn,sizeof head-hn,"%s: %s\r\n",xh.m->keys[i],tostr(xh.m->vals[i])); } hn+=snprintf(head+hn,sizeof head-hn,"\r\n"); write(c,head,hn); write(c,body,bl2); close(c); return NIL(); }
static Value vc_serve(long port, Value(*handler)(Value)){ int srv=socket(AF_INET,SOCK_STREAM,0); int opt=1; setsockopt(srv,SOL_SOCKET,SO_REUSEADDR,&opt,sizeof opt); struct sockaddr_in a; memset(&a,0,sizeof a); a.sin_family=AF_INET; a.sin_addr.s_addr=INADDR_ANY; a.sin_port=htons(port); if(bind(srv,(struct sockaddr*)&a,sizeof a)<0){perror("bind");return NIL();} listen(srv,64); printf("Vanta native server on http://localhost:%ld\n",port); fflush(stdout); g_in_req=1;
  for(;;){ ebb(); int c=accept(srv,0,0); if(c<0)continue; long blen; char* raw=recv_request(c,&blen); if(raw&&blen>0){ Value req=parse_request(raw); Value resp=handler(req); long status=200; char* body=""; char* ctype="text/html; charset=utf-8"; Value xh=NIL();
        if(resp.t==TS){ body=resp.s; } else if(resp.t==TM){ Value st=INDEX(resp,STR("status")); if(st.t==TN)status=(long)st.n; Value bd=INDEX(resp,STR("body")); if(bd.t==TM||bd.t==TL){ body=tostr(B_to_json(bd)); ctype="application/json"; } else if(bd.t!=TX) body=tostr(bd); Value ty=INDEX(resp,STR("type")); if(ty.t==TS)ctype=ty.s; xh=INDEX(resp,STR("headers")); }
        char head[4096]; long bl=strlen(body); int hn=snprintf(head,sizeof head,"HTTP/1.1 %ld OK\r\nContent-Type: %s\r\nContent-Length: %ld\r\nConnection: close\r\n",status,ctype,bl); if(xh.t==TM){ for(long i=0;i<xh.m->len;i++) hn+=snprintf(head+hn,sizeof head-hn,"%s: %s\r\n",xh.m->keys[i],tostr(xh.m->vals[i])); } hn+=snprintf(head+hn,sizeof head-hn,"\r\n"); write(c,head,hn); write(c,body,bl); free(raw); } close(c); }
  return NIL(); }

/* ---- exceptions (attempt/rescue via setjmp) + http_post + run_vanta ---- */
#include <setjmp.h>
static jmp_buf g_jmp[128]; static int g_jmpsp=0; static Value g_err;
static Value B_fail(Value msg){ g_err=msg; if(g_jmpsp>0) longjmp(g_jmp[g_jmpsp-1],1); fprintf(stderr,"fail: %s\n",tostr(msg)); exit(1); }
static Value B_http_post(Value url, Value body, Value headers){
  char* bodystr = (body.t==TM||body.t==TL)?tostr(B_to_json(body)):tostr(body);
  char tmpf[]="/tmp/vcpostXXXXXX"; int fd=mkstemp(tmpf); if(fd>=0){ write(fd,bodystr,strlen(bodystr)); close(fd);} 
  char cmd[32768]; int n=snprintf(cmd,sizeof cmd,"curl -s -X POST '%s'",tostr(url));
  if(headers.t==TM){ for(long i=0;i<headers.m->len;i++) n+=snprintf(cmd+n,sizeof cmd-n," -H '%s: %s'",headers.m->keys[i],tostr(headers.m->vals[i])); }
  n+=snprintf(cmd+n,sizeof cmd-n," --data-binary @%s",tmpf);
  Value out=B_run(STR(cmd)); unlink(tmpf);
  return MKMAP(2, STR("status"),NUM(200), STR("body"),out);
}
static Value B_run_vanta(Value code){
  char tmpf[]="/tmp/vcrunXXXXXX.va"; int fd=mkstemps(tmpf,3); char* c=tostr(code); if(fd>=0){ write(fd,c,strlen(c)); close(fd);} 
  char cmd[256]; snprintf(cmd,sizeof cmd,"vself '%s'",tmpf);
  Value r=B_shell(STR(cmd)); unlink(tmpf);
  int ok=((long)INDEX(r,STR("code")).n)==0;
  return MKMAP(3, STR("ok"),BOOLV(ok), STR("output"),INDEX(r,STR("output")), STR("error"), ok?STR(""):INDEX(r,STR("output")));
}

static Value B_starts_with(Value s, Value p){ char* a=tostr(s); char* b=tostr(p); return BOOLV(strncmp(a,b,strlen(b))==0); }
static Value B_ends_with(Value s, Value p){ char* a=tostr(s); char* b=tostr(p); long la=strlen(a),lb=strlen(b); return BOOLV(la>=lb&&strcmp(a+la-lb,b)==0); }
static Value B_find(Value s, Value sub){ char* a=tostr(s); char* q=strstr(a,tostr(sub)); return NUM(q?(q-a):-1); }
static Value B_os_name(void){
#ifdef __APPLE__
  return STR("mac");
#elif defined(_WIN32)
  return STR("windows");
#else
  return STR("linux");
#endif
}
static Value B_open_url(Value url){ char cmd[8192];
#ifdef __APPLE__
  snprintf(cmd,sizeof cmd,"open '%s' >/dev/null 2>&1",tostr(url));
#else
  snprintf(cmd,sizeof cmd,"xdg-open '%s' >/dev/null 2>&1",tostr(url));
#endif
  system(cmd); return NIL(); }

static Value B_reverse(Value v){ if(v.t==TS){ char* s=tostr(v); long n=strlen(s); char* r=malloc(n+1); for(long i=0;i<n;i++) r[i]=s[n-1-i]; r[n]=0; Value x=STR(r); free(r); return x; } if(v.t==TL){ Value o=LIST0(); for(long i=v.l->len-1;i>=0;i--) listpush(o,v.l->items[i]); return o; } return v; }
static Value B_first(Value v){ if(v.t==TL&&v.l->len>0) return v.l->items[0]; if(v.t==TS&&v.s[0]){ char b[2]={v.s[0],0}; return STR(b);} return NIL(); }
static Value B_last(Value v){ if(v.t==TL&&v.l->len>0) return v.l->items[v.l->len-1]; if(v.t==TS){ long n=strlen(v.s); if(n>0){char b[2]={v.s[n-1],0}; return STR(b);} } return NIL(); }
static Value B_floor(Value v){ long t=(long)v.n; return NUM((double)(t-((v.n<0&&v.n!=t)?1:0))); }
static Value B_ceil(Value v){ long t=(long)v.n; return NUM((double)(t+((v.n>0&&v.n!=t)?1:0))); }
static Value B_round(Value v){ return NUM((double)(long)(v.n+(v.n>=0?0.5:-0.5))); }
static Value B_abs(Value v){ return NUM(v.n<0?-v.n:v.n); }

Value v_LOOPN;
Value v_RUNTIME_B64;
Value v_args;

Value v_is_name_char(Value);
Value v_lex_line(Value);
Value v_parse_expr(Value, Value);
Value v_parse_or(Value, Value);
Value v_parse_and(Value, Value);
Value v_parse_cmp(Value, Value);
Value v_parse_add(Value, Value);
Value v_parse_mul(Value, Value);
Value v_parse_unary(Value, Value);
Value v_parse_primary(Value, Value);
Value v_parse_postfix(Value, Value, Value);
Value v_parse_atom(Value, Value);
Value v_parse_block(Value, Value);
Value v_parse_if(Value, Value, Value);
Value v_parse_stmt(Value, Value);
Value v_c_escape(Value);
Value v_cmp_fn(Value);
Value v_bin_fn(Value);
Value v_builtin_fn(Value);
Value v_gen_expr(Value);
Value v_join_pre(Value);
Value v_gen_block(Value, Value);
Value v_gen_stmt(Value, Value);
Value v_params_proto(Value);
Value v_params_decl(Value);
Value v_compile_prog(Value);
Value v_compile_kernel(Value);
Value v_build_and_run(Value, Value);
Value v_compile_only(Value, Value);

Value v_is_name_char(Value v_c) {
    return INOP(v_c, STR("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_"));
    return NIL();
}

Value v_lex_line(Value v_line) {
    Value v_toks = MKLIST(0);
    Value v_i = NUM(0);
    Value v_n = B_length(v_line);
    while (truthy(LT(v_i, v_n))) {
        Value v_c = INDEX(v_line, v_i);
        if (truthy(ORV(EQ(v_c, STR(" ")), EQ(v_c, STR("\t"))))) {
            v_i = ADD(v_i, NUM(1));
        } else {
            if (truthy(EQ(v_c, STR("#")))) {
                v_i = v_n;
            } else {
                if (truthy(INOP(v_c, STR("0123456789")))) {
                    Value v_start = v_i;
                    while (truthy(ANDV(LT(v_i, v_n), INOP(INDEX(v_line, v_i), STR("0123456789."))))) {
                        v_i = ADD(v_i, NUM(1));
                    }
                    listpush(v_toks, MKMAP(2, STR("t"), STR("num"), STR("v"), B_number(B_slice(v_line, v_start, v_i))));
                } else {
                    if (truthy(EQ(v_c, STR("\"")))) {
                        v_i = ADD(v_i, NUM(1));
                        Value v_sb = STR("");
                        while (truthy(ANDV(LT(v_i, v_n), NE(INDEX(v_line, v_i), STR("\""))))) {
                            if (truthy(ANDV(ANDV(EQ(INDEX(v_line, v_i), STR("{")), LT(ADD(v_i, NUM(1)), v_n)), EQ(INDEX(v_line, ADD(v_i, NUM(1))), STR("{"))))) {
                                v_sb = ADD(v_sb, STR("{"));
                                v_i = ADD(v_i, NUM(2));
                            } else {
                                if (truthy(ANDV(ANDV(EQ(INDEX(v_line, v_i), STR("}")), LT(ADD(v_i, NUM(1)), v_n)), EQ(INDEX(v_line, ADD(v_i, NUM(1))), STR("}"))))) {
                                    v_sb = ADD(v_sb, STR("}"));
                                    v_i = ADD(v_i, NUM(2));
                                } else {
                                    if (truthy(ANDV(EQ(INDEX(v_line, v_i), STR("\\")), LT(ADD(v_i, NUM(1)), v_n)))) {
                                        Value v_nx = INDEX(v_line, ADD(v_i, NUM(1)));
                                        if (truthy(EQ(v_nx, STR("n")))) {
                                            v_sb = ADD(v_sb, STR("\n"));
                                        } else {
                                            if (truthy(EQ(v_nx, STR("t")))) {
                                                v_sb = ADD(v_sb, STR("\t"));
                                            } else {
                                                v_sb = ADD(v_sb, v_nx);
                                            }
                                        }
                                        v_i = ADD(v_i, NUM(2));
                                    } else {
                                        v_sb = ADD(v_sb, INDEX(v_line, v_i));
                                        v_i = ADD(v_i, NUM(1));
                                    }
                                }
                            }
                        }
                        listpush(v_toks, MKMAP(2, STR("t"), STR("str"), STR("v"), v_sb));
                        v_i = ADD(v_i, NUM(1));
                    } else {
                        if (truthy(v_is_name_char(v_c))) {
                            Value v_start = v_i;
                            while (truthy(ANDV(LT(v_i, v_n), v_is_name_char(INDEX(v_line, v_i))))) {
                                v_i = ADD(v_i, NUM(1));
                            }
                            listpush(v_toks, MKMAP(2, STR("t"), STR("word"), STR("v"), B_slice(v_line, v_start, v_i)));
                        } else {
                            listpush(v_toks, MKMAP(2, STR("t"), STR("sym"), STR("v"), v_c));
                            v_i = ADD(v_i, NUM(1));
                        }
                    }
                }
            }
        }
    }
    return v_toks;
    return NIL();
}

Value v_parse_expr(Value v_toks, Value v_i) {
    return v_parse_or(v_toks, v_i);
    return NIL();
}

Value v_parse_or(Value v_toks, Value v_i) {
    Value v_left = v_parse_and(v_toks, v_i);
    Value v_node = INDEX(v_left, STR("node"));
    Value v_j = INDEX(v_left, STR("i"));
    while (truthy(ANDV(ANDV(LT(v_j, B_length(v_toks)), EQ(INDEX(INDEX(v_toks, v_j), STR("t")), STR("word"))), EQ(INDEX(INDEX(v_toks, v_j), STR("v")), STR("or"))))) {
        Value v_right = v_parse_and(v_toks, ADD(v_j, NUM(1)));
        v_node = MKMAP(3, STR("k"), STR("or"), STR("a"), v_node, STR("b"), INDEX(v_right, STR("node")));
        v_j = INDEX(v_right, STR("i"));
    }
    return MKMAP(2, STR("node"), v_node, STR("i"), v_j);
    return NIL();
}

Value v_parse_and(Value v_toks, Value v_i) {
    Value v_left = v_parse_cmp(v_toks, v_i);
    Value v_node = INDEX(v_left, STR("node"));
    Value v_j = INDEX(v_left, STR("i"));
    while (truthy(ANDV(ANDV(LT(v_j, B_length(v_toks)), EQ(INDEX(INDEX(v_toks, v_j), STR("t")), STR("word"))), EQ(INDEX(INDEX(v_toks, v_j), STR("v")), STR("and"))))) {
        Value v_right = v_parse_cmp(v_toks, ADD(v_j, NUM(1)));
        v_node = MKMAP(3, STR("k"), STR("and"), STR("a"), v_node, STR("b"), INDEX(v_right, STR("node")));
        v_j = INDEX(v_right, STR("i"));
    }
    return MKMAP(2, STR("node"), v_node, STR("i"), v_j);
    return NIL();
}

Value v_parse_cmp(Value v_toks, Value v_i) {
    if (truthy(ANDV(ANDV(LT(v_i, B_length(v_toks)), EQ(INDEX(INDEX(v_toks, v_i), STR("t")), STR("word"))), EQ(INDEX(INDEX(v_toks, v_i), STR("v")), STR("not"))))) {
        Value v_inner = v_parse_cmp(v_toks, ADD(v_i, NUM(1)));
        return MKMAP(2, STR("node"), MKMAP(2, STR("k"), STR("not"), STR("a"), INDEX(v_inner, STR("node"))), STR("i"), INDEX(v_inner, STR("i")));
    }
    Value v_left = v_parse_add(v_toks, v_i);
    Value v_node = INDEX(v_left, STR("node"));
    Value v_j = INDEX(v_left, STR("i"));
    if (truthy(ANDV(ANDV(LT(v_j, B_length(v_toks)), EQ(INDEX(INDEX(v_toks, v_j), STR("t")), STR("word"))), EQ(INDEX(INDEX(v_toks, v_j), STR("v")), STR("is"))))) {
        v_j = ADD(v_j, NUM(1));
        Value v_op = STR("==");
        if (truthy(ANDV(LT(v_j, B_length(v_toks)), EQ(INDEX(INDEX(v_toks, v_j), STR("t")), STR("word"))))) {
            Value v_w = INDEX(INDEX(v_toks, v_j), STR("v"));
            if (truthy(EQ(v_w, STR("not")))) {
                v_op = STR("!=");
                v_j = ADD(v_j, NUM(1));
            } else {
                if (truthy(EQ(v_w, STR("in")))) {
                    v_op = STR("in");
                    v_j = ADD(v_j, NUM(1));
                } else {
                    if (truthy(EQ(v_w, STR("over")))) {
                        v_op = STR(">");
                        v_j = ADD(v_j, NUM(1));
                    } else {
                        if (truthy(EQ(v_w, STR("under")))) {
                            v_op = STR("<");
                            v_j = ADD(v_j, NUM(1));
                        } else {
                            if (truthy(EQ(v_w, STR("at")))) {
                                v_j = ADD(v_j, NUM(1));
                                if (truthy(ANDV(LT(v_j, B_length(v_toks)), EQ(INDEX(INDEX(v_toks, v_j), STR("v")), STR("least"))))) {
                                    v_op = STR(">=");
                                } else {
                                    v_op = STR("<=");
                                }
                                v_j = ADD(v_j, NUM(1));
                            }
                        }
                    }
                }
            }
        }
        Value v_right = v_parse_add(v_toks, v_j);
        return MKMAP(2, STR("node"), MKMAP(4, STR("k"), STR("cmp"), STR("op"), v_op, STR("a"), v_node, STR("b"), INDEX(v_right, STR("node"))), STR("i"), INDEX(v_right, STR("i")));
    }
    return MKMAP(2, STR("node"), v_node, STR("i"), v_j);
    return NIL();
}

Value v_parse_add(Value v_toks, Value v_i) {
    Value v_left = v_parse_mul(v_toks, v_i);
    Value v_node = INDEX(v_left, STR("node"));
    Value v_j = INDEX(v_left, STR("i"));
    while (truthy(ANDV(ANDV(LT(v_j, B_length(v_toks)), EQ(INDEX(INDEX(v_toks, v_j), STR("t")), STR("sym"))), ORV(EQ(INDEX(INDEX(v_toks, v_j), STR("v")), STR("+")), EQ(INDEX(INDEX(v_toks, v_j), STR("v")), STR("-")))))) {
        Value v_op = INDEX(INDEX(v_toks, v_j), STR("v"));
        Value v_right = v_parse_mul(v_toks, ADD(v_j, NUM(1)));
        v_node = MKMAP(4, STR("k"), STR("bin"), STR("op"), v_op, STR("a"), v_node, STR("b"), INDEX(v_right, STR("node")));
        v_j = INDEX(v_right, STR("i"));
    }
    return MKMAP(2, STR("node"), v_node, STR("i"), v_j);
    return NIL();
}

Value v_parse_mul(Value v_toks, Value v_i) {
    Value v_left = v_parse_unary(v_toks, v_i);
    Value v_node = INDEX(v_left, STR("node"));
    Value v_j = INDEX(v_left, STR("i"));
    while (truthy(ANDV(ANDV(LT(v_j, B_length(v_toks)), EQ(INDEX(INDEX(v_toks, v_j), STR("t")), STR("sym"))), ORV(EQ(INDEX(INDEX(v_toks, v_j), STR("v")), STR("*")), EQ(INDEX(INDEX(v_toks, v_j), STR("v")), STR("/")))))) {
        Value v_op = INDEX(INDEX(v_toks, v_j), STR("v"));
        Value v_right = v_parse_unary(v_toks, ADD(v_j, NUM(1)));
        v_node = MKMAP(4, STR("k"), STR("bin"), STR("op"), v_op, STR("a"), v_node, STR("b"), INDEX(v_right, STR("node")));
        v_j = INDEX(v_right, STR("i"));
    }
    return MKMAP(2, STR("node"), v_node, STR("i"), v_j);
    return NIL();
}

Value v_parse_unary(Value v_toks, Value v_i) {
    if (truthy(ANDV(ANDV(LT(v_i, B_length(v_toks)), EQ(INDEX(INDEX(v_toks, v_i), STR("t")), STR("sym"))), EQ(INDEX(INDEX(v_toks, v_i), STR("v")), STR("-"))))) {
        Value v_inner = v_parse_unary(v_toks, ADD(v_i, NUM(1)));
        return MKMAP(2, STR("node"), MKMAP(2, STR("k"), STR("neg"), STR("a"), INDEX(v_inner, STR("node"))), STR("i"), INDEX(v_inner, STR("i")));
    }
    return v_parse_primary(v_toks, v_i);
    return NIL();
}

Value v_parse_primary(Value v_toks, Value v_i) {
    Value v_base = v_parse_atom(v_toks, v_i);
    return v_parse_postfix(INDEX(v_base, STR("node")), v_toks, INDEX(v_base, STR("i")));
    return NIL();
}

Value v_parse_postfix(Value v_node, Value v_toks, Value v_i) {
    Value v_nd = v_node;
    Value v_j = v_i;
    while (truthy(ANDV(ANDV(LT(v_j, B_length(v_toks)), EQ(INDEX(INDEX(v_toks, v_j), STR("t")), STR("sym"))), EQ(INDEX(INDEX(v_toks, v_j), STR("v")), STR("["))))) {
        Value v_a = v_parse_expr(v_toks, ADD(v_j, NUM(1)));
        Value v_after = INDEX(v_a, STR("i"));
        if (truthy(ANDV(ANDV(LT(v_after, B_length(v_toks)), EQ(INDEX(INDEX(v_toks, v_after), STR("t")), STR("sym"))), EQ(INDEX(INDEX(v_toks, v_after), STR("v")), STR(":"))))) {
            Value v_b = v_parse_expr(v_toks, ADD(v_after, NUM(1)));
            v_nd = MKMAP(4, STR("k"), STR("slice"), STR("o"), v_nd, STR("a"), INDEX(v_a, STR("node")), STR("b"), INDEX(v_b, STR("node")));
            v_j = ADD(INDEX(v_b, STR("i")), NUM(1));
        } else {
            v_nd = MKMAP(3, STR("k"), STR("index"), STR("o"), v_nd, STR("idx"), INDEX(v_a, STR("node")));
            v_j = ADD(v_after, NUM(1));
        }
    }
    return MKMAP(2, STR("node"), v_nd, STR("i"), v_j);
    return NIL();
}

Value v_parse_atom(Value v_toks, Value v_i) {
    Value v_tk = INDEX(v_toks, v_i);
    if (truthy(EQ(INDEX(v_tk, STR("t")), STR("num")))) {
        return MKMAP(2, STR("node"), MKMAP(2, STR("k"), STR("num"), STR("v"), INDEX(v_tk, STR("v"))), STR("i"), ADD(v_i, NUM(1)));
    }
    if (truthy(EQ(INDEX(v_tk, STR("t")), STR("str")))) {
        return MKMAP(2, STR("node"), MKMAP(2, STR("k"), STR("str"), STR("v"), INDEX(v_tk, STR("v"))), STR("i"), ADD(v_i, NUM(1)));
    }
    if (truthy(ANDV(EQ(INDEX(v_tk, STR("t")), STR("sym")), EQ(INDEX(v_tk, STR("v")), STR("("))))) {
        Value v_inner = v_parse_expr(v_toks, ADD(v_i, NUM(1)));
        return MKMAP(2, STR("node"), INDEX(v_inner, STR("node")), STR("i"), ADD(INDEX(v_inner, STR("i")), NUM(1)));
    }
    if (truthy(ANDV(EQ(INDEX(v_tk, STR("t")), STR("sym")), EQ(INDEX(v_tk, STR("v")), STR("["))))) {
        Value v_items = MKLIST(0);
        Value v_j = ADD(v_i, NUM(1));
        while (truthy(ANDV(LT(v_j, B_length(v_toks)), NE(INDEX(INDEX(v_toks, v_j), STR("v")), STR("]"))))) {
            Value v_e = v_parse_expr(v_toks, v_j);
            listpush(v_items, INDEX(v_e, STR("node")));
            v_j = INDEX(v_e, STR("i"));
            if (truthy(ANDV(LT(v_j, B_length(v_toks)), EQ(INDEX(INDEX(v_toks, v_j), STR("v")), STR(","))))) {
                v_j = ADD(v_j, NUM(1));
            }
        }
        return MKMAP(2, STR("node"), MKMAP(2, STR("k"), STR("list"), STR("items"), v_items), STR("i"), ADD(v_j, NUM(1)));
    }
    if (truthy(ANDV(EQ(INDEX(v_tk, STR("t")), STR("sym")), EQ(INDEX(v_tk, STR("v")), STR("{"))))) {
        Value v_pairs = MKLIST(0);
        Value v_j = ADD(v_i, NUM(1));
        while (truthy(ANDV(LT(v_j, B_length(v_toks)), NE(INDEX(INDEX(v_toks, v_j), STR("v")), STR("}"))))) {
            Value v_kx = v_parse_expr(v_toks, v_j);
            v_j = ADD(INDEX(v_kx, STR("i")), NUM(1));
            Value v_vx = v_parse_expr(v_toks, v_j);
            v_j = INDEX(v_vx, STR("i"));
            listpush(v_pairs, MKMAP(2, STR("kn"), INDEX(v_kx, STR("node")), STR("vn"), INDEX(v_vx, STR("node"))));
            if (truthy(ANDV(LT(v_j, B_length(v_toks)), EQ(INDEX(INDEX(v_toks, v_j), STR("v")), STR(","))))) {
                v_j = ADD(v_j, NUM(1));
            }
        }
        return MKMAP(2, STR("node"), MKMAP(2, STR("k"), STR("map"), STR("pairs"), v_pairs), STR("i"), ADD(v_j, NUM(1)));
    }
    if (truthy(EQ(INDEX(v_tk, STR("t")), STR("word")))) {
        Value v_w = INDEX(v_tk, STR("v"));
        if (truthy(EQ(v_w, STR("yes")))) {
            return MKMAP(2, STR("node"), MKMAP(2, STR("k"), STR("lit"), STR("v"), BOOLV(1)), STR("i"), ADD(v_i, NUM(1)));
        }
        if (truthy(EQ(v_w, STR("no")))) {
            return MKMAP(2, STR("node"), MKMAP(2, STR("k"), STR("lit"), STR("v"), BOOLV(0)), STR("i"), ADD(v_i, NUM(1)));
        }
        if (truthy(EQ(v_w, STR("nothing")))) {
            return MKMAP(2, STR("node"), MKMAP(2, STR("k"), STR("lit"), STR("v"), NIL()), STR("i"), ADD(v_i, NUM(1)));
        }
        if (truthy(ANDV(ANDV(LT(ADD(v_i, NUM(1)), B_length(v_toks)), EQ(INDEX(INDEX(v_toks, ADD(v_i, NUM(1))), STR("t")), STR("sym"))), EQ(INDEX(INDEX(v_toks, ADD(v_i, NUM(1))), STR("v")), STR("("))))) {
            Value v_args = MKLIST(0);
            Value v_j = ADD(v_i, NUM(2));
            while (truthy(ANDV(LT(v_j, B_length(v_toks)), NE(INDEX(INDEX(v_toks, v_j), STR("v")), STR(")"))))) {
                Value v_a = v_parse_expr(v_toks, v_j);
                listpush(v_args, INDEX(v_a, STR("node")));
                v_j = INDEX(v_a, STR("i"));
                if (truthy(ANDV(LT(v_j, B_length(v_toks)), EQ(INDEX(INDEX(v_toks, v_j), STR("v")), STR(","))))) {
                    v_j = ADD(v_j, NUM(1));
                }
            }
            return MKMAP(2, STR("node"), MKMAP(3, STR("k"), STR("call"), STR("name"), v_w, STR("args"), v_args), STR("i"), ADD(v_j, NUM(1)));
        }
        return MKMAP(2, STR("node"), MKMAP(2, STR("k"), STR("var"), STR("name"), v_w), STR("i"), ADD(v_i, NUM(1)));
    }
    return MKMAP(2, STR("node"), MKMAP(2, STR("k"), STR("num"), STR("v"), NUM(0)), STR("i"), ADD(v_i, NUM(1)));
    return NIL();
}

Value v_parse_block(Value v_prog, Value v_i) {
    Value v_stmts = MKLIST(0);
    while (truthy(LT(v_i, B_length(v_prog)))) {
        Value v_first = INDEX(INDEX(INDEX(v_prog, v_i), NUM(0)), STR("v"));
        if (truthy(ORV(ORV(EQ(v_first, STR("end")), EQ(v_first, STR("otherwise"))), EQ(v_first, STR("rescue"))))) {
            return MKMAP(2, STR("stmts"), v_stmts, STR("i"), v_i);
        }
        Value v_r = v_parse_stmt(v_prog, v_i);
        listpush(v_stmts, INDEX(v_r, STR("stmt")));
        v_i = INDEX(v_r, STR("i"));
    }
    return MKMAP(2, STR("stmts"), v_stmts, STR("i"), v_i);
    return NIL();
}

Value v_parse_if(Value v_prog, Value v_i, Value v_cstart) {
    Value v_toks = INDEX(v_prog, v_i);
    Value v_cond = v_parse_expr(v_toks, v_cstart);
    Value v_b = v_parse_block(v_prog, ADD(v_i, NUM(1)));
    Value v_j = INDEX(v_b, STR("i"));
    if (truthy(ANDV(LT(v_j, B_length(v_prog)), EQ(INDEX(INDEX(INDEX(v_prog, v_j), NUM(0)), STR("v")), STR("otherwise"))))) {
        if (truthy(ANDV(GT(B_length(INDEX(v_prog, v_j)), NUM(1)), EQ(INDEX(INDEX(INDEX(v_prog, v_j), NUM(1)), STR("v")), STR("if"))))) {
            Value v_r = v_parse_if(v_prog, v_j, NUM(2));
            return MKMAP(2, STR("stmt"), MKMAP(4, STR("k"), STR("if"), STR("c"), INDEX(v_cond, STR("node")), STR("body"), INDEX(v_b, STR("stmts")), STR("els"), MKLIST(1, INDEX(v_r, STR("stmt")))), STR("i"), INDEX(v_r, STR("i")));
        }
        Value v_e2 = v_parse_block(v_prog, ADD(v_j, NUM(1)));
        return MKMAP(2, STR("stmt"), MKMAP(4, STR("k"), STR("if"), STR("c"), INDEX(v_cond, STR("node")), STR("body"), INDEX(v_b, STR("stmts")), STR("els"), INDEX(v_e2, STR("stmts"))), STR("i"), ADD(INDEX(v_e2, STR("i")), NUM(1)));
    }
    return MKMAP(2, STR("stmt"), MKMAP(4, STR("k"), STR("if"), STR("c"), INDEX(v_cond, STR("node")), STR("body"), INDEX(v_b, STR("stmts")), STR("els"), MKLIST(0)), STR("i"), ADD(v_j, NUM(1)));
    return NIL();
}

Value v_parse_stmt(Value v_prog, Value v_i) {
    Value v_toks = INDEX(v_prog, v_i);
    Value v_head = INDEX(INDEX(v_toks, NUM(0)), STR("v"));
    if (truthy(EQ(v_head, STR("let")))) {
        Value v_ex = v_parse_expr(v_toks, NUM(3));
        return MKMAP(2, STR("stmt"), MKMAP(3, STR("k"), STR("let"), STR("name"), INDEX(INDEX(v_toks, NUM(1)), STR("v")), STR("e"), INDEX(v_ex, STR("node"))), STR("i"), ADD(v_i, NUM(1)));
    }
    if (truthy(EQ(v_head, STR("change")))) {
        if (truthy(ANDV(GT(B_length(v_toks), NUM(2)), EQ(INDEX(INDEX(v_toks, NUM(2)), STR("v")), STR("at"))))) {
            Value v_kx = v_parse_expr(v_toks, NUM(3));
            Value v_vx = v_parse_expr(v_toks, ADD(INDEX(v_kx, STR("i")), NUM(1)));
            return MKMAP(2, STR("stmt"), MKMAP(4, STR("k"), STR("setat"), STR("name"), INDEX(INDEX(v_toks, NUM(1)), STR("v")), STR("key"), INDEX(v_kx, STR("node")), STR("e"), INDEX(v_vx, STR("node"))), STR("i"), ADD(v_i, NUM(1)));
        }
        Value v_ex = v_parse_expr(v_toks, NUM(3));
        return MKMAP(2, STR("stmt"), MKMAP(3, STR("k"), STR("set"), STR("name"), INDEX(INDEX(v_toks, NUM(1)), STR("v")), STR("e"), INDEX(v_ex, STR("node"))), STR("i"), ADD(v_i, NUM(1)));
    }
    if (truthy(EQ(v_head, STR("say")))) {
        Value v_ex = v_parse_expr(v_toks, NUM(1));
        return MKMAP(2, STR("stmt"), MKMAP(2, STR("k"), STR("say"), STR("e"), INDEX(v_ex, STR("node"))), STR("i"), ADD(v_i, NUM(1)));
    }
    if (truthy(EQ(v_head, STR("add")))) {
        Value v_ax = v_parse_expr(v_toks, NUM(1));
        Value v_tx = v_parse_expr(v_toks, ADD(INDEX(v_ax, STR("i")), NUM(1)));
        return MKMAP(2, STR("stmt"), MKMAP(3, STR("k"), STR("add"), STR("val"), INDEX(v_ax, STR("node")), STR("target"), INDEX(v_tx, STR("node"))), STR("i"), ADD(v_i, NUM(1)));
    }
    if (truthy(EQ(v_head, STR("give")))) {
        Value v_ex = v_parse_expr(v_toks, NUM(2));
        return MKMAP(2, STR("stmt"), MKMAP(2, STR("k"), STR("ret"), STR("e"), INDEX(v_ex, STR("node"))), STR("i"), ADD(v_i, NUM(1)));
    }
    if (truthy(EQ(v_head, STR("if")))) {
        return v_parse_if(v_prog, v_i, NUM(1));
    }
    if (truthy(EQ(v_head, STR("while")))) {
        Value v_cond = v_parse_expr(v_toks, NUM(1));
        Value v_b = v_parse_block(v_prog, ADD(v_i, NUM(1)));
        return MKMAP(2, STR("stmt"), MKMAP(3, STR("k"), STR("while"), STR("c"), INDEX(v_cond, STR("node")), STR("body"), INDEX(v_b, STR("stmts"))), STR("i"), ADD(INDEX(v_b, STR("i")), NUM(1)));
    }
    if (truthy(EQ(v_head, STR("for")))) {
        Value v_lx = v_parse_expr(v_toks, NUM(4));
        Value v_b = v_parse_block(v_prog, ADD(v_i, NUM(1)));
        return MKMAP(2, STR("stmt"), MKMAP(4, STR("k"), STR("for"), STR("var"), INDEX(INDEX(v_toks, NUM(2)), STR("v")), STR("list"), INDEX(v_lx, STR("node")), STR("body"), INDEX(v_b, STR("stmts"))), STR("i"), ADD(INDEX(v_b, STR("i")), NUM(1)));
    }
    if (truthy(EQ(v_head, STR("repeat")))) {
        Value v_nx = v_parse_expr(v_toks, NUM(1));
        Value v_b = v_parse_block(v_prog, ADD(v_i, NUM(1)));
        return MKMAP(2, STR("stmt"), MKMAP(3, STR("k"), STR("repeat"), STR("n"), INDEX(v_nx, STR("node")), STR("body"), INDEX(v_b, STR("stmts"))), STR("i"), ADD(INDEX(v_b, STR("i")), NUM(1)));
    }
    if (truthy(EQ(v_head, STR("attempt")))) {
        Value v_b = v_parse_block(v_prog, ADD(v_i, NUM(1)));
        Value v_j = INDEX(v_b, STR("i"));
        Value v_errname = STR("error");
        if (truthy(GT(B_length(INDEX(v_prog, v_j)), NUM(1)))) {
            v_errname = INDEX(INDEX(INDEX(v_prog, v_j), NUM(1)), STR("v"));
        }
        Value v_rb = v_parse_block(v_prog, ADD(v_j, NUM(1)));
        return MKMAP(2, STR("stmt"), MKMAP(4, STR("k"), STR("attempt"), STR("body"), INDEX(v_b, STR("stmts")), STR("errname"), v_errname, STR("rescue"), INDEX(v_rb, STR("stmts"))), STR("i"), ADD(INDEX(v_rb, STR("i")), NUM(1)));
    }
    if (truthy(EQ(v_head, STR("to")))) {
        Value v_params = MKLIST(0);
        Value v_p = NUM(3);
        while (truthy(ANDV(LT(v_p, B_length(v_toks)), NE(INDEX(INDEX(v_toks, v_p), STR("v")), STR(")"))))) {
            if (truthy(EQ(INDEX(INDEX(v_toks, v_p), STR("t")), STR("word")))) {
                listpush(v_params, INDEX(INDEX(v_toks, v_p), STR("v")));
            }
            v_p = ADD(v_p, NUM(1));
        }
        Value v_b = v_parse_block(v_prog, ADD(v_i, NUM(1)));
        return MKMAP(2, STR("stmt"), MKMAP(4, STR("k"), STR("func"), STR("name"), INDEX(INDEX(v_toks, NUM(1)), STR("v")), STR("params"), v_params, STR("body"), INDEX(v_b, STR("stmts"))), STR("i"), ADD(INDEX(v_b, STR("i")), NUM(1)));
    }
    Value v_ex = v_parse_expr(v_toks, NUM(0));
    return MKMAP(2, STR("stmt"), MKMAP(2, STR("k"), STR("expr"), STR("e"), INDEX(v_ex, STR("node"))), STR("i"), ADD(v_i, NUM(1)));
    return NIL();
}

Value v_c_escape(Value v_s) {
    Value v_a = B_replace(v_s, STR("\\"), STR("\\\\"));
    Value v_b = B_replace(v_a, STR("\""), STR("\\\""));
    Value v_c = B_replace(v_b, STR("\n"), STR("\\n"));
    return B_replace(v_c, STR("\t"), STR("\\t"));
    return NIL();
}

Value v_cmp_fn(Value v_op) {
    if (truthy(EQ(v_op, STR("==")))) {
        return STR("EQ");
    }
    if (truthy(EQ(v_op, STR("!=")))) {
        return STR("NE");
    }
    if (truthy(EQ(v_op, STR(">")))) {
        return STR("GT");
    }
    if (truthy(EQ(v_op, STR("<")))) {
        return STR("LT");
    }
    if (truthy(EQ(v_op, STR(">=")))) {
        return STR("GE");
    }
    if (truthy(EQ(v_op, STR("<=")))) {
        return STR("LE");
    }
    return STR("INOP");
    return NIL();
}

Value v_bin_fn(Value v_op) {
    if (truthy(EQ(v_op, STR("+")))) {
        return STR("ADD");
    }
    if (truthy(EQ(v_op, STR("-")))) {
        return STR("SUB");
    }
    if (truthy(EQ(v_op, STR("*")))) {
        return STR("MUL");
    }
    return STR("DIVV");
    return NIL();
}

Value v_builtin_fn(Value v_name) {
    if (truthy(EQ(v_name, STR("text")))) {
        return STR("B_text");
    }
    if (truthy(EQ(v_name, STR("length")))) {
        return STR("B_length");
    }
    if (truthy(EQ(v_name, STR("keys")))) {
        return STR("B_keys");
    }
    if (truthy(EQ(v_name, STR("values")))) {
        return STR("B_values");
    }
    if (truthy(EQ(v_name, STR("uppercase")))) {
        return STR("B_upper");
    }
    if (truthy(EQ(v_name, STR("lowercase")))) {
        return STR("B_lower");
    }
    if (truthy(EQ(v_name, STR("trim")))) {
        return STR("B_trim");
    }
    if (truthy(EQ(v_name, STR("number")))) {
        return STR("B_number");
    }
    if (truthy(EQ(v_name, STR("join")))) {
        return STR("B_join");
    }
    if (truthy(EQ(v_name, STR("split")))) {
        return STR("B_split");
    }
    if (truthy(EQ(v_name, STR("sort")))) {
        return STR("B_sort");
    }
    if (truthy(EQ(v_name, STR("contains")))) {
        return STR("B_contains");
    }
    if (truthy(EQ(v_name, STR("slice")))) {
        return STR("B_slice");
    }
    if (truthy(EQ(v_name, STR("replace")))) {
        return STR("B_replace");
    }
    if (truthy(EQ(v_name, STR("read_file")))) {
        return STR("B_read_file");
    }
    if (truthy(EQ(v_name, STR("write_file")))) {
        return STR("B_write_file");
    }
    if (truthy(EQ(v_name, STR("run")))) {
        return STR("B_run");
    }
    if (truthy(EQ(v_name, STR("shell")))) {
        return STR("B_shell");
    }
    if (truthy(EQ(v_name, STR("arguments")))) {
        return STR("B_arguments");
    }
    if (truthy(EQ(v_name, STR("base64_decode")))) {
        return STR("B_b64decode");
    }
    if (truthy(EQ(v_name, STR("from_json")))) {
        return STR("B_from_json");
    }
    if (truthy(EQ(v_name, STR("to_json")))) {
        return STR("B_to_json");
    }
    if (truthy(EQ(v_name, STR("http_get")))) {
        return STR("B_http_get");
    }
    if (truthy(EQ(v_name, STR("make_dir")))) {
        return STR("B_make_dir");
    }
    if (truthy(EQ(v_name, STR("path_exists")))) {
        return STR("B_path_exists");
    }
    if (truthy(EQ(v_name, STR("is_file")))) {
        return STR("B_is_file");
    }
    if (truthy(EQ(v_name, STR("is_dir")))) {
        return STR("B_is_dir");
    }
    if (truthy(EQ(v_name, STR("file_size")))) {
        return STR("B_file_size");
    }
    if (truthy(EQ(v_name, STR("list_dir")))) {
        return STR("B_list_dir");
    }
    if (truthy(EQ(v_name, STR("remove_path")))) {
        return STR("B_remove_path");
    }
    if (truthy(EQ(v_name, STR("move_path")))) {
        return STR("B_move_path");
    }
    if (truthy(EQ(v_name, STR("dirname")))) {
        return STR("B_dirname");
    }
    if (truthy(EQ(v_name, STR("basename")))) {
        return STR("B_basename");
    }
    if (truthy(EQ(v_name, STR("home_dir")))) {
        return STR("B_home_dir");
    }
    if (truthy(EQ(v_name, STR("env")))) {
        return STR("B_env");
    }
    if (truthy(EQ(v_name, STR("now")))) {
        return STR("B_now");
    }
    if (truthy(EQ(v_name, STR("clock")))) {
        return STR("B_clock");
    }
    if (truthy(EQ(v_name, STR("today")))) {
        return STR("B_today");
    }
    if (truthy(EQ(v_name, STR("html_escape")))) {
        return STR("B_html_escape");
    }
    if (truthy(EQ(v_name, STR("url_encode")))) {
        return STR("B_url_encode");
    }
    if (truthy(EQ(v_name, STR("url_decode")))) {
        return STR("B_url_decode");
    }
    if (truthy(EQ(v_name, STR("append_file")))) {
        return STR("B_append_file");
    }
    if (truthy(EQ(v_name, STR("fail")))) {
        return STR("B_fail");
    }
    if (truthy(EQ(v_name, STR("http_post")))) {
        return STR("B_http_post");
    }
    if (truthy(EQ(v_name, STR("run_vanta")))) {
        return STR("B_run_vanta");
    }
    if (truthy(EQ(v_name, STR("starts_with")))) {
        return STR("B_starts_with");
    }
    if (truthy(EQ(v_name, STR("ends_with")))) {
        return STR("B_ends_with");
    }
    if (truthy(EQ(v_name, STR("find")))) {
        return STR("B_find");
    }
    if (truthy(EQ(v_name, STR("os_name")))) {
        return STR("B_os_name");
    }
    if (truthy(EQ(v_name, STR("open_url")))) {
        return STR("B_open_url");
    }
    if (truthy(EQ(v_name, STR("reverse")))) {
        return STR("B_reverse");
    }
    if (truthy(EQ(v_name, STR("first")))) {
        return STR("B_first");
    }
    if (truthy(EQ(v_name, STR("last")))) {
        return STR("B_last");
    }
    if (truthy(EQ(v_name, STR("floor")))) {
        return STR("B_floor");
    }
    if (truthy(EQ(v_name, STR("ceil")))) {
        return STR("B_ceil");
    }
    if (truthy(EQ(v_name, STR("round")))) {
        return STR("B_round");
    }
    if (truthy(EQ(v_name, STR("abs")))) {
        return STR("B_abs");
    }
    if (truthy(EQ(v_name, STR("typeof")))) {
        return STR("B_typeof");
    }
    if (truthy(EQ(v_name, STR("tcp_listen")))) {
        return STR("B_tcp_listen");
    }
    if (truthy(EQ(v_name, STR("accept_req")))) {
        return STR("B_accept_req");
    }
    if (truthy(EQ(v_name, STR("respond")))) {
        return STR("B_respond");
    }
    return STR("");
    return NIL();
}

Value v_gen_expr(Value v_node) {
    Value v_k = INDEX(v_node, STR("k"));
    if (truthy(EQ(v_k, STR("num")))) {
        return ADD(ADD(STR("NUM("), B_text(INDEX(v_node, STR("v")))), STR(")"));
    }
    if (truthy(EQ(v_k, STR("str")))) {
        return ADD(ADD(STR("STR(\""), v_c_escape(INDEX(v_node, STR("v")))), STR("\")"));
    }
    if (truthy(EQ(v_k, STR("lit")))) {
        if (truthy(EQ(INDEX(v_node, STR("v")), BOOLV(1)))) {
            return STR("BOOLV(1)");
        }
        if (truthy(EQ(INDEX(v_node, STR("v")), BOOLV(0)))) {
            return STR("BOOLV(0)");
        }
        return STR("NIL()");
    }
    if (truthy(EQ(v_k, STR("var")))) {
        return ADD(STR("v_"), INDEX(v_node, STR("name")));
    }
    if (truthy(EQ(v_k, STR("neg")))) {
        return ADD(ADD(STR("NEG("), v_gen_expr(INDEX(v_node, STR("a")))), STR(")"));
    }
    if (truthy(EQ(v_k, STR("not")))) {
        return ADD(ADD(STR("NOTV("), v_gen_expr(INDEX(v_node, STR("a")))), STR(")"));
    }
    if (truthy(EQ(v_k, STR("and")))) {
        return ADD(ADD(ADD(ADD(STR("ANDV("), v_gen_expr(INDEX(v_node, STR("a")))), STR(", ")), v_gen_expr(INDEX(v_node, STR("b")))), STR(")"));
    }
    if (truthy(EQ(v_k, STR("or")))) {
        return ADD(ADD(ADD(ADD(STR("ORV("), v_gen_expr(INDEX(v_node, STR("a")))), STR(", ")), v_gen_expr(INDEX(v_node, STR("b")))), STR(")"));
    }
    if (truthy(EQ(v_k, STR("bin")))) {
        return ADD(ADD(ADD(ADD(ADD(v_bin_fn(INDEX(v_node, STR("op"))), STR("(")), v_gen_expr(INDEX(v_node, STR("a")))), STR(", ")), v_gen_expr(INDEX(v_node, STR("b")))), STR(")"));
    }
    if (truthy(EQ(v_k, STR("cmp")))) {
        return ADD(ADD(ADD(ADD(ADD(v_cmp_fn(INDEX(v_node, STR("op"))), STR("(")), v_gen_expr(INDEX(v_node, STR("a")))), STR(", ")), v_gen_expr(INDEX(v_node, STR("b")))), STR(")"));
    }
    if (truthy(EQ(v_k, STR("list")))) {
        Value v_parts = MKLIST(0);
        { Value _s1 = INDEX(v_node, STR("items")); long _n1 = (long)LEN(_s1).n;
        for (long _i1 = 0; _i1 < _n1; _i1++) {
            Value v_it = INDEX(_s1, NUM(_i1));
            listpush(v_parts, v_gen_expr(v_it));
        } }
        return ADD(ADD(ADD(STR("MKLIST("), B_text(B_length(INDEX(v_node, STR("items"))))), v_join_pre(v_parts)), STR(")"));
    }
    if (truthy(EQ(v_k, STR("map")))) {
        Value v_parts = MKLIST(0);
        { Value _s2 = INDEX(v_node, STR("pairs")); long _n2 = (long)LEN(_s2).n;
        for (long _i2 = 0; _i2 < _n2; _i2++) {
            Value v_pr = INDEX(_s2, NUM(_i2));
            listpush(v_parts, v_gen_expr(INDEX(v_pr, STR("kn"))));
            listpush(v_parts, v_gen_expr(INDEX(v_pr, STR("vn"))));
        } }
        return ADD(ADD(ADD(STR("MKMAP("), B_text(B_length(INDEX(v_node, STR("pairs"))))), v_join_pre(v_parts)), STR(")"));
    }
    if (truthy(EQ(v_k, STR("index")))) {
        return ADD(ADD(ADD(ADD(STR("INDEX("), v_gen_expr(INDEX(v_node, STR("o")))), STR(", ")), v_gen_expr(INDEX(v_node, STR("idx")))), STR(")"));
    }
    if (truthy(EQ(v_k, STR("slice")))) {
        return ADD(ADD(ADD(ADD(ADD(ADD(STR("SLICE("), v_gen_expr(INDEX(v_node, STR("o")))), STR(", ")), v_gen_expr(INDEX(v_node, STR("a")))), STR(", ")), v_gen_expr(INDEX(v_node, STR("b")))), STR(")"));
    }
    if (truthy(EQ(v_k, STR("call")))) {
        Value v_name = INDEX(v_node, STR("name"));
        if (truthy(EQ(v_name, STR("range")))) {
            if (truthy(EQ(B_length(INDEX(v_node, STR("args"))), NUM(1)))) {
                return ADD(ADD(STR("B_range("), v_gen_expr(INDEX(INDEX(v_node, STR("args")), NUM(0)))), STR(", NUM(0), 0)"));
            }
            return ADD(ADD(ADD(ADD(STR("B_range("), v_gen_expr(INDEX(INDEX(v_node, STR("args")), NUM(0)))), STR(", ")), v_gen_expr(INDEX(INDEX(v_node, STR("args")), NUM(1)))), STR(", 1)"));
        }
        if (truthy(EQ(v_name, STR("serve")))) {
            return ADD(ADD(ADD(ADD(STR("vc_serve((long)("), v_gen_expr(INDEX(INDEX(v_node, STR("args")), NUM(0)))), STR(").n, v_")), INDEX(INDEX(INDEX(v_node, STR("args")), NUM(1)), STR("name"))), STR(")"));
        }
        if (truthy(EQ(v_name, STR("path_join")))) {
            Value v_pj = MKLIST(0);
            { Value _s3 = INDEX(v_node, STR("args")); long _n3 = (long)LEN(_s3).n;
            for (long _i3 = 0; _i3 < _n3; _i3++) {
                Value v_an = INDEX(_s3, NUM(_i3));
                listpush(v_pj, v_gen_expr(v_an));
            } }
            return ADD(ADD(ADD(STR("B_path_join("), B_text(B_length(INDEX(v_node, STR("args"))))), v_join_pre(v_pj)), STR(")"));
        }
        if (truthy(EQ(v_name, STR("http_get")))) {
            if (truthy(EQ(B_length(INDEX(v_node, STR("args"))), NUM(1)))) {
                return ADD(ADD(STR("B_http_get("), v_gen_expr(INDEX(INDEX(v_node, STR("args")), NUM(0)))), STR(", NIL())"));
            }
            return ADD(ADD(ADD(ADD(STR("B_http_get("), v_gen_expr(INDEX(INDEX(v_node, STR("args")), NUM(0)))), STR(", ")), v_gen_expr(INDEX(INDEX(v_node, STR("args")), NUM(1)))), STR(")"));
        }
        Value v_bf = v_builtin_fn(v_name);
        Value v_parts = MKLIST(0);
        { Value _s4 = INDEX(v_node, STR("args")); long _n4 = (long)LEN(_s4).n;
        for (long _i4 = 0; _i4 < _n4; _i4++) {
            Value v_an = INDEX(_s4, NUM(_i4));
            listpush(v_parts, v_gen_expr(v_an));
        } }
        if (truthy(NE(v_bf, STR("")))) {
            return ADD(ADD(ADD(v_bf, STR("(")), B_join(v_parts, STR(", "))), STR(")"));
        }
        return ADD(ADD(ADD(ADD(STR("v_"), v_name), STR("(")), B_join(v_parts, STR(", "))), STR(")"));
    }
    return STR("NIL()");
    return NIL();
}

Value v_join_pre(Value v_parts) {
    Value v_s = STR("");
    { Value _s5 = v_parts; long _n5 = (long)LEN(_s5).n;
    for (long _i5 = 0; _i5 < _n5; _i5++) {
        Value v_p = INDEX(_s5, NUM(_i5));
        v_s = ADD(ADD(v_s, STR(", ")), v_p);
    } }
    return v_s;
    return NIL();
}

Value v_gen_block(Value v_stmts, Value v_ind) {
    Value v_out = STR("");
    { Value _s6 = v_stmts; long _n6 = (long)LEN(_s6).n;
    for (long _i6 = 0; _i6 < _n6; _i6++) {
        Value v_st = INDEX(_s6, NUM(_i6));
        v_out = ADD(v_out, v_gen_stmt(v_st, v_ind));
    } }
    return v_out;
    return NIL();
}

Value v_gen_stmt(Value v_st, Value v_ind) {
    Value v_k = INDEX(v_st, STR("k"));
    if (truthy(EQ(v_k, STR("let")))) {
        return ADD(ADD(ADD(ADD(ADD(v_ind, STR("Value v_")), INDEX(v_st, STR("name"))), STR(" = ")), v_gen_expr(INDEX(v_st, STR("e")))), STR(";\n"));
    }
    if (truthy(EQ(v_k, STR("set")))) {
        return ADD(ADD(ADD(ADD(ADD(v_ind, STR("v_")), INDEX(v_st, STR("name"))), STR(" = ")), v_gen_expr(INDEX(v_st, STR("e")))), STR(";\n"));
    }
    if (truthy(EQ(v_k, STR("setat")))) {
        return ADD(ADD(ADD(ADD(ADD(ADD(ADD(v_ind, STR("SETAT(v_")), INDEX(v_st, STR("name"))), STR(", ")), v_gen_expr(INDEX(v_st, STR("key")))), STR(", ")), v_gen_expr(INDEX(v_st, STR("e")))), STR(");\n"));
    }
    if (truthy(EQ(v_k, STR("add")))) {
        return ADD(ADD(ADD(ADD(ADD(v_ind, STR("listpush(")), v_gen_expr(INDEX(v_st, STR("target")))), STR(", ")), v_gen_expr(INDEX(v_st, STR("val")))), STR(");\n"));
    }
    if (truthy(EQ(v_k, STR("say")))) {
        return ADD(ADD(ADD(v_ind, STR("SAY(")), v_gen_expr(INDEX(v_st, STR("e")))), STR(");\n"));
    }
    if (truthy(EQ(v_k, STR("ret")))) {
        return ADD(ADD(ADD(v_ind, STR("return ")), v_gen_expr(INDEX(v_st, STR("e")))), STR(";\n"));
    }
    if (truthy(EQ(v_k, STR("expr")))) {
        return ADD(ADD(v_ind, v_gen_expr(INDEX(v_st, STR("e")))), STR(";\n"));
    }
    if (truthy(EQ(v_k, STR("if")))) {
        Value v_s = ADD(ADD(ADD(ADD(ADD(ADD(v_ind, STR("if (truthy(")), v_gen_expr(INDEX(v_st, STR("c")))), STR(")) {\n")), v_gen_block(INDEX(v_st, STR("body")), ADD(v_ind, STR("    ")))), v_ind), STR("}"));
        if (truthy(GT(B_length(INDEX(v_st, STR("els"))), NUM(0)))) {
            v_s = ADD(ADD(ADD(ADD(v_s, STR(" else {\n")), v_gen_block(INDEX(v_st, STR("els")), ADD(v_ind, STR("    ")))), v_ind), STR("}"));
        }
        return ADD(v_s, STR("\n"));
    }
    if (truthy(EQ(v_k, STR("while")))) {
        return ADD(ADD(ADD(ADD(ADD(ADD(v_ind, STR("while (truthy(")), v_gen_expr(INDEX(v_st, STR("c")))), STR(")) {\n")), v_gen_block(INDEX(v_st, STR("body")), ADD(v_ind, STR("    ")))), v_ind), STR("}\n"));
    }
    if (truthy(EQ(v_k, STR("for")))) {
        v_LOOPN = ADD(v_LOOPN, NUM(1));
        Value v_id = B_text(v_LOOPN);
        Value v_s = ADD(ADD(ADD(ADD(ADD(ADD(ADD(ADD(ADD(v_ind, STR("{ Value _s")), v_id), STR(" = ")), v_gen_expr(INDEX(v_st, STR("list")))), STR("; long _n")), v_id), STR(" = (long)LEN(_s")), v_id), STR(").n;\n"));
        v_s = ADD(ADD(ADD(ADD(ADD(ADD(ADD(ADD(ADD(ADD(v_s, v_ind), STR("for (long _i")), v_id), STR(" = 0; _i")), v_id), STR(" < _n")), v_id), STR("; _i")), v_id), STR("++) {\n"));
        v_s = ADD(ADD(ADD(ADD(ADD(ADD(ADD(ADD(v_s, v_ind), STR("    Value v_")), INDEX(v_st, STR("var"))), STR(" = INDEX(_s")), v_id), STR(", NUM(_i")), v_id), STR("));\n"));
        v_s = ADD(v_s, v_gen_block(INDEX(v_st, STR("body")), ADD(v_ind, STR("    "))));
        v_s = ADD(ADD(v_s, v_ind), STR("} }\n"));
        return v_s;
    }
    if (truthy(EQ(v_k, STR("repeat")))) {
        v_LOOPN = ADD(v_LOOPN, NUM(1));
        Value v_id = B_text(v_LOOPN);
        Value v_s = ADD(ADD(ADD(ADD(ADD(v_ind, STR("{ long _r")), v_id), STR(" = (long)(")), v_gen_expr(INDEX(v_st, STR("n")))), STR(").n;\n"));
        v_s = ADD(ADD(ADD(ADD(ADD(ADD(ADD(ADD(ADD(ADD(v_s, v_ind), STR("for (long _c")), v_id), STR(" = 0; _c")), v_id), STR(" < _r")), v_id), STR("; _c")), v_id), STR("++) {\n"));
        v_s = ADD(v_s, v_gen_block(INDEX(v_st, STR("body")), ADD(v_ind, STR("    "))));
        v_s = ADD(ADD(v_s, v_ind), STR("} }\n"));
        return v_s;
    }
    if (truthy(EQ(v_k, STR("attempt")))) {
        Value v_s = ADD(v_ind, STR("{ int _sp = g_jmpsp; if (setjmp(g_jmp[g_jmpsp++]) == 0) {\n"));
        v_s = ADD(v_s, v_gen_block(INDEX(v_st, STR("body")), ADD(v_ind, STR("    "))));
        v_s = ADD(ADD(ADD(ADD(v_s, v_ind), STR("    g_jmpsp = _sp;\n")), v_ind), STR("} else {\n"));
        v_s = ADD(ADD(ADD(ADD(v_s, v_ind), STR("    g_jmpsp = _sp; Value v_")), INDEX(v_st, STR("errname"))), STR(" = g_err;\n"));
        v_s = ADD(v_s, v_gen_block(INDEX(v_st, STR("rescue")), ADD(v_ind, STR("    "))));
        v_s = ADD(ADD(v_s, v_ind), STR("} }\n"));
        return v_s;
    }
    return STR("");
    return NIL();
}

Value v_params_proto(Value v_params) {
    if (truthy(EQ(B_length(v_params), NUM(0)))) {
        return STR("void");
    }
    Value v_p = MKLIST(0);
    { Value _s7 = v_params; long _n7 = (long)LEN(_s7).n;
    for (long _i7 = 0; _i7 < _n7; _i7++) {
        Value v_x = INDEX(_s7, NUM(_i7));
        listpush(v_p, STR("Value"));
    } }
    return B_join(v_p, STR(", "));
    return NIL();
}

Value v_params_decl(Value v_params) {
    if (truthy(EQ(B_length(v_params), NUM(0)))) {
        return STR("void");
    }
    Value v_p = MKLIST(0);
    { Value _s8 = v_params; long _n8 = (long)LEN(_s8).n;
    for (long _i8 = 0; _i8 < _n8; _i8++) {
        Value v_x = INDEX(_s8, NUM(_i8));
        listpush(v_p, ADD(STR("Value v_"), v_x));
    } }
    return B_join(v_p, STR(", "));
    return NIL();
}

Value v_compile_prog(Value v_src) {
    Value v_prog = MKLIST(0);
    { Value _s9 = B_split(v_src, STR("\n")); long _n9 = (long)LEN(_s9).n;
    for (long _i9 = 0; _i9 < _n9; _i9++) {
        Value v_line = INDEX(_s9, NUM(_i9));
        Value v_toks = v_lex_line(v_line);
        if (truthy(GT(B_length(v_toks), NUM(0)))) {
            listpush(v_prog, v_toks);
        }
    } }
    Value v_block = v_parse_block(v_prog, NUM(0));
    Value v_funcs = MKLIST(0);
    Value v_top = MKLIST(0);
    { Value _s10 = INDEX(v_block, STR("stmts")); long _n10 = (long)LEN(_s10).n;
    for (long _i10 = 0; _i10 < _n10; _i10++) {
        Value v_st = INDEX(_s10, NUM(_i10));
        if (truthy(EQ(INDEX(v_st, STR("k")), STR("func")))) {
            listpush(v_funcs, v_st);
        } else {
            listpush(v_top, v_st);
        }
    } }
    Value v_c = ADD(B_b64decode(v_RUNTIME_B64), STR("\n"));
    Value v_mainstmts = MKLIST(0);
    { Value _s11 = v_top; long _n11 = (long)LEN(_s11).n;
    for (long _i11 = 0; _i11 < _n11; _i11++) {
        Value v_st = INDEX(_s11, NUM(_i11));
        if (truthy(EQ(INDEX(v_st, STR("k")), STR("let")))) {
            v_c = ADD(ADD(ADD(v_c, STR("Value v_")), INDEX(v_st, STR("name"))), STR(";\n"));
            listpush(v_mainstmts, MKMAP(3, STR("k"), STR("set"), STR("name"), INDEX(v_st, STR("name")), STR("e"), INDEX(v_st, STR("e"))));
        } else {
            listpush(v_mainstmts, v_st);
        }
    } }
    v_c = ADD(v_c, STR("\n"));
    { Value _s12 = v_funcs; long _n12 = (long)LEN(_s12).n;
    for (long _i12 = 0; _i12 < _n12; _i12++) {
        Value v_f = INDEX(_s12, NUM(_i12));
        v_c = ADD(ADD(ADD(ADD(ADD(v_c, STR("Value v_")), INDEX(v_f, STR("name"))), STR("(")), v_params_proto(INDEX(v_f, STR("params")))), STR(");\n"));
    } }
    v_c = ADD(v_c, STR("\n"));
    { Value _s13 = v_funcs; long _n13 = (long)LEN(_s13).n;
    for (long _i13 = 0; _i13 < _n13; _i13++) {
        Value v_f = INDEX(_s13, NUM(_i13));
        v_c = ADD(ADD(ADD(ADD(ADD(v_c, STR("Value v_")), INDEX(v_f, STR("name"))), STR("(")), v_params_decl(INDEX(v_f, STR("params")))), STR(") {\n"));
        v_c = ADD(v_c, v_gen_block(INDEX(v_f, STR("body")), STR("    ")));
        v_c = ADD(v_c, STR("    return NIL();\n}\n\n"));
    } }
    v_c = ADD(ADD(ADD(v_c, STR("int main(int argc, char** argv) {\n    g_argc = argc; g_argv = argv;\n")), v_gen_block(v_mainstmts, STR("    "))), STR("    return 0;\n}\n"));
    return v_c;
    return NIL();
}

Value v_compile_kernel(Value v_src) {
    Value v_prog = MKLIST(0);
    { Value _s14 = B_split(v_src, STR("\n")); long _n14 = (long)LEN(_s14).n;
    for (long _i14 = 0; _i14 < _n14; _i14++) {
        Value v_line = INDEX(_s14, NUM(_i14));
        Value v_toks = v_lex_line(v_line);
        if (truthy(GT(B_length(v_toks), NUM(0)))) {
            listpush(v_prog, v_toks);
        }
    } }
    Value v_block = v_parse_block(v_prog, NUM(0));
    Value v_funcs = MKLIST(0);
    Value v_top = MKLIST(0);
    { Value _s15 = INDEX(v_block, STR("stmts")); long _n15 = (long)LEN(_s15).n;
    for (long _i15 = 0; _i15 < _n15; _i15++) {
        Value v_st = INDEX(_s15, NUM(_i15));
        if (truthy(EQ(INDEX(v_st, STR("k")), STR("func")))) {
            listpush(v_funcs, v_st);
        } else {
            listpush(v_top, v_st);
        }
    } }
    Value v_c = STR("");
    Value v_mainstmts = MKLIST(0);
    { Value _s16 = v_top; long _n16 = (long)LEN(_s16).n;
    for (long _i16 = 0; _i16 < _n16; _i16++) {
        Value v_st = INDEX(_s16, NUM(_i16));
        if (truthy(EQ(INDEX(v_st, STR("k")), STR("let")))) {
            v_c = ADD(ADD(ADD(v_c, STR("Value v_")), INDEX(v_st, STR("name"))), STR(";\n"));
            listpush(v_mainstmts, MKMAP(3, STR("k"), STR("set"), STR("name"), INDEX(v_st, STR("name")), STR("e"), INDEX(v_st, STR("e"))));
        } else {
            listpush(v_mainstmts, v_st);
        }
    } }
    v_c = ADD(v_c, STR("\n"));
    { Value _s17 = v_funcs; long _n17 = (long)LEN(_s17).n;
    for (long _i17 = 0; _i17 < _n17; _i17++) {
        Value v_f = INDEX(_s17, NUM(_i17));
        v_c = ADD(ADD(ADD(ADD(ADD(v_c, STR("Value v_")), INDEX(v_f, STR("name"))), STR("(")), v_params_proto(INDEX(v_f, STR("params")))), STR(");\n"));
    } }
    v_c = ADD(v_c, STR("\n"));
    { Value _s18 = v_funcs; long _n18 = (long)LEN(_s18).n;
    for (long _i18 = 0; _i18 < _n18; _i18++) {
        Value v_f = INDEX(_s18, NUM(_i18));
        v_c = ADD(ADD(ADD(ADD(ADD(v_c, STR("Value v_")), INDEX(v_f, STR("name"))), STR("(")), v_params_decl(INDEX(v_f, STR("params")))), STR(") {\n"));
        v_c = ADD(v_c, v_gen_block(INDEX(v_f, STR("body")), STR("    ")));
        v_c = ADD(v_c, STR("    return NIL();\n}\n\n"));
    } }
    v_c = ADD(ADD(ADD(v_c, STR("void kmain(void) {\n")), v_gen_block(v_mainstmts, STR("    "))), STR("}\n"));
    return v_c;
    return NIL();
}

Value v_build_and_run(Value v_src, Value v_base) {
    Value v_c = v_compile_prog(v_src);
    Value v_cfile = ADD(v_base, STR(".c"));
    B_write_file(v_cfile, v_c);
    Value v_exe = ADD(v_base, STR(".bin"));
    Value v_r = B_shell(ADD(ADD(ADD(STR("cc -O2 -w "), v_cfile), STR(" -o ")), v_exe));
    if (truthy(NE(INDEX(v_r, STR("code")), NUM(0)))) {
        SAY(STR("C compile error:"));
        SAY(INDEX(v_r, STR("output")));
        return NIL();
    }
    SAY(ADD(ADD(STR("compiled -> "), v_exe), STR(" (a native binary, no Python)")));
    SAY(STR("----"));
    Value v_runcmd = v_exe;
    if (truthy(EQ(B_starts_with(v_exe, STR("/")), BOOLV(0)))) {
        v_runcmd = ADD(STR("./"), v_exe);
    }
    SAY(B_run(v_runcmd));
    return NIL();
}

Value v_compile_only(Value v_src, Value v_base) {
    Value v_c = v_compile_prog(v_src);
    B_write_file(ADD(v_base, STR(".c")), v_c);
    Value v_r = B_shell(ADD(ADD(ADD(ADD(STR("cc -O2 -w "), v_base), STR(".c -o ")), v_base), STR(".bin")));
    if (truthy(NE(INDEX(v_r, STR("code")), NUM(0)))) {
        SAY(STR("C compile error:"));
        SAY(INDEX(v_r, STR("output")));
        return NIL();
    }
    SAY(ADD(ADD(STR("compiled -> "), v_base), STR(".bin")));
    return NIL();
}

int main(int argc, char** argv) {
    g_argc = argc; g_argv = argv;
    v_LOOPN = NUM(0);
    v_RUNTIME_B64 = STR("I2luY2x1ZGUgPHN0ZGlvLmg+CiNpbmNsdWRlIDxzdGRsaWIuaD4KI2luY2x1ZGUgPHN0cmluZy5oPgojaW5jbHVkZSA8c3RkYXJnLmg+CiNpbmNsdWRlIDxjdHlwZS5oPgojaW5jbHVkZSA8c3lzL3NvY2tldC5oPgojaW5jbHVkZSA8bmV0aW5ldC9pbi5oPgojaW5jbHVkZSA8YXJwYS9pbmV0Lmg+CiNpbmNsdWRlIDx1bmlzdGQuaD4KI2luY2x1ZGUgPHN5cy9zdGF0Lmg+CiNpbmNsdWRlIDxkaXJlbnQuaD4KI2luY2x1ZGUgPHRpbWUuaD4KCnR5cGVkZWYgZW51bSB7IFROLCBUUywgVEIsIFRMLCBUTSwgVFggfSBUYWc7CnR5cGVkZWYgc3RydWN0IFZhbHVlIFZhbHVlOwp0eXBlZGVmIHN0cnVjdCB7IFZhbHVlKiBpdGVtczsgbG9uZyBsZW4sIGNhcDsgaW50IHBlcm07IH0gTGlzdDsKdHlwZWRlZiBzdHJ1Y3QgeyBjaGFyKioga2V5czsgVmFsdWUqIHZhbHM7IGxvbmcgbGVuLCBjYXA7IGludCBwZXJtOyB9IE1hcDsKc3RydWN0IFZhbHVlIHsgVGFnIHQ7IGRvdWJsZSBuOyBjaGFyKiBzOyBMaXN0KiBsOyBNYXAqIG07IH07CgovKiA9PT09PSBFYmIgR0M6IHRyYWNrIGFsbG9jYXRpb25zIGR1cmluZyBhIHJlcXVlc3QsIGZyZWUgb24gZWJiLCBwcm9tb3RlIGVzY2FwZXMgPT09PT0gKi8Kc3RhdGljIHZvaWQqKiBnX3JlYz0wOyBzdGF0aWMgbG9uZyBnX3JlY2xlbj0wLGdfcmVjY2FwPTA7IHN0YXRpYyBpbnQgZ19pbl9yZXE9MDsKc3RhdGljIHZvaWQgZ19yZWNvcmQodm9pZCogcCl7IGlmKGdfcmVjbGVuPj1nX3JlY2NhcCl7IGdfcmVjY2FwPWdfcmVjY2FwP2dfcmVjY2FwKjI6MjA0ODsgZ19yZWM9cmVhbGxvYyhnX3JlYyxnX3JlY2NhcCpzaXplb2Yodm9pZCopKTsgfSBnX3JlY1tnX3JlY2xlbisrXT1wOyB9CnN0YXRpYyB2b2lkKiBnYWxsb2MobG9uZyBuKXsgdm9pZCogcD1tYWxsb2Mobik7IGlmKGdfaW5fcmVxKSBnX3JlY29yZChwKTsgcmV0dXJuIHA7IH0Kc3RhdGljIGNoYXIqIGdzdHJkdXAoY29uc3QgY2hhciogcyl7IGlmKCFzKXM9IiI7IGxvbmcgbj1zdHJsZW4ocykrMTsgY2hhciogcj1nYWxsb2Mobik7IG1lbWNweShyLHMsbik7IHJldHVybiByOyB9CnN0YXRpYyB2b2lkKiBncmVhbGxvYyh2b2lkKiBvbGQsbG9uZyBuKXsgdm9pZCogcD1yZWFsbG9jKG9sZCxuKTsgaWYocCE9b2xkKXsgZm9yKGxvbmcgaT1nX3JlY2xlbi0xO2k+PTA7aS0tKSBpZihnX3JlY1tpXT09b2xkKXsgZ19yZWNbaV09cDsgYnJlYWs7IH0gfSByZXR1cm4gcDsgfQpzdGF0aWMgdm9pZCBnX3VucmVjKHZvaWQqIHApeyBmb3IobG9uZyBpPWdfcmVjbGVuLTE7aT49MDtpLS0pIGlmKGdfcmVjW2ldPT1wKXsgZ19yZWNbaV09Z19yZWNbLS1nX3JlY2xlbl07IHJldHVybjsgfSB9CnN0YXRpYyB2b2lkIGViYih2b2lkKXsgZm9yKGxvbmcgaT0wO2k8Z19yZWNsZW47aSsrKSBmcmVlKGdfcmVjW2ldKTsgZ19yZWNsZW49MDsgfQpzdGF0aWMgdm9pZCBwaW4oVmFsdWUgdil7IGlmKHYudD09VFMpeyBpZih2LnMpIGdfdW5yZWModi5zKTsgcmV0dXJuOyB9IGlmKHYudD09VEwpeyBpZih2LmwtPnBlcm0pIHJldHVybjsgZ191bnJlYyh2LmwpOyBnX3VucmVjKHYubC0+aXRlbXMpOyB2LmwtPnBlcm09MTsgZm9yKGxvbmcgaT0wO2k8di5sLT5sZW47aSsrKSBwaW4odi5sLT5pdGVtc1tpXSk7IHJldHVybjsgfSBpZih2LnQ9PVRNKXsgaWYodi5tLT5wZXJtKSByZXR1cm47IGdfdW5yZWModi5tKTsgZ191bnJlYyh2Lm0tPmtleXMpOyBnX3VucmVjKHYubS0+dmFscyk7IHYubS0+cGVybT0xOyBmb3IobG9uZyBpPTA7aTx2Lm0tPmxlbjtpKyspeyBnX3VucmVjKHYubS0+a2V5c1tpXSk7IHBpbih2Lm0tPnZhbHNbaV0pOyB9IHJldHVybjsgfSB9CgpzdGF0aWMgaW50IGdfYXJnYz0wOyBzdGF0aWMgY2hhcioqIGdfYXJndj0wOwoKc3RhdGljIFZhbHVlIE5VTShkb3VibGUgbil7IFZhbHVlIHY7IHYudD1UTjsgdi5uPW47IHYucz0wOyB2Lmw9MDsgdi5tPTA7IHJldHVybiB2OyB9CnN0YXRpYyBWYWx1ZSBCT09MVihpbnQgYil7IFZhbHVlIHY9TlVNKGI/MTowKTsgdi50PVRCOyByZXR1cm4gdjsgfQpzdGF0aWMgVmFsdWUgTklMKHZvaWQpeyBWYWx1ZSB2PU5VTSgwKTsgdi50PVRYOyByZXR1cm4gdjsgfQpzdGF0aWMgVmFsdWUgU1RSKGNvbnN0IGNoYXIqIHMpeyBWYWx1ZSB2OyB2LnQ9VFM7IHYucz1nc3RyZHVwKHM/czoiIik7IHYubj0wOyB2Lmw9MDsgdi5tPTA7IHJldHVybiB2OyB9CnN0YXRpYyBjaGFyKiBudW1zdHIoZG91YmxlIGQpeyBjaGFyKiBiPWdhbGxvYyg0MCk7IGlmKGQ9PShsb25nKWQpIHNwcmludGYoYiwiJWxkIiwobG9uZylkKTsgZWxzZSBzcHJpbnRmKGIsIiVnIixkKTsgcmV0dXJuIGI7IH0Kc3RhdGljIGNoYXIqIHRvc3RyKFZhbHVlIHYpewogIGlmKHYudD09VFMpIHJldHVybiB2LnM7CiAgaWYodi50PT1UTikgcmV0dXJuIG51bXN0cih2Lm4pOwogIGlmKHYudD09VEIpIHJldHVybiB2Lm4hPTA/InllcyI6Im5vIjsKICBpZih2LnQ9PVRYKSByZXR1cm4gIm5vdGhpbmciOwogIGlmKHYudD09VEwpeyBsb25nIGNhcD0yNTYsbGVuPTE7IGNoYXIqIG89Z2FsbG9jKGNhcCk7IG9bMF09J1snOyBmb3IobG9uZyBpPTA7aTx2LmwtPmxlbjtpKyspeyBjaGFyKiBwPXRvc3RyKHYubC0+aXRlbXNbaV0pOyBsb25nIGxwPXN0cmxlbihwKTsgbG9uZyBuZWVkPWxlbitscCs0OyBpZihuZWVkPmNhcCl7IGNhcD1uZWVkKjI7IG89Z3JlYWxsb2MobyxjYXApO30gaWYoaSl7IG9bbGVuKytdPScsJzsgb1tsZW4rK109JyAnOyB9IG1lbWNweShvK2xlbixwLGxwKTsgbGVuKz1scDsgfSBvW2xlbisrXT0nXSc7IG9bbGVuXT0wOyByZXR1cm4gbzsgfQogIGlmKHYudD09VE0peyBsb25nIGNhcD0yNTYsbGVuPTE7IGNoYXIqIG89Z2FsbG9jKGNhcCk7IG9bMF09J3snOyBmb3IobG9uZyBpPTA7aTx2Lm0tPmxlbjtpKyspeyBjaGFyKiBrPXYubS0+a2V5c1tpXTsgY2hhciogcD10b3N0cih2Lm0tPnZhbHNbaV0pOyBsb25nIGxrPXN0cmxlbihrKSxscD1zdHJsZW4ocCk7IGxvbmcgbmVlZD1sZW4rbGsrbHArNjsgaWYobmVlZD5jYXApeyBjYXA9bmVlZCoyOyBvPWdyZWFsbG9jKG8sY2FwKTt9IGlmKGkpeyBvW2xlbisrXT0nLCc7IG9bbGVuKytdPScgJzsgfSBtZW1jcHkobytsZW4sayxsayk7IGxlbis9bGs7IG9bbGVuKytdPSc6Jzsgb1tsZW4rK109JyAnOyBtZW1jcHkobytsZW4scCxscCk7IGxlbis9bHA7IH0gb1tsZW4rK109J30nOyBvW2xlbl09MDsgcmV0dXJuIG87IH0KICByZXR1cm4gIiI7Cn0Kc3RhdGljIGludCB0cnV0aHkoVmFsdWUgdil7IGlmKHYudD09VFgpIHJldHVybiAwOyBpZih2LnQ9PVRCKSByZXR1cm4gdi5uIT0wOyByZXR1cm4gMTsgfQpzdGF0aWMgaW50IHZlcShWYWx1ZSBhLCBWYWx1ZSBiKXsgaWYoKGEudD09VE58fGEudD09VEIpJiYoYi50PT1UTnx8Yi50PT1UQikpIHJldHVybiBhLm49PWIubjsgaWYoYS50IT1iLnQpIHJldHVybiAwOyBpZihhLnQ9PVRTKSByZXR1cm4gc3RyY21wKGEucyxiLnMpPT0wOyBpZihhLnQ9PVRYKSByZXR1cm4gMTsgcmV0dXJuIDA7IH0Kc3RhdGljIFZhbHVlIEFERChWYWx1ZSBhLCBWYWx1ZSBiKXsgaWYoYS50PT1UTiYmYi50PT1UTikgcmV0dXJuIE5VTShhLm4rYi5uKTsgY2hhciogeD10b3N0cihhKTsgY2hhciogeT10b3N0cihiKTsgY2hhciogcj1tYWxsb2Moc3RybGVuKHgpK3N0cmxlbih5KSsxKTsgc3RyY3B5KHIseCk7IHN0cmNhdChyLHkpOyBWYWx1ZSB2PVNUUihyKTsgZnJlZShyKTsgcmV0dXJuIHY7IH0Kc3RhdGljIFZhbHVlIFNVQihWYWx1ZSBhLFZhbHVlIGIpeyByZXR1cm4gTlVNKGEubi1iLm4pOyB9CnN0YXRpYyBWYWx1ZSBNVUwoVmFsdWUgYSxWYWx1ZSBiKXsgcmV0dXJuIE5VTShhLm4qYi5uKTsgfQpzdGF0aWMgVmFsdWUgRElWVihWYWx1ZSBhLFZhbHVlIGIpeyByZXR1cm4gTlVNKGEubi9iLm4pOyB9CnN0YXRpYyBWYWx1ZSBORUcoVmFsdWUgYSl7IHJldHVybiBOVU0oLWEubik7IH0Kc3RhdGljIFZhbHVlIEVRKFZhbHVlIGEsVmFsdWUgYil7IHJldHVybiBCT09MVih2ZXEoYSxiKSk7IH0Kc3RhdGljIFZhbHVlIE5FKFZhbHVlIGEsVmFsdWUgYil7IHJldHVybiBCT09MVighdmVxKGEsYikpOyB9CnN0YXRpYyBWYWx1ZSBMVChWYWx1ZSBhLFZhbHVlIGIpeyBpZihhLnQ9PVRTJiZiLnQ9PVRTKSByZXR1cm4gQk9PTFYoc3RyY21wKGEucyxiLnMpPDApOyByZXR1cm4gQk9PTFYoYS5uPGIubik7IH0Kc3RhdGljIFZhbHVlIEdUKFZhbHVlIGEsVmFsdWUgYil7IGlmKGEudD09VFMmJmIudD09VFMpIHJldHVybiBCT09MVihzdHJjbXAoYS5zLGIucyk+MCk7IHJldHVybiBCT09MVihhLm4+Yi5uKTsgfQpzdGF0aWMgVmFsdWUgTEUoVmFsdWUgYSxWYWx1ZSBiKXsgaWYoYS50PT1UUyYmYi50PT1UUykgcmV0dXJuIEJPT0xWKHN0cmNtcChhLnMsYi5zKTw9MCk7IHJldHVybiBCT09MVihhLm48PWIubik7IH0Kc3RhdGljIFZhbHVlIEdFKFZhbHVlIGEsVmFsdWUgYil7IGlmKGEudD09VFMmJmIudD09VFMpIHJldHVybiBCT09MVihzdHJjbXAoYS5zLGIucyk+PTApOyByZXR1cm4gQk9PTFYoYS5uPj1iLm4pOyB9CnN0YXRpYyBWYWx1ZSBBTkRWKFZhbHVlIGEsVmFsdWUgYil7IHJldHVybiBCT09MVih0cnV0aHkoYSkmJnRydXRoeShiKSk7IH0Kc3RhdGljIFZhbHVlIE9SVihWYWx1ZSBhLFZhbHVlIGIpeyByZXR1cm4gQk9PTFYodHJ1dGh5KGEpfHx0cnV0aHkoYikpOyB9CnN0YXRpYyBWYWx1ZSBOT1RWKFZhbHVlIGEpeyByZXR1cm4gQk9PTFYoIXRydXRoeShhKSk7IH0Kc3RhdGljIExpc3QqIG5ld2xpc3Qodm9pZCl7IExpc3QqIGw9Z2FsbG9jKHNpemVvZihMaXN0KSk7IGwtPmxlbj0wOyBsLT5jYXA9ODsgbC0+aXRlbXM9Z2FsbG9jKHNpemVvZihWYWx1ZSkqOCk7IGwtPnBlcm09IWdfaW5fcmVxOyByZXR1cm4gbDsgfQpzdGF0aWMgVmFsdWUgTElTVDAodm9pZCl7IFZhbHVlIHY7IHYudD1UTDsgdi5sPW5ld2xpc3QoKTsgdi5zPTA7IHYubT0wOyB2Lm49MDsgcmV0dXJuIHY7IH0Kc3RhdGljIHZvaWQgbGlzdHB1c2goVmFsdWUgbHYsIFZhbHVlIHgpeyBMaXN0KiBsPWx2Lmw7IGlmKGwtPnBlcm0pIHBpbih4KTsgaWYobC0+bGVuPj1sLT5jYXApeyBsLT5jYXAqPTI7IGwtPml0ZW1zPWdyZWFsbG9jKGwtPml0ZW1zLHNpemVvZihWYWx1ZSkqbC0+Y2FwKTt9IGwtPml0ZW1zW2wtPmxlbisrXT14OyB9CnN0YXRpYyBWYWx1ZSBNS0xJU1QoaW50IG4sIC4uLil7IFZhbHVlIHY9TElTVDAoKTsgdmFfbGlzdCBhcDsgdmFfc3RhcnQoYXAsbik7IGZvcihpbnQgaT0wO2k8bjtpKyspIGxpc3RwdXNoKHYsIHZhX2FyZyhhcCxWYWx1ZSkpOyB2YV9lbmQoYXApOyByZXR1cm4gdjsgfQpzdGF0aWMgTWFwKiBuZXdtYXAodm9pZCl7IE1hcCogbT1nYWxsb2Moc2l6ZW9mKE1hcCkpOyBtLT5sZW49MDsgbS0+Y2FwPTg7IG0tPmtleXM9Z2FsbG9jKHNpemVvZihjaGFyKikqOCk7IG0tPnZhbHM9Z2FsbG9jKHNpemVvZihWYWx1ZSkqOCk7IG0tPnBlcm09IWdfaW5fcmVxOyByZXR1cm4gbTsgfQpzdGF0aWMgVmFsdWUgTUFQMCh2b2lkKXsgVmFsdWUgdjsgdi50PVRNOyB2Lm09bmV3bWFwKCk7IHYucz0wOyB2Lmw9MDsgdi5uPTA7IHJldHVybiB2OyB9CnN0YXRpYyB2b2lkIG1hcHNldChWYWx1ZSBtdiwgVmFsdWUgaywgVmFsdWUgdmFsKXsgTWFwKiBtPW12Lm07IGNoYXIqIGtleT10b3N0cihrKTsgaWYobS0+cGVybSkgcGluKHZhbCk7IGZvcihsb25nIGk9MDtpPG0tPmxlbjtpKyspIGlmKHN0cmNtcChtLT5rZXlzW2ldLGtleSk9PTApeyBtLT52YWxzW2ldPXZhbDsgcmV0dXJuOyB9IGlmKG0tPmxlbj49bS0+Y2FwKXsgbS0+Y2FwKj0yOyBtLT5rZXlzPWdyZWFsbG9jKG0tPmtleXMsc2l6ZW9mKGNoYXIqKSptLT5jYXApOyBtLT52YWxzPWdyZWFsbG9jKG0tPnZhbHMsc2l6ZW9mKFZhbHVlKSptLT5jYXApO30gbS0+a2V5c1ttLT5sZW5dPShtLT5wZXJtP3N0cmR1cChrZXkpOmdzdHJkdXAoa2V5KSk7IG0tPnZhbHNbbS0+bGVuXT12YWw7IG0tPmxlbisrOyB9CnN0YXRpYyBWYWx1ZSBNS01BUChpbnQgbiwgLi4uKXsgVmFsdWUgdj1NQVAwKCk7IHZhX2xpc3QgYXA7IHZhX3N0YXJ0KGFwLG4pOyBmb3IoaW50IGk9MDtpPG47aSsrKXsgVmFsdWUgaz12YV9hcmcoYXAsVmFsdWUpOyBWYWx1ZSB2YWw9dmFfYXJnKGFwLFZhbHVlKTsgbWFwc2V0KHYsayx2YWwpO30gdmFfZW5kKGFwKTsgcmV0dXJuIHY7IH0Kc3RhdGljIFZhbHVlIElOREVYKFZhbHVlIGMsIFZhbHVlIGspewogIGlmKGMudD09VEwpeyBsb25nIGk9KGxvbmcpay5uOyBpZihpPDApaSs9Yy5sLT5sZW47IGlmKGk8MHx8aT49Yy5sLT5sZW4pIHJldHVybiBOSUwoKTsgcmV0dXJuIGMubC0+aXRlbXNbaV07IH0KICBpZihjLnQ9PVRNKXsgY2hhcioga2V5PXRvc3RyKGspOyBmb3IobG9uZyBpPTA7aTxjLm0tPmxlbjtpKyspIGlmKHN0cmNtcChjLm0tPmtleXNbaV0sa2V5KT09MCkgcmV0dXJuIGMubS0+dmFsc1tpXTsgcmV0dXJuIE5JTCgpOyB9CiAgaWYoYy50PT1UUyl7IGxvbmcgTD1zdHJsZW4oYy5zKTsgbG9uZyBpPShsb25nKWsubjsgaWYoaTwwKWkrPUw7IGlmKGk8MHx8aT49TCkgcmV0dXJuIFNUUigiIik7IGNoYXIgYlsyXT17Yy5zW2ldLDB9OyByZXR1cm4gU1RSKGIpOyB9CiAgcmV0dXJuIE5JTCgpOwp9CnN0YXRpYyB2b2lkIFNFVEFUKFZhbHVlIGMsIFZhbHVlIGssIFZhbHVlIHZhbCl7IGlmKGMudD09VEwpeyBsb25nIGk9KGxvbmcpay5uOyBpZihpPj0wJiZpPGMubC0+bGVuKSBjLmwtPml0ZW1zW2ldPXZhbDsgfSBlbHNlIGlmKGMudD09VE0pIG1hcHNldChjLGssdmFsKTsgfQpzdGF0aWMgVmFsdWUgU0xJQ0UoVmFsdWUgYywgVmFsdWUgYSwgVmFsdWUgYil7IGxvbmcgbG89KGxvbmcpYS5uLCBoaT0obG9uZyliLm47CiAgaWYoYy50PT1UUyl7IGxvbmcgTD1zdHJsZW4oYy5zKTsgaWYobG88MClsbys9TDsgaWYoaGk8MCloaSs9TDsgaWYobG88MClsbz0wOyBpZihoaT5MKWhpPUw7IGlmKGhpPGxvKWhpPWxvOyBjaGFyKiByPW1hbGxvYyhoaS1sbysxKTsgbWVtY3B5KHIsYy5zK2xvLGhpLWxvKTsgcltoaS1sb109MDsgVmFsdWUgdj1TVFIocik7IGZyZWUocik7IHJldHVybiB2OyB9CiAgaWYoYy50PT1UTCl7IFZhbHVlIHY9TElTVDAoKTsgbG9uZyBMPWMubC0+bGVuOyBpZihsbzwwKWxvKz1MOyBpZihoaTwwKWhpKz1MOyBpZihsbzwwKWxvPTA7IGlmKGhpPkwpaGk9TDsgZm9yKGxvbmcgaT1sbztpPGhpO2krKykgbGlzdHB1c2godixjLmwtPml0ZW1zW2ldKTsgcmV0dXJuIHY7IH0KICByZXR1cm4gTklMKCk7Cn0Kc3RhdGljIFZhbHVlIExFTihWYWx1ZSB2KXsgaWYodi50PT1UUykgcmV0dXJuIE5VTShzdHJsZW4odi5zKSk7IGlmKHYudD09VEwpIHJldHVybiBOVU0odi5sLT5sZW4pOyBpZih2LnQ9PVRNKSByZXR1cm4gTlVNKHYubS0+bGVuKTsgcmV0dXJuIE5VTSgwKTsgfQpzdGF0aWMgVmFsdWUgSU5PUChWYWx1ZSBhLCBWYWx1ZSBiKXsgaWYoYi50PT1UUyYmYS50PT1UUykgcmV0dXJuIEJPT0xWKHN0cnN0cihiLnMsYS5zKSE9MCk7IGlmKGIudD09VEwpeyBmb3IobG9uZyBpPTA7aTxiLmwtPmxlbjtpKyspIGlmKHZlcShhLGIubC0+aXRlbXNbaV0pKSByZXR1cm4gQk9PTFYoMSk7IHJldHVybiBCT09MVigwKTt9IGlmKGIudD09VE0peyBjaGFyKiBrZXk9dG9zdHIoYSk7IGZvcihsb25nIGk9MDtpPGIubS0+bGVuO2krKykgaWYoc3RyY21wKGIubS0+a2V5c1tpXSxrZXkpPT0wKSByZXR1cm4gQk9PTFYoMSk7IHJldHVybiBCT09MVigwKTt9IHJldHVybiBCT09MVigwKTsgfQpzdGF0aWMgdm9pZCBTQVkoVmFsdWUgdil7IHByaW50ZigiJXNcbiIsIHRvc3RyKHYpKTsgfQpzdGF0aWMgVmFsdWUgQl90ZXh0KFZhbHVlIGEpeyByZXR1cm4gU1RSKHRvc3RyKGEpKTsgfQpzdGF0aWMgVmFsdWUgQl9sZW5ndGgoVmFsdWUgYSl7IHJldHVybiBMRU4oYSk7IH0Kc3RhdGljIFZhbHVlIEJfa2V5cyhWYWx1ZSBtKXsgVmFsdWUgdj1MSVNUMCgpOyBpZihtLnQ9PVRNKSBmb3IobG9uZyBpPTA7aTxtLm0tPmxlbjtpKyspIGxpc3RwdXNoKHYsU1RSKG0ubS0+a2V5c1tpXSkpOyByZXR1cm4gdjsgfQpzdGF0aWMgVmFsdWUgQl92YWx1ZXMoVmFsdWUgbSl7IFZhbHVlIHY9TElTVDAoKTsgaWYobS50PT1UTSkgZm9yKGxvbmcgaT0wO2k8bS5tLT5sZW47aSsrKSBsaXN0cHVzaCh2LG0ubS0+dmFsc1tpXSk7IHJldHVybiB2OyB9CnN0YXRpYyBWYWx1ZSBCX3JhbmdlKFZhbHVlIGEsIFZhbHVlIGIsIGludCB0d28peyBWYWx1ZSB2PUxJU1QwKCk7IGxvbmcgbG89dHdvPyhsb25nKWEubjowLCBoaT10d28/KGxvbmcpYi5uOihsb25nKWEubjsgZm9yKGxvbmcgaT1sbztpPGhpO2krKykgbGlzdHB1c2godixOVU0oaSkpOyByZXR1cm4gdjsgfQpzdGF0aWMgVmFsdWUgQl91cHBlcihWYWx1ZSBhKXsgY2hhciogcz1nc3RyZHVwKHRvc3RyKGEpKTsgZm9yKGNoYXIqIHA9czsqcDtwKyspKnA9dG91cHBlcigodW5zaWduZWQgY2hhcikqcCk7IHJldHVybiBTVFIocyk7IH0Kc3RhdGljIFZhbHVlIEJfbG93ZXIoVmFsdWUgYSl7IGNoYXIqIHM9Z3N0cmR1cCh0b3N0cihhKSk7IGZvcihjaGFyKiBwPXM7KnA7cCsrKSpwPXRvbG93ZXIoKHVuc2lnbmVkIGNoYXIpKnApOyByZXR1cm4gU1RSKHMpOyB9CnN0YXRpYyBWYWx1ZSBCX3RyaW0oVmFsdWUgYSl7IGNoYXIqIHM9dG9zdHIoYSk7IHdoaWxlKCpzPT0nICd8fCpzPT0nXHQnfHwqcz09J1xuJylzKys7IGxvbmcgZT1zdHJsZW4ocyk7IHdoaWxlKGU+MCYmKHNbZS0xXT09JyAnfHxzW2UtMV09PSdcdCd8fHNbZS0xXT09J1xuJykpZS0tOyBjaGFyKiByPW1hbGxvYyhlKzEpOyBtZW1jcHkocixzLGUpOyByW2VdPTA7IHJldHVybiBTVFIocik7IH0Kc3RhdGljIFZhbHVlIEJfbnVtYmVyKFZhbHVlIGEpeyBpZihhLnQ9PVROKSByZXR1cm4gYTsgcmV0dXJuIE5VTShhdG9mKHRvc3RyKGEpKSk7IH0Kc3RhdGljIFZhbHVlIEJfam9pbihWYWx1ZSBsc3QsIFZhbHVlIHNlcCl7IGlmKGxzdC50IT1UTCkgcmV0dXJuIFNUUigiIik7IGNoYXIqIGQ9dG9zdHIoc2VwKTsgbG9uZyBjYXA9ODE5MjsgY2hhciogbz1tYWxsb2MoY2FwKTsgb1swXT0wOyBsb25nIGxuPTA7IGZvcihsb25nIGk9MDtpPGxzdC5sLT5sZW47aSsrKXsgY2hhciogcGllY2U9dG9zdHIobHN0LmwtPml0ZW1zW2ldKTsgbG9uZyBuZWVkPWxuK3N0cmxlbihwaWVjZSkrc3RybGVuKGQpKzE7IGlmKG5lZWQ+Y2FwKXsgY2FwPW5lZWQqMjsgbz1yZWFsbG9jKG8sY2FwKTt9IGlmKGkpeyBzdHJjYXQobyxkKTt9IHN0cmNhdChvLHBpZWNlKTsgbG49c3RybGVuKG8pO30gcmV0dXJuIFNUUihvKTsgfQpzdGF0aWMgVmFsdWUgQl9zcGxpdChWYWx1ZSBhLCBWYWx1ZSBzZXB2KXsgVmFsdWUgdj1MSVNUMCgpOyBjaGFyKiBzPXRvc3RyKGEpOyBjaGFyKiBzZXA9dG9zdHIoc2Vwdik7IGxvbmcgc2w9c3RybGVuKHNlcCk7IGlmKHNsPT0wKXsgZm9yKGxvbmcgaT0wO3NbaV07aSsrKXsgY2hhciBiWzJdPXtzW2ldLDB9OyBsaXN0cHVzaCh2LFNUUihiKSk7IH0gcmV0dXJuIHY7IH0gY2hhciogcD1zOyBjaGFyKiBxOyB3aGlsZSgocT1zdHJzdHIocCxzZXApKSl7IGxvbmcgbj1xLXA7IGNoYXIqIHI9bWFsbG9jKG4rMSk7IG1lbWNweShyLHAsbik7IHJbbl09MDsgbGlzdHB1c2godixTVFIocikpOyBmcmVlKHIpOyBwPXErc2w7IH0gbGlzdHB1c2godixTVFIocCkpOyByZXR1cm4gdjsgfQpzdGF0aWMgVmFsdWUgQl9zb3J0KFZhbHVlIGxzdCl7IGlmKGxzdC50IT1UTCkgcmV0dXJuIGxzdDsgVmFsdWUgdj1MSVNUMCgpOyBmb3IobG9uZyBpPTA7aTxsc3QubC0+bGVuO2krKykgbGlzdHB1c2godixsc3QubC0+aXRlbXNbaV0pOyBmb3IobG9uZyBpPTE7aTx2LmwtPmxlbjtpKyspeyBWYWx1ZSBrZXk9di5sLT5pdGVtc1tpXTsgbG9uZyBqPWktMTsgd2hpbGUoaj49MCAmJiB0cnV0aHkoR1Qodi5sLT5pdGVtc1tqXSxrZXkpKSl7IHYubC0+aXRlbXNbaisxXT12LmwtPml0ZW1zW2pdOyBqLS07IH0gdi5sLT5pdGVtc1tqKzFdPWtleTsgfSByZXR1cm4gdjsgfQpzdGF0aWMgVmFsdWUgQl9jb250YWlucyhWYWx1ZSBhLCBWYWx1ZSBiKXsgcmV0dXJuIElOT1AoYixhKTsgfQpzdGF0aWMgVmFsdWUgQl9zbGljZShWYWx1ZSBjLCBWYWx1ZSBhLCBWYWx1ZSBiKXsgcmV0dXJuIFNMSUNFKGMsYSxiKTsgfQpzdGF0aWMgVmFsdWUgQl9yZXBsYWNlKFZhbHVlIHMsIFZhbHVlIG9sZHYsIFZhbHVlIG5ld3YpeyBjaGFyKiBzdHI9dG9zdHIocyk7IGNoYXIqIG89dG9zdHIob2xkdik7IGNoYXIqIG53PXRvc3RyKG5ld3YpOyBsb25nIG9sPXN0cmxlbihvKTsgbG9uZyBubD1zdHJsZW4obncpOyBpZihvbD09MCkgcmV0dXJuIFNUUihzdHIpOyBsb25nIGNudD0wOyB7IGNoYXIqIHA9c3RyOyBjaGFyKiBxOyB3aGlsZSgocT1zdHJzdHIocCxvKSkpeyBjbnQrKzsgcD1xK29sOyB9IH0gbG9uZyBvdXRsZW49c3RybGVuKHN0cikrY250KihubC1vbCkrMTsgY2hhciogb3V0PW1hbGxvYyhvdXRsZW4+MD9vdXRsZW46MSk7IGNoYXIqIHc9b3V0OyBjaGFyKiBwPXN0cjsgY2hhciogcTsgd2hpbGUoKHE9c3Ryc3RyKHAsbykpKXsgbG9uZyBwcmU9cS1wOyBtZW1jcHkodyxwLHByZSk7IHcrPXByZTsgbWVtY3B5KHcsbncsbmwpOyB3Kz1ubDsgcD1xK29sOyB9IHN0cmNweSh3LHApOyBWYWx1ZSB2PVNUUihvdXQpOyBmcmVlKG91dCk7IHJldHVybiB2OyB9CnN0YXRpYyBWYWx1ZSBCX3JlYWRfZmlsZShWYWx1ZSBwdGgpeyBGSUxFKiBmPWZvcGVuKHRvc3RyKHB0aCksInJiIik7IGlmKCFmKSByZXR1cm4gU1RSKCIiKTsgZnNlZWsoZiwwLFNFRUtfRU5EKTsgbG9uZyBuPWZ0ZWxsKGYpOyBmc2VlayhmLDAsU0VFS19TRVQpOyBjaGFyKiBiPW1hbGxvYyhuKzEpOyBmcmVhZChiLDEsbixmKTsgYltuXT0wOyBmY2xvc2UoZik7IFZhbHVlIHY9U1RSKGIpOyBmcmVlKGIpOyByZXR1cm4gdjsgfQpzdGF0aWMgVmFsdWUgQl93cml0ZV9maWxlKFZhbHVlIHB0aCwgVmFsdWUgYyl7IEZJTEUqIGY9Zm9wZW4odG9zdHIocHRoKSwid2IiKTsgaWYoZil7IGNoYXIqIHM9dG9zdHIoYyk7IGZ3cml0ZShzLDEsc3RybGVuKHMpLGYpOyBmY2xvc2UoZik7fSByZXR1cm4gTklMKCk7IH0Kc3RhdGljIFZhbHVlIEJfYXBwZW5kX2ZpbGUoVmFsdWUgcHRoLCBWYWx1ZSBjKXsgRklMRSogZj1mb3Blbih0b3N0cihwdGgpLCJhYiIpOyBpZihmKXsgY2hhciogcz10b3N0cihjKTsgZndyaXRlKHMsMSxzdHJsZW4ocyksZik7IGZjbG9zZShmKTt9IHJldHVybiBOSUwoKTsgfQpzdGF0aWMgVmFsdWUgQl9hcmd1bWVudHModm9pZCl7IFZhbHVlIHY9TElTVDAoKTsgZm9yKGludCBpPTE7aTxnX2FyZ2M7aSsrKSBsaXN0cHVzaCh2LFNUUihnX2FyZ3ZbaV0pKTsgcmV0dXJuIHY7IH0Kc3RhdGljIGNoYXIqIHJlYWRwaXBlKEZJTEUqIHApeyBsb25nIGNhcD00MDk2LGxlbj0wOyBjaGFyKiBiPW1hbGxvYyhjYXApOyBpbnQgY2g7IHdoaWxlKChjaD1mZ2V0YyhwKSkhPUVPRil7IGlmKGxlbisxPj1jYXApe2NhcCo9MjtiPXJlYWxsb2MoYixjYXApO30gYltsZW4rK109Y2g7IH0gYltsZW5dPTA7IHJldHVybiBiOyB9CnN0YXRpYyBWYWx1ZSBCX3J1bihWYWx1ZSBjbWQpeyBjaGFyKiBmdWxsPW1hbGxvYyhzdHJsZW4odG9zdHIoY21kKSkrOCk7IHNwcmludGYoZnVsbCwiJXMgMj4mMSIsdG9zdHIoY21kKSk7IEZJTEUqIHA9cG9wZW4oZnVsbCwiciIpOyBmcmVlKGZ1bGwpOyBpZighcCkgcmV0dXJuIFNUUigiIik7IGNoYXIqIGI9cmVhZHBpcGUocCk7IHBjbG9zZShwKTsgbG9uZyBsZW49c3RybGVuKGIpOyB3aGlsZShsZW4+MCYmYltsZW4tMV09PSdcbicpIGJbLS1sZW5dPTA7IFZhbHVlIHY9U1RSKGIpOyBmcmVlKGIpOyByZXR1cm4gdjsgfQpzdGF0aWMgVmFsdWUgQl9zaGVsbChWYWx1ZSBjbWQpeyBjaGFyKiBmdWxsPW1hbGxvYyhzdHJsZW4odG9zdHIoY21kKSkrOCk7IHNwcmludGYoZnVsbCwiJXMgMj4mMSIsdG9zdHIoY21kKSk7IEZJTEUqIHA9cG9wZW4oZnVsbCwiciIpOyBmcmVlKGZ1bGwpOyBpZighcCkgcmV0dXJuIE1LTUFQKDIsU1RSKCJvdXRwdXQiKSxTVFIoIiIpLFNUUigiY29kZSIpLE5VTSgxKSk7IGNoYXIqIGI9cmVhZHBpcGUocCk7IGludCBzdD1wY2xvc2UocCk7IGludCBjb2RlPShzdD09LTEpPzE6KHN0Pj44KTsgbG9uZyBsZW49c3RybGVuKGIpOyB3aGlsZShsZW4+MCYmYltsZW4tMV09PSdcbicpIGJbLS1sZW5dPTA7IFZhbHVlIHY9TUtNQVAoMixTVFIoIm91dHB1dCIpLFNUUihiKSxTVFIoImNvZGUiKSxOVU0oY29kZSkpOyBmcmVlKGIpOyByZXR1cm4gdjsgfQpzdGF0aWMgaW50IGI2NHYoY2hhciBjKXsgaWYoYz49J0EnJiZjPD0nWicpcmV0dXJuIGMtJ0EnOyBpZihjPj0nYScmJmM8PSd6JylyZXR1cm4gYy0nYScrMjY7IGlmKGM+PScwJyYmYzw9JzknKXJldHVybiBjLScwJys1MjsgaWYoYz09JysnKXJldHVybiA2MjsgaWYoYz09Jy8nKXJldHVybiA2MzsgcmV0dXJuIC0xOyB9CnN0YXRpYyBWYWx1ZSBCX2I2NGRlY29kZShWYWx1ZSBzdil7IGNoYXIqIGluPXRvc3RyKHN2KTsgbG9uZyBuPXN0cmxlbihpbik7IGNoYXIqIG91dD1tYWxsb2MobisxKTsgbG9uZyBvPTA7IGludCBidWY9MCxiaXRzPTA7IGZvcihsb25nIGk9MDtpPG47aSsrKXsgaW50IHY9YjY0dihpbltpXSk7IGlmKHY8MCkgY29udGludWU7IGJ1Zj0oYnVmPDw2KXx2OyBiaXRzKz02OyBpZihiaXRzPj04KXsgYml0cy09ODsgb3V0W28rK109KGNoYXIpKChidWY+PmJpdHMpJjB4RkYpOyB9IH0gb3V0W29dPTA7IFZhbHVlIHI9U1RSKG91dCk7IGZyZWUob3V0KTsgcmV0dXJuIHI7IH0Kc3RhdGljIGNoYXIqIHNkdXAoY29uc3QgY2hhciogcyl7IGNoYXIqIHI9bWFsbG9jKHN0cmxlbihzKSsxKTsgc3RyY3B5KHIscyk7IHJldHVybiByOyB9CnN0YXRpYyBWYWx1ZSBCX3VybF9kZWNvZGUoVmFsdWUgdil7IGNoYXIqIHM9dG9zdHIodik7IGxvbmcgbj1zdHJsZW4ocyk7IGNoYXIqIG89bWFsbG9jKG4rMSk7IGxvbmcgaj0wOyBmb3IobG9uZyBpPTA7aTxuO2krKyl7IGlmKHNbaV09PSclJyYmaSsyPG4peyBjaGFyIGg9c1tpKzFdLGw9c1tpKzJdOyBpbnQgaGk9KGg8PSc5Jyk/aC0nMCc6KHRvbG93ZXIoaCktJ2EnKzEwKTsgaW50IGxvPShsPD0nOScpP2wtJzAnOih0b2xvd2VyKGwpLSdhJysxMCk7IG9baisrXT0oY2hhcikoaGkqMTYrbG8pOyBpKz0yOyB9IGVsc2UgaWYoc1tpXT09JysnKSBvW2orK109JyAnOyBlbHNlIG9baisrXT1zW2ldOyB9IG9bal09MDsgVmFsdWUgcj1TVFIobyk7IGZyZWUobyk7IHJldHVybiByOyB9CnN0YXRpYyBWYWx1ZSBCX3VybF9lbmNvZGUoVmFsdWUgdil7IGNoYXIqIHM9dG9zdHIodik7IGxvbmcgbj1zdHJsZW4ocyk7IGNoYXIqIG89bWFsbG9jKG4qMysxKTsgbG9uZyBqPTA7IGZvcihsb25nIGk9MDtpPG47aSsrKXsgdW5zaWduZWQgY2hhciBjPXNbaV07IGlmKChjPj0nQScmJmM8PSdaJyl8fChjPj0nYScmJmM8PSd6Jyl8fChjPj0nMCcmJmM8PSc5Jyl8fGM9PSctJ3x8Yz09J18nfHxjPT0nLid8fGM9PSd+Jykgb1tqKytdPWM7IGVsc2UgeyBzcHJpbnRmKG8raiwiJSUlMDJYIixjKTsgais9MzsgfSB9IG9bal09MDsgVmFsdWUgcj1TVFIobyk7IGZyZWUobyk7IHJldHVybiByOyB9CnN0YXRpYyBWYWx1ZSBCX2h0bWxfZXNjYXBlKFZhbHVlIHYpeyBjaGFyKiBzPXRvc3RyKHYpOyBjaGFyKiBvPW1hbGxvYyhzdHJsZW4ocykqNisxKTsgY2hhciogdz1vOyBmb3IoY2hhciogcD1zOypwO3ArKyl7IGlmKCpwPT0nPCcpe3N0cmNweSh3LCImbHQ7Iik7dys9NDt9IGVsc2UgaWYoKnA9PSc+Jyl7c3RyY3B5KHcsIiZndDsiKTt3Kz00O30gZWxzZSBpZigqcD09JyYnKXtzdHJjcHkodywiJmFtcDsiKTt3Kz01O30gZWxzZSBpZigqcD09JyInKXtzdHJjcHkodywiJnF1b3Q7Iik7dys9Njt9IGVsc2UgKncrKz0qcDsgfSAqdz0wOyBWYWx1ZSByPVNUUihvKTsgZnJlZShvKTsgcmV0dXJuIHI7IH0Kc3RhdGljIHZvaWQganNvbl9zdHIoY2hhcioqIG91dCxsb25nKiBjYXAsbG9uZyogbGVuLGNvbnN0IGNoYXIqIHMpeyBsb25nIG5lZWQ9KmxlbitzdHJsZW4ocykqNis0OyBpZihuZWVkPipjYXApeypjYXA9bmVlZCoyOypvdXQ9cmVhbGxvYygqb3V0LCpjYXApO30gY2hhciogdz0qb3V0KypsZW47ICp3Kys9JyInOyBmb3IoY29uc3QgY2hhciogcD1zOypwO3ArKyl7IHVuc2lnbmVkIGNoYXIgYz0qcDsgaWYoYz09JyInKXsqdysrPSdcXCc7KncrKz0nIic7fSBlbHNlIGlmKGM9PSdcXCcpeyp3Kys9J1xcJzsqdysrPSdcXCc7fSBlbHNlIGlmKGM9PSdcbicpeyp3Kys9J1xcJzsqdysrPSduJzt9IGVsc2UgaWYoYz09J1x0Jyl7KncrKz0nXFwnOyp3Kys9J3QnO30gZWxzZSBpZihjPT0nXHInKXsqdysrPSdcXCc7KncrKz0ncic7fSBlbHNlIGlmKGM8MHgyMCl7c3ByaW50Zih3LCJcXHUlMDR4IixjKTt3Kz02O30gZWxzZSAqdysrPWM7IH0gKncrKz0nIic7ICp3PTA7ICpsZW49dy0qb3V0OyB9CnN0YXRpYyB2b2lkIHRvX2pzb25fcmVjKFZhbHVlIHYsY2hhcioqIG91dCxsb25nKiBjYXAsbG9uZyogbGVuKXsgaWYoKmxlbis2ND4qY2FwKXsqY2FwPSgqbGVuKzY0KSoyOypvdXQ9cmVhbGxvYygqb3V0LCpjYXApO30gaWYodi50PT1UUyl7anNvbl9zdHIob3V0LGNhcCxsZW4sdi5zKTtyZXR1cm47fSBpZih2LnQ9PVROKXtjaGFyKiBucz1udW1zdHIodi5uKTtzdHJjcHkoKm91dCsqbGVuLG5zKTsqbGVuKz1zdHJsZW4obnMpO2ZyZWUobnMpO3JldHVybjt9IGlmKHYudD09VEIpe2NvbnN0IGNoYXIqIGI9di5uIT0wPyJ0cnVlIjoiZmFsc2UiO3N0cmNweSgqb3V0KypsZW4sYik7Kmxlbis9c3RybGVuKGIpO3JldHVybjt9IGlmKHYudD09VFgpe3N0cmNweSgqb3V0KypsZW4sIm51bGwiKTsqbGVuKz00O3JldHVybjt9IGlmKHYudD09VEwpeygqb3V0KVsoKmxlbikrK109J1snOyBmb3IobG9uZyBpPTA7aTx2LmwtPmxlbjtpKyspeyBpZihpKSgqb3V0KVsoKmxlbikrK109JywnOyB0b19qc29uX3JlYyh2LmwtPml0ZW1zW2ldLG91dCxjYXAsbGVuKTt9IGlmKCpsZW4rMj4qY2FwKXsqY2FwPSpsZW4rMjsqb3V0PXJlYWxsb2MoKm91dCwqY2FwKTt9ICgqb3V0KVsoKmxlbikrK109J10nOyAoKm91dClbKmxlbl09MDsgcmV0dXJuO30gaWYodi50PT1UTSl7KCpvdXQpWygqbGVuKSsrXT0neyc7IGZvcihsb25nIGk9MDtpPHYubS0+bGVuO2krKyl7IGlmKGkpKCpvdXQpWygqbGVuKSsrXT0nLCc7IGpzb25fc3RyKG91dCxjYXAsbGVuLHYubS0+a2V5c1tpXSk7ICgqb3V0KVsoKmxlbikrK109JzonOyB0b19qc29uX3JlYyh2Lm0tPnZhbHNbaV0sb3V0LGNhcCxsZW4pO30gaWYoKmxlbisyPipjYXApeypjYXA9KmxlbisyOypvdXQ9cmVhbGxvYygqb3V0LCpjYXApO30gKCpvdXQpWygqbGVuKSsrXT0nfSc7ICgqb3V0KVsqbGVuXT0wOyByZXR1cm47fSB9CnN0YXRpYyBWYWx1ZSBCX3RvX2pzb24oVmFsdWUgdil7IGxvbmcgY2FwPTI1NixsZW49MDsgY2hhciogbz1tYWxsb2MoY2FwKTsgb1swXT0wOyB0b19qc29uX3JlYyh2LCZvLCZjYXAsJmxlbik7IG9bbGVuXT0wOyBWYWx1ZSByPVNUUihvKTsgZnJlZShvKTsgcmV0dXJuIHI7IH0Kc3RhdGljIFZhbHVlIGpwYXJzZShjb25zdCBjaGFyKiBzLGxvbmcqIGkpOwpzdGF0aWMgdm9pZCBqd3MoY29uc3QgY2hhciogcyxsb25nKiBpKXsgd2hpbGUoc1sqaV09PScgJ3x8c1sqaV09PSdcdCd8fHNbKmldPT0nXG4nfHxzWyppXT09J1xyJykoKmkpKys7IH0Kc3RhdGljIFZhbHVlIGpzdHJpbmcoY29uc3QgY2hhciogcyxsb25nKiBpKXsgKCppKSsrOyBjaGFyKiBiPW1hbGxvYyhzdHJsZW4ocykrMSk7IGxvbmcgaj0wOyB3aGlsZShzWyppXSYmc1sqaV0hPSciJyl7IGlmKHNbKmldPT0nXFwnKXsgKCppKSsrOyBjaGFyIGM9c1sqaV07IGlmKGM9PSduJyliW2orK109J1xuJzsgZWxzZSBpZihjPT0ndCcpYltqKytdPSdcdCc7IGVsc2UgaWYoYz09J3InKWJbaisrXT0nXHInOyBlbHNlIGlmKGM9PSd1Jyl7IGludCBjb2RlPTA7IGZvcihpbnQgaz0wO2s8NDtrKyspeygqaSkrKzsgY2hhciBoPXNbKmldOyBjb2RlPWNvZGUqMTYrKChoPD0nOScpP2gtJzAnOih0b2xvd2VyKGgpLSdhJysxMCkpO30gYltqKytdPShjaGFyKWNvZGU7IH0gZWxzZSBiW2orK109YzsgKCppKSsrOyB9IGVsc2UgYltqKytdPXNbKCppKSsrXTsgfSBpZihzWyppXT09JyInKSgqaSkrKzsgYltqXT0wOyBWYWx1ZSB2PVNUUihiKTsgZnJlZShiKTsgcmV0dXJuIHY7IH0Kc3RhdGljIFZhbHVlIGpwYXJzZShjb25zdCBjaGFyKiBzLGxvbmcqIGkpeyBqd3MocyxpKTsgY2hhciBjPXNbKmldOwogIGlmKGM9PSciJylyZXR1cm4ganN0cmluZyhzLGkpOwogIGlmKGM9PSd7Jyl7ICgqaSkrKzsgVmFsdWUgbT1NQVAwKCk7IGp3cyhzLGkpOyBpZihzWyppXT09J30nKXsoKmkpKys7cmV0dXJuIG07fSBmb3IoOzspeyBqd3MocyxpKTsgVmFsdWUgaz1qc3RyaW5nKHMsaSk7IGp3cyhzLGkpOyBpZihzWyppXT09JzonKSgqaSkrKzsgVmFsdWUgdj1qcGFyc2UocyxpKTsgbWFwc2V0KG0sayx2KTsgandzKHMsaSk7IGlmKHNbKmldPT0nLCcpeygqaSkrKztjb250aW51ZTt9IGlmKHNbKmldPT0nfScpeygqaSkrKzt9IGJyZWFrOyB9IHJldHVybiBtOyB9CiAgaWYoYz09J1snKXsgKCppKSsrOyBWYWx1ZSBhPUxJU1QwKCk7IGp3cyhzLGkpOyBpZihzWyppXT09J10nKXsoKmkpKys7cmV0dXJuIGE7fSBmb3IoOzspeyBWYWx1ZSB2PWpwYXJzZShzLGkpOyBsaXN0cHVzaChhLHYpOyBqd3MocyxpKTsgaWYoc1sqaV09PScsJyl7KCppKSsrO2NvbnRpbnVlO30gaWYoc1sqaV09PSddJyl7KCppKSsrO30gYnJlYWs7IH0gcmV0dXJuIGE7IH0KICBpZihjPT0ndCcpeyppKz00O3JldHVybiBCT09MVigxKTt9IGlmKGM9PSdmJyl7KmkrPTU7cmV0dXJuIEJPT0xWKDApO30gaWYoYz09J24nKXsqaSs9NDtyZXR1cm4gTklMKCk7fQogIHsgY2hhciogZW5kOyBkb3VibGUgZD1zdHJ0b2QocysqaSwmZW5kKTsgKmk9ZW5kLXM7IHJldHVybiBOVU0oZCk7IH0gfQpzdGF0aWMgVmFsdWUgQl9mcm9tX2pzb24oVmFsdWUgdil7IGxvbmcgaT0wOyByZXR1cm4ganBhcnNlKHRvc3RyKHYpLCZpKTsgfQpzdGF0aWMgVmFsdWUgQl9tYWtlX2RpcihWYWx1ZSBwKXsgY2hhciBjbWRbNDA5Nl07IHNucHJpbnRmKGNtZCxzaXplb2YgY21kLCJta2RpciAtcCAnJXMnIix0b3N0cihwKSk7IHN5c3RlbShjbWQpOyByZXR1cm4gTklMKCk7IH0Kc3RhdGljIFZhbHVlIEJfcGF0aF9leGlzdHMoVmFsdWUgcCl7IHN0cnVjdCBzdGF0IHN0OyByZXR1cm4gQk9PTFYoc3RhdCh0b3N0cihwKSwmc3QpPT0wKTsgfQpzdGF0aWMgVmFsdWUgQl9pc19maWxlKFZhbHVlIHApeyBzdHJ1Y3Qgc3RhdCBzdDsgcmV0dXJuIEJPT0xWKHN0YXQodG9zdHIocCksJnN0KT09MCYmU19JU1JFRyhzdC5zdF9tb2RlKSk7IH0Kc3RhdGljIFZhbHVlIEJfaXNfZGlyKFZhbHVlIHApeyBzdHJ1Y3Qgc3RhdCBzdDsgcmV0dXJuIEJPT0xWKHN0YXQodG9zdHIocCksJnN0KT09MCYmU19JU0RJUihzdC5zdF9tb2RlKSk7IH0Kc3RhdGljIFZhbHVlIEJfZmlsZV9zaXplKFZhbHVlIHApeyBzdHJ1Y3Qgc3RhdCBzdDsgaWYoc3RhdCh0b3N0cihwKSwmc3QpPT0wKSByZXR1cm4gTlVNKHN0LnN0X3NpemUpOyByZXR1cm4gTlVNKDApOyB9CnN0YXRpYyBWYWx1ZSBCX2xpc3RfZGlyKFZhbHVlIHApeyBWYWx1ZSB2PUxJU1QwKCk7IERJUiogZD1vcGVuZGlyKHRvc3RyKHApKTsgaWYoIWQpcmV0dXJuIHY7IHN0cnVjdCBkaXJlbnQqIGU7IHdoaWxlKChlPXJlYWRkaXIoZCkpKXsgaWYoc3RyY21wKGUtPmRfbmFtZSwiLiIpJiZzdHJjbXAoZS0+ZF9uYW1lLCIuLiIpKSBsaXN0cHVzaCh2LFNUUihlLT5kX25hbWUpKTsgfSBjbG9zZWRpcihkKTsgcmV0dXJuIEJfc29ydCh2KTsgfQpzdGF0aWMgVmFsdWUgQl9yZW1vdmVfcGF0aChWYWx1ZSBwKXsgY2hhciBjbWRbNDA5Nl07IHNucHJpbnRmKGNtZCxzaXplb2YgY21kLCJybSAtcmYgJyVzJyIsdG9zdHIocCkpOyBzeXN0ZW0oY21kKTsgcmV0dXJuIE5JTCgpOyB9CnN0YXRpYyBWYWx1ZSBCX21vdmVfcGF0aChWYWx1ZSBhLFZhbHVlIGIpeyBjaGFyIGNtZFs4MTkyXTsgc25wcmludGYoY21kLHNpemVvZiBjbWQsIm1rZGlyIC1wIFwiJChkaXJuYW1lICclcycpXCI7IG12ICclcycgJyVzJyIsdG9zdHIoYiksdG9zdHIoYSksdG9zdHIoYikpOyBzeXN0ZW0oY21kKTsgcmV0dXJuIE5JTCgpOyB9CnN0YXRpYyBWYWx1ZSBCX2Rpcm5hbWUoVmFsdWUgcCl7IGNoYXIqIHM9c2R1cCh0b3N0cihwKSk7IGNoYXIqIHNsYXNoPXN0cnJjaHIocywnLycpOyBpZighc2xhc2gpe2ZyZWUocyk7cmV0dXJuIFNUUigiIik7fSAqc2xhc2g9MDsgVmFsdWUgdj1TVFIocyk7IGZyZWUocyk7IHJldHVybiB2OyB9CnN0YXRpYyBWYWx1ZSBCX2Jhc2VuYW1lKFZhbHVlIHApeyBjaGFyKiBzPXRvc3RyKHApOyBjaGFyKiBzbGFzaD1zdHJyY2hyKHMsJy8nKTsgcmV0dXJuIFNUUihzbGFzaD9zbGFzaCsxOnMpOyB9CnN0YXRpYyBWYWx1ZSBCX3BhdGhfam9pbihpbnQgbiwgLi4uKXsgY2hhciBidWZbODE5Ml07IGJ1ZlswXT0wOyB2YV9saXN0IGFwOyB2YV9zdGFydChhcCxuKTsgZm9yKGludCBpPTA7aTxuO2krKyl7IFZhbHVlIGE9dmFfYXJnKGFwLFZhbHVlKTsgaWYoaSYmYnVmWzBdJiZidWZbc3RybGVuKGJ1ZiktMV0hPScvJykgc3RyY2F0KGJ1ZiwiLyIpOyBzdHJjYXQoYnVmLHRvc3RyKGEpKTsgfSB2YV9lbmQoYXApOyByZXR1cm4gU1RSKGJ1Zik7IH0Kc3RhdGljIFZhbHVlIEJfaG9tZV9kaXIodm9pZCl7IGNoYXIqIGg9Z2V0ZW52KCJIT01FIik7IHJldHVybiBTVFIoaD9oOiIuIik7IH0Kc3RhdGljIFZhbHVlIEJfZW52KFZhbHVlIGspeyBjaGFyKiB2PWdldGVudih0b3N0cihrKSk7IHJldHVybiBTVFIodj92OiIiKTsgfQpzdGF0aWMgVmFsdWUgQl9ub3codm9pZCl7IHJldHVybiBOVU0oKGRvdWJsZSl0aW1lKDApKTsgfQpzdGF0aWMgVmFsdWUgQl9jbG9jayh2b2lkKXsgdGltZV90IHQ9dGltZSgwKTsgc3RydWN0IHRtKiBtPWxvY2FsdGltZSgmdCk7IGNoYXIgYlsxNl07IHNwcmludGYoYiwiJTAyZDolMDJkOiUwMmQiLG0tPnRtX2hvdXIsbS0+dG1fbWluLG0tPnRtX3NlYyk7IHJldHVybiBTVFIoYik7IH0Kc3RhdGljIFZhbHVlIEJfdG9kYXkodm9pZCl7IHRpbWVfdCB0PXRpbWUoMCk7IHN0cnVjdCB0bSogbT1sb2NhbHRpbWUoJnQpOyBjaGFyIGJbMTZdOyBzcHJpbnRmKGIsIiUwNGQtJTAyZC0lMDJkIixtLT50bV95ZWFyKzE5MDAsbS0+dG1fbW9uKzEsbS0+dG1fbWRheSk7IHJldHVybiBTVFIoYik7IH0Kc3RhdGljIFZhbHVlIEJfaHR0cF9nZXQoVmFsdWUgdXJsLCBWYWx1ZSBoZWFkZXJzKXsgY2hhciBjbWRbMTYzODRdOyBpbnQgbj1zbnByaW50ZihjbWQsc2l6ZW9mIGNtZCwiY3VybCAtc0wiKTsgaWYoaGVhZGVycy50PT1UTSl7IGZvcihsb25nIGk9MDtpPGhlYWRlcnMubS0+bGVuO2krKykgbis9c25wcmludGYoY21kK24sc2l6ZW9mIGNtZC1uLCIgLUggJyVzOiAlcyciLGhlYWRlcnMubS0+a2V5c1tpXSx0b3N0cihoZWFkZXJzLm0tPnZhbHNbaV0pKTsgfSBzbnByaW50ZihjbWQrbixzaXplb2YgY21kLW4sIiAnJXMnIix0b3N0cih1cmwpKTsgVmFsdWUgb3V0PUJfcnVuKFNUUihjbWQpKTsgcmV0dXJuIE1LTUFQKDIsU1RSKCJzdGF0dXMiKSxOVU0oMjAwKSxTVFIoImJvZHkiKSxvdXQpOyB9CnN0YXRpYyBWYWx1ZSBwYXJzZV9xdWVyeShjb25zdCBjaGFyKiBxKXsgVmFsdWUgbT1NQVAwKCk7IGlmKCFxfHwhKnEpcmV0dXJuIG07IGNoYXIqIHM9c2R1cChxKTsgY2hhciogcD1zOyB3aGlsZShwJiYqcCl7IGNoYXIqIGFtcD1zdHJjaHIocCwnJicpOyBpZihhbXApKmFtcD0wOyBjaGFyKiBlcT1zdHJjaHIocCwnPScpOyBpZihlcSl7KmVxPTA7IFZhbHVlIGs9Ql91cmxfZGVjb2RlKFNUUihwKSk7IFZhbHVlIHY9Ql91cmxfZGVjb2RlKFNUUihlcSsxKSk7IG1hcHNldChtLGssdik7fSBwPWFtcD9hbXArMTowOyB9IGZyZWUocyk7IHJldHVybiBtOyB9CnN0YXRpYyBjaGFyKiBjaV9zdHJzdHIoY29uc3QgY2hhciogaCwgY29uc3QgY2hhciogbil7IGlmKCEqbikgcmV0dXJuIChjaGFyKiloOyBmb3IoOyAqaDsgaCsrKXsgY29uc3QgY2hhciogYT1oOyBjb25zdCBjaGFyKiBiPW47IHdoaWxlKCphICYmICpiICYmIHRvbG93ZXIoKHVuc2lnbmVkIGNoYXIpKmEpPT10b2xvd2VyKCh1bnNpZ25lZCBjaGFyKSpiKSl7IGErKzsgYisrOyB9IGlmKCEqYikgcmV0dXJuIChjaGFyKiloOyB9IHJldHVybiAwOyB9CnN0YXRpYyBjaGFyKiByZWN2X3JlcXVlc3QoaW50IGMsbG9uZyogYmxlbil7IGxvbmcgY2FwPTgxOTIsbGVuPTA7IGNoYXIqIGJ1Zj1tYWxsb2MoY2FwKTsgZm9yKDs7KXsgaWYobGVuKzQwOTY+PWNhcCl7Y2FwKj0yO2J1Zj1yZWFsbG9jKGJ1ZixjYXApO30gbG9uZyByPXJlY3YoYyxidWYrbGVuLDQwOTYsMCk7IGlmKHI8PTApYnJlYWs7IGxlbis9cjsgYnVmW2xlbl09MDsgY2hhciogaGU9c3Ryc3RyKGJ1ZiwiXHJcblxyXG4iKTsgaWYoaGUpeyBsb25nIGhsZW49aGUtYnVmKzQ7IGNoYXIqIGNsPWNpX3N0cnN0cihidWYsImNvbnRlbnQtbGVuZ3RoOiIpOyBsb25nIHdhbnQ9Y2w/YXRvbChjbCsxNSk6MDsgd2hpbGUoKGxvbmcpKGxlbi1obGVuKTx3YW50KXsgaWYobGVuKzQwOTY+PWNhcCl7Y2FwKj0yO2J1Zj1yZWFsbG9jKGJ1ZixjYXApO30gbG9uZyByMj1yZWN2KGMsYnVmK2xlbiw0MDk2LDApOyBpZihyMjw9MClicmVhazsgbGVuKz1yMjsgfSBidWZbbGVuXT0wOyBicmVhazsgfSB9ICpibGVuPWxlbjsgcmV0dXJuIGJ1ZjsgfQpzdGF0aWMgVmFsdWUgcGFyc2VfcmVxdWVzdChjaGFyKiByYXcpeyBWYWx1ZSByZXE9TUFQMCgpOyBjaGFyKiBubD1zdHJzdHIocmF3LCJcclxuIik7IGlmKCFubClyZXR1cm4gcmVxOyAqbmw9MDsgY2hhciogbWV0aG9kPXJhdzsgY2hhciogc3A9c3RyY2hyKHJhdywnICcpOyBpZighc3ApcmV0dXJuIHJlcTsgKnNwPTA7IGNoYXIqIHRhcmdldD1zcCsxOyBjaGFyKiBzcDI9c3RyY2hyKHRhcmdldCwnICcpOyBpZihzcDIpKnNwMj0wOyBjaGFyKiBxPXN0cmNocih0YXJnZXQsJz8nKTsgY2hhciogcXVlcnk9IiI7IGlmKHEpeypxPTA7cXVlcnk9cSsxO30gbWFwc2V0KHJlcSxTVFIoIm1ldGhvZCIpLFNUUihtZXRob2QpKTsgbWFwc2V0KHJlcSxTVFIoInBhdGgiKSxCX3VybF9kZWNvZGUoU1RSKHRhcmdldCkpKTsgbWFwc2V0KHJlcSxTVFIoInF1ZXJ5IikscGFyc2VfcXVlcnkocXVlcnkpKTsgVmFsdWUgaGRycz1NQVAwKCk7IGNoYXIqIGhlPXN0cnN0cihubCsyLCJcclxuXHJcbiIpOyBjaGFyKiBsaW5lPW5sKzI7IHdoaWxlKGxpbmUmJmhlJiZsaW5lPGhlKXsgY2hhciogZW9sPXN0cnN0cihsaW5lLCJcclxuIik7IGlmKCFlb2x8fGVvbD5oZSlicmVhazsgKmVvbD0wOyBjaGFyKiBjb2w9c3RyY2hyKGxpbmUsJzonKTsgaWYoY29sKXsqY29sPTA7IGNoYXIqIHZhbD1jb2wrMTsgd2hpbGUoKnZhbD09JyAnKXZhbCsrOyBtYXBzZXQoaGRycyxTVFIobGluZSksU1RSKHZhbCkpO30gbGluZT1lb2wrMjsgfSBtYXBzZXQocmVxLFNUUigiaGVhZGVycyIpLGhkcnMpOyBtYXBzZXQocmVxLFNUUigiYm9keSIpLFNUUihoZT9oZSs0OiIiKSk7IHJldHVybiByZXE7IH0Kc3RhdGljIFZhbHVlIEJfdHlwZW9mKFZhbHVlIHYpeyBpZih2LnQ9PVRTKXJldHVybiBTVFIoInN0cmluZyIpOyBpZih2LnQ9PVROKXJldHVybiBTVFIoIm51bWJlciIpOyBpZih2LnQ9PVRCKXJldHVybiBTVFIoImJvb2wiKTsgaWYodi50PT1UTClyZXR1cm4gU1RSKCJsaXN0Iik7IGlmKHYudD09VE0pcmV0dXJuIFNUUigibWFwIik7IHJldHVybiBTVFIoIm5vdGhpbmciKTsgfQpzdGF0aWMgVmFsdWUgQl90Y3BfbGlzdGVuKFZhbHVlIHB2KXsgaW50IHNydj1zb2NrZXQoQUZfSU5FVCxTT0NLX1NUUkVBTSwwKTsgaW50IG9wdD0xOyBzZXRzb2Nrb3B0KHNydixTT0xfU09DS0VULFNPX1JFVVNFQUREUiwmb3B0LHNpemVvZiBvcHQpOyBzdHJ1Y3Qgc29ja2FkZHJfaW4gYTsgbWVtc2V0KCZhLDAsc2l6ZW9mIGEpOyBhLnNpbl9mYW1pbHk9QUZfSU5FVDsgYS5zaW5fYWRkci5zX2FkZHI9SU5BRERSX0FOWTsgYS5zaW5fcG9ydD1odG9ucygobG9uZylwdi5uKTsgaWYoYmluZChzcnYsKHN0cnVjdCBzb2NrYWRkciopJmEsc2l6ZW9mIGEpPDApIHJldHVybiBOVU0oLTEpOyBsaXN0ZW4oc3J2LDY0KTsgcmV0dXJuIE5VTShzcnYpOyB9CnN0YXRpYyBWYWx1ZSBCX2FjY2VwdF9yZXEoVmFsdWUgc3YpeyBpbnQgYz1hY2NlcHQoKGludClzdi5uLDAsMCk7IGlmKGM8MCkgcmV0dXJuIE5JTCgpOyBsb25nIGJsOyBjaGFyKiByYXc9cmVjdl9yZXF1ZXN0KGMsJmJsKTsgVmFsdWUgcmVxPShyYXcmJmJsPjApP3BhcnNlX3JlcXVlc3QocmF3KTpOSUwoKTsgaWYocmVxLnQ9PVRNKSBtYXBzZXQocmVxLFNUUigiX2Nvbm4iKSxOVU0oYykpOyBpZihyYXcpIGZyZWUocmF3KTsgcmV0dXJuIHJlcTsgfQpzdGF0aWMgVmFsdWUgQl9yZXNwb25kKFZhbHVlIHJlcSwgVmFsdWUgcmVzcCl7IFZhbHVlIGN2PUlOREVYKHJlcSxTVFIoIl9jb25uIikpOyBpbnQgYz0oY3YudD09VE4pPyhpbnQpY3YubjotMTsgaWYoYzwwKSByZXR1cm4gTklMKCk7IGxvbmcgc3RhdHVzPTIwMDsgY2hhciogYm9keT0iIjsgY2hhciogY3R5cGU9InRleHQvaHRtbDsgY2hhcnNldD11dGYtOCI7IFZhbHVlIHhoPU5JTCgpOyBpZihyZXNwLnQ9PVRTKXsgYm9keT1yZXNwLnM7IH0gZWxzZSBpZihyZXNwLnQ9PVRNKXsgVmFsdWUgc3Q9SU5ERVgocmVzcCxTVFIoInN0YXR1cyIpKTsgaWYoc3QudD09VE4pc3RhdHVzPShsb25nKXN0Lm47IFZhbHVlIGJkPUlOREVYKHJlc3AsU1RSKCJib2R5IikpOyBpZihiZC50PT1UTXx8YmQudD09VEwpeyBib2R5PXRvc3RyKEJfdG9fanNvbihiZCkpOyBjdHlwZT0iYXBwbGljYXRpb24vanNvbiI7IH0gZWxzZSBpZihiZC50IT1UWCkgYm9keT10b3N0cihiZCk7IFZhbHVlIHR5PUlOREVYKHJlc3AsU1RSKCJ0eXBlIikpOyBpZih0eS50PT1UUyljdHlwZT10eS5zOyB4aD1JTkRFWChyZXNwLFNUUigiaGVhZGVycyIpKTsgfSBjaGFyIGhlYWRbNDA5Nl07IGxvbmcgYmwyPXN0cmxlbihib2R5KTsgaW50IGhuPXNucHJpbnRmKGhlYWQsc2l6ZW9mIGhlYWQsIkhUVFAvMS4xICVsZCBPS1xyXG5Db250ZW50LVR5cGU6ICVzXHJcbkNvbnRlbnQtTGVuZ3RoOiAlbGRcclxuQ29ubmVjdGlvbjogY2xvc2VcclxuIixzdGF0dXMsY3R5cGUsYmwyKTsgaWYoeGgudD09VE0peyBmb3IobG9uZyBpPTA7aTx4aC5tLT5sZW47aSsrKSBobis9c25wcmludGYoaGVhZCtobixzaXplb2YgaGVhZC1obiwiJXM6ICVzXHJcbiIseGgubS0+a2V5c1tpXSx0b3N0cih4aC5tLT52YWxzW2ldKSk7IH0gaG4rPXNucHJpbnRmKGhlYWQraG4sc2l6ZW9mIGhlYWQtaG4sIlxyXG4iKTsgd3JpdGUoYyxoZWFkLGhuKTsgd3JpdGUoYyxib2R5LGJsMik7IGNsb3NlKGMpOyByZXR1cm4gTklMKCk7IH0Kc3RhdGljIFZhbHVlIHZjX3NlcnZlKGxvbmcgcG9ydCwgVmFsdWUoKmhhbmRsZXIpKFZhbHVlKSl7IGludCBzcnY9c29ja2V0KEFGX0lORVQsU09DS19TVFJFQU0sMCk7IGludCBvcHQ9MTsgc2V0c29ja29wdChzcnYsU09MX1NPQ0tFVCxTT19SRVVTRUFERFIsJm9wdCxzaXplb2Ygb3B0KTsgc3RydWN0IHNvY2thZGRyX2luIGE7IG1lbXNldCgmYSwwLHNpemVvZiBhKTsgYS5zaW5fZmFtaWx5PUFGX0lORVQ7IGEuc2luX2FkZHIuc19hZGRyPUlOQUREUl9BTlk7IGEuc2luX3BvcnQ9aHRvbnMocG9ydCk7IGlmKGJpbmQoc3J2LChzdHJ1Y3Qgc29ja2FkZHIqKSZhLHNpemVvZiBhKTwwKXtwZXJyb3IoImJpbmQiKTtyZXR1cm4gTklMKCk7fSBsaXN0ZW4oc3J2LDY0KTsgcHJpbnRmKCJWYW50YSBuYXRpdmUgc2VydmVyIG9uIGh0dHA6Ly9sb2NhbGhvc3Q6JWxkXG4iLHBvcnQpOyBmZmx1c2goc3Rkb3V0KTsgZ19pbl9yZXE9MTsKICBmb3IoOzspeyBlYmIoKTsgaW50IGM9YWNjZXB0KHNydiwwLDApOyBpZihjPDApY29udGludWU7IGxvbmcgYmxlbjsgY2hhciogcmF3PXJlY3ZfcmVxdWVzdChjLCZibGVuKTsgaWYocmF3JiZibGVuPjApeyBWYWx1ZSByZXE9cGFyc2VfcmVxdWVzdChyYXcpOyBWYWx1ZSByZXNwPWhhbmRsZXIocmVxKTsgbG9uZyBzdGF0dXM9MjAwOyBjaGFyKiBib2R5PSIiOyBjaGFyKiBjdHlwZT0idGV4dC9odG1sOyBjaGFyc2V0PXV0Zi04IjsgVmFsdWUgeGg9TklMKCk7CiAgICAgICAgaWYocmVzcC50PT1UUyl7IGJvZHk9cmVzcC5zOyB9IGVsc2UgaWYocmVzcC50PT1UTSl7IFZhbHVlIHN0PUlOREVYKHJlc3AsU1RSKCJzdGF0dXMiKSk7IGlmKHN0LnQ9PVROKXN0YXR1cz0obG9uZylzdC5uOyBWYWx1ZSBiZD1JTkRFWChyZXNwLFNUUigiYm9keSIpKTsgaWYoYmQudD09VE18fGJkLnQ9PVRMKXsgYm9keT10b3N0cihCX3RvX2pzb24oYmQpKTsgY3R5cGU9ImFwcGxpY2F0aW9uL2pzb24iOyB9IGVsc2UgaWYoYmQudCE9VFgpIGJvZHk9dG9zdHIoYmQpOyBWYWx1ZSB0eT1JTkRFWChyZXNwLFNUUigidHlwZSIpKTsgaWYodHkudD09VFMpY3R5cGU9dHkuczsgeGg9SU5ERVgocmVzcCxTVFIoImhlYWRlcnMiKSk7IH0KICAgICAgICBjaGFyIGhlYWRbNDA5Nl07IGxvbmcgYmw9c3RybGVuKGJvZHkpOyBpbnQgaG49c25wcmludGYoaGVhZCxzaXplb2YgaGVhZCwiSFRUUC8xLjEgJWxkIE9LXHJcbkNvbnRlbnQtVHlwZTogJXNcclxuQ29udGVudC1MZW5ndGg6ICVsZFxyXG5Db25uZWN0aW9uOiBjbG9zZVxyXG4iLHN0YXR1cyxjdHlwZSxibCk7IGlmKHhoLnQ9PVRNKXsgZm9yKGxvbmcgaT0wO2k8eGgubS0+bGVuO2krKykgaG4rPXNucHJpbnRmKGhlYWQraG4sc2l6ZW9mIGhlYWQtaG4sIiVzOiAlc1xyXG4iLHhoLm0tPmtleXNbaV0sdG9zdHIoeGgubS0+dmFsc1tpXSkpOyB9IGhuKz1zbnByaW50ZihoZWFkK2huLHNpemVvZiBoZWFkLWhuLCJcclxuIik7IHdyaXRlKGMsaGVhZCxobik7IHdyaXRlKGMsYm9keSxibCk7IGZyZWUocmF3KTsgfSBjbG9zZShjKTsgfQogIHJldHVybiBOSUwoKTsgfQoKLyogLS0tLSBleGNlcHRpb25zIChhdHRlbXB0L3Jlc2N1ZSB2aWEgc2V0am1wKSArIGh0dHBfcG9zdCArIHJ1bl92YW50YSAtLS0tICovCiNpbmNsdWRlIDxzZXRqbXAuaD4Kc3RhdGljIGptcF9idWYgZ19qbXBbMTI4XTsgc3RhdGljIGludCBnX2ptcHNwPTA7IHN0YXRpYyBWYWx1ZSBnX2VycjsKc3RhdGljIFZhbHVlIEJfZmFpbChWYWx1ZSBtc2cpeyBnX2Vycj1tc2c7IGlmKGdfam1wc3A+MCkgbG9uZ2ptcChnX2ptcFtnX2ptcHNwLTFdLDEpOyBmcHJpbnRmKHN0ZGVyciwiZmFpbDogJXNcbiIsdG9zdHIobXNnKSk7IGV4aXQoMSk7IH0Kc3RhdGljIFZhbHVlIEJfaHR0cF9wb3N0KFZhbHVlIHVybCwgVmFsdWUgYm9keSwgVmFsdWUgaGVhZGVycyl7CiAgY2hhciogYm9keXN0ciA9IChib2R5LnQ9PVRNfHxib2R5LnQ9PVRMKT90b3N0cihCX3RvX2pzb24oYm9keSkpOnRvc3RyKGJvZHkpOwogIGNoYXIgdG1wZltdPSIvdG1wL3ZjcG9zdFhYWFhYWCI7IGludCBmZD1ta3N0ZW1wKHRtcGYpOyBpZihmZD49MCl7IHdyaXRlKGZkLGJvZHlzdHIsc3RybGVuKGJvZHlzdHIpKTsgY2xvc2UoZmQpO30gCiAgY2hhciBjbWRbMzI3NjhdOyBpbnQgbj1zbnByaW50ZihjbWQsc2l6ZW9mIGNtZCwiY3VybCAtcyAtWCBQT1NUICclcyciLHRvc3RyKHVybCkpOwogIGlmKGhlYWRlcnMudD09VE0peyBmb3IobG9uZyBpPTA7aTxoZWFkZXJzLm0tPmxlbjtpKyspIG4rPXNucHJpbnRmKGNtZCtuLHNpemVvZiBjbWQtbiwiIC1IICclczogJXMnIixoZWFkZXJzLm0tPmtleXNbaV0sdG9zdHIoaGVhZGVycy5tLT52YWxzW2ldKSk7IH0KICBuKz1zbnByaW50ZihjbWQrbixzaXplb2YgY21kLW4sIiAtLWRhdGEtYmluYXJ5IEAlcyIsdG1wZik7CiAgVmFsdWUgb3V0PUJfcnVuKFNUUihjbWQpKTsgdW5saW5rKHRtcGYpOwogIHJldHVybiBNS01BUCgyLCBTVFIoInN0YXR1cyIpLE5VTSgyMDApLCBTVFIoImJvZHkiKSxvdXQpOwp9CnN0YXRpYyBWYWx1ZSBCX3J1bl92YW50YShWYWx1ZSBjb2RlKXsKICBjaGFyIHRtcGZbXT0iL3RtcC92Y3J1blhYWFhYWC52YSI7IGludCBmZD1ta3N0ZW1wcyh0bXBmLDMpOyBjaGFyKiBjPXRvc3RyKGNvZGUpOyBpZihmZD49MCl7IHdyaXRlKGZkLGMsc3RybGVuKGMpKTsgY2xvc2UoZmQpO30gCiAgY2hhciBjbWRbMjU2XTsgc25wcmludGYoY21kLHNpemVvZiBjbWQsInZzZWxmICclcyciLHRtcGYpOwogIFZhbHVlIHI9Ql9zaGVsbChTVFIoY21kKSk7IHVubGluayh0bXBmKTsKICBpbnQgb2s9KChsb25nKUlOREVYKHIsU1RSKCJjb2RlIikpLm4pPT0wOwogIHJldHVybiBNS01BUCgzLCBTVFIoIm9rIiksQk9PTFYob2spLCBTVFIoIm91dHB1dCIpLElOREVYKHIsU1RSKCJvdXRwdXQiKSksIFNUUigiZXJyb3IiKSwgb2s/U1RSKCIiKTpJTkRFWChyLFNUUigib3V0cHV0IikpKTsKfQoKc3RhdGljIFZhbHVlIEJfc3RhcnRzX3dpdGgoVmFsdWUgcywgVmFsdWUgcCl7IGNoYXIqIGE9dG9zdHIocyk7IGNoYXIqIGI9dG9zdHIocCk7IHJldHVybiBCT09MVihzdHJuY21wKGEsYixzdHJsZW4oYikpPT0wKTsgfQpzdGF0aWMgVmFsdWUgQl9lbmRzX3dpdGgoVmFsdWUgcywgVmFsdWUgcCl7IGNoYXIqIGE9dG9zdHIocyk7IGNoYXIqIGI9dG9zdHIocCk7IGxvbmcgbGE9c3RybGVuKGEpLGxiPXN0cmxlbihiKTsgcmV0dXJuIEJPT0xWKGxhPj1sYiYmc3RyY21wKGErbGEtbGIsYik9PTApOyB9CnN0YXRpYyBWYWx1ZSBCX2ZpbmQoVmFsdWUgcywgVmFsdWUgc3ViKXsgY2hhciogYT10b3N0cihzKTsgY2hhciogcT1zdHJzdHIoYSx0b3N0cihzdWIpKTsgcmV0dXJuIE5VTShxPyhxLWEpOi0xKTsgfQpzdGF0aWMgVmFsdWUgQl9vc19uYW1lKHZvaWQpewojaWZkZWYgX19BUFBMRV9fCiAgcmV0dXJuIFNUUigibWFjIik7CiNlbGlmIGRlZmluZWQoX1dJTjMyKQogIHJldHVybiBTVFIoIndpbmRvd3MiKTsKI2Vsc2UKICByZXR1cm4gU1RSKCJsaW51eCIpOwojZW5kaWYKfQpzdGF0aWMgVmFsdWUgQl9vcGVuX3VybChWYWx1ZSB1cmwpeyBjaGFyIGNtZFs4MTkyXTsKI2lmZGVmIF9fQVBQTEVfXwogIHNucHJpbnRmKGNtZCxzaXplb2YgY21kLCJvcGVuICclcycgPi9kZXYvbnVsbCAyPiYxIix0b3N0cih1cmwpKTsKI2Vsc2UKICBzbnByaW50ZihjbWQsc2l6ZW9mIGNtZCwieGRnLW9wZW4gJyVzJyA+L2Rldi9udWxsIDI+JjEiLHRvc3RyKHVybCkpOwojZW5kaWYKICBzeXN0ZW0oY21kKTsgcmV0dXJuIE5JTCgpOyB9CgpzdGF0aWMgVmFsdWUgQl9yZXZlcnNlKFZhbHVlIHYpeyBpZih2LnQ9PVRTKXsgY2hhciogcz10b3N0cih2KTsgbG9uZyBuPXN0cmxlbihzKTsgY2hhciogcj1tYWxsb2MobisxKTsgZm9yKGxvbmcgaT0wO2k8bjtpKyspIHJbaV09c1tuLTEtaV07IHJbbl09MDsgVmFsdWUgeD1TVFIocik7IGZyZWUocik7IHJldHVybiB4OyB9IGlmKHYudD09VEwpeyBWYWx1ZSBvPUxJU1QwKCk7IGZvcihsb25nIGk9di5sLT5sZW4tMTtpPj0wO2ktLSkgbGlzdHB1c2gobyx2LmwtPml0ZW1zW2ldKTsgcmV0dXJuIG87IH0gcmV0dXJuIHY7IH0Kc3RhdGljIFZhbHVlIEJfZmlyc3QoVmFsdWUgdil7IGlmKHYudD09VEwmJnYubC0+bGVuPjApIHJldHVybiB2LmwtPml0ZW1zWzBdOyBpZih2LnQ9PVRTJiZ2LnNbMF0peyBjaGFyIGJbMl09e3Yuc1swXSwwfTsgcmV0dXJuIFNUUihiKTt9IHJldHVybiBOSUwoKTsgfQpzdGF0aWMgVmFsdWUgQl9sYXN0KFZhbHVlIHYpeyBpZih2LnQ9PVRMJiZ2LmwtPmxlbj4wKSByZXR1cm4gdi5sLT5pdGVtc1t2LmwtPmxlbi0xXTsgaWYodi50PT1UUyl7IGxvbmcgbj1zdHJsZW4odi5zKTsgaWYobj4wKXtjaGFyIGJbMl09e3Yuc1tuLTFdLDB9OyByZXR1cm4gU1RSKGIpO30gfSByZXR1cm4gTklMKCk7IH0Kc3RhdGljIFZhbHVlIEJfZmxvb3IoVmFsdWUgdil7IGxvbmcgdD0obG9uZyl2Lm47IHJldHVybiBOVU0oKGRvdWJsZSkodC0oKHYubjwwJiZ2Lm4hPXQpPzE6MCkpKTsgfQpzdGF0aWMgVmFsdWUgQl9jZWlsKFZhbHVlIHYpeyBsb25nIHQ9KGxvbmcpdi5uOyByZXR1cm4gTlVNKChkb3VibGUpKHQrKCh2Lm4+MCYmdi5uIT10KT8xOjApKSk7IH0Kc3RhdGljIFZhbHVlIEJfcm91bmQoVmFsdWUgdil7IHJldHVybiBOVU0oKGRvdWJsZSkobG9uZykodi5uKyh2Lm4+PTA/MC41Oi0wLjUpKSk7IH0Kc3RhdGljIFZhbHVlIEJfYWJzKFZhbHVlIHYpeyByZXR1cm4gTlVNKHYubjwwPy12Lm46di5uKTsgfQo=");
    v_args = B_arguments();
    if (truthy(ANDV(GT(B_length(v_args), NUM(1)), EQ(INDEX(v_args, NUM(1)), STR("-k"))))) {
        B_write_file(ADD(INDEX(v_args, NUM(0)), STR(".c")), v_compile_kernel(B_read_file(INDEX(v_args, NUM(0)))));
        SAY(ADD(ADD(STR("emitted freestanding kernel C -> "), INDEX(v_args, NUM(0))), STR(".c")));
    } else {
        if (truthy(ANDV(GT(B_length(v_args), NUM(1)), EQ(INDEX(v_args, NUM(1)), STR("-c"))))) {
            v_compile_only(B_read_file(INDEX(v_args, NUM(0))), INDEX(v_args, NUM(0)));
        } else {
            if (truthy(GT(B_length(v_args), NUM(0)))) {
                v_build_and_run(B_read_file(INDEX(v_args, NUM(0))), INDEX(v_args, NUM(0)));
            } else {
                SAY(STR("vc - compiling a Vanta program (strings, lists, maps) to a NATIVE binary:"));
                Value v_demo = STR("to fib(n)\n    if n is under 2\n        give back n\n    end\n    give back fib(n - 1) + fib(n - 2)\nend\nsay \"fib(20) = \" + text(fib(20))\nlet nums be [4, 1, 3, 1, 5, 9, 2, 6]\nlet total be 0\nfor each n in nums\n    change total to total + n\nend\nsay \"sum \" + text(nums) + \" = \" + text(total)\nsay \"sorted = \" + text(sort(nums))\nlet who be {\"name\": \"Ada\", \"lang\": \"Vanta\"}\nsay who[\"name\"] + \" writes \" + who[\"lang\"]\nlet shout be \"\"\nfor each w in [\"compiled\", \"to\", \"native\"]\n    change shout to shout + uppercase(w) + \" \"\nend\nsay trim(shout)\n");
                v_build_and_run(v_demo, STR("/tmp/vcdemo2"));
            }
        }
    }
    return 0;
}
