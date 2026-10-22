#define main game_main
#include "../../source/main.c"
#undef main
#include <stdlib.h>
#include <time.h>
u64 osGetTime(void){ static u64 t=777; return t+=1234; }
static void init(void){ vbo=(Vertex*)malloc(sizeof(Vertex)*MAX_VERTS); buildCity(); staticCount=vcount; initNav(); initCars(); initPeds(); initCrates(); }
int main(int argc,char**argv){
  init();
  int open=0; for(int i=0;i<NAV_N*NAV_N;i++) open+=navOpen[i];
  printf("nav open cells %d of %d\n",open,NAV_N*NAV_N);
  // A* validity: random open pairs
  int ok=0,fail=0,bad=0; unsigned t0=clock();
  for(int k=0;k<300;k++){
    float ax,az,bx,bz; pickSpot(&ax,&az,0,0,0,999,0); pickSpot(&bx,&bz,0,0,0,999,0);
    short cx[600],cz[600]; int n=navPath(ax,az,bx,bz,cx,cz,PATH_N>0?600:0);
    if(n<=0){fail++;continue;}
    int reach = (abs(cx[n-1]-navCell(bx))<=1 && abs(cz[n-1]-navCell(bz))<=1);
    // each step adjacent and open
    for(int i=0;i<n;i++){ if(!navOpen[cz[i]*NAV_N+cx[i]]) bad++; if(i&&(abs(cx[i]-cx[i-1])>1||abs(cz[i]-cz[i-1])>1)) bad++; }
    if(reach) ok++; else fail++;
  }
  printf("A*: %d reached, %d not, %d bad steps, %.2f ms avg\n",ok,fail,bad,(clock()-t0)*1000.0/CLOCKS_PER_SEC/300);
  int mode=argc>1?atoi(argv[1]):1; int frames=argc>2?atoi(argv[2]):20000;
  setupMode(mode);
  printf("mode %d start (%.1f,%.1f) bots %d loot:",mode,gPlX,gPlZ,botsAlive());
  int nl=0; for(int i=0;i<N_LOOT;i++) nl+=loot[i].active; printf(" %d\n",nl);
  // run countdown
  int f=0; while(gState==GS_COUNTDOWN){updateMode(gPlX,gPlZ,0);f++;}
  printf("countdown took %d frames\n",f);
  int firstHit=-1,lastLevel=0,lastAlive=-1;
  float px=gPlX,pz=gPlZ;
  for(f=0;f<frames && gState==GS_PLAY;f++){
    // the "player" runs around randomly and shoots nearest bot (cheats a bit) for BR
    if(mode==M_DM && f%50==0 && gKills<60){ // kill nearest bot every so often to test levelling
      int bi=-1; float bd=1e9; for(int i=0;i<MAX_BOTS;i++) if(bots[i].active&&!bots[i].dead){float d=hypotf(bots[i].x-px,bots[i].z-pz); if(d<bd){bd=d;bi=i;}}
      if(bi>=0 && f>400) damageBot(bi,200,-1);
    }
    if(argc>3) plHp=1e9; updateMode(px,pz,0);
    int a=botsAlive();
    if(mode==M_BR && f%1200==0){int ng=0,nm=0,nh=0;for(int i=0;i<MAX_BOTS;i++)if(bots[i].active&&!bots[i].dead){for(int w=1;w<WP_COUNT;w++)ng+=bots[i].own[w]?1:0;nm+=bots[i].medkits;nh+=(int)bots[i].hp;} printf("  t=%ds alive=%d zone R=%.0f phase %d guns %d medkits %d sumhp %d\n",f/60,a,zR,zPhase,ng,nm,nh);}
    if(mode==M_DM && gLevel!=lastLevel){lastLevel=gLevel;printf("  level %d at kills %d (t=%ds) alive bots %d hp %.0f\n",gLevel,gKills,f/60,a,plHp);}
    if(firstHit<0 && plHp<100){firstHit=f;printf("  first damage to player at %.1fs\n",f/60.0);}
  }
  printf("end: state %d won %d place %d kills %d level %d hp %.0f frames %d\n",gState,gWon,gPlace,gKills,gLevel,plHp,f);
  return 0;
}
