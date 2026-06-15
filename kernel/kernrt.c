/* V-NOx graphical kernel runtime: freestanding Value runtime + framebuffer GUI. */
typedef __builtin_va_list va_list;
#define va_start __builtin_va_start
#define va_arg __builtin_va_arg
#define va_end __builtin_va_end
typedef unsigned int u32; typedef unsigned char u8;

void* memcpy(void* d,const void* s,unsigned long n){ u8* a=d; const u8* b=s; for(unsigned long i=0;i<n;i++)a[i]=b[i]; return d; }
void* memset(void* d,int c,unsigned long n){ u8* a=d; for(unsigned long i=0;i<n;i++)a[i]=(u8)c; return d; }
void* memmove(void* d,const void* s,unsigned long n){ u8* a=d; const u8* b=s; if(a<b)for(unsigned long i=0;i<n;i++)a[i]=b[i]; else for(unsigned long i=n;i;i--)a[i-1]=b[i-1]; return d; }
static unsigned long slen(const char* s){ unsigned long n=0; while(s[n])n++; return n; }
static int scmp(const char* a,const char* b){ while(*a&&*a==*b){a++;b++;} return (u8)*a-(u8)*b; }
static char* scpy(char* d,const char* s){ char* r=d; while((*d++=*s++)){} return r; }
static char* scat(char* d,const char* s){ char* r=d; while(*d)d++; while((*d++=*s++)){} return r; }
static char* sstr(const char* h,const char* n){ unsigned long nl=slen(n); if(!nl)return (char*)h; for(;*h;h++){ unsigned long i=0; while(i<nl&&h[i]==n[i])i++; if(i==nl)return (char*)h; } return 0; }
static unsigned char heap[96u*1024u*1024u]; static unsigned long hp=0;
static void* galloc(long n){ n=(n+7)&~7L; if(hp+(unsigned long)n>sizeof(heap))hp=0; void* p=&heap[hp]; hp+=n; return p; }
static char* numstr(long v){ char t[32]; int i=0,neg=v<0; unsigned long u=neg?(unsigned long)(-v):(unsigned long)v; if(!u)t[i++]='0'; while(u){t[i++]=(char)('0'+u%10);u/=10;} char* b=galloc(i+neg+1); int j=0; if(neg)b[j++]='-'; while(i)b[j++]=t[--i]; b[j]=0; return b; }

enum { TN,TS,TB,TL,TM,TX };
typedef struct Value Value; typedef struct { Value* items; long len,cap; } List; typedef struct { char** keys; Value* vals; long len,cap; } Map;
struct Value { int t; long n; char* s; List* l; Map* m; };
static Value NUM(long n){ Value v; v.t=TN; v.n=n; v.s=0;v.l=0;v.m=0; return v; }
static Value BOOLV(int b){ Value v=NUM(b?1:0); v.t=TB; return v; }
static Value NIL(void){ Value v=NUM(0); v.t=TX; return v; }
static Value STR(const char* s){ Value v; v.t=TS; if(!s)s=""; char* r=galloc(slen(s)+1); scpy(r,s); v.s=r; v.n=0;v.l=0;v.m=0; return v; }
static char* tostr(Value v){ if(v.t==TS)return v.s; if(v.t==TN)return numstr(v.n); if(v.t==TB)return v.n?"yes":"no"; if(v.t==TX)return "nothing"; if(v.t==TL){ char* o=galloc(8192); o[0]=0; scat(o,"["); for(long i=0;i<v.l->len;i++){if(i)scat(o,", ");scat(o,tostr(v.l->items[i]));} scat(o,"]"); return o;} if(v.t==TM){ char* o=galloc(8192); o[0]=0; scat(o,"{"); for(long i=0;i<v.m->len;i++){if(i)scat(o,", ");scat(o,v.m->keys[i]);scat(o,": ");scat(o,tostr(v.m->vals[i]));} scat(o,"}"); return o;} return ""; }
static int truthy(Value v){ if(v.t==TX)return 0; if(v.t==TB)return v.n!=0; return 1; }
static int veq(Value a,Value b){ if((a.t==TN||a.t==TB)&&(b.t==TN||b.t==TB))return a.n==b.n; if(a.t!=b.t)return 0; if(a.t==TS)return scmp(a.s,b.s)==0; if(a.t==TX)return 1; return 0; }
static Value ADD(Value a,Value b){ if(a.t==TN&&b.t==TN)return NUM(a.n+b.n); char* x=tostr(a); char* y=tostr(b); char* r=galloc(slen(x)+slen(y)+1); scpy(r,x); scat(r,y); Value v; v.t=TS; v.s=r; v.n=0;v.l=0;v.m=0; return v; }
static Value SUB(Value a,Value b){ return NUM(a.n-b.n); } static Value MUL(Value a,Value b){ return NUM(a.n*b.n); } static Value DIVV(Value a,Value b){ return NUM(b.n?a.n/b.n:0); } static Value NEG(Value a){ return NUM(-a.n); }
static Value EQ(Value a,Value b){ return BOOLV(veq(a,b)); } static Value NE(Value a,Value b){ return BOOLV(!veq(a,b)); }
static Value LT(Value a,Value b){ if(a.t==TS&&b.t==TS)return BOOLV(scmp(a.s,b.s)<0); return BOOLV(a.n<b.n); } static Value GT(Value a,Value b){ if(a.t==TS&&b.t==TS)return BOOLV(scmp(a.s,b.s)>0); return BOOLV(a.n>b.n); }
static Value LE(Value a,Value b){ if(a.t==TS&&b.t==TS)return BOOLV(scmp(a.s,b.s)<=0); return BOOLV(a.n<=b.n); } static Value GE(Value a,Value b){ if(a.t==TS&&b.t==TS)return BOOLV(scmp(a.s,b.s)>=0); return BOOLV(a.n>=b.n); }
static Value ANDV(Value a,Value b){ return BOOLV(truthy(a)&&truthy(b)); } static Value ORV(Value a,Value b){ return BOOLV(truthy(a)||truthy(b)); } static Value NOTV(Value a){ return BOOLV(!truthy(a)); }
static List* newlist(void){ List* l=galloc(sizeof(List)); l->len=0;l->cap=8;l->items=galloc(sizeof(Value)*8); return l; }
static Value LIST0(void){ Value v; v.t=TL; v.l=newlist(); v.s=0;v.m=0;v.n=0; return v; }
static void listpush(Value lv,Value x){ List* l=lv.l; if(l->len>=l->cap){ long nc=l->cap*2; Value* ni=galloc(sizeof(Value)*nc); memcpy(ni,l->items,sizeof(Value)*l->len); l->items=ni; l->cap=nc; } l->items[l->len++]=x; }
static Value MKLIST(int n,...){ Value v=LIST0(); va_list ap; va_start(ap,n); for(int i=0;i<n;i++)listpush(v,va_arg(ap,Value)); va_end(ap); return v; }
static Map* newmap(void){ Map* m=galloc(sizeof(Map)); m->len=0;m->cap=8;m->keys=galloc(sizeof(char*)*8);m->vals=galloc(sizeof(Value)*8); return m; }
static Value MAP0(void){ Value v; v.t=TM; v.m=newmap(); v.s=0;v.l=0;v.n=0; return v; }
static void mapset(Value mv,Value k,Value val){ Map* m=mv.m; char* key=tostr(k); for(long i=0;i<m->len;i++)if(scmp(m->keys[i],key)==0){m->vals[i]=val;return;} if(m->len>=m->cap){long nc=m->cap*2;char** nk=galloc(sizeof(char*)*nc);Value* nv=galloc(sizeof(Value)*nc);memcpy(nk,m->keys,sizeof(char*)*m->len);memcpy(nv,m->vals,sizeof(Value)*m->len);m->keys=nk;m->vals=nv;m->cap=nc;} char* kc=galloc(slen(key)+1); scpy(kc,key); m->keys[m->len]=kc; m->vals[m->len]=val; m->len++; }
static Value MKMAP(int n,...){ Value v=MAP0(); va_list ap; va_start(ap,n); for(int i=0;i<n;i++){Value k=va_arg(ap,Value);Value val=va_arg(ap,Value);mapset(v,k,val);} va_end(ap); return v; }
static Value INDEX(Value c,Value k){ if(c.t==TL){long i=k.n;if(i<0)i+=c.l->len;if(i<0||i>=c.l->len)return NIL();return c.l->items[i];} if(c.t==TM){char* key=tostr(k);for(long i=0;i<c.m->len;i++)if(scmp(c.m->keys[i],key)==0)return c.m->vals[i];return NIL();} if(c.t==TS){long L=slen(c.s);long i=k.n;if(i<0)i+=L;if(i<0||i>=L)return STR("");char b[2]={c.s[i],0};return STR(b);} return NIL(); }
static void SETAT(Value c,Value k,Value val){ if(c.t==TL){long i=k.n;if(i>=0&&i<c.l->len)c.l->items[i]=val;} else if(c.t==TM)mapset(c,k,val); }
static Value SLICE(Value c,Value a,Value b){ long lo=a.n,hi=b.n; if(c.t==TS){long L=slen(c.s);if(lo<0)lo+=L;if(hi<0)hi+=L;if(lo<0)lo=0;if(hi>L)hi=L;if(hi<lo)hi=lo;char* r=galloc(hi-lo+1);memcpy(r,c.s+lo,hi-lo);r[hi-lo]=0;return STR(r);} if(c.t==TL){Value v=LIST0();long L=c.l->len;if(lo<0)lo+=L;if(hi<0)hi+=L;if(lo<0)lo=0;if(hi>L)hi=L;for(long i=lo;i<hi;i++)listpush(v,c.l->items[i]);return v;} return NIL(); }
static Value LEN(Value v){ if(v.t==TS)return NUM(slen(v.s)); if(v.t==TL)return NUM(v.l->len); if(v.t==TM)return NUM(v.m->len); return NUM(0); }
static Value INOP(Value a,Value b){ if(b.t==TS&&a.t==TS)return BOOLV(sstr(b.s,a.s)!=0); if(b.t==TL){for(long i=0;i<b.l->len;i++)if(veq(a,b.l->items[i]))return BOOLV(1);return BOOLV(0);} return BOOLV(0); }
static Value B_text(Value a){ return STR(tostr(a)); } static Value B_length(Value a){ return LEN(a); }
static Value B_range(Value a,Value b,int two){ Value v=LIST0(); long lo=two?a.n:0,hi=two?b.n:a.n; for(long i=lo;i<hi;i++)listpush(v,NUM(i)); return v; }
static Value B_join(Value lst,Value sep){ if(lst.t!=TL)return STR(""); char* d=tostr(sep); char* o=galloc(8192); o[0]=0; for(long i=0;i<lst.l->len;i++){if(i)scat(o,d);scat(o,tostr(lst.l->items[i]));} return STR(o); }
static Value B_upper(Value a){ char* s=tostr(a); char* r=galloc(slen(s)+1); long i=0; for(;s[i];i++){char c=s[i];r[i]=(c>='a'&&c<='z')?c-32:c;} r[i]=0; return STR(r); }
static Value B_sort(Value lst){ if(lst.t!=TL)return lst; Value v=LIST0(); for(long i=0;i<lst.l->len;i++)listpush(v,lst.l->items[i]); for(long i=1;i<v.l->len;i++){Value key=v.l->items[i];long j=i-1;while(j>=0&&truthy(GT(v.l->items[j],key))){v.l->items[j+1]=v.l->items[j];j--;}v.l->items[j+1]=key;} return v; }

/* ============ framebuffer (double-buffered) + font + mouse + keyboard ============ */
typedef unsigned int u32; typedef unsigned char u8;
static u32 FB,PITCH,SW,SH; static u32* BACK; static u32* WALL;
static inline void outb(unsigned short p,u8 v){ __asm__ volatile("outb %0,%1"::"a"(v),"Nd"(p)); }
static inline u8 inb(unsigned short p){ u8 r; __asm__ volatile("inb %1,%0":"=a"(r):"Nd"(p)); return r; }
static inline void putpx(int x,int y,u32 c){ if((unsigned)x>=SW||(unsigned)y>=SH)return; BACK[y*SW+x]=c; }
static void fillrect(int x,int y,int w,int h,u32 c){ if(x<0){w+=x;x=0;} if(y<0){h+=y;y=0;} for(int j=0;j<h;j++)for(int i=0;i<w;i++)putpx(x+i,y+j,c); }
static const u8 FONT[95][8]={
{0,0,0,0,0,0,0,0},{24,24,24,24,0,0,24,0},{54,54,0,0,0,0,0,0},{54,54,127,54,127,54,54,0},{12,63,3,30,48,31,12,0},{0,99,51,24,12,102,99,0},{28,54,28,110,59,51,110,0},{6,6,3,0,0,0,0,0},
{24,12,6,6,6,12,24,0},{6,12,24,24,24,12,6,0},{0,102,60,255,60,102,0,0},{0,12,12,63,12,12,0,0},{0,0,0,0,0,12,12,6},{0,0,0,63,0,0,0,0},{0,0,0,0,0,12,12,0},{96,48,24,12,6,3,1,0},
{62,99,115,123,111,103,62,0},{12,14,12,12,12,12,63,0},{30,51,48,28,6,51,63,0},{30,51,48,28,48,51,30,0},{56,60,54,51,127,48,120,0},{63,3,31,48,48,51,30,0},{28,6,3,31,51,51,30,0},{63,51,48,24,12,12,12,0},
{30,51,51,30,51,51,30,0},{30,51,51,62,48,24,14,0},{0,12,12,0,0,12,12,0},{0,12,12,0,0,12,12,6},{24,12,6,3,6,12,24,0},{0,0,63,0,0,63,0,0},{6,12,24,48,24,12,6,0},{30,51,48,24,12,0,12,0},
{62,99,123,123,123,3,30,0},{12,30,51,51,63,51,51,0},{63,102,102,62,102,102,63,0},{60,102,3,3,3,102,60,0},{31,54,102,102,102,54,31,0},{127,70,22,30,22,70,127,0},{127,70,22,30,22,6,15,0},{60,102,3,3,115,102,124,0},
{51,51,51,63,51,51,51,0},{30,12,12,12,12,12,30,0},{120,48,48,48,51,51,30,0},{103,102,54,30,54,102,103,0},{15,6,6,6,70,102,127,0},{99,119,127,127,107,99,99,0},{99,103,111,123,115,99,99,0},{28,54,99,99,99,54,28,0},
{63,102,102,62,6,6,15,0},{30,51,51,51,59,30,56,0},{63,102,102,62,54,102,103,0},{30,51,7,14,56,51,30,0},{63,45,12,12,12,12,30,0},{51,51,51,51,51,51,63,0},{51,51,51,51,51,30,12,0},{99,99,99,107,127,119,99,0},
{99,99,54,28,28,54,99,0},{51,51,51,30,12,12,30,0},{127,99,49,24,76,102,127,0},{30,6,6,6,6,6,30,0},{3,6,12,24,48,96,64,0},{30,24,24,24,24,24,30,0},{8,28,54,99,0,0,0,0},{0,0,0,0,0,0,0,255},
{12,12,24,0,0,0,0,0},{0,0,30,48,62,51,110,0},{7,6,6,62,102,102,59,0},{0,0,30,51,3,51,30,0},{56,48,48,62,51,51,110,0},{0,0,30,51,63,3,30,0},{28,54,6,15,6,6,15,0},{0,0,110,51,51,62,48,31},
{7,6,54,110,102,102,103,0},{12,0,14,12,12,12,30,0},{48,0,48,48,48,51,51,30},{7,6,102,54,30,54,103,0},{14,12,12,12,12,12,30,0},{0,0,51,127,127,107,99,0},{0,0,31,51,51,51,51,0},{0,0,30,51,51,51,30,0},
{0,0,59,102,102,62,6,15},{0,0,110,51,51,62,48,120},{0,0,59,110,102,6,15,0},{0,0,62,3,30,48,31,0},{8,12,62,12,12,44,24,0},{0,0,51,51,51,51,110,0},{0,0,51,51,51,30,12,0},{0,0,99,107,127,127,54,0},
{0,0,99,54,28,54,99,0},{0,0,51,51,51,62,48,31},{0,0,63,25,12,38,63,0},{56,12,12,7,12,12,56,0},{24,24,24,0,24,24,24,0},{7,12,12,56,12,12,7,0},{110,59,0,0,0,0,0,0}};
static void drawchar(int x,int y,char c,u32 col,int sc){ if(c<32||c>126)return; const u8* g=FONT[c-32]; for(int r=0;r<8;r++){u8 b=g[r];for(int i=0;i<8;i++)if(b&(1<<i))fillrect(x+i*sc,y+r*sc,sc,sc,col);} }
static void drawtext(int x,int y,const char* s,u32 col,int sc){ int cx=x; while(*s){ if(*s=='\n'){y+=8*sc+3;cx=x;} else {drawchar(cx,y,*s,col,sc);cx+=8*sc;} s++; } }
static const char SCAN[128]={0,27,'1','2','3','4','5','6','7','8','9','0','-','=','\b','\t','q','w','e','r','t','y','u','i','o','p','[',']','\n',0,'a','s','d','f','g','h','j','k','l',';',39,'`',0,92,'z','x','c','v','b','n','m',',','.','/',0,'*',0,' '};
static char kbuf=0; static int mpkt[3],mcyc=0,mx,my,mbtn;
static void ps2in(void){ int g=0; while((inb(0x64)&2)&&g++<200000){} }
static void mwait(void){ int g=0; while(!(inb(0x64)&1)&&g++<200000){} }
static void mcmd(u8 c){ ps2in(); outb(0x64,0xD4); ps2in(); outb(0x60,c); mwait(); inb(0x60); }
static void mouse_init(void){ ps2in(); outb(0x64,0xA8); ps2in(); outb(0x64,0x20); mwait(); u8 cfg=inb(0x60); cfg|=2; cfg&=~0x20; ps2in(); outb(0x64,0x60); ps2in(); outb(0x60,cfg); mcmd(0xF6); mcmd(0xF4); }
static void mouse_byte(u8 b){ if(mcyc==0&&!(b&8))return; mpkt[mcyc++]=b; if(mcyc==3){ mcyc=0; int dx=mpkt[1],dy=mpkt[2]; if(mpkt[0]&0x10)dx|=~0xff; if(mpkt[0]&0x20)dy|=~0xff; mbtn=mpkt[0]&1; mx+=dx; my-=dy; if(mx<0)mx=0; if(mx>=(int)SW)mx=SW-1; if(my<0)my=0; if(my>=(int)SH)my=SH-1; } }
Value v_poll(void){ int g=0; while(g++<256){ u8 st=inb(0x64); if(!(st&1))break; u8 d=inb(0x60); if(st&0x20)mouse_byte(d); else if(!(d&0x80)&&d<128){char c=SCAN[d]; if(c)kbuf=c;} } return NIL(); }
Value v_key(void){ if(kbuf){char b[2]={kbuf,0}; kbuf=0; return STR(b);} return STR(""); }
Value v_mouse_x(void){ return NUM(mx); } Value v_mouse_y(void){ return NUM(my); } Value v_mouse_down(void){ return NUM(mbtn); }
Value v_rgb(Value r,Value g,Value b){ return NUM((long)(((u32)r.n<<16)|((u32)g.n<<8)|(u32)b.n)); }
Value v_fill(Value x,Value y,Value w,Value h,Value c){ fillrect(x.n,y.n,w.n,h.n,(u32)c.n); return NIL(); }
Value v_text_at(Value x,Value y,Value s,Value c){ drawtext(x.n,y.n,tostr(s),(u32)c.n,1); return NIL(); }
Value v_text_big(Value x,Value y,Value s,Value c){ drawtext(x.n,y.n,tostr(s),(u32)c.n,2); return NIL(); }
Value v_screen_w(void){ return NUM(SW); } Value v_screen_h(void){ return NUM(SH); }
Value v_wallpaper(void){ for(u32 y=0;y<SH;y++){u32 t=(y*130)/SH;u32 c=((0x0a+t/16)<<16)|((0x12+t/4)<<8)|(0x26+t);for(u32 x=0;x<SW;x++)WALL[y*SW+x]=c;} for(u32 i=0;i<SW*SH;i++)BACK[i]=WALL[i]; return NIL(); }
Value v_clear(void){ for(u32 i=0;i<SW*SH;i++)BACK[i]=WALL[i]; return NIL(); }
Value v_present(void){ for(u32 y=0;y<SH;y++){ u32* d=(u32*)(FB+y*PITCH); u32* s=&BACK[y*SW]; for(u32 x=0;x<SW;x++)d[x]=s[x]; } return NIL(); }
Value v_cursor(Value vx,Value vy){ int x=vx.n,y=vy.n; for(int r=0;r<16;r++){ int w=r+1; if(w>10)w=10; for(int c=0;c<w;c++) putpx(x+c+1,y+r+1,0x000000); } for(int r=0;r<16;r++){ int w=r+1; if(w>10)w=10; for(int c=0;c<w;c++) putpx(x+c,y+r,0xffffff); } return NIL(); }
static unsigned long frame_mark=0;
Value v_gc_mark(void){ frame_mark=hp; return NIL(); }
Value v_frame_reset(void){ hp=frame_mark; return NIL(); }
static Value SAY(Value v){ (void)v; return NIL(); }
extern void kmain(void);
void kstart(u32 mbi){ u32* m=(u32*)mbi; FB=*(u32*)((char*)m+88); PITCH=*(u32*)((char*)m+96); SW=*(u32*)((char*)m+100); SH=*(u32*)((char*)m+104); BACK=galloc((long)SW*SH*4); WALL=galloc((long)SW*SH*4); mx=SW/2; my=SH/2; mouse_init(); kmain(); for(;;)__asm__ volatile("hlt"); }
