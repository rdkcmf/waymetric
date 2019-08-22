/*
 * If not stated otherwise in this file or this component's Licenses.txt file the
 * following copyright and licenses apply:
 *
 * Copyright 2019 RDK Management
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#ifdef USE_PLATFORM_USERLAND

#include <stdlib.h>
#include <stdio.h>
#include <memory.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>

#include <bcm_host.h>

#include "platform.h"

typedef struct _PlatformCtx
{
   int displayId;
   int displayWidth;
   int displayHeight;
   DISPMANX_DISPLAY_HANDLE_T dispmanDisplay;
} PlatformCtx;

PlatformCtx* PlatfromInit( void )
{
   PlatformCtx *ctx= 0;

   bcm_host_init();

   ctx= (PlatformCtx*)calloc( 1, sizeof(PlatformCtx) );
   if ( ctx )
   {
      int displayId;
      uint32_t rc, width, height;
      TV_DISPLAY_STATE_T tvstate;
      DISPMANX_DISPLAY_HANDLE_T dispmanDisplay;

      displayId= DISPMANX_ID_MAIN_LCD;
      rc= graphics_get_display_size( displayId,
                                             &width,
                                             &height );
      if ( rc < 0 )
      {
         printf("Error: PlatformInit: graphics_get_display_size failed\n");
         free(ctx);
         ctx= 0;
         goto exit;
      }

      printf("PlatformInit: display %d size %dx%d\n", displayId, width, height );

      dispmanDisplay= vc_dispmanx_display_open( displayId );
      if ( dispmanDisplay == DISPMANX_NO_HANDLE )
      {
         printf("Error: PlatformInit: vc_dispmanx_display_open failed for display %d\n", displayId );
         free(ctx);
         ctx= 0;
         goto exit;
      }
      printf("PlatformInit: dispmanDisplay %p\n", dispmanDisplay );

      ctx->displayId= displayId;
      ctx->displayWidth= width;
      ctx->displayHeight= height;
      ctx->dispmanDisplay= dispmanDisplay;
   }

exit:
   return ctx;
}

void PlatformTerm( PlatformCtx *ctx )
{
   if ( ctx )
   {
      if ( ctx->dispmanDisplay != DISPMANX_NO_HANDLE )
      {
         vc_dispmanx_display_close( ctx->dispmanDisplay );
         ctx->dispmanDisplay= DISPMANX_NO_HANDLE;
      }
      bcm_host_deinit();

      free( ctx );
   }
}

NativeDisplayType PlatformGetEGLDisplayType( PlatformCtx *ctx )
{
   NativeDisplayType displayType;

   displayType= (NativeDisplayType)EGL_DEFAULT_DISPLAY;

   return displayType;
}

EGLDisplay PlatformGetEGLDisplay( PlatformCtx *ctx, NativeDisplayType type )
{
   EGLDisplay dpy= EGL_NO_DISPLAY;

   dpy= eglGetDisplay( type );

   return dpy;
}

EGLDisplay PlatformGetEGLDisplayWayland( PlatformCtx *ctx, struct wl_display *display )
{
   EGLDisplay dpy= EGL_NO_DISPLAY;

   dpy= eglGetDisplay( (NativeDisplayType)display );

   return dpy;
}

void *PlatformCreateNativeWindow( PlatformCtx *ctx, int width, int height )
{
   void *nativeWindow= 0;

   if ( ctx )
   {
      if ( ctx->displayWidth < width ) width= ctx->displayWidth;
      if ( ctx->displayHeight < height ) height= ctx->displayHeight;

      if ( ctx->dispmanDisplay )
      {
         VC_RECT_T destRect;
         VC_RECT_T srcRect;
         DISPMANX_ELEMENT_HANDLE_T dispmanElement;
         DISPMANX_UPDATE_HANDLE_T dispmanUpdate;
         EGL_DISPMANX_WINDOW_T *nw;

         dispmanUpdate= vc_dispmanx_update_start( 0 );
         if ( dispmanUpdate == DISPMANX_NO_HANDLE )
         {
            printf("Error: PlatformCreateNativeWindow: vc_dispmanx_update_start failed\n" );
            goto exit;
         }
         printf("PlatformCreateNativeWindow: dispmanUpdate %p\n", dispmanUpdate );
      
         nw= (EGL_DISPMANX_WINDOW_T *)calloc( 1, sizeof(EGL_DISPMANX_WINDOW_T) );
         if ( nw )
         {
            // Dest rect uses 32.0 fixed point
            destRect.x= 0;
            destRect.y= 0;
            destRect.width= width;
            destRect.height= height;

            // Src rect uses 16.16 fixed point
            srcRect.x= 0;
            srcRect.y= 0;
            srcRect.width= (width<<16);
            srcRect.height= (height<<16);
            
            dispmanElement= vc_dispmanx_element_add( dispmanUpdate,
                                                     ctx->dispmanDisplay,
                                                     110, //layer
                                                     &destRect,
                                                     0, //src
                                                     &srcRect,
                                                     DISPMANX_PROTECTION_NONE,
                                                     0, //alpha
                                                     0, //clamp
                                                     DISPMANX_NO_ROTATE  //transform
                                                   );
             if ( dispmanElement !=  DISPMANX_NO_HANDLE )
             {
                printf("PlatformCreateNativeWindow: dispmanElement %p\n", dispmanElement );
                
                nw->element= dispmanElement;
                nw->width= width;
                nw->height= height;
                
                nativeWindow= (void*)nw;
         
                vc_dispmanx_update_submit_sync( dispmanUpdate );
             }                                                                            
         }
      }
   }

exit:   
   return nativeWindow;   
}

void PlatformDestroyNativeWindow( PlatformCtx *ctx, void *nativeWindow )
{
   if ( ctx )
   {
      EGL_DISPMANX_WINDOW_T *nw;
      DISPMANX_UPDATE_HANDLE_T dispmanUpdate;

      nw= (EGL_DISPMANX_WINDOW_T*)nativeWindow;
      if ( nw )
      {
         dispmanUpdate= vc_dispmanx_update_start( 0 );
         if ( dispmanUpdate != DISPMANX_NO_HANDLE )
         {
            vc_dispmanx_element_remove( dispmanUpdate,
                                        nw->element );
         }
                                     
         free( nw );
      }
   }
}

#endif

