#define main game_main
#include "../../source/main.c"
#undef main
#include <stdlib.h>
u64 osGetTime(void){ static u64 t=777; return t+=1234; }
// usage: sim3 mode frames out.bin camx camy camz yaw pitch   (camera <0 values: auto = follow player)
int main(int argc,char**argv){
  vbo=(Vertex*)malloc(sizeof(Vertex)*MAX_VERTS); buildCity(); staticCount=vcount; initNav(); initCars(); initPeds(); initCrates();
  int mode=atoi(argv[1]), frames=atoi(argv[2]);
  setupMode(mode);
  int f=0;
  while(f<frames){
    plHp=1e9f;
    // TP: the "player" fires 3 shots on its turn aiming near the centre
    if(mode==M_TP && gState==GS_PLAY && tpTurn==TP_PL && tpPhase==1 && tpShotsLeft>0 && fireCool--<=0){
      float cam[3]={gPlX,1.62f,gPlZ}; float fwd[3]={0.002f*(rnd()),(0.38f+0.1f*rnd())/17.0f,-1}; float muz[3]={gPlX,1.4f,gPlZ-0.9f};
      float l=sqrtf(fwd[0]*fwd[0]+fwd[1]*fwd[1]+1); for(int i=0;i<3;i++) fwd[i]/=l;
      tpPlayerShot(cam,fwd,muz); fireCool=20;
    }
    updateMode(gPlX,gPlZ,0); updateFx(); f++;
    if(gState==GS_OVER) break;
  }
  printf("mode %d frames %d state %d  tpScores %d %d %d %d round %d | alive bots %d\n",mode,f,gState,tpScore[0],tpScore[1],tpScore[2],tpScore[3],tpRound,botsAlive());
  if(mode==M_TP) printf("result won %d draw %d place %d\n",gWon,gDraw,gPlace);
  gCamX=gPlX; gCamZ=gPlZ; gCullR=500; gCone=0;
  drawDynamic();
  if(mode==M_TP) drawTpRoom(10); else if(mode!=M_FREE) drawBots();
  int lit=vcount; drawLoot(10); drawStorm();
  printf("dynamic verts %d (room for %d) loot+storm %d\n",lit-staticCount,MAX_VERTS-staticCount,vcount-lit);
  if(argc>3){FILE*o=fopen(argv[3],"wb"); fwrite(vbo,sizeof(Vertex),vcount,o); fclose(o);}
  printf("player at %.1f %.1f\n",gPlX,gPlZ); for(int i=0;i<MAX_BOTS;i++) if(bots[i].active) {printf("bot %d at %.1f %.1f face %.2f gun %d\n",i,bots[i].x,bots[i].z,bots[i].face,bots[i].gun); break;} for(int i=0;i<N_LOOT;i++) if(loot[i].active){printf("loot %d kind %d at %.1f %.1f\n",i,loot[i].kind,loot[i].x,loot[i].z);break;}
  return 0;
}
