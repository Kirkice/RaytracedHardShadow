#include "pch.h"
#ifdef _WIN32
#include "rthsResourceTranslatorDXR.h"
#include "rthsGfxContextDXR.h"

namespace rths {

class ResourceTranslatorBase : public IResourceTranslator
{
protected:
    ID3D12ResourcePtr createTemporaryTextureImpl(int width, int height, bool shared);
};


class D3D11ResourceTranslator : public ResourceTranslatorBase
{
public:
    D3D11ResourceTranslator(ID3D11Device *device);
    ~D3D11ResourceTranslator() override;

    ID3D12FencePtr getFence(ID3D12Device *dxr_device) override;
    uint64_t inclementFenceValue() override;

    TextureDataDXR createTemporaryTexture(void *ptr) override;
    void applyTexture(TextureDataDXR& tex) override;
    BufferDataDXR translateBuffer(void *ptr) override;

    void copyResource(ID3D11Resource *dst, ID3D11Resource *src);

private:
    // note:
    // sharing resources from d3d12 to d3d11 require d3d11.1. also, fence is supported only d3d11.4 or newer.
    ID3D11Device5Ptr m_unity_device;
    ID3D11DeviceContext4Ptr m_unity_context;

    ID3D11FencePtr m_fence;
    uint64_t m_fence_value = 0;
    FenceEvent m_fence_event;
};

class D3D12ResourceTranslator : public ResourceTranslatorBase
{
public:
    D3D12ResourceTranslator(ID3D12Device *device);
    ~D3D12ResourceTranslator() override;

    ID3D12FencePtr getFence(ID3D12Device *dxr_device) override;
    uint64_t inclementFenceValue() override;

    TextureDataDXR createTemporaryTexture(void *ptr) override;
    void applyTexture(TextureDataDXR& tex) override;
    BufferDataDXR translateBuffer(void *ptr) override;

    void executeAndWaitCopy();
    void executeAndWaitResourceBarrier(ID3D12Resource *resource, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after);

private:
    ID3D12DevicePtr m_unity_device;

    ID3D12FencePtr m_fence;
    uint64_t m_fence_value = 0;
    FenceEvent m_fence_event;

    // command list for resource barrier
    ID3D12CommandAllocatorPtr m_cmd_allocator;
    ID3D12GraphicsCommandList4Ptr m_cmd_list;
    ID3D12CommandQueuePtr m_cmd_queue;

    // command list for copy
    ID3D12CommandAllocatorPtr m_cmd_allocator_copy;
    ID3D12GraphicsCommandList4Ptr m_cmd_list_copy;
    ID3D12CommandQueuePtr m_cmd_queue_copy;
};




ID3D12ResourcePtr ResourceTranslatorBase::createTemporaryTextureImpl(int width, int height, bool shared)
{
    // note: sharing textures with d3d11 requires some flags and restrictions:
    // - MipLevels must be 1
    // - D3D12_HEAP_FLAG_SHARED for heap flags
    // - D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET and D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS for resource flags

    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Alignment = 0;
    desc.Width = width;
    desc.Height = height;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_R32_FLOAT; // typeless is not acceptable when enabling unordered access
    desc.SampleDesc.Count = 1;
    desc.SampleDesc.Quality = 0;
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    D3D12_HEAP_FLAGS flags = D3D12_HEAP_FLAG_NONE;
    D3D12_RESOURCE_STATES initial_state = D3D12_RESOURCE_STATE_COMMON;
    if (shared) {
        desc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS;
        flags |= D3D12_HEAP_FLAG_SHARED;
    }

    ID3D12ResourcePtr ret;
    auto hr = GfxContextDXR::getInstance()->getDevice()->CreateCommittedResource(&kDefaultHeapProps, flags, &desc, initial_state, nullptr, IID_PPV_ARGS(&ret));
    return ret;

}

D3D11ResourceTranslator::D3D11ResourceTranslator(ID3D11Device *device)
{
    device->QueryInterface(IID_PPV_ARGS(&m_unity_device));

    ID3D11DeviceContextPtr device_context;
    m_unity_device->GetImmediateContext(&device_context);
    device_context->QueryInterface(IID_PPV_ARGS(&m_unity_context));

    m_unity_device->CreateFence(0, D3D11_FENCE_FLAG_SHARED, IID_PPV_ARGS(&m_fence));
}

D3D11ResourceTranslator::~D3D11ResourceTranslator()
{
}

ID3D12FencePtr D3D11ResourceTranslator::getFence(ID3D12Device *dxr_device)
{
    ID3D12FencePtr ret;
    HANDLE hfence;
    auto hr = m_fence->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &hfence);
    if (SUCCEEDED(hr)) {
        hr = dxr_device->OpenSharedHandle(hfence, IID_PPV_ARGS(&ret));
    }
    return ret;
}

uint64_t D3D11ResourceTranslator::inclementFenceValue()
{
    return ++m_fence_value;
}


TextureDataDXR D3D11ResourceTranslator::createTemporaryTexture(void *ptr)
{
    TextureDataDXR ret;

    auto tex_unity = (ID3D11Texture2D*)ptr;
    D3D11_TEXTURE2D_DESC src_desc{};
    tex_unity->GetDesc(&src_desc);

    ret.texture = ptr;
    ret.width = src_desc.Width;
    ret.height = src_desc.Height;
    ret.resource = createTemporaryTextureImpl(src_desc.Width, src_desc.Height, true);

    auto hr = GfxContextDXR::getInstance()->getDevice()->CreateSharedHandle(ret.resource, nullptr, GENERIC_ALL, nullptr, &ret.handle);
    if (SUCCEEDED(hr)) {
        // note: ID3D11Device::OpenSharedHandle() doesn't accept handles created by d3d12. ID3D11Device1::OpenSharedHandle1() is needed.
        hr = m_unity_device->OpenSharedResource1(ret.handle, IID_PPV_ARGS(&ret.temporary_d3d11));
    }
    return ret;
}

void D3D11ResourceTranslator::applyTexture(TextureDataDXR& src)
{
    if (src.temporary_d3d11) {
        copyResource((ID3D11Texture2D*)src.texture, src.temporary_d3d11);
    }
}

BufferDataDXR D3D11ResourceTranslator::translateBuffer(void *ptr)
{
    BufferDataDXR ret;
    ret.buffer = ptr;

    auto buf_unity = (ID3D11Buffer*)ptr;
    D3D11_BUFFER_DESC src_desc{};
    buf_unity->GetDesc(&src_desc);

    // create temporary buffer that can be shared with DXR side
    D3D11_BUFFER_DESC tmp_desc = src_desc;
    tmp_desc.Usage = D3D11_USAGE_DEFAULT;
    tmp_desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    tmp_desc.CPUAccessFlags = 0;
    tmp_desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED;
    HRESULT hr = m_unity_device->CreateBuffer(&tmp_desc, nullptr, &ret.temporary_d3d11);
    if (SUCCEEDED(hr)) {
        // copy contents to temporary
        copyResource(ret.temporary_d3d11, buf_unity);

        // translate temporary as d3d12 resource
        IDXGIResourcePtr ires;
        hr = ret.temporary_d3d11->QueryInterface(IID_PPV_ARGS(&ires));
        if (SUCCEEDED(hr)) {
            hr = ires->GetSharedHandle(&ret.handle);
            hr = GfxContextDXR::getInstance()->getDevice()->OpenSharedHandle(ret.handle, IID_PPV_ARGS(&ret.resource));
            ret.size = src_desc.ByteWidth;
        }
    }
    return ret;
}

void D3D11ResourceTranslator::copyResource(ID3D11Resource *dst, ID3D11Resource *src)
{
    m_unity_context->CopyResource(dst, src);

    // wait for completion of CopyResource()
    auto fence_value = inclementFenceValue();
    m_unity_context->Signal(m_fence, fence_value);
    m_fence->SetEventOnCompletion(fence_value, m_fence_event);
    ::WaitForSingleObject(m_fence_event, INFINITE);
}




D3D12ResourceTranslator::D3D12ResourceTranslator(ID3D12Device *device)
    : m_unity_device(device)
{
    m_unity_device->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&m_fence));

    // command queue for resource barrier
    {
        D3D12_COMMAND_QUEUE_DESC desc{};
        desc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
        desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        m_unity_device->CreateCommandQueue(&desc, IID_PPV_ARGS(&m_cmd_queue));
    }
    m_unity_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&m_cmd_allocator));
    m_unity_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, m_cmd_allocator, nullptr, IID_PPV_ARGS(&m_cmd_list));

    // command queue for copy
    {
        D3D12_COMMAND_QUEUE_DESC desc{};
        desc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
        desc.Type = D3D12_COMMAND_LIST_TYPE_COPY;
        m_unity_device->CreateCommandQueue(&desc, IID_PPV_ARGS(&m_cmd_queue_copy));
    }
    m_unity_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_COPY, IID_PPV_ARGS(&m_cmd_allocator_copy));
    m_unity_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_COPY, m_cmd_allocator_copy, nullptr, IID_PPV_ARGS(&m_cmd_list_copy));
}

D3D12ResourceTranslator::~D3D12ResourceTranslator()
{
}

ID3D12FencePtr D3D12ResourceTranslator::getFence(ID3D12Device * dxr_device)
{
    return m_fence;
}

uint64_t D3D12ResourceTranslator::inclementFenceValue()
{
    return ++m_fence_value;
}

TextureDataDXR D3D12ResourceTranslator::createTemporaryTexture(void *ptr)
{
    TextureDataDXR ret;

    auto tex_unity = (ID3D12Resource*)ptr;
    D3D12_RESOURCE_DESC src_desc = tex_unity->GetDesc();

    ret.texture = ptr;
    ret.width = (int)src_desc.Width;
    ret.height = (int)src_desc.Height;

    // if unordered access is allowed, it can be directly used as DXR's result buffer. so temporary texture is not needed
    if ((src_desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) != 0) {
        ret.resource = tex_unity;
    }
    else {
        ret.resource = createTemporaryTextureImpl((int)src_desc.Width, (int)src_desc.Height, false);
    }
    return ret;
}

void D3D12ResourceTranslator::applyTexture(TextureDataDXR& src)
{
    auto tex_unity = (ID3D12Resource*)src.texture;
    // copy is not needed if unordered access is allowed
    if (src.resource == tex_unity)
        return;

    executeAndWaitResourceBarrier(tex_unity, D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_STATE_COMMON);

    m_cmd_list_copy->CopyResource(tex_unity, src.resource);
    m_cmd_list_copy->Close();
    executeAndWaitCopy();

    executeAndWaitResourceBarrier(tex_unity, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_GENERIC_READ);
}

BufferDataDXR D3D12ResourceTranslator::translateBuffer(void *ptr)
{
    BufferDataDXR ret;

    auto buf_unity = (ID3D12Resource*)ptr;
    D3D12_RESOURCE_DESC src_desc = buf_unity->GetDesc();

    // on d3d12, buffer can be directly shared with DXR side
    ret.resource = buf_unity;
    ret.size = (int)src_desc.Width;
    return ret;
}

void D3D12ResourceTranslator::executeAndWaitCopy()
{
    ID3D12CommandList* cmd_list_copy = m_cmd_list_copy.GetInterfacePtr();
    m_cmd_queue_copy->ExecuteCommandLists(1, &cmd_list_copy);

    auto fence_value = inclementFenceValue();
    m_cmd_queue_copy->Signal(m_fence, fence_value);
    m_fence->SetEventOnCompletion(fence_value, m_fence_event);
    ::WaitForSingleObject(m_fence_event, INFINITE);

    m_cmd_allocator_copy->Reset();
    m_cmd_list_copy->Reset(m_cmd_allocator_copy, nullptr);
}

void D3D12ResourceTranslator::executeAndWaitResourceBarrier(ID3D12Resource *resource, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
{
    {
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = resource;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier.Transition.StateBefore = before;
        barrier.Transition.StateAfter = after;
        m_cmd_list->ResourceBarrier(1, &barrier);
    }
    m_cmd_list->Close();

    ID3D12CommandList* cmd_list = m_cmd_list.GetInterfacePtr();
    m_cmd_queue->ExecuteCommandLists(1, &cmd_list);

    auto fence_value = inclementFenceValue();
    m_cmd_queue->Signal(m_fence, fence_value);
    m_fence->SetEventOnCompletion(fence_value, m_fence_event);
    ::WaitForSingleObject(m_fence_event, INFINITE);

    m_cmd_allocator->Reset();
    m_cmd_list->Reset(m_cmd_allocator, nullptr);
}


ID3D11Device *g_unity_d3d11_device;
ID3D12Device *g_unity_d3d12_device;

IResourceTranslatorPtr CreateResourceTranslator()
{
    if (g_unity_d3d11_device)
        return std::make_shared<D3D11ResourceTranslator>(g_unity_d3d11_device);
    if (g_unity_d3d12_device)
        return std::make_shared<D3D12ResourceTranslator>(g_unity_d3d12_device);
    return IResourceTranslatorPtr();
}

} // namespace rths
#endif // _WIN32