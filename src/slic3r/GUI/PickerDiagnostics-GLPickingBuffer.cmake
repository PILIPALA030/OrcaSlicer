pd_entry("bool GLPickingBuffer::EnsureSize(" [=[
    PD_SCOPE("PickingFBO.EnsureSize");
    PD_EVENT("FBO_ENSURE", "requested=" << width << 'x' << height << " current=" << _width << 'x' << _height
        << " ready=" << IsReady() << " failed=" << _failedWidth << 'x' << _failedHeight << " fbo=" << _framebuffer);
]=])
pd_entry("bool GLPickingBuffer::CreateResources(" [=[
    PD_SCOPE("PickingFBO.CreateResources");
    PD_EVENT("FBO_CREATE", "width=" << width << " height=" << height);
]=])
pd_entry("bool GLPickingBuffer::BeginRender(" [=[
    PD_SCOPE("PickingFBO.BeginRender");
    PD_EVENT("FBO_BEGIN", "ready=" << IsReady() << " render_bound=" << _renderBound << " fbo=" << _framebuffer);
]=])
pd_entry("void GLPickingBuffer::EndRender(" [=[
    PD_SCOPE("PickingFBO.EndRender");
    PD_EVENT("FBO_RESTORE", "draw=" << _previousDrawFramebuffer << " read=" << _previousReadFramebuffer << " bound=" << _renderBound);
]=])
pd_entry("void GLPickingBuffer::BindFramebuffer(" [=[
    PD_VERBOSE_SCOPE("PickingFBO.BindFramebuffer");
    PD_DETAIL("FBO_BIND", "target=" << target << " framebuffer=" << framebuffer << " api=" << static_cast<int>(_framebufferType));
]=])
pd_entry("void GLPickingBuffer::Reset(" [=[
    PD_SCOPE("PickingFBO.Reset");
    PD_EVENT("FBO_RESET", "fbo=" << _framebuffer << " width=" << _width << " height=" << _height);
]=])
pd_entry("void GLPickingBuffer::DestroyResources(" [=[
    PD_SCOPE("PickingFBO.DestroyResources");
    PD_EVENT("FBO_DESTROY", "fbo=" << _framebuffer << " color=" << _colorTexture << " depth=" << _depthRenderbuffer);
]=])
pd_entry("bool GLPickingBuffer::ReadColorRect(" [=[
    PD_SCOPE("PickingFBO.ReadColorRect");
    PD_EVENT("COLOR_RECT", "fbo=" << _framebuffer << " x=" << x << " y=" << y << " width=" << width << " height=" << height
        << " valid_rect=" << ValidateRect(x,y,width,height) << " capacity=" << pixels.capacity());
]=])
pd_replace("pixels.resize(pixelCount);" "PD_CALL(\"readback_cpu_vector_resize\", pixels.resize(pixelCount));")
pd_entry("bool GLPickingBuffer::ReadDepthPoint(" [=[
    PD_SCOPE("PickingFBO.ReadDepthPoint");
    PD_EVENT("DEPTH_POINT", "fbo=" << _framebuffer << " x=" << x << " y=" << y << " valid_rect=" << ValidateRect(x,y,1,1));
]=])
pd_entry("bool GLPickingBuffer::ReadPoint(" [=[
    PD_SCOPE("PickingFBO.ReadPoint");
    PD_EVENT("POINT", "fbo=" << _framebuffer << " x=" << x << " y=" << y << " valid_rect=" << ValidateRect(x,y,1,1));
]=])
pd_replace("    _width = width;\n    _height = height;" [=[
    PD_EVENT("FBO_CREATED", "fbo=" << _framebuffer << " color=" << _colorTexture << " depth=" << _depthRenderbuffer
        << " width=" << width << " height=" << height);
    _width = width;
    _height = height;]=])
