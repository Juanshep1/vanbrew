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
  if(v.t==TL){ long cap=256,len=1; char* o=galloc(cap); o[0]='['; for(long i=0;i<v.l->len;i++){ Value e=v.l->items[i]; char* p=tostr(e); long lp=strlen(p); int q=(e.t==TS); long need=len+lp+6; if(need>cap){ cap=need*2; o=grealloc(o,cap);} if(i){ o[len++]=','; o[len++]=' '; } if(q)o[len++]='"'; memcpy(o+len,p,lp); len+=lp; if(q)o[len++]='"'; } o[len++]=']'; o[len]=0; return o; }
  if(v.t==TM){ long cap=256,len=1; char* o=galloc(cap); o[0]='{'; for(long i=0;i<v.m->len;i++){ char* k=v.m->keys[i]; Value vv=v.m->vals[i]; char* p=tostr(vv); long lk=strlen(k),lp=strlen(p); int q=(vv.t==TS); long need=len+lk+lp+8; if(need>cap){ cap=need*2; o=grealloc(o,cap);} if(i){ o[len++]=','; o[len++]=' '; } o[len++]='"'; memcpy(o+len,k,lk); len+=lk; o[len++]='"'; o[len++]=':'; o[len++]=' '; if(q)o[len++]='"'; memcpy(o+len,p,lp); len+=lp; if(q)o[len++]='"'; } o[len++]='}'; o[len]=0; return o; }
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
static void to_json_rec(Value v,char** out,long* cap,long* len){ if(*len+64>*cap){*cap=(*len+64)*2;*out=realloc(*out,*cap);} if(v.t==TS){json_str(out,cap,len,v.s);return;} if(v.t==TN){char* ns=numstr(v.n);strcpy(*out+*len,ns);*len+=strlen(ns);free(ns);return;} if(v.t==TB){const char* b=v.n!=0?"true":"false";strcpy(*out+*len,b);*len+=strlen(b);return;} if(v.t==TX){strcpy(*out+*len,"null");*len+=4;return;} if(v.t==TL){(*out)[(*len)++]='['; for(long i=0;i<v.l->len;i++){ if(i){if(*len+2>*cap){*cap=(*len+2)*2;*out=realloc(*out,*cap);}(*out)[(*len)++]=',';(*out)[(*len)++]=' ';} to_json_rec(v.l->items[i],out,cap,len);} if(*len+2>*cap){*cap=*len+2;*out=realloc(*out,*cap);} (*out)[(*len)++]=']'; (*out)[*len]=0; return;} if(v.t==TM){(*out)[(*len)++]='{'; for(long i=0;i<v.m->len;i++){ if(i){if(*len+2>*cap){*cap=(*len+2)*2;*out=realloc(*out,*cap);}(*out)[(*len)++]=',';(*out)[(*len)++]=' ';} json_str(out,cap,len,v.m->keys[i]); if(*len+2>*cap){*cap=(*len+2)*2;*out=realloc(*out,*cap);}(*out)[(*len)++]=':';(*out)[(*len)++]=' '; to_json_rec(v.m->vals[i],out,cap,len);} if(*len+2>*cap){*cap=*len+2;*out=realloc(*out,*cap);} (*out)[(*len)++]='}'; (*out)[*len]=0; return;} }
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
static Value B_typeof(Value v){ if(v.t==TS)return STR("text"); if(v.t==TN)return STR("number"); if(v.t==TB)return STR("bool"); if(v.t==TL)return STR("list"); if(v.t==TM)return STR("map"); return STR("nothing"); }
static Value B_tcp_listen(Value pv){ int srv=socket(AF_INET,SOCK_STREAM,0); int opt=1; setsockopt(srv,SOL_SOCKET,SO_REUSEADDR,&opt,sizeof opt); struct sockaddr_in a; memset(&a,0,sizeof a); a.sin_family=AF_INET; a.sin_addr.s_addr=INADDR_ANY; a.sin_port=htons((long)pv.n); if(bind(srv,(struct sockaddr*)&a,sizeof a)<0) return NUM(-1); listen(srv,64); return NUM(srv); }
static Value B_accept_req(Value sv){ int c=accept((int)sv.n,0,0); if(c<0) return NIL(); long bl; char* raw=recv_request(c,&bl); Value req=(raw&&bl>0)?parse_request(raw):NIL(); if(req.t==TM) mapset(req,STR("_conn"),NUM(c)); if(raw) free(raw); return req; }
static Value B_respond(Value req, Value resp){ Value cv=INDEX(req,STR("_conn")); int c=(cv.t==TN)?(int)cv.n:-1; if(c<0) return NIL(); long status=200; char* body=""; char* ctype="text/html; charset=utf-8"; Value xh=NIL(); if(resp.t==TS){ body=resp.s; } else if(resp.t==TM){ Value st=INDEX(resp,STR("status")); if(st.t==TN)status=(long)st.n; Value bd=INDEX(resp,STR("body")); if(bd.t==TM||bd.t==TL){ body=tostr(B_to_json(bd)); ctype="application/json"; } else if(bd.t!=TX) body=tostr(bd); Value ty=INDEX(resp,STR("type")); if(ty.t==TS)ctype=ty.s; xh=INDEX(resp,STR("headers")); } char head[4096]; long bl2=strlen(body); int hn=snprintf(head,sizeof head,"HTTP/1.1 %ld OK\r\nContent-Type: %s\r\nContent-Length: %ld\r\nConnection: close\r\n",status,ctype,bl2); if(xh.t==TM){ for(long i=0;i<xh.m->len;i++) hn+=snprintf(head+hn,sizeof head-hn,"%s: %s\r\n",xh.m->keys[i],tostr(xh.m->vals[i])); } hn+=snprintf(head+hn,sizeof head-hn,"\r\n"); write(c,head,hn); write(c,body,bl2); close(c); return NIL(); }
static Value B_assert(Value c, Value msg){ if(!truthy(c)){ fprintf(stderr,"assertion failed: %s\n", tostr(msg)); exit(1);} return NIL(); }
static Value B_is_number(Value v){ return BOOLV(v.t==TN); }
static Value B_is_text(Value v){ return BOOLV(v.t==TS); }
static Value B_is_list(Value v){ return BOOLV(v.t==TL); }
static Value B_is_map(Value v){ return BOOLV(v.t==TM); }
static Value B_is_nothing(Value v){ return BOOLV(v.t==TX); }
static Value B_is_bool(Value v){ return BOOLV(v.t==TB); }
static Value B_band(Value a, Value b){ return NUM((double)((long)a.n & (long)b.n)); }
static Value B_bor(Value a, Value b){ return NUM((double)((long)a.n | (long)b.n)); }
static Value B_bxor(Value a, Value b){ return NUM((double)((long)a.n ^ (long)b.n)); }
static Value B_bnot(Value a, Value w){ long bits=(long)w.n; long mask=(bits>=63)?-1L:((1L<<bits)-1); return NUM((double)((~(long)a.n) & mask)); }
static Value B_shl(Value a, Value b){ return NUM((double)((long)a.n << (long)b.n)); }
static Value B_shr(Value a, Value b){ return NUM((double)((long)a.n >> (long)b.n)); }
static Value B_minl(Value v){ if(v.t!=TL||v.l->len==0) return NIL(); Value m=v.l->items[0]; for(long i=1;i<v.l->len;i++) if(v.l->items[i].n<m.n) m=v.l->items[i]; return m; }
static Value B_maxl(Value v){ if(v.t!=TL||v.l->len==0) return NIL(); Value m=v.l->items[0]; for(long i=1;i<v.l->len;i++) if(v.l->items[i].n>m.n) m=v.l->items[i]; return m; }
static Value B_suml(Value v){ double s=0; if(v.t==TL) for(long i=0;i<v.l->len;i++) s+=v.l->items[i].n; return NUM(s); }
static Value B_productl(Value v){ double s=1; if(v.t==TL) for(long i=0;i<v.l->len;i++) s*=v.l->items[i].n; return NUM(s); }
static Value B_push(Value lst, Value x){ if(lst.t==TL) listpush(lst,x); return lst; }
static Value B_pop(Value lst){ if(lst.t==TL && lst.l->len>0) return lst.l->items[--lst.l->len]; return NIL(); }
static Value B_remove_at(Value lst, Value iv){ if(lst.t==TL){ long i=(long)iv.n; if(i>=0&&i<lst.l->len){ for(long j=i;j<lst.l->len-1;j++) lst.l->items[j]=lst.l->items[j+1]; lst.l->len--; } } return lst; }
static Value B_sqrt(Value v){ double x=v.n; if(x<=0) return NUM(0); double g=x>1?x:1.0; for(int i=0;i<60;i++) g=(g+x/g)/2; return NUM(g); }
static Value B_power(Value a, Value b){ double base=a.n; long e=(long)b.n; double r=1; long n=e<0?-e:e; for(long i=0;i<n;i++) r*=base; return NUM(e<0?1.0/r:r); }
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
    if (truthy(EQ(v_name, STR("type_of")))) {
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
    if (truthy(EQ(v_name, STR("is_number")))) {
        return STR("B_is_number");
    }
    if (truthy(EQ(v_name, STR("is_text")))) {
        return STR("B_is_text");
    }
    if (truthy(EQ(v_name, STR("is_list")))) {
        return STR("B_is_list");
    }
    if (truthy(EQ(v_name, STR("is_map")))) {
        return STR("B_is_map");
    }
    if (truthy(EQ(v_name, STR("is_nothing")))) {
        return STR("B_is_nothing");
    }
    if (truthy(EQ(v_name, STR("is_bool")))) {
        return STR("B_is_bool");
    }
    if (truthy(EQ(v_name, STR("band")))) {
        return STR("B_band");
    }
    if (truthy(EQ(v_name, STR("bor")))) {
        return STR("B_bor");
    }
    if (truthy(EQ(v_name, STR("bxor")))) {
        return STR("B_bxor");
    }
    if (truthy(EQ(v_name, STR("bnot")))) {
        return STR("B_bnot");
    }
    if (truthy(EQ(v_name, STR("shift_left")))) {
        return STR("B_shl");
    }
    if (truthy(EQ(v_name, STR("shift_right")))) {
        return STR("B_shr");
    }
    if (truthy(EQ(v_name, STR("min")))) {
        return STR("B_minl");
    }
    if (truthy(EQ(v_name, STR("max")))) {
        return STR("B_maxl");
    }
    if (truthy(EQ(v_name, STR("sum")))) {
        return STR("B_suml");
    }
    if (truthy(EQ(v_name, STR("product")))) {
        return STR("B_productl");
    }
    if (truthy(EQ(v_name, STR("push")))) {
        return STR("B_push");
    }
    if (truthy(EQ(v_name, STR("pop")))) {
        return STR("B_pop");
    }
    if (truthy(EQ(v_name, STR("remove_at")))) {
        return STR("B_remove_at");
    }
    if (truthy(EQ(v_name, STR("sqrt")))) {
        return STR("B_sqrt");
    }
    if (truthy(EQ(v_name, STR("power")))) {
        return STR("B_power");
    }
    if (truthy(EQ(v_name, STR("url_encode")))) {
        return STR("B_url_encode");
    }
    if (truthy(EQ(v_name, STR("url_decode")))) {
        return STR("B_url_decode");
    }
    if (truthy(EQ(v_name, STR("html_escape")))) {
        return STR("B_html_escape");
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
        if (truthy(EQ(v_name, STR("assert")))) {
            if (truthy(EQ(B_length(INDEX(v_node, STR("args"))), NUM(1)))) {
                return ADD(ADD(STR("B_assert("), v_gen_expr(INDEX(INDEX(v_node, STR("args")), NUM(0)))), STR(", STR(\"\"))"));
            }
            return ADD(ADD(ADD(ADD(STR("B_assert("), v_gen_expr(INDEX(INDEX(v_node, STR("args")), NUM(0)))), STR(", ")), v_gen_expr(INDEX(INDEX(v_node, STR("args")), NUM(1)))), STR(")"));
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
    v_RUNTIME_B64 = STR("I2luY2x1ZGUgPHN0ZGlvLmg+CiNpbmNsdWRlIDxzdGRsaWIuaD4KI2luY2x1ZGUgPHN0cmluZy5oPgojaW5jbHVkZSA8c3RkYXJnLmg+CiNpbmNsdWRlIDxjdHlwZS5oPgojaW5jbHVkZSA8c3lzL3NvY2tldC5oPgojaW5jbHVkZSA8bmV0aW5ldC9pbi5oPgojaW5jbHVkZSA8YXJwYS9pbmV0Lmg+CiNpbmNsdWRlIDx1bmlzdGQuaD4KI2luY2x1ZGUgPHN5cy9zdGF0Lmg+CiNpbmNsdWRlIDxkaXJlbnQuaD4KI2luY2x1ZGUgPHRpbWUuaD4KCnR5cGVkZWYgZW51bSB7IFROLCBUUywgVEIsIFRMLCBUTSwgVFggfSBUYWc7CnR5cGVkZWYgc3RydWN0IFZhbHVlIFZhbHVlOwp0eXBlZGVmIHN0cnVjdCB7IFZhbHVlKiBpdGVtczsgbG9uZyBsZW4sIGNhcDsgaW50IHBlcm07IH0gTGlzdDsKdHlwZWRlZiBzdHJ1Y3QgeyBjaGFyKioga2V5czsgVmFsdWUqIHZhbHM7IGxvbmcgbGVuLCBjYXA7IGludCBwZXJtOyB9IE1hcDsKc3RydWN0IFZhbHVlIHsgVGFnIHQ7IGRvdWJsZSBuOyBjaGFyKiBzOyBMaXN0KiBsOyBNYXAqIG07IH07CgovKiA9PT09PSBFYmIgR0M6IHRyYWNrIGFsbG9jYXRpb25zIGR1cmluZyBhIHJlcXVlc3QsIGZyZWUgb24gZWJiLCBwcm9tb3RlIGVzY2FwZXMgPT09PT0gKi8Kc3RhdGljIHZvaWQqKiBnX3JlYz0wOyBzdGF0aWMgbG9uZyBnX3JlY2xlbj0wLGdfcmVjY2FwPTA7IHN0YXRpYyBpbnQgZ19pbl9yZXE9MDsKc3RhdGljIHZvaWQgZ19yZWNvcmQodm9pZCogcCl7IGlmKGdfcmVjbGVuPj1nX3JlY2NhcCl7IGdfcmVjY2FwPWdfcmVjY2FwP2dfcmVjY2FwKjI6MjA0ODsgZ19yZWM9cmVhbGxvYyhnX3JlYyxnX3JlY2NhcCpzaXplb2Yodm9pZCopKTsgfSBnX3JlY1tnX3JlY2xlbisrXT1wOyB9CnN0YXRpYyB2b2lkKiBnYWxsb2MobG9uZyBuKXsgdm9pZCogcD1tYWxsb2Mobik7IGlmKGdfaW5fcmVxKSBnX3JlY29yZChwKTsgcmV0dXJuIHA7IH0Kc3RhdGljIGNoYXIqIGdzdHJkdXAoY29uc3QgY2hhciogcyl7IGlmKCFzKXM9IiI7IGxvbmcgbj1zdHJsZW4ocykrMTsgY2hhciogcj1nYWxsb2Mobik7IG1lbWNweShyLHMsbik7IHJldHVybiByOyB9CnN0YXRpYyB2b2lkKiBncmVhbGxvYyh2b2lkKiBvbGQsbG9uZyBuKXsgdm9pZCogcD1yZWFsbG9jKG9sZCxuKTsgaWYocCE9b2xkKXsgZm9yKGxvbmcgaT1nX3JlY2xlbi0xO2k+PTA7aS0tKSBpZihnX3JlY1tpXT09b2xkKXsgZ19yZWNbaV09cDsgYnJlYWs7IH0gfSByZXR1cm4gcDsgfQpzdGF0aWMgdm9pZCBnX3VucmVjKHZvaWQqIHApeyBmb3IobG9uZyBpPWdfcmVjbGVuLTE7aT49MDtpLS0pIGlmKGdfcmVjW2ldPT1wKXsgZ19yZWNbaV09Z19yZWNbLS1nX3JlY2xlbl07IHJldHVybjsgfSB9CnN0YXRpYyB2b2lkIGViYih2b2lkKXsgZm9yKGxvbmcgaT0wO2k8Z19yZWNsZW47aSsrKSBmcmVlKGdfcmVjW2ldKTsgZ19yZWNsZW49MDsgfQpzdGF0aWMgdm9pZCBwaW4oVmFsdWUgdil7IGlmKHYudD09VFMpeyBpZih2LnMpIGdfdW5yZWModi5zKTsgcmV0dXJuOyB9IGlmKHYudD09VEwpeyBpZih2LmwtPnBlcm0pIHJldHVybjsgZ191bnJlYyh2LmwpOyBnX3VucmVjKHYubC0+aXRlbXMpOyB2LmwtPnBlcm09MTsgZm9yKGxvbmcgaT0wO2k8di5sLT5sZW47aSsrKSBwaW4odi5sLT5pdGVtc1tpXSk7IHJldHVybjsgfSBpZih2LnQ9PVRNKXsgaWYodi5tLT5wZXJtKSByZXR1cm47IGdfdW5yZWModi5tKTsgZ191bnJlYyh2Lm0tPmtleXMpOyBnX3VucmVjKHYubS0+dmFscyk7IHYubS0+cGVybT0xOyBmb3IobG9uZyBpPTA7aTx2Lm0tPmxlbjtpKyspeyBnX3VucmVjKHYubS0+a2V5c1tpXSk7IHBpbih2Lm0tPnZhbHNbaV0pOyB9IHJldHVybjsgfSB9CgpzdGF0aWMgaW50IGdfYXJnYz0wOyBzdGF0aWMgY2hhcioqIGdfYXJndj0wOwoKc3RhdGljIFZhbHVlIE5VTShkb3VibGUgbil7IFZhbHVlIHY7IHYudD1UTjsgdi5uPW47IHYucz0wOyB2Lmw9MDsgdi5tPTA7IHJldHVybiB2OyB9CnN0YXRpYyBWYWx1ZSBCT09MVihpbnQgYil7IFZhbHVlIHY9TlVNKGI/MTowKTsgdi50PVRCOyByZXR1cm4gdjsgfQpzdGF0aWMgVmFsdWUgTklMKHZvaWQpeyBWYWx1ZSB2PU5VTSgwKTsgdi50PVRYOyByZXR1cm4gdjsgfQpzdGF0aWMgVmFsdWUgU1RSKGNvbnN0IGNoYXIqIHMpeyBWYWx1ZSB2OyB2LnQ9VFM7IHYucz1nc3RyZHVwKHM/czoiIik7IHYubj0wOyB2Lmw9MDsgdi5tPTA7IHJldHVybiB2OyB9CnN0YXRpYyBjaGFyKiBudW1zdHIoZG91YmxlIGQpeyBjaGFyKiBiPWdhbGxvYyg0MCk7IGlmKGQ9PShsb25nKWQpIHNwcmludGYoYiwiJWxkIiwobG9uZylkKTsgZWxzZSBzcHJpbnRmKGIsIiVnIixkKTsgcmV0dXJuIGI7IH0Kc3RhdGljIGNoYXIqIHRvc3RyKFZhbHVlIHYpewogIGlmKHYudD09VFMpIHJldHVybiB2LnM7CiAgaWYodi50PT1UTikgcmV0dXJuIG51bXN0cih2Lm4pOwogIGlmKHYudD09VEIpIHJldHVybiB2Lm4hPTA/InllcyI6Im5vIjsKICBpZih2LnQ9PVRYKSByZXR1cm4gIm5vdGhpbmciOwogIGlmKHYudD09VEwpeyBsb25nIGNhcD0yNTYsbGVuPTE7IGNoYXIqIG89Z2FsbG9jKGNhcCk7IG9bMF09J1snOyBmb3IobG9uZyBpPTA7aTx2LmwtPmxlbjtpKyspeyBWYWx1ZSBlPXYubC0+aXRlbXNbaV07IGNoYXIqIHA9dG9zdHIoZSk7IGxvbmcgbHA9c3RybGVuKHApOyBpbnQgcT0oZS50PT1UUyk7IGxvbmcgbmVlZD1sZW4rbHArNjsgaWYobmVlZD5jYXApeyBjYXA9bmVlZCoyOyBvPWdyZWFsbG9jKG8sY2FwKTt9IGlmKGkpeyBvW2xlbisrXT0nLCc7IG9bbGVuKytdPScgJzsgfSBpZihxKW9bbGVuKytdPSciJzsgbWVtY3B5KG8rbGVuLHAsbHApOyBsZW4rPWxwOyBpZihxKW9bbGVuKytdPSciJzsgfSBvW2xlbisrXT0nXSc7IG9bbGVuXT0wOyByZXR1cm4gbzsgfQogIGlmKHYudD09VE0peyBsb25nIGNhcD0yNTYsbGVuPTE7IGNoYXIqIG89Z2FsbG9jKGNhcCk7IG9bMF09J3snOyBmb3IobG9uZyBpPTA7aTx2Lm0tPmxlbjtpKyspeyBjaGFyKiBrPXYubS0+a2V5c1tpXTsgVmFsdWUgdnY9di5tLT52YWxzW2ldOyBjaGFyKiBwPXRvc3RyKHZ2KTsgbG9uZyBsaz1zdHJsZW4oayksbHA9c3RybGVuKHApOyBpbnQgcT0odnYudD09VFMpOyBsb25nIG5lZWQ9bGVuK2xrK2xwKzg7IGlmKG5lZWQ+Y2FwKXsgY2FwPW5lZWQqMjsgbz1ncmVhbGxvYyhvLGNhcCk7fSBpZihpKXsgb1tsZW4rK109JywnOyBvW2xlbisrXT0nICc7IH0gb1tsZW4rK109JyInOyBtZW1jcHkobytsZW4sayxsayk7IGxlbis9bGs7IG9bbGVuKytdPSciJzsgb1tsZW4rK109JzonOyBvW2xlbisrXT0nICc7IGlmKHEpb1tsZW4rK109JyInOyBtZW1jcHkobytsZW4scCxscCk7IGxlbis9bHA7IGlmKHEpb1tsZW4rK109JyInOyB9IG9bbGVuKytdPSd9Jzsgb1tsZW5dPTA7IHJldHVybiBvOyB9CiAgcmV0dXJuICIiOwp9CnN0YXRpYyBpbnQgdHJ1dGh5KFZhbHVlIHYpeyBpZih2LnQ9PVRYKSByZXR1cm4gMDsgaWYodi50PT1UQikgcmV0dXJuIHYubiE9MDsgcmV0dXJuIDE7IH0Kc3RhdGljIGludCB2ZXEoVmFsdWUgYSwgVmFsdWUgYil7IGlmKChhLnQ9PVROfHxhLnQ9PVRCKSYmKGIudD09VE58fGIudD09VEIpKSByZXR1cm4gYS5uPT1iLm47IGlmKGEudCE9Yi50KSByZXR1cm4gMDsgaWYoYS50PT1UUykgcmV0dXJuIHN0cmNtcChhLnMsYi5zKT09MDsgaWYoYS50PT1UWCkgcmV0dXJuIDE7IHJldHVybiAwOyB9CnN0YXRpYyBWYWx1ZSBBREQoVmFsdWUgYSwgVmFsdWUgYil7IGlmKGEudD09VE4mJmIudD09VE4pIHJldHVybiBOVU0oYS5uK2Iubik7IGNoYXIqIHg9dG9zdHIoYSk7IGNoYXIqIHk9dG9zdHIoYik7IGNoYXIqIHI9bWFsbG9jKHN0cmxlbih4KStzdHJsZW4oeSkrMSk7IHN0cmNweShyLHgpOyBzdHJjYXQocix5KTsgVmFsdWUgdj1TVFIocik7IGZyZWUocik7IHJldHVybiB2OyB9CnN0YXRpYyBWYWx1ZSBTVUIoVmFsdWUgYSxWYWx1ZSBiKXsgcmV0dXJuIE5VTShhLm4tYi5uKTsgfQpzdGF0aWMgVmFsdWUgTVVMKFZhbHVlIGEsVmFsdWUgYil7IHJldHVybiBOVU0oYS5uKmIubik7IH0Kc3RhdGljIFZhbHVlIERJVlYoVmFsdWUgYSxWYWx1ZSBiKXsgcmV0dXJuIE5VTShhLm4vYi5uKTsgfQpzdGF0aWMgVmFsdWUgTkVHKFZhbHVlIGEpeyByZXR1cm4gTlVNKC1hLm4pOyB9CnN0YXRpYyBWYWx1ZSBFUShWYWx1ZSBhLFZhbHVlIGIpeyByZXR1cm4gQk9PTFYodmVxKGEsYikpOyB9CnN0YXRpYyBWYWx1ZSBORShWYWx1ZSBhLFZhbHVlIGIpeyByZXR1cm4gQk9PTFYoIXZlcShhLGIpKTsgfQpzdGF0aWMgVmFsdWUgTFQoVmFsdWUgYSxWYWx1ZSBiKXsgaWYoYS50PT1UUyYmYi50PT1UUykgcmV0dXJuIEJPT0xWKHN0cmNtcChhLnMsYi5zKTwwKTsgcmV0dXJuIEJPT0xWKGEubjxiLm4pOyB9CnN0YXRpYyBWYWx1ZSBHVChWYWx1ZSBhLFZhbHVlIGIpeyBpZihhLnQ9PVRTJiZiLnQ9PVRTKSByZXR1cm4gQk9PTFYoc3RyY21wKGEucyxiLnMpPjApOyByZXR1cm4gQk9PTFYoYS5uPmIubik7IH0Kc3RhdGljIFZhbHVlIExFKFZhbHVlIGEsVmFsdWUgYil7IGlmKGEudD09VFMmJmIudD09VFMpIHJldHVybiBCT09MVihzdHJjbXAoYS5zLGIucyk8PTApOyByZXR1cm4gQk9PTFYoYS5uPD1iLm4pOyB9CnN0YXRpYyBWYWx1ZSBHRShWYWx1ZSBhLFZhbHVlIGIpeyBpZihhLnQ9PVRTJiZiLnQ9PVRTKSByZXR1cm4gQk9PTFYoc3RyY21wKGEucyxiLnMpPj0wKTsgcmV0dXJuIEJPT0xWKGEubj49Yi5uKTsgfQpzdGF0aWMgVmFsdWUgQU5EVihWYWx1ZSBhLFZhbHVlIGIpeyByZXR1cm4gQk9PTFYodHJ1dGh5KGEpJiZ0cnV0aHkoYikpOyB9CnN0YXRpYyBWYWx1ZSBPUlYoVmFsdWUgYSxWYWx1ZSBiKXsgcmV0dXJuIEJPT0xWKHRydXRoeShhKXx8dHJ1dGh5KGIpKTsgfQpzdGF0aWMgVmFsdWUgTk9UVihWYWx1ZSBhKXsgcmV0dXJuIEJPT0xWKCF0cnV0aHkoYSkpOyB9CnN0YXRpYyBMaXN0KiBuZXdsaXN0KHZvaWQpeyBMaXN0KiBsPWdhbGxvYyhzaXplb2YoTGlzdCkpOyBsLT5sZW49MDsgbC0+Y2FwPTg7IGwtPml0ZW1zPWdhbGxvYyhzaXplb2YoVmFsdWUpKjgpOyBsLT5wZXJtPSFnX2luX3JlcTsgcmV0dXJuIGw7IH0Kc3RhdGljIFZhbHVlIExJU1QwKHZvaWQpeyBWYWx1ZSB2OyB2LnQ9VEw7IHYubD1uZXdsaXN0KCk7IHYucz0wOyB2Lm09MDsgdi5uPTA7IHJldHVybiB2OyB9CnN0YXRpYyB2b2lkIGxpc3RwdXNoKFZhbHVlIGx2LCBWYWx1ZSB4KXsgTGlzdCogbD1sdi5sOyBpZihsLT5wZXJtKSBwaW4oeCk7IGlmKGwtPmxlbj49bC0+Y2FwKXsgbC0+Y2FwKj0yOyBsLT5pdGVtcz1ncmVhbGxvYyhsLT5pdGVtcyxzaXplb2YoVmFsdWUpKmwtPmNhcCk7fSBsLT5pdGVtc1tsLT5sZW4rK109eDsgfQpzdGF0aWMgVmFsdWUgTUtMSVNUKGludCBuLCAuLi4peyBWYWx1ZSB2PUxJU1QwKCk7IHZhX2xpc3QgYXA7IHZhX3N0YXJ0KGFwLG4pOyBmb3IoaW50IGk9MDtpPG47aSsrKSBsaXN0cHVzaCh2LCB2YV9hcmcoYXAsVmFsdWUpKTsgdmFfZW5kKGFwKTsgcmV0dXJuIHY7IH0Kc3RhdGljIE1hcCogbmV3bWFwKHZvaWQpeyBNYXAqIG09Z2FsbG9jKHNpemVvZihNYXApKTsgbS0+bGVuPTA7IG0tPmNhcD04OyBtLT5rZXlzPWdhbGxvYyhzaXplb2YoY2hhciopKjgpOyBtLT52YWxzPWdhbGxvYyhzaXplb2YoVmFsdWUpKjgpOyBtLT5wZXJtPSFnX2luX3JlcTsgcmV0dXJuIG07IH0Kc3RhdGljIFZhbHVlIE1BUDAodm9pZCl7IFZhbHVlIHY7IHYudD1UTTsgdi5tPW5ld21hcCgpOyB2LnM9MDsgdi5sPTA7IHYubj0wOyByZXR1cm4gdjsgfQpzdGF0aWMgdm9pZCBtYXBzZXQoVmFsdWUgbXYsIFZhbHVlIGssIFZhbHVlIHZhbCl7IE1hcCogbT1tdi5tOyBjaGFyKiBrZXk9dG9zdHIoayk7IGlmKG0tPnBlcm0pIHBpbih2YWwpOyBmb3IobG9uZyBpPTA7aTxtLT5sZW47aSsrKSBpZihzdHJjbXAobS0+a2V5c1tpXSxrZXkpPT0wKXsgbS0+dmFsc1tpXT12YWw7IHJldHVybjsgfSBpZihtLT5sZW4+PW0tPmNhcCl7IG0tPmNhcCo9MjsgbS0+a2V5cz1ncmVhbGxvYyhtLT5rZXlzLHNpemVvZihjaGFyKikqbS0+Y2FwKTsgbS0+dmFscz1ncmVhbGxvYyhtLT52YWxzLHNpemVvZihWYWx1ZSkqbS0+Y2FwKTt9IG0tPmtleXNbbS0+bGVuXT0obS0+cGVybT9zdHJkdXAoa2V5KTpnc3RyZHVwKGtleSkpOyBtLT52YWxzW20tPmxlbl09dmFsOyBtLT5sZW4rKzsgfQpzdGF0aWMgVmFsdWUgTUtNQVAoaW50IG4sIC4uLil7IFZhbHVlIHY9TUFQMCgpOyB2YV9saXN0IGFwOyB2YV9zdGFydChhcCxuKTsgZm9yKGludCBpPTA7aTxuO2krKyl7IFZhbHVlIGs9dmFfYXJnKGFwLFZhbHVlKTsgVmFsdWUgdmFsPXZhX2FyZyhhcCxWYWx1ZSk7IG1hcHNldCh2LGssdmFsKTt9IHZhX2VuZChhcCk7IHJldHVybiB2OyB9CnN0YXRpYyBWYWx1ZSBJTkRFWChWYWx1ZSBjLCBWYWx1ZSBrKXsKICBpZihjLnQ9PVRMKXsgbG9uZyBpPShsb25nKWsubjsgaWYoaTwwKWkrPWMubC0+bGVuOyBpZihpPDB8fGk+PWMubC0+bGVuKSByZXR1cm4gTklMKCk7IHJldHVybiBjLmwtPml0ZW1zW2ldOyB9CiAgaWYoYy50PT1UTSl7IGNoYXIqIGtleT10b3N0cihrKTsgZm9yKGxvbmcgaT0wO2k8Yy5tLT5sZW47aSsrKSBpZihzdHJjbXAoYy5tLT5rZXlzW2ldLGtleSk9PTApIHJldHVybiBjLm0tPnZhbHNbaV07IHJldHVybiBOSUwoKTsgfQogIGlmKGMudD09VFMpeyBsb25nIEw9c3RybGVuKGMucyk7IGxvbmcgaT0obG9uZylrLm47IGlmKGk8MClpKz1MOyBpZihpPDB8fGk+PUwpIHJldHVybiBTVFIoIiIpOyBjaGFyIGJbMl09e2Muc1tpXSwwfTsgcmV0dXJuIFNUUihiKTsgfQogIHJldHVybiBOSUwoKTsKfQpzdGF0aWMgdm9pZCBTRVRBVChWYWx1ZSBjLCBWYWx1ZSBrLCBWYWx1ZSB2YWwpeyBpZihjLnQ9PVRMKXsgbG9uZyBpPShsb25nKWsubjsgaWYoaT49MCYmaTxjLmwtPmxlbikgYy5sLT5pdGVtc1tpXT12YWw7IH0gZWxzZSBpZihjLnQ9PVRNKSBtYXBzZXQoYyxrLHZhbCk7IH0Kc3RhdGljIFZhbHVlIFNMSUNFKFZhbHVlIGMsIFZhbHVlIGEsIFZhbHVlIGIpeyBsb25nIGxvPShsb25nKWEubiwgaGk9KGxvbmcpYi5uOwogIGlmKGMudD09VFMpeyBsb25nIEw9c3RybGVuKGMucyk7IGlmKGxvPDApbG8rPUw7IGlmKGhpPDApaGkrPUw7IGlmKGxvPDApbG89MDsgaWYoaGk+TCloaT1MOyBpZihoaTxsbyloaT1sbzsgY2hhciogcj1tYWxsb2MoaGktbG8rMSk7IG1lbWNweShyLGMucytsbyxoaS1sbyk7IHJbaGktbG9dPTA7IFZhbHVlIHY9U1RSKHIpOyBmcmVlKHIpOyByZXR1cm4gdjsgfQogIGlmKGMudD09VEwpeyBWYWx1ZSB2PUxJU1QwKCk7IGxvbmcgTD1jLmwtPmxlbjsgaWYobG88MClsbys9TDsgaWYoaGk8MCloaSs9TDsgaWYobG88MClsbz0wOyBpZihoaT5MKWhpPUw7IGZvcihsb25nIGk9bG87aTxoaTtpKyspIGxpc3RwdXNoKHYsYy5sLT5pdGVtc1tpXSk7IHJldHVybiB2OyB9CiAgcmV0dXJuIE5JTCgpOwp9CnN0YXRpYyBWYWx1ZSBMRU4oVmFsdWUgdil7IGlmKHYudD09VFMpIHJldHVybiBOVU0oc3RybGVuKHYucykpOyBpZih2LnQ9PVRMKSByZXR1cm4gTlVNKHYubC0+bGVuKTsgaWYodi50PT1UTSkgcmV0dXJuIE5VTSh2Lm0tPmxlbik7IHJldHVybiBOVU0oMCk7IH0Kc3RhdGljIFZhbHVlIElOT1AoVmFsdWUgYSwgVmFsdWUgYil7IGlmKGIudD09VFMmJmEudD09VFMpIHJldHVybiBCT09MVihzdHJzdHIoYi5zLGEucykhPTApOyBpZihiLnQ9PVRMKXsgZm9yKGxvbmcgaT0wO2k8Yi5sLT5sZW47aSsrKSBpZih2ZXEoYSxiLmwtPml0ZW1zW2ldKSkgcmV0dXJuIEJPT0xWKDEpOyByZXR1cm4gQk9PTFYoMCk7fSBpZihiLnQ9PVRNKXsgY2hhcioga2V5PXRvc3RyKGEpOyBmb3IobG9uZyBpPTA7aTxiLm0tPmxlbjtpKyspIGlmKHN0cmNtcChiLm0tPmtleXNbaV0sa2V5KT09MCkgcmV0dXJuIEJPT0xWKDEpOyByZXR1cm4gQk9PTFYoMCk7fSByZXR1cm4gQk9PTFYoMCk7IH0Kc3RhdGljIHZvaWQgU0FZKFZhbHVlIHYpeyBwcmludGYoIiVzXG4iLCB0b3N0cih2KSk7IH0Kc3RhdGljIFZhbHVlIEJfdGV4dChWYWx1ZSBhKXsgcmV0dXJuIFNUUih0b3N0cihhKSk7IH0Kc3RhdGljIFZhbHVlIEJfbGVuZ3RoKFZhbHVlIGEpeyByZXR1cm4gTEVOKGEpOyB9CnN0YXRpYyBWYWx1ZSBCX2tleXMoVmFsdWUgbSl7IFZhbHVlIHY9TElTVDAoKTsgaWYobS50PT1UTSkgZm9yKGxvbmcgaT0wO2k8bS5tLT5sZW47aSsrKSBsaXN0cHVzaCh2LFNUUihtLm0tPmtleXNbaV0pKTsgcmV0dXJuIHY7IH0Kc3RhdGljIFZhbHVlIEJfdmFsdWVzKFZhbHVlIG0peyBWYWx1ZSB2PUxJU1QwKCk7IGlmKG0udD09VE0pIGZvcihsb25nIGk9MDtpPG0ubS0+bGVuO2krKykgbGlzdHB1c2godixtLm0tPnZhbHNbaV0pOyByZXR1cm4gdjsgfQpzdGF0aWMgVmFsdWUgQl9yYW5nZShWYWx1ZSBhLCBWYWx1ZSBiLCBpbnQgdHdvKXsgVmFsdWUgdj1MSVNUMCgpOyBsb25nIGxvPXR3bz8obG9uZylhLm46MCwgaGk9dHdvPyhsb25nKWIubjoobG9uZylhLm47IGZvcihsb25nIGk9bG87aTxoaTtpKyspIGxpc3RwdXNoKHYsTlVNKGkpKTsgcmV0dXJuIHY7IH0Kc3RhdGljIFZhbHVlIEJfdXBwZXIoVmFsdWUgYSl7IGNoYXIqIHM9Z3N0cmR1cCh0b3N0cihhKSk7IGZvcihjaGFyKiBwPXM7KnA7cCsrKSpwPXRvdXBwZXIoKHVuc2lnbmVkIGNoYXIpKnApOyByZXR1cm4gU1RSKHMpOyB9CnN0YXRpYyBWYWx1ZSBCX2xvd2VyKFZhbHVlIGEpeyBjaGFyKiBzPWdzdHJkdXAodG9zdHIoYSkpOyBmb3IoY2hhciogcD1zOypwO3ArKykqcD10b2xvd2VyKCh1bnNpZ25lZCBjaGFyKSpwKTsgcmV0dXJuIFNUUihzKTsgfQpzdGF0aWMgVmFsdWUgQl90cmltKFZhbHVlIGEpeyBjaGFyKiBzPXRvc3RyKGEpOyB3aGlsZSgqcz09JyAnfHwqcz09J1x0J3x8KnM9PSdcbicpcysrOyBsb25nIGU9c3RybGVuKHMpOyB3aGlsZShlPjAmJihzW2UtMV09PScgJ3x8c1tlLTFdPT0nXHQnfHxzW2UtMV09PSdcbicpKWUtLTsgY2hhciogcj1tYWxsb2MoZSsxKTsgbWVtY3B5KHIscyxlKTsgcltlXT0wOyByZXR1cm4gU1RSKHIpOyB9CnN0YXRpYyBWYWx1ZSBCX251bWJlcihWYWx1ZSBhKXsgaWYoYS50PT1UTikgcmV0dXJuIGE7IHJldHVybiBOVU0oYXRvZih0b3N0cihhKSkpOyB9CnN0YXRpYyBWYWx1ZSBCX2pvaW4oVmFsdWUgbHN0LCBWYWx1ZSBzZXApeyBpZihsc3QudCE9VEwpIHJldHVybiBTVFIoIiIpOyBjaGFyKiBkPXRvc3RyKHNlcCk7IGxvbmcgY2FwPTgxOTI7IGNoYXIqIG89bWFsbG9jKGNhcCk7IG9bMF09MDsgbG9uZyBsbj0wOyBmb3IobG9uZyBpPTA7aTxsc3QubC0+bGVuO2krKyl7IGNoYXIqIHBpZWNlPXRvc3RyKGxzdC5sLT5pdGVtc1tpXSk7IGxvbmcgbmVlZD1sbitzdHJsZW4ocGllY2UpK3N0cmxlbihkKSsxOyBpZihuZWVkPmNhcCl7IGNhcD1uZWVkKjI7IG89cmVhbGxvYyhvLGNhcCk7fSBpZihpKXsgc3RyY2F0KG8sZCk7fSBzdHJjYXQobyxwaWVjZSk7IGxuPXN0cmxlbihvKTt9IHJldHVybiBTVFIobyk7IH0Kc3RhdGljIFZhbHVlIEJfc3BsaXQoVmFsdWUgYSwgVmFsdWUgc2Vwdil7IFZhbHVlIHY9TElTVDAoKTsgY2hhciogcz10b3N0cihhKTsgY2hhciogc2VwPXRvc3RyKHNlcHYpOyBsb25nIHNsPXN0cmxlbihzZXApOyBpZihzbD09MCl7IGZvcihsb25nIGk9MDtzW2ldO2krKyl7IGNoYXIgYlsyXT17c1tpXSwwfTsgbGlzdHB1c2godixTVFIoYikpOyB9IHJldHVybiB2OyB9IGNoYXIqIHA9czsgY2hhciogcTsgd2hpbGUoKHE9c3Ryc3RyKHAsc2VwKSkpeyBsb25nIG49cS1wOyBjaGFyKiByPW1hbGxvYyhuKzEpOyBtZW1jcHkocixwLG4pOyByW25dPTA7IGxpc3RwdXNoKHYsU1RSKHIpKTsgZnJlZShyKTsgcD1xK3NsOyB9IGxpc3RwdXNoKHYsU1RSKHApKTsgcmV0dXJuIHY7IH0Kc3RhdGljIFZhbHVlIEJfc29ydChWYWx1ZSBsc3QpeyBpZihsc3QudCE9VEwpIHJldHVybiBsc3Q7IFZhbHVlIHY9TElTVDAoKTsgZm9yKGxvbmcgaT0wO2k8bHN0LmwtPmxlbjtpKyspIGxpc3RwdXNoKHYsbHN0LmwtPml0ZW1zW2ldKTsgZm9yKGxvbmcgaT0xO2k8di5sLT5sZW47aSsrKXsgVmFsdWUga2V5PXYubC0+aXRlbXNbaV07IGxvbmcgaj1pLTE7IHdoaWxlKGo+PTAgJiYgdHJ1dGh5KEdUKHYubC0+aXRlbXNbal0sa2V5KSkpeyB2LmwtPml0ZW1zW2orMV09di5sLT5pdGVtc1tqXTsgai0tOyB9IHYubC0+aXRlbXNbaisxXT1rZXk7IH0gcmV0dXJuIHY7IH0Kc3RhdGljIFZhbHVlIEJfY29udGFpbnMoVmFsdWUgYSwgVmFsdWUgYil7IHJldHVybiBJTk9QKGIsYSk7IH0Kc3RhdGljIFZhbHVlIEJfc2xpY2UoVmFsdWUgYywgVmFsdWUgYSwgVmFsdWUgYil7IHJldHVybiBTTElDRShjLGEsYik7IH0Kc3RhdGljIFZhbHVlIEJfcmVwbGFjZShWYWx1ZSBzLCBWYWx1ZSBvbGR2LCBWYWx1ZSBuZXd2KXsgY2hhciogc3RyPXRvc3RyKHMpOyBjaGFyKiBvPXRvc3RyKG9sZHYpOyBjaGFyKiBudz10b3N0cihuZXd2KTsgbG9uZyBvbD1zdHJsZW4obyk7IGxvbmcgbmw9c3RybGVuKG53KTsgaWYob2w9PTApIHJldHVybiBTVFIoc3RyKTsgbG9uZyBjbnQ9MDsgeyBjaGFyKiBwPXN0cjsgY2hhciogcTsgd2hpbGUoKHE9c3Ryc3RyKHAsbykpKXsgY250Kys7IHA9cStvbDsgfSB9IGxvbmcgb3V0bGVuPXN0cmxlbihzdHIpK2NudCoobmwtb2wpKzE7IGNoYXIqIG91dD1tYWxsb2Mob3V0bGVuPjA/b3V0bGVuOjEpOyBjaGFyKiB3PW91dDsgY2hhciogcD1zdHI7IGNoYXIqIHE7IHdoaWxlKChxPXN0cnN0cihwLG8pKSl7IGxvbmcgcHJlPXEtcDsgbWVtY3B5KHcscCxwcmUpOyB3Kz1wcmU7IG1lbWNweSh3LG53LG5sKTsgdys9bmw7IHA9cStvbDsgfSBzdHJjcHkodyxwKTsgVmFsdWUgdj1TVFIob3V0KTsgZnJlZShvdXQpOyByZXR1cm4gdjsgfQpzdGF0aWMgVmFsdWUgQl9yZWFkX2ZpbGUoVmFsdWUgcHRoKXsgRklMRSogZj1mb3Blbih0b3N0cihwdGgpLCJyYiIpOyBpZighZikgcmV0dXJuIFNUUigiIik7IGZzZWVrKGYsMCxTRUVLX0VORCk7IGxvbmcgbj1mdGVsbChmKTsgZnNlZWsoZiwwLFNFRUtfU0VUKTsgY2hhciogYj1tYWxsb2MobisxKTsgZnJlYWQoYiwxLG4sZik7IGJbbl09MDsgZmNsb3NlKGYpOyBWYWx1ZSB2PVNUUihiKTsgZnJlZShiKTsgcmV0dXJuIHY7IH0Kc3RhdGljIFZhbHVlIEJfd3JpdGVfZmlsZShWYWx1ZSBwdGgsIFZhbHVlIGMpeyBGSUxFKiBmPWZvcGVuKHRvc3RyKHB0aCksIndiIik7IGlmKGYpeyBjaGFyKiBzPXRvc3RyKGMpOyBmd3JpdGUocywxLHN0cmxlbihzKSxmKTsgZmNsb3NlKGYpO30gcmV0dXJuIE5JTCgpOyB9CnN0YXRpYyBWYWx1ZSBCX2FwcGVuZF9maWxlKFZhbHVlIHB0aCwgVmFsdWUgYyl7IEZJTEUqIGY9Zm9wZW4odG9zdHIocHRoKSwiYWIiKTsgaWYoZil7IGNoYXIqIHM9dG9zdHIoYyk7IGZ3cml0ZShzLDEsc3RybGVuKHMpLGYpOyBmY2xvc2UoZik7fSByZXR1cm4gTklMKCk7IH0Kc3RhdGljIFZhbHVlIEJfYXJndW1lbnRzKHZvaWQpeyBWYWx1ZSB2PUxJU1QwKCk7IGZvcihpbnQgaT0xO2k8Z19hcmdjO2krKykgbGlzdHB1c2godixTVFIoZ19hcmd2W2ldKSk7IHJldHVybiB2OyB9CnN0YXRpYyBjaGFyKiByZWFkcGlwZShGSUxFKiBwKXsgbG9uZyBjYXA9NDA5NixsZW49MDsgY2hhciogYj1tYWxsb2MoY2FwKTsgaW50IGNoOyB3aGlsZSgoY2g9ZmdldGMocCkpIT1FT0YpeyBpZihsZW4rMT49Y2FwKXtjYXAqPTI7Yj1yZWFsbG9jKGIsY2FwKTt9IGJbbGVuKytdPWNoOyB9IGJbbGVuXT0wOyByZXR1cm4gYjsgfQpzdGF0aWMgVmFsdWUgQl9ydW4oVmFsdWUgY21kKXsgY2hhciogZnVsbD1tYWxsb2Moc3RybGVuKHRvc3RyKGNtZCkpKzgpOyBzcHJpbnRmKGZ1bGwsIiVzIDI+JjEiLHRvc3RyKGNtZCkpOyBGSUxFKiBwPXBvcGVuKGZ1bGwsInIiKTsgZnJlZShmdWxsKTsgaWYoIXApIHJldHVybiBTVFIoIiIpOyBjaGFyKiBiPXJlYWRwaXBlKHApOyBwY2xvc2UocCk7IGxvbmcgbGVuPXN0cmxlbihiKTsgd2hpbGUobGVuPjAmJmJbbGVuLTFdPT0nXG4nKSBiWy0tbGVuXT0wOyBWYWx1ZSB2PVNUUihiKTsgZnJlZShiKTsgcmV0dXJuIHY7IH0Kc3RhdGljIFZhbHVlIEJfc2hlbGwoVmFsdWUgY21kKXsgY2hhciogZnVsbD1tYWxsb2Moc3RybGVuKHRvc3RyKGNtZCkpKzgpOyBzcHJpbnRmKGZ1bGwsIiVzIDI+JjEiLHRvc3RyKGNtZCkpOyBGSUxFKiBwPXBvcGVuKGZ1bGwsInIiKTsgZnJlZShmdWxsKTsgaWYoIXApIHJldHVybiBNS01BUCgyLFNUUigib3V0cHV0IiksU1RSKCIiKSxTVFIoImNvZGUiKSxOVU0oMSkpOyBjaGFyKiBiPXJlYWRwaXBlKHApOyBpbnQgc3Q9cGNsb3NlKHApOyBpbnQgY29kZT0oc3Q9PS0xKT8xOihzdD4+OCk7IGxvbmcgbGVuPXN0cmxlbihiKTsgd2hpbGUobGVuPjAmJmJbbGVuLTFdPT0nXG4nKSBiWy0tbGVuXT0wOyBWYWx1ZSB2PU1LTUFQKDIsU1RSKCJvdXRwdXQiKSxTVFIoYiksU1RSKCJjb2RlIiksTlVNKGNvZGUpKTsgZnJlZShiKTsgcmV0dXJuIHY7IH0Kc3RhdGljIGludCBiNjR2KGNoYXIgYyl7IGlmKGM+PSdBJyYmYzw9J1onKXJldHVybiBjLSdBJzsgaWYoYz49J2EnJiZjPD0neicpcmV0dXJuIGMtJ2EnKzI2OyBpZihjPj0nMCcmJmM8PSc5JylyZXR1cm4gYy0nMCcrNTI7IGlmKGM9PScrJylyZXR1cm4gNjI7IGlmKGM9PScvJylyZXR1cm4gNjM7IHJldHVybiAtMTsgfQpzdGF0aWMgVmFsdWUgQl9iNjRkZWNvZGUoVmFsdWUgc3YpeyBjaGFyKiBpbj10b3N0cihzdik7IGxvbmcgbj1zdHJsZW4oaW4pOyBjaGFyKiBvdXQ9bWFsbG9jKG4rMSk7IGxvbmcgbz0wOyBpbnQgYnVmPTAsYml0cz0wOyBmb3IobG9uZyBpPTA7aTxuO2krKyl7IGludCB2PWI2NHYoaW5baV0pOyBpZih2PDApIGNvbnRpbnVlOyBidWY9KGJ1Zjw8Nil8djsgYml0cys9NjsgaWYoYml0cz49OCl7IGJpdHMtPTg7IG91dFtvKytdPShjaGFyKSgoYnVmPj5iaXRzKSYweEZGKTsgfSB9IG91dFtvXT0wOyBWYWx1ZSByPVNUUihvdXQpOyBmcmVlKG91dCk7IHJldHVybiByOyB9CnN0YXRpYyBjaGFyKiBzZHVwKGNvbnN0IGNoYXIqIHMpeyBjaGFyKiByPW1hbGxvYyhzdHJsZW4ocykrMSk7IHN0cmNweShyLHMpOyByZXR1cm4gcjsgfQpzdGF0aWMgVmFsdWUgQl91cmxfZGVjb2RlKFZhbHVlIHYpeyBjaGFyKiBzPXRvc3RyKHYpOyBsb25nIG49c3RybGVuKHMpOyBjaGFyKiBvPW1hbGxvYyhuKzEpOyBsb25nIGo9MDsgZm9yKGxvbmcgaT0wO2k8bjtpKyspeyBpZihzW2ldPT0nJScmJmkrMjxuKXsgY2hhciBoPXNbaSsxXSxsPXNbaSsyXTsgaW50IGhpPShoPD0nOScpP2gtJzAnOih0b2xvd2VyKGgpLSdhJysxMCk7IGludCBsbz0obDw9JzknKT9sLScwJzoodG9sb3dlcihsKS0nYScrMTApOyBvW2orK109KGNoYXIpKGhpKjE2K2xvKTsgaSs9MjsgfSBlbHNlIGlmKHNbaV09PScrJykgb1tqKytdPScgJzsgZWxzZSBvW2orK109c1tpXTsgfSBvW2pdPTA7IFZhbHVlIHI9U1RSKG8pOyBmcmVlKG8pOyByZXR1cm4gcjsgfQpzdGF0aWMgVmFsdWUgQl91cmxfZW5jb2RlKFZhbHVlIHYpeyBjaGFyKiBzPXRvc3RyKHYpOyBsb25nIG49c3RybGVuKHMpOyBjaGFyKiBvPW1hbGxvYyhuKjMrMSk7IGxvbmcgaj0wOyBmb3IobG9uZyBpPTA7aTxuO2krKyl7IHVuc2lnbmVkIGNoYXIgYz1zW2ldOyBpZigoYz49J0EnJiZjPD0nWicpfHwoYz49J2EnJiZjPD0neicpfHwoYz49JzAnJiZjPD0nOScpfHxjPT0nLSd8fGM9PSdfJ3x8Yz09Jy4nfHxjPT0nficpIG9baisrXT1jOyBlbHNlIHsgc3ByaW50ZihvK2osIiUlJTAyWCIsYyk7IGorPTM7IH0gfSBvW2pdPTA7IFZhbHVlIHI9U1RSKG8pOyBmcmVlKG8pOyByZXR1cm4gcjsgfQpzdGF0aWMgVmFsdWUgQl9odG1sX2VzY2FwZShWYWx1ZSB2KXsgY2hhciogcz10b3N0cih2KTsgY2hhciogbz1tYWxsb2Moc3RybGVuKHMpKjYrMSk7IGNoYXIqIHc9bzsgZm9yKGNoYXIqIHA9czsqcDtwKyspeyBpZigqcD09JzwnKXtzdHJjcHkodywiJmx0OyIpO3crPTQ7fSBlbHNlIGlmKCpwPT0nPicpe3N0cmNweSh3LCImZ3Q7Iik7dys9NDt9IGVsc2UgaWYoKnA9PScmJyl7c3RyY3B5KHcsIiZhbXA7Iik7dys9NTt9IGVsc2UgaWYoKnA9PSciJyl7c3RyY3B5KHcsIiZxdW90OyIpO3crPTY7fSBlbHNlICp3Kys9KnA7IH0gKnc9MDsgVmFsdWUgcj1TVFIobyk7IGZyZWUobyk7IHJldHVybiByOyB9CnN0YXRpYyB2b2lkIGpzb25fc3RyKGNoYXIqKiBvdXQsbG9uZyogY2FwLGxvbmcqIGxlbixjb25zdCBjaGFyKiBzKXsgbG9uZyBuZWVkPSpsZW4rc3RybGVuKHMpKjYrNDsgaWYobmVlZD4qY2FwKXsqY2FwPW5lZWQqMjsqb3V0PXJlYWxsb2MoKm91dCwqY2FwKTt9IGNoYXIqIHc9Km91dCsqbGVuOyAqdysrPSciJzsgZm9yKGNvbnN0IGNoYXIqIHA9czsqcDtwKyspeyB1bnNpZ25lZCBjaGFyIGM9KnA7IGlmKGM9PSciJyl7KncrKz0nXFwnOyp3Kys9JyInO30gZWxzZSBpZihjPT0nXFwnKXsqdysrPSdcXCc7KncrKz0nXFwnO30gZWxzZSBpZihjPT0nXG4nKXsqdysrPSdcXCc7KncrKz0nbic7fSBlbHNlIGlmKGM9PSdcdCcpeyp3Kys9J1xcJzsqdysrPSd0Jzt9IGVsc2UgaWYoYz09J1xyJyl7KncrKz0nXFwnOyp3Kys9J3InO30gZWxzZSBpZihjPDB4MjApe3NwcmludGYodywiXFx1JTA0eCIsYyk7dys9Njt9IGVsc2UgKncrKz1jOyB9ICp3Kys9JyInOyAqdz0wOyAqbGVuPXctKm91dDsgfQpzdGF0aWMgdm9pZCB0b19qc29uX3JlYyhWYWx1ZSB2LGNoYXIqKiBvdXQsbG9uZyogY2FwLGxvbmcqIGxlbil7IGlmKCpsZW4rNjQ+KmNhcCl7KmNhcD0oKmxlbis2NCkqMjsqb3V0PXJlYWxsb2MoKm91dCwqY2FwKTt9IGlmKHYudD09VFMpe2pzb25fc3RyKG91dCxjYXAsbGVuLHYucyk7cmV0dXJuO30gaWYodi50PT1UTil7Y2hhciogbnM9bnVtc3RyKHYubik7c3RyY3B5KCpvdXQrKmxlbixucyk7Kmxlbis9c3RybGVuKG5zKTtmcmVlKG5zKTtyZXR1cm47fSBpZih2LnQ9PVRCKXtjb25zdCBjaGFyKiBiPXYubiE9MD8idHJ1ZSI6ImZhbHNlIjtzdHJjcHkoKm91dCsqbGVuLGIpOypsZW4rPXN0cmxlbihiKTtyZXR1cm47fSBpZih2LnQ9PVRYKXtzdHJjcHkoKm91dCsqbGVuLCJudWxsIik7Kmxlbis9NDtyZXR1cm47fSBpZih2LnQ9PVRMKXsoKm91dClbKCpsZW4pKytdPSdbJzsgZm9yKGxvbmcgaT0wO2k8di5sLT5sZW47aSsrKXsgaWYoaSl7aWYoKmxlbisyPipjYXApeypjYXA9KCpsZW4rMikqMjsqb3V0PXJlYWxsb2MoKm91dCwqY2FwKTt9KCpvdXQpWygqbGVuKSsrXT0nLCc7KCpvdXQpWygqbGVuKSsrXT0nICc7fSB0b19qc29uX3JlYyh2LmwtPml0ZW1zW2ldLG91dCxjYXAsbGVuKTt9IGlmKCpsZW4rMj4qY2FwKXsqY2FwPSpsZW4rMjsqb3V0PXJlYWxsb2MoKm91dCwqY2FwKTt9ICgqb3V0KVsoKmxlbikrK109J10nOyAoKm91dClbKmxlbl09MDsgcmV0dXJuO30gaWYodi50PT1UTSl7KCpvdXQpWygqbGVuKSsrXT0neyc7IGZvcihsb25nIGk9MDtpPHYubS0+bGVuO2krKyl7IGlmKGkpe2lmKCpsZW4rMj4qY2FwKXsqY2FwPSgqbGVuKzIpKjI7Km91dD1yZWFsbG9jKCpvdXQsKmNhcCk7fSgqb3V0KVsoKmxlbikrK109JywnOygqb3V0KVsoKmxlbikrK109JyAnO30ganNvbl9zdHIob3V0LGNhcCxsZW4sdi5tLT5rZXlzW2ldKTsgaWYoKmxlbisyPipjYXApeypjYXA9KCpsZW4rMikqMjsqb3V0PXJlYWxsb2MoKm91dCwqY2FwKTt9KCpvdXQpWygqbGVuKSsrXT0nOic7KCpvdXQpWygqbGVuKSsrXT0nICc7IHRvX2pzb25fcmVjKHYubS0+dmFsc1tpXSxvdXQsY2FwLGxlbik7fSBpZigqbGVuKzI+KmNhcCl7KmNhcD0qbGVuKzI7Km91dD1yZWFsbG9jKCpvdXQsKmNhcCk7fSAoKm91dClbKCpsZW4pKytdPSd9JzsgKCpvdXQpWypsZW5dPTA7IHJldHVybjt9IH0Kc3RhdGljIFZhbHVlIEJfdG9fanNvbihWYWx1ZSB2KXsgbG9uZyBjYXA9MjU2LGxlbj0wOyBjaGFyKiBvPW1hbGxvYyhjYXApOyBvWzBdPTA7IHRvX2pzb25fcmVjKHYsJm8sJmNhcCwmbGVuKTsgb1tsZW5dPTA7IFZhbHVlIHI9U1RSKG8pOyBmcmVlKG8pOyByZXR1cm4gcjsgfQpzdGF0aWMgVmFsdWUganBhcnNlKGNvbnN0IGNoYXIqIHMsbG9uZyogaSk7CnN0YXRpYyB2b2lkIGp3cyhjb25zdCBjaGFyKiBzLGxvbmcqIGkpeyB3aGlsZShzWyppXT09JyAnfHxzWyppXT09J1x0J3x8c1sqaV09PSdcbid8fHNbKmldPT0nXHInKSgqaSkrKzsgfQpzdGF0aWMgVmFsdWUganN0cmluZyhjb25zdCBjaGFyKiBzLGxvbmcqIGkpeyAoKmkpKys7IGNoYXIqIGI9bWFsbG9jKHN0cmxlbihzKSsxKTsgbG9uZyBqPTA7IHdoaWxlKHNbKmldJiZzWyppXSE9JyInKXsgaWYoc1sqaV09PSdcXCcpeyAoKmkpKys7IGNoYXIgYz1zWyppXTsgaWYoYz09J24nKWJbaisrXT0nXG4nOyBlbHNlIGlmKGM9PSd0JyliW2orK109J1x0JzsgZWxzZSBpZihjPT0ncicpYltqKytdPSdccic7IGVsc2UgaWYoYz09J3UnKXsgaW50IGNvZGU9MDsgZm9yKGludCBrPTA7azw0O2srKyl7KCppKSsrOyBjaGFyIGg9c1sqaV07IGNvZGU9Y29kZSoxNisoKGg8PSc5Jyk/aC0nMCc6KHRvbG93ZXIoaCktJ2EnKzEwKSk7fSBiW2orK109KGNoYXIpY29kZTsgfSBlbHNlIGJbaisrXT1jOyAoKmkpKys7IH0gZWxzZSBiW2orK109c1soKmkpKytdOyB9IGlmKHNbKmldPT0nIicpKCppKSsrOyBiW2pdPTA7IFZhbHVlIHY9U1RSKGIpOyBmcmVlKGIpOyByZXR1cm4gdjsgfQpzdGF0aWMgVmFsdWUganBhcnNlKGNvbnN0IGNoYXIqIHMsbG9uZyogaSl7IGp3cyhzLGkpOyBjaGFyIGM9c1sqaV07CiAgaWYoYz09JyInKXJldHVybiBqc3RyaW5nKHMsaSk7CiAgaWYoYz09J3snKXsgKCppKSsrOyBWYWx1ZSBtPU1BUDAoKTsgandzKHMsaSk7IGlmKHNbKmldPT0nfScpeygqaSkrKztyZXR1cm4gbTt9IGZvcig7Oyl7IGp3cyhzLGkpOyBWYWx1ZSBrPWpzdHJpbmcocyxpKTsgandzKHMsaSk7IGlmKHNbKmldPT0nOicpKCppKSsrOyBWYWx1ZSB2PWpwYXJzZShzLGkpOyBtYXBzZXQobSxrLHYpOyBqd3MocyxpKTsgaWYoc1sqaV09PScsJyl7KCppKSsrO2NvbnRpbnVlO30gaWYoc1sqaV09PSd9Jyl7KCppKSsrO30gYnJlYWs7IH0gcmV0dXJuIG07IH0KICBpZihjPT0nWycpeyAoKmkpKys7IFZhbHVlIGE9TElTVDAoKTsgandzKHMsaSk7IGlmKHNbKmldPT0nXScpeygqaSkrKztyZXR1cm4gYTt9IGZvcig7Oyl7IFZhbHVlIHY9anBhcnNlKHMsaSk7IGxpc3RwdXNoKGEsdik7IGp3cyhzLGkpOyBpZihzWyppXT09JywnKXsoKmkpKys7Y29udGludWU7fSBpZihzWyppXT09J10nKXsoKmkpKys7fSBicmVhazsgfSByZXR1cm4gYTsgfQogIGlmKGM9PSd0Jyl7KmkrPTQ7cmV0dXJuIEJPT0xWKDEpO30gaWYoYz09J2YnKXsqaSs9NTtyZXR1cm4gQk9PTFYoMCk7fSBpZihjPT0nbicpeyppKz00O3JldHVybiBOSUwoKTt9CiAgeyBjaGFyKiBlbmQ7IGRvdWJsZSBkPXN0cnRvZChzKyppLCZlbmQpOyAqaT1lbmQtczsgcmV0dXJuIE5VTShkKTsgfSB9CnN0YXRpYyBWYWx1ZSBCX2Zyb21fanNvbihWYWx1ZSB2KXsgbG9uZyBpPTA7IHJldHVybiBqcGFyc2UodG9zdHIodiksJmkpOyB9CnN0YXRpYyBWYWx1ZSBCX21ha2VfZGlyKFZhbHVlIHApeyBjaGFyIGNtZFs0MDk2XTsgc25wcmludGYoY21kLHNpemVvZiBjbWQsIm1rZGlyIC1wICclcyciLHRvc3RyKHApKTsgc3lzdGVtKGNtZCk7IHJldHVybiBOSUwoKTsgfQpzdGF0aWMgVmFsdWUgQl9wYXRoX2V4aXN0cyhWYWx1ZSBwKXsgc3RydWN0IHN0YXQgc3Q7IHJldHVybiBCT09MVihzdGF0KHRvc3RyKHApLCZzdCk9PTApOyB9CnN0YXRpYyBWYWx1ZSBCX2lzX2ZpbGUoVmFsdWUgcCl7IHN0cnVjdCBzdGF0IHN0OyByZXR1cm4gQk9PTFYoc3RhdCh0b3N0cihwKSwmc3QpPT0wJiZTX0lTUkVHKHN0LnN0X21vZGUpKTsgfQpzdGF0aWMgVmFsdWUgQl9pc19kaXIoVmFsdWUgcCl7IHN0cnVjdCBzdGF0IHN0OyByZXR1cm4gQk9PTFYoc3RhdCh0b3N0cihwKSwmc3QpPT0wJiZTX0lTRElSKHN0LnN0X21vZGUpKTsgfQpzdGF0aWMgVmFsdWUgQl9maWxlX3NpemUoVmFsdWUgcCl7IHN0cnVjdCBzdGF0IHN0OyBpZihzdGF0KHRvc3RyKHApLCZzdCk9PTApIHJldHVybiBOVU0oc3Quc3Rfc2l6ZSk7IHJldHVybiBOVU0oMCk7IH0Kc3RhdGljIFZhbHVlIEJfbGlzdF9kaXIoVmFsdWUgcCl7IFZhbHVlIHY9TElTVDAoKTsgRElSKiBkPW9wZW5kaXIodG9zdHIocCkpOyBpZighZClyZXR1cm4gdjsgc3RydWN0IGRpcmVudCogZTsgd2hpbGUoKGU9cmVhZGRpcihkKSkpeyBpZihzdHJjbXAoZS0+ZF9uYW1lLCIuIikmJnN0cmNtcChlLT5kX25hbWUsIi4uIikpIGxpc3RwdXNoKHYsU1RSKGUtPmRfbmFtZSkpOyB9IGNsb3NlZGlyKGQpOyByZXR1cm4gQl9zb3J0KHYpOyB9CnN0YXRpYyBWYWx1ZSBCX3JlbW92ZV9wYXRoKFZhbHVlIHApeyBjaGFyIGNtZFs0MDk2XTsgc25wcmludGYoY21kLHNpemVvZiBjbWQsInJtIC1yZiAnJXMnIix0b3N0cihwKSk7IHN5c3RlbShjbWQpOyByZXR1cm4gTklMKCk7IH0Kc3RhdGljIFZhbHVlIEJfbW92ZV9wYXRoKFZhbHVlIGEsVmFsdWUgYil7IGNoYXIgY21kWzgxOTJdOyBzbnByaW50ZihjbWQsc2l6ZW9mIGNtZCwibWtkaXIgLXAgXCIkKGRpcm5hbWUgJyVzJylcIjsgbXYgJyVzJyAnJXMnIix0b3N0cihiKSx0b3N0cihhKSx0b3N0cihiKSk7IHN5c3RlbShjbWQpOyByZXR1cm4gTklMKCk7IH0Kc3RhdGljIFZhbHVlIEJfZGlybmFtZShWYWx1ZSBwKXsgY2hhciogcz1zZHVwKHRvc3RyKHApKTsgY2hhciogc2xhc2g9c3RycmNocihzLCcvJyk7IGlmKCFzbGFzaCl7ZnJlZShzKTtyZXR1cm4gU1RSKCIiKTt9ICpzbGFzaD0wOyBWYWx1ZSB2PVNUUihzKTsgZnJlZShzKTsgcmV0dXJuIHY7IH0Kc3RhdGljIFZhbHVlIEJfYmFzZW5hbWUoVmFsdWUgcCl7IGNoYXIqIHM9dG9zdHIocCk7IGNoYXIqIHNsYXNoPXN0cnJjaHIocywnLycpOyByZXR1cm4gU1RSKHNsYXNoP3NsYXNoKzE6cyk7IH0Kc3RhdGljIFZhbHVlIEJfcGF0aF9qb2luKGludCBuLCAuLi4peyBjaGFyIGJ1Zls4MTkyXTsgYnVmWzBdPTA7IHZhX2xpc3QgYXA7IHZhX3N0YXJ0KGFwLG4pOyBmb3IoaW50IGk9MDtpPG47aSsrKXsgVmFsdWUgYT12YV9hcmcoYXAsVmFsdWUpOyBpZihpJiZidWZbMF0mJmJ1ZltzdHJsZW4oYnVmKS0xXSE9Jy8nKSBzdHJjYXQoYnVmLCIvIik7IHN0cmNhdChidWYsdG9zdHIoYSkpOyB9IHZhX2VuZChhcCk7IHJldHVybiBTVFIoYnVmKTsgfQpzdGF0aWMgVmFsdWUgQl9ob21lX2Rpcih2b2lkKXsgY2hhciogaD1nZXRlbnYoIkhPTUUiKTsgcmV0dXJuIFNUUihoP2g6Ii4iKTsgfQpzdGF0aWMgVmFsdWUgQl9lbnYoVmFsdWUgayl7IGNoYXIqIHY9Z2V0ZW52KHRvc3RyKGspKTsgcmV0dXJuIFNUUih2P3Y6IiIpOyB9CnN0YXRpYyBWYWx1ZSBCX25vdyh2b2lkKXsgcmV0dXJuIE5VTSgoZG91YmxlKXRpbWUoMCkpOyB9CnN0YXRpYyBWYWx1ZSBCX2Nsb2NrKHZvaWQpeyB0aW1lX3QgdD10aW1lKDApOyBzdHJ1Y3QgdG0qIG09bG9jYWx0aW1lKCZ0KTsgY2hhciBiWzE2XTsgc3ByaW50ZihiLCIlMDJkOiUwMmQ6JTAyZCIsbS0+dG1faG91cixtLT50bV9taW4sbS0+dG1fc2VjKTsgcmV0dXJuIFNUUihiKTsgfQpzdGF0aWMgVmFsdWUgQl90b2RheSh2b2lkKXsgdGltZV90IHQ9dGltZSgwKTsgc3RydWN0IHRtKiBtPWxvY2FsdGltZSgmdCk7IGNoYXIgYlsxNl07IHNwcmludGYoYiwiJTA0ZC0lMDJkLSUwMmQiLG0tPnRtX3llYXIrMTkwMCxtLT50bV9tb24rMSxtLT50bV9tZGF5KTsgcmV0dXJuIFNUUihiKTsgfQpzdGF0aWMgVmFsdWUgQl9odHRwX2dldChWYWx1ZSB1cmwsIFZhbHVlIGhlYWRlcnMpeyBjaGFyIGNtZFsxNjM4NF07IGludCBuPXNucHJpbnRmKGNtZCxzaXplb2YgY21kLCJjdXJsIC1zTCIpOyBpZihoZWFkZXJzLnQ9PVRNKXsgZm9yKGxvbmcgaT0wO2k8aGVhZGVycy5tLT5sZW47aSsrKSBuKz1zbnByaW50ZihjbWQrbixzaXplb2YgY21kLW4sIiAtSCAnJXM6ICVzJyIsaGVhZGVycy5tLT5rZXlzW2ldLHRvc3RyKGhlYWRlcnMubS0+dmFsc1tpXSkpOyB9IHNucHJpbnRmKGNtZCtuLHNpemVvZiBjbWQtbiwiICclcyciLHRvc3RyKHVybCkpOyBWYWx1ZSBvdXQ9Ql9ydW4oU1RSKGNtZCkpOyByZXR1cm4gTUtNQVAoMixTVFIoInN0YXR1cyIpLE5VTSgyMDApLFNUUigiYm9keSIpLG91dCk7IH0Kc3RhdGljIFZhbHVlIHBhcnNlX3F1ZXJ5KGNvbnN0IGNoYXIqIHEpeyBWYWx1ZSBtPU1BUDAoKTsgaWYoIXF8fCEqcSlyZXR1cm4gbTsgY2hhciogcz1zZHVwKHEpOyBjaGFyKiBwPXM7IHdoaWxlKHAmJipwKXsgY2hhciogYW1wPXN0cmNocihwLCcmJyk7IGlmKGFtcCkqYW1wPTA7IGNoYXIqIGVxPXN0cmNocihwLCc9Jyk7IGlmKGVxKXsqZXE9MDsgVmFsdWUgaz1CX3VybF9kZWNvZGUoU1RSKHApKTsgVmFsdWUgdj1CX3VybF9kZWNvZGUoU1RSKGVxKzEpKTsgbWFwc2V0KG0sayx2KTt9IHA9YW1wP2FtcCsxOjA7IH0gZnJlZShzKTsgcmV0dXJuIG07IH0Kc3RhdGljIGNoYXIqIGNpX3N0cnN0cihjb25zdCBjaGFyKiBoLCBjb25zdCBjaGFyKiBuKXsgaWYoISpuKSByZXR1cm4gKGNoYXIqKWg7IGZvcig7ICpoOyBoKyspeyBjb25zdCBjaGFyKiBhPWg7IGNvbnN0IGNoYXIqIGI9bjsgd2hpbGUoKmEgJiYgKmIgJiYgdG9sb3dlcigodW5zaWduZWQgY2hhcikqYSk9PXRvbG93ZXIoKHVuc2lnbmVkIGNoYXIpKmIpKXsgYSsrOyBiKys7IH0gaWYoISpiKSByZXR1cm4gKGNoYXIqKWg7IH0gcmV0dXJuIDA7IH0Kc3RhdGljIGNoYXIqIHJlY3ZfcmVxdWVzdChpbnQgYyxsb25nKiBibGVuKXsgbG9uZyBjYXA9ODE5MixsZW49MDsgY2hhciogYnVmPW1hbGxvYyhjYXApOyBmb3IoOzspeyBpZihsZW4rNDA5Nj49Y2FwKXtjYXAqPTI7YnVmPXJlYWxsb2MoYnVmLGNhcCk7fSBsb25nIHI9cmVjdihjLGJ1ZitsZW4sNDA5NiwwKTsgaWYocjw9MClicmVhazsgbGVuKz1yOyBidWZbbGVuXT0wOyBjaGFyKiBoZT1zdHJzdHIoYnVmLCJcclxuXHJcbiIpOyBpZihoZSl7IGxvbmcgaGxlbj1oZS1idWYrNDsgY2hhciogY2w9Y2lfc3Ryc3RyKGJ1ZiwiY29udGVudC1sZW5ndGg6Iik7IGxvbmcgd2FudD1jbD9hdG9sKGNsKzE1KTowOyB3aGlsZSgobG9uZykobGVuLWhsZW4pPHdhbnQpeyBpZihsZW4rNDA5Nj49Y2FwKXtjYXAqPTI7YnVmPXJlYWxsb2MoYnVmLGNhcCk7fSBsb25nIHIyPXJlY3YoYyxidWYrbGVuLDQwOTYsMCk7IGlmKHIyPD0wKWJyZWFrOyBsZW4rPXIyOyB9IGJ1ZltsZW5dPTA7IGJyZWFrOyB9IH0gKmJsZW49bGVuOyByZXR1cm4gYnVmOyB9CnN0YXRpYyBWYWx1ZSBwYXJzZV9yZXF1ZXN0KGNoYXIqIHJhdyl7IFZhbHVlIHJlcT1NQVAwKCk7IGNoYXIqIG5sPXN0cnN0cihyYXcsIlxyXG4iKTsgaWYoIW5sKXJldHVybiByZXE7ICpubD0wOyBjaGFyKiBtZXRob2Q9cmF3OyBjaGFyKiBzcD1zdHJjaHIocmF3LCcgJyk7IGlmKCFzcClyZXR1cm4gcmVxOyAqc3A9MDsgY2hhciogdGFyZ2V0PXNwKzE7IGNoYXIqIHNwMj1zdHJjaHIodGFyZ2V0LCcgJyk7IGlmKHNwMikqc3AyPTA7IGNoYXIqIHE9c3RyY2hyKHRhcmdldCwnPycpOyBjaGFyKiBxdWVyeT0iIjsgaWYocSl7KnE9MDtxdWVyeT1xKzE7fSBtYXBzZXQocmVxLFNUUigibWV0aG9kIiksU1RSKG1ldGhvZCkpOyBtYXBzZXQocmVxLFNUUigicGF0aCIpLEJfdXJsX2RlY29kZShTVFIodGFyZ2V0KSkpOyBtYXBzZXQocmVxLFNUUigicXVlcnkiKSxwYXJzZV9xdWVyeShxdWVyeSkpOyBWYWx1ZSBoZHJzPU1BUDAoKTsgY2hhciogaGU9c3Ryc3RyKG5sKzIsIlxyXG5cclxuIik7IGNoYXIqIGxpbmU9bmwrMjsgd2hpbGUobGluZSYmaGUmJmxpbmU8aGUpeyBjaGFyKiBlb2w9c3Ryc3RyKGxpbmUsIlxyXG4iKTsgaWYoIWVvbHx8ZW9sPmhlKWJyZWFrOyAqZW9sPTA7IGNoYXIqIGNvbD1zdHJjaHIobGluZSwnOicpOyBpZihjb2wpeypjb2w9MDsgY2hhciogdmFsPWNvbCsxOyB3aGlsZSgqdmFsPT0nICcpdmFsKys7IG1hcHNldChoZHJzLFNUUihsaW5lKSxTVFIodmFsKSk7fSBsaW5lPWVvbCsyOyB9IG1hcHNldChyZXEsU1RSKCJoZWFkZXJzIiksaGRycyk7IG1hcHNldChyZXEsU1RSKCJib2R5IiksU1RSKGhlP2hlKzQ6IiIpKTsgcmV0dXJuIHJlcTsgfQpzdGF0aWMgVmFsdWUgQl90eXBlb2YoVmFsdWUgdil7IGlmKHYudD09VFMpcmV0dXJuIFNUUigidGV4dCIpOyBpZih2LnQ9PVROKXJldHVybiBTVFIoIm51bWJlciIpOyBpZih2LnQ9PVRCKXJldHVybiBTVFIoImJvb2wiKTsgaWYodi50PT1UTClyZXR1cm4gU1RSKCJsaXN0Iik7IGlmKHYudD09VE0pcmV0dXJuIFNUUigibWFwIik7IHJldHVybiBTVFIoIm5vdGhpbmciKTsgfQpzdGF0aWMgVmFsdWUgQl90Y3BfbGlzdGVuKFZhbHVlIHB2KXsgaW50IHNydj1zb2NrZXQoQUZfSU5FVCxTT0NLX1NUUkVBTSwwKTsgaW50IG9wdD0xOyBzZXRzb2Nrb3B0KHNydixTT0xfU09DS0VULFNPX1JFVVNFQUREUiwmb3B0LHNpemVvZiBvcHQpOyBzdHJ1Y3Qgc29ja2FkZHJfaW4gYTsgbWVtc2V0KCZhLDAsc2l6ZW9mIGEpOyBhLnNpbl9mYW1pbHk9QUZfSU5FVDsgYS5zaW5fYWRkci5zX2FkZHI9SU5BRERSX0FOWTsgYS5zaW5fcG9ydD1odG9ucygobG9uZylwdi5uKTsgaWYoYmluZChzcnYsKHN0cnVjdCBzb2NrYWRkciopJmEsc2l6ZW9mIGEpPDApIHJldHVybiBOVU0oLTEpOyBsaXN0ZW4oc3J2LDY0KTsgcmV0dXJuIE5VTShzcnYpOyB9CnN0YXRpYyBWYWx1ZSBCX2FjY2VwdF9yZXEoVmFsdWUgc3YpeyBpbnQgYz1hY2NlcHQoKGludClzdi5uLDAsMCk7IGlmKGM8MCkgcmV0dXJuIE5JTCgpOyBsb25nIGJsOyBjaGFyKiByYXc9cmVjdl9yZXF1ZXN0KGMsJmJsKTsgVmFsdWUgcmVxPShyYXcmJmJsPjApP3BhcnNlX3JlcXVlc3QocmF3KTpOSUwoKTsgaWYocmVxLnQ9PVRNKSBtYXBzZXQocmVxLFNUUigiX2Nvbm4iKSxOVU0oYykpOyBpZihyYXcpIGZyZWUocmF3KTsgcmV0dXJuIHJlcTsgfQpzdGF0aWMgVmFsdWUgQl9yZXNwb25kKFZhbHVlIHJlcSwgVmFsdWUgcmVzcCl7IFZhbHVlIGN2PUlOREVYKHJlcSxTVFIoIl9jb25uIikpOyBpbnQgYz0oY3YudD09VE4pPyhpbnQpY3YubjotMTsgaWYoYzwwKSByZXR1cm4gTklMKCk7IGxvbmcgc3RhdHVzPTIwMDsgY2hhciogYm9keT0iIjsgY2hhciogY3R5cGU9InRleHQvaHRtbDsgY2hhcnNldD11dGYtOCI7IFZhbHVlIHhoPU5JTCgpOyBpZihyZXNwLnQ9PVRTKXsgYm9keT1yZXNwLnM7IH0gZWxzZSBpZihyZXNwLnQ9PVRNKXsgVmFsdWUgc3Q9SU5ERVgocmVzcCxTVFIoInN0YXR1cyIpKTsgaWYoc3QudD09VE4pc3RhdHVzPShsb25nKXN0Lm47IFZhbHVlIGJkPUlOREVYKHJlc3AsU1RSKCJib2R5IikpOyBpZihiZC50PT1UTXx8YmQudD09VEwpeyBib2R5PXRvc3RyKEJfdG9fanNvbihiZCkpOyBjdHlwZT0iYXBwbGljYXRpb24vanNvbiI7IH0gZWxzZSBpZihiZC50IT1UWCkgYm9keT10b3N0cihiZCk7IFZhbHVlIHR5PUlOREVYKHJlc3AsU1RSKCJ0eXBlIikpOyBpZih0eS50PT1UUyljdHlwZT10eS5zOyB4aD1JTkRFWChyZXNwLFNUUigiaGVhZGVycyIpKTsgfSBjaGFyIGhlYWRbNDA5Nl07IGxvbmcgYmwyPXN0cmxlbihib2R5KTsgaW50IGhuPXNucHJpbnRmKGhlYWQsc2l6ZW9mIGhlYWQsIkhUVFAvMS4xICVsZCBPS1xyXG5Db250ZW50LVR5cGU6ICVzXHJcbkNvbnRlbnQtTGVuZ3RoOiAlbGRcclxuQ29ubmVjdGlvbjogY2xvc2VcclxuIixzdGF0dXMsY3R5cGUsYmwyKTsgaWYoeGgudD09VE0peyBmb3IobG9uZyBpPTA7aTx4aC5tLT5sZW47aSsrKSBobis9c25wcmludGYoaGVhZCtobixzaXplb2YgaGVhZC1obiwiJXM6ICVzXHJcbiIseGgubS0+a2V5c1tpXSx0b3N0cih4aC5tLT52YWxzW2ldKSk7IH0gaG4rPXNucHJpbnRmKGhlYWQraG4sc2l6ZW9mIGhlYWQtaG4sIlxyXG4iKTsgd3JpdGUoYyxoZWFkLGhuKTsgd3JpdGUoYyxib2R5LGJsMik7IGNsb3NlKGMpOyByZXR1cm4gTklMKCk7IH0Kc3RhdGljIFZhbHVlIEJfYXNzZXJ0KFZhbHVlIGMsIFZhbHVlIG1zZyl7IGlmKCF0cnV0aHkoYykpeyBmcHJpbnRmKHN0ZGVyciwiYXNzZXJ0aW9uIGZhaWxlZDogJXNcbiIsIHRvc3RyKG1zZykpOyBleGl0KDEpO30gcmV0dXJuIE5JTCgpOyB9CnN0YXRpYyBWYWx1ZSBCX2lzX251bWJlcihWYWx1ZSB2KXsgcmV0dXJuIEJPT0xWKHYudD09VE4pOyB9CnN0YXRpYyBWYWx1ZSBCX2lzX3RleHQoVmFsdWUgdil7IHJldHVybiBCT09MVih2LnQ9PVRTKTsgfQpzdGF0aWMgVmFsdWUgQl9pc19saXN0KFZhbHVlIHYpeyByZXR1cm4gQk9PTFYodi50PT1UTCk7IH0Kc3RhdGljIFZhbHVlIEJfaXNfbWFwKFZhbHVlIHYpeyByZXR1cm4gQk9PTFYodi50PT1UTSk7IH0Kc3RhdGljIFZhbHVlIEJfaXNfbm90aGluZyhWYWx1ZSB2KXsgcmV0dXJuIEJPT0xWKHYudD09VFgpOyB9CnN0YXRpYyBWYWx1ZSBCX2lzX2Jvb2woVmFsdWUgdil7IHJldHVybiBCT09MVih2LnQ9PVRCKTsgfQpzdGF0aWMgVmFsdWUgQl9iYW5kKFZhbHVlIGEsIFZhbHVlIGIpeyByZXR1cm4gTlVNKChkb3VibGUpKChsb25nKWEubiAmIChsb25nKWIubikpOyB9CnN0YXRpYyBWYWx1ZSBCX2JvcihWYWx1ZSBhLCBWYWx1ZSBiKXsgcmV0dXJuIE5VTSgoZG91YmxlKSgobG9uZylhLm4gfCAobG9uZyliLm4pKTsgfQpzdGF0aWMgVmFsdWUgQl9ieG9yKFZhbHVlIGEsIFZhbHVlIGIpeyByZXR1cm4gTlVNKChkb3VibGUpKChsb25nKWEubiBeIChsb25nKWIubikpOyB9CnN0YXRpYyBWYWx1ZSBCX2Jub3QoVmFsdWUgYSwgVmFsdWUgdyl7IGxvbmcgYml0cz0obG9uZyl3Lm47IGxvbmcgbWFzaz0oYml0cz49NjMpPy0xTDooKDFMPDxiaXRzKS0xKTsgcmV0dXJuIE5VTSgoZG91YmxlKSgofihsb25nKWEubikgJiBtYXNrKSk7IH0Kc3RhdGljIFZhbHVlIEJfc2hsKFZhbHVlIGEsIFZhbHVlIGIpeyByZXR1cm4gTlVNKChkb3VibGUpKChsb25nKWEubiA8PCAobG9uZyliLm4pKTsgfQpzdGF0aWMgVmFsdWUgQl9zaHIoVmFsdWUgYSwgVmFsdWUgYil7IHJldHVybiBOVU0oKGRvdWJsZSkoKGxvbmcpYS5uID4+IChsb25nKWIubikpOyB9CnN0YXRpYyBWYWx1ZSBCX21pbmwoVmFsdWUgdil7IGlmKHYudCE9VEx8fHYubC0+bGVuPT0wKSByZXR1cm4gTklMKCk7IFZhbHVlIG09di5sLT5pdGVtc1swXTsgZm9yKGxvbmcgaT0xO2k8di5sLT5sZW47aSsrKSBpZih2LmwtPml0ZW1zW2ldLm48bS5uKSBtPXYubC0+aXRlbXNbaV07IHJldHVybiBtOyB9CnN0YXRpYyBWYWx1ZSBCX21heGwoVmFsdWUgdil7IGlmKHYudCE9VEx8fHYubC0+bGVuPT0wKSByZXR1cm4gTklMKCk7IFZhbHVlIG09di5sLT5pdGVtc1swXTsgZm9yKGxvbmcgaT0xO2k8di5sLT5sZW47aSsrKSBpZih2LmwtPml0ZW1zW2ldLm4+bS5uKSBtPXYubC0+aXRlbXNbaV07IHJldHVybiBtOyB9CnN0YXRpYyBWYWx1ZSBCX3N1bWwoVmFsdWUgdil7IGRvdWJsZSBzPTA7IGlmKHYudD09VEwpIGZvcihsb25nIGk9MDtpPHYubC0+bGVuO2krKykgcys9di5sLT5pdGVtc1tpXS5uOyByZXR1cm4gTlVNKHMpOyB9CnN0YXRpYyBWYWx1ZSBCX3Byb2R1Y3RsKFZhbHVlIHYpeyBkb3VibGUgcz0xOyBpZih2LnQ9PVRMKSBmb3IobG9uZyBpPTA7aTx2LmwtPmxlbjtpKyspIHMqPXYubC0+aXRlbXNbaV0ubjsgcmV0dXJuIE5VTShzKTsgfQpzdGF0aWMgVmFsdWUgQl9wdXNoKFZhbHVlIGxzdCwgVmFsdWUgeCl7IGlmKGxzdC50PT1UTCkgbGlzdHB1c2gobHN0LHgpOyByZXR1cm4gbHN0OyB9CnN0YXRpYyBWYWx1ZSBCX3BvcChWYWx1ZSBsc3QpeyBpZihsc3QudD09VEwgJiYgbHN0LmwtPmxlbj4wKSByZXR1cm4gbHN0LmwtPml0ZW1zWy0tbHN0LmwtPmxlbl07IHJldHVybiBOSUwoKTsgfQpzdGF0aWMgVmFsdWUgQl9yZW1vdmVfYXQoVmFsdWUgbHN0LCBWYWx1ZSBpdil7IGlmKGxzdC50PT1UTCl7IGxvbmcgaT0obG9uZylpdi5uOyBpZihpPj0wJiZpPGxzdC5sLT5sZW4peyBmb3IobG9uZyBqPWk7ajxsc3QubC0+bGVuLTE7aisrKSBsc3QubC0+aXRlbXNbal09bHN0LmwtPml0ZW1zW2orMV07IGxzdC5sLT5sZW4tLTsgfSB9IHJldHVybiBsc3Q7IH0Kc3RhdGljIFZhbHVlIEJfc3FydChWYWx1ZSB2KXsgZG91YmxlIHg9di5uOyBpZih4PD0wKSByZXR1cm4gTlVNKDApOyBkb3VibGUgZz14PjE/eDoxLjA7IGZvcihpbnQgaT0wO2k8NjA7aSsrKSBnPShnK3gvZykvMjsgcmV0dXJuIE5VTShnKTsgfQpzdGF0aWMgVmFsdWUgQl9wb3dlcihWYWx1ZSBhLCBWYWx1ZSBiKXsgZG91YmxlIGJhc2U9YS5uOyBsb25nIGU9KGxvbmcpYi5uOyBkb3VibGUgcj0xOyBsb25nIG49ZTwwPy1lOmU7IGZvcihsb25nIGk9MDtpPG47aSsrKSByKj1iYXNlOyByZXR1cm4gTlVNKGU8MD8xLjAvcjpyKTsgfQpzdGF0aWMgVmFsdWUgdmNfc2VydmUobG9uZyBwb3J0LCBWYWx1ZSgqaGFuZGxlcikoVmFsdWUpKXsgaW50IHNydj1zb2NrZXQoQUZfSU5FVCxTT0NLX1NUUkVBTSwwKTsgaW50IG9wdD0xOyBzZXRzb2Nrb3B0KHNydixTT0xfU09DS0VULFNPX1JFVVNFQUREUiwmb3B0LHNpemVvZiBvcHQpOyBzdHJ1Y3Qgc29ja2FkZHJfaW4gYTsgbWVtc2V0KCZhLDAsc2l6ZW9mIGEpOyBhLnNpbl9mYW1pbHk9QUZfSU5FVDsgYS5zaW5fYWRkci5zX2FkZHI9SU5BRERSX0FOWTsgYS5zaW5fcG9ydD1odG9ucyhwb3J0KTsgaWYoYmluZChzcnYsKHN0cnVjdCBzb2NrYWRkciopJmEsc2l6ZW9mIGEpPDApe3BlcnJvcigiYmluZCIpO3JldHVybiBOSUwoKTt9IGxpc3RlbihzcnYsNjQpOyBwcmludGYoIlZhbnRhIG5hdGl2ZSBzZXJ2ZXIgb24gaHR0cDovL2xvY2FsaG9zdDolbGRcbiIscG9ydCk7IGZmbHVzaChzdGRvdXQpOyBnX2luX3JlcT0xOwogIGZvcig7Oyl7IGViYigpOyBpbnQgYz1hY2NlcHQoc3J2LDAsMCk7IGlmKGM8MCljb250aW51ZTsgbG9uZyBibGVuOyBjaGFyKiByYXc9cmVjdl9yZXF1ZXN0KGMsJmJsZW4pOyBpZihyYXcmJmJsZW4+MCl7IFZhbHVlIHJlcT1wYXJzZV9yZXF1ZXN0KHJhdyk7IFZhbHVlIHJlc3A9aGFuZGxlcihyZXEpOyBsb25nIHN0YXR1cz0yMDA7IGNoYXIqIGJvZHk9IiI7IGNoYXIqIGN0eXBlPSJ0ZXh0L2h0bWw7IGNoYXJzZXQ9dXRmLTgiOyBWYWx1ZSB4aD1OSUwoKTsKICAgICAgICBpZihyZXNwLnQ9PVRTKXsgYm9keT1yZXNwLnM7IH0gZWxzZSBpZihyZXNwLnQ9PVRNKXsgVmFsdWUgc3Q9SU5ERVgocmVzcCxTVFIoInN0YXR1cyIpKTsgaWYoc3QudD09VE4pc3RhdHVzPShsb25nKXN0Lm47IFZhbHVlIGJkPUlOREVYKHJlc3AsU1RSKCJib2R5IikpOyBpZihiZC50PT1UTXx8YmQudD09VEwpeyBib2R5PXRvc3RyKEJfdG9fanNvbihiZCkpOyBjdHlwZT0iYXBwbGljYXRpb24vanNvbiI7IH0gZWxzZSBpZihiZC50IT1UWCkgYm9keT10b3N0cihiZCk7IFZhbHVlIHR5PUlOREVYKHJlc3AsU1RSKCJ0eXBlIikpOyBpZih0eS50PT1UUyljdHlwZT10eS5zOyB4aD1JTkRFWChyZXNwLFNUUigiaGVhZGVycyIpKTsgfQogICAgICAgIGNoYXIgaGVhZFs0MDk2XTsgbG9uZyBibD1zdHJsZW4oYm9keSk7IGludCBobj1zbnByaW50ZihoZWFkLHNpemVvZiBoZWFkLCJIVFRQLzEuMSAlbGQgT0tcclxuQ29udGVudC1UeXBlOiAlc1xyXG5Db250ZW50LUxlbmd0aDogJWxkXHJcbkNvbm5lY3Rpb246IGNsb3NlXHJcbiIsc3RhdHVzLGN0eXBlLGJsKTsgaWYoeGgudD09VE0peyBmb3IobG9uZyBpPTA7aTx4aC5tLT5sZW47aSsrKSBobis9c25wcmludGYoaGVhZCtobixzaXplb2YgaGVhZC1obiwiJXM6ICVzXHJcbiIseGgubS0+a2V5c1tpXSx0b3N0cih4aC5tLT52YWxzW2ldKSk7IH0gaG4rPXNucHJpbnRmKGhlYWQraG4sc2l6ZW9mIGhlYWQtaG4sIlxyXG4iKTsgd3JpdGUoYyxoZWFkLGhuKTsgd3JpdGUoYyxib2R5LGJsKTsgZnJlZShyYXcpOyB9IGNsb3NlKGMpOyB9CiAgcmV0dXJuIE5JTCgpOyB9CgovKiAtLS0tIGV4Y2VwdGlvbnMgKGF0dGVtcHQvcmVzY3VlIHZpYSBzZXRqbXApICsgaHR0cF9wb3N0ICsgcnVuX3ZhbnRhIC0tLS0gKi8KI2luY2x1ZGUgPHNldGptcC5oPgpzdGF0aWMgam1wX2J1ZiBnX2ptcFsxMjhdOyBzdGF0aWMgaW50IGdfam1wc3A9MDsgc3RhdGljIFZhbHVlIGdfZXJyOwpzdGF0aWMgVmFsdWUgQl9mYWlsKFZhbHVlIG1zZyl7IGdfZXJyPW1zZzsgaWYoZ19qbXBzcD4wKSBsb25nam1wKGdfam1wW2dfam1wc3AtMV0sMSk7IGZwcmludGYoc3RkZXJyLCJmYWlsOiAlc1xuIix0b3N0cihtc2cpKTsgZXhpdCgxKTsgfQpzdGF0aWMgVmFsdWUgQl9odHRwX3Bvc3QoVmFsdWUgdXJsLCBWYWx1ZSBib2R5LCBWYWx1ZSBoZWFkZXJzKXsKICBjaGFyKiBib2R5c3RyID0gKGJvZHkudD09VE18fGJvZHkudD09VEwpP3Rvc3RyKEJfdG9fanNvbihib2R5KSk6dG9zdHIoYm9keSk7CiAgY2hhciB0bXBmW109Ii90bXAvdmNwb3N0WFhYWFhYIjsgaW50IGZkPW1rc3RlbXAodG1wZik7IGlmKGZkPj0wKXsgd3JpdGUoZmQsYm9keXN0cixzdHJsZW4oYm9keXN0cikpOyBjbG9zZShmZCk7fSAKICBjaGFyIGNtZFszMjc2OF07IGludCBuPXNucHJpbnRmKGNtZCxzaXplb2YgY21kLCJjdXJsIC1zIC1YIFBPU1QgJyVzJyIsdG9zdHIodXJsKSk7CiAgaWYoaGVhZGVycy50PT1UTSl7IGZvcihsb25nIGk9MDtpPGhlYWRlcnMubS0+bGVuO2krKykgbis9c25wcmludGYoY21kK24sc2l6ZW9mIGNtZC1uLCIgLUggJyVzOiAlcyciLGhlYWRlcnMubS0+a2V5c1tpXSx0b3N0cihoZWFkZXJzLm0tPnZhbHNbaV0pKTsgfQogIG4rPXNucHJpbnRmKGNtZCtuLHNpemVvZiBjbWQtbiwiIC0tZGF0YS1iaW5hcnkgQCVzIix0bXBmKTsKICBWYWx1ZSBvdXQ9Ql9ydW4oU1RSKGNtZCkpOyB1bmxpbmsodG1wZik7CiAgcmV0dXJuIE1LTUFQKDIsIFNUUigic3RhdHVzIiksTlVNKDIwMCksIFNUUigiYm9keSIpLG91dCk7Cn0Kc3RhdGljIFZhbHVlIEJfcnVuX3ZhbnRhKFZhbHVlIGNvZGUpewogIGNoYXIgdG1wZltdPSIvdG1wL3ZjcnVuWFhYWFhYLnZhIjsgaW50IGZkPW1rc3RlbXBzKHRtcGYsMyk7IGNoYXIqIGM9dG9zdHIoY29kZSk7IGlmKGZkPj0wKXsgd3JpdGUoZmQsYyxzdHJsZW4oYykpOyBjbG9zZShmZCk7fSAKICBjaGFyIGNtZFsyNTZdOyBzbnByaW50ZihjbWQsc2l6ZW9mIGNtZCwidnNlbGYgJyVzJyIsdG1wZik7CiAgVmFsdWUgcj1CX3NoZWxsKFNUUihjbWQpKTsgdW5saW5rKHRtcGYpOwogIGludCBvaz0oKGxvbmcpSU5ERVgocixTVFIoImNvZGUiKSkubik9PTA7CiAgcmV0dXJuIE1LTUFQKDMsIFNUUigib2siKSxCT09MVihvayksIFNUUigib3V0cHV0IiksSU5ERVgocixTVFIoIm91dHB1dCIpKSwgU1RSKCJlcnJvciIpLCBvaz9TVFIoIiIpOklOREVYKHIsU1RSKCJvdXRwdXQiKSkpOwp9CgpzdGF0aWMgVmFsdWUgQl9zdGFydHNfd2l0aChWYWx1ZSBzLCBWYWx1ZSBwKXsgY2hhciogYT10b3N0cihzKTsgY2hhciogYj10b3N0cihwKTsgcmV0dXJuIEJPT0xWKHN0cm5jbXAoYSxiLHN0cmxlbihiKSk9PTApOyB9CnN0YXRpYyBWYWx1ZSBCX2VuZHNfd2l0aChWYWx1ZSBzLCBWYWx1ZSBwKXsgY2hhciogYT10b3N0cihzKTsgY2hhciogYj10b3N0cihwKTsgbG9uZyBsYT1zdHJsZW4oYSksbGI9c3RybGVuKGIpOyByZXR1cm4gQk9PTFYobGE+PWxiJiZzdHJjbXAoYStsYS1sYixiKT09MCk7IH0Kc3RhdGljIFZhbHVlIEJfZmluZChWYWx1ZSBzLCBWYWx1ZSBzdWIpeyBjaGFyKiBhPXRvc3RyKHMpOyBjaGFyKiBxPXN0cnN0cihhLHRvc3RyKHN1YikpOyByZXR1cm4gTlVNKHE/KHEtYSk6LTEpOyB9CnN0YXRpYyBWYWx1ZSBCX29zX25hbWUodm9pZCl7CiNpZmRlZiBfX0FQUExFX18KICByZXR1cm4gU1RSKCJtYWMiKTsKI2VsaWYgZGVmaW5lZChfV0lOMzIpCiAgcmV0dXJuIFNUUigid2luZG93cyIpOwojZWxzZQogIHJldHVybiBTVFIoImxpbnV4Iik7CiNlbmRpZgp9CnN0YXRpYyBWYWx1ZSBCX29wZW5fdXJsKFZhbHVlIHVybCl7IGNoYXIgY21kWzgxOTJdOwojaWZkZWYgX19BUFBMRV9fCiAgc25wcmludGYoY21kLHNpemVvZiBjbWQsIm9wZW4gJyVzJyA+L2Rldi9udWxsIDI+JjEiLHRvc3RyKHVybCkpOwojZWxzZQogIHNucHJpbnRmKGNtZCxzaXplb2YgY21kLCJ4ZGctb3BlbiAnJXMnID4vZGV2L251bGwgMj4mMSIsdG9zdHIodXJsKSk7CiNlbmRpZgogIHN5c3RlbShjbWQpOyByZXR1cm4gTklMKCk7IH0KCnN0YXRpYyBWYWx1ZSBCX3JldmVyc2UoVmFsdWUgdil7IGlmKHYudD09VFMpeyBjaGFyKiBzPXRvc3RyKHYpOyBsb25nIG49c3RybGVuKHMpOyBjaGFyKiByPW1hbGxvYyhuKzEpOyBmb3IobG9uZyBpPTA7aTxuO2krKykgcltpXT1zW24tMS1pXTsgcltuXT0wOyBWYWx1ZSB4PVNUUihyKTsgZnJlZShyKTsgcmV0dXJuIHg7IH0gaWYodi50PT1UTCl7IFZhbHVlIG89TElTVDAoKTsgZm9yKGxvbmcgaT12LmwtPmxlbi0xO2k+PTA7aS0tKSBsaXN0cHVzaChvLHYubC0+aXRlbXNbaV0pOyByZXR1cm4gbzsgfSByZXR1cm4gdjsgfQpzdGF0aWMgVmFsdWUgQl9maXJzdChWYWx1ZSB2KXsgaWYodi50PT1UTCYmdi5sLT5sZW4+MCkgcmV0dXJuIHYubC0+aXRlbXNbMF07IGlmKHYudD09VFMmJnYuc1swXSl7IGNoYXIgYlsyXT17di5zWzBdLDB9OyByZXR1cm4gU1RSKGIpO30gcmV0dXJuIE5JTCgpOyB9CnN0YXRpYyBWYWx1ZSBCX2xhc3QoVmFsdWUgdil7IGlmKHYudD09VEwmJnYubC0+bGVuPjApIHJldHVybiB2LmwtPml0ZW1zW3YubC0+bGVuLTFdOyBpZih2LnQ9PVRTKXsgbG9uZyBuPXN0cmxlbih2LnMpOyBpZihuPjApe2NoYXIgYlsyXT17di5zW24tMV0sMH07IHJldHVybiBTVFIoYik7fSB9IHJldHVybiBOSUwoKTsgfQpzdGF0aWMgVmFsdWUgQl9mbG9vcihWYWx1ZSB2KXsgbG9uZyB0PShsb25nKXYubjsgcmV0dXJuIE5VTSgoZG91YmxlKSh0LSgodi5uPDAmJnYubiE9dCk/MTowKSkpOyB9CnN0YXRpYyBWYWx1ZSBCX2NlaWwoVmFsdWUgdil7IGxvbmcgdD0obG9uZyl2Lm47IHJldHVybiBOVU0oKGRvdWJsZSkodCsoKHYubj4wJiZ2Lm4hPXQpPzE6MCkpKTsgfQpzdGF0aWMgVmFsdWUgQl9yb3VuZChWYWx1ZSB2KXsgcmV0dXJuIE5VTSgoZG91YmxlKShsb25nKSh2Lm4rKHYubj49MD8wLjU6LTAuNSkpKTsgfQpzdGF0aWMgVmFsdWUgQl9hYnMoVmFsdWUgdil7IHJldHVybiBOVU0odi5uPDA/LXYubjp2Lm4pOyB9Cg==");
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
