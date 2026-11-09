/* X3 Tornado Fang, $81:A9F6/$BE58. See docs/weapons/tornado-fang.md.
 * Saved fields: variant=normal single/top/bottom or charged orientation;
 * muzzle_pose=route/wait/accelerate/contact/deflected; radius=phase timer;
 * tether_pose=remaining drill contacts; origin_x=route cursor,
 * origin_y=normal source speed cap / charged previous player height.
 * No unsaved simulation state. */
static void fang_player(uint8_t *r) {
  r[0xc0f]=3;
  for (unsigned i=0;i<8;++i) {
    const MmxWeaponShot *s=combat.shots+i;
    if (!s->active || s->page!=2 || s->weapon!=8 || !s->charged) continue;
    stop_charge(r);r[0xbdf]&=(uint8_t)~64;r[0xbe3]&=(uint8_t)~64;
    if (combat.held && r[0xbaa]!=14 && r[0xbaa]!=12 && !r[0x1f0c]) r[0xbf8]=16;
    return;
  }
  /* Unlike the release weapons, X3 deploys its drill at full charge while
   * Y is still held. Let X1's native full-release path allocate it. */
  if (combat.held && (r[0x1f99]&2) && r[0xc03]==1) r[0xbdf]&=(uint8_t)~64;
}
static void fang_contact(const uint8_t *r,const MmxWeaponShot *s,unsigned enemy) {
  /* X3 $81:ABDC/$C046: class $10, group $13 sequence 11. The last
   * normal contact uses $84:D6E0's shared puff instead. No attack slot. */
  for(unsigned i=0;i<16;++i) if(!combat.effects[i].active) {
    MmxWeaponShot *t=combat.effects+i;*t=*s;
    bool exhausted=!s->charged && s->tether_pose==1;
    t->variant=exhausted?4:3;t->group=exhausted?8:20;t->charged=0;
    t->age=1;t->born=combat.tick;t->vx=t->vy=0;t->radius=t->tether_pose=t->muzzle_pose=0;
    t->facing=s->facing^64;
    t->x=(s->charged?(s->x>>8)+(s->facing?4:-4):(int)word(r+enemy+5))*256;
    t->y=(s->charged?(s->y>>8)-4:(int)word(r+enemy+8))*256;
    animation_start(t,exhausted?5:11);return;
  }
}
static void fang_route(MmxWeaponShot *s) {
  /* $86:BA36 records are duration/VX/VY, upward-positive source Y. */
  static const uint8_t duration[3][3]={{4,4,0},{4,6,8},{2,0,0}};
  unsigned route=s->muzzle_pose==3?2:s->variant?1:0,index=(unsigned)s->origin_x;
  if (index>=3 || !duration[route][index]) {
    if (s->muzzle_pose!=3) {s->muzzle_pose=1;animation_start(s,0);}
    s->vx=s->vy=0;return;
  }
  s->radius=duration[route][index];
  int speed=route==2?16:route==1 && index==1?0:512;
  s->vx=(int16_t)(s->facing?speed:-speed);
  s->vy=(route==1 && index==1)?(s->variant==1?-768:768):0;
}
static bool fang_visible(const uint8_t *r,const MmxWeaponShot *s) {
  int x=(s->x>>8)-(int)camera_x(r),y=(s->y>>8)-(int)camera_y(r);
  return x>=-32 && x<288 && y>=-16 && y<240;
}
static void fang_attach(uint8_t *r,unsigned d,MmxWeaponShot *s) {
  int previous_y=s->origin_y;
  s->facing=r[0xbb9]&64;muzzle_origin(r,d,s);
  /* Source standing arm offsets are X +/-23, Y -5. Keep Zero's actual
   * muzzle offsets and add the seven-pixel drill mount beyond the barrel. */
  s->x+=(s->facing?7:-7)*256;s->y-=2*256;s->variant=0;
  if (r[0xbaa]==0x12) {
    s->variant=2;s->x=((int)word(r+0xbad)+(s->facing?8:-8))*256;
    s->y=((int)word(r+0xbb0)-16)*256;
    if (terrain_solid(r,(s->x>>8)+(s->facing?1:-1)*8,s->y>>8,false,NULL)) {
      s->variant=3;
      /* X3 $84:8A19 stops wall sliding while +$77 is nonzero. Preserve
       * X1's wall/jump transitions, only suppress the current slide step. */
      if (s->age>1) {putword(r+0xbb0,(unsigned)previous_y);s->y=(previous_y-16)*256;}
      putword(r+0xbc4,0);
    }
  }
  s->origin_y=(int16_t)word(r+0xbb0);
  s->pose=(uint8_t)(42+((s->age/2)&3)*6+s->variant);
}
static void fang_tick(uint8_t *r,unsigned d,MmxWeaponShot *s) {
  if (!s->age) {
    unsigned count=0;bool recent=false;
    for (unsigned i=0;i<8;++i) {
      const MmxWeaponShot *p=combat.shots+i;
      if (p==s || !p->active || p->page!=2 || p->weapon!=8) continue;
      ++count;recent|=!p->charged && !p->variant && p->age<=60;
      if (p->charged) {retire(r,d);return;}
    }
    if ((!s->charged && count>=3) || !MmxWeaponsSpend(2,8,(s->charged?3:1)*256)) {retire(r,d);return;}
    s->age=1;s->born=combat.tick;s->tether_pose=7;muzzle_origin(r,d,s);
    if (s->charged) {s->radius=64;fang_attach(r,d,s);stop_charge(r);r[0xbf8]=16;}
    else {
      s->variant=recent?1:0;s->origin_x=0;
      s->origin_y=terrain_water(r,s->x>>8,s->y>>8)?1024:768;
      animation_start(s,13);fang_route(s);
      if (recent && count<2) {
        MmxWeaponShot *child=spawn_child(r,s);if(child) child->variant=2;
      }
    }
    native_object(r,d,s);return;
  }
  if (s->age==1 && s->born==combat.tick) return;
  if (s->charged) {
    if (!combat.held || r[0xbaa]==14 || r[0xbaa]==12 || r[0x1f0c] ||
        !(r[0xbcf]&127) || !MmxWeaponsEnergyAmount(2,8)) {retire(r,d);return;}
    if (!--s->radius) {
      if (!MmxWeaponsSpend(2,8,3*256)) {retire(r,d);return;}s->radius=64;
    }
    ++s->age;if (!s->age) s->age=2;
    fang_attach(r,d,s);s->hit_slots=0;native_object(r,d,s);
    if (s->variant==3) putword(r+d+0x20,0);
    return;
  }
  if (r[d+1]>=6 && s->muzzle_pose!=3 && s->muzzle_pose!=4) {
    if (!--s->tether_pose) {retire(r,d);return;}
    s->muzzle_pose=3;s->radius=16;s->vx=(int16_t)(s->facing?16:-16);s->vy=0;
    s->origin_x=0;animation_start(s,2);
  } else if (r[d+1]==4 && s->muzzle_pose!=4) {
    s->muzzle_pose=4;s->vx/=2;s->vy=-768;animation_start(s,3);
  }
  if (s->muzzle_pose==3) {
    if (s->radius>14) s->x+=s->vx;
    if (!--s->radius) {
      s->muzzle_pose=2;s->vx=s->vy=0;s->hit_slots=0;animation_start(s,1);
    }
  } else if (s->muzzle_pose==4) {s->vy+=16;s->x+=s->vx;s->y+=s->vy;}
  else if (s->muzzle_pose==0) {
    s->x+=s->vx;s->y+=s->vy;
    if (s->radius && !--s->radius) {++s->origin_x;fang_route(s);}
  } else if (s->muzzle_pose==1) {
    if (s->age>=60) {s->muzzle_pose=2;s->vx=s->vy=0;animation_start(s,1);}
  } else {
    int cap=s->origin_y;
    s->vx+=(int16_t)(s->facing?32:-32);
    if (s->vx>cap) s->vx=(int16_t)cap;if (s->vx < -cap) s->vx=(int16_t)-cap;
    s->x+=s->vx;
  }
  animation_step(s);++s->age;
  if (!s->active || !fang_visible(r,s) || s->age>900) {retire(r,d);return;}
  native_object(r,d,s);
  if (s->muzzle_pose==4) putword(r+d+0x20,0);
}
