// ============================================================================
//  REread. — e-paper reader  (v9 "Kindle-style" redesign)
//  ESP32-S3 N16R8 · GDEY042T81 400x300 SSD1683 · GxEPD2 (1-bit)
//
//  v9 changes vs v8:
//    * Kindle-style layout: top status bar, bottom progress bar, no overlap
//    * Wireless mode is now a runtime Settings option (Off / AP+Remote /
//      Remote-only / STA+Web), persisted in NVS — no more compile-time #define
//    * Partial refresh everywhere sensible; full GC refresh every 6 turns
//    * Fast mono blit via drawBitmap (image turns no longer flash)
//    * Fixed on-screen keyboard: WiFi password actually connects on OK
//    * Fixed PDF font indexing, per-page font leak, number-stack cleanup
//    * Fixed TXT highlight box position (no longer overlaps the top bar)
//    * Fixed image resume, UTF-8 continuation, dual flip, web JS syntax error
//    * Boot bar uses partial updates (one full clear + fast steps)
//    * Optional inactivity sleep; clock screen auto-refreshes
//
//  Pins: MOSI=47 SCK=48 CS=21 DC=20 RST=19 BUSY=18
//        SD:  CS=10 SCK=12 MISO=13 MOSI=11
// ============================================================================
#include <SPI.h>
#include <SD.h>
#include <FS.h>
#include <WiFi.h>
#include <WebServer.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <esp_heap_caps.h>
#include <Preferences.h>
#include <time.h>
#include <vector>
#include <functional>
#include <algorithm>
#include <GxEPD2_BW.h>
#include <Adafruit_GFX.h>
#include <JPEGDEC.h>
#include <PNGdec.h>

#include <Fonts/FreeMono9pt7b.h>
#include <Fonts/FreeSans9pt7b.h>
#include <Fonts/FreeSansBold9pt7b.h>
#include <Fonts/FreeSerif9pt7b.h>
#include <Fonts/FreeSerif12pt7b.h>
#include <Fonts/FreeSerif18pt7b.h>
#include <Fonts/FreeSerif24pt7b.h>
#include <Fonts/FreeSerifBold18pt7b.h>

// ── Pins ──────────────────────────────────────────────────────────────────
#define EPD_MOSI 47
#define EPD_SCK  48
#define EPD_CS   21
#define EPD_DC   20
#define EPD_RST  19
#define EPD_BUSY 18
#define SD_CS    10
#define SD_SCK   12
#define SD_MISO  13
#define SD_MOSI  11

// ── Geometry ──────────────────────────────────────────────────────────────
#define EPD_W       400
#define EPD_H       300
#define EPD_MONO_SZ (EPD_W*EPD_H/8)
#define BRAND       "REread."

#define FONT_UI      &FreeSans9pt7b
#define FONT_UI_BOLD &FreeSansBold9pt7b
#define FONT_META    &FreeMono9pt7b
#define FONT_LOGO    &FreeSerifBold18pt7b

// Kindle-style layout: thin top status bar, thin bottom progress bar.
#define MARGIN_X        26
#define TOP_DIVIDER     20     // y of the line under the top status bar
#define READER_BASELINE 36     // y of the first text baseline in the reader
#define READER_FLOOR    276    // lowest permitted text baseline
#define PROGRESS_Y      282    // y of the bottom reading-progress bar
#define FOOTER_Y        288    // home/menu footer baseline
#define NVS_NS          "reread"
#define NOW_CH          6
#define FULL_EVERY      6      // full GC refresh every N page turns

// ── Wireless (runtime selectable) ─────────────────────────────────────────
enum WifiMode : uint8_t {
  WIFI_OFF = 0,          // radio off
  WIFI_AP_REMOTE = 1,    // AP on ch6 + ESP-NOW remote + web (192.168.4.1)
  WIFI_REMOTE_ONLY = 2,  // STA fixed ch6, ESP-NOW only, no AP/web
  WIFI_STA_WEB = 3       // STA joins saved network + web (ESP-NOW best-effort)
};
#define WIFI_AP_SSID "REread."
#define WIFI_AP_PASS "reread123"
#define WIFI_SSID    "pRaNaG"
#define WIFI_PASS    "Stupid$99"

// ── Display ───────────────────────────────────────────────────────────────
GxEPD2_BW<GxEPD2_420_GDEY042T81, GxEPD2_420_GDEY042T81::HEIGHT>
  display(GxEPD2_420_GDEY042T81(EPD_CS,EPD_DC,EPD_RST,EPD_BUSY));

SPIClass sdSPI(HSPI);
static uint16_t COL_BG = GxEPD_WHITE;
static uint16_t COL_FG = GxEPD_BLACK;

static inline int twStr(const String& s){
  int16_t x1,y1; uint16_t w,h;
  display.getTextBounds(s,0,0,&x1,&y1,&w,&h);
  return (int)w;
}
#define TW(x) (twStr(String(x)))

namespace Disp {
  void begin(){
    pinMode(EPD_CS,OUTPUT);   digitalWrite(EPD_CS,HIGH);
    pinMode(EPD_DC,OUTPUT);   digitalWrite(EPD_DC,HIGH);
    pinMode(EPD_RST,OUTPUT);  digitalWrite(EPD_RST,HIGH);
    pinMode(EPD_BUSY,INPUT_PULLUP);
    pinMode(EPD_MOSI,OUTPUT); pinMode(EPD_SCK,OUTPUT);
    SPI.begin(EPD_SCK,-1,EPD_MOSI,-1);
    display.init(115200,true,2,false);
    display.setRotation(0); display.setTextWrap(false);
  }
  // Full GC refresh (flashes). Use sparingly.
  template<typename F> void full(F r){
    display.setFullWindow(); display.firstPage();
    do { display.fillScreen(COL_BG); r(); } while(display.nextPage());
  }
  // Fast partial window (no flash).
  template<typename F> void partial(int x,int y,int w,int h,F r){
    display.setPartialWindow(x,y,w,h); display.firstPage();
    do { for(int yy=0;yy<h;++yy) display.drawFastHLine(x,y+yy,w,COL_BG); r(); }
    while(display.nextPage());
  }
  // Full-screen fast partial (Kindle "fast" mode).
  template<typename F> void page(F r){
    display.setPartialWindow(0,0,EPD_W,EPD_H); display.firstPage();
    do { display.fillScreen(COL_BG); r(); } while(display.nextPage());
  }
  // 1-bit framebuffer blit. fast=true uses partial waveform.
  void blitMono(uint8_t* mono,bool fast){
    auto fn=[&]{ display.drawBitmap(0,0,mono,EPD_W,EPD_H,COL_FG,COL_BG); };
    if(fast) page(fn); else full(fn);
  }
  void border(){ display.drawRect(4,4,EPD_W-8,EPD_H-8,COL_FG); }
  void header(const char* t){
    display.setFont(FONT_UI_BOLD); display.setTextColor(COL_FG);
    display.setCursor(MARGIN_X,26); display.print(t);
    display.drawFastHLine(MARGIN_X,34,EPD_W-2*MARGIN_X,COL_FG);
  }
  void sleep(){ display.hibernate(); }
  void wake(){ display.init(115200,false,2,false); display.setRotation(0); }
}

// ── Settings (NVS) ────────────────────────────────────────────────────────
Preferences prefs;
namespace Store {
  bool sdReady=false;
  void begin(){
    pinMode(SD_CS,OUTPUT); digitalWrite(SD_CS,HIGH);
    sdSPI.begin(SD_SCK,SD_MISO,SD_MOSI,SD_CS);
    sdReady=SD.begin(SD_CS,sdSPI,8000000);
    prefs.begin(NVS_NS,false);
  }
  String lastBook(){
    String t=prefs.getString("lastTXT","");
    if(!t.length()) t=prefs.getString("lastIMG","");
    if(!t.length()) t=prefs.getString("lastPDF","");
    return t;
  }
  int lastPage(){ return prefs.getInt("page",0); }
  void saveProgress(const String& b,int p,uint8_t k){
    if(k==0) prefs.putString("lastTXT",b);
    else if(k==1) prefs.putString("lastIMG",b);
    else prefs.putString("lastPDF",b);
    prefs.putInt("page",p);
  }
  bool inverted(){ return prefs.getBool("inv",false); }
  void setInverted(bool v){ prefs.putBool("inv",v);
    COL_BG=v?GxEPD_BLACK:GxEPD_WHITE; COL_FG=v?GxEPD_WHITE:GxEPD_BLACK; }
  uint8_t textSize(){ return prefs.getUChar("txt",1); }
  void setTextSize(uint8_t s){ if(s<4) prefs.putUChar("txt",s); }
  String wifiSsid(){ return prefs.getString("wifi_ssid",""); }
  String wifiPass(){ return prefs.getString("wifi_pass",""); }
  void saveWifi(const String& s,const String& p){ prefs.putString("wifi_ssid",s); prefs.putString("wifi_pass",p); }
  uint8_t wifiMode(){ return prefs.getUChar("wmode",WIFI_AP_REMOTE); }
  void saveWifiMode(uint8_t m){ prefs.putUChar("wmode",m); }
  uint8_t sleepMin(){ return prefs.getUChar("sleepm",10); }   // 0=off
  void saveSleepMin(uint8_t m){ prefs.putUChar("sleepm",m); }
}

namespace TSize {
  const GFXfont* font(uint8_t s){
    switch(s){ case 0:return &FreeSerif9pt7b; case 2:return &FreeSerif18pt7b;
      case 3:return &FreeSerif24pt7b; default:return &FreeSerif12pt7b; }
  }
  const char* label(uint8_t s){
    switch(s){ case 0:return "Small"; case 2:return "Large"; case 3:return "XL"; default:return "Medium"; }
  }
}

// ── Remote protocol (must match REread_remote.ino) ────────────────────────
enum RemoteCmd : uint8_t {
  CMD_NONE=0,CMD_UP,CMD_DOWN,CMD_LEFT,CMD_RIGHT,
  CMD_SELECT,CMD_BACK,CMD_HOME,
  CMD_THEME,CMD_SLEEP,CMD_WAKE,CMD_OLED_TOGGLE,
  CMD_PING,CMD_PONG,CMD_ACK,CMD_ZOOM_IN,CMD_ZOOM_OUT
};
#pragma pack(push,1)
struct RemotePacket { uint8_t cmd,seq; uint16_t battery; uint8_t checksum; };
#pragma pack(pop)
static volatile uint8_t g_webCmd = CMD_NONE;

namespace Input {
  static uint8_t bmac[6]={0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};
  static uint8_t lastSeq=0; static volatile bool ready=false;
  static RemotePacket rx; static bool awake=true; static bool inited=false;
  static uint8_t xsum(const RemotePacket& p){
    const uint8_t* b=(const uint8_t*)&p; uint8_t c=0;
    for(int i=0;i<3;i++) c^=b[i]; return c;
  }
  static void onRecv(const uint8_t*,const uint8_t* d,int len){
    if(len!=(int)sizeof(RemotePacket)) return;
    RemotePacket p; memcpy(&p,d,sizeof(p));
    if(xsum(p)!=p.checksum||p.seq==lastSeq) return;
    lastSeq=p.seq; rx=p; ready=true;
  }
  static void onSend(const uint8_t*,esp_now_send_status_t){}
  // fixedCh=true only for the ch6-locked remote-only profile.
  void begin(bool fixedCh){
    end();
    if(fixedCh) esp_wifi_set_channel(NOW_CH,WIFI_SECOND_CHAN_NONE);
    WiFi.setSleep(WIFI_PS_MIN_MODEM);
    if(esp_now_init()==ESP_OK){
      esp_now_register_send_cb(onSend);
      esp_now_register_recv_cb(onRecv);
      esp_now_peer_info_t p; memset(&p,0,sizeof(p));
      memcpy(p.peer_addr,bmac,6);
      p.channel=fixedCh?NOW_CH:0;
      esp_now_add_peer(&p);
      inited=true;
    }
  }
  void end(){ if(inited){ esp_now_deinit(); inited=false; } }
  bool available(){ return ready; }
  RemoteCmd get(){ ready=false; return (RemoteCmd)rx.cmd; }
  void ack(RemoteCmd c){
    RemotePacket p; p.cmd=(c==CMD_PING)?CMD_PONG:CMD_ACK; p.seq=lastSeq;
    p.battery=0; p.checksum=xsum(p);
    if(inited) esp_now_send(bmac,(uint8_t*)&p,sizeof(p));
  }
  bool isAwake(){ return awake; } void setAwake(bool a){ awake=a; }
}

// ── Image processor → 1-bit ───────────────────────────────────────────────
enum class Resize : uint8_t { FILL=0, FIT=1, STRETCH=2 };
enum class Dither : uint8_t { NONE=0, FLOYD=1, BAYER=2 };
struct ImgSettings {
  Resize resize=Resize::FILL; uint16_t rotation=0;
  bool flipH=false,flipV=false,invert=false;
  int8_t brightness=0,contrast=0; uint8_t threshold=128;
  Dither dither=Dither::FLOYD;
  int32_t cropX=0,cropY=0,cropW=0,cropH=0;
};

namespace ImgProc {
  static void* psAlloc(size_t n){
    void* p=heap_caps_malloc(n,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    if(!p) p=heap_caps_malloc(n,MALLOC_CAP_8BIT); return p;
  }
  static inline uint8_t gray(uint8_t r,uint8_t g,uint8_t b){ return (uint8_t)((r*77+g*150+b*29)>>8); }
  static uint8_t* g_rgb=nullptr; static int g_w=0,g_h=0;
  static const char* g_err="ok";
  int srcW(){ return g_w; } int srcH(){ return g_h; } const char* lastError(){ return g_err; }

  static JPEGDEC jpeg;
  static int jpegCb(JPEGDRAW* p){
    const uint16_t* px=(const uint16_t*)p->pPixels;
    for(int y=0;y<p->iHeight;y++){
      int dy=p->y+y; if(dy<0||dy>=g_h) continue;
      for(int x=0;x<p->iWidth;x++){
        int dx=p->x+x; if(dx<0||dx>=g_w) continue;
        uint16_t c=px[y*p->iWidth+x];
        uint8_t* d=g_rgb+((size_t)dy*g_w+dx)*3;
        d[0]=(uint8_t)(((c>>11)&0x1F)*255/31);
        d[1]=(uint8_t)(((c>>5)&0x3F)*255/63);
        d[2]=(uint8_t)((c&0x1F)*255/31);
      }
    }
    return 1;
  }
  static bool decJPEG(const uint8_t* data,size_t len,uint32_t maxDim){
    if(!jpeg.openRAM((uint8_t*)data,(int)len,jpegCb)){ g_err="JPEG open"; return false; }
    int w=jpeg.getWidth(),h=jpeg.getHeight();
    bool half=(uint32_t)std::max(w,h)>maxDim; if(half){ w=(w+1)/2; h=(h+1)/2; }
    g_w=w; g_h=h;
    if(g_rgb){ free(g_rgb); g_rgb=nullptr; }
    g_rgb=(uint8_t*)psAlloc((size_t)w*h*3);
    if(!g_rgb){ jpeg.close(); g_err="OOM"; return false; }
    memset(g_rgb,255,(size_t)w*h*3);
    jpeg.setPixelType(RGB565_LITTLE_ENDIAN);
    int rc=jpeg.decode(0,0,(int)(half?JPEG_SCALE_HALF:0)); jpeg.close();
    return rc==1;
  }
  static PNG png;
  static int pngCb(PNGDRAW* p){
    uint16_t line[2048]; if(p->iWidth>2048) return 0;
    png.getLineAsRGB565(p,line,PNG_RGB565_LITTLE_ENDIAN,0xFFFFFFFF);
    int dy=p->y; if(dy<0||dy>=g_h) return 1;
    for(int x=0;x<p->iWidth&&x<g_w;x++){
      uint16_t c=line[x];
      uint8_t* d=g_rgb+((size_t)dy*g_w+x)*3;
      d[0]=(uint8_t)(((c>>11)&0x1F)*255/31);
      d[1]=(uint8_t)(((c>>5)&0x3F)*255/63);
      d[2]=(uint8_t)((c&0x1F)*255/31);
    }
    return 1;
  }
  static bool decPNG(const uint8_t* data,size_t len){
    if(png.openRAM((uint8_t*)data,(int)len,pngCb)!=PNG_SUCCESS){ g_err="PNG open"; return false; }
    g_w=png.getWidth(); g_h=png.getHeight();
    if(g_rgb){ free(g_rgb); g_rgb=nullptr; }
    g_rgb=(uint8_t*)psAlloc((size_t)g_w*g_h*3);
    if(!g_rgb){ png.close(); g_err="OOM"; return false; }
    memset(g_rgb,255,(size_t)g_w*g_h*3);
    int rc=png.decode(nullptr,0); png.close(); return rc==PNG_SUCCESS;
  }
  static uint16_t rd16(const uint8_t* p){ return (uint16_t)p[0]|(uint16_t)(p[1]<<8); }
  static uint32_t rd32(const uint8_t* p){ return (uint32_t)p[0]|((uint32_t)p[1]<<8)|((uint32_t)p[2]<<16)|((uint32_t)p[3]<<24); }
  static bool decBMP(const uint8_t* data,size_t len){
    if(len<54||data[0]!='B'||data[1]!='M'){ g_err="Not BMP"; return false; }
    uint32_t off=rd32(data+10);
    int32_t w=(int32_t)rd32(data+18),h=(int32_t)rd32(data+22);
    uint16_t bpp=rd16(data+28),comp=rd32(data+30);
    if(comp!=0||(bpp!=24&&bpp!=32)){ g_err="BMP unsupported"; return false; }
    bool flip=h>0; if(h<0) h=-h;
    if(w<=0||h<=0){ g_err="BMP dims"; return false; }
    g_w=w; g_h=h;
    if(g_rgb){ free(g_rgb); g_rgb=nullptr; }
    g_rgb=(uint8_t*)psAlloc((size_t)w*h*3);
    if(!g_rgb){ g_err="OOM"; return false; }
    int rowBytes=(((int)w*bpp+31)/32)*4;
    for(int y=0;y<h;y++){
      int sy=flip?(h-1-y):y;
      const uint8_t* row=data+off+(size_t)sy*rowBytes;
      for(int x=0;x<w;x++){
        uint8_t* d=g_rgb+((size_t)y*w+x)*3;
        d[0]=row[x*(bpp/8)+2]; d[1]=row[x*(bpp/8)+1]; d[2]=row[x*(bpp/8)];
      }
    }
    return true;
  }
  static bool cropSource(int32_t x,int32_t y,int32_t cw,int32_t ch){
    if(cw<=0||ch<=0) return true;
    if(x<0)x=0; if(y<0)y=0;
    if(x+cw>g_w)cw=g_w-x; if(y+ch>g_h)ch=g_h-y;
    if(cw<=0||ch<=0) return false;
    uint8_t* out=(uint8_t*)psAlloc((size_t)cw*ch*3);
    if(!out) return false;
    for(int yy=0;yy<ch;yy++) memcpy(out+(size_t)yy*cw*3, g_rgb+((size_t)(y+yy)*g_w+x)*3, (size_t)cw*3);
    free(g_rgb); g_rgb=out; g_w=cw; g_h=ch; return true;
  }
  static bool rotateSource(uint16_t rot){
    rot%=360; if(rot==0) return true;
    int w=g_w,h=g_h, nw=(rot==180)?w:h, nh=(rot==180)?h:w;
    uint8_t* out=(uint8_t*)psAlloc((size_t)nw*nh*3); if(!out) return false;
    for(int y=0;y<h;y++) for(int x=0;x<w;x++){
      int dx,dy;
      if(rot==90){dx=h-1-y;dy=x;} else if(rot==180){dx=w-1-x;dy=h-1-y;} else {dx=y;dy=w-1-x;}
      memcpy(out+((size_t)dy*nw+dx)*3, g_rgb+((size_t)y*w+x)*3, 3);
    }
    free(g_rgb); g_rgb=out; g_w=nw; g_h=nh; return true;
  }
  // Flip axes without in-place corruption. vv (vertical flip) is built from a
  // temporary copy so paired rows can't double-swap; hh mirrors each row.
  static void flipSource(bool hh,bool vv){
    if(!hh&&!vv) return;
    const size_t rowBytes=(size_t)g_w*3;
    uint8_t* src=g_rgb;
    uint8_t* tmp=nullptr;
    if(vv){
      tmp=(uint8_t*)psAlloc((size_t)g_h*rowBytes);
      if(!tmp) return;
      memcpy(tmp,g_rgb,(size_t)g_h*rowBytes);   // snapshot, then rebuild
      src=tmp;
    }
    for(int y=0;y<g_h;y++){
      int sy=vv?(g_h-1-y):y;
      uint8_t* dst=g_rgb+(size_t)y*rowBytes;
      const uint8_t* srow=src+(size_t)sy*rowBytes;
      if(hh){ for(int x=0;x<g_w;x++) memcpy(dst+x*3,srow+(g_w-1-x)*3,3); }
      else   { memcpy(dst,srow,rowBytes); }
    }
    if(tmp) free(tmp);
  }
  static uint8_t* g_gray=nullptr;
  static inline uint8_t sampleAt(int sx,int sy){
    if(sx<0)sx=0; if(sy<0)sy=0; if(sx>=g_w)sx=g_w-1; if(sy>=g_h)sy=g_h-1;
    const uint8_t* p=g_rgb+((size_t)sy*g_w+sx)*3; return gray(p[0],p[1],p[2]);
  }
  static void resize(Resize m){
    if(!g_gray) g_gray=(uint8_t*)psAlloc(EPD_W*EPD_H);
    if(!g_gray||!g_rgb) return;
    memset(g_gray,255,EPD_W*EPD_H);
    if(m==Resize::STRETCH){
      for(int y=0;y<EPD_H;y++) for(int x=0;x<EPD_W;x++)
        g_gray[y*EPD_W+x]=sampleAt(x*g_w/EPD_W,y*g_h/EPD_H);
      return;
    }
    float sw=(float)EPD_W/g_w, sh=(float)EPD_H/g_h;
    float scl=(m==Resize::FIT)?std::min(sw,sh):std::max(sw,sh);
    int dw=std::max(1,(int)(g_w*scl)), dh=std::max(1,(int)(g_h*scl));
    int ox=(m==Resize::FIT)?(EPD_W-dw)/2:0, oy=(m==Resize::FIT)?(EPD_H-dh)/2:0;
    for(int y=0;y<dh;y++) for(int x=0;x<dw;x++){
      int dx=ox+x, dy=oy+y;
      if(dx>=0&&dx<EPD_W&&dy>=0&&dy<EPD_H) g_gray[dy*EPD_W+dx]=sampleAt(x*g_w/dw,y*g_h/dh);
    }
  }
  static void adjust(int8_t br,int8_t ct){
    if(!g_gray) return;
    float c=259.0f*(ct+255)/(255.0f*(259-ct));
    for(int i=0;i<EPD_W*EPD_H;i++){
      int v=(int)(c*(g_gray[i]-128)+128+br);
      g_gray[i]=(uint8_t)std::min(255,std::max(0,v));
    }
  }
  static void toMono(uint8_t* mono,const ImgSettings& s){
    memset(mono,0,EPD_MONO_SZ);
    auto black=[&](int x,int y){ size_t i=(size_t)y*EPD_W+x; mono[i>>3]|=(uint8_t)(0x80>>(i&7)); };
    if(s.dither==Dither::NONE){
      for(int i=0;i<EPD_W*EPD_H;i++){
        bool b=s.invert?(g_gray[i]>=s.threshold):(g_gray[i]<s.threshold);
        if(b) mono[i>>3]|=(uint8_t)(0x80>>(i&7));
      }
      return;
    }
    if(s.dither==Dither::BAYER){
      static const uint8_t B[4][4]={{0,8,2,10},{12,4,14,6},{3,11,1,9},{15,7,13,5}};
      for(int y=0;y<EPD_H;y++) for(int x=0;x<EPD_W;x++){
        int t=(int)s.threshold+((int)B[y&3][x&3]-8)*8; t=std::min(254,std::max(1,t));
        bool b=s.invert?(g_gray[y*EPD_W+x]>=t):(g_gray[y*EPD_W+x]<t);
        if(b) black(x,y);
      }
      return;
    }
    int16_t* e=(int16_t*)psAlloc((size_t)EPD_W*EPD_H*2);
    if(!e){ for(int i=0;i<EPD_W*EPD_H;i++){ bool b=s.invert?(g_gray[i]>=s.threshold):(g_gray[i]<s.threshold); if(b) mono[i>>3]|=(uint8_t)(0x80>>(i&7)); } return; }
    for(int i=0;i<EPD_W*EPD_H;i++) e[i]=g_gray[i];
    for(int y=0;y<EPD_H;y++) for(int x=0;x<EPD_W;x++){
      int i=y*EPD_W+x; int op=std::min(255,std::max(0,(int)e[i]));
      bool b=s.invert?(op>=s.threshold):(op<s.threshold);
      if(b) black(x,y);
      int nv=b?0:255; int err=op-nv;
      if(x+1<EPD_W) e[i+1]+=(int16_t)(err*7/16);
      if(y+1<EPD_H&&x>0) e[i+EPD_W-1]+=(int16_t)(err*3/16);
      if(y+1<EPD_H) e[i+EPD_W]+=(int16_t)(err*5/16);
      if(y+1<EPD_H&&x+1<EPD_W) e[i+EPD_W+1]+=(int16_t)(err*1/16);
    }
    free(e);
  }
  bool process(const uint8_t* data,size_t len,const ImgSettings& s,uint8_t* mono,uint32_t maxDim=1600){
    if(g_rgb){ free(g_rgb); g_rgb=nullptr; } g_w=g_h=0;
    bool ok=false;
    if(len>=2&&data[0]==0xFF&&data[1]==0xD8) ok=decJPEG(data,len,maxDim);
    else if(len>=8&&data[0]==0x89&&memcmp(data+1,"PNG",3)==0) ok=decPNG(data,len);
    else if(len>=2&&data[0]=='B'&&data[1]=='M') ok=decBMP(data,len);
    else { g_err="Unknown format"; return false; }
    if(!ok) return false;
    if(!cropSource(s.cropX,s.cropY,s.cropW,s.cropH)){ g_err="Crop OOM"; return false; }
    if(s.rotation&&!rotateSource(s.rotation)){ g_err="Rotate OOM"; return false; }
    flipSource(s.flipH,s.flipV);
    resize(s.resize); adjust(s.brightness,s.contrast); toMono(mono,s);
    free(g_rgb); g_rgb=nullptr;
    if(g_gray){ free(g_gray); g_gray=nullptr; }
    g_err="ok"; return true;
  }
}

// ── DEFLATE inflate (PDF FlateDecode) ─────────────────────────────────────
namespace Z {
  static const uint8_t* IN; static size_t INP,INL; static uint8_t* OUT; static size_t OUTP,OUTC;
  static uint32_t BB; static int BC; static bool FAIL;
  static void init(const uint8_t* in,size_t inl,uint8_t* out,size_t outc){ IN=in;INL=inl;INP=0;OUT=out;OUTC=outc;OUTP=0;BB=0;BC=0;FAIL=false; }
  static inline void put(uint8_t b){ if(OUTP<OUTC) OUT[OUTP++]=b; else FAIL=true; }
  static uint32_t bits(int n){ while(BC<n){ if(INP>=INL){FAIL=true;return 0;} BB|=(uint32_t)IN[INP++]<<BC; BC+=8; }
    uint32_t v=BB&((1u<<n)-1); BB>>=n; BC-=n; return v; }
  struct H { uint16_t count[16]; uint16_t sym[288]; };
  static void buildH(const uint8_t* l,int n,H& h){
    for(int i=0;i<16;i++) h.count[i]=0;
    for(int i=0;i<n;i++) if(l[i]) h.count[l[i]]++;
    uint16_t offs[16]; offs[1]=0;
    for(int i=1;i<15;i++) offs[i+1]=offs[i]+h.count[i];
    for(int i=0;i<n;i++) if(l[i]) h.sym[offs[l[i]]++]=i;
  }
  static int symOf(const H& h){
    uint32_t code=0; int first=0,idx=0;
    for(int len=1;len<=15;len++){ code|=bits(1); int cnt=h.count[len];
      if((int)(code-first)<cnt) return h.sym[idx+(code-first)];
      idx+=cnt; first+=cnt; first<<=1; code<<=1; }
    return -1;
  }
  static void stored(){ bits(BC&7); uint32_t len=bits(16),nlen=bits(16);
    if((len^0xFFFF)!=nlen){FAIL=true;return;}
    for(uint32_t i=0;i<len;i++){ if(INP>=INL){FAIL=true;return;} put(IN[INP++]); } }
  static void inflateBlock(const H& lh,const H& dh){
    static const int lb[29]={3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258};
    static const int le[29]={0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0};
    static const int db[30]={1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,1025,1537,2049,3073,4097,6145,8193,12289,16385,24577};
    static const int de[30]={0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13};
    for(;;){ int s=symOf(lh); if(s<0) return;
      if(s<256){ put((uint8_t)s); } else if(s==256) return;
      else { s-=257; if(s>=29){FAIL=true;return;} uint32_t len=lb[s]+bits(le[s]);
        int d=symOf(dh); if(d<0) return; if(d>=30){FAIL=true;return;}
        uint32_t dist=db[d]+bits(de[d]); if(dist>OUTP){FAIL=true;return;}
        for(uint32_t i=0;i<len;i++){ uint8_t b=OUT[OUTP-dist]; put(b); if(FAIL)return; } } }
  }
  static void inflate(){
    static const int cOrd[19]={16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15};
    uint8_t ll[288],dl[30],cl[19]; int fin=0;
    while(!fin&&!FAIL){ fin=bits(1); int type=bits(2);
      if(type==0){ stored(); continue; }
      if(type==1){ for(int i=0;i<144;i++)ll[i]=8; for(int i=144;i<256;i++)ll[i]=9;
        for(int i=256;i<280;i++)ll[i]=7; for(int i=280;i<288;i++)ll[i]=8; for(int i=0;i<30;i++)dl[i]=5;
        H lh,dh; buildH(ll,288,lh); buildH(dl,30,dh); inflateBlock(lh,dh); continue; }
      int hlit=bits(5)+257,hdist=bits(5)+1,hclen=bits(4)+4;
      memset(cl,0,19); for(int i=0;i<hclen;i++) cl[cOrd[i]]=bits(3);
      H ch; buildH(cl,19,ch); memset(ll,0,288); memset(dl,0,30);
      int total=hlit+hdist,i=0;
      while(i<total&&!FAIL){ int s=symOf(ch); if(s<0) break;
        if(s<16){ (i<hlit?ll:dl)[i<hlit?i:i-hlit]=s; i++; }
        else if(s==16){ if(i==0){FAIL=true;break;} int r=bits(2)+3;
          uint8_t pv=(i-1<hlit)?ll[i-1]:dl[i-1-hlit];
          for(int k=0;k<r&&i<total;k++){ (i<hlit?ll:dl)[i<hlit?i:i-hlit]=pv; i++; } }
        else if(s==17){ int r=bits(3)+3; for(int k=0;k<r&&i<total;k++){ (i<hlit?ll:dl)[i<hlit?i:i-hlit]=0; i++; } }
        else { int r=bits(7)+11; for(int k=0;k<r&&i<total;k++){ (i<hlit?ll:dl)[i<hlit?i:i-hlit]=0; i++; } } }
      if(FAIL||hlit>288||hdist>30) return;
      H lh,dh; buildH(ll,hlit,lh); buildH(dl,hdist,dh); inflateBlock(lh,dh);
    }
  }
  static size_t run(const uint8_t* in,size_t inl,uint8_t* out,size_t outc){ init(in,inl,out,outc); inflate(); return FAIL?0:OUTP; }
}

// ── PDF engine (text; FlateDecode/DCT passthrough) ────────────────────────
namespace Pdf {
  struct Run { float x,y; int fam; float size; String text; };
  struct FontF { String base; bool has; int widths[256]; };
  struct Page { std::vector<Run> runs; float w,h; };
  static const uint8_t* D; static size_t L; static int root=0;
  static std::vector<int> pageRefs;

  static int parseInt(const uint8_t*&p,const uint8_t*e){ bool neg=false; if(p<e&&(*p=='-'||*p=='+')){neg=*p=='-';p++;}
    int v=0; while(p<e&&*p>='0'&&*p<='9'){v=v*10+(*p-'0');p++;} return neg?-v:v; }
  static void skipWS(const uint8_t*&p,const uint8_t*e){ while(p<e&&(*p==' '||*p=='\t'||*p=='\n'||*p=='\r'||*p=='\f'||*p==0))p++; }
  static bool kw(const uint8_t*&p,const uint8_t*e,const char* k){ size_t n=strlen(k); if(p+n>e) return false;
    if(memcmp(p,k,n)==0){ p+=n; skipWS(p,e); return true; } return false; }
  static int findObj(uint32_t num){
    for(size_t i=0;i+8<L;i++){ if(D[i]>='0'&&D[i]<='9'){ size_t j=i; uint32_t v=0;
        while(j<L&&D[j]>='0'&&D[j]<='9'&&v<100000000){v=v*10+(D[j]-'0');j++;}
        if(j>i){ size_t k=j; while(k<L&&(D[k]==' '||D[k]=='\t'||D[k]=='\n'||D[k]=='\r'))k++;
          if(k+3<L&&memcmp(D+k,"obj",3)==0&&v==num) return (int)i; i=j; } } }
    return -1;
  }
  static const uint8_t* body(uint32_t num){ int off=findObj(num); if(off<0) return nullptr;
    const uint8_t* p=D+off; const uint8_t* e=D+L;
    while(p<e&&!(*p>='0'&&*p<='9'))p++; while(p<e&&*p>='0'&&*p<='9')p++;
    skipWS(p,e); while(p<e&&*p>='0'&&*p<='9')p++; skipWS(p,e);
    if(!kw(p,e,"obj")) return nullptr; return p; }
  static int ref(const uint8_t* p,const uint8_t* e){ skipWS(p,e); int n=parseInt(p,e); skipWS(p,e); parseInt(p,e); skipWS(p,e); return n; }
  static const uint8_t* dictKey(const uint8_t* p,const uint8_t* e,const char* key){
    int depth=0;
    while(p<e){ if(*p=='<'&&p+1<e&&p[1]=='<'){depth++;p+=2;continue;}
      if(*p=='>'&&p+1<e&&p[1]=='>'){depth--;if(depth<=0)return nullptr;p+=2;continue;}
      if(*p=='/'){ p++; skipWS(p,e); const uint8_t* ks=p;
        while(p<e&&*p!=' '&&*p!='\n'&&*p!='\r'&&*p!='/'&&*p!='>'&&*p!='['&&*p!='('&&*p!='<')p++;
        size_t kl=(size_t)(p-ks); if(strlen(key)==kl&&memcmp(ks,key,kl)==0){ skipWS(p,e); return p; } continue; }
      p++; }
    return nullptr;
  }
  static int famOf(const String& b){ String s=b; s.toLowerCase();
    if(s.indexOf("courier")>=0) return 4;
    if(s.indexOf("times")>=0) return s.indexOf("bold")>=0?3:2;
    return s.indexOf("bold")>=0?1:0; }
  static FontF parseFont(int r){
    FontF f; memset(&f,0,sizeof(f));
    const uint8_t* p=body(r); if(!p) return f; const uint8_t* e=D+L;
    const uint8_t* bf=dictKey(p,e,"BaseFont");
    if(bf&&*bf=='/'){ bf++; const uint8_t* s=bf; while(bf<e&&*bf!=' '&&*bf!='\n'&&*bf!='/'&&*bf!='>'&&*bf!='<')bf++;
      size_t n=(size_t)(bf-s); char tmp[96]; if(n>95)n=95; memcpy(tmp,s,n); tmp[n]=0; f.base=tmp; }
    const uint8_t* wd=dictKey(p,e,"Widths"); int first=0;
    const uint8_t* fc=dictKey(p,e,"FirstChar"); if(fc) first=parseInt(fc,e);
    if(wd&&*wd=='['){ wd++; skipWS(wd,e); int idx=first;
      while(wd<e&&*wd!=']'&&idx<256){ int v=parseInt(wd,e); if(v>0){ f.widths[idx]=v; f.has=true; } idx++; skipWS(wd,e); } }
    return f;
  }
  static String parseStr(const uint8_t*&p,const uint8_t*e){ String out;
    if(p<e&&*p=='('){ p++; int d=1;
      while(p<e&&d>0){ if(*p=='\\'&&p+1<e){ p++; char c=*p;
          if(c=='n')out+='\n'; else if(c=='r')out+='\r'; else if(c=='t')out+='\t';
          else if(c=='('||c==')'||c=='\\')out+=c; else out+=c; p++; }
        else if(*p=='('){d++;out+='(';p++;} else if(*p==')'){d--;if(d)out+=')';p++;}
        else { out+=(char)*p; p++; } } }
    else if(p<e&&*p=='<'){ p++; int hi=-1;
      while(p<e&&*p!='>'){ char c=*p; int v=(c>='0'&&c<='9')?c-'0':(c>='A'&&c<='F')?c-'A'+10:(c>='a'&&c<='f')?c-'a'+10:-1;
        if(v>=0){ if(hi<0)hi=v; else{ out+=(char)(hi*16+v); hi=-1; } } p++; } if(p<e)p++; }
    return out;
  }
  static void parseContents(const uint8_t* cs,size_t len,Page& pg,
                            const std::vector<FontF>& pageFonts,const int fontIdxByNum[64]){
    const uint8_t* p=cs; const uint8_t* e=cs+len; float cx=0,cy=0; int fr=-1; float sz=10;
    std::vector<float> st;
    auto popN=[&](int n){ if((int)st.size()>=n){ st.erase(st.end()-n,st.end()); } };
    while(p<e){ skipWS(p,e); if(p>=e) break; char c=*p;
      if(c=='/'){ p++; const uint8_t* s=p;
        while(p<e&&*p!=' '&&*p!='\n'&&*p!='\r'&&*p!='/'&&*p!='>'&&*p!='('&&*p!='<'&&*p!='['&&*p!=']'&&*p!='{')p++;
        String nm; for(const uint8_t*q=s;q<p;q++)nm+=(char)*q; skipWS(p,e);
        if(kw(p,e,"Tf")){ if(!st.empty()){ sz=st.back(); st.clear(); }
          int num=-1; if(nm.length()&&nm[0]=='F') num=nm.substring(1).toInt();
          if(num>=0&&num<64&&fontIdxByNum[num]>=0) fr=fontIdxByNum[num]; else fr=-1;
        }
        continue; }
      if((c>='0'&&c<='9')||c=='-'||c=='+'||c=='.'){ int sg=1; if(*p=='-'){sg=-1;p++;} else if(*p=='+')p++;
        long ip=0; int frc=0,fdiv=1; while(p<e&&*p>='0'&&*p<='9'){ip=ip*10+(*p-'0');p++;}
        if(p<e&&*p=='.'){p++;while(p<e&&*p>='0'&&*p<='9'){frc=frc*10+(*p-'0');fdiv*=10;p++;}}
        st.push_back((float)sg*((float)ip+(fdiv?(float)frc/fdiv:0))); continue; }
      if(c=='('||c=='<'){ String str=parseStr(p,e); skipWS(p,e);
        if(p<e&&memcmp(p,"Tj",2)==0){ p+=2; int fam=0; if(fr>=0&&fr<(int)pageFonts.size()) fam=famOf(pageFonts[fr].base);
          pg.runs.push_back({cx,cy,fam,sz,str}); }
        else if(p<e&&memcmp(p,"TJ",2)==0){ p+=2; int fam=0; if(fr>=0&&fr<(int)pageFonts.size()) fam=famOf(pageFonts[fr].base);
          pg.runs.push_back({cx,cy,fam,sz,str}); }
        else if(p<e&&*p=='\''){ p+=1; pg.runs.push_back({cx,cy,0,sz,str}); }
        st.clear();
        continue; }
      if(c=='T'||c=='E'||c=='D'||c=='B'||c=='q'||c=='Q'||c=='c'||c=='m'||c=='l'||c=='f'||c=='S'||c=='n'||c=='W'||c=='w'||c=='g'||c=='G'||c=='d'||c=='i'||c=='J'||c=='j'||c=='R'||c=='s'||c=='r'||c=='*'||c=='A'||c=='M'||c=='k'){
        if(c=='T'&&p+1<e){ if(p[1]=='d'||p[1]=='D'){ p+=2; if(st.size()>=2){ cx+=st[st.size()-2]; cy+=st[st.size()-1]; } popN(2); }
          else if(p[1]=='m'){ p+=2; if(st.size()>=6){ cx=st[st.size()-6]; cy=st[st.size()-5]; } popN(6); }
          else if(p[1]=='*'){ p+=2; cy-=sz; }
          else if(p[1]=='j'||p[1]=='J'||p[1]=='c'){ p+=2; }  // text params, ignore
          else p++; }
        else p++;
        st.clear(); continue; }
      p++;
    }
  }
  bool open(const uint8_t* data,size_t len){ D=data; L=len; root=0; pageRefs.clear();
    if(!data||len<8) return false;
    for(size_t i=0;i+8<L;i++){ if(memcmp(D+i,"trailer",7)==0){ const uint8_t* r=dictKey(D+i,D+L,"Root"); if(r){ root=ref(r,D+L); break; } } }
    if(!root) return false;
    const uint8_t* cat=body(root); if(!cat) return false;
    const uint8_t* pg=dictKey(cat,D+L,"Pages"); if(!pg) return false;
    std::vector<int> stack; stack.push_back(ref(pg,D+L)); int guard=0;
    while(!stack.empty()&&guard++<2000){ int r=stack.back(); stack.pop_back();
      const uint8_t* o=body(r); if(!o) continue;
      const uint8_t* kids=dictKey(o,D+L,"Kids");
      if(kids&&*kids=='['){ const uint8_t* p=kids+1; while(p<D+L&&*p!=']'){ skipWS(p,D+L); if(p>=D+L) break;
          if(*p>='0'&&*p<='9'){ stack.push_back(ref(p,D+L)); } else p++; } }
      else pageRefs.push_back(r);
    }
    return !pageRefs.empty();
  }
  int pages(){ return (int)pageRefs.size(); }
  static void load(int idx,Page& pg){
    pg.runs.clear(); pg.w=EPD_W; pg.h=EPD_H;
    if(idx<0||idx>=(int)pageRefs.size()) return;
    const uint8_t* o=body(pageRefs[idx]); if(!o) return; const uint8_t* e=D+L;
    const uint8_t* mb=dictKey(o,e,"MediaBox");
    if(mb&&*mb=='['){ mb++; skipWS(mb,e); parseInt(mb,e); skipWS(mb,e); parseInt(mb,e);
      skipWS(mb,e); int x1=parseInt(mb,e); skipWS(mb,e); int y1=parseInt(mb,e);
      if(x1>0&&y1>0){ pg.w=(float)x1; pg.h=(float)y1; } }
    // Parse this page's fonts ONCE, mapping /F<num> -> index in pageFonts.
    std::vector<FontF> pageFonts;
    int fontIdxByNum[64]; for(int i=0;i<64;i++) fontIdxByNum[i]=-1;
    const uint8_t* res=dictKey(o,e,"Resources");
    if(res&&*res=='<'){ const uint8_t* fr=dictKey(res,e,"Font");
      if(fr&&*fr=='<'){ const uint8_t* p=fr; int depth=0;
        while(p<e){ if(*p=='<'&&p+1<e&&p[1]=='<'){depth++;p+=2;continue;}
          if(*p=='>'&&p+1<e&&p[1]=='>'){depth--;if(depth<=0)break;p+=2;continue;}
          if(*p=='/'){ p++; const uint8_t* ns=p;
            while(p<e&&*p!=' '&&*p!='\n'&&*p!='\r'&&*p!='/'&&*p!='>'&&*p!='<')p++;
            String nm; for(const uint8_t*q=ns;q<p;q++) nm+=(char)*q;
            int num=-1; if(nm.length()>1&&nm[0]=='F') num=nm.substring(1).toInt();
            skipWS(p,e);
            int idx2=(int)pageFonts.size();
            pageFonts.push_back(parseFont(ref(p,e)));
            if(num>=0&&num<64) fontIdxByNum[num]=idx2;
            continue; }
          p++; } } }
    const uint8_t* cnt=dictKey(o,e,"Contents"); if(!cnt) return;
    auto loadStream=[&](int r)->std::vector<uint8_t>{ std::vector<uint8_t> out;
      const uint8_t* sp=body(r); if(!sp) return out; const uint8_t* se=D+L;
      const uint8_t* st=sp; while(st+6<se&&memcmp(st,"stream",6)!=0) st++;
      if(st+6>=se) return out; st+=6; if(st<se&&*st=='\r')st++; if(st<se&&*st=='\n')st++;
      const uint8_t* en=st; while(en+9<se&&memcmp(en,"endstream",9)!=0) en++;
      size_t slen=(size_t)(en-st);
      const uint8_t* flt=dictKey(sp,se,"Filter");
      if(flt&&flt[0]=='/'){ flt++; const uint8_t* fs=flt;
        while(flt<se&&*flt!=' '&&*flt!='\n'&&*flt!='/'&&*flt!='>'&&*flt!='<')flt++;
        String fn; for(const uint8_t*q=fs;q<flt;q++)fn+=(char)*q;
        if(fn=="DCTDecode"){ out.assign(st,en); return out; } }
      size_t cap=slen*6+1024; uint8_t* buf=(uint8_t*)ImgProc::psAlloc(cap);
      if(!buf) return out; size_t got=Z::run(st,slen,buf,cap);
      if(got) out.assign(buf,buf+got); free(buf); return out; };
    std::vector<uint8_t> content;
    if(*cnt=='['){ const uint8_t* p=cnt+1; while(p<e&&*p!=']'){ skipWS(p,e); if(p>=e) break;
        if(*p>='0'&&*p<='9'){ auto v=loadStream(ref(p,e)); content.insert(content.end(),v.begin(),v.end()); } else p++; } }
    else { auto v=loadStream(ref(cnt,e)); content.insert(content.end(),v.begin(),v.end()); }
    if(!content.empty()) parseContents(content.data(),content.size(),pg,pageFonts,fontIdxByNum);
  }
}

// ── Reader ────────────────────────────────────────────────────────────────
namespace Reader {
  enum class Mode : uint8_t { NONE, TXT, IMG, PDF };
  enum class RState : uint8_t { READING, MENU, HIGHLIGHT, GOTO };
  Mode mode=Mode::NONE; RState state=RState::READING;
  String path; int page=0,count=0;
  static std::vector<String> tpages, imgList;
  static uint8_t* mono=nullptr;
  static const GFXfont* pgFont=&FreeSerif12pt7b; static int pgLineH=22;
  static uint8_t* pdfBuf=nullptr; static size_t pdfLen=0;
  static float zoom=1.0f; static int panX=0,panY=0;
  static int menuCur=0, hlCur=0; static String gotoBuf;
  static int turnsSinceFull=0;

  struct WordBox { String w; int x,y,wd; };
  static std::vector<WordBox> words; static std::vector<bool> hl;

  static uint8_t* ensureMono(){ if(!mono) mono=(uint8_t*)ImgProc::psAlloc(EPD_MONO_SZ); return mono; }
  static void freeMono(){ if(mono){ free(mono); mono=nullptr; } }
  void reset(); void setReaderFont(uint8_t s);

  // UTF-8 cleaner: skips lead + continuation bytes for multi-byte sequences.
  static char clean(uint8_t b){
    static int skip=0;
    if(skip){ skip--; return 0; }
    if(b<0x80){ if(b>=32&&b<=126) return (char)b;
      if(b=='\n'||b=='\r') return '\n'; if(b=='\t') return ' '; return ' '; }
    if((b&0xE0)==0xC0){ skip=1; return ' '; }   // 2-byte
    if((b&0xF0)==0xE0){ skip=2; return ' '; }   // 3-byte (em-dash, quotes)
    if((b&0xF8)==0xF0){ skip=3; return ' '; }   // 4-byte
    if((b&0xC0)==0x80) return 0;                // stray continuation
    return ' ';
  }
  static int cw(){ return EPD_W-2*MARGIN_X; }
  static int textW(const String& s){ display.setFont(pgFont); return twStr(s); }
  void setReaderFont(uint8_t s){ pgFont=TSize::font(s); pgLineH=pgFont->yAdvance+4; }

  static void paginate(File& f){
    tpages.clear();
    std::vector<String> lines; String cur;
    while(f.available()){ char c=clean((uint8_t)f.read()); if(c==0) continue;
      if(c=='\n'){ lines.push_back(cur); cur=""; } else cur+=c; }
    if(cur.length()) lines.push_back(cur);
    std::vector<String> wrapped;
    int maxLines=(READER_FLOOR-READER_BASELINE)/pgLineH+1; if(maxLines<1) maxLines=1;
    for(auto& para : lines){
      if(!para.length()){ wrapped.push_back(""); continue; }
      String line=""; int li=0;
      while(li<(int)para.length()){
        while(li<(int)para.length()&&para[li]==' ') li++;
        int we=li; while(we<(int)para.length()&&para[we]!=' ') we++;
        if(we==li) break;
        String word=para.substring(li,we);
        String test=line.length()?(line+" "+word):word;
        if(line.length()&&textW(test)>cw()){ wrapped.push_back(line); line=word; } else line=test;
        li=we;
      }
      if(line.length()) wrapped.push_back(line);
    }
    String pg; int lc=0;
    for(auto& l : wrapped){ if(lc>=maxLines){ tpages.push_back(pg); pg=""; lc=0; } pg+=l; pg+="\n"; lc++; }
    if(pg.length()) tpages.push_back(pg);
    count=(int)tpages.size();
  }

  bool openTXT(const String& p){
    if(!Store::sdReady) return false;
    File f=SD.open(p,FILE_READ); if(!f) return false;
    reset(); mode=Mode::TXT; path=p; setReaderFont(Store::textSize());
    hlCur=0;
    paginate(f); f.close();
    if(count==0){ tpages.push_back("(empty)"); count=1; }
    if(Store::lastBook()==p) page=Store::lastPage();
    if(page<0||page>=count) page=0;
    state=RState::READING; turnsSinceFull=0; return true;
  }
  static bool isImg(const String& n){ String s=n; s.toLowerCase();
    return s.endsWith(".bmp")||s.endsWith(".jpg")||s.endsWith(".jpeg")||s.endsWith(".png"); }
  bool openIMG(const String& p){
    if(!Store::sdReady) return false;
    reset(); mode=Mode::IMG; path=p; imgList.clear();
    String dir=p.substring(0,p.lastIndexOf('/')+1);
    File d=SD.open(dir); if(!d) return false;
    while(true){ File e=d.openNextFile(); if(!e) break;
      if(!e.isDirectory()&&isImg(e.name())) imgList.push_back(dir+e.name()); e.close(); }
    d.close(); count=(int)imgList.size();
    path=(count>0)?imgList[0]:p;   // let progress track the current image file
    // Resume by file path (v8 compared against the folder, which never matched).
    String last=Store::lastBook();
    for(int i=0;i<count;i++) if(imgList[i]==last){ page=i; break; }
    if(page<0||page>=count) page=0; ensureMono();
    state=RState::READING; turnsSinceFull=0; return count>0;
  }
  bool openPDF(const String& p){
    if(!Store::sdReady) return false;
    File f=SD.open(p,FILE_READ); if(!f) return false;
    size_t sz=f.size(); reset(); mode=Mode::PDF; path=p;
    pdfBuf=(uint8_t*)ImgProc::psAlloc(sz);
    if(!pdfBuf){ f.close(); return false; }
    f.read(pdfBuf,sz); f.close(); pdfLen=sz;
    if(!Pdf::open(pdfBuf,pdfLen)){ reset(); return false; }
    count=Pdf::pages(); page=0; zoom=1; panX=panY=0;
    if(Store::lastBook()==p) page=Store::lastPage();
    if(page<0||page>=count) page=0;
    state=RState::READING; turnsSinceFull=0; return true;
  }
  bool open(const String& p){ String s=p; s.toLowerCase();
    if(s.endsWith(".txt")) return openTXT(p);
    if(s.endsWith(".pdf")) return openPDF(p);
    if(isImg(p)) return openIMG(p); return false; }
  void reset(){ mode=Mode::NONE; state=RState::READING; tpages.clear(); imgList.clear(); path=""; page=count=0;
    if(pdfBuf){ free(pdfBuf); pdfBuf=nullptr; } pdfLen=0; zoom=1; panX=panY=0;
    words.clear(); hl.clear(); freeMono(); }
  uint8_t kind(){ return (uint8_t)mode; }
  static void syncPath(){ if(mode==Mode::IMG && page<(int)imgList.size()) path=imgList[page]; }
  void next(){ if(page<count-1){ page++; syncPath(); Store::saveProgress(path,page,kind());
    if(mode==Mode::PDF){zoom=1;panX=panY=0;}
    if(mode==Mode::TXT){ words.clear(); hl.clear(); hlCur=0; }
    turnsSinceFull++; } }
  void prev(){ if(page>0){ page--; syncPath(); Store::saveProgress(path,page,kind());
    if(mode==Mode::PDF){zoom=1;panX=panY=0;}
    if(mode==Mode::TXT){ words.clear(); hl.clear(); hlCur=0; }
    turnsSinceFull++; } }
  void goTo(int p){ if(p>=0&&p<count){ page=p; Store::saveProgress(path,page,kind()); turnsSinceFull++; } }
  void zoomIn(){ if(mode==Mode::PDF) zoom=std::min(8.0f,zoom*1.25f); }
  void zoomOut(){ if(mode==Mode::PDF) zoom=std::max(0.25f,zoom/1.25f); }
  bool fullDue(){ return turnsSinceFull>0 && (turnsSinceFull%FULL_EVERY==0); }

  static String titleString(){
    int s=path.lastIndexOf('/')+1; String t=path.substring(s);
    int dot=t.lastIndexOf('.'); if(dot>0) t=t.substring(0,dot);
    if(t.length()>28) t=t.substring(0,27)+"…";
    return t;
  }

  // build word boxes + highlight flags for current TXT page
  static void buildWords(){
    words.clear();
    if(mode!=Mode::TXT||page>=(int)tpages.size()) return;
    display.setFont(pgFont);
    String pg=tpages[page]; int start=0; int y=READER_BASELINE;
    while(start<(int)pg.length()){
      int nl=pg.indexOf('\n',start);
      String line=(nl<0)?pg.substring(start):pg.substring(start,nl);
      int x=MARGIN_X; int li=0;
      while(li<(int)line.length()){
        while(li<(int)line.length()&&line[li]==' '){ li++; x+=6; }
        if(li>=(int)line.length()) break;
        int we=li; while(we<(int)line.length()&&line[we]!=' ') we++;
        String w=line.substring(li,we);
        int wdx=textW(w);
        words.push_back({w,x,y,wdx});
        x+=wdx+6;
        li=we;
      }
      y+=pgLineH; if(y>READER_FLOOR) break;
      start=(nl<0)?(int)pg.length():nl+1;
    }
    hl.assign(words.size(),false);
    if(hlCur>=(int)words.size()) hlCur=0;
  }

  // Kindle-style top status bar (title left, page right) + bottom progress.
  static void chrome(){
    display.setFont(FONT_META); display.setTextColor(COL_FG);
    display.setCursor(MARGIN_X,14); display.print(titleString());
    char h[24]; snprintf(h,sizeof(h),"%d / %d",page+1,count);
    int w=TW(h); display.setCursor(EPD_W-w-MARGIN_X,14); display.print(h);
    display.drawFastHLine(MARGIN_X,TOP_DIVIDER,EPD_W-2*MARGIN_X,COL_FG);
    // progress: thick filled bar + thin dotted track
    int bx=MARGIN_X, bw=EPD_W-2*MARGIN_X;
    for(int x=0;x<bw;x+=3) display.drawPixel(bx+x, PROGRESS_Y+2, COL_FG);
    int pw=(count>0)?(bw*(page+1)/count):0;
    if(pw>0) display.fillRect(bx,PROGRESS_Y,pw,3,COL_FG);
  }

  static void drawTXTWords(){
    display.setFont(pgFont);
    if(words.empty()) buildWords();
    int boxTopAdj=pgFont->yAdvance-4;     // keep box just below top divider
    for(int i=0;i<(int)words.size();i++){
      bool inv=hl[i]||(state==RState::HIGHLIGHT && i==hlCur);
      if(inv){
        display.fillRect(words[i].x-1, words[i].y-boxTopAdj, words[i].wd+2, pgFont->yAdvance, COL_FG);
        display.setTextColor(COL_BG);
      } else display.setTextColor(COL_FG);
      display.setCursor(words[i].x, words[i].y);
      display.print(words[i].w);
    }
  }

  static void drawTXT(){ drawTXTWords(); chrome(); }

  void drawCurrentImage(bool forceFull=false){
    if(mode!=Mode::IMG||!mono||page>=(int)imgList.size()) return;
    File f=SD.open(imgList[page],FILE_READ); if(!f) return;
    size_t sz=f.size(); uint8_t* buf=(uint8_t*)ImgProc::psAlloc(sz);
    if(buf){ f.read(buf,sz); ImgSettings s;
      s.resize=Resize::FIT; ImgProc::process(buf,sz,s,mono); free(buf); }
    f.close();
    Disp::blitMono(mono, forceFull?false:true);
  }

  void drawPDFPage(bool forceFull=false){
    if(mode!=Mode::PDF||!pdfBuf) return;
    Pdf::Page pg; Pdf::load(page,pg);
    // Fit PDF into the content band (between top bar and progress bar).
    float cw=(float)(EPD_W-2*MARGIN_X);
    float top=TOP_DIVIDER+4, bottom=PROGRESS_Y-6, ch=bottom-top;
    float scale=std::min(cw/pg.w,ch/pg.h)*zoom;
    float offX=MARGIN_X+(cw-pg.w*scale)/2.0f-panX;
    float bottomY=bottom;
    struct G { int sx,sy; const GFXfont* f; String t; };
    std::vector<G> gl;
    for(auto& r : pg.runs){
      int sx=(int)(r.x*scale+offX), sy=(int)(bottomY-r.y*scale-panY);
      if(sx<-400||sx>EPD_W+400) continue;
      const GFXfont* f=&FreeSans9pt7b;
      switch(r.fam){ case 4:f=&FreeMono9pt7b;break; case 3:f=&FreeSerifBold18pt7b;break;
        case 2:f=&FreeSerif18pt7b;break; case 1:f=&FreeSansBold9pt7b;break; }
      float ps=r.size*scale;
      if(ps<10) f=&FreeSans9pt7b;
      else if(ps<15) f=(r.fam>=2?&FreeSerif12pt7b:&FreeSans9pt7b);
      else if(ps<24) f=(r.fam>=2?&FreeSerif18pt7b:&FreeSans9pt7b);
      else f=(r.fam>=2?&FreeSerif24pt7b:&FreeSans9pt7b);
      gl.push_back({sx,sy,f,r.text});
    }
    auto body=[&]{
      for(auto& g : gl){ display.setFont(g.f); display.setTextColor(COL_FG);
        display.setCursor(g.sx,g.sy); display.print(g.t); }
      chrome();
    };
    if(forceFull) Disp::full(body); else Disp::page(body);
  }

  static void drawMenu(){
    const char* items[]={"Highlight","Go to page","Close"};
    Disp::page([&]{
      // Dim backdrop: draw a Kindle-style centered panel.
      display.fillRoundRect(MARGIN_X+20,70,EPD_W-2*MARGIN_X-40,150,8,COL_BG);
      display.drawRoundRect(MARGIN_X+20,70,EPD_W-2*MARGIN_X-40,150,8,COL_FG);
      display.setFont(FONT_UI_BOLD); display.setTextColor(COL_FG);
      display.setCursor(MARGIN_X+34,92); display.print("MENU");
      display.drawFastHLine(MARGIN_X+34,98,EPD_W-2*MARGIN_X-68,COL_FG);
      display.setFont(FONT_UI);
      for(int i=0;i<3;i++){ int y=120+i*30; bool sel=(i==menuCur);
        if(sel){ display.fillRoundRect(MARGIN_X+30,y-16,EPD_W-2*MARGIN_X-60,24,4,COL_FG); display.setTextColor(COL_BG); }
        else display.setTextColor(COL_FG);
        display.setCursor(MARGIN_X+42,y); display.print(items[i]); }
    });
  }
  static void drawGoto(){
    Disp::page([&]{
      display.fillRoundRect(MARGIN_X+20,90,EPD_W-2*MARGIN_X-40,110,8,COL_BG);
      display.drawRoundRect(MARGIN_X+20,90,EPD_W-2*MARGIN_X-40,110,8,COL_FG);
      display.setFont(FONT_UI_BOLD); display.setTextColor(COL_FG);
      display.setCursor(MARGIN_X+34,112); display.print("GO TO PAGE");
      display.setFont(FONT_UI);
      display.drawRoundRect(MARGIN_X+34,124,EPD_W-2*MARGIN_X-68,30,5,COL_FG);
      display.setCursor(MARGIN_X+44,144);
      if(gotoBuf.length()) display.print(gotoBuf); else { display.setTextColor(COL_FG); display.print("_"); }
      display.setCursor(MARGIN_X+34,172); display.print("page 1 - "); display.print(count);
      display.setCursor(MARGIN_X+34,190); display.print("SEL type · BACK del · OK go");
    });
  }

  // Draw the reading view. full=true forces a GC refresh (every N turns).
  void drawReading(bool forceFull=false){
    if(mode==Mode::TXT){ if(forceFull) Disp::full(drawTXT); else Disp::page(drawTXT); }
    else if(mode==Mode::PDF) drawPDFPage(forceFull);
    else if(mode==Mode::IMG) drawCurrentImage(forceFull);
  }
  void draw(){
    if(state==RState::MENU){ drawMenu(); return; }
    if(state==RState::GOTO){ drawGoto(); return; }
    drawReading(fullDue());
  }
  void drawOverlay(){ // fast redraw after highlight/goto key (keeps partial)
    if(state==RState::HIGHLIGHT) Disp::page(drawTXT);
    else drawReading(false);
  }
}

// ── Wi-Fi (runtime mode + scan/connect) ───────────────────────────────────
// WebSrv is defined later; forward-declare so WifiMgr::applyMode can call it.
namespace WebSrv { void begin(); void end(); void loop(); }
namespace WifiMgr {
  static int scanCount=0; static int sel=0;
  static bool timeSet=false;
  uint8_t mode(){ return Store::wifiMode(); }
  bool webEnabled(){ uint8_t m=mode(); return m==WIFI_AP_REMOTE||m==WIFI_STA_WEB; }
  bool radioOn(){ return mode()!=WIFI_OFF; }
  bool espNowEnabled(){ uint8_t m=mode(); return m==WIFI_AP_REMOTE||m==WIFI_REMOTE_ONLY; }
  bool connected(){ return WiFi.status()==WL_CONNECTED; }
  String ip(){ return connected()?WiFi.localIP().toString():String(""); }
  String apIP(){ return (mode()==WIFI_AP_REMOTE)?WiFi.softAPIP().toString():String(""); }
  String reachable(){ uint8_t m=mode(); if(m==WIFI_STA_WEB) return ip(); if(m==WIFI_AP_REMOTE) return connected()?ip():apIP(); return String(""); }
  const char* modeLabel(){ uint8_t m=mode();
    switch(m){ case WIFI_OFF:return "Off"; case WIFI_AP_REMOTE:return "AP+Remote";
      case WIFI_REMOTE_ONLY:return "Remote"; default:return "STA+Web"; } }
  const char* modeName(uint8_t m){
    switch(m){ case WIFI_OFF:return "Off"; case WIFI_AP_REMOTE:return "AP + Remote + Web";
      case WIFI_REMOTE_ONLY:return "Remote Only"; default:return "STA + Web"; } }

  // Apply the selected mode: tear down, reconfigure, bring up radio/input/web.
  void applyMode(uint8_t m){
    WebSrv::end(); Input::end();
    WiFi.disconnect(true); WiFi.mode(WIFI_OFF); delay(200);
    Store::saveWifiMode(m);
    switch(m){
      case WIFI_OFF: break;
      case WIFI_AP_REMOTE:
        // AP only on ch6 → ESP-NOW remote stays reliable; web on 192.168.4.1
        WiFi.mode(WIFI_AP);
        WiFi.softAP(WIFI_AP_SSID,WIFI_AP_PASS,NOW_CH);
        Input::begin(true); WebSrv::begin();
        break;
      case WIFI_REMOTE_ONLY:
        WiFi.mode(WIFI_STA);
        Input::begin(true);  // locks ch6, no AP/web
        break;
      case WIFI_STA_WEB:
        WiFi.mode(WIFI_STA);
        { String s=Store::wifiSsid();
          if(s.length()) WiFi.begin(s.c_str(),Store::wifiPass().c_str());
          else if(strlen(WIFI_SSID)>0) WiFi.begin(WIFI_SSID,WIFI_PASS); }
        WebSrv::begin();
        break;
    }
    timeSet=false;
  }
  void begin(){ applyMode(mode()); }
  void maybeSyncTime(){
    if(timeSet) return;
    if(mode()==WIFI_STA_WEB && connected()){ configTime(19800,0,"pool.ntp.org","time.nist.gov"); timeSet=true; }
  }
  void scan(){ scanCount=WiFi.scanNetworks(); sel=0; }
  int count(){ return scanCount; }
  String ssid(int i){ return WiFi.SSID(i); }
  int rssi(int i){ return WiFi.RSSI(i); }
  bool secure(int i){ return WiFi.encryptionType(i)!=WIFI_AUTH_OPEN; }
  void connect(const String& s,const String& p){ Store::saveWifi(s,p);
    if(mode()==WIFI_STA_WEB) WiFi.begin(s.c_str(),p.c_str()); }
}

// ── On-screen keyboard ────────────────────────────────────────────────────
namespace Kbd {
  bool open=false; String buf; int cx=0,cy=0,layer=0; bool numeric=false;
  String* target=nullptr;                 // where to write the result on OK
  enum DoneAction { ACT_NONE, ACT_WIFI, ACT_GOTO };
  DoneAction doneAct=ACT_NONE;
  static const char* R[4][3]={
    {"qwertyuiop","asdfghjkl ","zxcvbnm   "},
    {"QWERTYUIOP","ASDFGHJKL ","ZXCVBNM   "},
    {"1234567890","-_=+[]{}  ","'\"|<>,./?"},
    {"!@#$%^&*()","~`;:'\"_+ ","<>?[]{}|\\ "}
  };
  void open_(String init,bool num,DoneAction a,String* tgt){
    buf=init; cx=cy=0; layer=0; numeric=num; open=true; doneAct=a; target=tgt;
  }
  void draw(){
    Disp::page([&]{
      Disp::header(numeric?"NUM":"TEXT");
      display.setFont(FONT_UI); display.setTextColor(COL_FG);
      display.drawRoundRect(MARGIN_X,44,EPD_W-2*MARGIN_X,26,4,COL_FG);
      display.setCursor(MARGIN_X+8,62); display.print(buf);
      int kx=2,ky=86,kw=36,kh=32,ks=4;
      if(numeric){
        const char* nums="1234567890";
        for(int c=0;c<10;c++){ int px=kx+c*(kw+ks), py=ky;
          bool sel=(cy==0&&cx==c);
          if(sel){ display.fillRoundRect(px,py,kw,kh,4,COL_FG); display.setTextColor(COL_BG); } else { display.drawRoundRect(px,py,kw,kh,4,COL_FG); display.setTextColor(COL_FG); }
          display.setCursor(px+13,py+12); display.print(nums[c]); }
        const char* btns[]={"DEL","OK"};
        int bxs[]={5,210};
        for(int i=0;i<2;i++){ bool sel=(cy==1&&cx==i);
          if(sel){ display.fillRoundRect(bxs[i],ky+kh+ks,kw*3,kh,4,COL_FG); display.setTextColor(COL_BG); } else { display.drawRoundRect(bxs[i],ky+kh+ks,kw*3,kh,4,COL_FG); display.setTextColor(COL_FG); }
          display.setCursor(bxs[i]+40,ky+kh+ks+12); display.print(btns[i]); }
      } else {
        for(int r=0;r<3;r++) for(int c=0;c<10;c++){
          char k=R[layer][r][c]; if(k==' ') continue;
          int px=kx+c*(kw+ks), py=ky+r*(kh+ks);
          bool sel=(cy==r&&cx==c);
          if(sel){ display.fillRoundRect(px,py,kw,kh,4,COL_FG); display.setTextColor(COL_BG); } else { display.drawRoundRect(px,py,kw,kh,4,COL_FG); display.setTextColor(COL_FG); }
          display.setCursor(px+13,py+12); display.print(k);
        }
        const char* btns[]={"SHIFT","SYM","SPACE","DEL","OK"};
        int bxs[]={2,82,162,262,342}; int bws[]={76,76,96,76,56};
        int by=ky+3*(kh+ks);
        for(int i=0;i<5;i++){ bool sel=(cy==3&&cx==i);
          if(sel){ display.fillRoundRect(bxs[i],by,bws[i],kh,4,COL_FG); display.setTextColor(COL_BG); } else { display.drawRoundRect(bxs[i],by,bws[i],kh,4,COL_FG); display.setTextColor(COL_FG); }
          display.setCursor(bxs[i]+10,by+12); display.print(btns[i]); }
      }
    });
  }
  bool handle(uint8_t cmd){
    if(numeric){
      if(cmd==CMD_UP){ cy=cy>0?cy-1:cy; }
      else if(cmd==CMD_DOWN){ cy=cy<1?cy+1:cy; }
      else if(cmd==CMD_LEFT){ cx=cx>0?cx-1:cx; }
      else if(cmd==CMD_RIGHT){ if(cy==0) cx=cx<9?cx+1:cx; else cx=cx<1?cx+1:cx; }
      else if(cmd==CMD_SELECT){
        if(cy==0){ buf+=(char)('0'+cx); }
        else { if(cx==0){ if(buf.length()) buf.remove(buf.length()-1); } else { open=false; return true; } }
      }
      else if(cmd==CMD_BACK){ if(buf.length()) buf.remove(buf.length()-1); }
      else if(cmd==CMD_HOME){ open=false; return true; }
      draw(); return false;
    }
    if(cmd==CMD_UP){ if(cy>0) cy--; }
    else if(cmd==CMD_DOWN){ if(cy<3){ cy++; if(cy==3&&cx>4) cx=4; } }
    else if(cmd==CMD_LEFT){ if(cx>0) cx--; }
    else if(cmd==CMD_RIGHT){ if(cy<3&&cx<9) cx++; else if(cy==3&&cx<4) cx++; }
    else if(cmd==CMD_SELECT){
      if(cy==3){
        if(cx==0) layer=(layer==0)?1:0;
        else if(cx==1) layer=(layer==2)?3:2;
        else if(cx==2) buf+=" ";
        else if(cx==3){ if(buf.length()) buf.remove(buf.length()-1); }
        else { open=false; return true; }
      } else { char k=R[layer][cy][cx]; if(k!=' ') buf+=k; }
    }
    else if(cmd==CMD_BACK){ if(buf.length()) buf.remove(buf.length()-1); }
    else if(cmd==CMD_HOME){ open=false; return true; }
    draw(); return false;
  }
}

// ── Web server (always compiled; enabled by runtime mode) ─────────────────
namespace UI { const char* screenName(); }
namespace WebSrv {
  static WebServer server(80);
  static bool routesSet=false;
  static uint8_t* g_up=nullptr; static size_t g_upLen=0,g_upCap=0;
  static uint8_t* g_mono=nullptr; static bool g_have=false;
  static char g_status[160]="Ready"; static ImgSettings g_set;
  static void* psAlloc(size_t n){ void* p=heap_caps_malloc(n,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    if(!p) p=heap_caps_malloc(n,MALLOC_CAP_8BIT); return p; }
  static void setStat(const char* s){ strncpy(g_status,s,sizeof(g_status)-1); Serial.printf("[web] %s\n",s); }
  static void json(int code,bool ok,const char* m){ String s=String("{\"ok\":")+(ok?"true":"false")+",\"message\":\"";
    for(const char* p=m;*p;p++){ if(*p=='"'||*p=='\\')s+='\\'; if(*p=='\n'){s+="\\n";continue;} s+=*p; }
    s+="\"}"; server.send(code,"application/json",s); }

  static const char LANDING_HTML[] PROGMEM = R"HTML(
<!doctype html><html><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>REread.</title>
<style>*{margin:0;box-sizing:border-box}:root{--bg:#0b0d10;--c:#14171c;--b:#262b33;--t:#e6e9ef;--m:#8a919c;--a:#f59e0b}
body{font-family:-apple-system,Segoe UI,Roboto,sans-serif;background:var(--bg);color:var(--t);min-height:100vh;display:flex;align-items:center;justify-content:center}
.w{max-width:560px;width:100%;padding:40px 20px}.hd{text-align:center;margin-bottom:32px}.lg{font-size:30px;font-weight:800;letter-spacing:-.03em}.lg span{color:var(--a)}
.sub{color:var(--m);font-size:13px;margin-top:6px}.apps{display:grid;grid-template-columns:1fr 1fr;gap:16px}@media(max-width:480px){.apps{grid-template-columns:1fr}}
.app{display:block;text-decoration:none;color:var(--t);background:var(--c);border:1px solid var(--b);border-radius:18px;padding:24px;transition:all .16s}
.app:hover{border-color:var(--a);transform:translateY(-2px);box-shadow:0 8px 28px rgba(245,158,11,.12)}
.ic{width:46px;height:46px;border-radius:13px;background:#1c2027;color:var(--a);display:flex;align-items:center;justify-content:center;font-size:24px;margin-bottom:14px}
h2{font-size:16px;font-weight:700;margin-bottom:4px}p{font-size:12.5px;color:var(--m)}.ft{margin-top:34px;text-align:center;font-size:11px;color:var(--m)}
</style></head><body><div class="w">
<div class="hd"><div class="lg">REread<span>.</span></div><div class="sub">E-Paper control hub · GDEY042T81</div></div>
<div class="apps">
<a class="app" href="/remote"><div class="ic">&#128295;</div><h2>Remote</h2><p>Navigate, turn pages, zoom, invert.</p></a>
<a class="app" href="/image"><div class="ic">&#128444;</div><h2>Image</h2><p>Convert &amp; push an image to the display.</p></a>
</div><div class="ft">REread. e-paper reader</div></div></body></html>
)HTML";

  static const char REMOTE_HTML[] PROGMEM = R"HTML(
<!doctype html><html><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>REread. Remote</title>
<style>*{margin:0;box-sizing:border-box}:root{--bg:#0b0d10;--c:#14171c;--b:#262b33;--t:#e6e9ef;--m:#8a919c;--a:#f59e0b}
body{font-family:-apple-system,Segoe UI,Roboto,sans-serif;background:var(--bg);color:var(--t);min-height:100vh}
.w{max-width:520px;margin:0 auto;padding:30px 18px}.hd{display:flex;justify-content:space-between;margin-bottom:16px}
.bk{color:var(--m);text-decoration:none;font-size:13px}h1{font-size:20px;font-weight:800}h1 span{color:var(--a)}
.st{background:#0f1216;border:1px solid var(--b);color:var(--t);border-radius:14px;padding:14px 18px;font-family:ui-monospace,monospace;font-size:12px;margin-bottom:16px;white-space:pre-line}.st b{color:var(--a)}
.dp{display:grid;grid-template-columns:repeat(3,66px);gap:8px;justify-content:center;user-select:none;touch-action:manipulation}
.k{height:66px;border:1px solid var(--b);background:var(--c);border-radius:14px;font-size:24px;cursor:pointer;display:flex;align-items:center;justify-content:center;color:var(--t)}
.k:active{background:#1c2027;border-color:var(--a)}.k.ok{grid-column:2;background:var(--a);color:#0b0d10;font-size:15px;font-weight:800}
h3{font-size:11px;text-transform:uppercase;letter-spacing:.12em;color:var(--m);margin:18px 0 8px}.pg{display:grid;grid-template-columns:1fr 1fr;gap:10px}
.b{padding:14px;border:1px solid var(--b);background:var(--c);border-radius:13px;font-size:13px;font-weight:700;color:var(--t);cursor:pointer}
.b:active{background:#1c2027;border-color:var(--a)}.b.acc{color:var(--a);border-color:#3a2f14}
</style></head><body><div class="w">
<div class="hd"><a class="bk" href="/">&#8592;</a><h1>Remote<span>.</span></h1></div>
<div class="st" id="st">Connecting…</div>
<div class="dp">
<button class="k" data-c="UP">&#8593;</button><button class="k" data-c="LEFT">&#8592;</button>
<button class="k ok" data-c="SELECT">OK</button><button class="k" data-c="RIGHT">&#8594;</button>
<button class="k" data-c="DOWN">&#8595;</button></div>
<h3>Page</h3><div class="pg">
<button class="b acc" data-c="LEFT">&#8592; Prev</button><button class="b acc" data-c="RIGHT">Next &#8594;</button></div>
<h3>System</h3><div class="pg">
<button class="b" data-c="THEME">Invert</button><button class="b" data-c="HOME">Home</button>
<button class="b" data-c="BACK">Back</button><button class="b" data-c="WAKE">Wake</button></div>
<script>
function send(c){var b=document.querySelector('[data-c="'+c+'"]');if(b){b.style.opacity=.4}
fetch('/api/remote?cmd='+c).then(function(r){return r.json()}).then(function(){if(b)b.style.opacity=1}).catch(function(){if(b)b.style.opacity=1})}
document.querySelectorAll('[data-c]').forEach(function(b){b.onclick=function(){send(b.dataset.c)}});
setInterval(function(){fetch('/api/ui').then(function(r){return r.json()}).then(function(j){
document.getElementById('st').textContent=j.screen+' '+j.reader+"\nWifi "+j.wifi+" · Heap "+j.heap+"k";}).catch(function(){})},1500);
</script></body></html>
)HTML";

  static const char IMAGE_HTML[] PROGMEM = R"HTML(
<!doctype html><html><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>REread. Image</title>
<style>*{margin:0;box-sizing:border-box}:root{--bg:#0b0d10;--c:#14171c;--b:#262b33;--t:#e6e9ef;--m:#8a919c;--a:#f59e0b}
body{font-family:-apple-system,Segoe UI,Roboto,sans-serif;background:var(--bg);color:var(--t);line-height:1.5}
.w{max-width:1040px;margin:0 auto;padding:24px 16px}.hd{display:flex;justify-content:space-between;margin-bottom:16px}
.bk{color:var(--m);text-decoration:none;font-size:13px}h1{font-size:19px;font-weight:800}h1 span{color:var(--a)}
.g{display:grid;grid-template-columns:340px 1fr;gap:16px}@media(max-width:820px){.g{grid-template-columns:1fr}}
.card{background:var(--c);border:1px solid var(--b);border-radius:16px;padding:16px;margin-bottom:12px}
.drop{border:1.5px dashed #3a3322;border-radius:13px;padding:24px;text-align:center;cursor:pointer;color:var(--m);background:#111419}
.drop:hover{border-color:var(--a);color:var(--t)}.drop b{display:block;font-size:14px;color:var(--t);margin-bottom:2px}.drop span{font-size:11px}
.lb{font-size:10px;font-weight:700;color:var(--m);text-transform:uppercase;letter-spacing:.09em;margin:14px 0 6px}
.tg{display:grid;gap:2px;padding:2px;background:#0e1115;border-radius:9px}.tg.c3{grid-template-columns:repeat(3,1fr)}.tg.c4{grid-template-columns:repeat(4,1fr)}
.tb{padding:7px 8px;border:0;background:transparent;border-radius:7px;font-size:11px;font-weight:600;color:var(--m);cursor:pointer}.tb.a{background:#1c2027;color:var(--t)}
input[type=range]{width:100%;accent-color:var(--a)}
.btn{width:100%;padding:12px;border:0;border-radius:11px;font-size:13px;font-weight:700;cursor:pointer;color:#0b0d10;background:var(--a);margin-top:10px}
.btn.sec{background:var(--c);color:var(--t);border:1px solid var(--b)}
.sw{display:flex;align-items:center;justify-content:space-between;margin-top:13px}.sw span{font-size:12px;font-weight:600}
.swn{width:38px;height:22px;background:#2a2f37;border-radius:11px;position:relative;cursor:pointer;border:0}.swn.on{background:var(--a)}
.swn::after{content:'';position:absolute;top:2px;left:2px;width:18px;height:18px;background:#fff;border-radius:50%;transition:.2s}.swn.on::after{left:18px}
.pv{display:grid;grid-template-columns:1fr 1fr;gap:16px}@media(max-width:500px){.pv{grid-template-columns:1fr}}
.pa{aspect-ratio:4/3;background:#0e1115;border-radius:10px;display:flex;align-items:center;justify-content:center;overflow:hidden;border:1px solid var(--b);position:relative}
.pa img{max-width:100%;max-height:100%}.crop{position:absolute;inset:0;cursor:crosshair}
.sim{background:#0e1115;padding:20px;display:flex;justify-content:center;border-radius:0 0 14px 14px}
.bz{background:#1c2027;padding:10px;border-radius:9px;box-shadow:0 10px 28px rgba(0,0,0,.5)}
.scr{background:#d6d3d1;width:400px;max-width:100%;aspect-ratio:4/3;display:flex;align-items:center;justify-content:center;overflow:hidden}
.scr img{width:100%;height:100%;object-fit:contain;image-rendering:pixelated}
.alert{display:none;padding:10px 14px;border-radius:11px;font-size:12px;margin-top:12px;font-weight:600}
.alert.ok{background:#0f2417;color:#4ade80}.alert.err{background:#2a1212;color:#f87171}
</style></head><body><div class="w">
<div class="hd"><a class="bk" href="/">&#8592;</a><h1>Image &#8594; B/W<span>.</span></h1></div>
<div class="g"><div>
<div class="card"><div class="drop" id="dz" onclick="document.getElementById('f').click()"><b>Drop an image</b><span>JPG · PNG · BMP</span></div>
<input type="file" id="f" accept=".jpg,.jpeg,.png,.bmp" hidden></div>
<div class="card">
<div class="lb">Fit</div><div class="tg c3" id="fg"><button class="tb a" data-v="fill">Fill</button><button class="tb" data-v="fit">Fit</button><button class="tb" data-v="stretch">Stretch</button></div>
<div class="lb">Dither</div><div class="tg c3" id="dg"><button class="tb a" data-v="floyd">Floyd</button><button class="tb" data-v="bayer">Bayer</button><button class="tb" data-v="none">None</button></div>
<div class="lb">Rotate</div><div class="tg c4" id="rg"><button class="tb a" data-v="0">0</button><button class="tb" data-v="90">90</button><button class="tb" data-v="180">180</button><button class="tb" data-v="270">270</button></div>
<div class="lb">Threshold <span id="tv">128</span></div><input type="range" id="th" min="1" max="254" value="128">
<div class="lb">Brightness</div><input type="range" id="br" min="-100" max="100" value="0">
<div class="lb">Contrast</div><input type="range" id="ct" min="-100" max="100" value="0">
<div class="lb">Crop <span id="cv">none</span></div><div class="tg c3"><button class="tb" id="cc">Clear</button><button class="tb" id="ca">Apply</button></div>
<div class="sw"><span>Invert</span><button class="swn" id="inv"></button></div>
<div class="sw"><span>Flip H</span><button class="swn" id="fh"></button></div>
<div class="sw"><span>Flip V</span><button class="swn" id="fv"></button></div>
<button class="btn sec" id="bc">Preview</button><button class="btn" id="bd">Display</button>
<div class="alert" id="al"></div></div></div>
<div><div class="pv">
<div class="card"><div class="lb">Original <span style="text-transform:none;font-weight:400">— drag to crop</span></div><div class="pa" id="oa"><img id="oi" style="display:none"><canvas class="crop" id="cl" style="display:none"></canvas><span id="oe" style="color:#3a4048;font-size:12px">No image</span></div></div>
<div class="card"><div class="lb">B/W preview</div><div class="pa"><img id="gp" style="display:none"><span id="ge" style="color:#3a4048;font-size:12px">—</span></div></div></div>
<div class="card"><div class="lb">E-Paper simulation</div><div class="sim"><div class="bz"><div class="scr" id="es"><span style="color:#a8a29e;font-size:13px">waiting</span></div></div></div></div>
</div></div></div>
<script>
var file=null,fit='fill',dither='floyd',rot=0,inv=false,fh=false,fv=false,th=128,br=0,ct=0,crop={x:0,y:0,w:0,h:0};
function $(s){return document.querySelector(s)}
function al(t,m){var a=$('#al');a.style.display='block';a.className='alert '+t;a.innerHTML=m}
$('#th').oninput=function(){th=+this.value;$('#tv').textContent=th};$('#br').oninput=function(){br=+this.value};$('#ct').oninput=function(){ct=+this.value};
$('#inv').onclick=function(){inv=!inv;this.classList.toggle('on',inv)};$('#fh').onclick=function(){fh=!fh;this.classList.toggle('on',fh)};$('#fv').onclick=function(){fv=!fv;this.classList.toggle('on',fv)};
['fg','dg','rg'].forEach(function(g){$('#'+g).onclick=function(e){if(!e.target.classList.contains('tb'))return;this.querySelectorAll('.tb').forEach(function(b){b.classList.remove('a')});e.target.classList.add('a');
var v=e.target.dataset.v;if(g=='fg')fit=v;else if(g=='dg')dither=v;else rot=+v;}});
$('#dz').onclick=function(){$('#f').click()};
['dragenter','dragover'].forEach(function(ev){$('#dz').addEventListener(ev,function(e){e.preventDefault()})});
['dragleave','drop'].forEach(function(ev){$('#dz').addEventListener(ev,function(e){e.preventDefault()})});
$('#dz').ondrop=function(e){if(e.dataTransfer.files[0])pick(e.dataTransfer.files[0])};$('#f').onchange=function(){if(this.files[0])pick(this.files[0])};
function pick(f){file=f;crop={x:0,y:0,w:0,h:0};$('#cv').textContent='none';
var u=URL.createObjectURL(f);var im=new Image();im.onload=function(){setup(im)};im.src=u;
$('#oi').src=u;$('#oi').style.display='block';$('#oe').style.display='none';}
function setup(im){var cv=$('#cl');cv.width=$('#oa').clientWidth;cv.height=$('#oa').clientHeight;cv.style.display='block';
var s=Math.min(cv.width/im.naturalWidth,cv.height/im.naturalHeight);var ox=(cv.width-im.naturalWidth*s)/2,oy=(cv.height-im.naturalHeight*s)/2;
cv.ox=ox;cv.oy=oy;cv.s=s;var ctx=cv.getContext('2d');ctx.clearRect(0,0,cv.width,cv.height);
cv.onmousedown=function(e){var r=cv.getBoundingClientRect();cv.drag={x:e.clientX-r.left,y:e.clientY-r.top}};
cv.onmousemove=function(e){if(!cv.drag)return;var r=cv.getBoundingClientRect();var cx=e.clientX-r.left,cy=e.clientY-r.top;
var x1=Math.max(cv.ox,Math.min(cv.drag.x,cx)),y1=Math.max(cv.oy,Math.min(cv.drag.y,cy));
var x2=Math.min(cv.ox+im.naturalWidth*cv.s,Math.max(cv.drag.x,cx)),y2=Math.min(cv.oy+im.naturalHeight*cv.s,Math.max(cv.drag.y,cy));
crop={x:Math.round((x1-cv.ox)/cv.s),y:Math.round((y1-cv.oy)/cv.s),w:Math.round((x2-x1)/cv.s),h:Math.round((y2-y1)/cv.s)};
$('#cv').textContent=crop.w+'x'+crop.h+' @'+crop.x+','+crop.y;
var c=cv.getContext('2d');c.clearRect(0,0,cv.width,cv.height);c.fillStyle='rgba(245,158,11,.25)';c.fillRect(x1,y1,x2-x1,y2-y1);c.strokeStyle='#f59e0b';c.strokeRect(x1,y1,x2-x1,y2-y1)};
cv.onmouseup=function(){cv.drag=null}}
$('#cc').onclick=function(){crop={x:0,y:0,w:0,h:0};$('#cv').textContent='none';var c=$('#cl').getContext('2d');c.clearRect(0,0,$('#cl').width,$('#cl').height)};
function set(){return new URLSearchParams({resize:fit,rotation:rot,dither:dither,threshold:th,brightness:br,contrast:ct,flipH:fh?1:0,flipV:fv?1:0,invert:inv?1:0,cx:crop.x,cy:crop.y,cw:crop.w,ch:crop.h})}
$('#bc').onclick=async function(){if(!file){al('err','Choose an image');return}
var fd=new FormData();fd.append('image',file,file.name);
await(await fetch('/api/upload',{method:'POST',body:fd})).json();
await fetch('/api/settings',{method:'POST',body:set()});
var r=await(await fetch('/api/convert',{method:'POST'})).json();
al(r.ok?'ok':'err',r.message);
if(r.ok){$('#gp').src='/api/preview.bmp?t='+Date.now();$('#gp').style.display='block';$('#ge').style.display='none';
$('#es').innerHTML='<img src="/api/preview.bmp?t='+Date.now()+'">'}};
$('#bd').onclick=async function(){var r=await(await fetch('/api/display',{method:'POST'})).json();al(r.ok?'ok':'err',r.message)};
</script></body></html>
)HTML";

  static void hLanding(){ server.send_P(200,"text/html",LANDING_HTML); }
  static void hRemote(){ server.send_P(200,"text/html",REMOTE_HTML); }
  static void hImage(){ server.send_P(200,"text/html",IMAGE_HTML); }
  static void hStatus(){ char b[200]; snprintf(b,sizeof(b),"{\"ip\":\"%s\",\"status\":\"%s\",\"heap\":%u}",
      WifiMgr::reachable().c_str(),g_status,(unsigned)ESP.getFreeHeap()); server.send(200,"application/json",b); }
  static void hUI(){ String rd="";
    if(Reader::mode!=Reader::Mode::NONE) rd=" · p"+String(Reader::page+1)+"/"+String(Reader::count)+(Reader::mode==Reader::Mode::PDF?" (PDF)":(Reader::mode==Reader::Mode::IMG?" (IMG)":""));
    char b[200]; snprintf(b,sizeof(b),"{\"screen\":\"%s\",\"reader\":\"%s\",\"wifi\":\"%s\",\"heap\":%u}",
      UI::screenName(),rd.c_str(),WifiMgr::modeLabel(),(unsigned)(ESP.getFreeHeap()/1024)); server.send(200,"application/json",b); }
  static void hRemoteCmd(){ String c=server.arg("cmd"); uint8_t cmd=CMD_NONE;
    if(c=="UP")cmd=CMD_UP; else if(c=="DOWN")cmd=CMD_DOWN;
    else if(c=="LEFT")cmd=CMD_LEFT; else if(c=="RIGHT")cmd=CMD_RIGHT;
    else if(c=="SELECT")cmd=CMD_SELECT; else if(c=="BACK")cmd=CMD_BACK;
    else if(c=="HOME")cmd=CMD_HOME; else if(c=="THEME")cmd=CMD_THEME;
    else if(c=="SLEEP")cmd=CMD_SLEEP; else if(c=="WAKE")cmd=CMD_WAKE;
    else if(c=="ZOOM_IN")cmd=CMD_ZOOM_IN; else if(c=="ZOOM_OUT")cmd=CMD_ZOOM_OUT;
    if(cmd!=CMD_NONE) g_webCmd=cmd;
    json(cmd!=CMD_NONE?200:400,cmd!=CMD_NONE,cmd!=CMD_NONE?"ok":"bad cmd"); }
  static void hSettings(){ if(server.hasArg("resize")){String v=server.arg("resize");
      g_set.resize=(v=="fit")?Resize::FIT:(v=="stretch")?Resize::STRETCH:Resize::FILL;}
    if(server.hasArg("rotation")){int r=server.arg("rotation").toInt();if(r==0||r==90||r==180||r==270)g_set.rotation=r;}
    if(server.hasArg("dither")){String v=server.arg("dither"); g_set.dither=(v=="none")?Dither::NONE:(v=="bayer")?Dither::BAYER:Dither::FLOYD;}
    if(server.hasArg("threshold"))g_set.threshold=(uint8_t)std::min(254,std::max(1,(int)server.arg("threshold").toInt()));
    if(server.hasArg("brightness"))g_set.brightness=(int8_t)std::min(100,std::max(-100,(int)server.arg("brightness").toInt()));
    if(server.hasArg("contrast"))g_set.contrast=(int8_t)std::min(100,std::max(-100,(int)server.arg("contrast").toInt()));
    if(server.hasArg("flipH"))g_set.flipH=server.arg("flipH").toInt()!=0;
    if(server.hasArg("flipV"))g_set.flipV=server.arg("flipV").toInt()!=0;
    if(server.hasArg("invert"))g_set.invert=server.arg("invert").toInt()!=0;
    if(server.hasArg("cx"))g_set.cropX=server.arg("cx").toInt();
    if(server.hasArg("cy"))g_set.cropY=server.arg("cy").toInt();
    if(server.hasArg("cw"))g_set.cropW=server.arg("cw").toInt();
    if(server.hasArg("ch"))g_set.cropH=server.arg("ch").toInt();
    json(200,true,"ok"); }
  static void hUpload(){ HTTPUpload& u=server.upload();
    if(u.status==UPLOAD_FILE_START){ g_have=false; g_upLen=0; if(!g_up){ g_upCap=2*1024*1024; g_up=(uint8_t*)psAlloc(g_upCap); } if(g_up) memset(g_up,0,g_upCap); }
    else if(u.status==UPLOAD_FILE_WRITE){ if(!g_up) return; if(g_upLen+u.currentSize>g_upCap){ setStat(">2MB"); return; } memcpy(g_up+g_upLen,u.buf,u.currentSize); g_upLen+=u.currentSize; }
    else if(u.status==UPLOAD_FILE_END){ g_have=g_up&&g_upLen>16; } }
  static void hUploadDone(){ json(g_have?200:400,g_have,g_have?"Uploaded":"Empty"); }
  static void hConvert(){ if(!g_have){ json(400,false,"Upload first"); return; }
    if(!g_mono) g_mono=(uint8_t*)psAlloc(EPD_MONO_SZ);
    bool ok=ImgProc::process(g_up,g_upLen,g_set,g_mono); json(ok?200:500,ok,ok?"Converted":ImgProc::lastError()); }
  static void hDisplay(){ if(!g_mono){ json(400,false,"Convert first"); return; }
    Disp::blitMono(g_mono,true); setStat("Displayed"); json(200,true,g_status); }
  static void hPreview(){ if(!g_mono){ server.send(404,"text/plain","none"); return; }
    const int rb=((EPD_W+31)/32)*4; const uint32_t isz=(uint32_t)rb*EPD_H, fsz=62+isz;
    uint8_t* bmp=(uint8_t*)psAlloc(fsz); if(!bmp){ server.send(500,"text/plain","oom"); return; }
    memset(bmp,0,fsz); bmp[0]='B'; bmp[1]='M';
    bmp[2]=fsz&0xFF; bmp[3]=(fsz>>8)&0xFF; bmp[4]=(fsz>>16)&0xFF; bmp[5]=(fsz>>24)&0xFF;
    bmp[10]=62; bmp[14]=40;
    bmp[18]=EPD_W&0xFF; bmp[19]=(EPD_W>>8)&0xFF;
    bmp[22]=EPD_H&0xFF; bmp[23]=(EPD_H>>8)&0xFF;
    bmp[26]=1; bmp[28]=1;
    bmp[34]=isz&0xFF; bmp[35]=(isz>>8)&0xFF; bmp[36]=(isz>>16)&0xFF; bmp[37]=(isz>>24)&0xFF;
    bmp[54]=0;bmp[55]=0;bmp[56]=0;bmp[57]=0;
    bmp[58]=255;bmp[59]=255;bmp[60]=255;bmp[61]=0;
    for(int y=0;y<EPD_H;y++){ int sy=EPD_H-1-y; uint8_t* row=bmp+62+(size_t)y*rb;
      for(int x=0;x<EPD_W;x++){ size_t i=(size_t)sy*EPD_W+x;
        if(!(g_mono[i>>3]&(uint8_t)(0x80>>(i&7)))) row[x>>3]|=(uint8_t)(0x80>>(x&7)); } }
    server.sendHeader("Cache-Control","no-cache"); server.setContentLength(fsz); server.send(200,"image/bmp","");
    WiFiClient c=server.client(); size_t sent=0;
    while(sent<fsz){ size_t n=c.write(bmp+sent,fsz-sent); if(!n) break; sent+=n; } free(bmp); }

  void regRoutes(){
    server.on("/",hLanding); server.on("/remote",hRemote); server.on("/image",hImage);
    server.on("/api/status",hStatus); server.on("/api/ui",hUI);
    server.on("/api/remote",HTTP_ANY,hRemoteCmd);
    server.on("/api/settings",HTTP_POST,hSettings); server.on("/api/settings",HTTP_GET,hSettings);
    server.on("/api/upload",HTTP_POST,hUploadDone,hUpload);
    server.on("/api/convert",HTTP_POST,hConvert); server.on("/api/display",HTTP_POST,hDisplay);
    server.on("/api/preview.bmp",HTTP_GET,hPreview);
    routesSet=true;
  }
  void begin(){ if(!routesSet) regRoutes(); if(!g_mono) g_mono=(uint8_t*)psAlloc(EPD_MONO_SZ);
    server.begin(); Serial.printf("[WEB] http://%s/\n",WifiMgr::reachable().c_str()); }
  void end(){ server.stop(); }
  void loop(){ server.handleClient(); }
}

// ── UI ────────────────────────────────────────────────────────────────────
namespace UI {
  enum Screen { HOME, FILES, READER, CLOCK, NETWORK, SETTINGS, WIRELESS, ABOUT };
  Screen screen=HOME;
  struct Item { String name; bool dir; };
  static std::vector<Item> files; static int cursor=0,topVis=0; static String cwd="/";
  static int menuCur=0, setCur=0, wifiCur=0, wirelessCur=0;
  static unsigned long lastActivity=0, lastClockDraw=0;
  static uint8_t sleepSel=1;        // index into sleep options
  static const uint16_t sleepMins[4]={0,5,10,30};
  static const char* sleepLbl[4]={"Off","5 min","10 min","30 min"};

  static void listFolder(const String& path){
    files.clear(); cwd=path; if(!Store::sdReady) return;
    File d=SD.open(path); if(!d) return;
    while(true){ File e=d.openNextFile(); if(!e) break;
      String n=e.name(); if(n.endsWith("/")) n=n.substring(0,n.length()-1);
      bool show=e.isDirectory(); String lo=n; lo.toLowerCase();
      if(!show&&(lo.endsWith(".txt")||lo.endsWith(".bmp")||lo.endsWith(".jpg")||lo.endsWith(".jpeg")||lo.endsWith(".png")||lo.endsWith(".pdf"))) show=true;
      if(show) files.push_back({n,e.isDirectory()}); e.close(); }
    d.close();
    std::sort(files.begin(),files.end(),[](const Item&a,const Item&b){ if(a.dir!=b.dir) return a.dir; return a.name.compareTo(b.name)<0; });
    cursor=topVis=0;
  }
  void begin(){ listFolder("/");
    uint8_t m=Store::sleepMin(); sleepSel=1; for(int i=0;i<4;i++) if(sleepMins[i]==m) sleepSel=i;
    lastActivity=millis();
  }
  static void poke(){ lastActivity=millis(); }

  static void footer(){
    display.setFont(FONT_META); display.setTextColor(COL_FG);
    display.drawFastHLine(0,FOOTER_Y,EPD_W,COL_FG);
    char t[8]="--:--"; struct tm tv;
    if(getLocalTime(&tv,20)) strftime(t,sizeof(t),"%H:%M",&tv);
    String ip=WifiMgr::webEnabled()?WifiMgr::reachable():"off";
    if(!WifiMgr::radioOn()) ip="radio off";
    String line=String(t)+"  "+ip;
    display.setCursor(MARGIN_X,EPD_H-6); display.print(line);
    int bw=TW(BRAND); display.setCursor(EPD_W-bw-MARGIN_X,EPD_H-6); display.print(BRAND);
  }

  static void drawHomeBody(){
    const char* items[]={"Resume","Library","Network","Clock","Settings","About"};
    int sy=150;
    for(int i=0;i<6;i++){ int y=sy+i*22; bool sel=(i==menuCur);
      display.setFont(FONT_UI);
      if(sel){ display.fillRoundRect(EPD_W/2-90,y-15,180,22,5,COL_FG); display.setTextColor(COL_BG); }
      else display.setTextColor(COL_FG);
      int w=TW(items[i]); display.setCursor((EPD_W-w)/2,y); display.print(items[i]); }
  }
  static const int HOME_TOP=120, HOME_H=160;
  void drawHome(){
    Disp::full([&]{
      Disp::border();
      display.setFont(FONT_LOGO); display.setTextColor(COL_FG);
      int16_t x1,y1; uint16_t w,h; display.getTextBounds(BRAND,0,0,&x1,&y1,&w,&h);
      display.setCursor((EPD_W-(int)w)/2-x1, 70-y1); display.print(BRAND);
      drawHomeBody(); footer();
    });
  }
  static void drawHomePartial(){ Disp::partial(0,HOME_TOP,EPD_W,HOME_H,drawHomeBody); }

  static void drawFilesBody(){
    if(!Store::sdReady||files.empty()){ display.setFont(FONT_UI); display.setTextColor(COL_FG);
      display.setCursor(MARGIN_X,60); display.print(Store::sdReady?"Empty.":"Insert a card."); return; }
    const int per=11; if(cursor<topVis) topVis=cursor; if(cursor>=topVis+per) topVis=cursor-per+1;
    display.setFont(FONT_UI);
    for(int i=0;i<per;i++){ int idx=topVis+i; if(idx>=(int)files.size()) break;
      int y=40+i*21; bool sel=(idx==cursor);
      if(sel){ display.fillRoundRect(MARGIN_X-4,y-14,EPD_W-2*MARGIN_X+8,19,3,COL_FG); display.setTextColor(COL_BG); }
      else display.setTextColor(COL_FG);
      display.setCursor(MARGIN_X,y);
      if(files[idx].dir) display.print("["); display.print(files[idx].name); if(files[idx].dir) display.print("]"); }
  }
  static const int FILES_TOP=28, FILES_H=250;
  void drawFiles(){ Disp::full([&]{ Disp::border(); Disp::header(Store::sdReady?"LIBRARY":"NO SD"); drawFilesBody(); footer(); }); }
  static void drawFilesPartial(){ Disp::partial(0,FILES_TOP,EPD_W,FILES_H,drawFilesBody); }

  void drawClock(){
    Disp::page([&]{
      Disp::border(); Disp::header("CLOCK");
      struct tm t;
      if(getLocalTime(&t,50)){
        char buf[8]; strftime(buf,sizeof(buf),"%H:%M",&t);
        display.setFont(FONT_LOGO); display.setTextColor(COL_FG);
        int tw=TW(buf); display.setCursor((EPD_W-tw)/2,140); display.print(buf);
        char d[32]; strftime(d,sizeof(d),"%A, %d %B %Y",&t);
        display.setFont(FONT_UI); int dw=TW(d); display.setCursor((EPD_W-dw)/2,180); display.print(d);
      } else { display.setFont(FONT_UI); display.setTextColor(COL_FG);
        display.setCursor(MARGIN_X,120); display.print("Waiting for time sync..."); }
      footer();
    });
    lastClockDraw=millis();
  }

  static void drawWifiBody(){
    display.setFont(FONT_UI);
    uint8_t m=WifiMgr::mode();
    if(m==WIFI_OFF){ display.setCursor(MARGIN_X,70); display.print("Wireless is off."); display.setCursor(MARGIN_X,92); display.print("Settings > Wireless to enable."); return; }
    if(m==WIFI_AP_REMOTE){
      display.setCursor(MARGIN_X,64); display.print("AP: "); display.print(WIFI_AP_SSID);
      display.setCursor(MARGIN_X,84); display.print("Pass: "); display.print(WIFI_AP_PASS);
      display.setCursor(MARGIN_X,104); display.print("IP: "); display.print(WifiMgr::apIP());
      display.setCursor(MARGIN_X,124); display.print("ESP-NOW remote active (ch6)");
      return;
    }
    if(m==WIFI_REMOTE_ONLY){ display.setCursor(MARGIN_X,70); display.print("Remote only — ch 6"); display.setCursor(MARGIN_X,92); display.print("Web/AP disabled."); return; }
    // STA_WEB
    if(WifiMgr::connected()){ display.setCursor(MARGIN_X,56); display.print("Connected: "+WifiMgr::ip()); }
    else { display.setCursor(MARGIN_X,56); display.print("Select a network:"); }
    if(WifiMgr::count()==0){ display.setCursor(MARGIN_X,80); display.print("No networks found."); return; }
    const int per=9; int top=0; if(wifiCur>=per) top=wifiCur-per+1;
    for(int i=0;i<per&&top+i<WifiMgr::count();i++){ int idx=top+i; int y=80+i*21; bool sel=(idx==wifiCur);
      if(sel){ display.fillRoundRect(MARGIN_X-4,y-14,EPD_W-2*MARGIN_X+8,19,3,COL_FG); display.setTextColor(COL_BG); }
      else display.setTextColor(COL_FG);
      display.setCursor(MARGIN_X,y); display.print(WifiMgr::ssid(idx).substring(0,26));
      if(WifiMgr::secure(idx)){ display.setCursor(EPD_W-MARGIN_X-8,y); display.print("*"); } }
  }
  static const int WIFI_TOP=36, WIFI_H=250;
  void drawNetwork(){
    Disp::full([&]{
      Disp::border(); Disp::header("NETWORK");
      drawWifiBody(); footer();
    });
  }
  static void drawWifiPartial(){ Disp::partial(0,WIFI_TOP,EPD_W,WIFI_H,drawWifiBody); }

  static void drawSettingsBody(){
    const char* items[]={"Wireless","Invert","Font size","Auto-sleep","Sleep now"};
    for(int i=0;i<5;i++){ int y=58+i*28; bool sel=(i==setCur);
      display.setFont(FONT_UI);
      if(sel){ display.fillRoundRect(MARGIN_X-4,y-16,EPD_W-2*MARGIN_X+8,24,4,COL_FG); display.setTextColor(COL_BG); }
      else display.setTextColor(COL_FG);
      display.setCursor(MARGIN_X,y); display.print(items[i]);
      if(i==0){ String v=WifiMgr::modeName(WifiMgr::mode()); int vw=TW(v); display.setCursor(EPD_W-MARGIN_X-vw-8,y); display.print(v); }
      else if(i==1){ display.setCursor(EPD_W-MARGIN_X-36,y); display.print(Store::inverted()?"[on]":"[off]"); }
      else if(i==2){ const char* v=TSize::label(Store::textSize()); int vw=TW(v); display.setCursor(EPD_W-MARGIN_X-vw-8,y); display.print(v); }
      else if(i==3){ const char* v=sleepLbl[sleepSel]; int vw=TW(v); display.setCursor(EPD_W-MARGIN_X-vw-8,y); display.print(v); }
    }
  }
  static const int SET_TOP=40, SET_H=160;
  void drawSettings(){ Disp::full([&]{ Disp::border(); Disp::header("SETTINGS"); drawSettingsBody(); footer(); }); }
  static void drawSettingsPartial(){ Disp::partial(0,SET_TOP,EPD_W,SET_H,drawSettingsBody); }

  static void drawWirelessBody(){
    const char* names[]={"Off","AP + Remote + Web","Remote Only","STA + Web"};
    uint8_t cur=WifiMgr::mode();
    for(int i=0;i<4;i++){ int y=70+i*30; bool sel=(i==wirelessCur);
      display.setFont(FONT_UI);
      if(sel){ display.fillRoundRect(MARGIN_X-4,y-16,EPD_W-2*MARGIN_X+8,24,4,COL_FG); display.setTextColor(COL_BG); }
      else display.setTextColor(COL_FG);
      display.setCursor(MARGIN_X,y); display.print(names[i]);
      if((uint8_t)i==cur){ display.setCursor(EPD_W-MARGIN_X-18,y); display.print("x"); }
    }
    display.setFont(FONT_META); display.setTextColor(COL_FG);
    display.setCursor(MARGIN_X,210); display.print("SEL applies · BACK cancels");
  }
  static const int WLESS_TOP=40, WLESS_H=200;
  void drawWireless(){ Disp::full([&]{ Disp::border(); Disp::header("WIRELESS MODE"); drawWirelessBody(); footer(); }); }
  static void drawWirelessPartial(){ Disp::partial(0,WLESS_TOP,EPD_W,WLESS_H,drawWirelessBody); }

  void drawAbout(){
    Disp::full([&]{
      Disp::border(); Disp::header("ABOUT");
      display.setFont(FONT_META); display.setTextColor(COL_FG); int y=64;
      auto row=[&](const char* k,const String& v){ char b[80]; snprintf(b,sizeof(b),"%-8s %s",k,v.c_str()); display.setCursor(MARGIN_X,y); display.print(b); y+=22; };
      row("Flash", String((unsigned long)ESP.getFlashChipSize()/(1024*1024))+" MB");
      row("PSRAM", String(ESP.getPsramSize()/(1024*1024))+" MB");
      row("Heap", String(ESP.getFreeHeap()/1024)+"k");
      row("SD", Store::sdReady?"mounted":"absent");
      row("Wireless", WifiMgr::modeLabel());
      row("Panel","GDEY042T81"); row("Build","REread. 9.0");
      footer();
    });
  }

  const char* screenName(){ switch(screen){
    case HOME:return "Home"; case FILES:return "Library"; case READER:return "Reader";
    case CLOCK:return "Clock"; case NETWORK:return "Network"; case SETTINGS:return "Settings";
    case WIRELESS:return "Wireless"; default:return "About"; } }

  void redraw(){
    switch(screen){
      case HOME:drawHome();break; case FILES:drawFiles();break;
      case READER:Reader::draw();break; case CLOCK:drawClock();break;
      case NETWORK:drawNetwork();break; case SETTINGS:drawSettings();break;
      case WIRELESS:drawWireless();break; case ABOUT:drawAbout();break;
    }
  }
  static void openBook(const String& p){
    if(Reader::open(p)){ screen=READER; redraw(); }
    else Disp::full([&]{ display.setFont(FONT_UI); display.setTextColor(COL_FG); display.setCursor(MARGIN_X,80); display.print("Cannot open"); });
  }

  // Finish an on-screen keyboard session. Returns true if it handled the cmd.
  static bool handleKbd(uint8_t cmd){
    if(!Kbd::open) return false;
    if(Kbd::handle(cmd)){
      Kbd::DoneAction act=Kbd::doneAct; String val=Kbd::buf;
      Kbd::open=false;
      if(act==Kbd::ACT_WIFI){
        WifiMgr::connect(WifiMgr::ssid(wifiCur),val);
        screen=NETWORK; redraw();
      } else if(act==Kbd::ACT_GOTO){
        int p=val.toInt()-1; Reader::goTo(p); Reader::state=Reader::RState::READING;
        screen=READER; redraw();
      } else {
        if(Kbd::target) *Kbd::target=val;
        redraw();
      }
    }
    return true;
  }

  void command(uint8_t cmd){
    poke();
    if(cmd==CMD_THEME){ Store::setInverted(!Store::inverted()); redraw(); return; }
    if(cmd==CMD_SLEEP){ Disp::sleep(); Input::setAwake(false); return; }
    // Any non-sleep/non-ping button implicitly wakes the panel before acting.
    if(!Input::isAwake() && cmd!=CMD_PING){
      Disp::wake(); Input::setAwake(true);
    }
    if(cmd==CMD_WAKE){ redraw(); return; }
    if(cmd==CMD_PING){ Input::ack(CMD_PING); return; }

    if(handleKbd(cmd)) return;

    if(screen==READER){
      if(Reader::state==Reader::RState::MENU){
        if(cmd==CMD_UP&&Reader::menuCur>0) Reader::menuCur--;
        else if(cmd==CMD_DOWN&&Reader::menuCur<2) Reader::menuCur++;
        else if(cmd==CMD_SELECT){
          if(Reader::menuCur==0){
            if(Reader::mode==Reader::Mode::TXT){ Reader::state=Reader::RState::HIGHLIGHT; Reader::buildWords(); }
            else { Reader::state=Reader::RState::READING; }
          } else if(Reader::menuCur==1){
            Reader::state=Reader::RState::GOTO; Reader::gotoBuf="";
          } else Reader::state=Reader::RState::READING;
        }
        else if(cmd==CMD_BACK) Reader::state=Reader::RState::READING;
        Reader::draw(); return;
      }
      if(Reader::state==Reader::RState::HIGHLIGHT){
        int n=(int)Reader::words.size();
        if(cmd==CMD_RIGHT&&Reader::hlCur<n-1) Reader::hlCur++;
        else if(cmd==CMD_LEFT&&Reader::hlCur>0) Reader::hlCur--;
        else if(cmd==CMD_SELECT){ if(n) Reader::hl[Reader::hlCur]=!Reader::hl[Reader::hlCur]; }
        else if(cmd==CMD_BACK){ Reader::state=Reader::RState::READING; }
        else if(cmd==CMD_HOME){ Reader::state=Reader::RState::READING; Reader::reset(); screen=HOME; }
        if(Reader::state==Reader::RState::HIGHLIGHT) Reader::drawOverlay();
        else redraw(); return;
      }
      if(Reader::state==Reader::RState::GOTO){
        if(!Kbd::open){
          if(cmd==CMD_SELECT){ Kbd::open_(Reader::gotoBuf,true,Kbd::ACT_GOTO,nullptr); Kbd::draw(); return; }
          if(cmd==CMD_BACK){ Reader::state=Reader::RState::READING; redraw(); return; }
          if(cmd==CMD_HOME){ Reader::state=Reader::RState::READING; Reader::reset(); screen=HOME; redraw(); return; }
        }
        Reader::draw(); return;
      }
      // normal reading
      if(cmd==CMD_RIGHT||cmd==CMD_DOWN){ Reader::next(); redraw(); }
      else if(cmd==CMD_LEFT||cmd==CMD_UP){ Reader::prev(); redraw(); }
      else if(cmd==CMD_SELECT){ Reader::menuCur=0; Reader::state=Reader::RState::MENU; Reader::draw(); }
      else if(cmd==CMD_ZOOM_IN){ Reader::zoomIn(); redraw(); }
      else if(cmd==CMD_ZOOM_OUT){ Reader::zoomOut(); redraw(); }
      else if(cmd==CMD_BACK||cmd==CMD_HOME){ Reader::reset(); screen=HOME; redraw(); }
      return;
    }

    switch(screen){
      case HOME:{
        bool moved=false;
        if(cmd==CMD_UP&&menuCur>0){ menuCur--; moved=true; }
        else if(cmd==CMD_DOWN&&menuCur<5){ menuCur++; moved=true; }
        else if(cmd==CMD_SELECT){
          switch(menuCur){
            case 0:{ String b=Store::lastBook(); if(b.length()&&SD.exists(b)) openBook(b); else { screen=FILES; listFolder("/"); redraw(); } break; }
            case 1: screen=FILES; listFolder("/"); redraw(); break;
            case 2: screen=NETWORK; if(WifiMgr::mode()==WIFI_STA_WEB){ WifiMgr::scan(); } redraw(); break;
            case 3: screen=CLOCK; redraw(); break;
            case 4: screen=SETTINGS; redraw(); break;
            case 5: screen=ABOUT; redraw(); break;
          }
          return;
        }
        if(moved) drawHomePartial(); else drawHome(); break;
      }
      case FILES:{
        bool moved=false;
        if(cmd==CMD_UP&&cursor>0){ cursor--; moved=true; }
        else if(cmd==CMD_DOWN&&cursor<(int)files.size()-1){ cursor++; moved=true; }
        else if(cmd==CMD_BACK){ int sl=cwd.lastIndexOf('/',cwd.length()-2);
          if(sl>=0&&cwd!="/"){ cwd=cwd.substring(0,sl+1); listFolder(cwd); redraw(); } else { screen=HOME; redraw(); } return; }
        else if(cmd==CMD_HOME){ screen=HOME; redraw(); return; }
        else if(cmd==CMD_SELECT){ if(cursor<(int)files.size()){
            String p=cwd; if(!p.endsWith("/")) p+="/"; p+=files[cursor].name;
            if(files[cursor].dir){ listFolder(p); redraw(); } else openBook(p); } return; }
        if(moved) drawFilesPartial(); else drawFiles(); break;
      }
      case NETWORK:{
        int n=WifiMgr::count();
        if(WifiMgr::mode()!=WIFI_STA_WEB){
          if(cmd==CMD_BACK||cmd==CMD_HOME||cmd==CMD_SELECT){ screen=HOME; redraw(); }
          else drawNetwork();
          break;
        }
        if(cmd==CMD_UP&&wifiCur>0){ wifiCur--; drawWifiPartial(); }
        else if(cmd==CMD_DOWN&&wifiCur<n-1){ wifiCur++; drawWifiPartial(); }
        else if(cmd==CMD_SELECT&&n>0){
          if(WifiMgr::secure(wifiCur)){ Kbd::open_("",false,Kbd::ACT_WIFI,nullptr); Kbd::draw(); }
          else { WifiMgr::connect(WifiMgr::ssid(wifiCur),""); redraw(); }
        }
        else if(cmd==CMD_BACK||cmd==CMD_HOME){ screen=HOME; redraw(); }
        break;
      }
      case WIRELESS:{
        bool moved=false;
        if(cmd==CMD_UP&&wirelessCur>0){ wirelessCur--; moved=true; }
        else if(cmd==CMD_DOWN&&wirelessCur<3){ wirelessCur++; moved=true; }
        else if(cmd==CMD_SELECT){
          WifiMgr::applyMode((uint8_t)wirelessCur);
          screen=SETTINGS; redraw(); return;
        }
        else if(cmd==CMD_BACK||cmd==CMD_HOME){ screen=SETTINGS; redraw(); return; }
        if(moved) drawWirelessPartial(); else drawWireless(); break;
      }
      case CLOCK: if(cmd==CMD_BACK||cmd==CMD_HOME||cmd==CMD_SELECT){ screen=HOME; redraw(); } else drawClock(); break;
      case SETTINGS:{
        bool moved=false;
        if(cmd==CMD_UP&&setCur>0){ setCur--; moved=true; }
        else if(cmd==CMD_DOWN&&setCur<4){ setCur++; moved=true; }
        else if(cmd==CMD_LEFT||cmd==CMD_RIGHT){
          if(setCur==0){ screen=WIRELESS; wirelessCur=WifiMgr::mode(); redraw(); return; }
          if(setCur==1) Store::setInverted(!Store::inverted());
          else if(setCur==2){ int s=Store::textSize(); s+=(cmd==CMD_RIGHT)?1:-1; s=std::min(3,std::max(0,s)); Store::setTextSize(s); }
          else if(setCur==3){ sleepSel+=(cmd==CMD_RIGHT)?1:-1; sleepSel=std::min(3,std::max(0,sleepSel)); Store::saveSleepMin(sleepMins[sleepSel]); }
          redraw(); return;
        }
        else if(cmd==CMD_SELECT){
          if(setCur==0){ screen=WIRELESS; wirelessCur=WifiMgr::mode(); redraw(); return; }
          if(setCur==1) Store::setInverted(!Store::inverted());
          else if(setCur==2){ int s=Store::textSize(); Store::setTextSize((s+1)%4); }
          else if(setCur==3){ sleepSel=(sleepSel+1)%4; Store::saveSleepMin(sleepMins[sleepSel]); }
          else if(setCur==4){ Disp::sleep(); Input::setAwake(false); return; }
          redraw(); return;
        }
        else if(cmd==CMD_BACK||cmd==CMD_HOME){ screen=HOME; redraw(); return; }
        if(moved) drawSettingsPartial(); else drawSettings(); break;
      }
      case ABOUT: if(cmd==CMD_BACK||cmd==CMD_HOME||cmd==CMD_SELECT){ screen=HOME; redraw(); } else drawAbout(); break;
      default: break;
    }
  }

  void loop(){
    WifiMgr::maybeSyncTime();
    if(screen==CLOCK && millis()-lastClockDraw>20000){ drawClock(); }
    uint8_t sm=Store::sleepMin();
    if(sm>0 && Input::isAwake() && (millis()-lastActivity > (unsigned long)sm*60000UL)){
      Disp::sleep(); Input::setAwake(false);
    }
  }
}

// ── Boot (loading bar, mostly partial) ────────────────────────────────────
static void bootScreen(){
  const char* stages[]={"init","storage","radio","apps","ready"};
  int n=5;
  // First frame is a full clear so the panel starts clean.
  Disp::full([&]{
    Disp::border();
    display.setFont(FONT_LOGO); display.setTextColor(COL_FG);
    int16_t x1,y1; uint16_t w,h; display.getTextBounds(BRAND,0,0,&x1,&y1,&w,&h);
    display.setCursor((EPD_W-(int)w)/2-x1, 120-y1); display.print(BRAND);
    display.setFont(FONT_UI);
    const char* sub="e-paper reader"; int sw=TW(sub);
    display.setCursor((EPD_W-sw)/2,150); display.print(sub);
    int bx=70,by=180,bw=EPD_W-140,bh=12;
    display.drawRoundRect(bx,by,bw,bh,6,COL_FG);
  });
  for(int i=1;i<=n;i++){
    Disp::partial(60,172,EPD_W-120,40,[&]{
      int bx=70,by=180,bw=EPD_W-140,bh=12;
      display.drawRoundRect(bx,by,bw,bh,6,COL_FG);
      int fw=(bw*i)/n;
      if(fw>2) display.fillRoundRect(bx+2,by+2,fw-2,bh-4,4,COL_FG);
      display.setFont(FONT_META); display.setTextColor(COL_FG);
      display.setCursor(bx,by+bh+16); display.print(stages[i-1]);
    });
    delay(90);
  }
}

void setup(){
  Serial.begin(115200); delay(40);
  Serial.println(); Serial.println(F(BRAND));
  Disp::begin();
  Store::begin();
  if(Store::inverted()) Store::setInverted(true);
  bootScreen();
  WifiMgr::begin();          // also brings up Input + WebSrv per mode
  UI::begin();
  UI::redraw();
}

void loop(){
  if(Input::available()){ RemoteCmd c=Input::get();
    if(c==CMD_PING) Input::ack(CMD_PING); else UI::command(c); }
  if(g_webCmd!=CMD_NONE){ uint8_t c=g_webCmd; g_webCmd=CMD_NONE; UI::command(c); }
  WebSrv::loop();
  UI::loop();
  delay(8);
}
