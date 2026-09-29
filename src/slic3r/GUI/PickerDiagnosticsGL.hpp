#pragma once
// Included after the translation unit's existing GLEW include.
#include <GL/glew.h>
#include "PickerDiagnostics.hpp"
#include <array>
#include <cmath>
#include <iomanip>
#include <memory>
#include <unordered_map>

namespace Slic3r { namespace GUI { namespace PickerDiag {
inline void error_record(GLenum error, const char* where) noexcept {
    if (error != GL_NO_ERROR) PD_EVENT("GL_ERROR","where=" << where << " code=" << error << " hex=0x" << std::hex << error);
}
inline GLenum observed_error(const char* where) noexcept {
    // Return the same error to the original glsafe/error consumer.
    const auto error = ::glGetError(); error_record(error,where); return error;
}
inline void optional_error_probe(const char* where) noexcept {
    // Opt-in only: these calls consume errors and can change glsafe's assertions.
    if (!probe_errors()) return;
    for (int i=0;i<16;++i) { const GLenum error = ::glGetError(); if (error == GL_NO_ERROR) break; error_record(error,where); }
}
inline void buffer_data(GLenum target, GLsizeiptr size, const void* data, GLenum usage) {
    Scope scope("BUFFER_DATA");
    PD_EVENT("UPLOAD_REQUEST","target=" << target << " requested_bytes=" << size << " usage=" << usage << " data_null=" << (data==nullptr));
    if (size>0) note_upload(target,uint64_t(size));
    PD_CALL("glBufferData",::glBufferData(target,size,data,usage));
    optional_error_probe("after_glBufferData_including_preexisting");
}
inline void read_pixels(GLint x, GLint y, GLsizei width, GLsizei height, GLenum format, GLenum type, void* data) {
    Scope scope("READ_PIXELS");
    const unsigned bytes_per_pixel = format == GL_RGBA && type == GL_UNSIGNED_BYTE ? 4 :
                                     format == GL_DEPTH_COMPONENT && type == GL_FLOAT ? 4 : 0;
    const uint64_t bytes = width>0 && height>0 ? uint64_t(width)*uint64_t(height)*bytes_per_pixel : 0;
    note_read(bytes);
    PD_EVENT("READ_REQUEST","x=" << x << " y=" << y << " width=" << width << " height=" << height
        << " format=" << format << " type=" << type << " payload_bytes=" << bytes << " bytes_known=" << (bytes_per_pixel!=0));
    if (detailed()) {
        GLint framebuffer=0, read_buffer=0, pack_buffer=0, alignment=0, row_length=0, skip_rows=0, skip_pixels=0, swap_bytes=0;
        if (GLEW_VERSION_3_0 || GLEW_ARB_framebuffer_object) ::glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING,&framebuffer);
        else if (GLEW_EXT_framebuffer_object) ::glGetIntegerv(GL_FRAMEBUFFER_BINDING_EXT,&framebuffer);
        ::glGetIntegerv(GL_READ_BUFFER,&read_buffer);
        if (GLEW_VERSION_2_1 || GLEW_ARB_pixel_buffer_object) ::glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING,&pack_buffer);
        ::glGetIntegerv(GL_PACK_ALIGNMENT,&alignment); ::glGetIntegerv(GL_PACK_ROW_LENGTH,&row_length);
        ::glGetIntegerv(GL_PACK_SKIP_ROWS,&skip_rows); ::glGetIntegerv(GL_PACK_SKIP_PIXELS,&skip_pixels); ::glGetIntegerv(GL_PACK_SWAP_BYTES,&swap_bytes);
        PD_EVENT("READ_STATE","read_fbo=" << framebuffer << " read_buffer=" << read_buffer << " pack_buffer=" << pack_buffer
            << " pack_alignment=" << alignment << " pack_row_length=" << row_length << " pack_skip_rows=" << skip_rows
            << " pack_skip_pixels=" << skip_pixels << " pack_swap_bytes=" << swap_bytes);
    }
    PD_CALL(format == GL_DEPTH_COMPONENT ? "glReadPixels_depth" : "glReadPixels_color",
            ::glReadPixels(x,y,width,height,format,type,data));
    optional_error_probe("after_glReadPixels_including_preexisting");
}
inline void draw_elements(GLenum mode, GLsizei count, GLenum type, const void* indices) {
    Scope scope("glDrawElements",true);
    note_draw(mode==GL_TRIANGLES && count>0 ? uint64_t(count)/3 : 0);
    PD_DETAIL("DRAW","mode=" << mode << " count=" << count << " index_type=" << type << " offset=" << indices);
    ::glDrawElements(mode,count,type,indices);
}
inline void draw_elements_instanced(GLenum mode, GLsizei count, GLenum type, const void* indices, GLsizei instances) {
    Scope scope("glDrawElementsInstanced",true);
    note_draw(mode==GL_TRIANGLES && count>0 && instances>0 ? (uint64_t(count)/3)*uint64_t(instances) : 0);
    PD_DETAIL("DRAW_INSTANCED","mode=" << mode << " count=" << count << " instances=" << instances << " index_type=" << type);
    ::glDrawElementsInstanced(mode,count,type,indices,instances);
}
inline void tex_image_2d(GLenum target, GLint level, GLint internal_format, GLsizei width, GLsizei height, GLint border, GLenum format, GLenum type, const void* data) {
    Scope scope("glTexImage2D");
    PD_EVENT("TEXTURE_STORAGE","target=" << target << " level=" << level << " internal_format=" << internal_format
        << " width=" << width << " height=" << height << " format=" << format << " type=" << type);
    ::glTexImage2D(target,level,internal_format,width,height,border,format,type,data);
    optional_error_probe("after_glTexImage2D_including_preexisting");
}
inline void renderbuffer_storage(GLenum target, GLenum format, GLsizei width, GLsizei height) {
    Scope scope("glRenderbufferStorage");
    PD_EVENT("RENDERBUFFER_STORAGE","format=" << format << " width=" << width << " height=" << height << " samples=0");
    ::glRenderbufferStorage(target,format,width,height); optional_error_probe("after_renderbuffer_storage");
}
inline void renderbuffer_storage_ext(GLenum target, GLenum format, GLsizei width, GLsizei height) {
    Scope scope("glRenderbufferStorageEXT");
    PD_EVENT("RENDERBUFFER_STORAGE_EXT","format=" << format << " width=" << width << " height=" << height);
    ::glRenderbufferStorageEXT(target,format,width,height); optional_error_probe("after_renderbuffer_storage_ext");
}
inline void renderbuffer_storage_ms(GLenum target, GLsizei samples, GLenum format, GLsizei width, GLsizei height) {
    Scope scope("glRenderbufferStorageMultisample");
    PD_EVENT("RENDERBUFFER_STORAGE","format=" << format << " width=" << width << " height=" << height << " samples=" << samples);
    ::glRenderbufferStorageMultisample(target,samples,format,width,height); optional_error_probe("after_renderbuffer_storage_ms");
}
inline void blit(GLint x0,GLint y0,GLint x1,GLint y1,GLint dx0,GLint dy0,GLint dx1,GLint dy1,GLbitfield mask,GLenum filter) {
    Scope scope("glBlitFramebuffer");
    PD_EVENT("BLIT","src=" << x0 << ',' << y0 << ',' << x1 << ',' << y1 << " dst=" << dx0 << ',' << dy0 << ',' << dx1 << ',' << dy1 << " mask=" << mask << " filter=" << filter);
    ::glBlitFramebuffer(x0,y0,x1,y1,dx0,dy0,dx1,dy1,mask,filter); optional_error_probe("after_blit");
}
inline void blit_ext(GLint x0,GLint y0,GLint x1,GLint y1,GLint dx0,GLint dy0,GLint dx1,GLint dy1,GLbitfield mask,GLenum filter) {
    Scope scope("glBlitFramebufferEXT");
    PD_EVENT("BLIT_EXT","mask=" << mask << " filter=" << filter);
    ::glBlitFramebufferEXT(x0,y0,x1,y1,dx0,dy0,dx1,dy1,mask,filter); optional_error_probe("after_blit_ext");
}
inline GLenum check_framebuffer(GLenum target) {
    Scope scope("glCheckFramebufferStatus");
    const GLenum status = ::glCheckFramebufferStatus(target);
    PD_EVENT("FBO_STATUS","target=" << target << " status=" << status << " complete=" << (status==GL_FRAMEBUFFER_COMPLETE));
    return status;
}
inline GLenum check_framebuffer_ext(GLenum target) {
    Scope scope("glCheckFramebufferStatusEXT");
    const GLenum status = ::glCheckFramebufferStatusEXT(target);
    PD_EVENT("FBO_STATUS_EXT","target=" << target << " status=" << status << " complete=" << (status==GL_FRAMEBUFFER_COMPLETE_EXT));
    return status;
}

// Optional, non-blocking timestamp pairs. Each pool belongs to one GL context
// on its rendering thread; no GL operation is ever performed by the watchdog.
struct GpuPool {
    struct Item { GLuint ids[2]{0,0}; bool busy=false, ended=false; const char* name=""; uint64_t frame=0, issued=0; };
    std::array<Item,64> items;
    bool supported=false;
};
inline auto& gpu_pools() { static thread_local std::unordered_map<const void*,std::unique_ptr<GpuPool>> pools; return pools; }
inline const void*& gpu_current_context() { static thread_local const void* ptr=nullptr; return ptr; }
inline void on_context(const void* ptr) noexcept {
    if (!enabled()) return;
    gpu_current_context()=ptr;
    try {
        auto& pools=gpu_pools();
        auto it=pools.find(ptr);
        if (it==pools.end()) {
            auto pool=std::make_unique<GpuPool>();
            pool->supported=GLEW_VERSION_3_3 || GLEW_ARB_timer_query;
            it=pools.emplace(ptr,std::move(pool)).first;
            const auto string_value=[](GLenum name) { const GLubyte* p=::glGetString(name); return p ? reinterpret_cast<const char*>(p) : "unavailable"; };
            GLint samples=0, viewport[4]{};
            ::glGetIntegerv(GL_SAMPLES,&samples); ::glGetIntegerv(GL_VIEWPORT,viewport);
            PD_EVENT("GL_ENV","context=" << ptr << " vendor=" << std::quoted(string_value(GL_VENDOR))
                << " renderer=" << std::quoted(string_value(GL_RENDERER)) << " version=" << std::quoted(string_value(GL_VERSION))
                << " glsl=" << std::quoted(string_value(GL_SHADING_LANGUAGE_VERSION)) << " samples=" << samples
                << " viewport=" << viewport[0] << ',' << viewport[1] << ',' << viewport[2] << ',' << viewport[3]
                << " timer_supported=" << it->second->supported);
        }
        if (!gpu_timing() || !it->second->supported) return;
        for (auto& item:it->second->items) {
            if (!item.busy || !item.ended) continue;
            GLint available=GL_FALSE;
            ::glGetQueryObjectiv(item.ids[1],GL_QUERY_RESULT_AVAILABLE,&available);
            if (!available) continue; // No wait and no reuse of a pending query.
            GLuint64 begin=0,end=0;
            ::glGetQueryObjectui64v(item.ids[0],GL_QUERY_RESULT,&begin);
            ::glGetQueryObjectui64v(item.ids[1],GL_QUERY_RESULT,&end);
            PD_EVENT("GPU_TIME","stage=" << item.name << " issued_frame=" << item.frame << " resolved_frame=" << context().frame
                << " timeline_ns=" << (end>=begin ? end-begin : 0) << " timestamp_order_valid=" << (end>=begin)
                << " age_us=" << now_us()-item.issued << " includes_uploads_and_gpu_idle_if_in_scope=1");
            item.busy=false;
        }
    } catch (...) {}
}
inline void release_context(const void* ptr) noexcept {
    try {
        auto& pools=gpu_pools(); auto it=pools.find(ptr); if(it==pools.end()) return;
        for (auto& item:it->second->items) if(item.ids[0]) ::glDeleteQueries(2,item.ids);
        pools.erase(it); if(gpu_current_context()==ptr) gpu_current_context()=nullptr;
    } catch (...) {}
}
class GpuScope {
    GpuPool::Item* item_=nullptr;
public:
    explicit GpuScope(const char* name) noexcept {
        if(!gpu_timing()) return;
        try {
            auto& pools=gpu_pools(); auto it=pools.find(gpu_current_context());
            if(it==pools.end() || !it->second->supported) return;
            for(auto& item:it->second->items) if(!item.busy) {
                if(!item.ids[0]) ::glGenQueries(2,item.ids);
                if(!item.ids[0] || !item.ids[1]) return;
                item.busy=true; item.ended=false; item.name=name; item.frame=context().frame; item.issued=now_us();
                item_=&item; ::glQueryCounter(item.ids[0],GL_TIMESTAMP); return;
            }
            PD_DETAIL("GPU_QUERY_SKIPPED","reason=pool_full");
        } catch (...) {}
    }
    ~GpuScope() noexcept { if(item_) { ::glQueryCounter(item_->ids[1],GL_TIMESTAMP); item_->ended=true; } }
    GpuScope(const GpuScope&)=delete;
    GpuScope& operator=(const GpuScope&)=delete;
};
}}}
#define PD_GPU_SCOPE(name) ::Slic3r::GUI::PickerDiag::GpuScope PD_JOIN(_pd_gpu_,__LINE__)(name)
