static void sn_write(PSGContext *ctx, SN76489 *p, uint8_t b)
{
    if (b & 0x80) {                       /* LATCH/DATA byte: %1 cc t dddd */
        p->latch = (b >> 4) & 0x07;
        uint8_t chan = (b >> 5) & 0x03;
        uint8_t type = (b >> 4) & 0x01;   /* 1 = volume, 0 = tone/noise */
        if (type)            p->vol[chan] = b & 0x0F;
        else if (chan < 3)   p->tone_period[chan] = (uint16_t)((p->tone_period[chan] & 0x3F0) | (b & 0x0F));
        else               { p->noise_ctrl = b & 0x07; p->lfsr = 0x8000; }
    } else {                              /* DATA byte: %0 - dddddd */
        uint8_t chan = (p->latch >> 1) & 0x03;
        uint8_t type = p->latch & 0x01;
        if (type)            p->vol[chan] = b & 0x0F;
        else if (chan < 3)   p->tone_period[chan] = (uint16_t)((p->tone_period[chan] & 0x00F) | ((uint16_t)(b & 0x3F) << 4));
        else               { p->noise_ctrl = b & 0x07; p->lfsr = 0x8000; }
    }
}
static int sn_noise_reload(PSGContext *ctx, const SN76489 *p)
{
    switch (p->noise_ctrl & 0x03) {
        case 0:  return 0x10;
        case 1:  return 0x20;
        case 2:  return 0x40;
        default: return p->tone_period[2] ? p->tone_period[2] : 1;  /* track ch2 */
    }
}
