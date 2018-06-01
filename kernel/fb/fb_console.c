
#include <common/basic_defs.h>
#include <common/string_util.h>
#include <common/vga_textmode_defs.h>

#include <exos/fb_console.h>
#include <exos/term.h>
#include <exos/hal.h>
#include <exos/kmalloc.h>
#include <exos/process.h>
#include <exos/timer.h>
#include <exos/datetime.h>

#include "fb_int.h"

extern char _binary_font8x16_psf_start;
extern char _binary_font16x32_psf_start;

bool __use_framebuffer;
psf2_header *fb_font_header;

static bool use_optimized;
static u32 fb_term_rows;
static u32 fb_term_cols;
static u32 fb_offset_y;

static bool cursor_enabled;
static int cursor_row;
static int cursor_col;
static u32 *under_cursor_buf;
static volatile bool cursor_visible = true;
static task_info *blink_thread_ti;
static const u32 blink_half_period = (TIMER_HZ * 60)/100;
static u32 cursor_color = fb_make_color(255, 255, 255);

/* Could we really need more than 256 rows? Probably we won't. */
static bool rows_to_flush[256];

static video_interface framebuffer_vi;

u32 vga_rgb_colors[16] =
{
   [COLOR_BLACK] = fb_make_color(0, 0, 0),
   [COLOR_BLUE] = fb_make_color(0, 0, 168),
   [COLOR_GREEN] = fb_make_color(0, 168, 0),
   [COLOR_CYAN] = fb_make_color(0, 168, 168),
   [COLOR_RED] = fb_make_color(168, 0, 0),
   [COLOR_MAGENTA] = fb_make_color(168, 0, 168),
   [COLOR_BROWN] = fb_make_color(168, 168, 0),
   [COLOR_LIGHT_GREY] = fb_make_color(208, 208, 208),
   [COLOR_DARK_GREY] = fb_make_color(168, 168, 168),
   [COLOR_LIGHT_BLUE] = fb_make_color(0, 0, 252),
   [COLOR_LIGHT_GREEN] = fb_make_color(0, 252, 0),
   [COLOR_LIGHT_CYAN] = fb_make_color(0, 252, 252),
   [COLOR_LIGHT_RED] = fb_make_color(252, 0, 0),
   [COLOR_LIGHT_MAGENTA] = fb_make_color(252, 0, 252),
   [COLOR_LIGHT_BROWN] = fb_make_color(252, 252, 0),
   [COLOR_WHITE] = fb_make_color(252, 252, 252)
};

void fb_save_under_cursor_buf(void)
{
   if (!under_cursor_buf)
      return;

   // Assumption: bbp is 32
   psf2_header *h = fb_font_header;

   const u32 ix = cursor_col * h->width;
   const u32 iy = fb_offset_y + cursor_row * h->height;
   fb_copy_from_screen(ix, iy, h->width, h->height, under_cursor_buf);
}

void fb_restore_under_cursor_buf(void)
{
   if (!under_cursor_buf)
      return;

   // Assumption: bbp is 32
   psf2_header *h = fb_font_header;

   const u32 ix = cursor_col * h->width;
   const u32 iy = fb_offset_y + cursor_row * h->height;
   fb_copy_to_screen(ix, iy, h->width, h->height, under_cursor_buf);

   rows_to_flush[cursor_row] = true;
}

static void fb_reset_blink_timer(void)
{
   if (!blink_thread_ti)
      return;

   cursor_visible = true;
   wait_obj *w = &blink_thread_ti->wobj;
   kthread_timer_sleep_obj *timer = w->ptr;

   if (timer) {
      timer->ticks_to_sleep = blink_half_period;
   }
}

/* video_interface */

void fb_set_char_at_failsafe(int row, int col, u16 entry)
{
   psf2_header *h = fb_font_header;

   fb_draw_char_failsafe(col * h->width,
                         fb_offset_y + row * h->height,
                         entry);

   if (row == cursor_row && col == cursor_col)
      fb_save_under_cursor_buf();

   fb_reset_blink_timer();
   rows_to_flush[row] = true;
}

void fb_set_char_at_optimized(int row, int col, u16 entry)
{
   psf2_header *h = fb_font_header;

   fb_draw_char_optimized(col * h->width,
                          fb_offset_y + row * h->height,
                          entry);

   if (row == cursor_row && col == cursor_col)
      fb_save_under_cursor_buf();

   fb_reset_blink_timer();
   rows_to_flush[row] = true;
}



void fb_clear_row(int row_num, u8 color)
{
   psf2_header *h = fb_font_header;
   const u32 iy = fb_offset_y + row_num * h->height;
   fb_raw_color_lines(iy, h->height, vga_rgb_colors[vgaentry_color_bg(color)]);

   if (cursor_row == row_num)
      fb_save_under_cursor_buf();

   rows_to_flush[row_num] = true;
}

void fb_move_cursor(int row, int col)
{
   if (!under_cursor_buf)
      return;

   psf2_header *h = fb_font_header;

   fb_restore_under_cursor_buf();

   rows_to_flush[row] = true;
   rows_to_flush[cursor_row] = true;

   cursor_row = row;
   cursor_col = col;

   if (cursor_enabled) {

      fb_save_under_cursor_buf();

      if (cursor_visible)
         fb_draw_cursor_raw(cursor_col * h->width,
                            fb_offset_y + cursor_row * h->height,
                            cursor_color);
   }
}

void fb_enable_cursor(void)
{
   cursor_enabled = true;
   fb_move_cursor(cursor_row, cursor_col);
}

void fb_disable_cursor(void)
{
   cursor_enabled = false;
   fb_move_cursor(cursor_row, cursor_col);
}

static void fb_set_row_failsafe(int row, u16 *data, bool flush)
{
   for (u32 i = 0; i < fb_term_cols; i++)
      fb_set_char_at_failsafe(row, i, data[i]);

   fb_reset_blink_timer();
   rows_to_flush[row] = true;
}

static void fb_set_row_optimized(int row, u16 *data, bool flush)
{
   psf2_header *h = fb_font_header;

   fb_draw_char_optimized_row(fb_offset_y + row * h->height,
                              data,
                              fb_term_cols);

   if (flush) {
      fb_flush_lines(fb_offset_y + fb_font_header->height * row,
                     fb_font_header->height);
   } else {
      rows_to_flush[row] = true;
   }

   fb_reset_blink_timer();
}

static void fb_scroll_one_line_up(void)
{
   psf2_header *h = fb_font_header;

   bool enabled = cursor_enabled;

   if (enabled)
      fb_disable_cursor();

   fb_lines_shift_up(fb_offset_y + h->height, /* source: row 1 (+ following) */
                     fb_offset_y,             /* destination: row 0 */
                     fb_get_height() - fb_offset_y - h->height);

   if (enabled)
      fb_enable_cursor();

   for (u32 r = 0; r < fb_term_rows; r++)
      rows_to_flush[r] = true;
}

static void fb_flush(void)
{
   for (u32 r = 0; r < fb_term_rows; r++) {

      if (!rows_to_flush[r])
         continue;

      fb_flush_lines(fb_offset_y + fb_font_header->height * r,
                     fb_font_header->height);

      rows_to_flush[r] = false;
   }
}

// ---------------------------------------------

static video_interface framebuffer_vi =
{
   fb_set_char_at_failsafe,
   fb_set_row_failsafe,
   fb_clear_row,
   fb_move_cursor,
   fb_enable_cursor,
   fb_disable_cursor,
   fb_scroll_one_line_up,
   fb_flush
};


static void fb_blink_thread()
{
   while (true) {
      cursor_visible = !cursor_visible;
      fb_move_cursor(cursor_row, cursor_col);
      fb_flush();
      kernel_sleep(blink_half_period);
   }
}

static void fb_draw_string_at_raw(u32 x, u32 y, const char *str, u8 color)
{
   psf2_header *h = fb_font_header;

   if (use_optimized)

      for (; *str; str++, x += h->width)
         fb_draw_char_optimized(x, y, make_vgaentry(*str, color));
   else

      for (; *str; str++, x += h->width)
         fb_draw_char_failsafe(x, y, make_vgaentry(*str, color));
}

static void fb_setup_banner(void)
{
   psf2_header *h = fb_font_header;

   fb_offset_y = (20 * h->height)/10;
   fb_raw_color_lines(0, fb_offset_y, 0 /* black */);
   fb_raw_color_lines(fb_offset_y - 4, 1, vga_rgb_colors[COLOR_WHITE]);
}

static void fb_draw_banner(void)
{
   psf2_header *h = fb_font_header;
   char lbuf[fb_term_cols + 1];
   char rbuf[fb_term_cols + 1];
   int llen, rlen, padding, i;
   datetime_t d;

   ASSERT(fb_offset_y >= h->height);

   read_system_clock_datetime(&d);

   llen = snprintk(lbuf, sizeof(lbuf) - 1 - 1,
                   "exOS [%s build] framebuffer console", BUILDTYPE_STR);

   rlen = snprintk(rbuf, sizeof(rbuf) - 1 - llen - 1,
                   "%s%i/%s%i/%i %s%i:%s%i",
                   d.day < 10 ? "0" : "",
                   d.day,
                   d.month < 10 ? "0" : "",
                   d.month,
                   d.year,
                   d.hour < 10 ? "0" : "",
                   d.hour,
                   d.min < 10 ? "0" : "",
                   d.min);

   padding = (fb_term_cols - llen - rlen - 1);

   for (i = llen; i < llen + padding; i++)
      lbuf[i] = ' ';

   memcpy(lbuf + i, rbuf, rlen);
   lbuf[fb_term_cols - 1] = 0;

   fb_draw_string_at_raw(h->width/2, h->height/2, lbuf, COLOR_LIGHT_BROWN);
}

static void fb_update_banner_kthread()
{
   while (true) {
      fb_draw_banner();
      fb_flush_lines(0, fb_offset_y);
      kernel_sleep(60 * TIMER_HZ);
   }
}

static void fb_use_optimized_funcs_if_possible(void)
{
   if (in_panic())
      return;

   if (fb_get_bpp() != 32) {
      printk("[fb_console] WARNING: using slower code for bpp = %d\n",
             fb_get_bpp());
      printk("[fb_console] switch to a resolution with bpp = 32 if possible\n");
      return;
   }

   if (!fb_pre_render_char_scanlines()) {
      printk("WARNING: fb_pre_render_char_scanlines failed.\n");
      return;
   }

   use_optimized = true;
   framebuffer_vi.set_char_at = fb_set_char_at_optimized;
   framebuffer_vi.set_row = fb_set_row_optimized;
   printk("[fb_console] Use optimized functions\n");
}

void init_framebuffer_console(void)
{
   fb_font_header = fb_get_width() / 8 < 160
                        ? (void *)&_binary_font8x16_psf_start
                        : (void *)&_binary_font16x32_psf_start;

   psf2_header *h = fb_font_header;

   ASSERT(h->magic == PSF2_FONT_MAGIC); // Support only PSF2
   ASSERT(!(h->width % 8)); // Support only fonts with width = 8, 16, 24, 32, ..

   fb_map_in_kernel_space();

   // For the moment, the pitch_size_buf is not used.
   // if (!in_panic())
   //    if (framebuffer_vi.scroll_one_line_up || framebuffer_vi.flush_buffers)
   //       fb_alloc_pitch_size_buf();

   if (framebuffer_vi.flush_buffers && !in_panic()) {

      /*
       * In hypervisors, using double buffering just slows the fb_console,
       * therefore, we enable it only when running on bare-metal.
       */

      if (fb_alloc_shadow_buffer()) {
         printk("[fb_console] Using double buffering\n");
      } else {
         printk("WARNING: unable to use double buffering for the framebuffer\n");
      }
   }

   fb_setup_banner();

   fb_term_rows = (fb_get_height() - fb_offset_y) / h->height;
   fb_term_cols = fb_get_width() / h->width;

   if (!in_panic()) {
      under_cursor_buf = kmalloc(sizeof(u32) * h->width * h->height);

      if (!under_cursor_buf)
         printk("WARNING: fb_console: unable to allocate under_cursor_buf!\n");
   }

   init_term(&framebuffer_vi, fb_term_rows, fb_term_cols, COLOR_WHITE);
   printk("[fb_console] screen resolution: %i x %i x %i bpp\n",
          fb_get_width(), fb_get_height(), fb_get_bpp());
   printk("[fb_console] font size: %i x %i, term size: %i x %i\n",
          h->width, h->height, fb_term_cols, fb_term_rows);

   fb_use_optimized_funcs_if_possible();

   if (in_panic())
      return;

   blink_thread_ti = kthread_create(fb_blink_thread, NULL);

   if (!blink_thread_ti) {
      printk("WARNING: unable to create the fb_blink_thread\n");
   }

   if (fb_offset_y) {
      kthread_create(fb_update_banner_kthread, NULL);
   }
}
