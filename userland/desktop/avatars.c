#include "desktop.h"
void avatar_draw(Rect r, unsigned choice) {
  static const uint32_t bg[] = {0x367EB5, 0x6977A4, 0x4B806D,
                                0xAA6B72, 0x42616F, 0xB18A49};
  unsigned at = choice % 6;
  rounded(r, r.w / 2, bg[at]);
  Rect saved = desk.clip;
  clip_set(intersect(saved, r));
  int q = MAX(1, r.w / 12), x = r.x, y = r.y;
  if (at == 0) {
    rounded((Rect){x + q * 7, y + q * 2, q * 2, q * 2}, q, 0xFFE6A9);
    for (int i = 0; i < 3; i++)
      rounded((Rect){x + q * (2 + i), y + q * (6 + i), q * (8 - i * 2), q},
              q / 2, 0xBDDCE2);
  } else if (at == 1) {
    line(x + q * 2, y + q * 8, x + q * 5, y + q * 3, 0xF4F0DF);
    line(x + q * 5, y + q * 3, x + q * 9, y + q * 8, 0xF4F0DF);
    rounded((Rect){x + q * 3, y + q * 8, q * 6, q}, q / 2, 0xCFD9ED);
  } else if (at == 2) {
    rounded((Rect){x + q * 3, y + q * 3, q * 5, q * 5}, q * 2, 0xC5E1AF);
    line(x + q * 4, y + q * 9, x + q * 7, y + q * 4, 0xF1F0C5);
  } else if (at == 3) {
    rounded((Rect){x + q * 3, y + q * 2, q * 6, q * 7}, q * 3, 0xF7DBB6);
    rounded((Rect){x + q * 5, y + q * 2, q * 5, q * 6}, q * 3, bg[at]);
  } else if (at == 4) {
    rounded((Rect){x + q * 3, y + q * 2, q * 3, q * 3}, q * 2, 0xE3BF7D);
    rounded((Rect){x + q * 5, y + q * 5, q * 4, q * 4}, q * 2, 0xA6CED4);
  } else {
    rounded((Rect){x + q * 4, y + q * 2, q * 4, q * 4}, q * 2, 0xFAE9CF);
    rounded((Rect){x + q * 3, y + q * 7, q * 6, q * 3}, q * 2, 0xFAE9CF);
  }
  clip_set(saved);
}
