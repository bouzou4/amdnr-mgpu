#pragma once

template <typename FeatureType> struct ContextData
{
    std::unique_ptr<FeatureType> feature;
    NVSDK_NGX_Parameter* createParams = nullptr;
    int changeBackendCounter = 0;
    // AMDNR (FeatureProvider_Dx12.cpp): a Ray Reconstruction handle keeps its own backend on Reset.
    bool rayReconstruction = false;
};
