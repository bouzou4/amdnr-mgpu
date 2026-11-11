// Copyright (c) 2026 the lmxxf_nr plugin authors. MIT licence, see ../LICENSE.
// Validation runs ([Plugin.lmxxf_nr] Validation): the D3D12 debug layer, GPU-based validation and DRED on every
// device of the process, every message in the log with its id and the object names.
#pragma once
#include <d3d12.h>

namespace Validation
{
// 0 off, 1 debug layer + DRED, 2 + GPU-based validation. Process wide: before any device is created.
void Enable(int level);
int Level();
// Once per device: its messages go to the log as "d3d12 <label> #<id> <severity>: <text>"
void Attach(ID3D12Device* device, const char* label);
// Once per removed device: the reason, DRED's page fault and the lists that did not finish. True when removed.
bool CheckRemoved(ID3D12Device* device, const char* label);

template <class T> void Name(T* object, const wchar_t* name)
{
    if (object != nullptr)
        object->SetName(name);
}
template <class T> void Name(const T& object, const wchar_t* name) { Name(object.Get(), name); }
} // namespace Validation
