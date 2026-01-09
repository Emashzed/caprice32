/* Caprice32 - Amstrad CPC Emulator
   (c) Copyright 1997-2004 Ulrich Doewich

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
*/

/*
   This file includes video filters from the SMS Plus/SDL 
   sega master system emulator :
   (c) Copyright Gregory Montoir
   http://membres.lycos.fr/cyxdown/smssdl/
*/

/*
   This file includes video filters from MAME
   (Multiple Arcade Machine Emulator) :
   (c) Copyright The MAME Team
   http://www.mame.net/
*/

#include "video.h"
#include "cap32.h"
#include "log.h"
#include "glfuncs.h"
#ifdef HAVE_GL
#include "SDL_opengl.h"
#endif
#include <math.h>
#include <memory>
#include <iostream>

using Uint8  = std::uint8_t;
using Uint16 = std::uint16_t;
using Uint32 = std::uint32_t;

SDL_Window* mainSDLWindow = nullptr;
SDL_Renderer* renderer = nullptr;
SDL_Texture* texture = nullptr;
SDL_GLContext glcontext;

// the video surface ready to display
SDL_Surface* vid = nullptr;
// the video surface scaled with same format as pub
SDL_Surface* scaled = nullptr;
// the video surface shown by the plugin to the application
SDL_Surface* pub = nullptr;

int offset_x;
int offset_y;
int width;
int height;

extern t_CPC CPC;

#ifndef min
#define min(a,b) ((a)<(b) ? (a) : (b))
#endif

#ifndef max
#define max(a,b) ((a)>(b) ? (a) : (b))
#endif

// checks for an OpenGL extension
#ifdef HAVE_GL
static bool have_gl_extension (const char *nom_ext)
{
   const char *ext;
   ext = reinterpret_cast<const char *> (eglGetString (GL_EXTENSIONS));
   const char *f;
   if (ext == nullptr)
      return false;
   f = ext + strlen (ext);
   while (ext < f)
   {
      unsigned int n = strcspn (ext, " ");
      if ((strlen (nom_ext) == n) && (strncmp (nom_ext, ext, n) == 0))
         return true;
      ext += (n + 1);
   }
   return false;
}
#endif

// Returns a bpp compatible with the renderer
int renderer_bpp(SDL_Renderer *sdl_renderer)
{
  SDL_RendererInfo infos;
  SDL_GetRendererInfo(sdl_renderer, &infos);
  return SDL_BITSPERPIXEL(infos.texture_formats[0]);
}

/* ------------------------------------------------------------------------------------ */
/* Unfiltered video plugin (direct blit) ---------------------------------------------- */
/* ------------------------------------------------------------------------------------ */
SDL_Surface* direct_init(video_plugin* t __attribute__((unused)), int scale, bool fs)
{
  Uint32 flags = SDL_WINDOW_ALLOW_HIGHDPI;
  if (fs) {
    if (CPC.scr_full_screen_exclusive) {
      flags |= SDL_WINDOW_FULLSCREEN;
    } else {
      flags |= SDL_WINDOW_FULLSCREEN_DESKTOP;
    }
  } else {
    flags |= SDL_WINDOW_SHOWN;
  }  
  
  SDL_CreateWindowAndRenderer(CPC_VISIBLE_SCR_WIDTH*scale, CPC_VISIBLE_SCR_HEIGHT*scale, flags, &mainSDLWindow, &renderer);
  if (!mainSDLWindow || !renderer) return nullptr;
  SDL_SetWindowTitle(mainSDLWindow, "Caprice32 " VERSION_STRING);
  
  int surface_width = CPC_VISIBLE_SCR_WIDTH * (2 - CPC.scr_half_res_x);
  int surface_height = CPC_VISIBLE_SCR_HEIGHT * (2 - CPC.scr_half_res_y);

  vid = SDL_CreateRGBSurface(0, surface_width, surface_height, renderer_bpp(renderer), 0, 0, 0, 0);
  if (!vid) return nullptr;
  texture = SDL_CreateTextureFromSurface(renderer, vid);
  if (!texture) return nullptr;
  SDL_FillRect(vid, nullptr, SDL_MapRGB(vid->format,0,0,0));
  
  int renderer_width, renderer_height;
  SDL_GetRendererOutputSize(renderer, &renderer_width, &renderer_height);
  int render_scale = min(renderer_width / CPC_VISIBLE_SCR_WIDTH, renderer_height / CPC_VISIBLE_SCR_HEIGHT);
  width = render_scale * CPC_VISIBLE_SCR_WIDTH;
  height = render_scale * CPC_VISIBLE_SCR_HEIGHT;
  offset_x = (renderer_width - width) / 2;
  offset_y = (renderer_height - height) / 2;

  return vid;
}

void direct_setpal(SDL_Color* c)
{
  SDL_SetPaletteColors(vid->format->palette, c, 0, 32);
}

void direct_flip(video_plugin* t __attribute__((unused)))
{
  SDL_UpdateTexture(texture, nullptr, vid->pixels, vid->pitch);
  SDL_RenderClear(renderer);
  if (CPC.scr_preserve_aspect_ratio) {
    SDL_Rect dst = { offset_x, offset_y, width, height };
    SDL_RenderCopy(renderer, texture, nullptr, &dst);
  } else {
    SDL_RenderCopy(renderer, texture, nullptr, nullptr);
  }
  SDL_RenderPresent(renderer);
}

void direct_close()
{
  if (texture) SDL_DestroyTexture(texture);
  if (vid) SDL_FreeSurface(vid);
  if (renderer) SDL_DestroyRenderer(renderer);
  if (mainSDLWindow) SDL_DestroyWindow(mainSDLWindow);
}


#ifdef HAVE_GL
/* ------------------------------------------------------------------------------------ */
/* OpenGL scaling video plugin -------------------------------------------------------- */
/* ------------------------------------------------------------------------------------ */
static int tex_x,tex_y;
static GLuint screen_texnum,modulate_texnum;
static int gl_scanlines;

SDL_Surface* glscale_init(video_plugin* t __attribute__((unused)), int scale, bool fs)
{
#ifdef _WIN32
  const char *gl_library = "OpenGL32.DLL";
#else
  const char *gl_library = "libGL.so.1";
#endif

  gl_scanlines=CPC.scr_oglscanlines;
  SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 0);
  SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
  if (SDL_GL_LoadLibrary(gl_library)<0)
  {
    fprintf(stderr,"Unable to dynamically open GL lib : %s\n",SDL_GetError());
    return nullptr;
  }

  int width = CPC_VISIBLE_SCR_WIDTH*scale;
  int height = CPC_VISIBLE_SCR_HEIGHT*scale;

  Uint32 flags = SDL_WINDOW_ALLOW_HIGHDPI | SDL_WINDOW_OPENGL;
  if (fs) {
    if (CPC.scr_full_screen_exclusive) {
      flags |= SDL_WINDOW_FULLSCREEN;
    } else {
      flags |= SDL_WINDOW_FULLSCREEN_DESKTOP;
    }
  } else {
    flags |= SDL_WINDOW_SHOWN;
  }
    
  SDL_CreateWindowAndRenderer(width, height, flags, &mainSDLWindow, &renderer);
  if (!mainSDLWindow || !renderer) return nullptr;
  if (fs) {
    SDL_DisplayMode display;
    SDL_GetCurrentDisplayMode(0, &display);
    width = display.w;
    height = display.h;
  }
  vid = SDL_CreateRGBSurface(0, width, height, renderer_bpp(renderer), 0, 0, 0, 0);
  if (!vid) return nullptr;
  glcontext = SDL_GL_CreateContext(mainSDLWindow);
  if (init_glfuncs()!=0)
  {
    fprintf(stderr, "Cannot init OpenGL functions: %s\n", SDL_GetError());
    return nullptr;
  }

  int major, minor;
  const char *version;
  version = reinterpret_cast<const char *>(eglGetString(GL_VERSION));
  if (sscanf(version, "%d.%d", &major, &minor) != 2) {
    fprintf(stderr, "Unable to get OpenGL version: got %s.\n", version);
    return nullptr;
  }

  GLint max_texsize;
  eglGetIntegerv(GL_MAX_TEXTURE_SIZE,&max_texsize);
  if (max_texsize<1024) {
      printf("Your OpenGL implementation doesn't support 1024x1024 textures: max size = %d\n", max_texsize);
      CPC.scr_half_res_x = 1;
      CPC.scr_half_res_y = 1;
   }
  if (max_texsize<512) {
    fprintf(stderr, "Your OpenGL implementation doesn't support 512x512 textures\n");
    return nullptr;
  }

   unsigned int original_width, original_height, tex_size;
   tex_size = 512;
   if (CPC.scr_half_res_x) {
      original_width = CPC_VISIBLE_SCR_WIDTH;
   } else {
      tex_size = 1024;
      original_width = CPC_VISIBLE_SCR_WIDTH * 2;
   }
    if (CPC.scr_half_res_y) {
        original_height = CPC_VISIBLE_SCR_HEIGHT;
   } else {
      tex_size = 1024;
      original_height = CPC_VISIBLE_SCR_HEIGHT * 2;
   }

  // We have to react differently to the bpp parameter than with software rendering
  // Here are the rules :
  // for 8bpp OpenGL, we need the GL_EXT_paletted_texture extension
  // for 16bpp OpenGL, we need OpenGL 1.2+
  // for 24bpp reversed OpenGL, we need OpenGL 1.2+
  std::vector<int> candidates_bpp{32, 24, 16, 8};
  int surface_bpp = 0;
  for (int try_bpp : candidates_bpp) {
    switch(try_bpp)
    {
      case 8:
        surface_bpp = (have_gl_extension("GL_EXT_paletted_texture"))?8:0;
        break;
      case 15:
      case 16:
        surface_bpp = ((major>1)||(major == 1 && minor >= 2))?16:0;
        break;
      case 24:
      case 32:
      default:
        surface_bpp = ((major>1)||(major == 1 && minor >= 2))?24:0;
        break;
    }
    if (surface_bpp == 0) {
      fprintf(stderr, "Your OpenGL implementation doesn't support %dbpp textures\n", try_bpp);
    } else {
      break;
    }
  }
  if (surface_bpp == 0) {
    fprintf(stderr, "FATAL: Couldn't find a supported OpenGL color depth.\n");
    return nullptr;
  }

  eglDisable(GL_FOG);
  eglDisable(GL_LIGHTING);
  eglDisable(GL_CULL_FACE);
  eglDisable(GL_DEPTH_TEST);
  eglDisable(GL_BLEND);
  eglDisable(GL_NORMALIZE);
  eglDisable(GL_ALPHA_TEST);
  eglEnable(GL_TEXTURE_2D);
  eglBlendFunc (GL_SRC_ALPHA, GL_ONE);

  eglGenTextures(1,&screen_texnum);
  eglBindTexture(GL_TEXTURE_2D,screen_texnum);
  eglTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, CPC.scr_oglfilter?GL_LINEAR:GL_NEAREST);
  eglTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, CPC.scr_oglfilter?GL_LINEAR:GL_NEAREST);
  tex_x=tex_size;
  tex_y=tex_size;

  switch(surface_bpp)
  {
    case 24:
      eglTexImage2D(GL_TEXTURE_2D, 0,GL_RGB,tex_x,tex_y, 0,
          GL_RGB,
          GL_UNSIGNED_BYTE, nullptr);
      break;
    case 16:
      eglTexImage2D(GL_TEXTURE_2D, 0,GL_RGB5,tex_x,tex_y, 0,
          GL_RGB,
          GL_UNSIGNED_BYTE, nullptr);
      break;
    case 8:
      eglTexImage2D(GL_TEXTURE_2D, 0,GL_COLOR_INDEX8_EXT,tex_x,tex_y, 0,
          GL_COLOR_INDEX,
          GL_UNSIGNED_BYTE, nullptr);
      break;
  }

  if (gl_scanlines!=0)
  {
    Uint8 texmod;
    texmod=(100-gl_scanlines)*255/100;
    eglGenTextures(1,&modulate_texnum);
    eglBindTexture(GL_TEXTURE_2D,modulate_texnum);
    eglTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, CPC.scr_oglfilter?GL_LINEAR:GL_NEAREST);
    eglTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, CPC.scr_oglfilter?GL_LINEAR:GL_NEAREST);

    Uint8 modulate_texture[]={
      255,255,255,
      0,0,0};
    modulate_texture[3]=texmod;
    modulate_texture[4]=texmod;
    modulate_texture[5]=texmod;
    eglTexImage2D(GL_TEXTURE_2D, 0,GL_RGB8,1,2, 0,GL_RGB,GL_UNSIGNED_BYTE, modulate_texture);
  }
  // if (CPC.scr_preserve_aspect_ratio) {
    // eglViewport(t->x_offset, t->y_offset, t->width, t->height);
  // } else {
    eglViewport(0, 0, width, height);
  // }
  eglMatrixMode(GL_PROJECTION);
  eglLoadIdentity();
  eglOrtho(0, width, height, 0, -1.0, 1.0);

  eglMatrixMode(GL_MODELVIEW);
  eglLoadIdentity();

  pub=SDL_CreateRGBSurface(0, original_width, original_height, surface_bpp, 0, 0, 0, 0);
  return pub;
}

void glscale_setpal(SDL_Color* c)
{
  SDL_SetPaletteColors(pub->format->palette, c, 0, 32);
  if (pub->format->palette)
  {
    std::unique_ptr<Uint8[]> pal = std::make_unique<Uint8[]>(256*3);
    for(int i=0;i<256;i++)
    {
      pal[3*i  ] = pub->format->palette->colors[i].r;
      pal[3*i+1] = pub->format->palette->colors[i].g;
      pal[3*i+2] = pub->format->palette->colors[i].b;
    }
    eglBindTexture(GL_TEXTURE_2D,screen_texnum);
    eglColorTableEXT(GL_TEXTURE_2D,GL_RGB8,256,GL_RGB,GL_UNSIGNED_BYTE,pal.get());
  }
}

void glscale_flip(video_plugin* t __attribute__((unused)))
{
  eglDisable(GL_BLEND);
  eglClearColor(0,0,0,1);
  eglClear(GL_COLOR_BUFFER_BIT);
  
  if (gl_scanlines!=0)
  {
    eglActiveTextureARB(GL_TEXTURE1_ARB);
    eglEnable(GL_TEXTURE_2D);
    eglBindTexture(GL_TEXTURE_2D,modulate_texnum);
    eglTexEnvf (GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
    eglColor4f(1.0,1.0,1.0,1.0);
    eglActiveTextureARB(GL_TEXTURE0_ARB);
  }

  eglEnable(GL_TEXTURE_2D);
  eglBindTexture(GL_TEXTURE_2D,screen_texnum);
  
  if (CPC.scr_remanency && !CPC.scr_gui_is_currently_on)
  {
    /* draw again using the old texture */
    eglBegin(GL_QUADS);
    eglColor4f(1.0,1.0,1.0,1.0);

    eglTexCoord2f(0.f, 0.f);
    if (gl_scanlines!=0)
      eglMultiTexCoord2fARB(GL_TEXTURE1_ARB,0.f, 0.f);
    eglVertex2i(0, 0);

    eglTexCoord2f(0.f, static_cast<float>(pub->h)/tex_y);
    if (gl_scanlines!=0)
      eglMultiTexCoord2fARB(GL_TEXTURE1_ARB,0.f, vid->h/2);
    eglVertex2i(0, vid->h);

    eglTexCoord2f(static_cast<float>(pub->w)/tex_x, static_cast<float>(pub->h)/tex_y);
    if (gl_scanlines!=0)
      eglMultiTexCoord2fARB(GL_TEXTURE1_ARB,vid->w, vid->h/2);
    eglVertex2i(vid->w, vid->h);

    eglTexCoord2f(static_cast<float>(pub->w)/tex_x, 0.f);
    if (gl_scanlines!=0)
      eglMultiTexCoord2fARB(GL_TEXTURE1_ARB,vid->w, 0);
    eglVertex2i(vid->w, 0);
    eglEnd();

    /* enable blending for the subsequent pass */
    eglEnable(GL_BLEND);
    eglBlendFunc (GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
  }

  /* upload the texture */
  switch(pub->format->BitsPerPixel)
  {
    case 24:
      eglTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0,
          pub->w, pub->h,
          GL_BGR,GL_UNSIGNED_BYTE,
          pub->pixels);
      break;
    case 16:
      eglTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0,
          pub->w, pub->h,
          GL_RGB,GL_UNSIGNED_SHORT_5_6_5,
          pub->pixels);
      break;
    case 8:
      eglTexSubImage2D (GL_TEXTURE_2D, 0, 0, 0,
          pub->w,pub->h, 
          GL_COLOR_INDEX, GL_UNSIGNED_BYTE, 
          pub->pixels);
      break;
  }

  /* draw ! */
  eglBegin(GL_QUADS);
  eglColor4f(1.0,1.0,1.0,0.5);

  eglTexCoord2f(0.f, 0.f);
  if (gl_scanlines!=0)
    eglMultiTexCoord2fARB(GL_TEXTURE1_ARB,0.f, 0.f);
  eglVertex2i(0, 0);

  eglTexCoord2f(0.f, static_cast<float>(pub->h)/tex_y);
  if (gl_scanlines!=0)
    eglMultiTexCoord2fARB(GL_TEXTURE1_ARB,0.f, vid->h/2);
  eglVertex2i(0, vid->h);

  eglTexCoord2f(static_cast<float>(pub->w)/tex_x, static_cast<float>(pub->h)/tex_y);
  if (gl_scanlines!=0)
    eglMultiTexCoord2fARB(GL_TEXTURE1_ARB,vid->w, vid->h/2);
  eglVertex2i(vid->w, vid->h);

  eglTexCoord2f(static_cast<float>(pub->w)/tex_x, 0.f);
  if (gl_scanlines!=0)
    eglMultiTexCoord2fARB(GL_TEXTURE1_ARB,vid->w, 0);
  eglVertex2i(vid->w, 0);
  eglEnd();

  SDL_GL_SwapWindow(mainSDLWindow);
}

void glscale_close()
{
  direct_close();
  SDL_FreeSurface(pub);
  pub = nullptr;
}
#endif // HAVE_GL

/* ------------------------------------------------------------------------------------ */
/* Common 2x software scaling code ---------------------------------------------------- */
/* ------------------------------------------------------------------------------------ */

/* Computes the clipping of pub and scaled surfaces and put the result in src and dst accordingly.
 *
 * This provides the rectangles to clip to obtain a centered doubled CPC display
 * in the middle of the dst surface if it fits
 *
 * dst is the screen
 * src is the internal window
 *
 * Only exposed for testing purposes. Shouldn't be used outside of video.cpp
 */
static void compute_rects(SDL_Rect* src, SDL_Rect* dst)
{
  src->x = 0;
  src->y = 0;
  src->w = pub->w;
  src->h = pub->h;

  dst->x = 0;
  dst->y = 0;
  dst->w = scaled->w;
  dst->h = scaled->h;
}

void compute_rects_for_tests(SDL_Rect* src, SDL_Rect* dst)
{
  compute_rects(src, dst);
}

SDL_Surface* swscale_init(video_plugin* t, int scale, bool fs)
{
  Uint32 flags = SDL_WINDOW_ALLOW_HIGHDPI;
  if (fs) {
    if (CPC.scr_full_screen_exclusive) {
      flags |= SDL_WINDOW_FULLSCREEN;
    } else {
      flags |= SDL_WINDOW_FULLSCREEN_DESKTOP;
    }
  } else {
    flags |= SDL_WINDOW_SHOWN;
  }

  int window_width = CPC_VISIBLE_SCR_WIDTH * scale;
  int window_height = CPC_VISIBLE_SCR_HEIGHT * scale;

  int surface_width = CPC_VISIBLE_SCR_WIDTH * (2 - CPC.scr_half_res_x);
  int surface_height = CPC_VISIBLE_SCR_HEIGHT * (2 - CPC.scr_half_res_y);

  int scaled_width = surface_width * t->multiplier_x;
  int scaled_height = surface_height * t->multiplier_y;

  SDL_CreateWindowAndRenderer(window_width, window_height, flags, &mainSDLWindow, &renderer);
  if (!mainSDLWindow || !renderer) return nullptr;
  SDL_SetWindowTitle(mainSDLWindow, "Caprice32 " VERSION_STRING);

  vid = SDL_CreateRGBSurface(0, scaled_width, scaled_height, renderer_bpp(renderer), 0, 0, 0, 0);
  if (!vid) return nullptr;
  texture = SDL_CreateTextureFromSurface(renderer, vid);
  if (!texture) return nullptr;

  scaled = SDL_CreateRGBSurface(0, scaled_width, scaled_height, 16, 0, 0, 0, 0);
  if (!scaled) return nullptr;
  if (scaled->format->BitsPerPixel!=16)
  {
    LOG_ERROR(t->name << ": SDL didn't return a 16 bpp surface but a " << static_cast<int>(scaled->format->BitsPerPixel) << " bpp one.");
    return nullptr;
  }
  SDL_FillRect(vid, nullptr, SDL_MapRGB(vid->format,0,0,0));

  pub = SDL_CreateRGBSurface(0, surface_width, surface_height, 16, 0, 0, 0, 0);
  if (pub->format->BitsPerPixel!=16)
  {
    LOG_ERROR(t->name << ": SDL didn't return a 16 bpp surface but a " << static_cast<int>(pub->format->BitsPerPixel) << " bpp one.");
    return nullptr;
  }

  int renderer_width, renderer_height;
  SDL_GetRendererOutputSize(renderer, &renderer_width, &renderer_height);
  int render_scale = min(renderer_width / CPC_VISIBLE_SCR_WIDTH, renderer_height / CPC_VISIBLE_SCR_HEIGHT);
  width = render_scale * CPC_VISIBLE_SCR_WIDTH;
  height = render_scale * CPC_VISIBLE_SCR_HEIGHT;
  offset_x = (renderer_width - width) / 2;
  offset_y = (renderer_height - height) / 2;

  return pub;
}

// Common code to all software plugin to display the vid surface after it's been computed.
void swscale_blit()
{
  SDL_BlitSurface(scaled, nullptr, vid, nullptr);
  SDL_UpdateTexture(texture, nullptr, vid->pixels, vid->pitch);
  SDL_RenderClear(renderer);
  if (CPC.scr_preserve_aspect_ratio) {
    SDL_Rect dst = { offset_x, offset_y, width, height };
    SDL_RenderCopy(renderer, texture, nullptr, &dst);
  } else {
    SDL_RenderCopy(renderer, texture, nullptr, nullptr);
  }
  SDL_RenderPresent(renderer);
}

void swscale_setpal(SDL_Color* c)
{
  SDL_SetPaletteColors(scaled->format->palette, c, 0, 32);
  SDL_SetPaletteColors(pub->format->palette, c, 0, 32);
}

void swscale_close()
{
  direct_close();
  SDL_FreeSurface(pub);
  pub = nullptr;
}

/* ------------------------------------------------------------------------------------ */
/* Super eagle video plugin ----------------------------------------------------------- */
/* ------------------------------------------------------------------------------------ */

/* 2X SAI Filter */
static Uint32 colorMask = 0xF7DEF7DE;
static Uint32 lowPixelMask = 0x08210821;
static Uint32 qcolorMask = 0xE79CE79C;
static Uint32 qlowpixelMask = 0x18631863;
static Uint32 redblueMask = 0xF81F;
static Uint32 greenMask = 0x7E0;

__inline__ int GetResult1 (Uint32 A, Uint32 B, Uint32 C, Uint32 D)
{
  int x = 0;
  int y = 0;
  int r = 0;

  if (A == C)
    x += 1;
  else if (B == C)
    y += 1;
  if (A == D)
    x += 1;
  else if (B == D)
    y += 1;
  if (x <= 1)
    r += 1;
  if (y <= 1)
    r -= 1;
  return r;
}

__inline__ int GetResult2 (Uint32 A, Uint32 B, Uint32 C, Uint32 D)
{
  int x = 0;
  int y = 0;
  int r = 0;

  if (A == C)
    x += 1;
  else if (B == C)
    y += 1;
  if (A == D)
    x += 1;
  else if (B == D)
    y += 1;
  if (x <= 1)
    r -= 1;
  if (y <= 1)
    r += 1;
  return r;
}

__inline__ int GetResult (Uint32 A, Uint32 B, Uint32 C, Uint32 D)
{
  int x = 0;
  int y = 0;
  int r = 0;

  if (A == C)
    x += 1;
  else if (B == C)
    y += 1;
  if (A == D)
    x += 1;
  else if (B == D)
    y += 1;
  if (x <= 1)
    r += 1;
  if (y <= 1)
    r -= 1;
  return r;
}


__inline__ Uint32 INTERPOLATE (Uint32 A, Uint32 B)
{
  if (A != B)
  {
    return (((A & colorMask) >> 1) + ((B & colorMask) >> 1) +
        (A & B & lowPixelMask));
  }
  return A;
}

__inline__ Uint32 Q_INTERPOLATE (Uint32 A, Uint32 B, Uint32 C, Uint32 D)
{
  Uint32 x = ((A & qcolorMask) >> 2) +
    ((B & qcolorMask) >> 2) +
    ((C & qcolorMask) >> 2) + ((D & qcolorMask) >> 2);
  Uint32 y = (A & qlowpixelMask) +
    (B & qlowpixelMask) + (C & qlowpixelMask) + (D & qlowpixelMask);
  y = (y >> 2) & qlowpixelMask;
  return x + y;
}

void filter_supereagle(Uint8 *srcPtr, Uint32 srcPitch, /* Uint8 *deltaPtr,  */
     Uint8 *dstPtr, Uint32 dstPitch, int width, int height)
{
  Uint8  *dP;
  Uint16 *bP;
  Uint32 inc_bP;



  Uint32 finish;
  Uint32 Nextline = srcPitch >> 1;

  inc_bP = 1;

  for (int line = 0; line < height; line++)
  {
    bP = reinterpret_cast<Uint16 *>(srcPtr);
    dP = dstPtr;
    if (line == 0 || line == (height - 1))
    {
      // copy first and last lines without filtering
      for (finish = width; finish; finish -= inc_bP)
      {
        Uint32 color1, color2;
        color1 = *(bP);
        color2 = *(bP + 1);
        *(reinterpret_cast<Uint32 *>(dP)) =
          (color1 | (color1 << 16));
        *(reinterpret_cast<Uint32 *>(dP + dstPitch)) =
          (color2 | (color2 << 16));
        bP += inc_bP;
        dP += sizeof (Uint32);
      }
    } else {
      for (finish = width; finish; finish -= inc_bP)
      {
        Uint32 color4, color5, color6;
        Uint32 color1, color2, color3;
        Uint32 colorA1, colorA2, colorB1, colorB2, colorS1, colorS2;
        Uint32 product1a, product1b, product2a, product2b;
        colorB1 = *(bP - Nextline);
        colorB2 = *(bP - Nextline + 1);

        color4 = *(bP - 1);
        color5 = *(bP);
        color6 = *(bP + 1);
        colorS2 = *(bP + 2);

        color1 = *(bP + Nextline - 1);
        color2 = *(bP + Nextline);
        color3 = *(bP + Nextline + 1);
        colorS1 = *(bP + Nextline + 2);

        colorA1 = *(bP + Nextline + Nextline);
        colorA2 = *(bP + Nextline + Nextline + 1);
        // --------------------------------------
        if (color2 == color6 && color5 != color3)
        {
          product1b = product2a = color2;
          if ((color1 == color2) || (color6 == colorB2))
          {
            product1a = INTERPOLATE (color2, color5);
            product1a = INTERPOLATE (color2, product1a);
            //                       product1a = color2;
          }
          else
          {
            product1a = INTERPOLATE (color5, color6);
          }

          if ((color6 == colorS2) || (color2 == colorA1))
          {
            product2b = INTERPOLATE (color2, color3);
            product2b = INTERPOLATE (color2, product2b);
            //                       product2b = color2;
          }
          else
          {
            product2b = INTERPOLATE (color2, color3);
          }
        }
        else if (color5 == color3 && color2 != color6)
        {
          product2b = product1a = color5;

          if ((colorB1 == color5) || (color3 == colorS1))
          {
            product1b = INTERPOLATE (color5, color6);
            product1b = INTERPOLATE (color5, product1b);
            //                       product1b = color5;
          }
          else
          {
            product1b = INTERPOLATE (color5, color6);
          }

          if ((color3 == colorA2) || (color4 == color5))
          {
            product2a = INTERPOLATE (color5, color2);
            product2a = INTERPOLATE (color5, product2a);
            //                       product2a = color5;
          }
          else
          {
            product2a = INTERPOLATE (color2, color3);
          }

        }
        else if (color5 == color3 && color2 == color6)
        {
          int r = 0;

          r += GetResult (color6, color5, color1, colorA1);
          r += GetResult (color6, color5, color4, colorB1);
          r += GetResult (color6, color5, colorA2, colorS1);
          r += GetResult (color6, color5, colorB2, colorS2);

          if (r > 0)
          {
            product1b = product2a = color2;
            product1a = product2b = INTERPOLATE (color5, color6);
          }
          else if (r < 0)
          {
            product2b = product1a = color5;
            product1b = product2a = INTERPOLATE (color5, color6);
          }
          else
          {
            product2b = product1a = color5;
            product1b = product2a = color2;
          }
        }
        else
        {
          product2b = product1a = INTERPOLATE (color2, color6);
          product2b =
            Q_INTERPOLATE (color3, color3, color3, product2b);
          product1a =
            Q_INTERPOLATE (color5, color5, color5, product1a);

          product2a = product1b = INTERPOLATE (color5, color3);
          product2a =
            Q_INTERPOLATE (color2, color2, color2, product2a);
          product1b =
            Q_INTERPOLATE (color6, color6, color6, product1b);

          //                    product1a = color5;
          //                    product1b = color6;
          //                    product2a = color2;
          //                    product2b = color3;
        }
  #if SDL_BYTEORDER == SDL_LIL_ENDIAN
        product1a = product1a | (product1b << 16);
        product2a = product2a | (product2b << 16);
  #else
        product1a = (product1a << 16) | product1b;
        product2a = (product2a << 16) | product2b;
  #endif

        *(reinterpret_cast<Uint32 *>(dP)) = product1a;
        *(reinterpret_cast<Uint32 *>(dP + dstPitch)) = product2a;

        bP += inc_bP;
        dP += sizeof (Uint32);
      }      // end of for ( finish= width etc..)
    }
    srcPtr += srcPitch;
    dstPtr += dstPitch * 2;
  }      // endof: for (height; height; height--)
}

void seagle_flip(video_plugin* t __attribute__((unused)))
{
  if (SDL_MUSTLOCK(scaled))
    SDL_LockSurface(scaled);
  SDL_Rect src;
  SDL_Rect dst;
  compute_rects(&src,&dst);
  filter_supereagle(static_cast<Uint8*>(pub->pixels) + (2*src.x+src.y*pub->pitch), pub->pitch,
     static_cast<Uint8*>(scaled->pixels) + (2*dst.x+dst.y*scaled->pitch), scaled->pitch, src.w, src.h);
  if (SDL_MUSTLOCK(scaled))
    SDL_UnlockSurface(scaled);
  swscale_blit();
}

/* ------------------------------------------------------------------------------------ */
/* Scale2x video plugin --------------------------------------------------------------- */
/* ------------------------------------------------------------------------------------ */
void filter_scale2x(Uint8 *srcPtr, Uint32 srcPitch, 
                      Uint8 *dstPtr, Uint32 dstPitch,
          int width, int height)
{
  unsigned int nextlineSrc = srcPitch / sizeof(short);
  short *p = reinterpret_cast<short *>(srcPtr);

  unsigned int nextlineDst = dstPitch / sizeof(short);
  short *q = reinterpret_cast<short *>(dstPtr);

  int lastLine = height - 1;
  int lastRow = width - 1;

  for (int line = 0; line < height; line++)
  {
    for(int i = 0, j = 0; i < width; i++, j += 2) {
      short E = *(p + i);                                         // center pixel
      short D = i == 0 ? E : *(p + i - 1);                        // left pixel
      short F = i == lastRow ? E : *(p + i + 1);                  // right pixel
      short B = line == 0 ? E : *(p + i - nextlineSrc);           // top pixel
      short H = line == lastLine ? E : *(p + i + nextlineSrc);    // bottom pixel

      *(q + j) = D == B && B != F && D != H ? D : E;
      *(q + j + 1) = B == F && B != D && F != H ? F : E;
      *(q + j + nextlineDst) = D == H && D != B && H != F ? D : E;
      *(q + j + nextlineDst + 1) = H == F && D != H && B != F ? F : E;
    }
    p += nextlineSrc;
    q += nextlineDst << 1;
  }
}

void scale2x_flip(video_plugin* t __attribute__((unused)))
{
  if (SDL_MUSTLOCK(scaled))
    SDL_LockSurface(scaled);
  SDL_Rect src;
  SDL_Rect dst;
  compute_rects(&src,&dst);
  filter_scale2x(static_cast<Uint8*>(pub->pixels) + (2*src.x+src.y*pub->pitch), pub->pitch,
     static_cast<Uint8*>(scaled->pixels) + (2*dst.x+dst.y*scaled->pitch), scaled->pitch, src.w, src.h);
  if (SDL_MUSTLOCK(scaled))
    SDL_UnlockSurface(scaled);
  swscale_blit();
}

/* ------------------------------------------------------------------------------------ */
/* ascale2x video plugin --------------------------------------------------------------- */
/* ------------------------------------------------------------------------------------ */
void filter_ascale2x (Uint8 *srcPtr, Uint32 srcPitch,
       Uint8 *dstPtr, Uint32 dstPitch, int width, int height)
{
  Uint8  *dP;
  Uint16 *bP;
  Uint32 inc_bP;

  Uint32 finish;
  Uint32 Nextline = srcPitch >> 1;
  inc_bP = 1;

  for (int line = 0; line < height; line++)
  {
    bP = reinterpret_cast<Uint16 *>(srcPtr);
    dP = dstPtr;

    if (line == 0 || line == height - 1)
    {
      // First and last lines are just doubled
      for (finish = width; finish; finish -= inc_bP)
      {
        Uint32 colorA;
        colorA = *(bP);
        *(reinterpret_cast<Uint32 *>(dP)) = colorA | (colorA << 16);
        bP += inc_bP;
        dP += sizeof (Uint32);
      }
    }
    else
    {
      // Other lines are scaled using ascale2x
      for (finish = width; finish; finish -= inc_bP)
      {

        Uint32 colorA, colorB;
        Uint32 colorC, colorD,
              colorE, colorF, colorG, colorH,
              colorI, colorJ, colorK, colorL,

              colorM, colorN, colorO;
        Uint32 product, product1, product2;

        //---------------------------------------
        // Map of the pixels:                    I|E F|J
        //                                       G|A B|K
        //                                       H|C D|L
        //                                       M|N O|P
        colorI = *(bP - Nextline - 1);
        colorE = *(bP - Nextline);
        colorF = *(bP - Nextline + 1);
        colorJ = *(bP - Nextline + 2);

        colorG = *(bP - 1);
        colorA = *(bP);
        colorB = *(bP + 1);
        colorK = *(bP + 2);

        colorH = *(bP + Nextline - 1);
        colorC = *(bP + Nextline);
        colorD = *(bP + Nextline + 1);
        colorL = *(bP + Nextline + 2);

        colorM = *(bP + Nextline + Nextline - 1);
        colorN = *(bP + Nextline + Nextline);
        colorO = *(bP + Nextline + Nextline + 1);

        if ((colorA == colorD) && (colorB != colorC))
        {
          if (((colorA == colorE) && (colorB == colorL)) ||
              ((colorA == colorC) && (colorA == colorF)
              && (colorB != colorE) && (colorB == colorJ)))
          {
            product = colorA;
          }
          else
          {
            product = INTERPOLATE (colorA, colorB);
          }

          if (((colorA == colorG) && (colorC == colorO)) ||
              ((colorA == colorB) && (colorA == colorH)
              && (colorG != colorC) && (colorC == colorM)))
          {
            product1 = colorA;
          }
          else
          {
            product1 = INTERPOLATE (colorA, colorC);
          }
          product2 = colorA;
        }
        else if ((colorB == colorC) && (colorA != colorD))
        {
          if (((colorB == colorF) && (colorA == colorH)) ||
              ((colorB == colorE) && (colorB == colorD)
              && (colorA != colorF) && (colorA == colorI)))
          {
            product = colorB;
          }
          else
          {
            product = INTERPOLATE (colorA, colorB);
          }

          if (((colorC == colorH) && (colorA == colorF)) ||
              ((colorC == colorG) && (colorC == colorD)
              && (colorA != colorH) && (colorA == colorI)))
          {
            product1 = colorC;
          }
          else
          {
            product1 = INTERPOLATE (colorA, colorC);
          }
          product2 = colorB;
        }
        else if ((colorA == colorD) && (colorB == colorC))
        {
          if (colorA == colorB)
          {
            product = colorA;
            product1 = colorA;
            product2 = colorA;
          }
          else
          {
            int r = 0;

            product1 = INTERPOLATE (colorA, colorC);
            product = INTERPOLATE (colorA, colorB);

            r += GetResult1 (colorA, colorB, colorG, colorE);
            r += GetResult2 (colorB, colorA, colorK, colorF);
            r += GetResult2 (colorB, colorA, colorH, colorN);
            r += GetResult1 (colorA, colorB, colorL, colorO);

            if (r > 0)
              product2 = colorA;
            else if (r < 0)
              product2 = colorB;
            else
            {
              product2 =
                Q_INTERPOLATE (colorA, colorB, colorC,
                    colorD);
            }
          }
        }
        else
        {
          product2 = Q_INTERPOLATE (colorA, colorB, colorC, colorD);

          if ((colorA == colorC) && (colorA == colorF)
              && (colorB != colorE) && (colorB == colorJ))
          {
            product = colorA;
          }
          else
            if ((colorB == colorE) && (colorB == colorD)
                && (colorA != colorF) && (colorA == colorI))
            {
              product = colorB;
            }
            else
            {
              product = INTERPOLATE (colorA, colorB);
            }

          if ((colorA == colorB) && (colorA == colorH)
              && (colorG != colorC) && (colorC == colorM))
          {
            product1 = colorA;
          }
          else
            if ((colorC == colorG) && (colorC == colorD)
                && (colorA != colorH) && (colorA == colorI))
            {
              product1 = colorC;
            }
            else
            {
              product1 = INTERPOLATE (colorA, colorC);
            }
        }
  #if SDL_BYTEORDER == SDL_LIL_ENDIAN
        product = colorA | (product << 16);
        product1 = product1 | (product2 << 16);
  #else
        product = (colorA << 16) | product;
        product1 = (product1 << 16) | product2;
  #endif
        *(reinterpret_cast<Uint32 *>(dP)) = product;
        *(reinterpret_cast<Uint32 *>(dP + dstPitch)) = product1;

        bP += inc_bP;
        dP += sizeof (Uint32);
      }      // end of for ( finish= width etc..)
    }      // endof: for (height; height; height--)

    srcPtr += srcPitch;
    dstPtr += dstPitch * 2;
  }
}



void ascale2x_flip(video_plugin* t __attribute__((unused)))
{
  if (SDL_MUSTLOCK(scaled))
    SDL_LockSurface(scaled);
  SDL_Rect src;
  SDL_Rect dst;
  compute_rects(&src,&dst);
  filter_ascale2x(static_cast<Uint8*>(pub->pixels) + (2*src.x+src.y*pub->pitch), pub->pitch,
      static_cast<Uint8*>(scaled->pixels) + (2*dst.x+dst.y*scaled->pitch), scaled->pitch, src.w, src.h);
  if (SDL_MUSTLOCK(scaled))
    SDL_UnlockSurface(scaled);
  swscale_blit();
}


/* ------------------------------------------------------------------------------------ */
/* tv2x video plugin ------------------------------------------------------------------ */
/* ------------------------------------------------------------------------------------ */
void filter_tv2x(Uint8 *srcPtr, Uint32 srcPitch, 
    Uint8 *dstPtr, Uint32 dstPitch, 
    int width, int height)
{
  unsigned int nextlineSrc = srcPitch / sizeof(Uint16);
  Uint16 *p = reinterpret_cast<Uint16 *>(srcPtr);

  unsigned int nextlineDst = dstPitch / sizeof(Uint16);
  Uint16 *q = reinterpret_cast<Uint16 *>(dstPtr);

  while(height--) {
    int i = 0, j = 0;
    for(; i < width; ++i, j += 2) {
      Uint16 p1 = *(p + i);
      Uint32 pi;

      pi = (((p1 & redblueMask) * 7) >> 3) & redblueMask;
      pi |= (((p1 & greenMask) * 7) >> 3) & greenMask;

      *(q + j) = p1;
      *(q + j + 1) = p1;
      *(q + j + nextlineDst) = pi;
      *(q + j + nextlineDst + 1) = pi;
    }
    p += nextlineSrc;
    q += nextlineDst << 1;
  }
}

void tv2x_flip(video_plugin* t __attribute__((unused)))
{
  if (SDL_MUSTLOCK(scaled))
    SDL_LockSurface(scaled);
  SDL_Rect src;
  SDL_Rect dst;
  compute_rects(&src,&dst);
  filter_tv2x(static_cast<Uint8*>(pub->pixels) + (2*src.x+src.y*pub->pitch), pub->pitch,
      static_cast<Uint8*>(scaled->pixels) + (2*dst.x+dst.y*scaled->pitch), scaled->pitch, src.w, src.h);
  if (SDL_MUSTLOCK(scaled))
    SDL_UnlockSurface(scaled);
  swscale_blit();
}

/* ------------------------------------------------------------------------------------ */
/* Software bilinear video plugin ----------------------------------------------------- */
/* ------------------------------------------------------------------------------------ */
void filter_bilinear(Uint8 *srcPtr, Uint32 srcPitch, 
    Uint8 *dstPtr, Uint32 dstPitch, 
    int width, int height)
{
  unsigned int nextlineSrc = srcPitch / sizeof(Uint16);
  Uint16 *p = reinterpret_cast<Uint16 *>(srcPtr);
  unsigned int nextlineDst = dstPitch / sizeof(Uint16);
  Uint16 *q = reinterpret_cast<Uint16 *>(dstPtr);

  for (int line = 0; line < height; line++ ) {
    if (line == height - 1) {
      // Last line: just duplicate
      for (int i = 0, ii = 0; i < width; ++i, ii += 2) {
        Uint16 A = *(p + i);
        *(q + ii) = A;
        *(q + ii + 1) = A;
        *(q + ii + nextlineDst) = A;
        *(q + ii + nextlineDst + 1) = A;
      }
      break;
    } else {
      int i, ii;
      for(i = 0, ii = 0; i < width; ++i, ii += 2) {
        Uint16 A = *(p + i);
        Uint16 B = *(p + i + 1);
        Uint16 C = *(p + i + nextlineSrc);
        Uint16 D = *(p + i + nextlineSrc + 1);
        *(q + ii) = A;
        *(q + ii + 1) = INTERPOLATE(A, B);
        *(q + ii + nextlineDst) = INTERPOLATE(A, C);
        *(q + ii + nextlineDst + 1) = Q_INTERPOLATE(A, B, C, D);
      }
    }
    p += nextlineSrc;
    q += nextlineDst << 1;
  }
}

void swbilin_flip(video_plugin* t __attribute__((unused)))
{
  if (SDL_MUSTLOCK(scaled))
    SDL_LockSurface(scaled);
  SDL_Rect src;
  SDL_Rect dst;
  compute_rects(&src,&dst);
  filter_bilinear(static_cast<Uint8*>(pub->pixels) + (2*src.x+src.y*pub->pitch), pub->pitch,
      static_cast<Uint8*>(scaled->pixels) + (2*dst.x+dst.y*scaled->pitch), scaled->pitch, src.w, src.h);
  if (SDL_MUSTLOCK(scaled))
    SDL_UnlockSurface(scaled);
  swscale_blit();
}

/* ------------------------------------------------------------------------------------ */
/* Software bicubic video plugin ------------------------------------------------------ */
/* ------------------------------------------------------------------------------------ */
#define BLUE_MASK565 0x001F001F
#define RED_MASK565 0xF800F800
#define GREEN_MASK565 0x07E007E0

#define BLUE_MASK555 0x001F001F
#define RED_MASK555 0x7C007C00
#define GREEN_MASK555 0x03E003E0

__inline__ static void MULT(Uint16 c, float* r, float* g, float* b, float alpha) {
  *r += alpha * ((c & RED_MASK565  ) >> 11);
  *g += alpha * ((c & GREEN_MASK565) >>  5);
  *b += alpha * ((c & BLUE_MASK565 ) >>  0);
}

__inline__ static Uint16 MAKE_RGB565(float r, float g, float b) {
  return 
    (((static_cast<Uint8>(r)) << 11) & RED_MASK565  ) |
    (((static_cast<Uint8>(g)) <<  5) & GREEN_MASK565) |
    (((static_cast<Uint8>(b)) <<  0) & BLUE_MASK565 );
}

__inline__ float CUBIC_WEIGHT(float x) {
  // P(x) = { x, x>0 | 0, x<=0 }
  // P(x + 2) ^ 3 - 4 * P(x + 1) ^ 3 + 6 * P(x) ^ 3 - 4 * P(x - 1) ^ 3
  double r = 0.;
  if(x + 2 > 0) r +=      pow(x + 2, 3);
  if(x + 1 > 0) r += -4 * pow(x + 1, 3);
  if(x     > 0) r +=  6 * pow(x    , 3);
  if(x - 1 > 0) r += -4 * pow(x - 1, 3);
  return static_cast<float>(r) / 6;
}

void filter_bicubic(Uint8 *srcPtr, Uint32 srcPitch, 
    Uint8 *dstPtr, Uint32 dstPitch, 
    int width, int height)
{
  unsigned int nextlineSrc = srcPitch / sizeof(Uint16);
  Uint16 *p = reinterpret_cast<Uint16 *>(srcPtr);
  unsigned int nextlineDst = dstPitch / sizeof(Uint16);
  Uint16 *q = reinterpret_cast<Uint16 *>(dstPtr);
  int dx = width << 1, dy = height << 1;
  float fsx = static_cast<float>(width) / dx;
  float fsy = static_cast<float>(height) / dy;
  float v = 0.0f;
  int j = 0;
  for(; j < dy; ++j) {
    float u = 0.0f;
    int iv = static_cast<int>(v);
    float decy = v - iv;
    int i = 0;
    for(; i < dx; ++i) {
      int iu = static_cast<int>(u);
      float decx = u - iu;
      float r, g, b;
      int m;
      r = g = b = 0.;
      for(m = -1; m <= 2; ++m) {
        float r1 = CUBIC_WEIGHT(decy - m);
        int n;
        for(n = -1; n <= 2; ++n) {
          float r2 = CUBIC_WEIGHT(n - decx);
          Uint16* pIn = p + (iu  + n) + (iv + m) * static_cast<int>(nextlineSrc);
          MULT(*pIn, &r, &g, &b, r1 * r2);
        }
      }
      *(q + i) = MAKE_RGB565(r, g, b);
      u += fsx;
    }
    q += nextlineDst;
    v += fsy;
  }
}

void swbicub_flip(video_plugin* t __attribute__((unused)))
{
  if (SDL_MUSTLOCK(scaled))
    SDL_LockSurface(scaled);
  SDL_Rect src;
  SDL_Rect dst;
  compute_rects(&src,&dst);
  filter_bicubic(static_cast<Uint8*>(pub->pixels) + (2*src.x+src.y*pub->pitch) + (pub->pitch), pub->pitch,
      static_cast<Uint8*>(scaled->pixels) + (2*dst.x+dst.y*scaled->pitch), scaled->pitch, src.w, src.h);
  if (SDL_MUSTLOCK(scaled))
    SDL_UnlockSurface(scaled);
  swscale_blit();
}

/* ------------------------------------------------------------------------------------ */
/* Dot matrix video plugin ------------------------------------------------------------ */
/* ------------------------------------------------------------------------------------ */
static Uint16 DOT_16(Uint16 c, int j, int i) {
  static constexpr Uint16 dotmatrix[16] = {
    0x01E0, 0x0007, 0x3800, 0x0000,
    0x39E7, 0x0000, 0x39E7, 0x0000,
    0x3800, 0x0000, 0x01E0, 0x0007,
    0x39E7, 0x0000, 0x39E7, 0x0000
  };
  return c - ((c >> 2) & *(dotmatrix + ((j & 3) << 2) + (i & 3)));
}

void filter_dotmatrix(Uint8 *srcPtr, Uint32 srcPitch, 
    Uint8 *dstPtr, Uint32 dstPitch,
    int width, int height)
{
  unsigned int nextlineSrc = srcPitch / sizeof(Uint16);
  Uint16 *p = reinterpret_cast<Uint16 *>(srcPtr);

  unsigned int nextlineDst = dstPitch / sizeof(Uint16);
  Uint16 *q = reinterpret_cast<Uint16 *>(dstPtr);

  int i, ii, j, jj;
  for(j = 0, jj = 0; j < height; ++j, jj += 2) {
    for(i = 0, ii = 0; i < width; ++i, ii += 2) {
      Uint16 c = *(p + i);
      *(q + ii) = DOT_16(c, jj, ii);
      *(q + ii + 1) = DOT_16(c, jj, ii + 1);
      *(q + ii + nextlineDst) = DOT_16(c, jj + 1, ii);
      *(q + ii + nextlineDst + 1) = DOT_16(c, jj + 1, ii + 1);
    }
    p += nextlineSrc;
    q += nextlineDst << 1;
  }
}

void dotmat_flip(video_plugin* t __attribute__((unused)))
{
  if (SDL_MUSTLOCK(scaled))
    SDL_LockSurface(scaled);
  SDL_Rect src;
  SDL_Rect dst;
  compute_rects(&src,&dst);
  filter_dotmatrix(static_cast<Uint8*>(pub->pixels) + (2*src.x+src.y*pub->pitch) + (pub->pitch), pub->pitch,
      static_cast<Uint8*>(scaled->pixels) + (2*dst.x+dst.y*scaled->pitch), scaled->pitch, src.w, src.h);
  if (SDL_MUSTLOCK(scaled))
    SDL_UnlockSurface(scaled);
  swscale_blit();
}

/* ------------------------------------------------------------------------------------ */
/* Monitor 2x video plugin ------------------------------------------------------------ */
/* ------------------------------------------------------------------------------------ */
static inline Uint16 rgb565_scale(Uint16 c, int num, int den)
{
  int r = (c >> 11) & 0x1F;
  int g = (c >> 5)  & 0x3F;
  int b =  c        & 0x1F;

  r = (r * num) / den;
  g = (g * num) / den;
  b = (b * num) / den;

  if (r > 0x1F) r = 0x1F;
  if (g > 0x3F) g = 0x3F;
  if (b > 0x1F) b = 0x1F;

  return static_cast<Uint16> ((r << 11) | (g << 5) | b);
}

void filter_monitor(Uint8 *srcPtr, Uint32 srcPitch,
                   Uint8 *dstPtr, Uint32 dstPitch,
                   int width, int height)
{
  const unsigned int nextlineSrc = srcPitch / sizeof(Uint16);
  Uint16 *p = reinterpret_cast<Uint16 *>(srcPtr);
  const unsigned int nextlineDst = dstPitch / sizeof(Uint16);
  Uint16 *q = reinterpret_cast<Uint16 *>(dstPtr);

  // Tweak these:
  const int GLOBAL_NUM = 9;  // global brightness boost (~112%)
  const int GLOBAL_DEN = 8;
  const int SCAN_NUM   = 1;  // scanline brightness (~50%)
  const int SCAN_DEN   = 2;

  for (int line = 0; line < height; line++) {

    if (line == height - 1) {
      // Last line: just duplicate with scaling
      for (int i = 0, ii = 0; i < width; ++i, ii += 2) {
        Uint16 A = *(p + i);
        Uint16 scaledA = rgb565_scale(A, GLOBAL_NUM, GLOBAL_DEN);
        *(q + ii) = scaledA;
        *(q + ii + 1) = scaledA;
        Uint16 scanA = rgb565_scale(scaledA, SCAN_NUM, SCAN_DEN);
        *(q + ii + nextlineDst) = scanA;
        *(q + ii + nextlineDst + 1) = scanA;
      }
    } else {
      int i, ii;

      for (i = 0, ii = 0; i < width; ++i, ii += 2) {
        Uint16 A = *(p + i);
        Uint16 B = *(p + i + 1);
        Uint16 C = *(p + i + nextlineSrc);
        Uint16 D = *(p + i + nextlineSrc + 1);

        Uint16 topL  = A;
        Uint16 topR  = INTERPOLATE(A, B);
        Uint16 botL  = INTERPOLATE(A, C);
        Uint16 botR  = Q_INTERPOLATE(A, B, C, D);

        Uint16 vMidL = INTERPOLATE(topL, botL);
        Uint16 vMidR = INTERPOLATE(topR, botR);

        topL = INTERPOLATE(topL, vMidL);
        topR = INTERPOLATE(topR, vMidR);

        botL = INTERPOLATE(botL, vMidL);
        botR = INTERPOLATE(botR, vMidR);

        topL = rgb565_scale(topL, GLOBAL_NUM, GLOBAL_DEN);
        topR = rgb565_scale(topR, GLOBAL_NUM, GLOBAL_DEN);
        botL = rgb565_scale(botL, GLOBAL_NUM, GLOBAL_DEN);
        botR = rgb565_scale(botR, GLOBAL_NUM, GLOBAL_DEN);

        *(q + ii)                 = topL;
        *(q + ii + 1)             = topR;

        Uint16 scanL = rgb565_scale(botL, SCAN_NUM, SCAN_DEN); // ~50%
        Uint16 scanR = rgb565_scale(botR, SCAN_NUM, SCAN_DEN);

        *(q + ii + nextlineDst)     = scanL;
        *(q + ii + nextlineDst + 1) = scanR;
      }
    }
    p += nextlineSrc;
    q += nextlineDst << 1;
  }
}

void monitor_flip(video_plugin* t __attribute__((unused)))
{
  if (SDL_MUSTLOCK(scaled))
    SDL_LockSurface(scaled);
  SDL_Rect src;
  SDL_Rect dst;
  compute_rects(&src,&dst);
  filter_monitor(static_cast<Uint8*>(pub->pixels) + (2*src.x+src.y*pub->pitch), pub->pitch,
      static_cast<Uint8*>(scaled->pixels) + (2*dst.x+dst.y*scaled->pitch), scaled->pitch, src.w, src.h);
  if (SDL_MUSTLOCK(scaled))
    SDL_UnlockSurface(scaled);
  swscale_blit();
}


// ============================================================
// CTM644 filter
//   - slight horizontal smear
//   - LUT transformation for RGB triads
//   - 4x4 expansion
// ============================================================

#define HALF_MASK    0xF7DEu
#define QUARTER_MASK 0xE79Cu
#define SHIFT3_MASK  0xC718u

#define LUT_CH 3
#define LUT_BR 2
#define LUT_SZ 65536

static Uint16 *gTriadLUT = NULL;
static Uint8 *gLumaLUT = NULL;

// RGB565 helpers

static inline Uint8 expand5to8(Uint16 v5) { return Uint8((v5 << 3) | (v5 >> 2)); }
static inline Uint8 expand6to8(Uint16 v6) { return Uint8((v6 << 2) | (v6 >> 4)); }

static inline void unpack565_to_rgb8(Uint16 p, float &r, float &g, float &b) {
  Uint16 r5 = (p >> 11) & 0x1F;
  Uint16 g6 = (p >> 5)  & 0x3F;
  Uint16 b5 =  p        & 0x1F;
  r = static_cast<float> (expand5to8(r5));
  g = static_cast<float> (expand6to8(g6));
  b = static_cast<float> (expand5to8(b5));
}

static inline Uint16 pack565_from_rgbf(float rf, float gf, float bf) {
  // clamp 0..255
  int r =  static_cast<int> (std::lround(min(255.0f, max(0.0f, rf))));
  int g =  static_cast<int> (std::lround(min(255.0f, max(0.0f, gf))));
  int b =  static_cast<int> (std::lround(min(255.0f, max(0.0f, bf))));
  return static_cast<Uint16>((((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)));
}

// 1/2 A + 1/2 B  (RGB565-safe)
static inline Uint16 avg_1_1(Uint16 a, Uint16 b) {
  return (((a & HALF_MASK) + (b & HALF_MASK)) >> 1);
}

// 1/4 L + 1/2 C + 1/4 R  (RGB565-safe)
static inline Uint16 avg_1_2_1(Uint16 L, Uint16 C, Uint16 R)
{
    return ((((L & QUARTER_MASK) + (R & QUARTER_MASK)) >> 1) + (C & HALF_MASK)) >> 1;
}

// 3/4 C + 1/4 N  (RGB565-safe)
static inline Uint16 avg_3_1(Uint16 C, Uint16 N)
{
    return ((C & HALF_MASK) >> 1) +
           ((C & QUARTER_MASK) >> 2) +
           ((N & QUARTER_MASK) >> 2);
}

// 3/8 A + 5/8 B  (RGB565-safe)
static inline Uint16 avg_3_5(Uint16 a, Uint16 b)
{
    return ((a & QUARTER_MASK) >> 2) +   // a/4
           ((a & SHIFT3_MASK)  >> 3) +   // a/8
           ((b & HALF_MASK)    >> 1) +   // b/2
           ((b & SHIFT3_MASK)  >> 3);    // b/8
}

// function pointer to smear function
typedef Uint16 (*smear_func)(Uint16 C, Uint16 L, Uint16 R);
smear_func smear = nullptr;

static inline Uint16 smear_halfres(Uint16 C, Uint16 L, Uint16 R) {
    // Luma-aware implementation that uses luma thresholds with light bleed
    constexpr int thresh = 8;

    if (C == L && C == R) return C;

    const int t = static_cast<int>(gLumaLUT[C]) + thresh;

    const bool bleedL = static_cast<int>(gLumaLUT[L]) > t;
    const bool bleedR = static_cast<int>(gLumaLUT[R]) > t;

    if (bleedL) {
        if (bleedR) 
            return avg_1_2_1(L, C, R);
        else
            return avg_3_1(C, L);
    } else {
        if (bleedR) 
            return avg_3_1(C, R);
        else
            return C;
    }
}

static inline Uint16 smear_fullres(Uint16 C, Uint16 L, Uint16 R) {
    // Luma-aware implementation that uses luma thresholds with heavy bleed
    constexpr int thresh = 8;

    if (C == L && C == R) return C;

    const int t = static_cast<int>(gLumaLUT[C]) + thresh;

    const bool bleedL = static_cast<int>(gLumaLUT[L]) > t;
    const bool bleedR = static_cast<int>(gLumaLUT[R]) > t;

    if (bleedL) {
        if (bleedR) 
            return avg_1_2_1(L, C, R);
        else
            return avg_3_5(L, C);
    } else {
        if (bleedR) 
            return avg_3_5(R, C);
        else
            return C;
    }
}

static inline Uint16 LUT_GET(int ch, int br, Uint16 src565) {
    // index = ((ch*2 + br)*65536 + src565)
    return gTriadLUT[ ((ch<<1) + br) * LUT_SZ + src565 ];
}

// Generation parameters
struct TriadParams {
  float BRIGHT_FACTOR;         // Values : [0.00 .. 3.00]
  float DIM_FACTOR;            // Values : [0.05 .. 2.00]
  float TRIAD_TINT;            // Values : [0.00 .. 1.00]
  float TRIAD_TINT_DIM;        // Values : [0.00 .. 1.50]
  float BLACK_FLOOR;           // Values : [0.0  .. 24.0]
  float CROSS_BLEED;           // Values : [0.00 .. 0.30]
  float MASK_GAIN;             // Values : [0.80 .. 2.50]
  float SUBPIX_FLOOR_8;        // Values : [0.0  .. 12.0]
  float GAIN_8;                // Values : [0.70 .. 1.80]
  float GAMMA;                 // Values : [0.00 .. 2.00]
  float OUTPUT_BLACK_POINT;    // Values : [0.00 .. 0.40]
  float OUTPUT_SATURATION;     // Values : [0.50 .. 2.00]
};

// ------------------------------------------------------------
// LUT build
// triadLUT[ch][bright][src565] -> dst565
// Layout note: [3][2][65536] is 3*2*65536 = 393,216 entries = 768 KB (Uint16)
// ------------------------------------------------------------
static inline float apply_black_point(float v255, float bp) {
  if (bp <= 0.0f) return v255;
  float x = v255 / 255.0f;
  x = (x - bp) / (1.0f - bp);
  if (x < 0.0f) x = 0.0f;
  return x * 255.0f;
}

static inline void apply_saturation(float &r, float &g, float &b, float sat) {
  if (sat == 1.0f) return;
  // Rec.601-ish luma (as in your python)
  float y = 0.299f * r + 0.587f * g + 0.114f * b;
  r = y + (r - y) * sat;
  g = y + (g - y) * sat;
  b = y + (b - y) * sat;
}

void build_luma_lut_565()
{
    gLumaLUT = new Uint8[65536];

    for (Uint32 p = 0; p < 65536u; ++p) {
        const Uint32 r5 = (p >> 11) & 31u;
        const Uint32 g6 = (p >> 5)  & 63u;
        const Uint32 b5 =  p        & 31u;

        // Expand RGB565 to 8-bit per channel (bit replication)
        const Uint32 r8 = (r5 << 3) | (r5 >> 2);
        const Uint32 g8 = (g6 << 2) | (g6 >> 4);
        const Uint32 b8 = (b5 << 3) | (b5 >> 2);

        // Integer luma approximation (0..255)
        const Uint32 y = (r8 * 54u + g8 * 183u + b8 * 19u) >> 8;
        gLumaLUT[p] = static_cast<Uint8>(y);
    }
}

void build_triad_lut_565(Uint16 *triadLUT /*size 3*2*65536*/, const TriadParams &P)
{
  // gamma table (uses 1/gamma like your python)
  float gammaLUT[256];
  float gamma = (P.GAMMA <= 0.0f) ? 1.0f : P.GAMMA;
  float invG = 1.0f / gamma;
  for (int i = 0; i < 256; ++i) {
    float x =  static_cast<float> (i) / 255.0f;
    gammaLUT[i] = 255.0f * std::pow(x, invG);
  }

  auto LUT = [&](int ch, int bright, int src565) -> Uint16& {
    // contiguous packing: ch-major, then bright, then src565
    return triadLUT[(ch * 2 + bright) * 65536 + src565];
  };

  const float floor = P.SUBPIX_FLOOR_8;

  for (int p = 0; p < 65536; ++p) {
    float r, g, b;
    unpack565_to_rgb8(static_cast<Uint16> (p), r, g, b);

    // GAIN_8 preconditioning
    r *= P.GAIN_8; g *= P.GAIN_8; b *= P.GAIN_8;
    r = min(255.0f, max(0.0f, r));
    g = min(255.0f, max(0.0f, g));
    b = min(255.0f, max(0.0f, b));

    // gamma via table
    int ri = static_cast<int> (std::lround(r)); if (ri < 0) ri = 0; if (ri > 255) ri = 255;
    int gi = static_cast<int> (std::lround(g)); if (gi < 0) gi = 0; if (gi > 255) gi = 255;
    int bi = static_cast<int> (std::lround(b)); if (bi < 0) bi = 0; if (bi > 255) bi = 255;
    r = gammaLUT[ri];
    g = gammaLUT[gi];
    b = gammaLUT[bi];

    // post-gamma shaping
    r = apply_black_point(r, P.OUTPUT_BLACK_POINT);
    g = apply_black_point(g, P.OUTPUT_BLACK_POINT);
    b = apply_black_point(b, P.OUTPUT_BLACK_POINT);
    apply_saturation(r, g, b, P.OUTPUT_SATURATION);

    // cross-bleed on underlying
    float br = r + P.CROSS_BLEED * (g + b) * 0.5f;
    float bg = g + P.CROSS_BLEED * (r + b) * 0.5f;
    float bb = b + P.CROSS_BLEED * (r + g) * 0.5f;

    // underlying floor
    if (br < P.BLACK_FLOOR) br = P.BLACK_FLOOR;
    if (bg < P.BLACK_FLOOR) bg = P.BLACK_FLOOR;
    if (bb < P.BLACK_FLOOR) bb = P.BLACK_FLOOR;

    for (int bright = 0; bright <= 1; ++bright) {
      float f = (bright == 1) ? P.BRIGHT_FACTOR : P.DIM_FACTOR;
      float t = (bright == 1) ? P.TRIAD_TINT    : P.TRIAD_TINT_DIM;

      float fullR = br * f;
      float fullG = bg * f;
      float fullB = bb * f;

      float maskR = fullR * P.MASK_GAIN;
      float maskG = fullG * P.MASK_GAIN;
      float maskB = fullB * P.MASK_GAIN;

      // out = (1-t)*mask + t*full + floor
      // R triad
      {
        float oR = (1.0f - t) * maskR + t * fullR + floor;
        float oG = (1.0f - t) * 0.0f  + t * fullG + floor;
        float oB = (1.0f - t) * 0.0f  + t * fullB + floor;
        LUT(0, bright, p) = pack565_from_rgbf(oR, oG, oB);
      }
      // G triad
      {
        float oR = (1.0f - t) * 0.0f  + t * fullR + floor;
        float oG = (1.0f - t) * maskG + t * fullG + floor;
        float oB = (1.0f - t) * 0.0f  + t * fullB + floor;
        LUT(1, bright, p) = pack565_from_rgbf(oR, oG, oB);
      }
      // B triad
      {
        float oR = (1.0f - t) * 0.0f  + t * fullR + floor;
        float oG = (1.0f - t) * 0.0f  + t * fullG + floor;
        float oB = (1.0f - t) * maskB + t * fullB + floor;
        LUT(2, bright, p) = pack565_from_rgbf(oR, oG, oB);
      }
    }
  }
}

void init_ctm644_lut() {
  if (gTriadLUT == NULL) {
    TriadParams P;

    if (CPC.scr_half_res_x) {
      P.BRIGHT_FACTOR      = 1.75f;
      P.DIM_FACTOR         = 0.45f;
      P.TRIAD_TINT         = 0.50f;
      P.TRIAD_TINT_DIM     = 1.00f;
      P.BLACK_FLOOR        = 12.0f;
      P.CROSS_BLEED        = 0.10f;
      P.MASK_GAIN          = 1.66f;
      P.SUBPIX_FLOOR_8     = 3.80f;
      P.GAIN_8             = 1.00f;
      P.GAMMA              = 0.75f;
      P.OUTPUT_BLACK_POINT = 0.00f;
      P.OUTPUT_SATURATION  = 1.20f;
    } else {
      P.BRIGHT_FACTOR      = 2.8f;
      P.DIM_FACTOR         = 0.6f;
      P.TRIAD_TINT         = 0.3f;
      P.TRIAD_TINT_DIM     = 0.8f;
      P.BLACK_FLOOR        = 6.0f;
      P.CROSS_BLEED        = 0.1f;
      P.MASK_GAIN          = 2.2f;
      P.SUBPIX_FLOOR_8     = 4.0f;
      P.GAIN_8             = 1.15f;
      P.GAMMA              = 0.90f;
      P.OUTPUT_BLACK_POINT = 0.08f;
      P.OUTPUT_SATURATION  = 1.0f;
    }

    gTriadLUT = new Uint16[3 * 2 * 65536];

    build_triad_lut_565(gTriadLUT, P);
  }

  if (gLumaLUT == NULL) {
      build_luma_lut_565();
  }
}

void free_ctm644_lut() {
    if (gTriadLUT) {
        delete[] gTriadLUT;
        gTriadLUT = NULL;
    }
    if (gLumaLUT) {
        delete[] gLumaLUT;
        gLumaLUT = NULL;
    }
}

#define CTM644_PATTERN_WIDTH_HALF 6
#define CTM644_PATTERN_WIDTH_FULL 12

int ctm644_pattern_width;
Uint8 (*ctm644_pattern_ch)[12];
Uint8 (*ctm644_pattern_br)[12];

// Patterns for half-res-x mode
static const Uint8 CTM644_PATTERN_CH_HALF[4][12] = {
  {0,1,2,0,1,2},
  {0,1,2,0,1,2},
  {0,1,2,0,1,2},
  {0,1,2,0,1,2},
};

static const Uint8 CTM644_PATTERN_BR_HALF[4][12] = {
  {1,1,1,1,1,1},
  {1,1,1,0,0,0},
  {1,1,1,1,1,1},
  {0,0,0,1,1,1},
};

// Patterns for full-res-x mode
static const Uint8 CTM644_PATTERN_CH_FULL[4][12] = {
  {0,0,1,1,2,2,0,0,1,1,2,2},
  {0,0,1,1,2,2,0,0,1,1,2,2},
  {0,0,1,1,2,2,0,0,1,1,2,2},
  {0,0,1,1,2,2,0,0,1,1,2,2},
};

static const Uint8 CTM644_PATTERN_BR_FULL[4][12] = {
  {1,0,1,0,1,0,1,0,1,0,1,0},
  {1,0,1,0,1,0,0,0,0,0,0,0},
  {1,0,1,0,1,0,1,0,1,0,1,0},
  {0,0,0,0,0,0,1,0,1,0,1,0},
};

void filter_ctm644(
  Uint8 *srcPtr, Uint32 srcPitch,
  Uint8 *dstPtr, Uint32 dstPitch,
  int width, int height
){
  const Uint32 nextlineSrc = srcPitch / sizeof(Uint16);
  const Uint32 nextlineDst = dstPitch / sizeof(Uint16);

  Uint16 *p = reinterpret_cast<Uint16*> (srcPtr);
  Uint16 *q = reinterpret_cast<Uint16*> (dstPtr);

  if (!gTriadLUT) return;

  int i, ii, j, jj;

  const int pattern_modulo = ctm644_pattern_width - 1;

  for (j = 0, jj = 0; j < height; ++j, jj += 4) {

    Uint16 *q0 = q;
    Uint16 *q1 = q + nextlineDst;
    Uint16 *q2 = q + (nextlineDst * 2);
    Uint16 *q3 = q + (nextlineDst * 3);

    const int pr0 = (jj + 0) & 3;
    const int pr1 = (jj + 1) & 3;
    const int pr2 = (jj + 2) & 3;
    const int pr3 = (jj + 3) & 3;

    int pcx = 0;

    for (i = 0, ii = 0; i < width; ++i, ii += 4) {

      const int im1 = (i == 0) ? 0 : (i - 1);
      const int ip1 = (i == width - 1) ? (width - 1) : (i + 1);

      const Uint16 C = p[i];
      const Uint16 L = p[im1];
      const Uint16 R = p[ip1];

      const Uint16 s = smear(C, L, R);

      const int pc0 = pcx;
      const int pc1 = (pc0 == pattern_modulo) ? 0 : (pc0 + 1);
      const int pc2 = (pc1 == pattern_modulo) ? 0 : (pc1 + 1);
      const int pc3 = (pc2 == pattern_modulo) ? 0 : (pc2 + 1);

      q0[ii + 0] = LUT_GET(ctm644_pattern_ch[pr0][pc0], ctm644_pattern_br[pr0][pc0], s);
      q0[ii + 1] = LUT_GET(ctm644_pattern_ch[pr0][pc1], ctm644_pattern_br[pr0][pc1], s);
      q0[ii + 2] = LUT_GET(ctm644_pattern_ch[pr0][pc2], ctm644_pattern_br[pr0][pc2], s);
      q0[ii + 3] = LUT_GET(ctm644_pattern_ch[pr0][pc3], ctm644_pattern_br[pr0][pc3], s);

      q1[ii + 0] = LUT_GET(ctm644_pattern_ch[pr1][pc0], ctm644_pattern_br[pr1][pc0], s);
      q1[ii + 1] = LUT_GET(ctm644_pattern_ch[pr1][pc1], ctm644_pattern_br[pr1][pc1], s);
      q1[ii + 2] = LUT_GET(ctm644_pattern_ch[pr1][pc2], ctm644_pattern_br[pr1][pc2], s);
      q1[ii + 3] = LUT_GET(ctm644_pattern_ch[pr1][pc3], ctm644_pattern_br[pr1][pc3], s);

      q2[ii + 0] = LUT_GET(ctm644_pattern_ch[pr2][pc0], ctm644_pattern_br[pr2][pc0], s);
      q2[ii + 1] = LUT_GET(ctm644_pattern_ch[pr2][pc1], ctm644_pattern_br[pr2][pc1], s);
      q2[ii + 2] = LUT_GET(ctm644_pattern_ch[pr2][pc2], ctm644_pattern_br[pr2][pc2], s);
      q2[ii + 3] = LUT_GET(ctm644_pattern_ch[pr2][pc3], ctm644_pattern_br[pr2][pc3], s);

      q3[ii + 0] = LUT_GET(ctm644_pattern_ch[pr3][pc0], ctm644_pattern_br[pr3][pc0], s);
      q3[ii + 1] = LUT_GET(ctm644_pattern_ch[pr3][pc1], ctm644_pattern_br[pr3][pc1], s);
      q3[ii + 2] = LUT_GET(ctm644_pattern_ch[pr3][pc2], ctm644_pattern_br[pr3][pc2], s);
      q3[ii + 3] = LUT_GET(ctm644_pattern_ch[pr3][pc3], ctm644_pattern_br[pr3][pc3], s);

      pcx += (4 % ctm644_pattern_width);
      if (pcx >= ctm644_pattern_width) pcx -= ctm644_pattern_width;
    }
    p += nextlineSrc;
    q += nextlineDst * 4;
  }
}

void ctm644_flip(video_plugin* t __attribute__((unused)))
{
  if (SDL_MUSTLOCK(scaled))
    SDL_LockSurface(scaled);
  SDL_Rect src;
  SDL_Rect dst;
  compute_rects(&src,&dst);
  filter_ctm644(
      static_cast<Uint8*>(pub->pixels) + (2*src.x+src.y*pub->pitch), 
      pub->pitch,
      static_cast<Uint8*>(scaled->pixels) + (2*dst.x+dst.y*scaled->pitch), 
      scaled->pitch, 
      src.w, 
      src.h);
  if (SDL_MUSTLOCK(scaled))
    SDL_UnlockSurface(scaled);
  swscale_blit();
}

SDL_Surface* ctm644_init(video_plugin* t, int scale, bool fs)
{
  init_ctm644_lut();

  if (CPC.scr_half_res_x) {
      ctm644_pattern_width = CTM644_PATTERN_WIDTH_HALF;
      ctm644_pattern_ch = const_cast<Uint8 (*)[12]> (CTM644_PATTERN_CH_HALF);
      ctm644_pattern_br = const_cast<Uint8 (*)[12]> (CTM644_PATTERN_BR_HALF);
      smear = smear_halfres;
  } else {
      ctm644_pattern_width = CTM644_PATTERN_WIDTH_FULL;
      ctm644_pattern_ch = const_cast<Uint8 (*)[12]> (CTM644_PATTERN_CH_FULL);
      ctm644_pattern_br = const_cast<Uint8 (*)[12]> (CTM644_PATTERN_BR_FULL);
      smear = smear_fullres;
  }

  return swscale_init(t, scale, fs);
}

void ctm644_close()
{
  free_ctm644_lut();
  swscale_close();
}

/* ------------------------------------------------------------------------------------ */
/* End of video plugins --------------------------------------------------------------- */
/* ------------------------------------------------------------------------------------ */

std::vector<video_plugin> video_plugin_list =
{
  // Hardware flip version are the same as software ones since switch to SDL2. Kept for compatibility of config, would be nice to not display them in the UI.
  /* Name                     Hidden Init func      Palette func     Flip func      Close func     Multiplier X/Y */  
  {"Direct",                  false, direct_init,   direct_setpal,   direct_flip,   direct_close,            1, 1 },
  {"Direct double",           true,  direct_init,   direct_setpal,   direct_flip,   direct_close,            1, 1 },
  {"Half size",               true,  direct_init,   direct_setpal,   direct_flip,   direct_close,            1, 1 },
  {"Double size",             true,  direct_init,   direct_setpal,   direct_flip,   direct_close,            1, 1 },
  {"Super eagle",             false, swscale_init,  swscale_setpal,  seagle_flip,   swscale_close,           2, 2 },
  {"Scale2x",                 false, swscale_init,  swscale_setpal,  scale2x_flip,  swscale_close,           2, 2 },
  {"Advanced Scale2x",        false, swscale_init,  swscale_setpal,  ascale2x_flip, swscale_close,           2, 2 },
  {"TV 2x",                   false, swscale_init,  swscale_setpal,  tv2x_flip,     swscale_close,           2, 2 },
  {"Software bilinear",       false, swscale_init,  swscale_setpal,  swbilin_flip,  swscale_close,           2, 2 },
  {"Software bicubic",        false, swscale_init,  swscale_setpal,  swbicub_flip,  swscale_close,           2, 2 },
  {"Dot matrix",              false, swscale_init,  swscale_setpal,  dotmat_flip,   swscale_close,           2, 2 },
#ifdef HAVE_GL
  {"OpenGL scaling",          false, glscale_init,  glscale_setpal,  glscale_flip,  glscale_close,           1, 1 },
#endif
  {"Monitor 2x",              false, swscale_init,  swscale_setpal,  monitor_flip,  swscale_close,           2, 2 },
  {"CTM644 4x",               false, ctm644_init,   swscale_setpal,  ctm644_flip,   ctm644_close,            4, 4 },
};
