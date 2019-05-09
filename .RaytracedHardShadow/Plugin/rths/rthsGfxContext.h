#pragma once

#include "rthsTypes.h"

namespace rths {

class GfxContext
{
public:
    static bool initializeInstance();
    static void finalizeInstance();
    static GfxContext* getInstance();

    bool valid() const;
    ID3D12Device5* getDevice();

    TextureData translateTexture(void *ptr);
    BufferData translateVertexBuffer(void *ptr);
    BufferData translateIndexBuffer(void *ptr);
    BufferData allocateTransformBuffer(const float4x4& trans);

    void setRenderTarget(TextureData rt);
    void setMeshes(std::vector<MeshBuffers>& meshes);
    void flush();
    void finish();

private:
    ID3D12ResourcePtr createBuffer(uint64_t size, D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES state, const D3D12_HEAP_PROPERTIES& heap_props);
    D3D12_CPU_DESCRIPTOR_HANDLE createRTV(ID3D12ResourcePtr pResource, ID3D12DescriptorHeapPtr pHeap, uint32_t& usedHeapEntries, DXGI_FORMAT format);
    AccelerationStructureBuffers createBottomLevelAS(ID3D12ResourcePtr vb);
    AccelerationStructureBuffers createTopLevelAS(ID3D12ResourcePtr bottom_level_as, uint64_t& tlas_size);
    void addResourceBarrier(ID3D12ResourcePtr resource, D3D12_RESOURCE_STATES state_before, D3D12_RESOURCE_STATES state_after);
    uint64_t submitCommandList();


private:
    GfxContext();
    ~GfxContext();

    ID3D12Device5Ptr m_device;
    ID3D12CommandAllocatorPtr m_cmd_allocator;
    ID3D12DescriptorHeapPtr m_desc_heap;
    ID3D12GraphicsCommandList4Ptr m_cmd_list;
    ID3D12CommandQueuePtr m_cmd_queue;
    ID3D12FencePtr m_fence;
    HANDLE m_fence_event;
    uint64_t m_fence_value = 0;

    AccelerationStructureBuffers m_as_buffers;
    void *m_render_target_unity = nullptr;
    TextureData m_render_target;
    D3D12_CPU_DESCRIPTOR_HANDLE m_rtv;

    ID3D12StateObjectPtr m_pipeline_state;
    ID3D12RootSignaturePtr m_empty_rootsig;
    ID3D12ResourcePtr m_shader_table;
    uint32_t m_shader_table_entry_size = 0;

    ID3D12DescriptorHeapPtr mpSrvUavHeap;
    static const uint32_t kSrvUavHeapSize = 2;
};

const std::string& GetErrorLog();
void SetErrorLog(const char *format, ...);

} // namespace rths
