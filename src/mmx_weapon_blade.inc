/* X3 $81:B423..B7A8: charged Blade controller and native contact responses.
 * Charged variants: 0 extend, 1/2 orbit up/down, 3 retract, 4 recoil, 5 hold.
 * Radius is pixels; vx is the orbit byte index and vy the hold countdown.
 * The normal pair retains its independent velocities and impact sequence. */
static void blade_tick(uint8_t *r,unsigned d,MmxWeaponShot *s) {
  int direction=s->facing?1:-1;
  if(s->charged && s->age && s->variant!=4) {
    if(r[d+1]==4) {retire(r,d);return;} /* Source reflected actor is discarded. */
    if(r[d+1]>=8) {
      s->variant=4;s->muzzle_pose=s->tether_pose=0;
      /* $B75C resets +/-$0800 each contact-state tick; $82:D6F8 then
       * subtracts the initial $E0 horizontal acceleration ($B48C). */
      s->vx=(int16_t)(-direction*0x800-0xe0);s->vy=-0x800;
    } else if(r[d+1]==6) s->hit_slots=0; /* A killed target does not dislodge it. */
  }
  if(!s->charged && s->age && (r[d+1]>=8 || s->variant==128)) {
    if(s->variant!=128) {s->variant=128;s->vy=26;animation_start(s,1);}
    if(!s->vy || !s->active) {retire(r,d);return;}
    --s->vy;animation_step(s);native_object(r,d,s);putword(r+d+0x20,0);return;
  }
  if(!s->age) {
    if(!MmxWeaponsSpend(s->page,s->weapon,(s->charged?3:1)*256)) {retire(r,d);return;}
    muzzle_origin(r,d,s);s->vx=(int16_t)(direction*1024);s->vy=0;
    animation_start(s,s->charged?4:0);s->born=combat.tick;
    if(!s->charged) {
      unsigned other=free_slot(r);
      if(other) {
        MmxWeaponShot *t=combat.shots+slot_index(other);*t=*s;t->variant=1;t->age=1;
        memset(r+other,0,64);native_object(r,other,t);++r[0xbdd];
      }
    } else {s->vx=0;s->tether_pose=30;s->muzzle_pose=39;}
    s->age=1;native_object(r,d,s);return;
  }
  if(s->age==1 && s->born==combat.tick) return;
  if(s->charged && s->variant!=4) {
    /* Original orbit offsets at $86:BA86. There are 32 positions, each
     * held for two ticks, rather than 64 interpolated positions. */
    static const int8_t orbit[32][2]={
      {0,-80},{15,-78},{29,-74},{44,-66},{56,-56},{65,-43},{71,-28},{75,-15},
      {77,0},{75,15},{71,28},{65,43},{56,56},{44,66},{29,74},{15,78},
      {0,80},{-15,78},{-29,74},{-44,66},{-56,56},{-65,43},{-71,28},{-75,15},
      {-77,0},{-75,-15},{-71,-28},{-65,-43},{-56,-56},{-44,-66},{-29,-74},{-15,-78}};
    muzzle_origin(r,d,s);
    static const uint8_t muzzle[4]={39,40,41,40};
    s->muzzle_pose=muzzle[(s->age/2)&3];
    int dx=s->radius,dy=0;
    if(s->variant==5) {
      if(!--s->vy) s->variant=3;
      else if(combat.direction&12) {
        s->variant=(combat.direction&8)?1:2;s->vx=0;
      }
    } else if(s->variant==1 || s->variant==2) {
      bool up=s->variant==1;
      if(s->age&1) s->vx+=2;
      unsigned angle=(unsigned)s->vx&63;
      dx=orbit[((16-angle)&63)/2][0];dy=orbit[((16-angle)&63)/2][1]*(up?1:-1);
      s->tether_pose=(uint8_t)(42+((24+(up?(int)(angle/2):-(int)(angle/2)))&31));
      if(s->vx>=64) s->variant=3;
    } else {
      if(s->age&1) {
        if(s->variant==3) {
          if(!s->radius) {retire(r,d);return;}
          s->radius=(uint8_t)(s->radius>8?s->radius-8:0);
        } else if(s->radius>=80) {s->variant=5;s->vy=120;}
        else s->radius=(uint8_t)(s->radius>=72?80:s->radius+8);
      }
      dx=s->radius;
      s->tether_pose=(uint8_t)(30+(s->radius>=72?8:s->radius?s->radius/8-1:0));
    }
    s->x+=direction*dx*256;s->y+=dy*256;
  } else if(s->charged) {
    s->x+=s->vx;s->y+=s->vy;
    if(s->x/256<(int)camera_x(r)-32 || s->x/256>(int)camera_x(r)+288 ||
        s->y/256<(int)camera_y(r)-32 || s->y/256>(int)camera_y(r)+256) {retire(r,d);return;}
  } else {
    s->vx-=(int16_t)(direction*32);s->vy+=s->variant?-16:16;
    if(s->vy>192) s->vy=192;
    if(s->vy<-192) s->vy=-192;
    s->x+=s->vx;s->y+=s->vy;
  }
  if(++s->age>240 || s->x/256<(int)camera_x(r)-256 ||
      s->x/256>(int)camera_x(r)+512 || s->y/256<(int)camera_y(r)-256 ||
      s->y/256>(int)camera_y(r)+480) {retire(r,d);return;}
  animation_step(s);
  if(!s->active) {retire(r,d);return;}
  native_object(r,d,s);
  if(s->charged && s->variant==4) {putword(r+d+0x20,0);r[d+0x30]=1;}
}
