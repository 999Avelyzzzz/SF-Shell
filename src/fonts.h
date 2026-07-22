#pragma once

/* Configura fontconfig affinche' la shell veda ESCLUSIVAMENTE i font
 * contenuti in src/fonts, ignorando /usr/share/fonts e ogni altro font
 * di sistema. Va chiamata all'inizio di main(), prima che GTK/Pango
 * creino la loro fontmap. */
void fonts_init(void);
