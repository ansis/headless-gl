#include <cstring>
#include <vector>
#include <iostream>

#include "webgl.h"

bool WebGLRenderingContext::HAS_DISPLAY = false;
EGLDisplay WebGLRenderingContext::DISPLAY;
WebGLRenderingContext* WebGLRenderingContext::ACTIVE = NULL;
WebGLRenderingContext* WebGLRenderingContext::CONTEXT_LIST_HEAD = NULL;

#define GL_METHOD(method_name)    NAN_METHOD(WebGLRenderingContext:: method_name)

#define GL_BOILERPLATE  \
  NanScope();\
  if(args.This()->InternalFieldCount() <= 0) { \
    return NanThrowError("Invalid WebGL Object"); \
  } \
  WebGLRenderingContext* inst = node::ObjectWrap::Unwrap<WebGLRenderingContext>(args.This()); \
  if(!(inst && inst->setActive())) { \
    return NanThrowError("Invalid GL context"); \
  }

// A 32-bit and 64-bit compatible way of converting a pointer to a GLuint.
static GLuint ToGLuint(const void* ptr) {
  return static_cast<GLuint>(reinterpret_cast<size_t>(ptr));
}

inline void *getImageData(v8::Local<v8::Value> arg) {
  void *pixels = NULL;
  if (!arg->IsNull()) {
    v8::Local<v8::Object> array = v8::Local<v8::Object>::Cast(arg);
    if (!array->IsObject()) {
      return NULL;
    }

    unsigned int offset = array->Get(
      NanNew<v8::String>("byteOffset"))->Uint32Value();

    pixels = (void*) &((char*) array->GetIndexedPropertiesExternalArrayData())[offset];
  }
  return pixels;
}

template<typename Type>
inline Type* getArrayData(v8::Local<v8::Value> arg, int* num = NULL) {
  Type *data=NULL;
  if(num) *num=0;

  if(!arg->IsNull()) {
    if(arg->IsArray()) {
      v8::Local<v8::Array> arr = v8::Local<v8::Array>::Cast(arg);
      if(num) *num=arr->Length();
      data = reinterpret_cast<Type*>(arr->GetIndexedPropertiesExternalArrayData());
    } else if(arg->IsObject()) {
      if(num) *num = arg->ToObject()->GetIndexedPropertiesExternalArrayDataLength();
      data = reinterpret_cast<Type*>(arg->ToObject()->GetIndexedPropertiesExternalArrayData());
    } else {
      return NULL;
    }
  }

  return data;
}

WebGLRenderingContext::WebGLRenderingContext(
  int width,
  int height,
  bool alpha,
  bool depth,
  bool stencil,
  bool antialias,
  bool premultipliedAlpha,
  bool preserveDrawingBuffer,
  bool preferLowPowerToHighPerformance,
  bool failIfMajorPerformanceCaveat) :

  state(GLCONTEXT_STATE_INIT),

  unpack_flip_y(false),
  unpack_premultiply_alpha(false),
  unpack_colorspace_conversion(0x9244),
  unpack_alignment(4),

  next(NULL),
  prev(NULL),
  lastError(GL_NO_ERROR) {

  //Get display
  if(!HAS_DISPLAY) {
    DISPLAY = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if(DISPLAY == EGL_NO_DISPLAY) {
      state = GLCONTEXT_STATE_ERROR;
      return;
    }

    //Initialize EGL
    if(!eglInitialize(DISPLAY, NULL, NULL)) {
      state = GLCONTEXT_STATE_ERROR;
      return;
    }

    //Save display
    HAS_DISPLAY = true;
  }

  //Set up configuration
  std::vector<EGLint> attrib_list;

  #define PUSH_ATTRIB(x, v) \
    attrib_list.push_back(x);\
    attrib_list.push_back(v);

  PUSH_ATTRIB(EGL_SURFACE_TYPE, EGL_PBUFFER_BIT);
  //PUSH_ATTRIB(EGL_CONFORMANT, EGL_OPENGL_ES2_BIT);
  PUSH_ATTRIB(EGL_RED_SIZE, 8);
  PUSH_ATTRIB(EGL_GREEN_SIZE, 8);
  PUSH_ATTRIB(EGL_BLUE_SIZE, 8);
  if(alpha) {
    PUSH_ATTRIB(EGL_ALPHA_SIZE, 8);
  } else {
    PUSH_ATTRIB(EGL_ALPHA_SIZE, 0);
  }
  if(depth) {
    PUSH_ATTRIB(EGL_DEPTH_SIZE, 24);
  } else {
    PUSH_ATTRIB(EGL_DEPTH_SIZE, 0);
  }
  if(stencil) {
    PUSH_ATTRIB(EGL_STENCIL_SIZE, 8);
  } else {
    PUSH_ATTRIB(EGL_STENCIL_SIZE, 0);
  }

  attrib_list.push_back(EGL_NONE);

  #undef PUSH_ATTRIB

  EGLint num_config;
  if(!eglChooseConfig(
      DISPLAY,
      &attrib_list[0],
      &config,
      1,
      &num_config) ||
      num_config != 1) {
    state = GLCONTEXT_STATE_ERROR;
    return;
  }

  //Create context
  EGLint contextAttribs[] = {
    EGL_CONTEXT_CLIENT_VERSION, 3,
    EGL_NONE
  };
  context = eglCreateContext(DISPLAY, config, EGL_NO_CONTEXT, contextAttribs);
  if(context == EGL_NO_CONTEXT) {
    state = GLCONTEXT_STATE_ERROR;
    return;
  }

  EGLint surfaceAttribs[] = {
      EGL_WIDTH,  (EGLint)width,
      EGL_HEIGHT, (EGLint)height,
      EGL_NONE
  };
  surface = eglCreatePbufferSurface(DISPLAY, config, surfaceAttribs);
  if(surface == EGL_NO_SURFACE) {
    state = GLCONTEXT_STATE_ERROR;
    return;
  }

  //Set active
  if(!eglMakeCurrent(DISPLAY, surface, surface, context)) {
    state = GLCONTEXT_STATE_ERROR;
    return;
  }

  //Success
  state = GLCONTEXT_STATE_OK;
  registerContext();
  ACTIVE = this;
}

bool WebGLRenderingContext::setActive() {
  if(state != GLCONTEXT_STATE_OK) {
    return false;
  }
  if(this == ACTIVE) {
    return true;
  }
  if(!eglMakeCurrent(DISPLAY, surface, surface, context)) {
    state = GLCONTEXT_STATE_ERROR;
    return false;
  }
  ACTIVE = this;
  return true;
}

void WebGLRenderingContext::setError(GLenum error) {
  if(error == GL_NO_ERROR || lastError != GL_NO_ERROR) {
    return;
  }
  GLenum prevError = glGetError();
  if(prevError == GL_NO_ERROR) {
    lastError = error;
  }
}

void WebGLRenderingContext::dispose() {
  //Unregister context
  unregisterContext();

  if(!setActive()) {
    state = GLCONTEXT_STATE_ERROR;
    return;
  }

  //Update state
  state = GLCONTEXT_STATE_DESTROY;

  //Destroy all object references
  for(std::map< std::pair<GLuint, GLObjectType>, bool >::iterator iter=objects.begin(); iter!=objects.end(); ++iter) {
    GLuint obj = iter->first.first;
    switch(iter->first.second) {
      case GLOBJECT_TYPE_PROGRAM:
        glDeleteProgram(obj);
        break;
      case GLOBJECT_TYPE_BUFFER:
        glDeleteBuffers(1,&obj);
        break;
      case GLOBJECT_TYPE_FRAMEBUFFER:
        glDeleteFramebuffers(1,&obj);
        break;
      case GLOBJECT_TYPE_RENDERBUFFER:
        glDeleteRenderbuffers(1,&obj);
        break;
      case GLOBJECT_TYPE_SHADER:
        glDeleteShader(obj);
        break;
      case GLOBJECT_TYPE_TEXTURE:
        glDeleteTextures(1,&obj);
        break;
      default:
        break;
    }
  }

  //Deactivate context
  eglMakeCurrent(DISPLAY, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
  ACTIVE = NULL;

  //Destroy surface and context

  //FIXME:  This shouldn't be commented out
  //eglDestroySurface(DISPLAY, surface);
  eglDestroyContext(DISPLAY, context);
}

WebGLRenderingContext::~WebGLRenderingContext() {
  dispose();
}

GL_METHOD(SetError) {
  GL_BOILERPLATE;
  inst->setError((GLenum)args[0]->Int32Value());
  NanReturnUndefined();
}

//Resize surface
GL_METHOD(Resize) {
  GL_BOILERPLATE;

  EGLint width  = (EGLint)args[0]->Int32Value();
  EGLint height = (EGLint)args[1]->Int32Value();

  EGLint surfaceAttribs[] = {
      EGL_WIDTH,  (EGLint)width,
      EGL_HEIGHT, (EGLint)height,
      EGL_NONE
  };

  EGLSurface nextSurface = eglCreatePbufferSurface(
    DISPLAY,
    inst->config,
    surfaceAttribs);
  if(nextSurface == EGL_NO_SURFACE) {
    inst->state = GLCONTEXT_STATE_ERROR;
    NanThrowError("Invalid surface dimensions");
  } else {
    if(!eglMakeCurrent(DISPLAY, nextSurface, nextSurface, inst->context)) {
      inst->state = GLCONTEXT_STATE_ERROR;
      NanThrowError("Error resizing surface");
    } else {
      EGLSurface prevSurface = inst->surface;
      inst->surface = nextSurface;
      //FIXME: Destroy surface when not needed
      //eglDestroySurface(DISPLAY, prevSurface);
    }
  }

  NanReturnUndefined();
}


GL_METHOD(DisposeAll) {
  NanScope();

  while(CONTEXT_LIST_HEAD) {
    CONTEXT_LIST_HEAD->dispose();
  }

  if(WebGLRenderingContext::HAS_DISPLAY) {
    eglTerminate(WebGLRenderingContext::DISPLAY);
    WebGLRenderingContext::HAS_DISPLAY = false;
  }

  NanReturnUndefined();
}

GL_METHOD(New) {
  NanScope();

  WebGLRenderingContext* instance = new WebGLRenderingContext(
    args[0]->Int32Value(),   //Width
    args[1]->Int32Value(),   //Height
    args[2]->BooleanValue(), //Alpha
    args[3]->BooleanValue(), //Depth
    args[4]->BooleanValue(), //Stencil
    args[5]->BooleanValue(), //antialias
    args[6]->BooleanValue(), //premultipliedAlpha
    args[7]->BooleanValue(), //preserve drawing buffer
    args[8]->BooleanValue(), //low power
    args[9]->BooleanValue()  //fail if crap
  );
  if(instance->state != GLCONTEXT_STATE_OK){
    return NanThrowError("Error creating WebGLContext");
  }

  instance->Wrap(args.This());
  NanReturnValue(args.This());
}

GL_METHOD(Destroy) {
  GL_BOILERPLATE
  inst->dispose();
  NanReturnUndefined();
}

GL_METHOD(Uniform1f) {
  GL_BOILERPLATE;

  int location = args[0]->Int32Value();
  float x = (float) args[1]->NumberValue();

  glUniform1f(location, x);
  NanReturnUndefined();
}

GL_METHOD(Uniform2f) {
  GL_BOILERPLATE;

  int location = args[0]->Int32Value();
  float x = (float) args[1]->NumberValue();
  float y = (float) args[2]->NumberValue();

  glUniform2f(location, x, y);
  NanReturnUndefined();
}

GL_METHOD(Uniform3f) {
  GL_BOILERPLATE;

  int location = args[0]->Int32Value();
  float x = (float) args[1]->NumberValue();
  float y = (float) args[2]->NumberValue();
  float z = (float) args[3]->NumberValue();

  glUniform3f(location, x, y, z);
  NanReturnUndefined();
}

GL_METHOD(Uniform4f) {
  GL_BOILERPLATE;

  int location = args[0]->Int32Value();
  float x = (float) args[1]->NumberValue();
  float y = (float) args[2]->NumberValue();
  float z = (float) args[3]->NumberValue();
  float w = (float) args[4]->NumberValue();

  glUniform4f(location, x, y, z, w);
  NanReturnUndefined();
}

GL_METHOD(Uniform1i) {
  GL_BOILERPLATE;

  int location = args[0]->Int32Value();
  int x = args[1]->Int32Value();

  glUniform1i(location, x);
  NanReturnUndefined();
}

GL_METHOD(Uniform2i) {
  GL_BOILERPLATE;

  int location = args[0]->Int32Value();
  int x = args[1]->Int32Value();
  int y = args[2]->Int32Value();

  glUniform2i(location, x, y);
  NanReturnUndefined();
}

GL_METHOD(Uniform3i) {
  GL_BOILERPLATE;

  int location = args[0]->Int32Value();
  int x = args[1]->Int32Value();
  int y = args[2]->Int32Value();
  int z = args[3]->Int32Value();

  glUniform3i(location, x, y, z);

  NanReturnUndefined();
}

GL_METHOD(Uniform4i) {
  GL_BOILERPLATE;

  int location = args[0]->Int32Value();
  int x = args[1]->Int32Value();
  int y = args[2]->Int32Value();
  int z = args[3]->Int32Value();
  int w = args[4]->Int32Value();

  glUniform4i(location, x, y, z, w);

  NanReturnUndefined();
}


GL_METHOD(PixelStorei) {
  GL_BOILERPLATE;

  int pname = args[0]->Int32Value();
  int param = args[1]->Int32Value();

  //Handle WebGL specific extensions
  switch(pname) {
    case 0x9240:
      inst->unpack_flip_y = param != 0;
    break;

    case 0x9241:
      inst->unpack_premultiply_alpha = param != 0;
    break;

    case 0x9243:
      inst->unpack_colorspace_conversion = param;
    break;

    case GL_UNPACK_ALIGNMENT:
      inst->unpack_alignment = param;
      glPixelStorei(pname, param);
    break;

    default:
      glPixelStorei(pname, param);
    break;
  }

  NanReturnUndefined();
}

GL_METHOD(BindAttribLocation) {
  GL_BOILERPLATE;

  GLint program = args[0]->Int32Value();
  GLint index   = args[1]->Int32Value();
  v8::String::Utf8Value name(args[2]);

  glBindAttribLocation(program, index, *name);

  NanReturnUndefined();
}

GLenum WebGLRenderingContext::getError() {
  GLenum error = glGetError();
  if(lastError != GL_NO_ERROR) {
    error = lastError;
  }
  lastError = GL_NO_ERROR;
  return error;
}

GL_METHOD(GetError) {
  GL_BOILERPLATE;
  NanReturnValue(NanNew<v8::Integer>(inst->getError()));
}


GL_METHOD(DrawArrays) {
  GL_BOILERPLATE;

  int mode  = args[0]->Int32Value();
  int first = args[1]->Int32Value();
  int count = args[2]->Int32Value();

  glDrawArrays(mode, first, count);

  NanReturnUndefined();
}

GL_METHOD(UniformMatrix2fv) {
  GL_BOILERPLATE;

  GLint location = args[0]->Int32Value();
  GLboolean transpose = args[1]->BooleanValue();

  GLsizei count=0;
  GLfloat* data=getArrayData<GLfloat>(args[2],&count);

  glUniformMatrix2fv(location, count / 4, transpose, data);

  NanReturnUndefined();
}

GL_METHOD(UniformMatrix3fv) {
  GL_BOILERPLATE;

  GLint location = args[0]->Int32Value();
  GLboolean transpose = args[1]->BooleanValue();
  GLsizei count=0;
  GLfloat* data=getArrayData<GLfloat>(args[2],&count);

  glUniformMatrix3fv(location, count / 9, transpose, data);

  NanReturnUndefined();
}

GL_METHOD(UniformMatrix4fv) {
  GL_BOILERPLATE;

  GLint location = args[0]->Int32Value();
  GLboolean transpose = args[1]->BooleanValue();
  GLsizei count=0;
  GLfloat* data=getArrayData<GLfloat>(args[2],&count);

  glUniformMatrix4fv(location, count / 16, transpose, data);

  NanReturnUndefined();
}

GL_METHOD(GenerateMipmap) {
  GL_BOILERPLATE;

  GLint target = args[0]->Int32Value();
  glGenerateMipmap(target);

  NanReturnUndefined();
}

GL_METHOD(GetAttribLocation) {
  GL_BOILERPLATE;

  int program = args[0]->Int32Value();
  v8::String::Utf8Value name(args[1]);

  NanReturnValue(NanNew<v8::Integer>(glGetAttribLocation(program, *name)));
}


GL_METHOD(DepthFunc) {
  GL_BOILERPLATE;

  glDepthFunc(args[0]->Int32Value());

  NanReturnUndefined();
}


GL_METHOD(Viewport) {
  GL_BOILERPLATE;

  int x = args[0]->Int32Value();
  int y = args[1]->Int32Value();
  int width = args[2]->Int32Value();
  int height = args[3]->Int32Value();

  glViewport(x, y, width, height);

  NanReturnUndefined();
}

GL_METHOD(CreateShader) {
  GL_BOILERPLATE;

  GLuint shader=glCreateShader(args[0]->Int32Value());
  inst->registerGLObj(GLOBJECT_TYPE_SHADER, shader);

  NanReturnValue(NanNew<v8::Integer>(shader));
}


GL_METHOD(ShaderSource) {
  GL_BOILERPLATE;

  int id = args[0]->Int32Value();
  v8::String::Utf8Value code(args[1]);

  const char* codes[1];
  codes[0] = *code;
  GLint length=code.length();

  glShaderSource  (id, 1, codes, &length);

  NanReturnUndefined();
}


GL_METHOD(CompileShader) {
  GL_BOILERPLATE;

  glCompileShader(args[0]->Int32Value());

  NanReturnUndefined();
}

GL_METHOD(FrontFace) {
  GL_BOILERPLATE;

  glFrontFace(args[0]->Int32Value());

  NanReturnUndefined();
}


GL_METHOD(GetShaderParameter) {
  GL_BOILERPLATE;

  int shader = args[0]->Int32Value();
  int pname = args[1]->Int32Value();
  int value = 0;

  glGetShaderiv(shader, pname, &value);

  NanReturnValue(NanNew<v8::Integer>(value));
}

GL_METHOD(GetShaderInfoLog) {
  GL_BOILERPLATE;

  int id = args[0]->Int32Value();
  int Len = 1024;
  char Error[1024];
  glGetShaderInfoLog(id, 1024, &Len, Error);

  NanReturnValue(NanNew<v8::String>(Error));
}


GL_METHOD(CreateProgram) {
  GL_BOILERPLATE;

  GLuint program=glCreateProgram();
  inst->registerGLObj(GLOBJECT_TYPE_PROGRAM, program);

  NanReturnValue(NanNew<v8::Integer>(program));
}


GL_METHOD(AttachShader) {
  GL_BOILERPLATE;

  int program = args[0]->Int32Value();
  int shader = args[1]->Int32Value();

  glAttachShader(program, shader);

  NanReturnUndefined();
}


GL_METHOD(LinkProgram) {
  GL_BOILERPLATE;

  glLinkProgram(args[0]->Int32Value());

  NanReturnUndefined();
}


GL_METHOD(GetProgramParameter) {
  GL_BOILERPLATE;

  GLint program = args[0]->Int32Value();
  GLenum pname  = (GLenum)(args[1]->Int32Value());
  GLint value = 0;

  glGetProgramiv(program, pname, &value);

  NanReturnValue(NanNew<v8::Integer>(value));
}


GL_METHOD(GetUniformLocation) {
  GL_BOILERPLATE;

  int program = args[0]->Int32Value();
  NanAsciiString name(args[1]);

  NanReturnValue(NanNew<v8::Integer>(glGetUniformLocation(program, *name)));
}


GL_METHOD(ClearColor) {
  GL_BOILERPLATE;

  float red   = (float) args[0]->NumberValue();
  float green = (float) args[1]->NumberValue();
  float blue  = (float) args[2]->NumberValue();
  float alpha = (float) args[3]->NumberValue();
  glClearColor(red, green, blue, alpha);

  NanReturnUndefined();
}


GL_METHOD(ClearDepth) {
  GL_BOILERPLATE;

  float depth = (float) args[0]->NumberValue();
  glClearDepthf(depth);

  NanReturnUndefined();
}

GL_METHOD(Disable) {
  GL_BOILERPLATE;

  glDisable(args[0]->Int32Value());

  NanReturnUndefined();
}

GL_METHOD(Enable) {
  GL_BOILERPLATE;

  glEnable(args[0]->Int32Value());

  NanReturnUndefined();
}


GL_METHOD(CreateTexture) {
  GL_BOILERPLATE;

  GLuint texture;
  glGenTextures(1, &texture);
  inst->registerGLObj(GLOBJECT_TYPE_TEXTURE, texture);

  NanReturnValue(NanNew<v8::Integer>(texture));
}


GL_METHOD(BindTexture) {
  GL_BOILERPLATE;

  int target = args[0]->Int32Value();
  int texture = args[1]->Int32Value();

  glBindTexture(target, texture);

  NanReturnUndefined();
}

unsigned char* WebGLRenderingContext::unpackPixels(
  GLenum type,
  GLenum format,
  GLint width,
  GLint height,
  unsigned char* pixels) {

  //Compute pixel size
  GLint pixelSize = 1;
  if(type == GL_UNSIGNED_BYTE || type == GL_FLOAT) {
    if(type == GL_FLOAT) {
      pixelSize = 4;
    }
    switch(format) {
      case GL_ALPHA:
      case GL_LUMINANCE:
      break;
      case GL_LUMINANCE_ALPHA:
        pixelSize *= 2;
      break;
      case GL_RGB:
        pixelSize *= 3;
      break;
      case GL_RGBA:
        pixelSize *= 4;
      break;
    }
  } else {
    pixelSize = 2;
  }

  //Compute row stride
  GLint rowStride = pixelSize * width;
  if((rowStride % unpack_alignment) != 0) {
    rowStride += unpack_alignment - (rowStride % unpack_alignment);
  }

  GLint imageSize = rowStride * height;
  unsigned char* unpacked = new unsigned char[imageSize];

  if(unpack_flip_y) {
    for(int i=0,j=height-1; j>=0; ++i, --j) {
      memcpy(
        (void*)(unpacked + j*rowStride),
        (void*)(pixels   + i*rowStride),
        width * pixelSize);
    }
  } else {
    memcpy((void*)unpacked, (void*)pixels, imageSize);
  }

  //Premultiply alpha unpacking
  if(unpack_premultiply_alpha &&
     (format == GL_LUMINANCE_ALPHA ||
      format == GL_RGBA)) {

    for(int row=0; row<height; ++row) {
      for(int col=0; col<width; ++col) {
        unsigned char* pixel = unpacked + (row*rowStride) + (col*pixelSize);
        if(format == GL_LUMINANCE_ALPHA) {
          pixel[0] *= pixel[1] / 255.0;
        } else if(type == GL_UNSIGNED_BYTE) {
          float scale = pixel[3] / 255.0;
          pixel[0] *= scale;
          pixel[1] *= scale;
          pixel[2] *= scale;
        } else if(type == GL_UNSIGNED_SHORT_4_4_4_4) {
          int r = pixel[0]&0x0f;
          int g = pixel[0]>>4;
          int b = pixel[1]&0x0f;
          int a = pixel[1]>>4;

          float scale = a / 15.0;
          r *= scale;
          g *= scale;
          b *= scale;

          pixel[0] = r + (g<<4);
          pixel[1] = b + (a<<4);
        } else if(type == GL_UNSIGNED_SHORT_5_5_5_1) {
          if((pixel[0]&1) == 0) {
            pixel[0] = 1; //why does this get set 1?!?!?!
            pixel[1] = 0;
          }
        }
      }
    }
  }

  return unpacked;
}

GL_METHOD(TexImage2D) {
  GL_BOILERPLATE;

  int target         = args[0]->Int32Value();
  int level          = args[1]->Int32Value();
  int internalformat = args[2]->Int32Value();
  int width          = args[3]->Int32Value();
  int height         = args[4]->Int32Value();
  int border         = args[5]->Int32Value();
  int format         = args[6]->Int32Value();
  int type           = args[7]->Int32Value();
  void *pixels       = getImageData(args[8]);

  if(pixels && (
      inst->unpack_flip_y ||
      inst->unpack_premultiply_alpha)) {
    unsigned char* unpacked = inst->unpackPixels(
      type,
      format,
      width, height,
      (unsigned char*)pixels);
    glTexImage2D(
      target,
      level,
      internalformat,
      width,
      height,
      border,
      format,
      type,
      (void*)unpacked);
    delete[] unpacked;
  } else {
    glTexImage2D(
      target,
      level,
      internalformat,
      width,
      height,
      border,
      format,
      type,
      pixels);
  }

  NanReturnUndefined();
}

GL_METHOD(TexSubImage2D) {
  GL_BOILERPLATE;
  GLenum target   = args[0]->Int32Value();
  GLint level     = args[1]->Int32Value();
  GLint xoffset   = args[2]->Int32Value();
  GLint yoffset   = args[3]->Int32Value();
  GLsizei width   = args[4]->Int32Value();
  GLsizei height  = args[5]->Int32Value();
  GLenum format   = args[6]->Int32Value();
  GLenum type     = args[7]->Int32Value();
  void *pixels    = getImageData(args[8]);

  if(inst->unpack_flip_y ||
     inst->unpack_premultiply_alpha) {
    unsigned char* unpacked = inst->unpackPixels(
      type,
      format,
      width, height,
      (unsigned char*)pixels);
    glTexSubImage2D(
      target,
      level,
      xoffset,
      yoffset,
      width,
      height,
      format,
      type,
      (void*)unpacked);
    delete[] unpacked;
  } else {
    glTexSubImage2D(
      target,
      level,
      xoffset,
      yoffset,
      width,
      height,
      format,
      type,
      pixels);
  }

  NanReturnUndefined();
}



GL_METHOD(TexParameteri) {
  GL_BOILERPLATE;

  int target = args[0]->Int32Value();
  int pname = args[1]->Int32Value();
  int param = args[2]->Int32Value();

  glTexParameteri(target, pname, param);

  NanReturnUndefined();
}

GL_METHOD(TexParameterf) {
  GL_BOILERPLATE;

  int target = args[0]->Int32Value();
  int pname = args[1]->Int32Value();
  float param = (float) args[2]->NumberValue();

  glTexParameterf(target, pname, param);

  NanReturnUndefined();
}


GL_METHOD(Clear) {
  GL_BOILERPLATE;

  glClear(args[0]->Int32Value());

  NanReturnUndefined();
}


GL_METHOD(UseProgram) {
  GL_BOILERPLATE;

  glUseProgram(args[0]->Int32Value());

  NanReturnUndefined();
}

GL_METHOD(CreateBuffer) {
  GL_BOILERPLATE;

  GLuint buffer;
  glGenBuffers(1, &buffer);
  inst->registerGLObj(GLOBJECT_TYPE_BUFFER, buffer);

  NanReturnValue(NanNew<v8::Integer>(buffer));
}

GL_METHOD(BindBuffer) {
  GL_BOILERPLATE;

  GLenum target = (GLenum)args[0]->Int32Value();
  GLuint buffer = (GLuint)args[1]->Uint32Value();
  glBindBuffer(target,buffer);

  NanReturnUndefined();
}


GL_METHOD(CreateFramebuffer) {
  GL_BOILERPLATE;

  GLuint buffer;
  glGenFramebuffers(1, &buffer);
  inst->registerGLObj(GLOBJECT_TYPE_FRAMEBUFFER, buffer);

  NanReturnValue(NanNew<v8::Integer>(buffer));
}


GL_METHOD(BindFramebuffer) {
  GL_BOILERPLATE;

  GLint target = (GLint)args[0]->Int32Value();
  GLint buffer = (GLint)args[1]->Int32Value();

  glBindFramebuffer(target, buffer);

  NanReturnUndefined();
}


GL_METHOD(FramebufferTexture2D) {
  GL_BOILERPLATE;

  GLenum target      = args[0]->Int32Value();
  GLenum attachment  = args[1]->Int32Value();
  GLint textarget   = args[2]->Int32Value();
  GLint texture     = args[3]->Int32Value();
  GLint level       = args[4]->Int32Value();

  glFramebufferTexture2D(target, attachment, textarget, texture, level);

  NanReturnUndefined();
}


GL_METHOD(BufferData) {
  GL_BOILERPLATE;

  GLint target = args[0]->Int32Value();
  GLenum usage = args[2]->Int32Value();

  if(args[1]->IsObject()) {
    v8::Local<v8::Object> array = v8::Local<v8::Object>::Cast(args[1]);

    unsigned int offset = array->Get(
      NanNew<v8::String>("byteOffset"))->Uint32Value();
    int length = array->Get(
      NanNew<v8::String>("byteLength"))->Uint32Value();
    void* ptr = (void*) &((char*) array->GetIndexedPropertiesExternalArrayData())[offset];

    glBufferData(target, length, ptr, usage);

  } else if(args[1]->IsNumber()) {

    int size = args[1]->Int32Value();
    if(size < 0) {
      inst->setError(GL_INVALID_VALUE);
    } else {
      glBufferData(target, size, NULL, usage);
    }
  }

  NanReturnUndefined();
}


GL_METHOD(BufferSubData) {
  GL_BOILERPLATE;

  int target = args[0]->Int32Value();
  int offset = args[1]->Int32Value();
  v8::Local<v8::Object> array = v8::Local<v8::Object>::Cast(args[2]);

  unsigned int a_offset = array->Get(
    NanNew<v8::String>("byteOffset"))->Uint32Value();
  int a_length = array->Get(
    NanNew<v8::String>("byteLength"))->Uint32Value();
  void* a_ptr = (void*) &((char*) array->GetIndexedPropertiesExternalArrayData())[a_offset];

  glBufferSubData(target, offset, a_length, a_ptr);

  NanReturnUndefined();
}


GL_METHOD(BlendEquation) {
  GL_BOILERPLATE;

  int mode=args[0]->Int32Value();;

  glBlendEquation(mode);

  NanReturnUndefined();
}


GL_METHOD(BlendFunc) {
  GL_BOILERPLATE;

  int sfactor=args[0]->Int32Value();;
  int dfactor=args[1]->Int32Value();;

  glBlendFunc(sfactor,dfactor);

  NanReturnUndefined();
}


GL_METHOD(EnableVertexAttribArray) {
  GL_BOILERPLATE;

  glEnableVertexAttribArray(args[0]->Int32Value());

  NanReturnUndefined();
}

GL_METHOD(VertexAttribPointer) {
  GL_BOILERPLATE;

  int indx = args[0]->Int32Value();
  int size = args[1]->Int32Value();
  int type = args[2]->Int32Value();
  int normalized = args[3]->BooleanValue();
  int stride = args[4]->Int32Value();
  long offset = args[5]->Int32Value();

  glVertexAttribPointer(
    indx,
    size,
    type,
    normalized,
    stride,
    (const GLvoid *)offset);

  NanReturnUndefined();
}


GL_METHOD(ActiveTexture) {
  GL_BOILERPLATE;

  glActiveTexture(args[0]->Int32Value());
  NanReturnUndefined();
}


GL_METHOD(DrawElements) {
  GL_BOILERPLATE;

  int mode = args[0]->Int32Value();
  int count = args[1]->Int32Value();
  int type = args[2]->Int32Value();
  GLvoid *offset = reinterpret_cast<GLvoid*>(args[3]->Uint32Value());
  glDrawElements(mode, count, type, offset);
  NanReturnUndefined();
}


GL_METHOD(Flush) {
  GL_BOILERPLATE;
  glFlush();
  NanReturnUndefined();
}

GL_METHOD(Finish) {
  GL_BOILERPLATE;
  glFinish();
  NanReturnUndefined();
}

GL_METHOD(VertexAttrib1f) {
  GL_BOILERPLATE;

  GLuint indx = args[0]->Int32Value();
  float x = (float) args[1]->NumberValue();

  glVertexAttrib1f(indx, x);
  NanReturnUndefined();
}

GL_METHOD(VertexAttrib2f) {
  GL_BOILERPLATE;

  GLuint indx = args[0]->Int32Value();
  float x = (float) args[1]->NumberValue();
  float y = (float) args[2]->NumberValue();

  glVertexAttrib2f(indx, x, y);
  NanReturnUndefined();
}

GL_METHOD(VertexAttrib3f) {
  GL_BOILERPLATE;

  GLuint indx = args[0]->Int32Value();
  float x = (float) args[1]->NumberValue();
  float y = (float) args[2]->NumberValue();
  float z = (float) args[3]->NumberValue();

  glVertexAttrib3f(indx, x, y, z);
  NanReturnUndefined();
}

GL_METHOD(VertexAttrib4f) {
  GL_BOILERPLATE;

  GLuint indx = args[0]->Int32Value();
  float x = (float) args[1]->NumberValue();
  float y = (float) args[2]->NumberValue();
  float z = (float) args[3]->NumberValue();
  float w = (float) args[4]->NumberValue();

  glVertexAttrib4f(indx, x, y, z, w);
  NanReturnUndefined();
}

GL_METHOD(BlendColor) {
  GL_BOILERPLATE;

  GLclampf r= (float) args[0]->NumberValue();
  GLclampf g= (float) args[1]->NumberValue();
  GLclampf b= (float) args[2]->NumberValue();
  GLclampf a= (float) args[3]->NumberValue();

  glBlendColor(r,g,b,a);
  NanReturnUndefined();
}

GL_METHOD(BlendEquationSeparate) {
  GL_BOILERPLATE;

  GLenum modeRGB= args[0]->Int32Value();
  GLenum modeAlpha= args[1]->Int32Value();

  glBlendEquationSeparate(modeRGB,modeAlpha);
  NanReturnUndefined();
}

GL_METHOD(BlendFuncSeparate) {
  GL_BOILERPLATE;

  GLenum srcRGB= args[0]->Int32Value();
  GLenum dstRGB= args[1]->Int32Value();
  GLenum srcAlpha= args[2]->Int32Value();
  GLenum dstAlpha= args[3]->Int32Value();

  glBlendFuncSeparate(srcRGB,dstRGB,srcAlpha,dstAlpha);
  NanReturnUndefined();
}

GL_METHOD(ClearStencil) {
  GL_BOILERPLATE;

  GLint s = args[0]->Int32Value();

  glClearStencil(s);
  NanReturnUndefined();
}

GL_METHOD(ColorMask) {
  GL_BOILERPLATE;

  GLboolean r = args[0]->BooleanValue();
  GLboolean g = args[1]->BooleanValue();
  GLboolean b = args[2]->BooleanValue();
  GLboolean a = args[3]->BooleanValue();

  glColorMask(r,g,b,a);
  NanReturnUndefined();
}

GL_METHOD(CopyTexImage2D) {
  GL_BOILERPLATE;

  GLenum target = args[0]->Int32Value();
  GLint level = args[1]->Int32Value();
  GLenum internalformat = args[2]->Int32Value();
  GLint x = args[3]->Int32Value();
  GLint y = args[4]->Int32Value();
  GLsizei width = args[5]->Int32Value();
  GLsizei height = args[6]->Int32Value();
  GLint border = args[7]->Int32Value();

  glCopyTexImage2D( target, level, internalformat, x, y, width, height, border);
  NanReturnUndefined();
}

GL_METHOD(CopyTexSubImage2D) {
  GL_BOILERPLATE;
  GLenum target  = args[0]->Int32Value();
  GLint level    = args[1]->Int32Value();
  GLint xoffset  = args[2]->Int32Value();
  GLint yoffset  = args[3]->Int32Value();
  GLint x        = args[4]->Int32Value();
  GLint y        = args[5]->Int32Value();
  GLsizei width  = args[6]->Int32Value();
  GLsizei height = args[7]->Int32Value();
  glCopyTexSubImage2D( target, level, xoffset, yoffset, x, y, width, height);
  NanReturnUndefined();
}

GL_METHOD(CullFace) {
  GL_BOILERPLATE;
  GLenum mode = args[0]->Int32Value();
  glCullFace(mode);
  NanReturnUndefined();
}

GL_METHOD(DepthMask) {
  GL_BOILERPLATE;
  GLboolean flag = args[0]->BooleanValue();
  glDepthMask(flag);
  NanReturnUndefined();
}

GL_METHOD(DepthRange) {
  GL_BOILERPLATE;
  GLclampf zNear = (float) args[0]->NumberValue();
  GLclampf zFar = (float) args[1]->NumberValue();
  glDepthRangef(zNear, zFar);
  NanReturnUndefined();
}

GL_METHOD(DisableVertexAttribArray) {
  GL_BOILERPLATE;
  GLuint index = args[0]->Int32Value();
  glDisableVertexAttribArray(index);
  NanReturnUndefined();
}

GL_METHOD(Hint) {
  GL_BOILERPLATE;
  GLenum target = args[0]->Int32Value();
  GLenum mode = args[1]->Int32Value();
  glHint(target, mode);
  NanReturnUndefined();
}

GL_METHOD(IsEnabled) {
  GL_BOILERPLATE;
  GLenum cap = args[0]->Int32Value();
  bool ret=glIsEnabled(cap)!=0;
  NanReturnValue(NanNew<v8::Boolean>(ret));
}

GL_METHOD(LineWidth) {
  GL_BOILERPLATE;
  GLfloat width = (float) args[0]->NumberValue();
  glLineWidth(width);
  NanReturnUndefined();
}

GL_METHOD(PolygonOffset) {
  GL_BOILERPLATE;
  GLfloat factor = (float) args[0]->NumberValue();
  GLfloat units = (float) args[1]->NumberValue();
  glPolygonOffset(factor, units);
  NanReturnUndefined();
}

GL_METHOD(SampleCoverage) {
  GL_BOILERPLATE;
  GLclampf value = (float) args[0]->NumberValue();
  GLboolean invert = args[1]->BooleanValue();
  glSampleCoverage(value, invert);
  NanReturnUndefined();
}

GL_METHOD(Scissor) {
  GL_BOILERPLATE;
  GLint x = args[0]->Int32Value();
  GLint y = args[1]->Int32Value();
  GLsizei width = args[2]->Int32Value();
  GLsizei height = args[3]->Int32Value();
  glScissor(x, y, width, height);
  NanReturnUndefined();
}

GL_METHOD(StencilFunc) {
  GL_BOILERPLATE;

  GLenum func = args[0]->Int32Value();
  GLint ref = args[1]->Int32Value();
  GLuint mask = args[2]->Int32Value();

  glStencilFunc(func, ref, mask);
  NanReturnUndefined();
}

GL_METHOD(StencilFuncSeparate) {
  GL_BOILERPLATE;
  GLenum face = args[0]->Int32Value();
  GLenum func = args[1]->Int32Value();
  GLint ref = args[2]->Int32Value();
  GLuint mask = args[3]->Int32Value();
  glStencilFuncSeparate(face, func, ref, mask);
  NanReturnUndefined();
}

GL_METHOD(StencilMask) {
  GL_BOILERPLATE;
  GLuint mask = args[0]->Uint32Value();
  glStencilMask(mask);
  NanReturnUndefined();
}

GL_METHOD(StencilMaskSeparate) {
  GL_BOILERPLATE;
  GLenum face = args[0]->Int32Value();
  GLuint mask = args[1]->Uint32Value();
  glStencilMaskSeparate(face, mask);
  NanReturnUndefined();
}

GL_METHOD(StencilOp) {
  GL_BOILERPLATE;

  GLenum fail = args[0]->Int32Value();
  GLenum zfail = args[1]->Int32Value();
  GLenum zpass = args[2]->Int32Value();
  glStencilOp(fail, zfail, zpass);

  NanReturnUndefined();
}

GL_METHOD(StencilOpSeparate) {
  GL_BOILERPLATE;
  GLenum face = args[0]->Int32Value();
  GLenum fail = args[1]->Int32Value();
  GLenum zfail = args[2]->Int32Value();
  GLenum zpass = args[3]->Int32Value();
  glStencilOpSeparate(face, fail, zfail, zpass);
  NanReturnUndefined();
}

GL_METHOD(BindRenderbuffer) {
  GL_BOILERPLATE;

  GLenum target = args[0]->Int32Value();
  GLuint buffer = args[1]->Int32Value();
  glBindRenderbuffer(target, buffer);

  NanReturnUndefined();
}

GL_METHOD(CreateRenderbuffer) {
  GL_BOILERPLATE;

  GLuint renderbuffers;
  glGenRenderbuffers(1,&renderbuffers);
  inst->registerGLObj(GLOBJECT_TYPE_RENDERBUFFER, renderbuffers);

  NanReturnValue(NanNew<v8::Integer>(renderbuffers));
}

GL_METHOD(DeleteBuffer) {
  GL_BOILERPLATE;

  GLuint buffer = (GLuint)args[0]->Uint32Value();
  inst->unregisterGLObj(GLOBJECT_TYPE_BUFFER, buffer);

  glDeleteBuffers(1,&buffer);
  NanReturnUndefined();
}

GL_METHOD(DeleteFramebuffer) {
  GL_BOILERPLATE;
  GLuint buffer = args[0]->Uint32Value();
  inst->unregisterGLObj(GLOBJECT_TYPE_FRAMEBUFFER, buffer);
  glDeleteFramebuffers(1,&buffer);
  NanReturnUndefined();
}

GL_METHOD(DeleteProgram) {
  GL_BOILERPLATE;
  GLuint program = args[0]->Uint32Value();
  inst->unregisterGLObj(GLOBJECT_TYPE_PROGRAM, program);
  glDeleteProgram(program);
  NanReturnUndefined();
}

GL_METHOD(DeleteRenderbuffer) {
  GL_BOILERPLATE;
  GLuint renderbuffer = args[0]->Uint32Value();
  inst->unregisterGLObj(GLOBJECT_TYPE_RENDERBUFFER, renderbuffer);
  glDeleteRenderbuffers(1, &renderbuffer);
  NanReturnUndefined();
}

GL_METHOD(DeleteShader) {
  GL_BOILERPLATE;
  GLuint shader = args[0]->Uint32Value();
  inst->unregisterGLObj(GLOBJECT_TYPE_SHADER, shader);
  glDeleteShader(shader);
  NanReturnUndefined();
}

GL_METHOD(DeleteTexture) {
  GL_BOILERPLATE;
  GLuint texture = args[0]->Uint32Value();
  inst->unregisterGLObj(GLOBJECT_TYPE_TEXTURE, texture);
  glDeleteTextures(1,&texture);
  NanReturnUndefined();
}

GL_METHOD(DetachShader) {
  GL_BOILERPLATE;
  GLuint program = args[0]->Uint32Value();
  GLuint shader = args[1]->Uint32Value();
  glDetachShader(program, shader);
  NanReturnUndefined();
}

GL_METHOD(FramebufferRenderbuffer) {
  GL_BOILERPLATE;
  GLenum target             = args[0]->Int32Value();
  GLenum attachment         = args[1]->Int32Value();
  GLenum renderbuffertarget = args[2]->Int32Value();
  GLuint renderbuffer       = args[3]->Uint32Value();
  glFramebufferRenderbuffer(
    target,
    attachment,
    renderbuffertarget,
    renderbuffer);
  NanReturnUndefined();
}

GL_METHOD(GetVertexAttribOffset) {
  GL_BOILERPLATE;
  GLuint index = args[0]->Uint32Value();
  GLenum pname = args[1]->Int32Value();
  void *ret=NULL;
  glGetVertexAttribPointerv(index, pname, &ret);
  NanReturnValue(NanNew<v8::Integer>(ToGLuint(ret)));
}

GL_METHOD(IsBuffer) {
  GL_BOILERPLATE;
  NanReturnValue(NanNew<v8::Boolean>(glIsBuffer(args[0]->Uint32Value())!=0));
}

GL_METHOD(IsFramebuffer) {
  GL_BOILERPLATE;
  NanReturnValue(NanNew<v8::Boolean>(glIsFramebuffer(args[0]->Uint32Value())!=0));
}

GL_METHOD(IsProgram) {
  GL_BOILERPLATE;
  NanReturnValue(NanNew<v8::Boolean>(glIsProgram(args[0]->Uint32Value())!=0));
}

GL_METHOD(IsRenderbuffer) {
  GL_BOILERPLATE;
  NanReturnValue(NanNew<v8::Boolean>(glIsRenderbuffer( args[0]->Uint32Value())!=0));
}

GL_METHOD(IsShader) {
  GL_BOILERPLATE;
  NanReturnValue(NanNew<v8::Boolean>(glIsShader(args[0]->Uint32Value())!=0));
}

GL_METHOD(IsTexture) {
  GL_BOILERPLATE;
  NanReturnValue(NanNew<v8::Boolean>(glIsTexture(args[0]->Uint32Value())!=0));
}

GL_METHOD(RenderbufferStorage) {
  GL_BOILERPLATE;
  GLenum target         = args[0]->Int32Value();
  GLenum internalformat = args[1]->Int32Value();
  GLsizei width         = args[2]->Uint32Value();
  GLsizei height        = args[3]->Uint32Value();

  if(internalformat == GL_DEPTH_STENCIL) {
    internalformat = GL_DEPTH24_STENCIL8;
  }
  glRenderbufferStorage(target, internalformat, width, height);

  NanReturnUndefined();
}

GL_METHOD(GetShaderSource) {
  GL_BOILERPLATE;
  int shader = args[0]->Int32Value();
  GLint len;
  glGetShaderiv(shader, GL_SHADER_SOURCE_LENGTH, &len);
  GLchar *source=new GLchar[len];
  glGetShaderSource(shader, len, NULL, source);
  v8::Local<v8::String> str = NanNew<v8::String>(source);
  delete source;
  NanReturnValue(str);
}

GL_METHOD(ValidateProgram) {
  GL_BOILERPLATE;
  glValidateProgram(args[0]->Int32Value());
  NanReturnUndefined();
}

GL_METHOD(ReadPixels) {
  GL_BOILERPLATE;
  GLint x        = args[0]->Int32Value();
  GLint y        = args[1]->Int32Value();
  GLsizei width  = args[2]->Int32Value();
  GLsizei height = args[3]->Int32Value();
  GLenum format  = args[4]->Int32Value();
  GLenum type    = args[5]->Int32Value();
  void *pixels   = getImageData(args[6]);

  int fbo = 0;
  glGetIntegerv(GL_FRAMEBUFFER_BINDING, &fbo);
  glReadPixels(x, y, width, height, format, type, pixels);

  NanReturnUndefined();
}

GL_METHOD(GetTexParameter) {
  GL_BOILERPLATE;
  GLenum target = args[0]->Int32Value();
  GLenum pname = args[1]->Int32Value();
  GLint param_value=0;
  glGetTexParameteriv(target, pname, &param_value);
  NanReturnValue(NanNew<v8::Integer>(param_value));
}

GL_METHOD(GetActiveAttrib) {
  GL_BOILERPLATE;
  GLuint program = args[0]->Int32Value();
  GLuint index = args[1]->Int32Value();
  //TODO: Check max attribute size length here
  char name[1024];
  GLsizei length=0;
  GLenum type;
  GLsizei size;
  glGetActiveAttrib(program, index, 1024, &length, &size, &type, name);
  if(length > 0) {
    v8::Local<v8::Object> activeInfo = NanNew<v8::Object>();
    activeInfo->Set(NanNew<v8::String>("size"), NanNew<v8::Integer>(size));
    activeInfo->Set(NanNew<v8::String>("type"), NanNew<v8::Integer>((int)type));
    activeInfo->Set(NanNew<v8::String>("name"), NanNew<v8::String>(name));
    NanReturnValue(activeInfo);
  } else {
    NanReturnUndefined();
  }
}

GL_METHOD(GetActiveUniform) {
  GL_BOILERPLATE;
  GLuint program = args[0]->Int32Value();
  GLuint index   = args[1]->Int32Value();

  //TODO: Check max attribute length
  char name[1024];
  GLsizei length=0;
  GLenum  type;
  GLsizei size;
  glGetActiveUniform(program, index, 1024, &length, &size, &type, name);

  if(length > 0) {
    v8::Local<v8::Object> activeInfo = NanNew<v8::Object>();
    activeInfo->Set(NanNew<v8::String>("size"), NanNew<v8::Integer>(size));
    activeInfo->Set(NanNew<v8::String>("type"), NanNew<v8::Integer>((int)type));
    activeInfo->Set(NanNew<v8::String>("name"), NanNew<v8::String>(name));
    NanReturnValue(activeInfo);
  } else {
    NanReturnNull();
  }
}

GL_METHOD(GetAttachedShaders) {
  GL_BOILERPLATE;

  GLuint program = args[0]->Int32Value();

  GLuint shaders[1024];
  GLsizei count;
  glGetAttachedShaders(program, 1024, &count, shaders);

  v8::Local<v8::Array> shadersArr = NanNew<v8::Array>(count);
  for(int i=0;i<count;i++)
    shadersArr->Set(i, NanNew<v8::Integer>((int)shaders[i]));

  NanReturnValue(shadersArr);
}

GL_METHOD(GetParameter) {
  GL_BOILERPLATE;
  GLenum name = args[0]->Int32Value();

  switch(name) {

  case 0x9240 /* UNPACK_FLIP_Y_WEBGL */:
    NanReturnValue(NanNew<v8::Boolean>(inst->unpack_flip_y));

  case 0x9241 /* UNPACK_PREMULTIPLY_ALPHA_WEBGL*/:
    NanReturnValue(NanNew<v8::Boolean>(inst->unpack_premultiply_alpha));

  case 0x9243 /* UNPACK_COLORSPACE_CONVERSION_WEBGL */:
    NanReturnValue(NanNew<v8::Integer>(inst->unpack_colorspace_conversion));

  case GL_BLEND:
  case GL_CULL_FACE:
  case GL_DEPTH_TEST:
  case GL_DEPTH_WRITEMASK:
  case GL_DITHER:
  case GL_POLYGON_OFFSET_FILL:
  case GL_SAMPLE_COVERAGE_INVERT:
  case GL_SCISSOR_TEST:
  case GL_STENCIL_TEST:
  {
    // return a boolean
    GLboolean params;
    ::glGetBooleanv(name, &params);
    NanReturnValue(NanNew<v8::Boolean>(params!=0));
  }
  case GL_DEPTH_CLEAR_VALUE:
  case GL_LINE_WIDTH:
  case GL_POLYGON_OFFSET_FACTOR:
  case GL_POLYGON_OFFSET_UNITS:
  case GL_SAMPLE_COVERAGE_VALUE:
  {
    // return a float
    GLfloat params;
    ::glGetFloatv(name, &params);
    NanReturnValue(NanNew<v8::Number>(params));
  }
  case GL_RENDERER:
  case GL_SHADING_LANGUAGE_VERSION:
  case GL_VENDOR:
  case GL_VERSION:
  case GL_EXTENSIONS:
  {
    // return a string
    char *params=(char*) ::glGetString(name);
    if(params)
      NanReturnValue(NanNew<v8::String>(params));
    NanReturnUndefined();
  }
  case GL_MAX_VIEWPORT_DIMS:
  {
    // return a int32[2]
    GLint params[2];
    ::glGetIntegerv(name, params);

    v8::Local<v8::Array> arr=NanNew<v8::Array>(2);
    arr->Set(0,NanNew<v8::Integer>(params[0]));
    arr->Set(1,NanNew<v8::Integer>(params[1]));
    NanReturnValue(arr);
  }
  case GL_SCISSOR_BOX:
  case GL_VIEWPORT:
  {
    // return a int32[4]
    GLint params[4];
    ::glGetIntegerv(name, params);

    v8::Local<v8::Array> arr=NanNew<v8::Array>(4);
    arr->Set(0,NanNew<v8::Integer>(params[0]));
    arr->Set(1,NanNew<v8::Integer>(params[1]));
    arr->Set(2,NanNew<v8::Integer>(params[2]));
    arr->Set(3,NanNew<v8::Integer>(params[3]));
    NanReturnValue(arr);
  }
  case GL_ALIASED_LINE_WIDTH_RANGE:
  case GL_ALIASED_POINT_SIZE_RANGE:
  case GL_DEPTH_RANGE:
  {
    // return a float[2]
    GLfloat params[2];
    ::glGetFloatv(name, params);
    v8::Local<v8::Array> arr=NanNew<v8::Array>(2);
    arr->Set(0,NanNew<v8::Number>(params[0]));
    arr->Set(1,NanNew<v8::Number>(params[1]));
    NanReturnValue(arr);
  }
  case GL_BLEND_COLOR:
  case GL_COLOR_CLEAR_VALUE:
  {
    // return a float[4]
    GLfloat params[4];
    ::glGetFloatv(name, params);
    v8::Local<v8::Array> arr=NanNew<v8::Array>(4);
    arr->Set(0,NanNew<v8::Number>(params[0]));
    arr->Set(1,NanNew<v8::Number>(params[1]));
    arr->Set(2,NanNew<v8::Number>(params[2]));
    arr->Set(3,NanNew<v8::Number>(params[3]));
    NanReturnValue(arr);
  }
  case GL_COLOR_WRITEMASK:
  {
    // return a boolean[4]
    GLboolean params[4];
    ::glGetBooleanv(name, params);
    v8::Local<v8::Array> arr=NanNew<v8::Array>(4);
    arr->Set(0,NanNew<v8::Boolean>(params[0]==1));
    arr->Set(1,NanNew<v8::Boolean>(params[1]==1));
    arr->Set(2,NanNew<v8::Boolean>(params[2]==1));
    arr->Set(3,NanNew<v8::Boolean>(params[3]==1));
    NanReturnValue(arr);
  }
  case GL_ARRAY_BUFFER_BINDING:
  case GL_CURRENT_PROGRAM:
  case GL_ELEMENT_ARRAY_BUFFER_BINDING:
  case GL_FRAMEBUFFER_BINDING:
  case GL_RENDERBUFFER_BINDING:
  case GL_TEXTURE_BINDING_2D:
  case GL_TEXTURE_BINDING_CUBE_MAP:
  {
    GLint params;
    ::glGetIntegerv(name, &params);
    NanReturnValue(NanNew<v8::Integer>(params));
  }
  default: {
    // return a long
    GLint params;
    ::glGetIntegerv(name, &params);
    NanReturnValue(NanNew<v8::Integer>(params));
  }
  }

  NanReturnUndefined();
}

GL_METHOD(GetBufferParameter) {
  GL_BOILERPLATE;

  GLenum target = args[0]->Int32Value();
  GLenum pname = args[1]->Int32Value();

  GLint params;
  glGetBufferParameteriv(target,pname,&params);

  NanReturnValue(NanNew<v8::Integer>(params));
}

GL_METHOD(GetFramebufferAttachmentParameter) {
  GL_BOILERPLATE;

  GLenum target     = args[0]->Int32Value();
  GLenum attachment = args[1]->Int32Value();
  GLenum pname      = args[2]->Int32Value();

  GLint params;
  glGetFramebufferAttachmentParameteriv(target, attachment, pname, &params);

  NanReturnValue(NanNew<v8::Integer>(params));
}

GL_METHOD(GetProgramInfoLog) {
  GL_BOILERPLATE;

  GLuint program = args[0]->Int32Value();
  int Len = 1024;
  char Error[1024];
  glGetProgramInfoLog(program, 1024, &Len, Error);

  NanReturnValue(NanNew<v8::String>(Error));
}

GL_METHOD(GetShaderPrecisionFormat) {
  GL_BOILERPLATE;

  GLenum shaderType    = args[0]->Int32Value();
  GLenum precisionType = args[1]->Int32Value();

  GLint range[2];
  GLint precision;

  glGetShaderPrecisionFormat(shaderType, precisionType, range, &precision);

  v8::Local<v8::Object> result = NanNew<v8::Object>();

  result->Set(NanNew<v8::String>("rangeMin"),
    NanNew<v8::Integer>(range[0]));
  result->Set(NanNew<v8::String>("rangeMax"),
    NanNew<v8::Integer>(range[1]));
  result->Set(NanNew<v8::String>("precision"),
    NanNew<v8::Integer>(precision));

  NanReturnValue(result);
}

GL_METHOD(GetRenderbufferParameter) {
  GL_BOILERPLATE;

  int target = args[0]->Int32Value();
  int pname = args[1]->Int32Value();
  int value = 0;
  glGetRenderbufferParameteriv(target,pname,&value);

  NanReturnValue(NanNew<v8::Integer>(value));
}

GL_METHOD(GetUniform) {
  GL_BOILERPLATE;

  GLuint program = args[0]->Int32Value();
  GLint location = args[1]->Int32Value();

  float data[16]; // worst case scenario is 16 floats

  glGetUniformfv(program, location, data);

  v8::Local<v8::Array> arr=NanNew<v8::Array>(16);
  for(int i=0;i<16;i++)
    arr->Set(i,NanNew<v8::Number>(data[i]));

  NanReturnValue(arr);
}

GL_METHOD(GetVertexAttrib) {
  GL_BOILERPLATE;

  GLuint index = args[0]->Int32Value();
  GLuint pname = args[1]->Int32Value();

  GLint value=0;

  switch (pname) {
  case GL_VERTEX_ATTRIB_ARRAY_ENABLED:
  case GL_VERTEX_ATTRIB_ARRAY_NORMALIZED:
    glGetVertexAttribiv(index,pname,&value);
    NanReturnValue(NanNew<v8::Boolean>(value!=0));
  case GL_VERTEX_ATTRIB_ARRAY_SIZE:
  case GL_VERTEX_ATTRIB_ARRAY_STRIDE:
  case GL_VERTEX_ATTRIB_ARRAY_TYPE:
    glGetVertexAttribiv(index,pname,&value);
    NanReturnValue(NanNew<v8::Integer>(value));
  case GL_VERTEX_ATTRIB_ARRAY_BUFFER_BINDING:
    glGetVertexAttribiv(index,pname,&value);
    NanReturnValue(NanNew<v8::Integer>(value));
  case GL_CURRENT_VERTEX_ATTRIB: {
    float vextex_attribs[4];
    glGetVertexAttribfv(index,pname,vextex_attribs);
    v8::Local<v8::Array> arr=NanNew<v8::Array>(4);
    arr->Set(0,NanNew<v8::Number>(vextex_attribs[0]));
    arr->Set(1,NanNew<v8::Number>(vextex_attribs[1]));
    arr->Set(2,NanNew<v8::Number>(vextex_attribs[2]));
    arr->Set(3,NanNew<v8::Number>(vextex_attribs[3]));
    NanReturnValue(arr);
  }
  default:
    inst->setError(GL_INVALID_ENUM);
  }
  NanReturnNull();
}

GL_METHOD(GetSupportedExtensions) {
  GL_BOILERPLATE;
  char *extensions=(char*) glGetString(GL_EXTENSIONS);
  NanReturnValue(NanNew<v8::String>(extensions));
}

GL_METHOD(GetExtension) {
  GL_BOILERPLATE;

  //TODO

  NanReturnUndefined();
}

GL_METHOD(CheckFramebufferStatus) {
  GL_BOILERPLATE;
  GLenum target=args[0]->Int32Value();
  NanReturnValue(NanNew<v8::Integer>((int)glCheckFramebufferStatus(target)));
}
