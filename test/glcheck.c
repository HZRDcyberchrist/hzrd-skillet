// Compiles each GLSL file on the command line in a headless Mesa GL 4.5 core
// context and prints the compiler/linker log for any that fail.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
typedef void* EGLDisplay; typedef void* EGLContext; typedef void* EGLConfig; typedef unsigned int EGLBoolean; typedef int32_t EGLint; typedef unsigned int EGLenum;
EGLDisplay eglGetPlatformDisplay(EGLenum, void*, const intptr_t*);
EGLBoolean eglInitialize(EGLDisplay, EGLint*, EGLint*);
EGLBoolean eglBindAPI(EGLenum);
EGLBoolean eglChooseConfig(EGLDisplay, const EGLint*, EGLConfig*, EGLint, EGLint*);
EGLContext eglCreateContext(EGLDisplay, EGLConfig, EGLContext, const EGLint*);
EGLBoolean eglMakeCurrent(EGLDisplay, void*, void*, EGLContext);
void* eglGetProcAddress(const char*);
typedef unsigned int GLuint; typedef int GLint; typedef unsigned int GLenum; typedef int GLsizei; typedef char GLchar;
static GLuint (*glCreateShader)(GLenum); static void (*glShaderSource)(GLuint, GLsizei, const GLchar**, const GLint*);
static void (*glCompileShader)(GLuint); static void (*glGetShaderiv)(GLuint, GLenum, GLint*);
static void (*glGetShaderInfoLog)(GLuint, GLsizei, GLsizei*, GLchar*);
static GLuint (*glCreateProgram)(void); static void (*glAttachShader)(GLuint, GLuint); static void (*glLinkProgram)(GLuint);
static void (*glGetProgramiv)(GLuint, GLenum, GLint*); static void (*glGetProgramInfoLog)(GLuint, GLsizei, GLsizei*, GLchar*);
#define L(n) *(void**)&n = eglGetProcAddress(#n)
static char* slurp(const char* p){FILE*f=fopen(p,"rb");if(!f)return 0;fseek(f,0,2);long n=ftell(f);fseek(f,0,0);char*b=malloc(n+1);fread(b,1,n,f);b[n]=0;fclose(f);return b;}
int main(int argc, char** argv){
  EGLDisplay d = eglGetPlatformDisplay(0x31DD, 0, 0); EGLint a,b; eglInitialize(d,&a,&b); eglBindAPI(0x30A2);
  EGLint ca[]={0x3040,0x0008,0x3038}; EGLConfig cfg=0; EGLint n=0; eglChooseConfig(d,ca,&cfg,1,&n);
  EGLint at[]={0x3098,4,0x30FB,5,0x30FD,1,0x3038}; EGLContext c = eglCreateContext(d,n?cfg:0,0,at); eglMakeCurrent(d,0,0,c);
  L(glCreateShader);L(glShaderSource);L(glCompileShader);L(glGetShaderiv);L(glGetShaderInfoLog);
  L(glCreateProgram);L(glAttachShader);L(glLinkProgram);L(glGetProgramiv);L(glGetProgramInfoLog);
  int bad=0; static char log[65536];
  for(int i=1;i<argc;i++){
    const char* p=argv[i]; char* src=slurp(p);
    GLenum ty = strstr(p,".vs.")?0x8B31: strstr(p,".fs.")?0x8B30: 0x91B9;
    GLuint s=glCreateShader(ty); glShaderSource(s,1,(const GLchar**)&src,0); glCompileShader(s);
    GLint ok=0; glGetShaderiv(s,0x8B81,&ok);
    if(!ok){ glGetShaderInfoLog(s,sizeof log,0,log); printf("=== %s COMPILE FAIL\n%s\n",p,log); bad++; continue; }
    if(ty==0x91B9){ GLuint pr=glCreateProgram(); glAttachShader(pr,s); glLinkProgram(pr); glGetProgramiv(pr,0x8B82,&ok);
      if(!ok){ glGetProgramInfoLog(pr,sizeof log,0,log); printf("=== %s LINK FAIL\n%s\n",p,log); bad++; continue; } }
    printf("ok  %s\n",p);
  }
  printf("%d failed\n",bad); return bad!=0;
}
