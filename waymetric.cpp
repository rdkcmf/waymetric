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
#include <dlfcn.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#if defined (USE_MESA)
#include <EGL/eglmesaext.h>
#endif

#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h> 

#include "wayland-server.h"
#include "wayland-client.h"
#include "wayland-egl.h"

#include "platform.h"

#include <vector>

#define WAYMETRIC_VERSION "0.60"

#define UNUSED(x) ((void)x)

#define MIN(x,y) (((x) < (y)) ? (x) : (y))

#define DEFAULT_WIDTH (1280)
#define DEFAULT_HEIGHT (720)
#define DEFAULT_ITERATIONS (300)

#define FRAME_PERIOD_MILLIS_60FPS (1000/60)

#ifndef PFNEGLGETPLATFORMDISPLAYEXTPROC
typedef EGLDisplay (EGLAPIENTRYP PFNEGLGETPLATFORMDISPLAYEXTPROC) (EGLenum platform, void *native_display, const EGLint *attrib_list);
#endif

typedef bool (*PFNREMOTEBEGIN)( struct wl_display *dspsrc, struct wl_display *dspdst );
typedef void (*PFNREMOTEEND)( struct wl_display *dspsrc, struct wl_display *dspdst );
typedef struct wl_buffer* (*PFNREMOTECLONEBUFFERFROMRESOURCE)( struct wl_display *dspsrc,
                                                               struct wl_resource *resource,
                                                               struct wl_display *dspdst,
                                                               int *width,
                                                               int *height );

typedef struct _AppCtx AppCtx;
typedef struct _WaylandCtx WaylandCtx;

#define MAX_TEXTURES (2)

typedef struct _Surface
{
   struct wl_resource *resource;
   WaylandCtx *ctx;
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
   struct wl_surface *surfaceNested;
} Surface;

typedef struct _EGLCtx
{
   AppCtx *appCtx;
   bool initialized;
   bool useWayland;
   void *nativeDisplay;
   struct wl_display *dispWayland;
   bool displayBound;
   EGLDisplay eglDisplay;
   EGLContext eglContext;   
   EGLSurface eglSurface;
   EGLConfig eglConfig;
   EGLint majorVersion;
   EGLint minorVersion;
} EGLCtx;

typedef struct _GLCtx
{
   AppCtx *appCtx;
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
} GLCtx;

typedef struct _NestedBufferInfo
{
   WaylandCtx *ctx;
   struct wl_surface *surface;
   struct wl_resource *bufferRemote;
} NestedBufferInfo;

typedef struct _WaylandCtx
{
   AppCtx *appCtx;
   EGLCtx eglServer;
   EGLCtx eglClient;
   GLCtx gl;
   pthread_mutex_t mutex;
   pthread_mutex_t mutexReady;
   pthread_cond_t condReady;
   struct wl_compositor *compositor;
   struct wl_surface *surface;
   struct wl_egl_window *winWayland;
   struct wl_display *dispWayland;
   struct wl_resource *rescb;
   struct wl_event_source *displayTimer;
   const char *upstreamDisplayName;
   bool isRepeater;
   struct wl_display *upstreamDisplay;
   pthread_mutex_t buffersToReleaseMutex;
   std::vector<NestedBufferInfo> buffersToRelease;
} WaylandCtx;

typedef struct _MultiComp
{
   AppCtx *appCtx;
   pthread_mutex_t mutexReady;
   pthread_cond_t condReady;
   pthread_mutex_t mutexStart;
   pthread_cond_t condStart;
   pthread_t threadId;
   char displayName[16];
   WaylandCtx ctx;
   int index;
   bool started;
   bool init;
   bool term;
   bool error;
} MultiComp;

typedef struct _AppCtx
{
   FILE *pReport;
   PlatformCtx *platformCtx;
   WaylandCtx master;
   WaylandCtx nested;
   WaylandCtx client;
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

   PFNREMOTEBEGIN remoteBegin;
   PFNREMOTEEND remoteEnd;
   PFNREMOTECLONEBUFFERFROMRESOURCE remoteCloneBufferFromResource;
   bool canRemoteClone;

   pthread_mutex_t mutex;
   const char *displayName;
   const char *nestedDisplayName;
   pthread_t clientThreadId;
   pthread_t clientDispatchThreadId;
   pthread_t nestedThreadId;
   pthread_t nestedDispatchThreadId;
   bool renderWayland;
   int pacingDelay;

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

#define RESULT_FILE "/tmp/waymetric-result"
#define RESULT_FORMAT "result: %lld\n"

static void writeResult( long long value )
{
   FILE *pFile= 0;
   pFile= fopen( RESULT_FILE, "wt");
   if ( pFile )
   {
      fprintf( pFile, RESULT_FORMAT, value );
      fclose( pFile );
   }
   else
   {
      printf("Error: unable to open result file: %s\n", RESULT_FILE);
   }
}

static long long readResult()
{
   long long value= 0;
   FILE *pFile= 0;
   pFile= fopen( RESULT_FILE, "rt");
   if ( pFile )
   {
      if ( fscanf( pFile, RESULT_FORMAT, &value ) != 1 )
      {
         printf("Error: unable to parse result from %s\n" RESULT_FILE);
      }
      fclose( pFile );
      remove( RESULT_FILE );
   }
   else
   {
      printf("Error: unable to open result file: %s\n", RESULT_FILE);
   }
   return value;
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

static bool initGL( WaylandCtx *ctx )
{
   bool result= false;
   GLint status;
   GLsizei length;
   char infoLog[512];
   const char *fragSrc, *vertSrc;

   if ( ctx->gl.haveYUVShaders )
   {
      fragSrc= fragTextureYUV;
      vertSrc= vertTextureYUV;
   }
   else
   {
      fragSrc= fragTexture;
      vertSrc= vertTexture;
   }

   ctx->gl.frag= glCreateShader( GL_FRAGMENT_SHADER );
   if ( !ctx->gl.frag )
   {
      printf("Error: initGL: failed to create fragment shader\n");
      goto exit;
   }

   glShaderSource( ctx->gl.frag, 1, (const char **)&fragSrc, NULL );
   glCompileShader( ctx->gl.frag );
   glGetShaderiv( ctx->gl.frag, GL_COMPILE_STATUS, &status );
   if ( !status )
   {
      glGetShaderInfoLog( ctx->gl.frag, sizeof(infoLog), &length, infoLog );
      printf("Error: initGL: compiling fragment shader: %*s\n", length, infoLog );
      goto exit;
   }

   ctx->gl.vert= glCreateShader( GL_VERTEX_SHADER );
   if ( !ctx->gl.vert )
   {
      printf("Error: initGL: failed to create vertex shader\n");
      goto exit;
   }

   glShaderSource( ctx->gl.vert, 1, (const char **)&vertSrc, NULL );
   glCompileShader( ctx->gl.vert );
   glGetShaderiv( ctx->gl.vert, GL_COMPILE_STATUS, &status );
   if ( !status )
   {
      glGetShaderInfoLog( ctx->gl.vert, sizeof(infoLog), &length, infoLog );
      printf("Error: initGL: compiling vertex shader: \n%*s\n", length, infoLog );
      goto exit;
   }

   ctx->gl.prog= glCreateProgram();
   glAttachShader(ctx->gl.prog, ctx->gl.frag);
   glAttachShader(ctx->gl.prog, ctx->gl.vert);

   ctx->gl.locPos= 0;
   ctx->gl.locTC= 1;
   glBindAttribLocation(ctx->gl.prog, ctx->gl.locPos, "pos");
   glBindAttribLocation(ctx->gl.prog, ctx->gl.locTC, "texcoord");
   if ( ctx->gl.haveYUVShaders )
   {
      ctx->gl.locTCUV= 2;
      glBindAttribLocation(ctx->gl.prog, ctx->gl.locTCUV, "texcoorduv");
   }

   glLinkProgram(ctx->gl.prog);
   glGetProgramiv(ctx->gl.prog, GL_LINK_STATUS, &status);
   if (!status)
   {
      glGetProgramInfoLog(ctx->gl.prog, sizeof(infoLog), &length, infoLog);
      printf("Error: initGL: linking:\n%*s\n", length, infoLog);
      goto exit;
   }

   ctx->gl.locRes= glGetUniformLocation(ctx->gl.prog,"u_resolution");
   ctx->gl.locMatrix= glGetUniformLocation(ctx->gl.prog,"u_matrix");
   ctx->gl.locTexture= glGetUniformLocation(ctx->gl.prog,"texture");
   if ( ctx->gl.haveYUVShaders )
   {
      ctx->gl.locTextureUV= glGetUniformLocation(ctx->gl.prog,"textureuv");
   }

   result= true;

exit:

   return result;
}

static void termGL( WaylandCtx *ctx )
{
   if ( ctx->gl.frag )
   {
      glDeleteShader( ctx->gl.frag );
      ctx->gl.frag= 0;
   }
   if ( ctx->gl.vert )
   {
      glDeleteShader( ctx->gl.vert );
      ctx->gl.vert= 0;
   }
   if ( ctx->gl.prog )
   {
      glDeleteProgram( ctx->gl.prog );
      ctx->gl.prog= 0;
   }
}

void drawGL( EGLCtx *eglCtx, Surface *surface )
{
   int x, y, w, h;
   GLenum glerr;
   WaylandCtx *ctx= surface->ctx;
   AppCtx *appCtx= ctx->appCtx;

   x= 0;
   y= 0;
   w= appCtx->windowWidth;
   h= appCtx->windowHeight;
 
   const float verts[4][2]=
   {
      { float(x), float(y) },
      { float(x+w), float(y) },
      { float(x),  float(y+h) },
      { float(x+w), float(y+h) }
   };
 
   const float uv[4][2]=
   {
      { 0,  0 },
      { 1,  0 },
      { 0,  1 },
      { 1,  1 }
   };

   const float identityMatrix[4][4]=
   {
      {1, 0, 0, 0},
      {0, 1, 0, 0},
      {0, 0, 1, 0},
      {0, 0, 0, 1}
   };

   if ( ctx->gl.haveYUVShaders != ctx->gl.haveYUVTextures )
   {
      termGL( ctx );
      ctx->gl.haveYUVShaders= ctx->gl.haveYUVTextures;
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
            appCtx->glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, surface->eglImage[i]);
         }
         glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
         glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
         glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
         glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
      }
   }

   glUseProgram(ctx->gl.prog);
   glUniform2f(ctx->gl.locRes, appCtx->windowWidth, appCtx->windowHeight);
   glUniformMatrix4fv(ctx->gl.locMatrix, 1, GL_FALSE, (GLfloat*)identityMatrix);

   glActiveTexture(GL_TEXTURE0); 
   glBindTexture(GL_TEXTURE_2D, surface->textureId[0]);
   glUniform1i(ctx->gl.locTexture, 0);
   glVertexAttribPointer(ctx->gl.locPos, 2, GL_FLOAT, GL_FALSE, 0, verts);
   glVertexAttribPointer(ctx->gl.locTC, 2, GL_FLOAT, GL_FALSE, 0, uv);
   glEnableVertexAttribArray(ctx->gl.locPos);
   glEnableVertexAttribArray(ctx->gl.locTC);
   if ( ctx->gl.haveYUVTextures )
   {
      glActiveTexture(GL_TEXTURE1); 
      glBindTexture(GL_TEXTURE_2D, surface->textureId[1]);
      glUniform1i(ctx->gl.locTexture, 1);
      glVertexAttribPointer(ctx->gl.locTCUV, 2, GL_FLOAT, GL_FALSE, 0, uv);
      glEnableVertexAttribArray(ctx->gl.locTCUV);
   }
   glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
   glDisableVertexAttribArray(ctx->gl.locPos);
   glDisableVertexAttribArray(ctx->gl.locTC);
   if ( ctx->gl.haveYUVTextures )
   {
      glDisableVertexAttribArray(ctx->gl.locTCUV);
   }
   
   glerr= glGetError();
   if ( glerr != GL_NO_ERROR )
   {
      printf("Warning: drawGL: glGetError: %X\n", glerr);
   }

   eglSwapBuffers( eglCtx->eglDisplay, eglCtx->eglSurface );
}

static void surfaceDestroy(struct wl_client *client, struct wl_resource *resource)
{
   Surface *surface= (Surface*)wl_resource_get_user_data(resource);

   if ( surface->surfaceNested )
   {
      wl_surface_destroy( surface->surfaceNested );
      wl_display_flush( surface->ctx->upstreamDisplay );
      surface->surfaceNested= 0;
   }
   // terminate display to end test
   wl_display_terminate( wl_client_get_display(client) );
   wl_resource_destroy(resource);
}

static void surfaceAttach(struct wl_client *client,
                          struct wl_resource *resource,
                          struct wl_resource *bufferResource, int32_t sx, int32_t sy)
{
   Surface *surface= (Surface*)wl_resource_get_user_data(resource);

   pthread_mutex_lock( &surface->ctx->mutex );
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
   pthread_mutex_unlock( &surface->ctx->mutex );
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
      surface->ctx->rescb= rescb;
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

static void buffer_release( void *data, struct wl_buffer *buffer )
{
   NestedBufferInfo *buffInfo= (NestedBufferInfo*)data;

   wl_buffer_destroy( buffer );

   if ( buffInfo )
   {
      if ( buffInfo->bufferRemote )
      {
         pthread_mutex_lock( &buffInfo->ctx->buffersToReleaseMutex );
         buffInfo->ctx->buffersToRelease.push_back( *buffInfo );
         pthread_mutex_unlock( &buffInfo->ctx->buffersToReleaseMutex );
      }
      free( buffInfo );
   }
}

static struct wl_buffer_listener wl_buffer_listener=
{
   buffer_release
};

static void surfaceCommit(struct wl_client *client, struct wl_resource *resource)
{
   Surface *surface= (Surface*)wl_resource_get_user_data(resource);
   struct wl_resource *committedBufferResource;
   WaylandCtx *ctx= surface->ctx;
   AppCtx *appCtx= ctx->appCtx;

   pthread_mutex_lock( &ctx->mutex );

   committedBufferResource= surface->attachedBufferResource;
   if ( committedBufferResource )
   {
      if ( ctx->isRepeater )
      {
         struct wl_buffer *clone;
         int bufferWidth, bufferHeight;

         clone= appCtx->remoteCloneBufferFromResource( appCtx->nested.dispWayland,
                                                       committedBufferResource,
                                                       appCtx->nested.upstreamDisplay,
                                                       &bufferWidth,
                                                       &bufferHeight );
         if ( clone )
         {
            NestedBufferInfo *buffInfo= (NestedBufferInfo*)malloc( sizeof(NestedBufferInfo) );
            if ( buffInfo )
            {
               buffInfo->ctx= ctx;
               buffInfo->surface= surface->surfaceNested;
               buffInfo->bufferRemote= committedBufferResource;
               wl_buffer_add_listener( clone, &wl_buffer_listener, buffInfo );
            }

            wl_surface_attach( surface->surfaceNested, clone, 0, 0 );
            wl_surface_damage( surface->surfaceNested, 0, 0, bufferWidth, bufferHeight);
            wl_surface_commit( surface->surfaceNested );
            wl_display_flush( appCtx->nested.upstreamDisplay );

            wl_list_remove(&surface->attachedBufferDestroyListener.link);
            surface->attachedBufferResource= 0;
         }
      }
      else
      if ( appCtx->renderWayland )
      {
         EGLImageKHR eglImage= 0;
         EGLint value, format;
         EGLint attrList[3];
         int bufferWidth= 0, bufferHeight= 0;

         if (EGL_TRUE == appCtx->eglQueryWaylandBufferWL( ctx->eglServer.eglDisplay, committedBufferResource,
                                                          EGL_WIDTH, &value ) )
         {
            bufferWidth= value;
         }

         if (EGL_TRUE == appCtx->eglQueryWaylandBufferWL( ctx->eglServer.eglDisplay, committedBufferResource,
                                                          EGL_HEIGHT, &value ) )
         {
            bufferHeight= value;
         }                                                        

         if (EGL_TRUE == appCtx->eglQueryWaylandBufferWL( ctx->eglServer.eglDisplay, committedBufferResource,
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
               appCtx->eglDestroyImageKHR( ctx->eglServer.eglDisplay, surface->eglImage[i] );
               surface->eglImage[i]= 0;
            }
         }

         switch ( format )
         {
            case EGL_TEXTURE_RGB:
            case EGL_TEXTURE_RGBA:
               eglImage= appCtx->eglCreateImageKHR( ctx->eglServer.eglDisplay, EGL_NO_CONTEXT,
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
               ctx->gl.haveYUVTextures= false;
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
                  
                  eglImage= appCtx->eglCreateImageKHR( ctx->eglServer.eglDisplay, EGL_NO_CONTEXT,
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
               ctx->gl.haveYUVTextures= true;
               break;
               
            case EGL_TEXTURE_Y_XUXV_WL:
               printf("Error: surfaceCommit: EGL_TEXTURE_Y_XUXV_WL not supported\n" );
               break;
               
            default:
               printf("Error: surfaceCommit:: unknown texture format: %x\n", format );
               break;
         }

         drawGL( &ctx->eglServer, surface );
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

#if ( (WAYLAND_VERSION_MAJOR >= 1) && (WAYLAND_VERSION_MINOR >= 18) )
static void surfaceDamageBuffer(struct wl_client *, struct wl_resource *, int32_t, int32_t, int32_t, int32_t)
{
   // ignore
}
#endif

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
   surfaceSetBufferScale,
   #if ( (WAYLAND_VERSION_MAJOR >= 1) && (WAYLAND_VERSION_MINOR >= 18) )
   surfaceDamageBuffer
   #endif
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

   WaylandCtx *ctx= surface->ctx;

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
   WaylandCtx *ctx= (WaylandCtx*)wl_resource_get_user_data(resource);
   Surface *surface;
   
   pthread_mutex_lock( &ctx->mutex );

   surface= (Surface*)calloc( 1, sizeof(Surface) );   
   if (!surface) 
   {
      wl_resource_post_no_memory(resource);
      pthread_mutex_unlock( &ctx->mutex );
      return;
   }

   surface->ctx= ctx;
   surface->refCount= 1;
   surface->attachedBufferDestroyListener.notify= attachedBufferDestroyCallback;
   surface->detachedBufferDestroyListener.notify= detachedBufferDestroyCallback;

   surface->resource= wl_resource_create(client, &wl_surface_interface, MIN(3,wl_resource_get_version(resource)), id);
   if (!surface->resource)
   {
      free(surface);
      wl_resource_post_no_memory(resource);
      pthread_mutex_unlock( &ctx->mutex );
      return;
   }

   wl_resource_set_implementation(surface->resource, &surface_interface, surface, destroySurfaceCallback);

   if ( ctx->isRepeater )
   {
      surface->surfaceNested= wl_compositor_create_surface(ctx->compositor);
      wl_display_flush( ctx->upstreamDisplay );
      if ( !surface->surfaceNested )
      {
         free(surface);
         wl_resource_post_no_memory(resource);
         pthread_mutex_unlock( &ctx->mutex );
         return;
      }
   }

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
   WaylandCtx *ctx= (WaylandCtx*)data;
   struct wl_resource *resource;

   resource= wl_resource_create(client, &wl_compositor_interface, MIN(3,version), id);
   if (!resource)
   {
      wl_client_post_no_memory(client);
   }
   else
   {
      wl_resource_set_implementation(resource, &compositor_interface, ctx, 0);
   }
}

static bool initWayland( WaylandCtx *ctx, const char *displayName )
{
   bool result= false;
   AppCtx *appCtx= ctx->appCtx;

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

   if ( wl_display_add_socket( ctx->dispWayland, displayName ) )
   {
      printf("Error: initWayland: failed to add socket\n");
      goto exit;
   }

   if ( !appCtx->eglBindWaylandDisplayWL( ctx->eglServer.eglDisplay, ctx->dispWayland ) )
   {
      printf("Error: initWayland: failed to bind EGL to wayland display\n");
      goto exit;
   }
   ctx->eglServer.displayBound= true;

   result= true;

exit:
   return result;
}

static void termWayland( WaylandCtx *ctx )
{
   AppCtx *appCtx= ctx->appCtx;
   if ( ctx->dispWayland )
   {
      if ( ctx->eglServer.displayBound )
      {
         appCtx->eglUnbindWaylandDisplayWL( ctx->eglServer.eglDisplay, ctx->dispWayland );
         ctx->eglServer.displayBound= false;
      }

      wl_display_destroy(ctx->dispWayland);
      ctx->dispWayland= 0;
   }
}

namespace waylandClient
{
static void registryAdd(void *data,
                        struct wl_registry *registry, uint32_t id,
                        const char *interface, uint32_t version)
{
   WaylandCtx *ctx = (WaylandCtx*)data;
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
} // namespace waylandClient

static void waylandClientRole( AppCtx *ctx )
{
   using namespace waylandClient;

   struct wl_display *dispWayland= 0;
   struct wl_registry *registry= 0;
   long long time1, time2, diff;
   GLfloat r, g, b, t;
   int rc, pacingInc, step, maxStep;
   const char *s;
   

   usleep(100000);

   dispWayland= wl_display_connect( ctx->client.upstreamDisplayName );
   if ( !dispWayland )
   {
      printf("Error: roleWaylandClient: failed to connect to display\n");
      goto exit;
   }
   ctx->client.upstreamDisplay= dispWayland;

   registry= wl_display_get_registry(dispWayland);
   if ( !registry )
   {
      printf("Error: roleWaylandClient: failed tp get wayland registry\n");
      goto exit;
   }

   wl_registry_add_listener(registry, &registryListener, &ctx->client);

   wl_display_roundtrip( dispWayland );

   if ( !ctx->client.compositor )
   {
      printf("Error: roleWaylandClient: failed to obtain wayland compositor\n");
      goto exit;
   }

   ctx->client.eglClient.useWayland= true;
   ctx->client.eglClient.dispWayland= dispWayland;
   if ( !initEGL( &ctx->client.eglClient ) )
   {
      printf("Error: roleWaylandClient: failed to setup EGL\n");
      goto exit;
   }

   s= eglQueryString( ctx->client.eglClient.eglDisplay, EGL_VENDOR );
   if ( s )
   {
      ctx->eglVendor= strdup(s);
   }

   ctx->client.surface= wl_compositor_create_surface(ctx->client.compositor);
   if ( !ctx->client.surface )
   {
      printf("Error: roleWaylandClient: failed to create wayland surface\n");
      goto exit;
   }

   ctx->client.winWayland= wl_egl_window_create(ctx->client.surface, ctx->windowWidth, ctx->windowHeight);
   if ( !ctx->client.winWayland )
   {
      printf("Error: roleWaylandClient: failed to create wayland window\n");
      goto exit;
   }

   ctx->client.eglClient.eglSurface= eglCreateWindowSurface( ctx->client.eglClient.eglDisplay,
                                                      ctx->client.eglClient.eglConfig,
                                                      (EGLNativeWindowType)ctx->client.winWayland,
                                                      NULL );
   if ( ctx->client.eglClient.eglSurface == EGL_NO_SURFACE )
   {
      printf("Error: roleWaylandClient: failed to create EGL surface\n");
      goto exit;
   }

   eglMakeCurrent( ctx->client.eglClient.eglDisplay, ctx->client.eglClient.eglSurface,
                   ctx->client.eglClient.eglSurface, ctx->client.eglClient.eglContext );

   eglSwapInterval( ctx->client.eglClient.eglDisplay, 1 );

   glClearColor( 0, 0, 0, 1 );
   glClear( GL_COLOR_BUFFER_BIT );
   eglSwapBuffers( ctx->client.eglClient.eglDisplay, ctx->client.eglClient.eglSurface );
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
         eglSwapBuffers( ctx->client.eglClient.eglDisplay, ctx->client.eglClient.eglSurface );
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
   eglSwapBuffers( ctx->client.eglClient.eglDisplay, ctx->client.eglClient.eglSurface );
   usleep( 1500000 );

exit:
   writeResult( ctx->waylandTotal );

   if ( ctx->client.surface )
   {
      wl_surface_destroy( ctx->client.surface );
      ctx->client.surface= 0;
   }

   if ( ctx->client.compositor )
   {
      wl_compositor_destroy( ctx->client.compositor );
      ctx->client.compositor= 0;
   }

   //TODO: why does this crash on some devices?
   if ( strcmp( ctx->eglVendor, "ARM" ) !=  0 )
   {
      termEGL( &ctx->client.eglClient );
   }

   if ( ctx->client.winWayland )
   {
      wl_egl_window_destroy( ctx->client.winWayland );
      ctx->client.winWayland= 0;
   }

   if ( registry )
   {
      wl_registry_destroy(registry);
      registry= 0;
   }

   if ( ctx->client.upstreamDisplayName == ctx->nestedDisplayName )
   {
      if ( ctx->nested.dispWayland )
      {
         wl_display_terminate( ctx->nested.dispWayland );
      }
   }
   else
   {
      if ( ctx->master.dispWayland )
      {
         wl_display_terminate( ctx->master.dispWayland );
      }
   }

   if ( dispWayland )
   {
      wl_display_flush( dispWayland );
      usleep(100000);
      wl_display_disconnect( dispWayland );
      dispWayland= 0;
   }
}

static void* waylandClientThread( void *arg )
{
   using namespace waylandClient;

   AppCtx *ctx= (AppCtx*)arg;
   char work[256];

   fflush( ctx->pReport );

   sprintf(work,"waymetric --iterations %d", ctx->maxIterations );
   if ( ctx->client.upstreamDisplayName == ctx->nestedDisplayName )
   {
      strcat( work, " --role-wayland-client-nested" );
   }
   else
   {
      strcat( work, " --role-wayland-client" );
   }
   if ( !ctx->renderWayland )
   {
      strcat( work, " --no-wayland-render" );
   }
   system(work);

   fseek( ctx->pReport, 0LL, SEEK_END );

   return NULL;
}

namespace waylandNested
{
static void registryAdd(void *data,
                        struct wl_registry *registry, uint32_t id,
                        const char *interface, uint32_t version)
{
   WaylandCtx *ctx = (WaylandCtx*)data;
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
} // namespace waylandNested

static void* waylandNestedDispatchThread( void *arg )
{
   AppCtx *ctx= (AppCtx*)arg;
   if ( ctx )
   {
      for( ; ; )
      {
         if ( wl_display_dispatch( ctx->nested.upstreamDisplay ) == -1 )
         {
            break;
         }
         wl_display_flush( ctx->nested.upstreamDisplay );
      }
   }
   return NULL;
}

static int nestedDisplayTimeOut( void *data )
{
   AppCtx *ctx= (AppCtx*)data;
   long long frameTime, now;
   int nextFrameDelay;

   frameTime= getCurrentTimeMillis();

   pthread_mutex_lock( &ctx->nested.buffersToReleaseMutex );
   while( ctx->nested.buffersToRelease.size() )
   {
      std::vector<NestedBufferInfo>::iterator it= ctx->nested.buffersToRelease.begin();
      struct wl_resource *bufferResource= (*it).bufferRemote;
      wl_buffer_send_release( bufferResource );
      ctx->nested.buffersToRelease.erase(it);
   }
   pthread_mutex_unlock( &ctx->nested.buffersToReleaseMutex );

   now= getCurrentTimeMillis();

   nextFrameDelay= (FRAME_PERIOD_MILLIS_60FPS-(now-frameTime));
   if ( nextFrameDelay < 1 ) nextFrameDelay= 1;

   wl_event_source_timer_update( ctx->nested.displayTimer, nextFrameDelay );

   return 0;
}

static void waylandNestedRole( AppCtx *ctx )
{
   using namespace waylandNested;

   struct wl_display *dispWayland= 0;
   struct wl_registry *registry= 0;
   int rc;

   dispWayland= wl_display_connect( ctx->displayName );
   printf("waylandNestedRole: dispWayland %p from name %s\n", dispWayland, ctx->displayName);
   if ( !dispWayland )
   {
      printf("Error: waylandNestedRole: failed to connect to display\n");
      goto exit;
   }
   ctx->nested.upstreamDisplay= dispWayland;

   registry= wl_display_get_registry(dispWayland);
   if ( !registry )
   {
      printf("Error: waylandNestedRole: failed tp get wayland registry\n");
      goto exit;
   }

   wl_registry_add_listener(registry, &registryListener, &ctx->nested);

   wl_display_roundtrip( dispWayland );

   if ( !ctx->nested.compositor )
   {
      printf("Error: waylandNestedRole: failed to obtain wayland compositor\n");
      goto exit;
   }

   ctx->nested.eglServer.useWayland= true;
   ctx->nested.eglServer.dispWayland= dispWayland;
   if ( !initEGL( &ctx->nested.eglServer ) )
   {
      printf("Error: waylandNestedRole: failed to setup EGL\n");
      goto exit;
   }

   ctx->nested.surface= wl_compositor_create_surface(ctx->nested.compositor);
   if ( !ctx->nested.surface )
   {
      printf("Error: waylandNestedRole: failed to create wayland surface\n");
      goto exit;
   }

   ctx->nested.winWayland= wl_egl_window_create(ctx->nested.surface, ctx->windowWidth, ctx->windowHeight);
   if ( !ctx->nested.winWayland )
   {
      printf("Error: waylandNestedRole: failed to create wayland window\n");
      goto exit;
   }

   ctx->nested.eglServer.eglSurface= eglCreateWindowSurface( ctx->nested.eglServer.eglDisplay,
                                                      ctx->nested.eglServer.eglConfig,
                                                      (EGLNativeWindowType)ctx->nested.winWayland,
                                                      NULL );
   if ( ctx->nested.eglServer.eglSurface == EGL_NO_SURFACE )
   {
      printf("Error: waylandNestedRole: failed to create EGL surface\n");
      goto exit;
   }

   eglMakeCurrent( ctx->nested.eglServer.eglDisplay, ctx->nested.eglServer.eglSurface,
                   ctx->nested.eglServer.eglSurface, ctx->nested.eglServer.eglContext );

   eglSwapInterval( ctx->nested.eglServer.eglDisplay, 1 );

   if ( !initGL( &ctx->nested ) )
   {
      printf("Error: waylandNestedRole: initGL failed\n");
   }

   if ( !initWayland( &ctx->nested, ctx->nestedDisplayName ) )
   {
      printf("Error: waylandNestedRole: initWayland failed\n");
      goto exit;
   }

   if ( ctx->canRemoteClone )
   {
      if ( !ctx->remoteBegin( ctx->nested.dispWayland, dispWayland ) )
      {
         printf("Error: waylandNestedRole: remoteBegin failure\n");
         ctx->canRemoteClone= false;
         goto exit;
      }
      pthread_mutex_init( &ctx->nested.buffersToReleaseMutex, 0 );
      ctx->nested.buffersToRelease= std::vector<NestedBufferInfo>();
      ctx->nested.displayTimer= wl_event_loop_add_timer( wl_display_get_event_loop(ctx->nested.dispWayland), nestedDisplayTimeOut, ctx );
      wl_event_source_timer_update( ctx->nested.displayTimer, FRAME_PERIOD_MILLIS_60FPS );
      rc= pthread_create( &ctx->nestedDispatchThreadId, NULL, waylandNestedDispatchThread, ctx );
      if ( rc )
      {
         printf("Error: waylandNestedRole: failed to start nested dispatch thread\n");
      }
      ctx->nested.isRepeater= true;
   }

   ctx->client.upstreamDisplayName= ctx->nestedDisplayName;
   rc= pthread_create( &ctx->clientThreadId, NULL, waylandClientThread, ctx );
   if ( !rc )
   {
      wl_display_run( ctx->nested.dispWayland );

      pthread_join( ctx->clientThreadId, NULL );
   }

exit:
   eglMakeCurrent( ctx->nested.eglServer.eglDisplay, EGL_NO_CONTEXT, EGL_NO_SURFACE, EGL_NO_SURFACE );

   if ( ctx->nested.eglServer.eglSurface != EGL_NO_SURFACE )
   {
      termGL( &ctx->nested );

      eglDestroySurface( ctx->nested.eglServer.eglDisplay, ctx->nested.eglServer.eglSurface );
      ctx->nested.eglServer.eglSurface= EGL_NO_SURFACE;
   }

   if ( ctx->canRemoteClone )
   {
      ctx->remoteEnd( ctx->nested.dispWayland, dispWayland );
   }

   termWayland( &ctx->nested );

   if ( ctx->nested.surface )
   {
      wl_surface_destroy( ctx->nested.surface );
      ctx->nested.surface= 0;
   }

   if ( ctx->nested.compositor )
   {
      wl_compositor_destroy( ctx->nested.compositor );
      ctx->nested.compositor= 0;
   }

   if ( ctx->nested.winWayland )
   {
      wl_egl_window_destroy( ctx->nested.winWayland );
      ctx->nested.winWayland= 0;
   }

   if ( registry )
   {
      wl_registry_destroy(registry);
      registry= 0;
   }

   if ( dispWayland )
   {
      wl_display_flush( dispWayland );
      usleep(100000);
      wl_display_disconnect( dispWayland );
      dispWayland= 0;
   }
}

static void* waylandNestedThread( void *arg )
{
   using namespace waylandNested;

   AppCtx *ctx= (AppCtx*)arg;
   char work[256];

   fflush( ctx->pReport );

   sprintf(work,"waymetric --role-wayland-nested --iterations %d", ctx->maxIterations );
   if ( !ctx->renderWayland )
   {
      strcat( work, " --no-wayland-render" );
   }
   system(work);

   fseek( ctx->pReport, 0LL, SEEK_END );

   return NULL;
}

static void measureWaylandEGL( AppCtx *ctx, EGLCtx *eglCtx )
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

            if ( !initGL( &ctx->master ) )
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

   ctx->client.upstreamDisplayName= ctx->displayName;
   rc= pthread_create( &ctx->clientThreadId, NULL, waylandClientThread, ctx );
   if ( !rc )
   {
      wl_display_run( ctx->master.dispWayland );

      pthread_join( ctx->clientThreadId, NULL );
      ctx->waylandTotal= readResult();
   }

   if ( ctx->renderWayland )
   {
      eglMakeCurrent( eglCtx->eglDisplay, EGL_NO_CONTEXT, EGL_NO_SURFACE, EGL_NO_SURFACE );

      if ( eglCtx->eglSurface != EGL_NO_SURFACE )
      {
         termGL( &ctx->master );

         eglDestroySurface( eglCtx->eglDisplay, eglCtx->eglSurface );
         eglCtx->eglSurface= EGL_NO_SURFACE;
      }

      if ( nativeWindow )
      {
         PlatformDestroyNativeWindow( ctx->platformCtx, nativeWindow );
      }
   }
}

static void measureDirectEGL( AppCtx *ctx, EGLCtx *eglCtx )
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

static void measureWaylandNested( AppCtx *ctx, EGLCtx *eglCtx )
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

            if ( !initGL( &ctx->master ) )
            {
               printf("Error: measureWaylandNested: initGL failed\n");
            }
         }
         else
         {
            printf("Error: measureWaylandNested: failed to create EGL surface\n");
         }
      }
      else
      {
         printf("Error: measureWaylandNested: failed to create native window\n");
      }
   }

   rc= pthread_create( &ctx->nestedThreadId, NULL, waylandNestedThread, ctx );
   if ( !rc )
   {
      wl_display_run( ctx->master.dispWayland );

      pthread_join( ctx->nestedThreadId, NULL );
      ctx->waylandTotal= readResult();
   }

   if ( ctx->renderWayland )
   {
      eglMakeCurrent( eglCtx->eglDisplay, EGL_NO_CONTEXT, EGL_NO_SURFACE, EGL_NO_SURFACE );

      if ( eglCtx->eglSurface != EGL_NO_SURFACE )
      {
         termGL( &ctx->master );

         eglDestroySurface( eglCtx->eglDisplay, eglCtx->eglSurface );
         eglCtx->eglSurface= EGL_NO_SURFACE;
      }

      if ( nativeWindow )
      {
         PlatformDestroyNativeWindow( ctx->platformCtx, nativeWindow );
      }
   }
}

static void* waylandMultiThread( void *arg )
{
   MultiComp *comp= (MultiComp*)arg;
   AppCtx *ctx= comp->appCtx;
   bool result;

   comp->started= true;

   printf("multi %d thread started\n", comp->index);

   comp->ctx.appCtx= ctx;
   comp->ctx.eglServer.appCtx= ctx;
   comp->ctx.eglServer.useWayland= false;
   comp->ctx.eglServer.nativeDisplay= PlatformGetEGLDisplayType( ctx->platformCtx );
   if ( !initEGL( &comp->ctx.eglServer ) )
   {
      printf("Error: waylandMultiThread %d failed to setup EGL\n", comp->index);
      comp->error= true;
   }

   pthread_mutex_lock( &comp->mutexReady );
   pthread_mutex_lock( &comp->mutexStart );
   printf("multi %d thread signal ready\n", comp->index);
   pthread_cond_signal( &comp->condReady );
   pthread_mutex_unlock( &comp->mutexReady );

   printf("multi %d thread wait start\n", comp->index);
   pthread_cond_wait( &comp->condStart, &comp->mutexStart );
   pthread_mutex_unlock( &comp->mutexStart );

   if ( !comp->error )
   {
      result= initWayland( &comp->ctx, (const char*)comp->displayName );
      if ( result )
      {
         printf("multi %d display created and bound\n", comp->index);
         comp->init= true;
      }
      else
      {
         comp->error= true;
      }
   }

   pthread_mutex_lock( &comp->mutexReady );
   pthread_mutex_lock( &comp->mutexStart );
   printf("multi %d thread signal ready\n", comp->index);
   pthread_cond_signal( &comp->condReady );
   pthread_mutex_unlock( &comp->mutexReady );

   printf("multi %d thread wait start\n", comp->index);
   pthread_cond_wait( &comp->condStart, &comp->mutexStart );
   pthread_mutex_unlock( &comp->mutexStart );

   termWayland( &comp->ctx );
   printf("multi %d display unbound and destroyed\n", comp->index);
   comp->term= true;

   termEGL( &comp->ctx.eglServer );

   pthread_mutex_lock( &comp->mutexReady );
   printf("multi %d thread signal ready\n", comp->index);
   pthread_cond_signal( &comp->condReady );
   pthread_mutex_unlock( &comp->mutexReady );

   return NULL;
}

#define NUM_COMP (4)
static void testMultipleCompositorsPerProcess( AppCtx *ctx )
{
   MultiComp comp[NUM_COMP];
   int i, rc, multiCount;

   for( i= 0; i < NUM_COMP; ++i )
   {
      memset( &comp[i], 0, sizeof(MultiComp) );

      comp[i].appCtx= ctx;
      comp[i].index= i;
      pthread_mutex_init( &comp[i].mutexReady, 0 );
      pthread_cond_init( &comp[i].condReady, 0 );
      pthread_mutex_init( &comp[i].mutexStart, 0 );
      pthread_cond_init( &comp[i].condStart, 0 );

      sprintf( comp[i].displayName, "waymetric-multi%d", i );

      pthread_mutex_lock( &comp[i].mutexReady );
      rc= pthread_create( &comp[i].threadId, NULL, waylandMultiThread, &comp[i] );
      if ( !rc )
      {
         printf("control wait for %d ready\n", i);
         pthread_cond_wait( &comp[i].condReady, &comp[i].mutexReady );
      }
      else
      {
         printf("Error: testMultipleCompositorsPerProcess: failed to start test thread for instance %d\n", i);
      }
      pthread_mutex_unlock( &comp[i].mutexReady );
   }

   for( i= 0; i < NUM_COMP; ++i )
   {
      if ( comp[i].started )
      {
         // signal to create and bind display
         pthread_mutex_lock( &comp[i].mutexStart );
         printf("control signal for %d start\n", i);
         pthread_cond_signal( &comp[i].condStart );
         pthread_mutex_unlock( &comp[i].mutexStart );

         // wait till display creation confirmed
         pthread_mutex_lock( &comp[i].mutexReady );
         printf("control wait for %d ready\n", i);
         pthread_cond_wait( &comp[i].condReady, &comp[i].mutexReady );
         pthread_mutex_unlock( &comp[i].mutexReady );
      }
   }

   for( i= 0; i < NUM_COMP; ++i )
   {
      if ( comp[i].started )
      {
         // signal to unbind and destroy display
         pthread_mutex_lock( &comp[i].mutexStart );
         printf("control signal for %d start\n", i);
         pthread_cond_signal( &comp[i].condStart );
         pthread_mutex_unlock( &comp[i].mutexStart );

         // wait till display destruction confirmed
         pthread_mutex_lock( &comp[i].mutexReady );
         printf("control wait for %d ready\n", i);
         pthread_cond_wait( &comp[i].condReady, &comp[i].mutexReady );
         pthread_mutex_unlock( &comp[i].mutexReady );
      }
   }

   multiCount= 0;
   for( i= 0; i < NUM_COMP; ++i )
   {
      if ( comp[i].started &&
           comp[i].init &&
           comp[i].term &&
           !comp[i].error )
      {
         multiCount += 1;
      }
   }
   fprintf(ctx->pReport, "Successful multiple compositor instances: %d out of %d\n", multiCount, NUM_COMP);
}

static void checkForRepeaterSupport( AppCtx *ctx )
{
   void *module= 0;

   module= dlopen( "libwayland-egl.so.0", RTLD_NOW );
   if ( !module )
   {
      module= dlopen( "libwayland-egl.so.1", RTLD_NOW );
      if ( !module )
      {
         module= dlopen( "libwayland-egl.so", RTLD_NOW );
      }
   }
   if ( module )
   {
      ctx->remoteBegin= (PFNREMOTEBEGIN)dlsym( module, "wl_egl_remote_begin" );
      ctx->remoteEnd= (PFNREMOTEEND)dlsym( module, "wl_egl_remote_end" );
      ctx->remoteCloneBufferFromResource= (PFNREMOTECLONEBUFFERFROMRESOURCE)dlsym( module, "wl_egl_remote_buffer_clone" );

      if ( (ctx->remoteBegin != 0) &&
           (ctx->remoteEnd != 0) &&
           (ctx->remoteCloneBufferFromResource != 0) )
      {
         ctx->canRemoteClone= true;
      }

      dlclose( module );
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
   printf("--no-multi\n");
   printf("--no-normal\n");
   printf("--no-nested\n");
   printf("--no-repeater\n");
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
   bool noNormal= false;
   bool noMulti= false;
   bool noNested= false;
   bool noRepeater= false;
   bool noWaylandRender= false;
   bool roleWaylandClient= false;
   bool roleWaylandClientNested= false;
   bool roleWaylandNested= false;
   const char *reportFilename= 0;
   int pacingInc, step, maxStep;
   long long directTotal, waylandTotal;

   printf("waymetric v%s\n", WAYMETRIC_VERSION);

   ctx= (AppCtx*)calloc( 1, sizeof(AppCtx) );
   if ( !ctx )
   {
      printf("Error: unable allocate AppCtx\n");
      goto exit;
   }
   pthread_mutex_init( &ctx->mutex, 0 );
   pthread_mutex_init( &ctx->master.mutex, 0 );
   pthread_mutex_init( &ctx->master.mutexReady, 0 );
   pthread_cond_init( &ctx->master.condReady, 0 );
   pthread_mutex_init( &ctx->nested.mutex, 0 );
   pthread_mutex_init( &ctx->nested.mutexReady, 0 );
   pthread_cond_init( &ctx->nested.condReady, 0 );
   pthread_mutex_init( &ctx->client.mutex, 0 );
   pthread_mutex_init( &ctx->client.mutexReady, 0 );
   pthread_cond_init( &ctx->client.condReady, 0 );
   ctx->displayName= "waymetric0";
   ctx->nestedDisplayName= "waymetric-nested0";
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
         else if ( (len == 11) && !strncmp( argv[argidx], "--no-normal", len) )
         {
            noNormal= true;
         }
         else if ( (len == 10) && !strncmp( argv[argidx], "--no-multi", len) )
         {
            noMulti= true;
         }
         else if ( (len == 11) && !strncmp( argv[argidx], "--no-nested", len) )
         {
            noNested= true;
         }
         else if ( (len == 13) && !strncmp( argv[argidx], "--no-repeater", len) )
         {
            noRepeater= true;
         }
         else if ( (len == 19) && !strncmp( argv[argidx], "--no-wayland-render", len) )
         {
            noWaylandRender= true;
         }
         else if ( (len == 21) && !strncmp( argv[argidx], "--role-wayland-client", len) )
         {
            roleWaylandClient= true;
         }
         else if ( (len == 28) && !strncmp( argv[argidx], "--role-wayland-client-nested", len) )
         {
            roleWaylandClient= true;
            roleWaylandClientNested= true;
         }
         else if ( (len == 21) && !strncmp( argv[argidx], "--role-wayland-nested", len) )
         {
            roleWaylandNested= true;
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

   setenv( "XDG_RUNTIME_DIR", "/tmp", true );

   ctx->master.appCtx= ctx;
   ctx->nested.appCtx= ctx;
   ctx->client.appCtx= ctx;
   ctx->master.eglServer.appCtx= ctx;
   ctx->nested.eglServer.appCtx= ctx;
   ctx->client.eglClient.appCtx= ctx;

   ctx->eglGetPlatformDisplayEXT= (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress( "eglGetPlatformDisplayEXT" );
   ctx->eglBindWaylandDisplayWL= (PFNEGLBINDWAYLANDDISPLAYWL)eglGetProcAddress("eglBindWaylandDisplayWL");
   ctx->eglUnbindWaylandDisplayWL= (PFNEGLUNBINDWAYLANDDISPLAYWL)eglGetProcAddress("eglUnbindWaylandDisplayWL");
   ctx->eglQueryWaylandBufferWL= (PFNEGLQUERYWAYLANDBUFFERWL)eglGetProcAddress("eglQueryWaylandBufferWL");
   ctx->eglCreateImageKHR= (PFNEGLCREATEIMAGEKHRPROC)eglGetProcAddress("eglCreateImageKHR");
   ctx->eglDestroyImageKHR= (PFNEGLDESTROYIMAGEKHRPROC)eglGetProcAddress("eglDestroyImageKHR");
   ctx->glEGLImageTargetTexture2DOES= (PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)eglGetProcAddress("glEGLImageTargetTexture2DOES");

   if ( roleWaylandClient )
   {
      ctx->pReport= fopen( reportFilename, "at");
      ctx->platformCtx= PlatfromInit();
      if ( !ctx->platformCtx )
      {
         printf("Error: PlatformInit failed\n");
      }
      if ( roleWaylandClientNested )
      {
         ctx->client.upstreamDisplayName= ctx->nestedDisplayName;
      }
      else
      {
         ctx->client.upstreamDisplayName= ctx->displayName;
      }
      waylandClientRole( ctx );
      nRC= 0;
      goto exit;
   }
   else if ( roleWaylandNested )
   {
      ctx->pReport= fopen( reportFilename, "at");
      ctx->platformCtx= PlatfromInit();
      if ( !ctx->platformCtx )
      {
         printf("Error: PlatformInit failed\n");
      }
      ctx->renderWayland= !noWaylandRender;
      ctx->client.upstreamDisplayName= ctx->displayName;
      waylandNestedRole( ctx );
      nRC= 0;
      goto exit;
   }

   ctx->pReport= fopen( reportFilename, "wt");
   
   ctx->platformCtx= PlatfromInit();
   if ( !ctx->platformCtx )
   {
      printf("Error: PlatformInit failed\n");
      goto exit;
   }

   ctx->master.eglServer.useWayland= false;
   ctx->master.eglServer.nativeDisplay= PlatformGetEGLDisplayType( ctx->platformCtx );
   if ( !initEGL( &ctx->master.eglServer ) )
   {
      printf("Error: failed to setup EGL\n");
      goto exit;
   }

   // Check for extensions
   s= eglQueryString( ctx->master.eglServer.eglDisplay, EGL_VENDOR );
   if ( s )
   {
      ctx->eglVendor= strdup(s);
   }
   s= eglQueryString( ctx->master.eglServer.eglDisplay, EGL_VERSION );
   if ( s )
   {
      ctx->eglVersion= strdup(s);
   }
   s= eglQueryString( ctx->master.eglServer.eglDisplay, EGL_CLIENT_APIS );
   if ( s )
   {
      ctx->eglClientAPIS= strdup(s);
   }
   s= eglQueryString( ctx->master.eglServer.eglDisplay, EGL_EXTENSIONS );
   if ( s )
   {
      ctx->eglExtensions= strdup(s);
   }

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

   fprintf(ctx->pReport, "waymetric v%s\n", WAYMETRIC_VERSION);
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

   if ( !noWayland && ctx->haveWaylandEGL && !noMulti )
   {
      fprintf(ctx->pReport, "\n");
      fprintf(ctx->pReport, "-----------------------------------------------------------------\n");
      fprintf(ctx->pReport, "Testing multiple compositor instances per process...\n");
      printf("\nTesting multiple compositor instances per process...\n");
      testMultipleCompositorsPerProcess( ctx );
      fprintf(ctx->pReport, "-----------------------------------------------------------------\n");

      // Tear down EGL and platform and re-init to have a clean start after multi-test
      termEGL( &ctx->master.eglServer );

      if ( ctx->platformCtx )
      {
         PlatformTerm( ctx->platformCtx );
         ctx->platformCtx= 0;
      }

      ctx->platformCtx= PlatfromInit();
      if ( !ctx->platformCtx )
      {
         printf("Error: WayMetPlatformInit failed\n");
         goto exit;
      }

      ctx->master.eglServer.useWayland= false;
      ctx->master.eglServer.nativeDisplay= PlatformGetEGLDisplayType( ctx->platformCtx );
      if ( !initEGL( &ctx->master.eglServer ) )
      {
         printf("Error: failed to setup EGL\n");
         goto exit;
      }
   }

   if ( !noWayland && ctx->haveWaylandEGL )
   { 
      if ( !initWayland( &ctx->master, ctx->displayName ) )
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
         measureDirectEGL( ctx, &ctx->master.eglServer );

         fprintf(ctx->pReport, "Iterations: %d Total time (us): %lld  FPS: %f\n", 
                 ctx->directEGLIterationCount, ctx->directEGLTimeTotal, ctx->directEGLFPS );

         fprintf(ctx->pReport, "-----------------------------------------------------------------\n");

         directTotal += ctx->directEGLTimeTotal;

         ctx->pacingDelay += pacingInc;
      }
   }

   if ( !noWayland && !noNormal && ctx->haveWaylandEGL )
   {
      fprintf(ctx->pReport, "\n");
      fprintf(ctx->pReport, "-----------------------------------------------------------------\n");
      fprintf(ctx->pReport, "Measuring Wayland...\n");
      printf("\nMeasuring Wayland...\n");

      ctx->renderWayland= !noWaylandRender;
      measureWaylandEGL( ctx, &ctx->master.eglServer );

      waylandTotal= ctx->waylandTotal;

      if ( waylandTotal == 0 )
      {
         fprintf(ctx->pReport, "Wayland failed\n");
         printf("\nWayland failed\n");
      }
      else
      if ( directTotal > 0 )
      {
         fprintf(ctx->pReport, "\n");
         fprintf(ctx->pReport, "=================================================================\n");
         fprintf(ctx->pReport, "waymetric speed index: %f\n", ((double)waylandTotal / (double)directTotal) );
         fprintf(ctx->pReport, "=================================================================\n");
      }
   }

   if ( !noWayland && !noNested && ctx->haveWaylandEGL )
   {
      fprintf(ctx->pReport, "\n");
      fprintf(ctx->pReport, "-----------------------------------------------------------------\n");
      fprintf(ctx->pReport, "Measuring Wayland Nested...\n");
      printf("\nMeasuring Wayland Nested...\n");

      ctx->waylandTotal= 0;
      ctx->renderWayland= !noWaylandRender;
      measureWaylandNested( ctx, &ctx->master.eglServer );

      waylandTotal= ctx->waylandTotal;

      if ( waylandTotal == 0 )
      {
         fprintf(ctx->pReport, "Wayland Nested failed\n");
         printf("\nWayland Nested failed\n");
      }
      else
      if ( directTotal > 0 )
      {
         fprintf(ctx->pReport, "\n");
         fprintf(ctx->pReport, "=================================================================\n");
         fprintf(ctx->pReport, "waymetric nested speed index: %f\n", ((double)waylandTotal / (double)directTotal) );
         fprintf(ctx->pReport, "=================================================================\n");
      }
   }

   if ( !noWayland && !noRepeater && ctx->haveWaylandEGL )
   {
      fprintf(ctx->pReport, "-----------------------------------------------------------------\n");
      fprintf(ctx->pReport, "Checking for repeater support...\n");
      printf("Checking for repeater support...\n");
      checkForRepeaterSupport( ctx );
      fprintf(ctx->pReport, "Repeater support: %s\n", ctx->canRemoteClone ? "yes" : "no" );
      printf("Repeater support: %s\n", ctx->canRemoteClone ? "yes" : "no" );
      if ( ctx->canRemoteClone )
      {
         fprintf(ctx->pReport, "Measuring Wayland Repeating...\n");
         printf("\nMeasuring Wayland Repeating...\n");

         ctx->waylandTotal= 0;
         ctx->renderWayland= !noWaylandRender;
         measureWaylandNested( ctx, &ctx->master.eglServer );

         waylandTotal= ctx->waylandTotal;

         if ( waylandTotal == 0 )
         {
            fprintf(ctx->pReport, "Wayland Repeating failed\n");
            printf("\nWayland Repeating failed\n");
         }
         else
         if ( directTotal > 0 )
         {
            fprintf(ctx->pReport, "\n");
            fprintf(ctx->pReport, "=================================================================\n");
            fprintf(ctx->pReport, "waymetric repeater speed index: %f\n", ((double)waylandTotal / (double)directTotal) );
            fprintf(ctx->pReport, "=================================================================\n");
         }
      }
   }

   printf("\n");
   printf("writing report to %s\n", reportFilename );

   nRC= 0;

exit:

   unsetenv( "XDG_RUNTIME_DIR" );

   if ( ctx )
   {
      if ( !noWayland && ctx->haveWaylandEGL )
      {
         termWayland( &ctx->master );
      }

      termEGL( &ctx->master.eglServer );

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

      pthread_mutex_destroy( &ctx->client.mutex );
      pthread_cond_destroy( &ctx->client.condReady );
      pthread_mutex_destroy( &ctx->client.mutexReady );
      pthread_mutex_destroy( &ctx->nested.mutex );
      pthread_cond_destroy( &ctx->nested.condReady );
      pthread_mutex_destroy( &ctx->nested.mutexReady );
      pthread_mutex_destroy( &ctx->master.mutex );
      pthread_cond_destroy( &ctx->master.condReady );
      pthread_mutex_destroy( &ctx->master.mutexReady );
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

