/* X3 $81:9A0B/$9CD8/$B167/$B230. See docs/weapons/triad-thunder.md.
 * variant: normal 0..2 orbs, 3..5 links, 6..8 rays; charged 0 body,
 * 1/2 ground waves. muzzle_pose: phase; radius: phase ticks;
 * tether_pose: renew count | fire-edge count<<2 (or damage enabled).
 * Every simulation field is included in the existing combat snapshot. */
static MmxWeaponShot *triad_body(void) {
  for (unsigned i=0;i<8;++i) {
    MmxWeaponShot *p=combat.shots+i;
    if (p->active && p->page==2 && p->weapon==3 && p->charged && !p->variant) return p;
  }
  return NULL;
}
static void triad_player(uint8_t *r) {
  r[0xc0f]=8;
  MmxWeaponShot *body=triad_body();
  if (body) {
    stop_charge(r);
    r[0xbde]=r[0xbe2]=r[0xbdf]=r[0xbe3]=0;
    putword(r+0xbc2,0);r[0xbf8]=0;
    if (body->muzzle_pose && r[0xbaa]!=14 && r[0xbaa]!=12) {
      r[0xbaa]=0;r[0xbab]=2;putword(r+0xbc4,0);
    } else if (!body->muzzle_pose && (int16_t)word(r+0xbc4)<-1408) putword(r+0xbc4,-1408);
    return;
  }
  for (unsigned i=0;i<8;++i) {
    MmxWeaponShot *p=combat.shots+i;
    if (!p->active || p->page!=2 || p->weapon!=3 || p->charged || p->variant>=3 || p->muzzle_pose>=3) continue;
    if (combat.pressed && p->muzzle_pose && (p->tether_pose>>2)<4) p->tether_pose+=4;
    r[0xbe3]&=(uint8_t)~64; /* Four repeat edges rotate the existing triangle. */
  }
}
static void triad_box(const MmxWeaponShot *s,uint8_t *box) {
  static const int8_t up[6][4]={{0,-8,9,10},{0,-16,9,16},{0,-24,9,20},
      {0,-32,9,27},{0,-39,9,35},{0,-48,9,42}};
  static const int8_t diagonal[9][4]={{-88,-88,8,8},{-80,-82,11,12},{-74,-74,18,13},
      {-1,0,0,24},{8,10,18,30},{0,5,5,5},{-52,-52,11,12},{-44,-44,16,13},{-32,-31,16,17}};
  box[0]=box[1]=0;box[2]=13;box[3]=15;
  if (s->charged) {if (!s->variant) {box[2]=127;box[3]=95;}return;}
  if (s->variant>=3 && s->variant<6) {
    if (s->variant==3) {box[2]=18;box[3]=9;}
    else {box[0]=254;box[2]=11;box[3]=20;}
  } else if (s->variant>=6) {
    unsigned f=s->flags&127;
    if (s->variant==6) memcpy(box,up[f>5?5:f],4);
    else {
      unsigned k=f<3?f:f==0x11?3:f==0x23?4:5;
      if (!(s->timer&1) && (f&0x30)) k=6+((f&0x30)>>4)-1;
      memcpy(box,diagonal[k],4);
    }
  }
}
static void triad_follow(const uint8_t *r,MmxWeaponShot *s) {
  int x=(int)word(r+0xbad),y=(int)word(r+0xbb0);
  s->x+=(x-s->origin_x)*256;s->y+=(y-s->origin_y)*256;
  s->origin_x=(int16_t)x;s->origin_y=(int16_t)y;
}
static void triad_native(uint8_t *r,unsigned d,MmxWeaponShot *s) {
  native_object(r,d,s);
  bool disabled=s->charged ? (s->variant ? !s->muzzle_pose : s->muzzle_pose<2) :
      s->variant<3 ? (!s->muzzle_pose || s->muzzle_pose==3) :
      s->variant<6 ? !s->tether_pose : false;
  if (disabled) putword(r+d+0x20,0);
}
static void triad_links(uint8_t *r,MmxWeaponShot *s) {
  static const int8_t xy[3][2]={{-16,-8},{0,24},{16,-8}};
  for (unsigned i=0;i<3;++i) {
    MmxWeaponShot *p=spawn_child(r,s);if (!p) break;
    p->variant=(uint8_t)(3+i);p->muzzle_pose=0;p->radius=30;p->tether_pose=0;
    int sign=(s->facing&128)?-1:1;
    p->x=(s->origin_x+xy[i][0])*256;p->y=(s->origin_y+xy[i][1]*sign)*256;
    p->facing=(uint8_t)((s->facing&128)|(i==0?64:0));
    animation_start(p,i==1?5:3);
    triad_native(r,0x1228+(unsigned)(p-combat.shots)*64,p);
  }
}
static bool triad_renew(uint8_t *r,MmxWeaponShot *s) {
  unsigned count=s->tether_pose&3;
  if ((s->tether_pose>>2)<4 || count>=3 || !MmxWeaponsSpend(2,3,256)) return false;
  static const int16_t vx[3]={0,-1024,1024},vy[3]={-1280,768,768};
  for (unsigned i=0;i<8;++i) {
    MmxWeaponShot *p=combat.shots+i;
    if (!p->active || p->page!=2 || p->weapon!=3 || p->charged || p->variant>=3 || p->muzzle_pose>=3) continue;
    p->facing^=128;p->muzzle_pose=0;p->radius=16;p->tether_pose=(uint8_t)(count+1);
    p->born=combat.tick;
    int sign=(p->facing&128)?-1:1;
    p->vx=(int16_t)(vx[p->variant]*sign);p->vy=(int16_t)(vy[p->variant]*sign);
    p->hit_slots=0;animation_start(p,0);
  }
  return true;
}
static void triad_release(uint8_t *r,MmxWeaponShot *s) {
  static const int16_t vx[3]={0,-1636,1636},vy[3]={-2048,1228,1228};
  static const int8_t xy[3][2]={{0,-40},{-32,24},{32,24}};
  MmxWeaponShot *p=spawn_child(r,s);
  if (p) {
    p->variant=(uint8_t)(6+s->variant);p->muzzle_pose=0;p->radius=18;p->tether_pose=0;
    int sign=(s->facing&128)?-1:1;
    p->facing=(uint8_t)((s->facing&128)|(s->variant==1?64:0));
    p->x=(s->origin_x+xy[s->variant][0])*256;p->y=(s->origin_y+xy[s->variant][1]*sign)*256;
    p->vx=vx[s->variant];p->vy=(int16_t)(vy[s->variant]*sign);
    animation_start(p,s->variant?8:7);
    triad_native(r,0x1228+(unsigned)(p-combat.shots)*64,p);
  }
  s->muzzle_pose=3;s->radius=(uint8_t)(10+((combat.tick+s->variant*3)&7));s->vx=s->vy=0;
  animation_start(s,2);
}
static bool triad_visible(const uint8_t *r,const MmxWeaponShot *s) {
  const MmxWeaponPose *p=MmxWeaponsPose(2,3,11,s->pose);
  int x=(s->x>>8)-(int)camera_x(r),y=(s->y>>8)-(int)camera_y(r);
  if (!p) return false;
  int left=(s->facing&64)?-p->left-p->width:p->left;
  int top=(s->facing&128)?-p->top-p->height:p->top;
  return x+left<288 && x+left+p->width>=-32 && y+top<240 && y+top+p->height>=-16;
}
/* The source's two-pixel probes follow floor slopes, climb wall faces,
 * and descend exposed edges. Integrate four one-pixel steps so X1's
 * narrower tiles/slopes cannot be skipped. Wave origin is the contact. */
static bool triad_ground(const uint8_t *r,MmxWeaponShot *s) {
  int x=s->x>>8,y=s->y>>8,dx=s->variant==1?-1:1;
  for (unsigned n=0;n<4;++n) {
    if (s->muzzle_pose==1) {
      if (terrain_solid(r,x+dx*2,y-2,false,NULL)) {s->muzzle_pose=2;s->vy=-1024;}
      else {
        int floor=0;bool found=false;
        for (int step=-2;step<=4;++step) if (terrain_solid(r,x+dx,y+step,true,&floor)) {found=true;break;}
        if (found) {x+=dx;y=floor;}
        else {x+=dx;s->muzzle_pose=2;s->vy=1024;}
      }
    }
    if (s->muzzle_pose==2) {
      int dy=s->vy<0?-1:1;y+=dy;
      if (dy<0) {
        if (terrain_solid(r,x,y-2,false,NULL)) return false;
        if (!terrain_solid(r,x+dx*2,y-1,false,NULL)) {x+=dx*2;s->muzzle_pose=1;}
      } else {
        int floor=0;
        if (terrain_solid(r,x,y+2,true,&floor)) {y=floor;s->muzzle_pose=1;}
        else if (!terrain_solid(r,x-dx*2,y,false,NULL)) return false;
      }
    }
  }
  s->x=x*256;s->y=y*256;return true;
}
static void triad_tick(uint8_t *r,unsigned d,MmxWeaponShot *s) {
  if (!s->age) {
    for (unsigned i=0;i<8;++i) {
      MmxWeaponShot *p=combat.shots+i;
      if (p==s || !p->active || p->page!=2 || p->weapon!=3) continue;
      if (!s->charged || p->charged) {retire(r,d);return;}
      retire(r,0x1228+i*64);
    }
    if (!MmxWeaponsSpend(2,3,(s->charged?3:1)*256)) {retire(r,d);return;}
    s->age=1;s->born=combat.tick;s->origin_x=(int16_t)word(r+0xbad);s->origin_y=(int16_t)word(r+0xbb0);
    s->x=s->origin_x*256;s->y=(s->origin_y-8)*256;
    if (s->charged) {
      s->group=51;s->facing=r[0xbb9]&64;animation_start(s,34);stop_charge(r);
      if (!(r[0xbd3]&4)) {r[0xbaa]=8;r[0xbab]=0;}
      for (unsigned i=1;i<=2;++i) {
        MmxWeaponShot *p=spawn_child(r,s);if (!p) break;
        p->group=11;p->variant=(uint8_t)i;p->facing=0;p->vx=(int16_t)(i==1?-1024:1024);p->vy=1024;
        animation_start(p,10);triad_native(r,0x1228+(unsigned)(p-combat.shots)*64,p);
      }
    } else {
      s->facing=0;s->radius=8;s->vx=0;s->vy=-1024;animation_start(s,0);
      for (unsigned i=1;i<=2;++i) {
        MmxWeaponShot *p=spawn_child(r,s);if (!p) break;
        p->variant=(uint8_t)i;p->vx=(int16_t)(i==1?-1024:1024);p->vy=1024;
        p->x+=p->vx;p->y+=p->vy;--p->radius;animation_step(p);
        triad_native(r,0x1228+(unsigned)(p-combat.shots)*64,p);
      }
      s->x+=s->vx;s->y+=s->vy;--s->radius;animation_step(s);
    }
    triad_native(r,d,s);return;
  }
  if (s->born==combat.tick) return;
  if (s->charged) {
    if (!s->variant) {
      if (r[0xbaa]==14 || r[0xbaa]==12 || r[0x1f0c]) {retire(r,d);return;}
      s->x=((int)camera_x(r)+128)*256;s->y=((int)camera_y(r)+112)*256;
      if (!s->muzzle_pose) {
        if (!(r[0xbd3]&4)) {triad_native(r,d,s);return;}
        s->muzzle_pose=1;r[0xbaa]=0;r[0xbab]=2;putword(r+0xbc2,0);putword(r+0xbc4,0);
      }
      animation_step(s);
      if ((s->flags&128) && s->timer==1) {retire(r,d);return;}
      if (s->flags&32) s->muzzle_pose=2;
      if (s->flags&64) s->muzzle_pose=3;
      if (s->muzzle_pose>=2) {
        s->hit_slots=0;
        if (!(s->radius++&7)) {
          /* Original X1 quake ($84:A341), X3 calls equivalent $84:D5A2. */
          r[0x1e7a]=6;r[0x1e83]=3;r[0x1e7f]=r[0x1e81]=1;r[0x1e85]=0;r[0x1e78]|=2;
        }
      }
    } else {
      if (!s->muzzle_pose) {
        MmxWeaponShot *body=triad_body();if (!body) {retire(r,d);return;}
        s->x=(int)word(r+0xbad)*256;s->y=((int)word(r+0xbb0)+16)*256;
        if (body->muzzle_pose>=3) s->muzzle_pose=1;
      } else {
        if (r[d+1]>=4 || !triad_ground(r,s) || !triad_visible(r,s)) {retire(r,d);return;}
        animation_step(s);
      }
    }
  } else if (s->variant<3) {
    if (s->muzzle_pose<3 || s->radius) triad_follow(r,s);
    if (!s->muzzle_pose) {
      s->x+=s->vx;s->y+=s->vy;
      if (!--s->radius) {s->muzzle_pose=1;s->radius=30;animation_start(s,1);}
    } else if (s->muzzle_pose==1) {
      if (!--s->radius) {s->muzzle_pose=2;s->radius=38;if (!s->variant) triad_links(r,s);}
    } else if (s->muzzle_pose==2) {
      if (!--s->radius) {
        if (s->variant || !triad_renew(r,s)) triad_release(r,s);
      }
    } else if (s->radius) --s->radius;
    else {s->vy+=32;if(s->vy>1024)s->vy=1024;s->y+=s->vy;}
    if (s->muzzle_pose<3) s->hit_slots=0;
    animation_step(s);
  } else if (s->variant<6) {
    triad_follow(r,s);
    if (!s->muzzle_pose) {
      if (s->flags&1) s->tether_pose=1;
      if (!--s->radius) {s->muzzle_pose=1;animation_start(s,s->variant==4?6:4);}
    } else {
      if ((s->flags&1) || s->pose==16) s->tether_pose=0;
      if ((s->flags&128) && s->timer==1) {retire(r,d);return;}
    }
    s->hit_slots=0;animation_step(s);
  } else {
    if (!s->muzzle_pose) {
      triad_follow(r,s);if (!--s->radius) s->muzzle_pose=1;
    } else {s->x+=s->vx;s->y+=s->vy;}
    s->hit_slots=0;animation_step(s);
  }
  ++s->age;
  if (!s->active || s->age>900 || (!s->charged && !triad_visible(r,s))) {retire(r,d);return;}
  triad_native(r,d,s);
}
