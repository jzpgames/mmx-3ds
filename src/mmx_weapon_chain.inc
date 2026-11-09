/* X2 $81:9816..9C79 / $81:A6A4..A8D9. Nine original link actors are
 * reconstructed by the renderer; only the hook participates in collision.
 * variant: startup, extending, retracting, wall anchor, carried pickup.
 * vy: held X1 item address; radius: extension timer or held item kind;
 * tether_pose: original link animation clock; muzzle_pose: pending contact. */
static bool chain_item(unsigned d) {
  return d>=0x1628 && d<0x1928 && (d-0x1628)%0x30==0;
}
static void chain_release(uint8_t *r,const MmxWeaponShot *s) {
  unsigned item=(uint16_t)s->vy;
  if (chain_item(item) && r[item] && r[item+10]==s->radius) r[item+0x28]=0;
  if (s->variant==3) putword(r+0xbc4,0);
}
static void chain_spend(unsigned amount) {
  unsigned energy=MmxWeaponsEnergyAmount(1,6);
  MmxWeaponsSpend(1,6,energy<amount ? energy : amount);
}
static void chain_contact(uint8_t *r,MmxWeaponShot *s,unsigned enemy,unsigned damage) {
  bool killed=damage>=(r[enemy+0x27]&127);
  chain_spend(s->charged ? 16 : 128);
  if (!s->charged) { s->muzzle_pose=killed ? 1 : 2;return; }
  if (!killed || r[enemy+0x28]>=6) return;
  /* X2 $86:B0CE, drop row 7: 50% small health / 50% small energy.
   * Allocate a real X1 item, so its collection, refill and lifetime remain
   * native. Source RNG arithmetic is seeded from saved simulation state. */
  for (unsigned item=0x1628;item<0x1928;item+=0x30) if (!word(r+item)) {
    memset(r+item,0,0x30);r[item]=1;
    r[item+10]=bubble_random(enemy+s->born)<128 ? 1 : 2;r[item+11]=1;
    putword(r+item+5,s->x>>8);putword(r+item+8,s->y>>8);break;
  }
}
static bool chain_grab(uint8_t *r,MmxWeaponShot *s) {
  int reach=s->charged ? 8 : 5;
  for (unsigned item=0x1628;item<0x1928;item+=0x30) {
    unsigned kind=r[item+10];
    if (!r[item] || r[item+1]!=2 || r[item+2]>=6 || r[item+0x28] ||
        (kind!=1 && kind!=2 && kind!=4)) continue;
    int dx=(int)word(r+item+5)-(s->x>>8),dy=(int)word(r+item+8)-(s->y>>8);
    /* Original small/large item collision radii are 6/8. */
    int half=(r[item+11]&127) ? 6 : 8;
    if (dx < -reach-half || dx > reach+half || dy < -reach-half || dy > reach+half) continue;
    s->vy=(int16_t)item;s->radius=(uint8_t)kind;s->variant=4;r[item+0x28]=1;
    return true;
  }
  return false;
}
static bool chain_cancelled(const uint8_t *r,const MmxWeaponShot *s) {
  unsigned action=r[0xbaa];
  return r[0x1f0c] || !(r[0xbcf]&127) || (r[0xbb9]&64)!=s->facing ||
      action==12 || action==14 || action==0x18 || action==0x1a ||
      action==0x1c || action==0x24 || action==0x2c || action==0x30 || action==0x42;
}
static void chain_tick(uint8_t *r,unsigned d,MmxWeaponShot *s) {
  if (!s->age) {
    for (unsigned i=0;i<8;++i) if (combat.shots+i!=s && combat.shots[i].active &&
        combat.shots[i].page==1 && combat.shots[i].weapon==6) { retire(r,d);return; }
    if (!MmxWeaponsSpend(1,6,128)) { retire(r,d);return; }
    muzzle_origin(r,d,s);s->vx=(int16_t)((s->facing ? 1 : -1)*(s->charged ? 1536 : 1024));
    s->born=combat.tick;s->age=1;animation_start(s,s->charged ? 6 : 0);
    animation_step(s); /* The source advances animation on its allocation tick. */
    native_object(r,d,s);return;
  }
  if (s->age==1 && s->born==combat.tick) return;
  if (chain_cancelled(r,s) || s->muzzle_pose==2 || (!s->charged && r[d+1]==4) || ++s->age>160) { retire(r,d);return; }
  if (s->charged && r[d+1]==4) chain_spend(16);
  s->muzzle_pose=0;
  if (s->charged) s->hit_slots=0; /* Source resets the contact counter every tick. */
  MmxWeaponShot muzzle=*s;muzzle_origin(r,d,&muzzle);
  int direction=s->facing ? 1 : -1;
  if (s->variant!=3) {
    /* Source $9A4D follows body displacement, bounded to twelve pixels.
     * Re-evaluating the arm anchor also accommodates Zero and wall poses. */
    int shift=muzzle.origin_x-s->origin_x;
    if (shift>12) shift=12;if (shift< -12) shift=-12;
    s->x+=shift*256;s->y=muzzle.y;
  }
  s->origin_x=muzzle.origin_x;s->origin_y=muzzle.origin_y;
  if (!s->variant) {
    if (s->flags&128) {
      if (terrain_solid(r,s->x>>8,s->y>>8,false,NULL)) { retire(r,d);return; }
      s->variant=1;s->radius=s->charged ? 18 : 16;s->tether_pose=1;
      s->x=muzzle.x;s->y=muzzle.y;animation_start(s,s->charged ? 7 : 3);animation_step(s);
    } else animation_step(s);
  } else {
    s->tether_pose=(uint8_t)((s->tether_pose+1)%(s->charged ? 12 : 18));
    if (s->variant==1) {
      if ((!s->charged && chain_grab(r,s))) { /* Retract with the item on the next tick. */ }
      else if ((!s->charged && !combat.held) || !--s->radius ||
          terrain_solid(r,s->x>>8,(s->y>>8)+(s->charged ? 8 : 5),true,NULL) ||
          terrain_solid(r,s->x>>8,(s->y>>8)-(s->charged ? 8 : 5),false,NULL)) s->variant=2;
      else {
        /* Chain movement is horizontal; vy stores a held item, not velocity. */
        unsigned hit=terrain_move(r,s,s->charged ? 8 : 5,0);
        if (hit&1) { s->variant=3;animation_start(s,s->charged ? 9 : 2); }
        else animation_step(s);
      }
    } else if (s->variant==2 || s->variant==4) {
      if (s->variant==2) chain_grab(r,s);
      s->x-=s->vx;
      if (s->variant==4) {
        unsigned item=(uint16_t)s->vy;
        if (!chain_item(item) || !r[item] || r[item+10]!=s->radius || r[item+2]>=6) {
          chain_release(r,s);s->vy=0;s->variant=2;
        } else { putword(r+item+5,s->x>>8);putword(r+item+8,s->y>>8); }
      }
      /* Zero's arm reaches beyond the native pickup box. Carry an item
       * through the last few pixels to the body, then let X1 collect it. */
      int destination=s->variant==4 ? (int)word(r+0xbad) : s->origin_x;
      if (direction*((s->x>>8)-destination)<=0) { retire(r,d);return; }
      /* Source return phases keep the current hook pose; links still animate. */
    } else if (s->variant==3) {
      int distance=direction*((s->x>>8)-(int)word(r+0xbad));
      if (distance<12 || distance>=(s->charged ? 128 : 64) ||
          direction*((s->x>>8)-s->origin_x)<=0 || (r[0xbdf]&(s->facing ? 2 : 1))) {
        retire(r,d);return;
      }
      /* Apply the source's 4/6 px pull after ordinary player movement.
       * Sweep the host body so a hook cannot pull X/Zero through a wall. */
      MmxWeaponShot body={0};body.x=(int)word(r+0xbad)*256;body.y=(int)word(r+0xbb0)*256;
      body.vx=s->vx;body.vy=(int16_t)((s->y-muzzle.y));
      int oldx=body.x;terrain_move(r,&body,MmxZeroActive() ? 10 : 6,15);
      putword(r+0xbad,body.x>>8);putword(r+0xbb0,body.y>>8);putword(r+0xbc4,0);
      if (oldx==body.x) { retire(r,d);return; }
      muzzle_origin(r,d,&muzzle);s->origin_x=muzzle.origin_x;s->origin_y=muzzle.origin_y;
      animation_step(s);
    }
  }
  if (!s->active) { retire(r,d);return; }
  native_object(r,d,s);
}
