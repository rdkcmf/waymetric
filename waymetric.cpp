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
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include <sys/time.h>
#include <unistd.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>

#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h> 

#include "wayland-server.h"
#include "wayland-client.h"
#include "wayland-egl.h"

#include "platform.h"

#define UNUSED(x) ((void)x)

#define DEFAULT_WIDTH (1280)
#define DEFAULT_HEIGHT (720)
#define DEFAULT_ITERATIONS (300)

#ifndef PFNEGLGETPLATFORMDISPLAYEXTPROC
typedef EGLDisplay (EGLAPIENTRYP PFNEGLGETPLATFORMDISPLAYEXTPROC) (EGLenum platform, void *native_display, const EGLint *attrib_list);
#endif

typedef struct _AppCtx AppCtx;

#define MAX_TEXTURES (2)

typedef struct _Surface
{
   struct wl_resource *resource;
   AppCtx *appCtx;
   struct wl_listener attachedBufferDestroyListener;
   struct wl_listener detachedBufferDestroyListener;
   struct wl_resource *attachedBufferResource;
   struct wl_resource *detachedBufferResource;
   int attachedX;
   int attachedY;
   int refCount;
   int textureCount;
   GLuint textureId[MAX_TEXTURES];
   EGLImageKHR eglImage[MAX_TEXTURES];
   int bufferWidth;
   int bufferHeight;
} Surface;

typedef struct _EGLCtx
{
   AppCtx *appCtx;
   bool initialized;
   bool useWayland;
   void *nativeDisplay;
   struct wl_display *dispWayland;
   EGLDisplay eglDisplay;
   EGLContext eglContext;   
   EGLSurface eglSurface;
   EGLConfig eglConfig;
   EGLint majorVersion;
   EGLint minorVersion;
} EGLCtx;

typedef struct _AppCtx
{
   FILE *pReport;
   PlatformCtx *platformCtx;
   EGLCtx eglServer;
   EGLCtx eglClient;
   bool haveWaylandEGL;
   const char *eglVendor;
   const char *eglVersion;
   const char *eglClientAPIS;
   const char *eglExtensions;
   PFNEGLGETPLATFORMDISPLAYEXTPROC eglGetPlatformDisplayEXT;
   PFNEGLBINDWAYLANDDISPLAYWL eglBindWaylandDisplayWL;
   PFNEGLUNBINDWAYLANDDISPLAYWL eglUnbindWaylandDisplayWL;
   PFNEGLQUERYWAYLANDBUFFERWL eglQueryWaylandBufferWL;
   PFNEGLCREATEIMAGEKHRPROC eglCreateImageKHR;
   PFNEGLDESTROYIMAGEKHRPROC eglDestroyImageKHR;
   PFNGLEGLIMAGETARGETTEXTURE2DOESPROC glEGLImageTargetTexture2DOES;

   pthread_mutex_t mutex;
   const char *displayName;
   struct wl_display *dispWayland;
   pthread_t clientThreadId;
   pthread_mutex_t mutexReady;
   pthread_cond_t condReady;
   struct wl_resource *rescb;
   bool renderWayland;
   int pacingDelay;

   bool haveYUVTextures;
   bool haveYUVShaders;
   GLuint frag;
   GLuint vert;
   GLuint prog;
   GLint locPos;
   GLint locTC;
   GLint locTCUV;
   GLint locRes;
   GLint locMatrix;
   GLint locTexture;
   GLint locTextureUV;

   struct wl_compositor *compositor;
   struct wl_surface *surface;
   struct wl_egl_window *winWayland;

   int maxIterations;
   int windowWidth;
   int windowHeight;

   int directEGLIterationCount;
   long long directEGLTimeTotal;
   double directEGLFPS;

   int waylandEGLIterationCount;
   long long waylandEGLTimeTotal;
   long long waylandTotal;
   double waylandEGLFPS;

} AppCtx;

static long long getCurrentTimeMicro(void)
{
   struct timeval tv;
   long long utcCurrentTimeMicro;

   gettimeofday(&tv,0);
   utcCurrentTimeMicro= tv.tv_sec*1000000LL+tv.tv_usec;

   return utcCurrentTimeMicro;
}

static long long getCurrentTimeMillis(void)
{
   struct timeval tv;
   long long utcCurrentTimeMillis;

   gettimeofday(&tv,0);
   utcCurrentTimeMillis= tv.tv_sec*1000LL+(tv.tv_usec/1000LL);

   return utcCurrentTimeMillis;
}

#define MAX_ATTRIBS (24)
#define RED_SIZE (8)
#define GREEN_SIZE (8)
#define BLUE_SIZE (8)
#define ALPHA_SIZE (8)
#define DEPTH_SIZE (0)

static bool initEGL( EGLCtx *eglCtx )
{
   bool result= false;
   int i;
   EGLBoolean br;
   EGLint configCount;
   EGLConfig *configs= 0;
   EGLint attrs[MAX_ATTRIBS];
   EGLint redSize, greenSize, blueSize;
   EGLint alphaSize, depthSize;

   eglCtx->eglDisplay= EGL_NO_DISPLAY;
   eglCtx->eglContext= EGL_NO_CONTEXT;
   eglCtx->eglSurface= EGL_NO_SURFACE;

   if ( eglCtx->useWayland )
   {
      eglCtx->eglDisplay= PlatformGetEGLDisplayWayland( eglCtx->appCtx->platformCtx, eglCtx->dispWayland );
   }
   else
   {
      eglCtx->eglDisplay= PlatformGetEGLDisplay( eglCtx->appCtx->platformCtx, (NativeDisplayType)eglCtx->nativeDisplay );
   }
   if ( eglCtx->eglDisplay == EGL_NO_DISPLAY )
   {
      printf("Error: initEGL: eglGetDisplay failed: %X\n", eglGetError() );
      goto exit;
   }

   br= eglInitialize( eglCtx->eglDisplay, &eglCtx->majorVersion, &eglCtx->minorVersion );
   if ( !br )
   {
      printf("Error: initEGL: unable to initialize EGL display: %X\n", eglGetError() );
      goto exit;
   }

   br= eglGetConfigs( eglCtx->eglDisplay, NULL, 0, &configCount );
   if ( !br )
   {
      printf("Error: initEGL: unable to get count of EGL configurations: %X\n", eglGetError() );
      goto exit;
   }

   configs= (EGLConfig*)malloc( configCount*sizeof(EGLConfig) );
   if ( !configs )
   {
      printf("Error: initEGL: no memory for EGL configurations\n");
      goto exit;
   }

   i= 0;
   attrs[i++]= EGL_RED_SIZE;
   attrs[i++]= RED_SIZE;
   attrs[i++]= EGL_GREEN_SIZE;
   attrs[i++]= GREEN_SIZE;
   attrs[i++]= EGL_BLUE_SIZE;
   attrs[i++]= BLUE_SIZE;
   attrs[i++]= EGL_DEPTH_SIZE;
   attrs[i++]= DEPTH_SIZE;
   attrs[i++]= EGL_STENCIL_SIZE;
   attrs[i++]= 0;
   attrs[i++]= EGL_SURFACE_TYPE;
   attrs[i++]= EGL_WINDOW_BIT;
   attrs[i++]= EGL_RENDERABLE_TYPE;
   attrs[i++]= EGL_OPENGL_ES2_BIT;
   attrs[i++]= EGL_NONE;

   br= eglChooseConfig( eglCtx->eglDisplay, attrs, configs, configCount, &configCount );
   if ( !br )
   {
      printf("Error: initEGL: eglChooseConfig failed: %X\n", eglGetError() );
      goto exit;
   }

   for( i= 0; i < configCount; ++i )
   {
      eglGetConfigAttrib( eglCtx->eglDisplay, configs[i], EGL_RED_SIZE, &redSize );
      eglGetConfigAttrib( eglCtx->eglDisplay, configs[i], EGL_GREEN_SIZE, &greenSize );
      eglGetConfigAttrib( eglCtx->eglDisplay, configs[i], EGL_BLUE_SIZE, &blueSize );
      eglGetConfigAttrib( eglCtx->eglDisplay, configs[i], EGL_ALPHA_SIZE, &alphaSize );
      eglGetConfigAttrib( eglCtx->eglDisplay, configs[i], EGL_DEPTH_SIZE, &depthSize );

      printf("config %d: red: %d green: %d blue: %d alpha: %d depth: %d\n",
              i, redSize, greenSize, blueSize, alphaSize, depthSize );

      if ( (redSize == RED_SIZE) &&
           (greenSize == GREEN_SIZE) &&
           (blueSize == BLUE_SIZE) &&
           (alphaSize == ALPHA_SIZE) &&
           (depthSize >= DEPTH_SIZE) )
      {
         printf( "choosing config %d\n", i);
         break;
      }
   }
   if ( configCount == i )
   {
      printf("Error: initEGL: no suitable configuration is available\n");
      goto exit;
   }
   eglCtx->eglConfig= configs[i];

   attrs[0]= EGL_CONTEXT_CLIENT_VERSION;
   attrs[1]= 2;
   attrs[2]= EGL_NONE;
    
   eglCtx->eglContext= eglCreateContext( eglCtx->eglDisplay, eglCtx->eglConfig, EGL_NO_CONTEXT, attrs );
   if ( eglCtx->eglContext == EGL_NO_CONTEXT )
   {
      printf( "Error: initEGL: eglCreateContext failed: %X\n", eglGetError() );
      goto exit;
   }

   eglCtx->initialized= true;

   result= true;

exit:

   if ( configs )
   {
      free( configs );
   }

   return result;
}

static void termEGL( EGLCtx *eglCtx )
{
   if ( eglCtx->eglDisplay != EGL_NO_DISPLAY )
   {
      eglMakeCurrent( eglCtx->eglDisplay, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT );
   }

   if ( eglCtx->eglSurface != EGL_NO_SURFACE )
   {
      eglDestroySurface( eglCtx->eglDisplay, eglCtx->eglSurface );
      eglCtx->eglSurface= EGL_NO_SURFACE;
   }

   if ( eglCtx->eglContext != EGL_NO_CONTEXT )
   {
      eglDestroyContext( eglCtx->eglDisplay, eglCtx->eglContext );
      eglCtx->eglContext= EGL_NO_CONTEXT;
   }

   if ( eglCtx->initialized )
   {      
      eglTerminate( eglCtx->eglDisplay );
      eglReleaseThread();
      eglCtx->initialized= false;
   }
}

static const char *vertTexture=
  "attribute vec2 pos;\n"
  "attribute vec2 texcoord;\n"
  "uniform mat4 u_matrix;\n"
  "uniform vec2 u_resolution;\n"
  "varying vec2 tx;\n"
  "void main()\n"
  "{\n"
  "  vec4 v1= u_matrix * vec4(pos, 0, 1);\n"
  "  vec4 v2= v1 / vec4(u_resolution, u_resolution.x, 1);\n"
  "  vec4 v3= v2 * vec4(2.0, 2.0, 1, 1);\n"
  "  vec4 v4= v3 - vec4(1.0, 1.0, 0, 0);\n"
  "  v4.w= 1.0+v4.z;\n"
  "  gl_Position=  v4 * vec4(1, -1, 1, 1);\n"
  "  tx= texcoord;\n"
  "}\n";

static const char *fragTexture=
  "#ifdef GL_ES\n"
  "precision mediump float;\n"
  "#endif\n"
  "uniform sampler2D texture;\n"
  "varying vec2 tx;\n"
  "void main()\n"
  "{\n"
  "  gl_FragColor= texture2D(texture, tx);\n"
  "}\n";

static const char *vertTextureYUV=
  "attribute vec2 pos;\n"
  "attribute vec2 texcoord;\n"
  "attribute vec2 texcoorduv;\n"
  "uniform mat4 u_matrix;\n"
  "uniform vec2 u_resolution;\n"
  "varying vec2 tx;\n"
  "varying vec2 txuv;\n"
  "void main()\n"
  "{\n"
  "  vec4 v1= u_matrix * vec4(pos, 0, 1);\n"
  "  vec4 v2= v1 / vec4(u_resolution, u_resolution.x, 1);\n"
  "  vec4 v3= v2 * vec4(2.0, 2.0, 1, 1);\n"
  "  vec4 v4= v3 - vec4(1.0, 1.0, 0, 0);\n"
  "  v4.w= 1.0+v4.z;\n"
  "  gl_Position=  v4 * vec4(1, -1, 1, 1);\n"
  "  tx= texcoord;\n"
  "  txuv= texcoorduv;\n"
  "}\n";

static const char *fragTextureYUV=
  "#ifdef GL_ES\n"
  "precision mediump float;\n"
  "#endif\n"
  "uniform sampler2D texture;\n"
  "uniform sampler2D textureuv;\n"
  "const vec3 cc_r= vec3(1.0, -0.8604, 1.59580);\n"
  "const vec4 cc_g= vec4(1.0, 0.539815, -0.39173, -0.81290);\n"
  "const vec3 cc_b= vec3(1.0, -1.071, 2.01700);\n"
  "varying vec2 tx;\n"
  "varying vec2 txuv;\n"
  "void main()\n"
  "{\n"
  "   vec4 y_vec= texture2D(texture, tx);\n"
  "   vec4 c_vec= texture2D(textureuv, txuv);\n"
  "   vec4 temp_vec= vec4(y_vec.a, 1.0, c_vec.b, c_vec.a);\n"
  "   gl_FragColor= vec4( dot(cc_r,temp_vec.xyw), dot(cc_g,temp_vec), dot(cc_b,temp_vec.xyz), alpha );\n"
  "}\n";

static bool initGL( AppCtx *ctx )
{
   bool result= false;
   GLint status;
   GLsizei length;
   char infoLog[512];
   const char *fragSrc, *vertSrc;

   if ( ctx->haveYUVShaders )
   {
      fragSrc= fragTextureYUV;
      vertSrc= vertTextureYUV;
   }
   else
   {
      fragSrc= fragTexture;
      vertSrc= vertTexture;
   }

   ctx->frag= glCreateShader( GL_FRAGMENT_SHADER );
   if ( !ctx->frag )
   {
      printf("Error: initGL: failed to create fragment shader\n");
      goto exit;
   }

   glShaderSource( ctx->frag, 1, (const char **)&fragSrc, NULL );
   glCompileShader( ctx->frag );
   glGetShaderiv( ctx->frag, GL_COMPILE_STATUS, &status );
   if ( !status )
   {
      glGetShaderInfoLog( ctx->frag, sizeof(infoLog), &length, infoLog );
      printf("Error: initGL: compiling fragment shader: %*s\n", length, infoLog );
      goto exit;
   }

   ctx->vert= glCreateShader( GL_VERTEX_SHADER );
   if ( !ctx->vert )
   {
      printf("Error: initGL: failed to create vertex shader\n");
      goto exit;
   }

   glShaderSource( ctx->vert, 1, (const char **)&vertSrc, NULL );
   glCompileShader( ctx->vert );
   glGetShaderiv( ctx->vert, GL_COMPILE_STATUS, &status );
   if ( !status )
   {
      glGetShaderInfoLog( ctx->vert, sizeof(infoLog), &length, infoLog );
      printf("Error: initGL: compiling vertex shader: \n%*s\n", length, infoLog );
      goto exit;
   }

   ctx->prog= glCreateProgram();
   glAttachShader(ctx->prog, ctx->frag);
   glAttachShader(ctx->prog, ctx->vert);

   ctx->locPos= 0;
   ctx->locTC= 1;
   glBindAttribLocation(ctx->prog, ctx->locPos, "pos");
   glBindAttribLocation(ctx->prog, ctx->locTC, "texcoord");
   if ( ctx->haveYUVShaders )
   {
      ctx->locTCUV= 2;
      glBindAttribLocation(ctx->prog, ctx->locTCUV, "texcoorduv");
   }

   glLinkProgram(ctx->prog);
   glGetProgramiv(ctx->prog, GL_LINK_STATUS, &status);
   if (!status)
   {
      glGetProgramInfoLog(ctx->prog, sizeof(infoLog), &length, infoLog);
      printf("Error: initGL: linking:\n%*s\n", length, infoLog);
      goto exit;
   }

   ctx->locRes= glGetUniformLocation(ctx->prog,"u_resolution");
   ctx->locMatrix= glGetUniformLocation(ctx->prog,"u_matrix");
   ctx->locTexture= glGetUniformLocation(ctx->prog,"texture");
   if ( ctx->haveYUVShaders )
   {
      ctx->locTextureUV= glGetUniformLocation(ctx->prog,"textureuv");
   }

   result= true;

exit:

   return result;
}

static void termGL( AppCtx *ctx )
{
   if ( ctx->frag )
   {
      glDeleteShader( ctx->frag );
      ctx->frag= 0;
   }
   if ( ctx->vert )
   {
      glDeleteShader( ctx->vert );
      ctx->vert= 0;
   }
   if ( ctx->prog )
   {
      glDeleteProgram( ctx->prog );
      ctx->prog= 0;
   }
}

void drawGL( AppCtx *ctx, Surface *surface )
{
   int x, y, w, h;
   GLenum glerr;

   x= 0;
   y= 0;
   w= ctx->windowWidth;
   h= ctx->windowHeight;
 
   const float verts[4][2]=
   {
      { float(x), float(y) },
      { float(x+w), float(y) },
      { float(x),  float(y+h) },
      { float(x+w), float(y+h) }
   };
 
   const float uv[4][2]=
   {
      { 0,  1 },
      { 1,  1 },
      { 0,  0 },
      { 1,  0 }
   };

   const float identityMatrix[4][4]=
   {
      {1, 0, 0, 0},
      {0, 1, 0, 0},
      {0, 0, 1, 0},
      {0, 0, 0, 1}
   };

   if ( ctx->haveYUVShaders != ctx->haveYUVTextures )
   {
      termGL( ctx );
      ctx->haveYUVShaders= ctx->haveYUVTextures;
      if ( !initGL( ctx ) )
      {
         printf("Error: drawGL: initGL failed while changing shaders\n");
      }
   }

   if ( surface->textureId[0] == GL_NONE )
   {
      for ( int i= 0; i < surface->textureCount; ++i )
      {
         if ( surface->textureId[i] == GL_NONE )
         {
            glGenTextures(1, &surface->textureId[i] );
         }
       
         glActiveTexture(GL_TEXTURE0+i);
         glBindTexture(GL_TEXTURE_2D, surface->textureId[i] );
         if ( surface->eglImage[i] )
         {
            ctx->glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, surface->eglImage[i]);
         }
         glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
         glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
         glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
         glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
      }
   }

   glUseProgram(ctx->prog);
   glUniform2f(ctx->locRes, ctx->windowWidth, ctx->windowHeight);
   glUniformMatrix4fv(ctx->locMatrix, 1, GL_FALSE, (GLfloat*)identityMatrix);

   glActiveTexture(GL_TEXTURE0); 
   glBindTexture(GL_TEXTURE_2D, surface->textureId[0]);
   glUniform1i(ctx->locTexture, 0);
   glVertexAttribPointer(ctx->locPos, 2, GL_FLOAT, GL_FALSE, 0, verts);
   glVertexAttribPointer(ctx->locTC, 2, GL_FLOAT, GL_FALSE, 0, uv);
   glEnableVertexAttribArray(ctx->locPos);
   glEnableVertexAttribArray(ctx->locTC);
   if ( ctx->haveYUVTextures )
   {
      glActiveTexture(GL_TEXTURE1); 
      glBindTexture(GL_TEXTURE_2D, surface->textureId[1]);
      glUniform1i(ctx->locTexture, 1);
      glVertexAttribPointer(ctx->locTCUV, 2, GL_FLOAT, GL_FALSE, 0, uv);
      glEnableVertexAttribArray(ctx->locTCUV);
   }
   glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
   glDisableVertexAttribArray(ctx->locPos);
   glDisableVertexAttribArray(ctx->locTC);
   if ( ctx->haveYUVTextures )
   {
      glDisableVertexAttribArray(ctx->locTCUV);
   }
   
   glerr= glGetError();
   if ( glerr != GL_NO_ERROR )
   {
      printf("Warning: drawGL: glGetError: %X\n", glerr);
   }

   eglSwapBuffers( ctx->eglServer.eglDisplay, ctx->eglServer.eglSurface );
}

static void registryAdd(void *data, 
                        struct wl_registry *registry, uint32_t id,
                        const char *interface, uint32_t version)
{
   AppCtx *ctx = (AppCtx*)data;
   int len;

   len= strlen(interface);
   if ( (len==13) && !strncmp(interface, "wl_compositor", len) ) {
      ctx->compositor= (struct wl_compositor*)wl_registry_bind(registry, id, &wl_compositor_interface, 1);
   }
}

static void registryRemove(void *, struct wl_registry *, uint32_t)
{
   // ignore
}

static const struct wl_registry_listener registryListener=
{
   registryAdd,
   registryRemove
};

static void* waylandClientThread( void *arg )
{
   AppCtx *ctx= (AppCtx*)arg;
   struct wl_display *dispWayland= 0;
   struct wl_registry *registry= 0;
   long long time1, time2, diff;
   GLfloat r, g, b, t;
   int pacingInc, step, maxStep;

   fprintf(ctx->pReport, "\n");
   fprintf(ctx->pReport, "-----------------------------------------------------------------\n");
   fprintf(ctx->pReport, "Measuring EGL Wayland...\n");
   printf("\nMeasuring EGL Wayland...\n");

   pthread_mutex_lock( &ctx->mutexReady );
   pthread_cond_signal( &ctx->condReady );
   pthread_mutex_unlock( &ctx->mutexReady );

   dispWayland= wl_display_connect( ctx->displayName );
   if ( !dispWayland )
   {
      printf("Error: waylandClientThread: failed to connect to display\n");
      goto exit;
   }

   registry= wl_display_get_registry(dispWayland);
   if ( !registry )
   {
      printf("Error: waylandClientThread: failed tp get wayland registry\n");
      goto exit;
   }

   wl_registry_add_listener(registry, &registryListener, ctx);

   wl_display_roundtrip( dispWayland );

   if ( !ctx->compositor )
   {
      printf("Error: waylandClientThread: failed to obtain wayland compositor\n");
      goto exit;
   }

   ctx->eglClient.useWayland= true;
   ctx->eglClient.dispWayland= dispWayland;
   if ( !initEGL( &ctx->eglClient ) )
   {
      printf("Error: waylandClientThread: failed to setup EGL\n");
      goto exit;
   }

   ctx->surface= wl_compositor_create_surface(ctx->compositor);
   if ( !ctx->surface )
   {
      printf("Error: waylandClientThread: failed to create wayland surface\n");
      goto exit;
   }

   ctx->winWayland= wl_egl_window_create(ctx->surface, ctx->windowWidth, ctx->windowHeight);
   if ( !ctx->winWayland )
   {
      printf("Error: waylandClientThread: failed to create wayland window\n");
      goto exit;
   }

   ctx->eglClient.eglSurface= eglCreateWindowSurface( ctx->eglClient.eglDisplay,
                                                      ctx->eglClient.eglConfig,
                                                      (EGLNativeWindowType)ctx->winWayland,
                                                      NULL );
   if ( ctx->eglClient.eglSurface == EGL_NO_SURFACE )
   {
      printf("Error: waylandClientThread: failed to create EGL surface\n");
      goto exit;
   }

   eglMakeCurrent( ctx->eglClient.eglDisplay, ctx->eglClient.eglSurface, 
                   ctx->eglClient.eglSurface, ctx->eglClient.eglContext );

   eglSwapInterval( ctx->eglClient.eglDisplay, 1 );

   glClearColor( 0, 0, 0, 1 );
   glClear( GL_COLOR_BUFFER_BIT );
   eglSwapBuffers( ctx->eglClient.eglDisplay, ctx->eglClient.eglSurface );
   usleep( 1500000 );

   pacingInc= 1000;
   maxStep= 17;
   ctx->pacingDelay= 0;
   for( step= 0; step <= maxStep; ++step  )
   {
      fprintf(ctx->pReport, "\n");
      fprintf(ctx->pReport, "%d) pacing %d us\n", step+1, ctx->pacingDelay);
      printf("%d) pacing %d us\n", step+1, ctx->pacingDelay);

      ctx->waylandEGLIterationCount= 0;
      ctx->waylandEGLTimeTotal= 0;

      r= 0;
      g= 1;
      b= 0;
      time1= getCurrentTimeMicro();
      for( int i= 0; i < ctx->maxIterations; ++i )
      {
         t= r;
         r= g;
         g= b;
         b= t;
         glClearColor( r, g, b, 1 );
         glClear( GL_COLOR_BUFFER_BIT );
         if ( ctx->pacingDelay )
         {
            usleep( ctx->pacingDelay );
         }
         eglSwapBuffers( ctx->eglClient.eglDisplay, ctx->eglClient.eglSurface );
      }
      time2= getCurrentTimeMicro();

      diff= (time2-time1);
      ctx->waylandEGLIterationCount += ctx->maxIterations;
      ctx->waylandEGLTimeTotal += diff;
      if ( ctx->waylandEGLIterationCount )
      {
         ctx->waylandEGLFPS= ((double)(ctx->waylandEGLIterationCount*1000000.0)) / (double)(ctx->waylandEGLTimeTotal);
      }

      fprintf(ctx->pReport, "Iterations: %d Total time (us): %lld  FPS: %f\n", 
              ctx->waylandEGLIterationCount, ctx->waylandEGLTimeTotal, ctx->waylandEGLFPS );

      fprintf(ctx->pReport, "-----------------------------------------------------------------\n");

      ctx->pacingDelay += pacingInc;
      ctx->waylandTotal += ctx->waylandEGLTimeTotal;
   }

   glClearColor( 0, 0, 0, 1 );
   glClear( GL_COLOR_BUFFER_BIT );
   eglSwapBuffers( ctx->eglClient.eglDisplay, ctx->eglClient.eglSurface );
   usleep( 1500000 );

exit:

   if ( ctx->surface )
   {
      wl_surface_destroy( ctx->surface );
      ctx->surface= 0;
   }

   if ( ctx->compositor )
   {
      wl_compositor_destroy( ctx->compositor );
      ctx->compositor= 0;
   }

   //TODO: why does this crash on some devices?
   //termEGL( &ctx->eglClient );

   if ( ctx->winWayland )
   {
      wl_egl_window_destroy( ctx->winWayland );
      ctx->winWayland= 0;
   }

   if ( registry )
   {
      wl_registry_destroy(registry);
      registry= 0;
   }

   if ( ctx->dispWayland )
   {
      wl_display_terminate( ctx->dispWayland );
   }

   if ( dispWayland )
   {
      wl_display_roundtrip( dispWayland );
      wl_display_disconnect( dispWayland );
      dispWayland= 0;  
   }

   return NULL;
}

static void surfaceDestroy(struct wl_client *client, struct wl_resource *resource)
{
   Surface *surface= (Surface*)wl_resource_get_user_data(resource);

   wl_resource_destroy(resource);
}

static void surfaceAttach(struct wl_client *client,
                          struct wl_resource *resource,
                          struct wl_resource *bufferResource, int32_t sx, int32_t sy)
{
   Surface *surface= (Surface*)wl_resource_get_user_data(resource);

   pthread_mutex_lock( &surface->appCtx->mutex );
   if ( surface->attachedBufferResource != bufferResource )
   {
      if ( surface->detachedBufferResource )
      {
         wl_list_remove(&surface->detachedBufferDestroyListener.link);
         wl_buffer_send_release( surface->detachedBufferResource );
      }
      if ( surface->attachedBufferResource )
      {
         wl_list_remove(&surface->attachedBufferDestroyListener.link);
      }
      surface->detachedBufferResource= surface->attachedBufferResource;
      if ( surface->detachedBufferResource )
      {
         wl_resource_add_destroy_listener( surface->detachedBufferResource, &surface->detachedBufferDestroyListener );
      }
      surface->attachedBufferResource= 0;
   }
   if ( bufferResource )
   {
      if ( surface->attachedBufferResource != bufferResource )
      {
         surface->attachedBufferResource= bufferResource;
         wl_resource_add_destroy_listener( surface->attachedBufferResource, &surface->attachedBufferDestroyListener );
      }
      surface->attachedX= sx;
      surface->attachedY= sy;
   }
   pthread_mutex_unlock( &surface->appCtx->mutex );
}

static void surfaceDamage(struct wl_client *, struct wl_resource *, int32_t, int32_t, int32_t, int32_t)
{
   // ignore
}

static void surfaceFrame(struct wl_client *client, struct wl_resource *resource, uint32_t callback)
{
   Surface *surface= (Surface*)wl_resource_get_user_data(resource);
   struct wl_resource *rescb;

   rescb= wl_resource_create( client, &wl_callback_interface, 1, callback );
   if ( rescb )
   {
      surface->appCtx->rescb= rescb;
   }
   else
   {
      wl_resource_post_no_memory(resource);
   }
}

static void surfaceSetOpaqueRegion(struct wl_client *, struct wl_resource *, struct wl_resource *)
{
   // ignore
}

static void surfaceSetInputRegion(struct wl_client *, struct wl_resource *, struct wl_resource *)
{
   // ignore
}

static void surfaceCommit(struct wl_client *client, struct wl_resource *resource)
{
   Surface *surface= (Surface*)wl_resource_get_user_data(resource);
   struct wl_resource *committedBufferResource;
   AppCtx *ctx= surface->appCtx;

   pthread_mutex_lock( &ctx->mutex );

   committedBufferResource= surface->attachedBufferResource;
   if ( committedBufferResource )
   {
      if ( surface->appCtx->renderWayland )
      {
         EGLImageKHR eglImage= 0;
         EGLint value, format;
         EGLint attrList[3];
         int bufferWidth= 0, bufferHeight= 0;

         if (EGL_TRUE == ctx->eglQueryWaylandBufferWL( ctx->eglServer.eglDisplay, committedBufferResource,
                                                       EGL_WIDTH, &value ) )
         {
            bufferWidth= value;
         }                                                        

         if (EGL_TRUE == ctx->eglQueryWaylandBufferWL( ctx->eglServer.eglDisplay, committedBufferResource,
                                                       EGL_HEIGHT, &value ) )
         {
            bufferHeight= value;
         }                                                        

         if (EGL_TRUE == ctx->eglQueryWaylandBufferWL( ctx->eglServer.eglDisplay, committedBufferResource,
                                                       EGL_TEXTURE_FORMAT, &value ) )
         {
            format= value;
         }

         if ( (surface->bufferWidth != bufferWidth) || (surface->bufferHeight != bufferHeight) )
         {
            surface->bufferWidth= bufferWidth;
            surface->bufferHeight= bufferHeight;
         }

         for( int i= 0; i < MAX_TEXTURES; ++i )
         {
            if ( surface->eglImage[i] )
            {
               ctx->eglDestroyImageKHR( ctx->eglServer.eglDisplay, surface->eglImage[i] );
               surface->eglImage[i]= 0;
            }
         }

         switch ( format )
         {
            case EGL_TEXTURE_RGB:
            case EGL_TEXTURE_RGBA:
               eglImage= ctx->eglCreateImageKHR( ctx->eglServer.eglDisplay, EGL_NO_CONTEXT,
                                                        EGL_WAYLAND_BUFFER_WL, committedBufferResource,
                                                        NULL // EGLInt attrList[]
                                                       );
               if ( eglImage )
               {
                  surface->eglImage[0]= eglImage;
                  if ( surface->textureId[0] != GL_NONE )
                  {
                     glDeleteTextures( 1, &surface->textureId[0] );
                  }
                  surface->textureId[0]= GL_NONE;
                  surface->textureCount= 1;
               }
               ctx->haveYUVTextures= false;
               break;
            
            case EGL_TEXTURE_Y_U_V_WL:
               printf("Error: surfaceCommit: EGL_TEXTURE_Y_U_V_WL not supported\n" );
               break;
             
            case EGL_TEXTURE_Y_UV_WL:
               attrList[0]= EGL_WAYLAND_PLANE_WL;
               attrList[2]= EGL_NONE;
               for( int i= 0; i < 2; ++i )
               {
                  attrList[1]= i;
                  
                  eglImage= ctx->eglCreateImageKHR( ctx->eglServer.eglDisplay, EGL_NO_CONTEXT,
                                                           EGL_WAYLAND_BUFFER_WL, committedBufferResource,
                                                           attrList
                                                          );
                  if ( eglImage )
                  {
                     surface->eglImage[i]= eglImage;
                     if ( surface->textureId[i] != GL_NONE )
                     {
                        glDeleteTextures( 1, &surface->textureId[i] );
                     }
                     surface->textureId[i]= GL_NONE;
                  }
               }
               surface->textureCount= 2;
               ctx->haveYUVTextures= true;
               break;
               
            case EGL_TEXTURE_Y_XUXV_WL:
               printf("Error: surfaceCommit: EGL_TEXTURE_Y_XUXV_WL not supported\n" );
               break;
               
            default:
               printf("Error: surfaceCommit:: unknown texture format: %x\n", format );
               break;
         }

         drawGL( ctx, surface );
      }

      if ( ctx->rescb )
      {
         wl_callback_send_done( ctx->rescb, getCurrentTimeMillis() );
         wl_resource_destroy( ctx->rescb );
         ctx->rescb= 0;
      }
   }

   pthread_mutex_unlock( &ctx->mutex );
}

static void surfaceSetBufferTransform(struct wl_client *, struct wl_resource *, int)
{
   // ignore
}

static void surfaceSetBufferScale(struct wl_client *, struct wl_resource *, int32_t)
{
   // ignore
}

static const struct wl_surface_interface surface_interface= 
{
   surfaceDestroy,
   surfaceAttach,
   surfaceDamage,
   surfaceFrame,
   surfaceSetOpaqueRegion,
   surfaceSetInputRegion,
   surfaceCommit,
   surfaceSetBufferTransform,
   surfaceSetBufferScale
};

static void attachedBufferDestroyCallback(struct wl_listener *listener, void *data )
{
   Surface *surface= wl_container_of(listener, surface, attachedBufferDestroyListener );
   surface->attachedBufferResource= 0;
}

static void detachedBufferDestroyCallback(struct wl_listener *listener, void *data )
{
   Surface *surface= wl_container_of(listener, surface, detachedBufferDestroyListener );
   surface->detachedBufferResource= 0;
}

static void destroySurfaceCallback(struct wl_resource *resource)
{
   Surface *surface= (Surface*)wl_resource_get_user_data(resource);

   AppCtx *ctx= surface->appCtx;

   pthread_mutex_lock( &ctx->mutex );

   surface->resource= NULL;

   if ( --surface->refCount <= 0 )
   {
      if ( surface->attachedBufferResource || surface->detachedBufferResource )
      {
         if ( surface->detachedBufferResource )
         {
            wl_list_remove(&surface->detachedBufferDestroyListener.link);
            wl_buffer_send_release( surface->detachedBufferResource );
         }
         if ( surface->attachedBufferResource )
         {
            wl_list_remove(&surface->attachedBufferDestroyListener.link);
            wl_buffer_send_release( surface->attachedBufferResource );
         }
         surface->attachedBufferResource= 0;
         surface->detachedBufferResource= 0;
      }
      free( surface );
   }

   pthread_mutex_unlock( &ctx->mutex );
}

static void compositorCreateSurface( struct wl_client *client, struct wl_resource *resource, uint32_t id)
{
   AppCtx *ctx= (AppCtx*)wl_resource_get_user_data(resource);
   Surface *surface;
   
   pthread_mutex_lock( &ctx->mutex );

   surface= (Surface*)calloc( 1, sizeof(Surface) );   
   if (!surface) 
   {
      wl_resource_post_no_memory(resource);
      pthread_mutex_unlock( &ctx->mutex );
      return;
   }

   surface->appCtx= ctx;
   surface->refCount= 1;
   surface->attachedBufferDestroyListener.notify= attachedBufferDestroyCallback;
   surface->detachedBufferDestroyListener.notify= detachedBufferDestroyCallback;

   surface->resource= wl_resource_create(client, &wl_surface_interface, wl_resource_get_version(resource), id);
   if (!surface->resource)
   {
      free(surface);
      wl_resource_post_no_memory(resource);
      pthread_mutex_unlock( &ctx->mutex );
      return;
   }

   wl_resource_set_implementation(surface->resource, &surface_interface, surface, destroySurfaceCallback);

   pthread_mutex_unlock( &ctx->mutex );
}

static void compositorCreateRegion(struct wl_client *, struct wl_resource *resource, uint32_t)
{
   wl_resource_post_no_memory(resource);
}

static const struct wl_compositor_interface compositor_interface= 
{
   compositorCreateSurface,
   compositorCreateRegion
};

static void compositorBind( struct wl_client *client, void *data, uint32_t version, uint32_t id)
{
   AppCtx *ctx= (AppCtx*)data;
   struct wl_resource *resource;

   resource= wl_resource_create(client, &wl_compositor_interface, version, id);
   if (!resource)
   {
      wl_client_post_no_memory(client);
   }
   else
   {
      wl_resource_set_implementation(resource, &compositor_interface, ctx, 0);
   }
}

bool initWayland( AppCtx *ctx )
{
   bool result= false;

   setenv( "XDG_RUNTIME_DIR", "/tmp", true );

   ctx->dispWayland= wl_display_create();
   if ( !ctx->dispWayland )
   {
      printf("Error: initWayland: wl_display_create failed\n");
      goto exit;
   }

   if (!wl_global_create(ctx->dispWayland, &wl_compositor_interface, 3, ctx, compositorBind))
   {
      printf("Error: initWayland: failed to create compositor interface\n");
      goto exit;
   }

   if ( wl_display_add_socket( ctx->dispWayland, ctx->displayName ) )
   {
      printf("Error: initWayland: failed to add socket\n");
      goto exit;
   }

   if ( !ctx->eglBindWaylandDisplayWL( ctx->eglServer.eglDisplay, ctx->dispWayland ) )
   {
      printf("Error: initWayland: failed to bind EGL to wayland display\n");
      goto exit;
   }

   result= true;

exit:
   return result;
}

void termWayland( AppCtx *ctx )
{
   if ( ctx->dispWayland )
   {
      ctx->eglUnbindWaylandDisplayWL( ctx->eglServer.eglDisplay, ctx->dispWayland );

      wl_display_destroy(ctx->dispWayland);
      ctx->dispWayland= 0;
   }

   unsetenv( "XDG_RUNTIME_DIR" );
}

void measureWaylandEGL( AppCtx *ctx, EGLCtx *eglCtx )
{
   int rc;
   void *nativeWindow= 0;

   if ( ctx->renderWayland )
   {
      nativeWindow= PlatformCreateNativeWindow( ctx->platformCtx, ctx->windowWidth, ctx->windowHeight );
      if ( nativeWindow )
      {
         eglCtx->eglSurface= eglCreateWindowSurface( eglCtx->eglDisplay,
                                                     eglCtx->eglConfig,
                                                     (EGLNativeWindowType)nativeWindow,
                                                     NULL );
         if ( eglCtx->eglSurface != EGL_NO_SURFACE )
         {
            eglMakeCurrent( eglCtx->eglDisplay, eglCtx->eglSurface, eglCtx->eglSurface, eglCtx->eglContext );

            eglSwapInterval( eglCtx->eglDisplay, 1 );

            if ( !initGL( ctx ) )
            {
               printf("Error: measureWaylandEGL: initGL failed\n");
            }
         }
         else
         {  
            printf("Error: measureWaylandEGL: failed to create EGL surface\n");
         }
      }
      else
      {
         printf("Error: measureWaylandEGL: failed to create native window\n");
      }
   }

   pthread_mutex_lock( &ctx->mutexReady );
   rc= pthread_create( &ctx->clientThreadId, NULL, waylandClientThread, ctx );
   if ( !rc )
   {
      pthread_cond_wait( &ctx->condReady, &ctx->mutexReady );
      pthread_mutex_unlock( &ctx->mutexReady );

      wl_display_run( ctx->dispWayland );
   }
   else
   {
      pthread_mutex_unlock( &ctx->mutexReady );
   }

   if ( ctx->renderWayland )
   {
      eglMakeCurrent( eglCtx->eglDisplay, EGL_NO_CONTEXT, EGL_NO_SURFACE, EGL_NO_SURFACE );

      if ( eglCtx->eglSurface != EGL_NO_SURFACE )
      {
         termGL( ctx );

         eglDestroySurface( eglCtx->eglDisplay, eglCtx->eglSurface );
         eglCtx->eglSurface= EGL_NO_SURFACE;
      }

      if ( nativeWindow )
      {
         PlatformDestroyNativeWindow( ctx->platformCtx, nativeWindow );
      }
   }
}

void measureDirectEGL( AppCtx *ctx, EGLCtx *eglCtx )
{
   void *nativeWindow= 0;
   long long time1, time2, diff;

   nativeWindow= PlatformCreateNativeWindow( ctx->platformCtx, ctx->windowWidth, ctx->windowHeight );
   if ( nativeWindow )
   {
      eglCtx->eglSurface= eglCreateWindowSurface( eglCtx->eglDisplay,
                                                  eglCtx->eglConfig,
                                                  (EGLNativeWindowType)nativeWindow,
                                                  NULL );
      if ( eglCtx->eglSurface != EGL_NO_SURFACE )
      {
         GLfloat r, g, b, t;

         eglMakeCurrent( eglCtx->eglDisplay, eglCtx->eglSurface, eglCtx->eglSurface, eglCtx->eglContext );

         eglSwapInterval( eglCtx->eglDisplay, 1 );

         glClearColor( 0, 0, 0, 1 );
         glClear( GL_COLOR_BUFFER_BIT );
         eglSwapBuffers( eglCtx->eglDisplay, eglCtx->eglSurface );

         r= 0;
         g= 1;
         b= 0;
         time1= getCurrentTimeMicro();
         for( int i= 0; i < ctx->maxIterations; ++i )
         {
            t= r;
            r= g;
            g= b;
            b= t;
            glClearColor( r, g, b, 1 );
            glClear( GL_COLOR_BUFFER_BIT );
            if ( ctx->pacingDelay )
            {
               usleep( ctx->pacingDelay );
            }            
            eglSwapBuffers( eglCtx->eglDisplay, eglCtx->eglSurface );
         }
         time2= getCurrentTimeMicro();

         glClearColor( 0, 0, 0, 1 );
         glClear( GL_COLOR_BUFFER_BIT );
         eglSwapBuffers( eglCtx->eglDisplay, eglCtx->eglSurface );

         diff= (time2-time1);
         ctx->directEGLIterationCount += ctx->maxIterations;
         ctx->directEGLTimeTotal += diff;
         if ( ctx->directEGLIterationCount )
         {
            ctx->directEGLFPS= ((double)(ctx->directEGLIterationCount*1000000.0)) / (double)(ctx->directEGLTimeTotal);
         }

         eglMakeCurrent( eglCtx->eglDisplay, EGL_NO_CONTEXT, EGL_NO_SURFACE, EGL_NO_SURFACE );

         eglDestroySurface( eglCtx->eglDisplay, eglCtx->eglSurface );
         eglCtx->eglSurface= EGL_NO_SURFACE;
      }
      else
      {
         printf("Error: measureDirectEGL: eglSurface %p eglGetError %X\n", eglCtx->eglSurface, eglGetError());
      }

      PlatformDestroyNativeWindow( ctx->platformCtx, nativeWindow );
   }
}

void showUsage( void )
{
   printf("Usage:\n");
   printf("waymetric <options> [<report-file>]\n");
   printf("where\n");
   printf("options are one of:\n");
   printf("--window-size <width>x<height> (eg --window-size 640x480)\n");
   printf("--iterations <count>\n");
   printf("--no-direct\n");
   printf("--no-wayland\n");
   printf("--no-wayland-render\n");
   printf("-? : show usage\n");
   printf("\n");
}

int main( int argc, const char **argv )
{
   int nRC= -1;
   int argidx;
   AppCtx *ctx= 0;
   const char *s;
   bool noDirect= false;
   bool noWayland= false;
   bool noWaylandRender= false;
   const char *reportFilename= 0;
   int pacingInc, step, maxStep;
   long long directTotal, waylandTotal;

   printf("waymetric v0.41\n");

   ctx= (AppCtx*)calloc( 1, sizeof(AppCtx) );
   if ( !ctx )
   {
      printf("Error: unable allocate AppCtx\n");
      goto exit;
   }
   pthread_mutex_init( &ctx->mutex, 0 );
   pthread_mutex_init( &ctx->mutexReady, 0 );
   pthread_cond_init( &ctx->condReady, 0 );
   ctx->displayName= "waymetric0";
   ctx->maxIterations= DEFAULT_ITERATIONS;
   ctx->windowWidth= DEFAULT_WIDTH;
   ctx->windowHeight= DEFAULT_HEIGHT;

   argidx= 1;
   while( argidx < argc )
   {
      if ( argv[argidx][0] == '-' )
      {
         int len= strlen( argv[argidx] );
         if ( (len == 2) && !strncmp( argv[argidx], "-?", len) )
         {
            showUsage();
            exit(0);
         }
         else if ( (len == 13) && !strncmp( argv[argidx], "--window-size", len) )
         {
            ++argidx;
            if ( argidx < argc )
            {
               int w, h;
               if ( sscanf( argv[argidx], "%dx%d", &w, &h ) == 2 )
               {
                  ctx->windowWidth= w;
                  ctx->windowHeight= h;
               }
            }
         }
         else if ( (len == 12) && !strncmp( argv[argidx], "--iterations", len) )
         {
            ++argidx;
            if ( argidx < argc )
            {
               ctx->maxIterations= atoi( argv[argidx] );
            }
         }
         else if ( (len == 11) && !strncmp( argv[argidx], "--no-direct", len) )
         {
            noDirect= true;
         }
         else if ( (len == 12) && !strncmp( argv[argidx], "--no-wayland", len) )
         {
            noWayland= true;
         }
         else if ( (len == 19) && !strncmp( argv[argidx], "--no-wayland-render", len) )
         {
            noWaylandRender= true;
         }
      }
      else
      {
         if ( !reportFilename )
         {
            reportFilename= argv[argidx];
         }
      }
      ++argidx;
   }

   if ( !reportFilename )
   {
      reportFilename= "/tmp/waymetric-report.txt";
   }

   ctx->pReport= fopen( reportFilename, "wt");

   ctx->platformCtx= PlatfromInit();
   if ( !ctx->platformCtx )
   {
      printf("ErrorL WayMetPlatformInit failed\n");
      goto exit;
   }

   ctx->eglServer.appCtx= ctx;
   ctx->eglClient.appCtx= ctx;

   ctx->eglServer.useWayland= false;
   ctx->eglServer.nativeDisplay= PlatformGetEGLDisplayType( ctx->platformCtx );
   if ( !initEGL( &ctx->eglServer ) )
   {
      printf("Error: failed to setup EGL\n");
      goto exit;
   }

   // Check for extensions
   s= eglQueryString( ctx->eglServer.eglDisplay, EGL_VENDOR );
   if ( s )
   {
      ctx->eglVendor= strdup(s);
   }
   s= eglQueryString( ctx->eglServer.eglDisplay, EGL_VERSION );
   if ( s )
   {
      ctx->eglVersion= strdup(s);
   }
   s= eglQueryString( ctx->eglServer.eglDisplay, EGL_CLIENT_APIS );
   if ( s )
   {
      ctx->eglClientAPIS= strdup(s);
   }
   s= eglQueryString( ctx->eglServer.eglDisplay, EGL_EXTENSIONS );
   if ( s )
   {
      ctx->eglExtensions= strdup(s);
   }
   ctx->eglGetPlatformDisplayEXT= (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress( "eglGetPlatformDisplayEXT" );
   ctx->eglBindWaylandDisplayWL= (PFNEGLBINDWAYLANDDISPLAYWL)eglGetProcAddress("eglBindWaylandDisplayWL");
   ctx->eglUnbindWaylandDisplayWL= (PFNEGLUNBINDWAYLANDDISPLAYWL)eglGetProcAddress("eglUnbindWaylandDisplayWL");
   ctx->eglQueryWaylandBufferWL= (PFNEGLQUERYWAYLANDBUFFERWL)eglGetProcAddress("eglQueryWaylandBufferWL");
   ctx->eglCreateImageKHR= (PFNEGLCREATEIMAGEKHRPROC)eglGetProcAddress("eglCreateImageKHR");
   ctx->eglDestroyImageKHR= (PFNEGLDESTROYIMAGEKHRPROC)eglGetProcAddress("eglDestroyImageKHR");
   ctx->glEGLImageTargetTexture2DOES= (PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)eglGetProcAddress("glEGLImageTargetTexture2DOES");

   if ( strstr( ctx->eglExtensions, "EGL_WL_bind_wayland_display" ) &&
        ctx->eglBindWaylandDisplayWL &&
        ctx->eglUnbindWaylandDisplayWL &&
        ctx->eglQueryWaylandBufferWL &&
        ctx->eglCreateImageKHR &&
        ctx->eglDestroyImageKHR &&
        ctx->glEGLImageTargetTexture2DOES )
   {
      ctx->haveWaylandEGL= true;
   }

   fprintf(ctx->pReport, "waymetric v0.41\n");
   fprintf(ctx->pReport, "-----------------------------------------------------------------\n");
   fprintf(ctx->pReport, "Have wayland-egl: %d\n", ctx->haveWaylandEGL );
   fprintf(ctx->pReport, "-----------------------------------------------------------------\n");
   fprintf(ctx->pReport, "EGL_VENDOR: (%s)\n", ctx->eglVendor );
   fprintf(ctx->pReport, "EGL_VERSION: (%s)\n", ctx->eglVersion );
   fprintf(ctx->pReport, "EGL_CLIENT_APIS: (%s)\n", ctx->eglClientAPIS );
   fprintf(ctx->pReport, "EGL_EXTENSIONS: (%s)\n", ctx->eglExtensions );
   fprintf(ctx->pReport, "eglGetPlatformDisplayEXT: %s\n", ctx->eglGetPlatformDisplayEXT ? "true" : "false");
   fprintf(ctx->pReport, "eglBindWaylandDisplayWL: %s\n", ctx->eglBindWaylandDisplayWL ? "true" : "false");
   fprintf(ctx->pReport, "eglUnbindWaylandDisplayWL: %s\n", ctx->eglUnbindWaylandDisplayWL ? "true" : "false");
   fprintf(ctx->pReport, "eglQueryWaylandBufferWL: %s\n", ctx->eglQueryWaylandBufferWL ? "true" : "false");
   fprintf(ctx->pReport, "eglCreateImageKHR: %s\n", ctx->eglCreateImageKHR ? "true" : "false");
   fprintf(ctx->pReport, "eglDestroyImageKHR: %s\n", ctx->eglDestroyImageKHR ? "true" : "false");
   fprintf(ctx->pReport, "glEGLImageTargetTexture2DOES: %s\n", ctx->glEGLImageTargetTexture2DOES ? "true" : "false");
   fprintf(ctx->pReport, "-----------------------------------------------------------------\n");

   if ( !noWayland && ctx->haveWaylandEGL )
   { 
      if ( !initWayland( ctx ) )
      {
         printf("Error: initWayland failed\n");
         goto exit;
      }
   }

   pacingInc= 1000;
   maxStep= 17;
   ctx->pacingDelay= 0;
   directTotal= 0;
   waylandTotal= 0;

   if ( !noDirect )
   {
      fprintf(ctx->pReport, "\n");
      fprintf(ctx->pReport, "-----------------------------------------------------------------\n");
      fprintf(ctx->pReport, "Measuring EGL direct...\n");
      printf("\nMeasuring EGL direct...\n");

      for( step= 0; step <= maxStep; ++step  )
      {
         fprintf(ctx->pReport, "\n");
         fprintf(ctx->pReport, "%d) pacing %d us\n", step+1, ctx->pacingDelay);
         printf("%d) pacing %d us\n", step+1, ctx->pacingDelay);

         ctx->directEGLIterationCount= 0;
         ctx->directEGLTimeTotal= 0;
         measureDirectEGL( ctx, &ctx->eglServer );

         fprintf(ctx->pReport, "Iterations: %d Total time (us): %lld  FPS: %f\n", 
                 ctx->directEGLIterationCount, ctx->directEGLTimeTotal, ctx->directEGLFPS );

         fprintf(ctx->pReport, "-----------------------------------------------------------------\n");

         directTotal += ctx->directEGLTimeTotal;

         ctx->pacingDelay += pacingInc;
      }
   }

   if ( !noWayland && ctx->haveWaylandEGL )
   {
      ctx->renderWayland= !noWaylandRender;
      measureWaylandEGL( ctx, &ctx->eglServer );

      waylandTotal= ctx->waylandTotal;
   }

   if ( directTotal > 0 )
   {
      fprintf(ctx->pReport, "\n");
      fprintf(ctx->pReport, "=================================================================\n");
      fprintf(ctx->pReport, "waymetric speed index: %f\n", ((double)waylandTotal / (double)directTotal) );
      fprintf(ctx->pReport, "=================================================================\n");
   }

   printf("\n");
   printf("writing report to %s\n", reportFilename );

   nRC= 0;

exit:

   if ( ctx )
   {
      if ( !noWayland && ctx->haveWaylandEGL )
      {
         termWayland( ctx );
      }

      termEGL( &ctx->eglServer );

      if ( ctx->platformCtx )
      {
         PlatformTerm( ctx->platformCtx );
         ctx->platformCtx= 0;
      }

      if ( ctx->eglVendor )
      {
         free( (void*)ctx->eglVendor );
         ctx->eglVendor= 0;
      }

      if ( ctx->eglVersion )
      {
         free( (void*)ctx->eglVersion );
         ctx->eglVersion= 0;
      }

      if ( ctx->eglClientAPIS )
      {
         free( (void*)ctx->eglClientAPIS );
         ctx->eglClientAPIS= 0;
      }

      if ( ctx->eglExtensions )
      {
         free( (void*)ctx->eglExtensions );
         ctx->eglExtensions= 0;
      }

      pthread_cond_destroy( &ctx->condReady );
      pthread_mutex_destroy( &ctx->mutexReady );
      pthread_mutex_destroy( &ctx->mutex );

      if ( ctx->pReport )
      {
         fclose( ctx->pReport );
         ctx->pReport= 0;
      }

      free( ctx );
   }

   return nRC;
}

