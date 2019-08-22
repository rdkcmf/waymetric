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
#ifdef USE_PLATFORM_TEMPLATE

#include <stdlib.h>
#include <stdio.h>
#include <memory.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>

#include "platform.h"

typedef struct _PlatformCtx
{
   // TBD
} PlatformCtx;

PlatformCtx* PlatfromInit( void )
{
   PlatformCtx *ctx= 0;

   ctx= (PlatformCtx*)calloc( 1, sizeof(PlatformCtx) );
   if ( ctx )
   {
      // TBD
   }

exit:
   return ctx;
}

void PlatformTerm( PlatformCtx *ctx )
{
   if ( ctx )
   {
      // TBD
      free( ctx );
   }
}

NativeDisplayType PlatformGetEGLDisplayType( PlatformCtx *ctx )
{
   NativeDisplayType displayType;

   // TBD
   displayType= (NativeDisplayType)EGL_DEFAULT_DISPLAY;

   return displayType;
}

EGLDisplay PlatformGetEGLDisplay( PlatformCtx *ctx, NativeDisplayType type )
{
   EGLDisplay dpy= EGL_NO_DISPLAY;

   // TBD
   dpy= eglGetDisplay( type );

   return dpy;
}

EGLDisplay PlatformGetEGLDisplayWayland( PlatformCtx *ctx, struct wl_display *display )
{
   EGLDisplay dpy= EGL_NO_DISPLAY;

   // TBD
   dpy= eglGetDisplay( (NativeDisplayType)display );

   return dpy;
}

void *PlatformCreateNativeWindow( PlatformCtx *ctx, int width, int height )
{
   void *nativeWindow= 0;

   if ( ctx )
   {
      // TBD
   }

exit:   
   return nativeWindow;   
}

void PlatformDestroyNativeWindow( PlatformCtx *ctx, void *nativeWindow )
{
   if ( ctx )
   {
      // TBD
   }
}

#endif

