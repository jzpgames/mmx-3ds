/* X2 Crystal Hunter: $81:8D1C and $81:9CCF. Source notes in
 * docs/weapons/crystal-hunter.md. Normal phases: 0 projectile, 1 forming,
 * 2 falling, 3 bouncing, 4 grounded, 5 shatter contact. origin_x/y store
 * the frozen enemy slot/type. Charged age counts display frames, including
 * the original 180-frame onset and 480-frame half-speed period. */
static const MmxWeaponShot *crystal_time(const MmxWeaponCombatState *c) {
  for(unsigned i=0;i<8;++i) {
    const MmxWeaponShot *s=c->shots+i;
    if(s->active && s->age && s->page==1 && s->weapon==1 && s->charged) return s;
  }
  return NULL;
}
unsigned MmxWeaponsTimePhase(const MmxWeaponCombatState *c) {
  const MmxWeaponShot *s=c ? crystal_time(c) : NULL;
  return !s ? 0 : s->muzzle_pose ? 2 : 1;
}
bool MmxWeaponsTimeActive(void) {
  return MmxWeaponsEnabled() && (crystal_time(&combat) ||
      (partner_combat && crystal_time(partner_combat)));
}
bool MmxWeaponsFrameTick(uint8_t r[0x20000]) {
  if(!MmxWeaponsEnabled()) return false;
  const MmxWeaponShot *p=crystal_time(&combat);if(!p) return false;
  unsigned slot=(unsigned)(p-combat.shots);MmxWeaponShot *s=combat.shots+slot;
  if(r[0xd1]!=2 || r[0xd2]!=4 || r[0xd3]!=4 || !(r[0xbcf]&127) || combat.stage!=r[0x1f7a]) {
    retire(r,0x1228+slot*64);return false;
  }
  if(r[0x1f10]) return false; /* Pause does not consume the effect. */
  if(s->age<=180) {++s->age;return true;}
  s->muzzle_pose=1;
  if(++s->age>660) {retire(r,0x1228+slot*64);return false;}
  return (s->age&1)==0;
}
void MmxWeaponsTimeRipple(const MmxWeaponCombatState *c,int16_t lines[224]) {
  memset(lines,0,224*sizeof(*lines));
  const MmxWeaponShot *s=crystal_time(c);if(!s || s->age>180) return;
  /* Replay the source HDMA-buffer updates, not a synthesized sine wave.
   * $86:B7ED supplies these four expansion records; $81:9D4F shifts two
   * scanlines outward, with opposite signs above/below the center. */
  static const uint8_t wave[4][13]={{1,2,3,0},{2,2,3,3,4,4,0},
    {3,3,4,4,5,5,6,6,7,7,0},{4,4,5,5,6,6,7,7,8,8,9,9,0}};
  for(unsigned frame=1;frame<s->age;++frame) {
    memmove(lines,lines+2,110*sizeof(*lines));
    memmove(lines+114,lines+112,110*sizeof(*lines));
    memset(lines+110,0,4*sizeof(*lines));
    if(frame>=32 && !(frame&15)) for(unsigned half=0;half<2;++half) {
      int i=half?112:111,step=half?1:-1;
      while(i>=0 && i<224) {
        unsigned n=(unsigned)(half?-lines[i]:lines[i]);
        if(!n) {i+=step;continue;}
        unsigned row=n<4?n:3;
        for(unsigned k=0;k<13 && wave[row][k] && i>=0 && i<224;++k,i+=step)
          lines[i]=(int16_t)(half?-(int)wave[row][k]:wave[row][k]);
      }
    }
    if(!(frame&15)) {lines[111]=1;lines[112]=-1;}
  }
}
bool MmxWeaponsFrozenEnemy(const MmxWeaponCombatState *c,unsigned enemy) {
  for(unsigned i=0;i<8;++i) {
    const MmxWeaponShot *s=c->shots+i;
    if(s->active && s->page==1 && s->weapon==1 && !s->charged &&
        s->muzzle_pose>=1 && s->muzzle_pose<=4 && s->origin_x==(int)enemy) return true;
  }
  return false;
}
unsigned MmxWeaponsEnemyActive(const uint8_t r[0x20000],unsigned enemy,unsigned active) {
  (void)r;
  return MmxWeaponsEnabled() && (MmxWeaponsFrozenEnemy(&combat,enemy) ||
      MmxWeaponsMovedEnemy(&combat,enemy) || (partner_combat &&
      (MmxWeaponsFrozenEnemy(partner_combat,enemy) || MmxWeaponsMovedEnemy(partner_combat,enemy)))) ? 0 : active;
}
static void crystal_contact(uint8_t *r,unsigned enemy,MmxWeaponShot *s) {
  s->origin_x=(int16_t)enemy;s->origin_y=r[enemy+10];s->muzzle_pose=1;
  s->x=(int32_t)word(r+enemy+5)*256;s->y=(int32_t)word(r+enemy+8)*256;
  s->vx=s->vy=0;s->radius=0;animation_start(s,2);
}
static void crystal_shatter(uint8_t *r,MmxWeaponShot *s) {
  s->muzzle_pose=5;s->radius=8;s->hit_slots=0;
  /* Re-enter the target's native lethal-hit path, including normal drops
   * and stage cleanup. The crystal itself supplies the final contact. */
  r[s->origin_x+0x27]&=127;
  r[s->origin_x+0x26]=0; /* Native death actors must not retain contact damage. */
  /* $81:8F14 creates eight class-08 debris actors, subtypes A5/A6.
   * $81:F068 uses the two velocity tables at $86:EE38/$86:EE48. */
  static const int16_t vx[8]={-512,-256,128,384,-640,-384,256,512};
  static const int16_t vy[8]={768,1024,1408,640,896,1152,1280,512};
  unsigned count=0;
  for(unsigned i=0;i<16 && count<8;++i) if(!combat.effects[i].active) {
    MmxWeaponShot *p=combat.effects+i;memset(p,0,sizeof(*p));
    p->active=1;p->age=1;p->page=1;p->weapon=1;p->group=16;p->variant=2;
    p->x=s->x;p->y=s->y;p->born=combat.tick;p->tether_pose=(uint8_t)(count++&1);
    p->facing=(uint8_t)(bubble_random(i+1)&64);
    p->vx=vx[bubble_random(i+2)&7];p->vy=-vy[bubble_random(i+3)&7];
    animation_start(p,4+(bubble_random(i+4)&1));
  }
}
static void crystal_effect_tick(uint8_t *r,MmxWeaponShot *s) {
  if(!fang_visible(r,s)) {memset(s,0,sizeof(*s));return;}
  ++s->age;s->vy+=48;s->x+=s->vx;s->y+=s->vy;animation_step(s);
}
static void crystal_terrain(uint8_t *r) {
  for(unsigned i=0;i<8;++i) {
    MmxWeaponShot *s=combat.shots+i;
    if(!s->active || s->page!=1 || s->weapon!=1 || s->charged ||
        s->muzzle_pose<2 || s->muzzle_pose>4) continue;
    int top=(s->y>>8)-21,dx=(int)word(r+0xbad)-(s->x>>8);
    int feet=(int)word(r+0xbb0)+16,previous=(int)word(r+0xbcc)+16;
    if((int16_t)word(r+0xbc4)>0 || dx < -22 || dx>22 || previous>top+2 || feet<top ||
        r[0xbaa]==14 || r[0xbaa]==12 || r[0x1f0c]) continue;
    putword(r+0xbb0,top-16);r[0xbaf]=0;r[0xbd3]|=4;r[0xbd7]=0;
  }
}
static void crystal_tick(uint8_t *r,unsigned d,MmxWeaponShot *s) {
  if(!s->age) {
    unsigned frozen=0;bool flying=false,charged=false;
    for(unsigned i=0;i<8;++i) {
      const MmxWeaponShot *p=combat.shots+i;
      if(p==s || !p->active || p->page!=1 || p->weapon!=1) continue;
      if(p->charged) charged=true;
      else if(!p->muzzle_pose) flying=true;
      else ++frozen;
    }
    if(charged || flying || (!s->charged && frozen>=3) ||
        !MmxWeaponsSpend(1,1,(s->charged?2:1)*256)) {retire(r,d);return;}
    muzzle_origin(r,d,s);s->age=1;s->born=combat.tick;
    if(s->charged) {stop_charge(r);r[0xbf8]=0;}
    else {
      bool water=terrain_water(r,s->x>>8,s->y>>8);
      s->vx=(int16_t)((s->facing?1:-1)*(water?768:832));s->vy=water?-320:0;
      animation_start(s,1);
    }
    native_object(r,d,s);
    if(s->charged) putword(r+d+0x20,0);
    return;
  }
  if(s->charged) {native_object(r,d,s);putword(r+d+0x20,0);return;}
  if(!s->muzzle_pose) {
    if(r[d+1]==4 && !s->variant) {s->variant=1;s->vx=-s->vx;}
    if(r[d+1]>=6) {retire(r,d);return;}
    s->vy+=16;if(s->vy>1280) s->vy=(int16_t)(1280+(s->vy&255));
    s->x+=s->vx;s->y+=s->vy;animation_step(s);
  } else {
    unsigned enemy=(unsigned)s->origin_x;
    if(enemy<0xe68 || enemy>=0x1228 || !r[enemy] || r[enemy+10]!=s->origin_y) {retire(r,d);return;}
    if(s->muzzle_pose==1) {
      if(s->flags&128) {s->muzzle_pose=2;s->vy=0;animation_start(s,3);}
      else animation_step(s);
    } else if(s->muzzle_pose<5) {
      if(s->muzzle_pose!=4) {
        s->vy+=64;if(s->vy>1280) s->vy=(int16_t)(1280+(s->vy&255));
        unsigned hit=terrain_move(r,s,16,21);
        if(hit&4) {
          if(s->muzzle_pose==2) {s->muzzle_pose=3;s->vy=(int16_t)(-s->vy/2);}
          else {s->muzzle_pose=4;s->vy=0;}
        }
      }
      putword(r+enemy+5,(unsigned)(s->x>>8));putword(r+enemy+8,(unsigned)(s->y>>8));
      animation_step(s);
      int dx=(int)word(r+0xbad)-(s->x>>8),dy=(int)word(r+0xbb0)-(s->y>>8);
      int speed=(int16_t)word(r+0xbc2);if(speed<0) speed=-speed;
      if(speed>=768 && dx>=-24 && dx<=24 && dy>=-31 && dy<=31) crystal_shatter(r,s);
    } else if(!--s->radius) {retire(r,d);return;}
  }
  ++s->age;
  if(!s->active || !fang_visible(r,s)) {retire(r,d);return;}
  native_object(r,d,s);
  if(s->muzzle_pose!=0 && s->muzzle_pose!=5) putword(r+d+0x20,0);
  if(s->variant) putword(r+d+0x20,0);
}
