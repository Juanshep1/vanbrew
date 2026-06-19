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
static Value B_bnot(Value a){ return NUM((double)(~(long)a.n)); }
static Value B_shl(Value a, Value b){ return NUM((double)((long)a.n << (long)b.n)); }
static Value B_shr(Value a, Value b){ return NUM((double)((long)a.n >> (long)b.n)); }
static Value B_minl(Value v){ if(v.t!=TL||v.l->len==0) return NIL(); Value m=v.l->items[0]; for(long i=1;i<v.l->len;i++) if(v.l->items[i].n<m.n) m=v.l->items[i]; return m; }
static Value B_maxl(Value v){ if(v.t!=TL||v.l->len==0) return NIL(); Value m=v.l->items[0]; for(long i=1;i<v.l->len;i++) if(v.l->items[i].n>m.n) m=v.l->items[i]; return m; }
static Value B_suml(Value v){ double s=0; if(v.t==TL) for(long i=0;i<v.l->len;i++) s+=v.l->items[i].n; return NUM(s); }
static Value B_productl(Value v){ double s=1; if(v.t==TL) for(long i=0;i<v.l->len;i++) s*=v.l->items[i].n; return NUM(s); }
static Value B_push(Value lst, Value x){ if(lst.t==TL) listpush(lst,x); return lst; }
static Value B_pop(Value lst){ if(lst.t==TL && lst.l->len>0) return lst.l->items[--lst.l->len]; return NIL(); }
static Value B_remove_at(Value lst, Value iv){ if(lst.t==TL){ long i=(long)iv.n; if(i>=0&&i<lst.l->len){ for(long j=i;j<lst.l->len-1;j++) lst.l->items[j]=lst.l->items[j+1]; lst.l->len--; } } return lst; }
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

Value v_DEMO;
Value v_args;
Value v_SUBARGS;

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
Value v_new_env(Value);
Value v_env_get(Value, Value);
Value v_env_set(Value, Value, Value);
Value v_env_def(Value, Value, Value);
Value v_truthy(Value);
Value v_eval_expr(Value, Value);
Value v_call_builtin(Value, Value);
Value v_apply_fn(Value, Value);
Value v_eval_call(Value, Value);
Value v_exec_block(Value, Value);
Value v_exec_stmt(Value, Value);
Value v_vrun(Value);

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
        if (truthy(ORV(EQ(v_first, STR("end")), EQ(v_first, STR("otherwise"))))) {
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

Value v_new_env(Value v_parent) {
    return MKMAP(2, STR("vars"), MKMAP(0), STR("parent"), v_parent);
    return NIL();
}

Value v_env_get(Value v_env, Value v_name) {
    Value v_e = v_env;
    while (truthy(NE(v_e, NIL()))) {
        if (truthy(INOP(v_name, INDEX(v_e, STR("vars"))))) {
            return INDEX(INDEX(v_e, STR("vars")), v_name);
        }
        v_e = INDEX(v_e, STR("parent"));
    }
    B_fail(ADD(STR("unknown name: "), v_name));
    return NIL();
}

Value v_env_set(Value v_env, Value v_name, Value v_val) {
    Value v_e = v_env;
    while (truthy(NE(v_e, NIL()))) {
        if (truthy(INOP(v_name, INDEX(v_e, STR("vars"))))) {
            Value v_vs = INDEX(v_e, STR("vars"));
            Value v_k = v_name;
            SETAT(v_vs, v_k, v_val);
            return NIL();
        }
        v_e = INDEX(v_e, STR("parent"));
    }
    B_fail(ADD(STR("cannot change unknown name: "), v_name));
    return NIL();
}

Value v_env_def(Value v_env, Value v_name, Value v_val) {
    Value v_vs = INDEX(v_env, STR("vars"));
    Value v_k = v_name;
    SETAT(v_vs, v_k, v_val);
    return NIL();
}

Value v_truthy(Value v_v) {
    if (truthy(ORV(EQ(v_v, BOOLV(0)), EQ(v_v, NIL())))) {
        return BOOLV(0);
    }
    return BOOLV(1);
    return NIL();
}

Value v_eval_expr(Value v_node, Value v_env) {
    Value v_k = INDEX(v_node, STR("k"));
    if (truthy(ORV(ORV(EQ(v_k, STR("num")), EQ(v_k, STR("str"))), EQ(v_k, STR("lit"))))) {
        return INDEX(v_node, STR("v"));
    }
    if (truthy(EQ(v_k, STR("var")))) {
        return v_env_get(v_env, INDEX(v_node, STR("name")));
    }
    if (truthy(EQ(v_k, STR("list")))) {
        Value v_l = MKLIST(0);
        { Value _s1 = INDEX(v_node, STR("items")); long _n1 = (long)LEN(_s1).n;
        for (long _i1 = 0; _i1 < _n1; _i1++) {
            Value v_it = INDEX(_s1, NUM(_i1));
            listpush(v_l, v_eval_expr(v_it, v_env));
        } }
        return v_l;
    }
    if (truthy(EQ(v_k, STR("map")))) {
        Value v_m = MKMAP(0);
        { Value _s2 = INDEX(v_node, STR("pairs")); long _n2 = (long)LEN(_s2).n;
        for (long _i2 = 0; _i2 < _n2; _i2++) {
            Value v_pr = INDEX(_s2, NUM(_i2));
            Value v_key = v_eval_expr(INDEX(v_pr, STR("kn")), v_env);
            Value v_val = v_eval_expr(INDEX(v_pr, STR("vn")), v_env);
            Value v_kk = v_key;
            SETAT(v_m, v_kk, v_val);
        } }
        return v_m;
    }
    if (truthy(EQ(v_k, STR("index")))) {
        Value v_o = v_eval_expr(INDEX(v_node, STR("o")), v_env);
        Value v_ix = v_eval_expr(INDEX(v_node, STR("idx")), v_env);
        return INDEX(v_o, v_ix);
    }
    if (truthy(EQ(v_k, STR("slice")))) {
        Value v_o = v_eval_expr(INDEX(v_node, STR("o")), v_env);
        return B_slice(v_o, v_eval_expr(INDEX(v_node, STR("a")), v_env), v_eval_expr(INDEX(v_node, STR("b")), v_env));
    }
    if (truthy(EQ(v_k, STR("neg")))) {
        return SUB(NUM(0), v_eval_expr(INDEX(v_node, STR("a")), v_env));
    }
    if (truthy(EQ(v_k, STR("not")))) {
        return NOTV(v_truthy(v_eval_expr(INDEX(v_node, STR("a")), v_env)));
    }
    if (truthy(EQ(v_k, STR("and")))) {
        if (truthy(NOTV(v_truthy(v_eval_expr(INDEX(v_node, STR("a")), v_env))))) {
            return BOOLV(0);
        }
        return v_truthy(v_eval_expr(INDEX(v_node, STR("b")), v_env));
    }
    if (truthy(EQ(v_k, STR("or")))) {
        if (truthy(v_truthy(v_eval_expr(INDEX(v_node, STR("a")), v_env)))) {
            return BOOLV(1);
        }
        return v_truthy(v_eval_expr(INDEX(v_node, STR("b")), v_env));
    }
    if (truthy(EQ(v_k, STR("bin")))) {
        Value v_a = v_eval_expr(INDEX(v_node, STR("a")), v_env);
        Value v_b = v_eval_expr(INDEX(v_node, STR("b")), v_env);
        Value v_op = INDEX(v_node, STR("op"));
        if (truthy(EQ(v_op, STR("+")))) {
            return ADD(v_a, v_b);
        }
        if (truthy(EQ(v_op, STR("-")))) {
            return SUB(v_a, v_b);
        }
        if (truthy(EQ(v_op, STR("*")))) {
            return MUL(v_a, v_b);
        }
        return DIVV(v_a, v_b);
    }
    if (truthy(EQ(v_k, STR("cmp")))) {
        Value v_a = v_eval_expr(INDEX(v_node, STR("a")), v_env);
        Value v_b = v_eval_expr(INDEX(v_node, STR("b")), v_env);
        Value v_op = INDEX(v_node, STR("op"));
        if (truthy(EQ(v_op, STR("==")))) {
            return EQ(v_a, v_b);
        }
        if (truthy(EQ(v_op, STR("!=")))) {
            return NE(v_a, v_b);
        }
        if (truthy(EQ(v_op, STR(">")))) {
            return GT(v_a, v_b);
        }
        if (truthy(EQ(v_op, STR("<")))) {
            return LT(v_a, v_b);
        }
        if (truthy(EQ(v_op, STR("in")))) {
            return INOP(v_a, v_b);
        }
        if (truthy(EQ(v_op, STR(">=")))) {
            return GE(v_a, v_b);
        }
        return LE(v_a, v_b);
    }
    if (truthy(EQ(v_k, STR("call")))) {
        return v_eval_call(v_node, v_env);
    }
    B_fail(STR("cannot evaluate node"));
    return NIL();
}

Value v_call_builtin(Value v_name, Value v_args) {
    if (truthy(EQ(v_name, STR("text")))) {
        return MKMAP(2, STR("hit"), BOOLV(1), STR("v"), B_text(INDEX(v_args, NUM(0))));
    }
    if (truthy(EQ(v_name, STR("length")))) {
        return MKMAP(2, STR("hit"), BOOLV(1), STR("v"), B_length(INDEX(v_args, NUM(0))));
    }
    if (truthy(EQ(v_name, STR("slice")))) {
        return MKMAP(2, STR("hit"), BOOLV(1), STR("v"), B_slice(INDEX(v_args, NUM(0)), INDEX(v_args, NUM(1)), INDEX(v_args, NUM(2))));
    }
    if (truthy(EQ(v_name, STR("number")))) {
        return MKMAP(2, STR("hit"), BOOLV(1), STR("v"), B_number(INDEX(v_args, NUM(0))));
    }
    if (truthy(EQ(v_name, STR("split")))) {
        return MKMAP(2, STR("hit"), BOOLV(1), STR("v"), B_split(INDEX(v_args, NUM(0)), INDEX(v_args, NUM(1))));
    }
    if (truthy(EQ(v_name, STR("join")))) {
        return MKMAP(2, STR("hit"), BOOLV(1), STR("v"), B_join(INDEX(v_args, NUM(0)), INDEX(v_args, NUM(1))));
    }
    if (truthy(EQ(v_name, STR("keys")))) {
        return MKMAP(2, STR("hit"), BOOLV(1), STR("v"), B_keys(INDEX(v_args, NUM(0))));
    }
    if (truthy(EQ(v_name, STR("values")))) {
        return MKMAP(2, STR("hit"), BOOLV(1), STR("v"), B_values(INDEX(v_args, NUM(0))));
    }
    if (truthy(EQ(v_name, STR("uppercase")))) {
        return MKMAP(2, STR("hit"), BOOLV(1), STR("v"), B_upper(INDEX(v_args, NUM(0))));
    }
    if (truthy(EQ(v_name, STR("lowercase")))) {
        return MKMAP(2, STR("hit"), BOOLV(1), STR("v"), B_lower(INDEX(v_args, NUM(0))));
    }
    if (truthy(EQ(v_name, STR("trim")))) {
        return MKMAP(2, STR("hit"), BOOLV(1), STR("v"), B_trim(INDEX(v_args, NUM(0))));
    }
    if (truthy(EQ(v_name, STR("replace")))) {
        return MKMAP(2, STR("hit"), BOOLV(1), STR("v"), B_replace(INDEX(v_args, NUM(0)), INDEX(v_args, NUM(1)), INDEX(v_args, NUM(2))));
    }
    if (truthy(EQ(v_name, STR("contains")))) {
        return MKMAP(2, STR("hit"), BOOLV(1), STR("v"), B_contains(INDEX(v_args, NUM(0)), INDEX(v_args, NUM(1))));
    }
    if (truthy(EQ(v_name, STR("reverse")))) {
        return MKMAP(2, STR("hit"), BOOLV(1), STR("v"), B_reverse(INDEX(v_args, NUM(0))));
    }
    if (truthy(EQ(v_name, STR("sort")))) {
        return MKMAP(2, STR("hit"), BOOLV(1), STR("v"), B_sort(INDEX(v_args, NUM(0))));
    }
    if (truthy(EQ(v_name, STR("first")))) {
        return MKMAP(2, STR("hit"), BOOLV(1), STR("v"), B_first(INDEX(v_args, NUM(0))));
    }
    if (truthy(EQ(v_name, STR("last")))) {
        return MKMAP(2, STR("hit"), BOOLV(1), STR("v"), B_last(INDEX(v_args, NUM(0))));
    }
    if (truthy(EQ(v_name, STR("abs")))) {
        return MKMAP(2, STR("hit"), BOOLV(1), STR("v"), B_abs(INDEX(v_args, NUM(0))));
    }
    if (truthy(EQ(v_name, STR("floor")))) {
        return MKMAP(2, STR("hit"), BOOLV(1), STR("v"), B_floor(INDEX(v_args, NUM(0))));
    }
    if (truthy(EQ(v_name, STR("ceil")))) {
        return MKMAP(2, STR("hit"), BOOLV(1), STR("v"), B_ceil(INDEX(v_args, NUM(0))));
    }
    if (truthy(EQ(v_name, STR("round")))) {
        return MKMAP(2, STR("hit"), BOOLV(1), STR("v"), B_round(INDEX(v_args, NUM(0))));
    }
    if (truthy(EQ(v_name, STR("range")))) {
        if (truthy(EQ(B_length(v_args), NUM(1)))) {
            return MKMAP(2, STR("hit"), BOOLV(1), STR("v"), B_range(INDEX(v_args, NUM(0)), NUM(0), 0));
        }
        return MKMAP(2, STR("hit"), BOOLV(1), STR("v"), B_range(INDEX(v_args, NUM(0)), INDEX(v_args, NUM(1)), 1));
    }
    if (truthy(EQ(v_name, STR("read_file")))) {
        return MKMAP(2, STR("hit"), BOOLV(1), STR("v"), B_read_file(INDEX(v_args, NUM(0))));
    }
    if (truthy(EQ(v_name, STR("arguments")))) {
        return MKMAP(2, STR("hit"), BOOLV(1), STR("v"), v_SUBARGS);
    }
    if (truthy(EQ(v_name, STR("write_file")))) {
        return MKMAP(2, STR("hit"), BOOLV(1), STR("v"), B_write_file(INDEX(v_args, NUM(0)), INDEX(v_args, NUM(1))));
    }
    if (truthy(EQ(v_name, STR("append_file")))) {
        return MKMAP(2, STR("hit"), BOOLV(1), STR("v"), B_append_file(INDEX(v_args, NUM(0)), INDEX(v_args, NUM(1))));
    }
    if (truthy(EQ(v_name, STR("base64_decode")))) {
        return MKMAP(2, STR("hit"), BOOLV(1), STR("v"), B_b64decode(INDEX(v_args, NUM(0))));
    }
    if (truthy(EQ(v_name, STR("shell")))) {
        return MKMAP(2, STR("hit"), BOOLV(1), STR("v"), B_shell(INDEX(v_args, NUM(0))));
    }
    if (truthy(EQ(v_name, STR("run")))) {
        return MKMAP(2, STR("hit"), BOOLV(1), STR("v"), B_run(INDEX(v_args, NUM(0))));
    }
    if (truthy(EQ(v_name, STR("to_json")))) {
        return MKMAP(2, STR("hit"), BOOLV(1), STR("v"), B_to_json(INDEX(v_args, NUM(0))));
    }
    if (truthy(EQ(v_name, STR("from_json")))) {
        return MKMAP(2, STR("hit"), BOOLV(1), STR("v"), B_from_json(INDEX(v_args, NUM(0))));
    }
    if (truthy(EQ(v_name, STR("starts_with")))) {
        return MKMAP(2, STR("hit"), BOOLV(1), STR("v"), B_starts_with(INDEX(v_args, NUM(0)), INDEX(v_args, NUM(1))));
    }
    if (truthy(EQ(v_name, STR("ends_with")))) {
        return MKMAP(2, STR("hit"), BOOLV(1), STR("v"), B_ends_with(INDEX(v_args, NUM(0)), INDEX(v_args, NUM(1))));
    }
    if (truthy(EQ(v_name, STR("find")))) {
        return MKMAP(2, STR("hit"), BOOLV(1), STR("v"), B_find(INDEX(v_args, NUM(0)), INDEX(v_args, NUM(1))));
    }
    if (truthy(EQ(v_name, STR("path_exists")))) {
        return MKMAP(2, STR("hit"), BOOLV(1), STR("v"), B_path_exists(INDEX(v_args, NUM(0))));
    }
    if (truthy(EQ(v_name, STR("make_dir")))) {
        return MKMAP(2, STR("hit"), BOOLV(1), STR("v"), B_make_dir(INDEX(v_args, NUM(0))));
    }
    if (truthy(EQ(v_name, STR("list_dir")))) {
        return MKMAP(2, STR("hit"), BOOLV(1), STR("v"), B_list_dir(INDEX(v_args, NUM(0))));
    }
    if (truthy(EQ(v_name, STR("http_get")))) {
        if (truthy(GT(B_length(v_args), NUM(1)))) {
            return MKMAP(2, STR("hit"), BOOLV(1), STR("v"), B_http_get(INDEX(v_args, NUM(0)), INDEX(v_args, NUM(1))));
        }
        return MKMAP(2, STR("hit"), BOOLV(1), STR("v"), B_http_get(INDEX(v_args, NUM(0)), NIL()));
    }
    if (truthy(EQ(v_name, STR("typeof")))) {
        return MKMAP(2, STR("hit"), BOOLV(1), STR("v"), B_typeof(INDEX(v_args, NUM(0))));
    }
    if (truthy(EQ(v_name, STR("serve")))) {
        Value v_srv = B_tcp_listen(INDEX(v_args, NUM(0)));
        if (truthy(LT(v_srv, NUM(0)))) {
            SAY(ADD(STR("serve: could not bind port "), B_text(INDEX(v_args, NUM(0)))));
            return MKMAP(2, STR("hit"), BOOLV(1), STR("v"), NIL());
        }
        SAY(ADD(STR("Vanta server (interpreted, native) on http://localhost:"), B_text(INDEX(v_args, NUM(0)))));
        Value v_handler = INDEX(v_args, NUM(1));
        while (truthy(BOOLV(1))) {
            Value v_req = B_accept_req(v_srv);
            if (truthy(EQ(B_typeof(v_req), STR("map")))) {
                Value v_resp = v_apply_fn(v_handler, MKLIST(1, v_req));
                B_respond(v_req, v_resp);
            }
        }
        return MKMAP(2, STR("hit"), BOOLV(1), STR("v"), NIL());
    }
    if (truthy(EQ(v_name, STR("now")))) {
        return MKMAP(2, STR("hit"), BOOLV(1), STR("v"), B_now());
    }
    if (truthy(EQ(v_name, STR("today")))) {
        return MKMAP(2, STR("hit"), BOOLV(1), STR("v"), B_today());
    }
    if (truthy(EQ(v_name, STR("clock")))) {
        return MKMAP(2, STR("hit"), BOOLV(1), STR("v"), B_clock());
    }
    if (truthy(EQ(v_name, STR("type_of")))) {
        return MKMAP(2, STR("hit"), BOOLV(1), STR("v"), B_typeof(INDEX(v_args, NUM(0))));
    }
    if (truthy(EQ(v_name, STR("is_number")))) {
        return MKMAP(2, STR("hit"), BOOLV(1), STR("v"), B_is_number(INDEX(v_args, NUM(0))));
    }
    if (truthy(EQ(v_name, STR("is_text")))) {
        return MKMAP(2, STR("hit"), BOOLV(1), STR("v"), B_is_text(INDEX(v_args, NUM(0))));
    }
    if (truthy(EQ(v_name, STR("is_list")))) {
        return MKMAP(2, STR("hit"), BOOLV(1), STR("v"), B_is_list(INDEX(v_args, NUM(0))));
    }
    if (truthy(EQ(v_name, STR("is_map")))) {
        return MKMAP(2, STR("hit"), BOOLV(1), STR("v"), B_is_map(INDEX(v_args, NUM(0))));
    }
    if (truthy(EQ(v_name, STR("is_nothing")))) {
        return MKMAP(2, STR("hit"), BOOLV(1), STR("v"), B_is_nothing(INDEX(v_args, NUM(0))));
    }
    if (truthy(EQ(v_name, STR("band")))) {
        return MKMAP(2, STR("hit"), BOOLV(1), STR("v"), B_band(INDEX(v_args, NUM(0)), INDEX(v_args, NUM(1))));
    }
    if (truthy(EQ(v_name, STR("bor")))) {
        return MKMAP(2, STR("hit"), BOOLV(1), STR("v"), B_bor(INDEX(v_args, NUM(0)), INDEX(v_args, NUM(1))));
    }
    if (truthy(EQ(v_name, STR("bxor")))) {
        return MKMAP(2, STR("hit"), BOOLV(1), STR("v"), B_bxor(INDEX(v_args, NUM(0)), INDEX(v_args, NUM(1))));
    }
    if (truthy(EQ(v_name, STR("bnot")))) {
        return MKMAP(2, STR("hit"), BOOLV(1), STR("v"), B_bnot(INDEX(v_args, NUM(0))));
    }
    if (truthy(EQ(v_name, STR("shift_left")))) {
        return MKMAP(2, STR("hit"), BOOLV(1), STR("v"), B_shl(INDEX(v_args, NUM(0)), INDEX(v_args, NUM(1))));
    }
    if (truthy(EQ(v_name, STR("shift_right")))) {
        return MKMAP(2, STR("hit"), BOOLV(1), STR("v"), B_shr(INDEX(v_args, NUM(0)), INDEX(v_args, NUM(1))));
    }
    if (truthy(EQ(v_name, STR("min")))) {
        return MKMAP(2, STR("hit"), BOOLV(1), STR("v"), B_minl(INDEX(v_args, NUM(0))));
    }
    if (truthy(EQ(v_name, STR("max")))) {
        return MKMAP(2, STR("hit"), BOOLV(1), STR("v"), B_maxl(INDEX(v_args, NUM(0))));
    }
    if (truthy(EQ(v_name, STR("sum")))) {
        return MKMAP(2, STR("hit"), BOOLV(1), STR("v"), B_suml(INDEX(v_args, NUM(0))));
    }
    if (truthy(EQ(v_name, STR("product")))) {
        return MKMAP(2, STR("hit"), BOOLV(1), STR("v"), B_productl(INDEX(v_args, NUM(0))));
    }
    if (truthy(EQ(v_name, STR("push")))) {
        return MKMAP(2, STR("hit"), BOOLV(1), STR("v"), B_push(INDEX(v_args, NUM(0)), INDEX(v_args, NUM(1))));
    }
    if (truthy(EQ(v_name, STR("pop")))) {
        return MKMAP(2, STR("hit"), BOOLV(1), STR("v"), B_pop(INDEX(v_args, NUM(0))));
    }
    if (truthy(EQ(v_name, STR("remove_at")))) {
        return MKMAP(2, STR("hit"), BOOLV(1), STR("v"), B_remove_at(INDEX(v_args, NUM(0)), INDEX(v_args, NUM(1))));
    }
    if (truthy(EQ(v_name, STR("assert")))) {
        if (truthy(GT(B_length(v_args), NUM(1)))) {
            return MKMAP(2, STR("hit"), BOOLV(1), STR("v"), B_assert(INDEX(v_args, NUM(0)), INDEX(v_args, NUM(1))));
        }
        return MKMAP(2, STR("hit"), BOOLV(1), STR("v"), B_assert(INDEX(v_args, NUM(0)), STR("")));
    }
    if (truthy(EQ(v_name, STR("fail")))) {
        B_fail(INDEX(v_args, NUM(0)));
    }
    return MKMAP(2, STR("hit"), BOOLV(0), STR("v"), NIL());
    return NIL();
}

Value v_apply_fn(Value v_fn, Value v_args) {
    Value v_fenv = v_new_env(INDEX(v_fn, STR("env")));
    Value v_i = NUM(0);
    { Value _s3 = INDEX(v_fn, STR("params")); long _n3 = (long)LEN(_s3).n;
    for (long _i3 = 0; _i3 < _n3; _i3++) {
        Value v_p = INDEX(_s3, NUM(_i3));
        v_env_def(v_fenv, v_p, INDEX(v_args, v_i));
        v_i = ADD(v_i, NUM(1));
    } }
    Value v_r = v_exec_block(INDEX(v_fn, STR("body")), v_fenv);
    if (truthy(INOP(STR("ret"), v_r))) {
        return INDEX(v_r, STR("ret"));
    }
    return NIL();
    return NIL();
}

Value v_eval_call(Value v_node, Value v_env) {
    Value v_name = INDEX(v_node, STR("name"));
    Value v_args = MKLIST(0);
    { Value _s4 = INDEX(v_node, STR("args")); long _n4 = (long)LEN(_s4).n;
    for (long _i4 = 0; _i4 < _n4; _i4++) {
        Value v_an = INDEX(_s4, NUM(_i4));
        listpush(v_args, v_eval_expr(v_an, v_env));
    } }
    Value v_bi = v_call_builtin(v_name, v_args);
    if (truthy(INDEX(v_bi, STR("hit")))) {
        return INDEX(v_bi, STR("v"));
    }
    Value v_fn = v_env_get(v_env, v_name);
    return v_apply_fn(v_fn, v_args);
    return NIL();
}

Value v_exec_block(Value v_stmts, Value v_env) {
    { Value _s5 = v_stmts; long _n5 = (long)LEN(_s5).n;
    for (long _i5 = 0; _i5 < _n5; _i5++) {
        Value v_st = INDEX(_s5, NUM(_i5));
        if (truthy(EQ(INDEX(v_st, STR("k")), STR("func")))) {
            v_exec_stmt(v_st, v_env);
        }
    } }
    { Value _s6 = v_stmts; long _n6 = (long)LEN(_s6).n;
    for (long _i6 = 0; _i6 < _n6; _i6++) {
        Value v_st = INDEX(_s6, NUM(_i6));
        if (truthy(NE(INDEX(v_st, STR("k")), STR("func")))) {
            Value v_r = v_exec_stmt(v_st, v_env);
            if (truthy(INOP(STR("ret"), v_r))) {
                return v_r;
            }
        }
    } }
    return MKMAP(0);
    return NIL();
}

Value v_exec_stmt(Value v_st, Value v_env) {
    Value v_k = INDEX(v_st, STR("k"));
    if (truthy(EQ(v_k, STR("let")))) {
        v_env_def(v_env, INDEX(v_st, STR("name")), v_eval_expr(INDEX(v_st, STR("e")), v_env));
        return MKMAP(0);
    }
    if (truthy(EQ(v_k, STR("set")))) {
        v_env_set(v_env, INDEX(v_st, STR("name")), v_eval_expr(INDEX(v_st, STR("e")), v_env));
        return MKMAP(0);
    }
    if (truthy(EQ(v_k, STR("setat")))) {
        Value v_coll = v_env_get(v_env, INDEX(v_st, STR("name")));
        Value v_key = v_eval_expr(INDEX(v_st, STR("key")), v_env);
        Value v_v = v_eval_expr(INDEX(v_st, STR("e")), v_env);
        Value v_kk = v_key;
        SETAT(v_coll, v_kk, v_v);
        return MKMAP(0);
    }
    if (truthy(EQ(v_k, STR("add")))) {
        Value v_lst = v_eval_expr(INDEX(v_st, STR("target")), v_env);
        listpush(v_lst, v_eval_expr(INDEX(v_st, STR("val")), v_env));
        return MKMAP(0);
    }
    if (truthy(EQ(v_k, STR("for")))) {
        Value v_seq = v_eval_expr(INDEX(v_st, STR("list")), v_env);
        { Value _s7 = v_seq; long _n7 = (long)LEN(_s7).n;
        for (long _i7 = 0; _i7 < _n7; _i7++) {
            Value v_item = INDEX(_s7, NUM(_i7));
            v_env_def(v_env, INDEX(v_st, STR("var")), v_item);
            Value v_r = v_exec_block(INDEX(v_st, STR("body")), v_env);
            if (truthy(INOP(STR("ret"), v_r))) {
                return v_r;
            }
        } }
        return MKMAP(0);
    }
    if (truthy(EQ(v_k, STR("repeat")))) {
        Value v_reps = v_eval_expr(INDEX(v_st, STR("n")), v_env);
        Value v_c = NUM(0);
        while (truthy(LT(v_c, v_reps))) {
            Value v_r = v_exec_block(INDEX(v_st, STR("body")), v_env);
            if (truthy(INOP(STR("ret"), v_r))) {
                return v_r;
            }
            v_c = ADD(v_c, NUM(1));
        }
        return MKMAP(0);
    }
    if (truthy(EQ(v_k, STR("say")))) {
        SAY(B_text(v_eval_expr(INDEX(v_st, STR("e")), v_env)));
        return MKMAP(0);
    }
    if (truthy(EQ(v_k, STR("expr")))) {
        v_eval_expr(INDEX(v_st, STR("e")), v_env);
        return MKMAP(0);
    }
    if (truthy(EQ(v_k, STR("ret")))) {
        return MKMAP(1, STR("ret"), v_eval_expr(INDEX(v_st, STR("e")), v_env));
    }
    if (truthy(EQ(v_k, STR("if")))) {
        if (truthy(v_truthy(v_eval_expr(INDEX(v_st, STR("c")), v_env)))) {
            return v_exec_block(INDEX(v_st, STR("body")), v_env);
        }
        return v_exec_block(INDEX(v_st, STR("els")), v_env);
    }
    if (truthy(EQ(v_k, STR("while")))) {
        while (truthy(v_truthy(v_eval_expr(INDEX(v_st, STR("c")), v_env)))) {
            Value v_r = v_exec_block(INDEX(v_st, STR("body")), v_env);
            if (truthy(INOP(STR("ret"), v_r))) {
                return v_r;
            }
        }
        return MKMAP(0);
    }
    if (truthy(EQ(v_k, STR("func")))) {
        v_env_def(v_env, INDEX(v_st, STR("name")), MKMAP(3, STR("params"), INDEX(v_st, STR("params")), STR("body"), INDEX(v_st, STR("body")), STR("env"), v_env));
        return MKMAP(0);
    }
    return MKMAP(0);
    return NIL();
}

Value v_vrun(Value v_src) {
    Value v_prog = MKLIST(0);
    { Value _s8 = B_split(v_src, STR("\n")); long _n8 = (long)LEN(_s8).n;
    for (long _i8 = 0; _i8 < _n8; _i8++) {
        Value v_line = INDEX(_s8, NUM(_i8));
        Value v_toks = v_lex_line(v_line);
        if (truthy(GT(B_length(v_toks), NUM(0)))) {
            listpush(v_prog, v_toks);
        }
    } }
    Value v_block = v_parse_block(v_prog, NUM(0));
    Value v_genv = v_new_env(NIL());
    v_exec_block(INDEX(v_block, STR("stmts")), v_genv);
    return NIL();
}

int main(int argc, char** argv) {
    g_argc = argc; g_argv = argv;
    v_DEMO = STR("# a program run by Vanta-in-Vanta\nlet x be 5\nlet y be 3\nsay \"x + y = \" + text(x + y)\nif x is over y\n    say \"x is bigger\"\notherwise\n    say \"y is bigger\"\nend\nto fib(n)\n    if n is under 2\n        give back n\n    end\n    give back fib(n - 1) + fib(n - 2)\nend\nsay \"fib(10) = \" + text(fib(10))\nlet nums be [4, 1, 3, 1, 5, 9, 2, 6]\nlet total be 0\nfor each n in nums\n    change total to total + n\nend\nsay \"sum \" + text(nums) + \" = \" + text(total)\nsay \"sorted = \" + text(sort(nums))\nlet who be {\"name\": \"Ada\", \"lang\": \"Vanta\"}\nsay who[\"name\"] + \" writes \" + who[\"lang\"]\nlet shout be \"\"\nfor each w in [\"self\", \"hosting\", \"works\"]\n    change shout to shout + uppercase(w) + \" \"\nend\nsay trim(shout)\n");
    v_args = B_arguments();
    v_SUBARGS = MKLIST(0);
    if (truthy(GT(B_length(v_args), NUM(1)))) {
        v_SUBARGS = B_slice(v_args, NUM(1), B_length(v_args));
    }
    if (truthy(GT(B_length(v_args), NUM(0)))) {
        v_vrun(B_read_file(INDEX(v_args, NUM(0))));
    } else {
        SAY(STR("Vanta-in-Vanta - running the demo program:"));
        SAY(STR("----"));
        v_vrun(v_DEMO);
    }
    return 0;
}
