/* V-NOx kernel runtime: a freestanding Value runtime - no libc, no OS.
   Output goes to VGA text mode (0xB8000) and the serial port (COM1). */
typedef __builtin_va_list va_list;
#define va_start __builtin_va_start
#define va_arg __builtin_va_arg
#define va_end __builtin_va_end

void* memcpy(void* d,const void* s,unsigned long n){ unsigned char* a=d; const unsigned char* b=s; for(unsigned long i=0;i<n;i++)a[i]=b[i]; return d; }
void* memset(void* d,int c,unsigned long n){ unsigned char* a=d; for(unsigned long i=0;i<n;i++)a[i]=(unsigned char)c; return d; }
void* memmove(void* d,const void* s,unsigned long n){ unsigned char* a=d; const unsigned char* b=s; if(a<b)for(unsigned long i=0;i<n;i++)a[i]=b[i]; else for(unsigned long i=n;i;i--)a[i-1]=b[i-1]; return d; }
int memcmp(const void* x,const void* y,unsigned long n){ const unsigned char* a=x; const unsigned char* b=y; for(unsigned long i=0;i<n;i++) if(a[i]!=b[i]) return a[i]-b[i]; return 0; }

static unsigned long slen(const char* s){ unsigned long n=0; while(s[n])n++; return n; }
static int scmp(const char* a,const char* b){ while(*a&&*a==*b){a++;b++;} return (unsigned char)*a-(unsigned char)*b; }
static int sncmp(const char* a,const char* b,unsigned long n){ for(unsigned long i=0;i<n;i++){ if(a[i]!=b[i])return (unsigned char)a[i]-(unsigned char)b[i]; if(!a[i])break; } return 0; }
static char* scpy(char* d,const char* s){ char* r=d; while((*d++=*s++)); return r; }
static char* scat(char* d,const char* s){ char* r=d; while(*d)d++; while((*d++=*s++)); return r; }
static char* sstr(const char* h,const char* n){ unsigned long nl=slen(n); if(!nl)return (char*)h; for(;*h;h++) if(sncmp(h,n,nl)==0) return (char*)h; return 0; }

static unsigned char heap[64u*1024u*1024u];
static unsigned long hp=0;
static void* galloc(long n){ n=(n+7)&~7L; if(hp+(unsigned long)n>sizeof(heap)) return 0; void* p=&heap[hp]; hp+=n; return p; }

static inline void outb(unsigned short p,unsigned char v){ __asm__ volatile("outb %0,%1"::"a"(v),"Nd"(p)); }
static inline unsigned char inb(unsigned short p){ unsigned char r; __asm__ volatile("inb %1,%0":"=a"(r):"Nd"(p)); return r; }
static volatile unsigned short* VGA=(unsigned short*)0xB8000;
static int vrow=0,vcol=0;
static void serial_init(void){ outb(0x3F9,0); outb(0x3FB,0x80); outb(0x3F8,3); outb(0x3F9,0); outb(0x3FB,0x03); outb(0x3FA,0xC7); outb(0x3FC,0x0B); }
static void serial_putc(char c){ while((inb(0x3FD)&0x20)==0); outb(0x3F8,(unsigned char)c); }
static void vga_putc(char c){ if(c=='\n'){vcol=0;vrow++;} else { VGA[vrow*80+vcol]=(unsigned short)((0x0F<<8)|(unsigned char)c); if(++vcol>=80){vcol=0;vrow++;} } if(vrow>=25){ for(int i=0;i<24*80;i++)VGA[i]=VGA[i+80]; for(int i=24*80;i<25*80;i++)VGA[i]=(0x0F<<8)|' '; vrow=24; } }
static void kputc(char c){ if(c=='\n')serial_putc('\r'); serial_putc(c); vga_putc(c); }
static void kputs(const char* s){ while(*s)kputc(*s++); }
static char* numstr(long v){ char t[32]; int i=0,neg=v<0; unsigned long u=neg?(unsigned long)(-v):(unsigned long)v; if(!u)t[i++]='0'; while(u){t[i++]=(char)('0'+u%10);u/=10;} char* b=galloc(i+neg+1); int j=0; if(neg)b[j++]='-'; while(i)b[j++]=t[--i]; b[j]=0; return b; }

enum { TN, TS, TB, TL, TM, TX };
typedef struct Value Value;
typedef struct { Value* items; long len,cap; } List;
typedef struct { char** keys; Value* vals; long len,cap; } Map;
struct Value { int t; long n; char* s; List* l; Map* m; };
static Value NUM(long n){ Value v; v.t=TN; v.n=n; v.s=0;v.l=0;v.m=0; return v; }
static Value BOOLV(int b){ Value v=NUM(b?1:0); v.t=TB; return v; }
static Value NIL(void){ Value v=NUM(0); v.t=TX; return v; }
static Value STR(const char* s){ Value v; v.t=TS; if(!s)s=""; char* r=galloc(slen(s)+1); scpy(r,s); v.s=r; v.n=0;v.l=0;v.m=0; return v; }
static char* tostr(Value v){ if(v.t==TS)return v.s; if(v.t==TN)return numstr(v.n); if(v.t==TB)return v.n?"yes":"no"; if(v.t==TX)return "nothing"; if(v.t==TL){ char* o=galloc(8192); o[0]=0; scat(o,"["); for(long i=0;i<v.l->len;i++){ if(i)scat(o,", "); scat(o,tostr(v.l->items[i])); } scat(o,"]"); return o; } if(v.t==TM){ char* o=galloc(8192); o[0]=0; scat(o,"{"); for(long i=0;i<v.m->len;i++){ if(i)scat(o,", "); scat(o,v.m->keys[i]); scat(o,": "); scat(o,tostr(v.m->vals[i])); } scat(o,"}"); return o; } return ""; }
static int truthy(Value v){ if(v.t==TX)return 0; if(v.t==TB)return v.n!=0; return 1; }
static int veq(Value a,Value b){ if((a.t==TN||a.t==TB)&&(b.t==TN||b.t==TB))return a.n==b.n; if(a.t!=b.t)return 0; if(a.t==TS)return scmp(a.s,b.s)==0; if(a.t==TX)return 1; return 0; }
static Value ADD(Value a,Value b){ if(a.t==TN&&b.t==TN)return NUM(a.n+b.n); char* x=tostr(a); char* y=tostr(b); char* r=galloc(slen(x)+slen(y)+1); scpy(r,x); scat(r,y); Value v; v.t=TS; v.s=r; v.n=0;v.l=0;v.m=0; return v; }
static Value SUB(Value a,Value b){ return NUM(a.n-b.n); }
static Value MUL(Value a,Value b){ return NUM(a.n*b.n); }
static Value DIVV(Value a,Value b){ return NUM(b.n?a.n/b.n:0); }
static Value NEG(Value a){ return NUM(-a.n); }
static Value EQ(Value a,Value b){ return BOOLV(veq(a,b)); }
static Value NE(Value a,Value b){ return BOOLV(!veq(a,b)); }
static Value LT(Value a,Value b){ if(a.t==TS&&b.t==TS)return BOOLV(scmp(a.s,b.s)<0); return BOOLV(a.n<b.n); }
static Value GT(Value a,Value b){ if(a.t==TS&&b.t==TS)return BOOLV(scmp(a.s,b.s)>0); return BOOLV(a.n>b.n); }
static Value LE(Value a,Value b){ if(a.t==TS&&b.t==TS)return BOOLV(scmp(a.s,b.s)<=0); return BOOLV(a.n<=b.n); }
static Value GE(Value a,Value b){ if(a.t==TS&&b.t==TS)return BOOLV(scmp(a.s,b.s)>=0); return BOOLV(a.n>=b.n); }
static Value ANDV(Value a,Value b){ return BOOLV(truthy(a)&&truthy(b)); }
static Value ORV(Value a,Value b){ return BOOLV(truthy(a)||truthy(b)); }
static Value NOTV(Value a){ return BOOLV(!truthy(a)); }
static List* newlist(void){ List* l=galloc(sizeof(List)); l->len=0;l->cap=8;l->items=galloc(sizeof(Value)*8); return l; }
static Value LIST0(void){ Value v; v.t=TL; v.l=newlist(); v.s=0;v.m=0;v.n=0; return v; }
static void listpush(Value lv,Value x){ List* l=lv.l; if(l->len>=l->cap){ long nc=l->cap*2; Value* ni=galloc(sizeof(Value)*nc); memcpy(ni,l->items,sizeof(Value)*l->len); l->items=ni; l->cap=nc; } l->items[l->len++]=x; }
static Value MKLIST(int n,...){ Value v=LIST0(); va_list ap; va_start(ap,n); for(int i=0;i<n;i++)listpush(v,va_arg(ap,Value)); va_end(ap); return v; }
static Map* newmap(void){ Map* m=galloc(sizeof(Map)); m->len=0;m->cap=8;m->keys=galloc(sizeof(char*)*8);m->vals=galloc(sizeof(Value)*8); return m; }
static Value MAP0(void){ Value v; v.t=TM; v.m=newmap(); v.s=0;v.l=0;v.n=0; return v; }
static void mapset(Value mv,Value k,Value val){ Map* m=mv.m; char* key=tostr(k); for(long i=0;i<m->len;i++) if(scmp(m->keys[i],key)==0){m->vals[i]=val;return;} if(m->len>=m->cap){ long nc=m->cap*2; char** nk=galloc(sizeof(char*)*nc); Value* nv=galloc(sizeof(Value)*nc); memcpy(nk,m->keys,sizeof(char*)*m->len); memcpy(nv,m->vals,sizeof(Value)*m->len); m->keys=nk;m->vals=nv;m->cap=nc; } char* kc=galloc(slen(key)+1); scpy(kc,key); m->keys[m->len]=kc; m->vals[m->len]=val; m->len++; }
static Value MKMAP(int n,...){ Value v=MAP0(); va_list ap; va_start(ap,n); for(int i=0;i<n;i++){ Value k=va_arg(ap,Value); Value val=va_arg(ap,Value); mapset(v,k,val); } va_end(ap); return v; }
static Value INDEX(Value c,Value k){ if(c.t==TL){ long i=k.n; if(i<0)i+=c.l->len; if(i<0||i>=c.l->len)return NIL(); return c.l->items[i]; } if(c.t==TM){ char* key=tostr(k); for(long i=0;i<c.m->len;i++) if(scmp(c.m->keys[i],key)==0)return c.m->vals[i]; return NIL(); } if(c.t==TS){ long L=slen(c.s); long i=k.n; if(i<0)i+=L; if(i<0||i>=L)return STR(""); char b[2]={c.s[i],0}; return STR(b); } return NIL(); }
static void SETAT(Value c,Value k,Value val){ if(c.t==TL){ long i=k.n; if(i>=0&&i<c.l->len)c.l->items[i]=val; } else if(c.t==TM) mapset(c,k,val); }
static Value SLICE(Value c,Value a,Value b){ long lo=a.n,hi=b.n; if(c.t==TS){ long L=slen(c.s); if(lo<0)lo+=L; if(hi<0)hi+=L; if(lo<0)lo=0; if(hi>L)hi=L; if(hi<lo)hi=lo; char* r=galloc(hi-lo+1); memcpy(r,c.s+lo,hi-lo); r[hi-lo]=0; return STR(r); } if(c.t==TL){ Value v=LIST0(); long L=c.l->len; if(lo<0)lo+=L; if(hi<0)hi+=L; if(lo<0)lo=0; if(hi>L)hi=L; for(long i=lo;i<hi;i++)listpush(v,c.l->items[i]); return v; } return NIL(); }
static Value LEN(Value v){ if(v.t==TS)return NUM(slen(v.s)); if(v.t==TL)return NUM(v.l->len); if(v.t==TM)return NUM(v.m->len); return NUM(0); }
static Value INOP(Value a,Value b){ if(b.t==TS&&a.t==TS)return BOOLV(sstr(b.s,a.s)!=0); if(b.t==TL){ for(long i=0;i<b.l->len;i++) if(veq(a,b.l->items[i]))return BOOLV(1); return BOOLV(0);} return BOOLV(0); }
static void SAY(Value v){ kputs(tostr(v)); kputc('\n'); }
static Value B_text(Value a){ return STR(tostr(a)); }
static Value B_length(Value a){ return LEN(a); }
static Value B_range(Value a,Value b,int two){ Value v=LIST0(); long lo=two?a.n:0,hi=two?b.n:a.n; for(long i=lo;i<hi;i++)listpush(v,NUM(i)); return v; }
static Value B_keys(Value m){ Value v=LIST0(); if(m.t==TM)for(long i=0;i<m.m->len;i++)listpush(v,STR(m.m->keys[i])); return v; }
static Value B_join(Value lst,Value sep){ if(lst.t!=TL)return STR(""); char* d=tostr(sep); char* o=galloc(8192); o[0]=0; for(long i=0;i<lst.l->len;i++){ if(i)scat(o,d); scat(o,tostr(lst.l->items[i])); } return STR(o); }
static Value B_upper(Value a){ char* s=tostr(a); char* r=galloc(slen(s)+1); long i=0; for(;s[i];i++){ char c=s[i]; r[i]=(c>='a'&&c<='z')?c-32:c; } r[i]=0; return STR(r); }
static Value B_sort(Value lst){ if(lst.t!=TL)return lst; Value v=LIST0(); for(long i=0;i<lst.l->len;i++)listpush(v,lst.l->items[i]); for(long i=1;i<v.l->len;i++){ Value key=v.l->items[i]; long j=i-1; while(j>=0&&truthy(GT(v.l->items[j],key))){ v.l->items[j+1]=v.l->items[j]; j--; } v.l->items[j+1]=key; } return v; }

extern void kmain(void);
void kstart(void){ serial_init(); for(int i=0;i<80*25;i++)VGA[i]=(0x0F<<8)|' '; vrow=0;vcol=0; kmain(); kputs("\n[ V-NOx kernel halted - it is now safe to power off ]\n"); }
