/* X2 Silk Shot: $81:91C0 normal, $81:A136 charged, $88:C475 gathering.
 * variant = source material (stone, metal, scrap, leaves, crystal).
 * muzzle_pose = 0 gathering/normal flight, 1 held/leaf launch,
 *               2 charged flight/leaf fall, 3 dispersed pieces.
 * tether_pose = original piece identity; radius = bounce/gather/life timer.
 * All coordinates, animation and RNG choices are stored in the combat state. */
static const uint8_t silk_normal_sequence[5][2]={{2,3},{6,7},{10,11},{16,16},{19,20}};
static const uint8_t silk_fragment_sequence[5][2]={{4,5},{8,8},{14,15},{17,17},{21,22}};
static const uint8_t silk_charged_sequence[5][8]={
  {2,3,2,3,4,5,4,5},{6,7,6,7,8,9,8,9},
  {10,11,12,13,14,15,14,15},{16,16,16,16,18,18,18,18},
  {19,20,19,20,21,22,21,22}};
static const int16_t silk_radial[8][2]={
  {0,2048},{-2048,2048},{-2048,0},{-2048,-2048},
  {0,-2048},{2048,2048},{2048,0},{2048,-2048}};
static const int8_t silk_offsets[8][2]={
  {-8,0},{-4,-10},{4,4},{8,-8},{-4,-12},{-4,8},{-2,-2},{10,0}};
static const uint8_t silk_flips[8]={0,0,64,64,0,0,64,64};

static unsigned silk_material(const uint8_t *r) {
  /* X1 has no Silk material events. Keep its environment choice explicit;
   * the attack physics and original assets remain those of the chosen form. */
  static const uint8_t material[13]={1,0,3,0,2,1,2,1,4,0,2,1,4};
  return r[0x1f7a]<13 ? material[r[0x1f7a]] : 0;
}
static unsigned silk_group(unsigned material) { return material==3 ? 73 : material==4 ? 74 : 72; }
static bool silk_blocked_action(const uint8_t *r) {
  return r[0xbaa]==12 || r[0xbaa]==14 || r[0xbaa]==0x18 || r[0xbaa]==0x42 || r[0x1f0c];
}
static void silk_held_origin(const uint8_t *r,unsigned d,MmxWeaponShot *s) {
  unsigned id=s->tether_pose-1;s->facing=r[0xbb9]&64;
  muzzle_origin(r,d,s);
  s->x+=(s->facing ? 1 : -1)*silk_offsets[id][0]*256;
  s->y+=silk_offsets[id][1]*256;s->facing^=silk_flips[id];
  s->origin_x=(int16_t)word(r+0xbad);s->origin_y=(int16_t)word(r+0xbb0);
}
static bool silk_create_pieces(uint8_t *r,bool released) {
  bool created=false;
  for (unsigned id=8;id;--id) {
    unsigned d=free_slot(r);if (!d) break;
    MmxWeaponShot *s=combat.shots+slot_index(d);memset(s,0,sizeof(*s));
    s->active=s->charged=s->age=1;s->page=1;s->weapon=3;
    s->variant=(uint8_t)silk_material(r);s->group=(uint8_t)silk_group(s->variant);
    s->tether_pose=(uint8_t)id;s->radius=32;s->born=combat.tick;
    silk_held_origin(r,d,s);
    if (released) {
      unsigned energy=MmxWeaponsEnergyAmount(1,3);
      MmxWeaponsSpend(1,3,energy<64 ? energy : 64);
      s->muzzle_pose=2;s->vx=(r[0xbb9]&64) ? 1024 : -1024;s->vy=-256;
    } else {
      s->vx=silk_radial[id-1][0];s->vy=silk_radial[id-1][1];
      s->x-=s->vx*32;s->y-=s->vy*32;
    }
    animation_start(s,silk_charged_sequence[s->variant][id-1]);
    memset(r+d,0,64);native_object(r,d,s);
    if (!released) putword(r+d+0x20,0);
    ++r[0xbdd];created=true;
  }
  return created;
}
static void silk_player_tick(uint8_t *r) {
  bool held=false,charged=false;
  for (unsigned i=0;i<8;++i) {
    MmxWeaponShot *s=combat.shots+i;
    if (!s->active || s->page!=1 || s->weapon!=3) continue;
    charged|=s->charged!=0;held|=s->charged && s->muzzle_pose<2;
  }
  r[0xc0f]=1;
  if (!charged && combat.held && r[0xc03]==1 && (r[0x1f99]&2) &&
      MmxWeaponsEnergyAmount(1,3) && !silk_blocked_action(r)) {
    /* Original $88:C49E allocates eight pieces at full charge while held.
     * Use free native slots; gathering itself spends no energy. */
    held=silk_create_pieces(r,false);
  }
  if (held) {
    /* Preserve actual held input in combat.held for the gathered pieces,
     * but prevent X1 from creating a second release projectile. */
    stop_charge(r);r[0xbe3]&=(uint8_t)~64;r[0xbdf]&=(uint8_t)~64;
    if (!silk_blocked_action(r)) r[0xbf8]=2;
  }
}
static void silk_effect_tick(MmxWeaponShot *s) {
  if (++s->age>=60) { memset(s,0,sizeof(*s));return; }
  s->y+=s->vy;animation_step(s);
}
static void silk_leaf_trail(const MmxWeaponShot *s) {
  if (combat.tick&3) return;
  for (unsigned i=0;i<16;++i) if (!combat.effects[i].active) {
    MmxWeaponShot *t=combat.effects+i;*t=*s;t->charged=0;t->age=1;t->tether_pose=0;
    t->x+=(bubble_random(i)&7)*256;t->y+=(bubble_random(i+19)&7)*256;
    t->vx=0;t->vy=(int16_t)(256+bubble_random(i+33));animation_start(t,17);return;
  }
}
static void silk_leaf_fall(MmxWeaponShot *s,unsigned id) {
  static const uint8_t sequence[4]={17,23,24,25};
  s->muzzle_pose=3;s->radius=60;s->vx=0;s->vy=(int16_t)(256+bubble_random(id));
  animation_start(s,sequence[bubble_random(id+7)&3]);
}
static void silk_split(uint8_t *r,unsigned d,MmxWeaponShot *s) {
  if (s->charged) {
    if (s->muzzle_pose==3) { retire(r,d);return; }
    s->muzzle_pose=3;
    if (s->variant==3) silk_leaf_fall(s,s->tether_pose);
    else { s->vx=silk_radial[s->tether_pose-1][0];s->vy=silk_radial[s->tether_pose-1][1]; }
    /* Retain each piece's first-target mask, so impact and its outgoing
     * shard cannot damage that same enemy twice during a single contact. */
    return;
  }
  MmxWeaponShot parent=*s;retire(r,d);
  for (unsigned id=4;id;--id) {
    MmxWeaponShot *t=spawn_child(r,&parent);if (!t) break;
    t->muzzle_pose=3;t->tether_pose=(uint8_t)id;
    animation_start(t,silk_fragment_sequence[t->variant][id&1]);
    if (t->variant==3) {
      static const int8_t offsets[4][2]={{0,8},{4,-8},{8,4},{-8,-5}};
      t->x+=(offsets[id-1][0]+(int)(bubble_random(id)&7))*256;
      t->y+=(offsets[id-1][1]+(int)(bubble_random(id+8)&7))*256;
      silk_leaf_fall(t,id);
    } else {
      int speed=t->variant==4 ? 1280 : 1024;
      t->vx=(int16_t)(id&1 ? speed : -speed);
      t->vy=(int16_t)(id>=3 ? speed : -speed);
    }
  }
}
static void silk_tick(uint8_t *r,unsigned d,MmxWeaponShot *s) {
  if (!s->age) {
    /* The normal shot is single-fire; source fragments keep that limit
     * occupied until all have left. A release after charge cancellation
     * can still enter here as a charged native command. */
    if (s->charged) { retire(r,d);silk_create_pieces(r,true);return; }
    if (!MmxWeaponsSpend(1,3,256)) { retire(r,d);return; }
    s->variant=(uint8_t)silk_material(r);s->group=(uint8_t)silk_group(s->variant);
    s->age=1;s->born=combat.tick;muzzle_origin(r,d,s);
    s->vx=s->facing ? 512 : -512;s->vy=-768;
    static const uint8_t bounces[5]={2,3,1,1,127};s->radius=bounces[s->variant];
    animation_start(s,silk_normal_sequence[s->variant][bubble_random(d)&1]);
    if (s->variant==3) { s->muzzle_pose=1;s->radius=20;s->vx=s->facing ? 1280 : -1280;s->vy=0; }
    native_object(r,d,s);return;
  }
  if (s->born==combat.tick) return;
  bool contact=r[d+1]>=4;
  if (s->charged && s->muzzle_pose<2) {
    if (!combat.held || silk_blocked_action(r)) {
      unsigned energy=MmxWeaponsEnergyAmount(1,3);
      MmxWeaponsSpend(1,3,energy<64 ? energy : 64); /* $86:AF44 / eight. */
      silk_held_origin(r,d,s);s->vx=(r[0xbb9]&64) ? 1024 : -1024;s->vy=-256;
      s->muzzle_pose=2;s->hit_slots=0;
    } else if (!s->muzzle_pose && s->radius) {
      s->x+=((int)word(r+0xbad)-s->origin_x)*256+s->vx;
      s->y+=((int)word(r+0xbb0)-s->origin_y)*256+s->vy;
      s->origin_x=(int16_t)word(r+0xbad);s->origin_y=(int16_t)word(r+0xbb0);
      if (!--s->radius) { s->muzzle_pose=1;silk_held_origin(r,d,s); }
    } else silk_held_origin(r,d,s);
  } else if (contact) {
    silk_split(r,d,s);
    if (!s->active || !s->charged) return;
  } else if (s->muzzle_pose==3) {
    s->x+=s->vx;s->y+=s->vy;
    if (s->variant==3 && (!s->radius || !--s->radius)) { retire(r,d);return; }
    animation_step(s);
  } else if (s->charged) {
    /* Source checks the leading piece centers, then scatters the eight
     * pieces on terrain. Native damage gives the same break on an enemy. */
    s->vy+=32;if (s->vy>1280) s->vy=1280;
    if (terrain_move(r,s,0,0)) silk_split(r,d,s);
  } else if (s->variant==3) {
    unsigned hit;
    if (s->muzzle_pose==1) {
      s->vx+=(int16_t)(s->facing ? -64 : 64);s->vy-=32;if (s->vy< -768) s->vy=-768;
      hit=terrain_move(r,s,0,0);silk_leaf_trail(s);
      if (hit || !--s->radius) { s->muzzle_pose=2;s->vx=0;s->vy=-256; }
    } else {
      s->vy+=32;hit=terrain_move(r,s,0,0);
      if (hit || s->vy>0) { silk_split(r,d,s);return; }
    }
  } else {
    static const uint8_t gravity[5]={96,64,64,32,64};
    static const uint16_t rebound[5]={384,256,512,1280,16};
    s->vy+=gravity[s->variant];if (s->vy>2048) s->vy=2048;
    unsigned hit=terrain_move(r,s,8,8);
    if (hit&1) { silk_split(r,d,s);return; }
    if (hit&8) s->vy=0;
    if (hit&4) {
      if (!--s->radius) { silk_split(r,d,s);return; }
      s->vy=(int16_t)(-s->vy+(int)rebound[s->variant]);if (s->vy>=0) s->vy=-256;
    }
  }
  if (!s->active) { retire(r,d);return; }
  if (s->age<65535) ++s->age; /* A held cluster has no source time limit. */
  if ((!s->charged || s->muzzle_pose>=2) &&
      (s->x/256 < (int)camera_x(r)-32 || s->x/256 >= (int)camera_x(r)+288 ||
       s->y/256 < (int)camera_y(r)-16 || s->y/256 >= (int)camera_y(r)+240)) {
    retire(r,d);return;
  }
  native_object(r,d,s);
  if (s->charged && s->muzzle_pose<2) putword(r+d+0x20,0);
}
