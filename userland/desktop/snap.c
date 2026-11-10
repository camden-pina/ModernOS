#include "desktop.h"

/* Fractions are evaluated at the edges, so odd work areas tile without seams. */
static const int slots[] = {2, 2, 3, 4, 3, 3};
static Rect fraction(Rect r, int x, int y, int w, int h, int dx, int dy) {
  int left = r.w * x / dx, top = r.h * y / dy;
  return (Rect){r.x + left, r.y + top,
                r.w * (x + w) / dx - left, r.h * (y + h) / dy - top};
}
static Rect slot_rect(Rect r, int layout, int slot) {
  if (layout == 0) return fraction(r, slot, 0, 1, 1, 2, 1);
  if (layout == 1) return fraction(r, slot ? 2 : 0, 0, slot ? 1 : 2, 1, 3, 1);
  if (layout == 2) return fraction(r, slot, 0, 1, 1, 3, 1);
  if (layout == 3) return fraction(r, slot % 2, slot / 2, 1, 1, 2, 2);
  if (layout == 4) return slot == 0 ? fraction(r, 0, 0, 1, 2, 2, 2)
                                     : fraction(r, 1, slot - 1, 1, 1, 2, 2);
  return slot == 2 ? fraction(r, 1, 0, 1, 2, 2, 2)
                   : fraction(r, 0, slot, 1, 1, 2, 2);
}
Rect snap_geometry(int layout, int slot) {
  Rect work = {0, S(DESKTOP_TOP), desk.width,
               desk.height - S(DESKTOP_TOP) - desktop_bottom()};
  if (layout < 0) return work;
  if (layout >= 6 || slot < 0 || slot >= slots[layout]) return (Rect){0};
  return slot_rect(work, layout, slot);
}
static struct {
  int layout, workspace, next, focus, count;
  Window *placed[4], *candidates[MAX_WINDOWS];
  int placed_id[4], candidate_id[MAX_WINDOWS];
  Rect cards[MAX_WINDOWS];
  uint64_t revisions[MAX_WINDOWS];
} assist;
static bool candidate(Window *w) {
  if (!w->alive || w->minimized || w->fullscreen || w->owner ||
      w->workspace!=assist.workspace) return false;
  for(int i=0;i<slots[assist.layout];i++)
    if(assist.placed[i]==w && assist.placed_id[i]==w->id) return false;
  return true;
}
bool snap_window_hidden(Window *w) { return desk.snap_assist && candidate(w); }
void snap_cancel(void) {
  if (desk.snap_open) transition_begin(desk.snap_box);
  if (desk.snap_assist) { transition_begin(snap_geometry(-1,0));desk.snap_assist=false;invalidate(snap_geometry(-1,0)); }
  if (desk.snap_open) invalidate(desk.snap_box);
  if (desk.snap_preview) invalidate(desk.snap_target);
  desk.snap_open = desk.snap_preview = false;
  desk.snap_bar = false;
  desk.snap_owner = NULL;
  desk.snap_hover_at = 0;
}
void snap_apply(Window *w, int layout, int slot) {
  Rect target = snap_geometry(layout, slot);
  if (!w || !w->alive || empty(target)) return;
  if (w->fullscreen) wm_fullscreen(w, false);
  damage_window(w);
  if (!w->maximized && !w->snapped) w->restore = w->box;
  w->box = target;
  w->maximized = layout < 0;
  w->snapped = layout >= 0;
  w->snap_layout = layout;
  w->snap_slot = slot;
  invalidate_window(w);
  wm_trace_geometry(w);
  trace("SNAP", w, layout < 0 ? "maximize" : "layout");
}
/* A selection session leaves unchosen windows untouched. Their live previews
   are compressed into free cells; Escape restores them immediately. */
static void assist_refresh(void) {
  if (!desk.snap_assist) return;
  int free=0;assist.next=-1;
  for(int i=0;i<slots[assist.layout];i++) {
    Window *w=assist.placed[i];
    if(w && (!w->alive || w->id!=assist.placed_id[i] || w->minimized ||
        w->workspace!=assist.workspace || !w->snapped || w->snap_layout!=assist.layout || w->snap_slot!=i)) {
      snap_cancel();return;
    }
    if(!w) {free++;if(assist.next<0)assist.next=i;}
  }
  assist.count=0;
  for(int i=0;i<MAX_WINDOWS;i++)if(candidate(&desk.windows[i])) {
    int at=assist.count++;
    assist.candidates[at]=&desk.windows[i];assist.candidate_id[at]=desk.windows[i].id;
  }
  /* Stable MRU ordering makes keyboard choice and repeated redraws predictable. */
  for(int i=1;i<assist.count;i++)for(int j=i;j>0 && assist.candidates[j]->z>assist.candidates[j-1]->z;j--) {
    Window *w=assist.candidates[j];assist.candidates[j]=assist.candidates[j-1];assist.candidates[j-1]=w;
    int id=assist.candidate_id[j];assist.candidate_id[j]=assist.candidate_id[j-1];assist.candidate_id[j-1]=id;
  }
  if(!free || !assist.count) {snap_cancel();return;}
  if(free==1 && assist.count==1) {
    Window *last=assist.candidates[0];snap_apply(last,assist.layout,assist.next);
    snap_cancel();return;
  }
  assist.focus=clampi(assist.focus,0,assist.count-1);
  for(int i=0;i<assist.count;i++) {
    int cell_index=i%free,cell=-1;
    for(int j=0;j<slots[assist.layout];j++)if(!assist.placed[j] && !cell_index--) {cell=j;break;}
    Rect area=snap_geometry(assist.layout,cell);
    int n=(assist.count+free-1-i%free)/free,columns=1;
    while(columns*columns<n)columns++;
    int rows=(n+columns-1)/columns,at=i/free;
    area.x+=S(12);area.y+=S(44);area.w-=S(24);area.h-=S(56);
    Rect r=fraction(area,at%columns,at/columns,1,1,columns,rows);
    r.x+=S(5);r.y+=S(5);r.w-=S(10);r.h-=S(10);
    Window *w=assist.candidates[i];
    int height=MIN(r.h,S(42)+(int64_t)MAX(1,r.w-S(16))*w->box.h/MAX(1,w->box.w));
    r.y+=(r.h-height)/2;r.h=height;
    bool changed=memcmp(&assist.cards[i],&r,sizeof(r)) || assist.revisions[i]!=w->surface_revision;
    assist.cards[i]=r;assist.revisions[i]=w->surface_revision;
    if(changed)invalidate(r);
  }
}
static void assist_choose(int at) {
  if(at<0 || at>=assist.count)return;
  Window *w=assist.candidates[at];
  if(!candidate(w) || w->id!=assist.candidate_id[at]) {assist_refresh();return;}
  transition_begin(snap_geometry(-1,0));
  assist.placed[assist.next]=w;assist.placed_id[assist.next]=w->id;
  snap_apply(w,assist.layout,assist.next);wm_focus(w);
  assist_refresh();invalidate(snap_geometry(-1,0));
}
static void arrange(Window *w, int layout, int slot) {
  snap_cancel();
  transition_begin(snap_geometry(-1,0));
  snap_apply(w,layout,slot);
  if(layout<0 || !desk.snap_assist_enabled)return;
  memset(&assist,0,sizeof(assist));
  assist.layout=layout;assist.workspace=w->workspace;
  assist.placed[slot]=w;assist.placed_id[slot]=w->id;
  desk.snap_assist=true;
  assist_refresh();invalidate(snap_geometry(-1,0));
  trace("SNAP_ASSIST",w,desk.snap_assist?"choose next window":"complete");
}
static void clear_preview(void) {
  if (!desk.snap_preview) return;
  invalidate(desk.snap_target);
  desk.snap_preview = false;
  if (desk.snap_open) invalidate(desk.snap_box);
}
static void preview(Window *w, int layout, int slot) {
  Rect target = snap_geometry(layout, slot);
  if (desk.snap_preview && desk.snap_owner == w && desk.snap_layout == layout &&
      desk.snap_slot == slot && !memcmp(&desk.snap_target, &target, sizeof(target))) return;
  if (desk.snap_preview) invalidate(desk.snap_target);
  desk.snap_owner = w;
  desk.snap_owner_id = w->id;
  desk.snap_layout = layout;
  desk.snap_slot = slot;
  desk.snap_target = target;
  desk.snap_preview = !empty(target);
  invalidate(target);
  if (desk.snap_open) invalidate(desk.snap_box);
}
void snap_show(Window *w) {
  if (!w || !w->alive || w->fullscreen) return;
  snap_cancel();
  desk.snap_owner = w;
  desk.snap_owner_id = w->id;
  int width = MIN(S(360), desk.width), height = S(188);
  Rect control = window_control(w, 2);
  desk.snap_box = (Rect){clampi(control.x, 0, desk.width - width),
    clampi(w->box.y + S(WINDOW_HEADER), S(DESKTOP_TOP),
           desk.height - desktop_bottom() - height), width, height};
  transition_begin(desk.snap_box);
  desk.snap_open = true;
  desk.snap_keyboard = true;
  preview(w, 0, 0);
  invalidate(desk.snap_box);
  trace("SNAP_LAYOUTS", w, "open");
}
static Rect diagram(int i) {
  Rect b = desk.snap_box;
  if (desk.snap_bar) {
    int width = (b.w - S(70)) / 6;
    return (Rect){b.x + S(10) + i * (width + S(10)), b.y + S(35), width, S(56)};
  }
  int width = (b.w - S(40)) / 3;
  return (Rect){b.x + S(10) + i % 3 * (width + S(10)),
                b.y + S(40) + i / 3 * S(70), width, S(56)};
}
static bool hover_choice(Window *w, int x, int y) {
  for (int i = 0; i < 6; i++) for (int j = 0; j < slots[i]; j++) {
    Rect r = slot_rect(diagram(i), i, j);
    r.x += S(2); r.y += S(2); r.w -= S(4); r.h -= S(4);
    if (contains(r, x, y)) { preview(w, i, j); return true; }
  }
  clear_preview();
  return false;
}
bool snap_mouse(int x, int y, unsigned pressed) {
  if(desk.snap_assist) {
    if(y<S(DESKTOP_TOP) || y>=desk.height-desktop_bottom()) {if(pressed)snap_cancel();return false;}
    for(int i=0;i<assist.count;i++)if(contains(assist.cards[i],x,y)) {
      if(assist.focus!=i) {assist.focus=i;invalidate(snap_geometry(-1,0));}
      if(pressed&1) {desk.snap_pointer_blocked=true;assist_choose(i);}
      return true;
    }
    if(pressed) {desk.snap_pointer_blocked=true;snap_cancel();}
    return true;
  }
  if (!desk.snap_open) return false;
  if (!contains(desk.snap_box, x, y)) {
    if (!desk.snap_keyboard) clear_preview();
    if (pressed) snap_cancel();
    return false;
  }
  desk.snap_keyboard = false;
  if (hover_choice(desk.snap_owner, x, y)) {
    if (pressed & 1) {
      /* The compositor polls native pointer state separately from dispatch.
         Keep this popup click captured through release, including after it
         closes, so the editor beneath it cannot receive the same press. */
      desk.snap_pointer_blocked = true;
      arrange(desk.snap_owner, desk.snap_layout, desk.snap_slot);
    }
    return true;
  }
  return true;
}
bool snap_key(Key k) {
  if(desk.snap_assist && k.down && k.code<0xe0) {
    if(k.code==KEY_ESCAPE) snap_cancel();
    else if(k.code==KEY_ENTER) assist_choose(assist.focus);
    else if(k.code==KEY_TAB || k.code==KEY_RIGHT || k.code==KEY_DOWN || k.code==KEY_LEFT || k.code==KEY_UP) {
      int step=(k.code==KEY_LEFT || k.code==KEY_UP || (k.mods&MOD_SHIFT))?-1:1;
      assist.focus=(assist.focus+step+assist.count)%assist.count;invalidate(snap_geometry(-1,0));
    } else {snap_cancel();return false;}
    return true;
  }
  if (!desk.snap_open || !k.down || k.code >= 0xe0) return false;
  if (k.code == KEY_ESCAPE) { snap_cancel(); return true; }
  if (k.code == KEY_ENTER) {
    if (desk.snap_preview) arrange(desk.snap_owner, desk.snap_layout, desk.snap_slot);
    else snap_cancel();
    return true;
  }
  int layout = desk.snap_layout, slot = desk.snap_slot;
  if (k.code == KEY_TAB || k.code == KEY_DOWN || k.code == KEY_UP) {
    layout = (layout + (k.code == KEY_UP || (k.mods & MOD_SHIFT) ? 5 : 1)) % 6;
    slot = 0;
  } else if (k.code == KEY_LEFT || k.code == KEY_RIGHT) {
    slot = (slot + (k.code == KEY_LEFT ? slots[layout] - 1 : 1)) % slots[layout];
  } else { snap_cancel(); return false; }
  desk.snap_keyboard = true;
  preview(desk.snap_owner, layout, slot);
  return true;
}
void snap_tick(void) {
  if (desk.dialog.open || desk.menu.open || desk.launcher || wm_fullscreen_active()) {
    snap_cancel(); return;
  }
  if (desk.snap_assist) {
    if(assist.workspace!=desk.workspace) snap_cancel();
    else assist_refresh();
    return;
  }
  if (desk.snap_open) {
    Window *w = desk.snap_owner;
    if (!w || !w->alive || w->id != desk.snap_owner_id ||
        w->workspace != desk.workspace || w->minimized) { snap_cancel(); return; }
    Rect bridge = window_control(w, 2);
    bridge.h = desk.snap_box.y + S(6) - bridge.y;
    if (!desk.snap_keyboard && !desk.drag && !contains(desk.snap_box, desk.mouse_x, desk.mouse_y) &&
        !contains(bridge, desk.mouse_x, desk.mouse_y)) snap_cancel();
    return;
  }
  if (desk.drag) return;
  Window *w = wm_hit_test(desk.mouse_x, desk.mouse_y);
  if (desk.snap_hover_enabled && w && !w->fullscreen && contains(window_control(w, 2), desk.mouse_x, desk.mouse_y)) {
    if (desk.snap_owner != w) { desk.snap_owner = w; desk.snap_hover_at = now_ms(); }
    if (now_ms() - desk.snap_hover_at >= 450) {
      snap_show(w); desk.snap_keyboard = false; clear_preview();
    }
  } else { desk.snap_owner = NULL; desk.snap_hover_at = 0; }
}
void snap_drag(Window *w, int x, int y, bool finished) {
  int top = S(DESKTOP_TOP), bottom = desk.height - desktop_bottom();
  /* A shallow top-center target expands into a horizontal snap bar. The bar
     remains reachable while the title bar continues following the pointer. */
  bool center = x >= desk.width / 2 - MIN(S(380), desk.width / 2) &&
                x < desk.width / 2 + MIN(S(380), desk.width / 2);
  if (desk.snap_bar_enabled && ((center && y < top + S(38)) ||
      (desk.snap_bar && center && y < top + S(128)))) {
    if (!desk.snap_bar) {
      snap_cancel();
      desk.snap_owner = w; desk.snap_owner_id = w->id;
      int width = MIN(S(720), desk.width);
      desk.snap_box = (Rect){(desk.width - width) / 2, top + S(4), width, S(112)};
      desk.snap_open = desk.snap_bar = true; desk.snap_keyboard = false;
      invalidate(desk.snap_box);
      trace("SNAP_BAR", w, "open");
    }
    hover_choice(w, x, y);
    if (finished) {
      if (desk.snap_preview) arrange(w, desk.snap_layout, desk.snap_slot);
      else snap_cancel();
    }
    return;
  }
  if (desk.snap_bar) snap_cancel();
  int layout = -2, slot = 0;
  if (x < S(12) || x >= desk.width - S(12)) {
    bool right = x >= desk.width / 2;
    layout = 0; slot = right;
    if (y < top + S(72) || y >= bottom - S(72)) {
      layout = 3; slot = (y >= bottom - S(72) ? 2 : 0) + right;
    }
  } else if (y <= top + S(8)) layout = -1;
  if (layout == -2) {
    clear_preview();
  } else preview(w, layout, slot);
  if (finished) {
    if (desk.snap_preview) arrange(w, desk.snap_layout, desk.snap_slot);
    else snap_cancel();
  }
}
void snap_draw(void) {
  if(desk.snap_assist) {
    for(int i=0;i<slots[assist.layout];i++)if(!assist.placed[i]) {
      Rect r=snap_geometry(assist.layout,i);
      blend(r,desk.accent,i==assist.next?38:16);
      r.x+=S(5);r.y+=S(5);r.w-=S(10);r.h-=S(10);
      border(r,S(8),i==assist.next?desk.accent:THEME_WINDOW_BORDER);
      label(r.x+S(16),r.y+S(14),i==assist.next?"Choose a window for this space":"Available space",
        FONT_BOLD,i==assist.next?THEME_TEXT:THEME_SECONDARY_TEXT,r.w-S(32));
    }
    for(int i=0;i<assist.count;i++) {
      Window *w=assist.candidates[i];Rect r=assist.cards[i];
      if(r.w<S(20) || r.h<S(32))continue;
      rounded(r,S(8),THEME_WINDOW);
      border(r,S(8),i==assist.focus?desk.accent:THEME_WINDOW_BORDER);
      icon(r.x+S(10),r.y+S(9),apps[w->app].icon,S(16),desk.accent);
      label(r.x+S(34),r.y+S(10),w->title,FONT_BODY,THEME_TEXT,r.w-S(46));
      window_thumbnail(w,(Rect){r.x+S(8),r.y+S(34),r.w-S(16),r.h-S(42)});
    }
  }
  if (desk.snap_preview) {
    Rect r = desk.snap_target;
    r.x += S(6); r.y += S(6); r.w -= S(12); r.h -= S(12);
    blend(r, desk.accent, 55);
    border(r, S(8), desk.accent);
  }
  if (!desk.snap_open) return;
  Rect b = desk.snap_box;
  rounded(b, S(9), THEME_WINDOW);
  border(b, S(9), THEME_WINDOW_BORDER);
  label(b.x + S(12), b.y + S(12), desk.snap_bar ? "Drop into a layout" : "Snap layouts",
        FONT_BOLD, THEME_TEXT, b.w - S(24));
  if (desk.snap_bar) label(b.x + S(12), b.y + S(96), "Choose each window to fill the remaining spaces",
                           FONT_SMALL, THEME_SECONDARY_TEXT, b.w - S(24));
  for (int i = 0; i < 6; i++) for (int j = 0; j < slots[i]; j++) {
    Rect r = slot_rect(diagram(i), i, j);
    r.x += S(2); r.y += S(2); r.w -= S(4); r.h -= S(4);
    bool active = desk.snap_preview && desk.snap_layout == i && desk.snap_slot == j;
    rounded(r, S(3), active ? desk.accent : THEME_TOOLBAR);
    border(r, S(3), active ? desk.accent : THEME_WINDOW_BORDER);
  }
}
