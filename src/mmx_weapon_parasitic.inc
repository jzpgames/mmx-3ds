/* X3 $81:96F0/$97AB/$AC82; docs/weapons/parasitic-bomb.md.
 * Variants: normal shot, captured enemy, charged cursor, homing bee, impact.
 * Payload origin_x/y = native host/type; cursor/bee origin_x/y = target/type.
 * Cursor tether_pose = quadrant, hit_slots = next-search quadrant (cursor 0).
 * Payload tether_pose = homing/stationary/rolling, radius = target index+1. */
static MmxWeaponShot *parasitic_cursor(unsigned id) {
  for(unsigned i=0;i<8;++i) {
    MmxWeaponShot *s=combat.shots+i;
    if(s->active && s->page==2 && s->weapon==2 && s->variant==2 && (id==4 || s->tether_pose==id)) return s;
  }
  return NULL;
}
static void parasitic_player(uint8_t *r) {
  r[0xc0f]=8;
  if(parasitic_cursor(4)) {
    stop_charge(r);r[0xbdf]&=(uint8_t)~64;r[0xbe3]&=(uint8_t)~64;return;
  }
  /* Source cursors appear at full charge while fire remains held. */
  if(combat.held && (r[0x1f99]&2) && r[0xc03]==1) r[0xbdf]&=(uint8_t)~64;
}
static void parasitic_release(uint8_t *r,const MmxWeaponShot *s) {
  MmxWeaponShot host=*s;gravity_destroy(r,&host);
}
static bool parasitic_target(const uint8_t *r,unsigned enemy,int kind) {
  if(enemy<0xe68 || enemy>=0x1228 || (enemy&63)!=0x28 || !r[enemy] ||
      r[enemy+1]!=2 || !(r[enemy+0x27]&127) || r[enemy+0x28]>=6 ||
      (kind>=0 && r[enemy+10]!=kind) || !word(r+enemy+0x20) ||
      !MmxWeaponsEnemyActive(r,enemy,1)) return false;
  int x=(int)word(r+enemy+5)-(int)camera_x(r),y=(int)word(r+enemy+8)-(int)camera_y(r);
  return x>=0 && x<256 && y>=0 && y<224;
}
static unsigned parasitic_search(const uint8_t *r,const MmxWeaponShot *s) {
  unsigned found=0;int distance=0x7fffffff;
  for(unsigned d=0xe68;d<0x1228;d+=64) {
    if(!parasitic_target(r,d,-1)) continue;
    bool reserved=false;
    if(s->variant==2) for(unsigned i=0;i<8;++i) {
      const MmxWeaponShot *p=combat.shots+i;
      if(p!=s && p->active && p->page==2 && p->weapon==2 && p->variant==2 &&
          p->muzzle_pose>0 && p->muzzle_pose<4 && p->origin_x==(int)d) reserved=true;
    }
    if(reserved) continue;
    int dx=(int)word(r+d+5)-(s->x>>8),dy=(int)word(r+d+8)-(s->y>>8);
    int n=dx*dx+dy*dy;if(n<distance) {found=d;distance=n;}
  }
  return found;
}
static bool parasitic_contact(uint8_t *r,unsigned enemy,MmxWeaponShot *s) {
  size_t off=0x30000+(word(r+enemy+0x20)&0x7fff);
  if(!collision_rom || off+4>collision_rom_size) return false;
  unsigned rx=collision_rom[off+2],ry=collision_rom[off+3];
  if(!rx || !ry || rx>24 || ry>32) return false;
  int x=(int)word(r+enemy+5),y=(int)word(r+enemy+8);
  s->variant=1;s->muzzle_pose=0;s->origin_x=(int16_t)enemy;s->origin_y=r[enemy+10];
  s->x=x*256;s->y=y*256;s->vy=0;s->radius=0;s->age=1;
  /* X1 has no X3 per-enemy response bytes. Keep large anchored actors
   * stationary; use rolling for small grounded actors, homing for airborne. */
  s->tether_pose=(rx>16 || ry>24)?2:terrain_solid(r,x,y+(int)ry+2,true,NULL)?3:1;
  if(s->tether_pose!=3) s->vx=0;
  animation_start(s,1);return true;
}
static void parasitic_aim(const uint8_t *r,MmxWeaponShot *s,unsigned target) {
  unsigned a=gravity_angle((int)word(r+target+5)-(s->x>>8),(int)word(r+target+8)-(s->y>>8));
  s->vx=(int16_t)(ray_vector(a)/4);s->vy=(int16_t)(-ray_vector(a+8)/4);
}
static void parasitic_home_cursor(const uint8_t *r,MmxWeaponShot *s) {
  static const int8_t offsets[4][2]={{-16,-20},{-16,20},{16,-20},{16,20}};
  s->x=((int)word(r+0xbad)+offsets[s->tether_pose][0])*256;
  s->y=((int)word(r+0xbb0)+offsets[s->tether_pose][1]-(MmxZeroActive()?8:0))*256;
}
static void parasitic_native(uint8_t *r,unsigned d,MmxWeaponShot *s) {
  native_object(r,d,s);
  if(s->variant==2 || s->variant==4 || (s->variant==1 && !s->muzzle_pose) ||
      (s->variant==3 && (s->muzzle_pose!=1 || !(s->hit_slots&0x8000)))) putword(r+d+0x20,0);
}
static void parasitic_impact(MmxWeaponShot *s,bool unit_contact) {
  s->variant=4;s->group=unit_contact?23:8;s->muzzle_pose=unit_contact;s->facing=0;
  animation_start(s,unit_contact?1:5);
}
static void parasitic_cursor_tick(uint8_t *r,unsigned d,MmxWeaponShot *s) {
  bool held=combat.held && MmxWeaponsEnergyAmount(2,2)>=256 && r[0xbaa]!=14 && r[0xbaa]!=12 && !r[0x1f0c];
  unsigned target=(unsigned)s->origin_x;
  MmxWeaponShot *leader=parasitic_cursor(0);
  if(!leader) {leader=parasitic_cursor(4);if(leader) leader->hit_slots=leader->tether_pose;}
  if(s->muzzle_pose && s->muzzle_pose<3 && !parasitic_target(r,target,s->origin_y)) {
    if(leader && s->muzzle_pose==1 && s->radius) leader->hit_slots=(s->tether_pose+1)&3;
    s->muzzle_pose=4;s->radius=10;
  }
  if(!s->muzzle_pose) {
    if(!held) {retire(r,d);return;}
    parasitic_home_cursor(r,s);
    if(leader && leader->hit_slots==s->tether_pose) {
      target=parasitic_search(r,s);
      if(target) {s->origin_x=(int16_t)target;s->origin_y=r[target+10];s->muzzle_pose=1;s->radius=4;parasitic_aim(r,s,target);}
    }
  } else if(s->muzzle_pose==1) {
    if(s->radius && !--s->radius && leader) leader->hit_slots=(s->tether_pose+1)&3;
    int dx=(int)word(r+target+5)-(s->x>>8),dy=(int)word(r+target+8)-(s->y>>8);
    /* Original $81:ADB5/$ADC6 snaps when either axis comes within 5px. */
    if((dx>-5 && dx<5) || (dy>-5 && dy<5)) {
      if(leader && s->radius) leader->hit_slots=(s->tether_pose+1)&3;
      s->muzzle_pose=2;s->radius=16;animation_start(s,10);
    } else {
      if(!(s->age&3)) parasitic_aim(r,s,target);s->x+=s->vx;s->y+=s->vy;
    }
  } else if(s->muzzle_pose==2) {
    s->x=(int)word(r+target+5)*256;s->y=(int)word(r+target+8)*256;
    if(s->radius) --s->radius;
    else if(free_slot(r) && MmxWeaponsSpend(2,2,256)) {
      MmxWeaponShot *bee=spawn_child(r,s);
      if(bee) {
        bee->variant=3;bee->muzzle_pose=0;bee->radius=10;bee->hit_slots=0;
        bee->facing=r[0xbb9]&64;muzzle_origin(r,d,bee);
        bee->origin_x=s->origin_x;bee->origin_y=s->origin_y;
        bee->vx=(int16_t)(bee->facing?768:-768);bee->vy=0;animation_start(bee,12);
        parasitic_native(r,0x1228+(unsigned)(bee-combat.shots)*64,bee);
        s->muzzle_pose=3;
      }
    } else if(MmxWeaponsEnergyAmount(2,2)<256) {s->muzzle_pose=4;s->radius=10;}
  } else if(s->muzzle_pose==3) {
    bool bee=false;
    for(unsigned i=0;i<8;++i) {
      const MmxWeaponShot *p=combat.shots+i;
      if(p->active && p->page==2 && p->weapon==2 && p->variant==3 && p->tether_pose==s->tether_pose) bee=true;
    }
    if(parasitic_target(r,target,s->origin_y)) {s->x=(int)word(r+target+5)*256;s->y=(int)word(r+target+8)*256;}
    if(!bee) {s->muzzle_pose=4;s->radius=30;}
  } else if(!s->radius || !--s->radius) {
    if(!held) {retire(r,d);return;}
    s->muzzle_pose=0;animation_start(s,8);
  }
  animation_step(s);
}
static void parasitic_tick(uint8_t *r,unsigned d,MmxWeaponShot *s) {
  if(!s->age) {
    if(s->charged) {
      if(parasitic_cursor(4)) {retire(r,d);return;}
      s->variant=2;s->age=1;s->born=combat.tick;animation_start(s,8);stop_charge(r);
      parasitic_home_cursor(r,s);
      for(unsigned i=1;i<4;++i) {
        MmxWeaponShot *p=spawn_child(r,s);if(!p) break;
        p->tether_pose=(uint8_t)i;parasitic_home_cursor(r,p);
        parasitic_native(r,0x1228+(unsigned)(p-combat.shots)*64,p);
      }
    } else {
      unsigned count=0;for(unsigned i=0;i<8;++i) if(combat.shots[i].active &&
          combat.shots[i].page==2 && combat.shots[i].weapon==2 && !combat.shots[i].charged) ++count;
      if(count>3 || !MmxWeaponsSpend(2,2,256)) {retire(r,d);return;}
      muzzle_origin(r,d,s);s->age=1;s->born=combat.tick;s->vx=(int16_t)(s->facing?768:-768);animation_start(s,0);
    }
    parasitic_native(r,d,s);return;
  }
  if(s->born==combat.tick) return;
  if(s->variant==2) parasitic_cursor_tick(r,d,s);
  else if(s->variant==1) {
    if(!gravity_host(r,s) || (s->muzzle_pose && r[d+1]>=6)) {retire(r,d);return;}
    if(!s->muzzle_pose) {
      if((s->flags&128) && s->timer==1) {s->muzzle_pose=s->tether_pose;s->age=1;s->hit_slots=0;animation_start(s,2);}
      else animation_step(s);
    } else {
      if(s->age>=(s->muzzle_pose==1?180:240)) {retire(r,d);return;}
      if(s->muzzle_pose==1) {
        unsigned target=s->radius?0xe68+(s->radius-1)*64:0;
        if(!parasitic_target(r,target,-1)) {target=parasitic_search(r,s);s->radius=(uint8_t)(target?(target-0xe68)/64+1:0);}
        if(!(s->age&3)) {
          if(target) parasitic_aim(r,s,target);
          else {unsigned a=(s->age/4)&31;s->vx=(int16_t)(gravity_slow_vector(a)>>1);s->vy=(int16_t)-(gravity_slow_vector(a+8)>>1);}
        }
        s->x+=s->vx;s->y+=s->vy;
      } else if(s->muzzle_pose==3) {
        s->vy+=32;if(s->vy>1280) s->vy=1280;
        unsigned hit=terrain_move(r,s,13,12);if(hit&3) {retire(r,d);return;}if(hit&4) s->vy=0;
      }
      putword(r+s->origin_x+5,s->x>>8);putword(r+s->origin_x+8,s->y>>8);animation_step(s);
    }
  } else if(s->variant==3) {
    if(r[d+1]>=6) parasitic_impact(s,true);
    else if(r[d+1]==4) {s->muzzle_pose=2;s->facing|=128;s->pose=39;s->vx=0;s->vy=32;s->y+=s->vy;}
    else if(!s->muzzle_pose) {
      s->vx+=(s->facing?-16:16);s->x+=s->vx;
      if(!--s->radius) {s->muzzle_pose=1;s->radius=120;}
      animation_step(s);
    } else if(s->muzzle_pose==1) {
      unsigned target=(unsigned)s->origin_x;
      /* Source $81:AEB5: collision and its 120-tick life become active
       * when the unit reaches its locked cursor. Bit 15 is outside the
       * fifteen native-enemy contact bits and survives snapshots. */
      MmxWeaponShot *cursor=parasitic_cursor(s->tether_pose);
      if(cursor && cursor->muzzle_pose==3) {
        int dx=(s->x-cursor->x)>>8,dy=(s->y-cursor->y)>>8;
        if(dx>-16 && dx<16 && dy>-18 && dy<18) s->hit_slots|=0x8000;
      }
      if((s->hit_slots&0x8000) && !--s->radius) {parasitic_impact(s,false);parasitic_native(r,d,s);return;}
      if(parasitic_target(r,target,s->origin_y) && !(s->age&3)) parasitic_aim(r,s,target);
      s->facing=s->vx>=0?64:0;s->x+=s->vx;s->y+=s->vy;animation_step(s);
    } else {s->vy+=32;s->y+=s->vy;}
  } else if(s->variant==4) {
    if((s->flags&128) && s->timer==1) {retire(r,d);return;}
    animation_step(s);
  } else {
    if(r[d+1]>=6) parasitic_impact(s,false);
    else {
      if(r[d+1]==4 && !s->muzzle_pose) {s->muzzle_pose=1;s->vx=-s->vx;s->vy=-768;}
      s->x+=s->vx;s->y+=s->vy;animation_step(s);
    }
  }
  if(!s->active) return;
  ++s->age;
  if(s->variant!=2 && !fang_visible(r,s)) {retire(r,d);return;}
  parasitic_native(r,d,s);
}
