#pragma once
// Minimal API mock for compiling and testing the diagnostic bridge WITHOUT a GPU.
// Never added to the application's include directories.
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <unordered_map>
using GLenum=unsigned; using GLuint=unsigned; using GLint=int; using GLsizei=int;
using GLbitfield=unsigned; using GLsizeiptr=std::ptrdiff_t; using GLuint64=uint64_t;
using GLubyte=unsigned char;
#define GLEW_VERSION_3_0 1
#define GLEW_VERSION_3_3 1
#define GLEW_VERSION_2_1 1
#define GLEW_ARB_framebuffer_object 1
#define GLEW_EXT_framebuffer_object 1
#define GLEW_ARB_pixel_buffer_object 1
#define GLEW_ARB_timer_query 1
#define GL_NO_ERROR 0
#define GL_FALSE 0
#define GL_TRUE 1
#define GL_OUT_OF_MEMORY 0x0505
#define GL_RGBA 0x1908
#define GL_UNSIGNED_BYTE 0x1401
#define GL_UNSIGNED_INT 0x1405
#define GL_DEPTH_COMPONENT 0x1902
#define GL_FLOAT 0x1406
#define GL_READ_FRAMEBUFFER_BINDING 0x8CAA
#define GL_FRAMEBUFFER_BINDING_EXT 0x8CA6
#define GL_READ_BUFFER 0x0C02
#define GL_PIXEL_PACK_BUFFER_BINDING 0x88ED
#define GL_PACK_ALIGNMENT 0x0D05
#define GL_PACK_ROW_LENGTH 0x0D02
#define GL_PACK_SKIP_ROWS 0x0D03
#define GL_PACK_SKIP_PIXELS 0x0D04
#define GL_PACK_SWAP_BYTES 0x0D00
#define GL_TRIANGLES 0x0004
#define GL_FRAMEBUFFER_COMPLETE 0x8CD5
#define GL_FRAMEBUFFER_COMPLETE_EXT 0x8CD5
#define GL_SAMPLES 0x80A9
#define GL_VIEWPORT 0x0BA2
#define GL_VENDOR 0x1F00
#define GL_RENDERER 0x1F01
#define GL_VERSION 0x1F02
#define GL_SHADING_LANGUAGE_VERSION 0x8B8C
#define GL_QUERY_RESULT_AVAILABLE 0x8867
#define GL_QUERY_RESULT 0x8866
#define GL_TIMESTAMP 0x8E28
#define GL_STATIC_DRAW 0x88E4
#define GL_ARRAY_BUFFER 0x8892
#define GL_ELEMENT_ARRAY_BUFFER 0x8893
namespace MockGL {
inline int buffers=0, reads=0, draws=0, string_queries=0, result_reads=0;
inline GLenum error=GL_NO_ERROR;
inline bool available=false;
inline GLuint next_query=1;
inline GLuint64 timestamp=100;
inline std::unordered_map<GLuint,GLuint64> times;
}
inline GLenum glGetError() { auto e=MockGL::error; MockGL::error=0; return e; }
inline const GLubyte* glGetString(GLenum) { ++MockGL::string_queries; return reinterpret_cast<const GLubyte*>("MockGL"); }
inline void glGetIntegerv(GLenum p,GLint* value) { if(p==GL_VIEWPORT) { value[0]=0;value[1]=0;value[2]=1920;value[3]=1080; } else *value=0; }
inline void glBufferData(GLenum,GLsizeiptr,const void*,GLenum) { ++MockGL::buffers; }
inline void glReadPixels(GLint,GLint,GLsizei w,GLsizei h,GLenum,GLenum,void* dst) { ++MockGL::reads; if(dst) std::memset(dst,0,size_t(w)*size_t(h)*4); }
inline void glDrawElements(GLenum,GLsizei,GLenum,const void*) { ++MockGL::draws; }
inline void glDrawElementsInstanced(GLenum,GLsizei,GLenum,const void*,GLsizei) { ++MockGL::draws; }
inline void glTexImage2D(GLenum,GLint,GLint,GLsizei,GLsizei,GLint,GLenum,GLenum,const void*) {}
inline void glRenderbufferStorage(GLenum,GLenum,GLsizei,GLsizei) {}
inline void glRenderbufferStorageEXT(GLenum,GLenum,GLsizei,GLsizei) {}
inline void glRenderbufferStorageMultisample(GLenum,GLsizei,GLenum,GLsizei,GLsizei) {}
inline void glBlitFramebuffer(GLint,GLint,GLint,GLint,GLint,GLint,GLint,GLint,GLbitfield,GLenum) {}
inline void glBlitFramebufferEXT(GLint,GLint,GLint,GLint,GLint,GLint,GLint,GLint,GLbitfield,GLenum) {}
inline GLenum glCheckFramebufferStatus(GLenum) { return GL_FRAMEBUFFER_COMPLETE; }
inline GLenum glCheckFramebufferStatusEXT(GLenum) { return GL_FRAMEBUFFER_COMPLETE_EXT; }
inline void glGenQueries(GLsizei n,GLuint* ids) { for(int i=0;i<n;++i) ids[i]=MockGL::next_query++; }
inline void glDeleteQueries(GLsizei n,const GLuint* ids) { for(int i=0;i<n;++i) MockGL::times.erase(ids[i]); }
inline void glQueryCounter(GLuint id,GLenum) { MockGL::times[id]=MockGL::timestamp++; }
inline void glGetQueryObjectiv(GLuint,GLenum,GLint* value) { *value=MockGL::available ? GL_TRUE : GL_FALSE; }
inline void glGetQueryObjectui64v(GLuint id,GLenum,GLuint64* value) { ++MockGL::result_reads; *value=MockGL::times[id]; }
