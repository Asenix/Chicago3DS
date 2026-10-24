#define main game_main
#include "../../source/main.c"
#undef main
#include <stdlib.h>
static int fails=0;
#define CHECK(c,...) do{ if(!(c)){ fails++; printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); } }while(0)

// BFS over the outdoor world on a 0.5 m grid, player radius
static unsigned char* seen; static int NX;
static int gi(float v){ return (int)floorf((v+CITY_H+30.0f)*2.0f); }
static void bfsWorld(float sx,float sz){
  NX=(int)((CITY_H*2+60.0f)*2.0f); seen=calloc((size_t)NX*NX,1);
  int* q=malloc(sizeof(int)*(size_t)NX*NX); int h=0,t=0; q[t++]=gi(sx)*NX+gi(sz); seen[q[0]]=1;
  static const int dx[4]={1,-1,0,0}, dz[4]={0,0,1,-1};
  while(h<t){ int c=q[h++]; int ci=c/NX, cj=c%NX;
    for(int k=0;k<4;k++){ int ni=ci+dx[k], nj=cj+dz[k]; if(ni<0||nj<0||ni>=NX||nj>=NX) continue; if(seen[ni*NX+nj]) continue;
      float x=(float)ni*0.5f-CITY_H-30.0f, z=(float)nj*0.5f-CITY_H-30.0f;
      if(blockedR(x,z,PLAYER_R)) continue;
      seen[ni*NX+nj]=1; q[t++]=ni*NX+nj; } }
  free(q);
}
// BFS inside a room (gRoom set)
static unsigned char rseen[400*400];
static void bfsRoom(const Room* m){
  memset(rseen,0,sizeof rseen); int W=(int)((m->x1-m->x0)*4)+2, H=(int)((m->z1-m->z0)*4)+2; if(W>399)W=399; if(H>399)H=399;
  int* q=malloc(sizeof(int)*400*400); int h=0,t=0;
  int si=(int)((m->spX-m->x0)*4), sj=(int)((m->spZ-m->z0)*4); q[t++]=si*400+sj; rseen[q[0]]=1;
  static const int dx[4]={1,-1,0,0}, dz[4]={0,0,1,-1};
  while(h<t){ int c=q[h++]; int ci=c/400,cj=c%400;
    for(int k=0;k<4;k++){ int ni=ci+dx[k], nj=cj+dz[k]; if(ni<0||nj<0||ni>=W||nj>=H) continue; if(rseen[ni*400+nj]) continue;
      float x=m->x0+(float)ni*0.25f, z=m->z0+(float)nj*0.25f; if(blockedR(x,z,PLAYER_R)) continue; rseen[ni*400+nj]=1; q[t++]=ni*400+nj; } }
  free(q);
}
static int rreach(const Room* m,float x,float z,float tol){ // any visited cell within tol
  for(float dx=-tol;dx<=tol;dx+=0.25f) for(float dz=-tol;dz<=tol;dz+=0.25f){ if(dx*dx+dz*dz>tol*tol) continue; int i=(int)((x+dx-m->x0)*4), j=(int)((z+dz-m->z0)*4); if(i>=0&&j>=0&&i<400&&j<400&&rseen[i*400+j]) return 1; }
  return 0;
}
static float px,pz;
static void frame(u32 down,int canAct){ updateTrain(); updateWorld(px,pz,down,canAct,0); if(gTraffic&&gRoom<0) updateWildlife(px,pz);
  if(gTele){ px=gTeleX; pz=gTeleZ; gTele=0; } }
int main(void){
  vbo=(Vertex*)malloc(sizeof(Vertex)*MAX_VERTS); buildCity(); staticCount=vcount; initNav(); initCars(); initPeds(); initCrates(); initWorld();
  setupMode(M_FREE);
  printf("doors %d  static verts %d / %d\n",nDoor,staticCount,MAX_VERTS);
  // 1. every door is reachable on foot from the spawn
  bfsWorld(0.0f,0.0f);
  CHECK(!blockedR(0.0f,0.0f,PLAYER_R),"spawn blocked");
  int unreach=0;
  for(int i=0;i<nDoor;i++){
    const Door* d=&doors[i];
    int ok=seen[gi(d->x)*NX+gi(d->z)];
    float ex=d->x+d->fx*0.6f, ez=d->z+d->fz*0.6f;
    int ok2=seen[gi(ex)*NX+gi(ez)];
    if(!ok||!ok2){ unreach++; printf("door %d room %d at %.1f %.1f unreachable (stand %d exit %d)\n",i,d->room,d->x,d->z,ok,ok2); }
  }
  CHECK(unreach==0,"%d doors unreachable",unreach);
  int per[RM_N]={0}; for(int i=0;i<nDoor;i++) per[doors[i].room]++;
  printf("doors per room:"); for(int k=0;k<RM_N;k++) printf(" %s=%d",roomName[k],per[k]); printf("\n");
  for(int k=0;k<RM_N;k++) CHECK(per[k]>0,"no door for room %d",k);
  // cash reachable
  int cashBad=0; for(int i=0;i<N_CASH;i++) if(cashP[i].active && !seen[gi(cashP[i].x)*NX+gi(cashP[i].z)]) cashBad++;
  CHECK(cashBad==0,"%d cash pickups unreachable",cashBad);
  // 2. rooms: reach service spot, exit, crates from the entry point
  for(int k=0;k<RM_N;k++){
    gRoom=k; const Room* m=&rooms[k]; bfsRoom(m);
    CHECK(!blockedR(m->spX,m->spZ,PLAYER_R),"room %d spawn blocked",k);
    CHECK(rreach(m,m->aX,m->aZ,2.2f),"room %d service spot unreachable",k);
    CHECK(rreach(m,m->exX,m->exZ,1.9f),"room %d exit unreachable",k);
    for(int c=0;c<m->nCrate;c++) CHECK(rreach(m,m->crate[c][0],m->crate[c][1],1.3f),"room %d crate %d unreachable",k,c);
    printf("room %d %-15s ok  walk %.1fx%.1f solids %d verts %d\n",k,roomName[k],m->x1-m->x0,m->z1-m->z0,m->nSol,m->count);
    gRoom=-1;
  }
  // 3. full enter / shop / leave cycle for every door
  int cycles=0;
  for(int i=0;i<nDoor;i++){
    gCash=500; gShop=-1; gRoom=-1; gTrans=0; px=doors[i].x; pz=doors[i].z;
    frame(0,1); CHECK(gPrompt==1,"door %d: no enter prompt (%d)",i,gPrompt);
    frame(KEY_A,1); CHECK(gTrans>0,"door %d: no fade started",i);
    for(int f=0;f<40;f++) frame(0,1);
    CHECK(gRoom==doors[i].room,"door %d: in room %d expected %d",i,gRoom,doors[i].room); if(gRoom<0) continue;
    const Room* m=&rooms[gRoom];
    CHECK(fabsf(px-m->spX)<0.01f&&fabsf(pz-m->spZ)<0.01f,"door %d: not at room spawn",i);
    // walk to the service spot and use it
    px=m->aX; pz=m->aZ; frame(0,1); CHECK(gPrompt==3,"door %d: no service prompt",i);
    frame(KEY_A,1); CHECK(gShop==gRoom,"door %d: shop did not open",i);
    int n; const ShopItem* it=shopItems(gShop,&n);
    if(gRoom!=RM_APT && gRoom!=RM_STATION){
      int before=gCash; for(int j=0;j<n;j++) shopBuy(gRoom,j); CHECK(gCash<=before,"door %d: cash grew from shop",i);
      gCash=0; int c0=gCash; shopBuy(gRoom,0); CHECK(gCash==c0,"door %d: bought with no cash",i);
    }
    if(gRoom==RM_APT){ int w0=weather; shopBuy(gRoom,2); CHECK(gTrans>0,"apt: no rest fade"); for(int f=0;f<40;f++) frame(0,1); (void)w0; }
    else if(gRoom==RM_STATION){ gCash=500; int rd=gRetDoor; shopBuy(gRoom,0); CHECK(gCash==495,"ticket price (%d)",gCash); for(int f=0;f<40;f++) frame(0,1); CHECK(gRoom==-1,"ride: still inside"); CHECK(!blockedR(px,pz,PLAYER_R),"ride: arrival blocked"); (void)rd; cycles++; continue; }
    gShop=-1;
    if(gRoom>=0){
      px=m->exX; pz=m->exZ; frame(0,1); CHECK(gPrompt==2,"door %d: no exit prompt (%d)",i,gPrompt);
      frame(KEY_A,1); for(int f=0;f<40;f++) frame(0,1);
      CHECK(gRoom==-1,"door %d: did not exit",i);
      CHECK(!blockedR(px,pz,PLAYER_R),"door %d: exit position blocked (%.1f %.1f)",i,px,pz);
      CHECK(fabsf(px-doors[i].x)<2.0f&&fabsf(pz-doors[i].z)<2.0f,"door %d: exit far from door",i);
    }
    cycles++;
  }
  printf("enter/shop/leave cycles ok: %d / %d\n",cycles,nDoor);
  // 4. shop item behaviours
  gRoom=-1; gCash=100; gShades=0; gCap=0; gJacketIx=0; gBoost=0;
  shopBuy(RM_STORE,0); CHECK(gBoost==90*60,"energy drink boost %d",gBoost);
  shopBuy(RM_STORE,3); CHECK(gShades==1,"shades"); int c1=gCash; shopBuy(RM_STORE,3); CHECK(gCash==c1,"shades bought twice");
  shopBuy(RM_STORE,2); CHECK(gCap==1,"cap");
  shopBuy(RM_LAUNDRY,1); CHECK(gJacketIx==2,"jacket");
  // 5. train
  trX=-300; trDir=1; int stopped[2]={0,0}; float mn=1e9,mx=-1e9; int dwellFrames=0;
  for(int f=0;f<60*60*8;f++){ updateTrain(); if(!(trX==trX)) {CHECK(0,"train NaN"); break;} if(trX<mn)mn=trX; if(trX>mx)mx=trX; if(trAt>=0 && trDwell>0){ stopped[trAt]=1; dwellFrames++; } }
  printf("train range %.0f..%.0f stations stopped %d %d  dwell frames %d\n",mn,mx,stopped[0],stopped[1],dwellFrames);
  CHECK(stopped[0]&&stopped[1],"train did not stop at both stations"); CHECK(mn>-440&&mx<440,"train out of range");
  // 6. wildlife
  int bad=0; px=0; pz=0;
  for(int f=0;f<60*120;f++){ updateWildlife(px,pz); }
  for(int a=0;a<N_ANIM;a++){ Animal* an=&anim[a]; if(!(an->x==an->x)||!(an->z==an->z)) bad++; else if(an->state!=AS_FLY && blockedR(an->x,an->z,0.25f)) bad++; }
  CHECK(bad==0,"%d animals NaN or inside obstacles",bad);
  // 7. per-frame dynamic geometry budget at a spread of positions
  int worst=0;
  for(int t=0;t<40;t++){ float cx=(float)((t*37)%500)-250.0f, cz=(float)((t*91)%500)-250.0f; gCamX=cx; gCamZ=cz; gCullR=90; gCone=0; vcount=staticCount; drawDynamic(); drawWorldDyn(t*7); drawWorldLit(t*7,1.0f); int d=vcount-staticCount; if(d>worst) worst=d; }
  printf("worst dynamic verts/frame %d (room %d)\n",worst,MAX_VERTS-staticCount);
  CHECK(worst<MAX_VERTS-staticCount,"dynamic overflow");
  CHECK(gVtxOver==0,"%u vertices refused because the buffer was full",gVtxOver);
  printf("%s (%d fails)\n",fails?"FAILED":"ALL OK",fails);
  return fails?1:0;
}
